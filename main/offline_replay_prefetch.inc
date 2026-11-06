/* One FILE owner and two slots. Release/acquire publishes each validated read
 * prefix. The sender releases a slot after synchronous TX consumption; the
 * sealed SD file backs any unacknowledged streaming window. Serial/probe
 * windows retain their slot through ACK retries.
 * The owner joins the suspended worker before closing FILE or freeing memory. */
/* Volatile validation checkpoints belong to the serialized replay owner.
 * Only media generation is shared with recovery; the reader never touches
 * this cache. It is published after join; network retries may reuse it.
 * Media recovery, identity changes and explicit test-worker restart expire it. */
#define REPLAY_CHECKPOINT_COUNT 8U
static atomic_uint s_replay_checkpoint_generation;
static void replay_checkpoint_invalidate(void)
{
    atomic_fetch_add_explicit(&s_replay_checkpoint_generation, 1U, memory_order_relaxed);
}
typedef struct {
    uint32_t generation, total, next;
    char segment[48];
    archive_header_t header;
    archive_footer_t footer;
    replay_stream_t states[REPLAY_CHECKPOINT_COUNT];
} replay_checkpoint_t;
static replay_checkpoint_t s_replay_checkpoint;
#define REPLAY_CHECKPOINT_RECENT_FILES 3U
#ifdef ESP_PLATFORM
static replay_checkpoint_t *s_replay_checkpoint_recent;
#else
static replay_checkpoint_t s_replay_checkpoint_recent_storage[REPLAY_CHECKPOINT_RECENT_FILES];
static replay_checkpoint_t *s_replay_checkpoint_recent = s_replay_checkpoint_recent_storage;
#endif
static unsigned s_replay_checkpoint_recent_next;

static bool replay_checkpoint_matches(const replay_checkpoint_t *c, uint32_t generation,
    const char *segment, uint32_t total, const archive_header_t *header, const archive_footer_t *footer)
{
    return c->generation == generation && c->total == total && !strcmp(c->segment, segment) &&
        !memcmp(&c->header, header, sizeof(*header)) && !memcmp(&c->footer, footer, sizeof(*footer));
}

/* Serialized owner after reader join. Optional PSRAM only: failure preserves
 * the hot file and falls back to revalidation when another file resumes. */
static void replay_checkpoint_store(const replay_checkpoint_t *c)
{
#ifdef ESP_PLATFORM
    if (!s_replay_checkpoint_recent)
        s_replay_checkpoint_recent = heap_caps_calloc(REPLAY_CHECKPOINT_RECENT_FILES,
            sizeof(*s_replay_checkpoint_recent), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    if (s_replay_checkpoint_recent && s_replay_checkpoint.segment[0] &&
        !replay_checkpoint_matches(&s_replay_checkpoint, c->generation, c->segment,
                                   c->total, &c->header, &c->footer)) {
        const unsigned slot = s_replay_checkpoint_recent_next++ % REPLAY_CHECKPOINT_RECENT_FILES;
        s_replay_checkpoint_recent[slot] = s_replay_checkpoint;
    }
    s_replay_checkpoint = *c;
}

static void replay_checkpoint_prepare(replay_checkpoint_t *c, const char *segment,
    uint32_t total, const archive_header_t *header, const archive_footer_t *footer)
{
    const uint32_t generation = atomic_load_explicit(&s_replay_checkpoint_generation, memory_order_relaxed);
    if (replay_checkpoint_matches(&s_replay_checkpoint, generation, segment, total, header, footer)) {
        *c = s_replay_checkpoint;
        return;
    }
    for (unsigned i = 0U; s_replay_checkpoint_recent && i < REPLAY_CHECKPOINT_RECENT_FILES; ++i) {
        if (replay_checkpoint_matches(&s_replay_checkpoint_recent[i], generation, segment, total, header, footer)) {
            *c = s_replay_checkpoint_recent[i];
            return;
        }
    }
    memset(c, 0, sizeof(*c));
    c->generation = generation; c->total = total; c->header = *header; c->footer = *footer;
    snprintf(c->segment, sizeof(c->segment), "%s", segment);
}

typedef struct {
    uint8_t *data;
    uint32_t position;
    size_t count;
    atomic_bool ready;
    atomic_uint published;
} replay_read_slot_t;

typedef struct {
    FILE *file;
    nuwax_ws_client_t *ws;
    uint32_t epoch, total, resume;
    size_t capacity;
    archive_header_t header;
    archive_footer_t footer;
    replay_read_slot_t slots[2];
    replay_stream_t validator;
    replay_checkpoint_t checkpoint;
    atomic_bool cancel;
    const atomic_bool *owner_cancel;
    atomic_int error;
    uint64_t read_us, validate_us, pressure_us;
    replay_storage_stats_t storage_stats;
    uint32_t read_wait_credits;
    replay_read_stats_t read_stats;
    StaticSemaphore_t data_signal_storage, space_signal_storage;
    SemaphoreHandle_t data_signal, space_signal;
    TaskHandle_t task;
    bool lane_probe;
    atomic_bool probe_read_done;
    uint64_t probe_read_wall_us;
} replay_prefetch_t;

/* Atomics remain the source of truth. A binary signal arriving between the
 * predicate check and wait stays pending; repeated publications coalesce.
 * Timeout bounds cancellation/epoch observation when progress stops. */
static bool replay_prefetch_wait_signal(SemaphoreHandle_t signal)
{
    if (xSemaphoreTake(signal, 0U) == pdTRUE) return false;
    const int64_t started = esp_timer_get_time();
    (void)xSemaphoreTake(signal, pdMS_TO_TICKS(20U));
    return offline_replay_wait_relaxes((uint64_t)(esp_timer_get_time() - started),
                                      portTICK_PERIOD_MS * 1000U);
}

static esp_err_t replay_prefetch_check(replay_prefetch_t *p)
{
    if (atomic_load_explicit(&p->cancel, memory_order_acquire) ||
        (p->owner_cancel && atomic_load_explicit(p->owner_cancel, memory_order_acquire)) ||
        nuwax_offline_audio_storage_busy()) return ESP_ERR_NOT_FINISHED;
    if (!nuwax_ws_is_connected(p->ws) ||
        nuwax_ws_connection_epoch(p->ws) != p->epoch) return ESP_ERR_INVALID_STATE;
    return ESP_OK;
}

#if CONFIG_NUWAX_SD_READ_UNLOCK_VALIDATE
static esp_err_t replay_prefetch_reenter_storage(void *context)
{
    replay_prefetch_t *p = context;
    return replay_read_storage_enter_profiled(p->ws, p->epoch, &p->cancel,
        &p->pressure_us, &p->storage_stats);
}
#endif

static void replay_prefetch_task(void *arg)
{
    replay_prefetch_t *p = arg;
#if CONFIG_NUWAX_SD_READ_UNLOCK_VALIDATE
    p->read_stats.storage_context = p;
    p->read_stats.enter_storage = replay_prefetch_reenter_storage;
    p->read_stats.leave_storage = replay_read_storage_leave;
#endif
    esp_err_t err = ESP_OK;
    uint32_t position = p->validator.position;
    unsigned slot = 0U;
    offline_replay_budget_t budget = {0};
    replay_read_prepare(&p->read_stats);
    const int64_t probe_started = esp_timer_get_time();
    while (position < p->total && (err = replay_prefetch_check(p)) == ESP_OK) {
        replay_read_slot_t *b = &p->slots[slot];
        while (atomic_load_explicit(&b->ready, memory_order_acquire)) {
            if ((err = replay_prefetch_check(p)) != ESP_OK) break;
            if (replay_prefetch_wait_signal(p->space_signal)) offline_replay_budget_relax(&budget);
        }
        if (err != ESP_OK) break;
        size_t count = p->total - position;
        if (count > p->capacity) count = p->capacity;
        if (position < p->resume && count > p->resume - position) count = p->resume - position;
        const bool upload = position >= p->resume;
        if (upload) {
            b->position = position;
            b->count = count;
            atomic_store_explicit(&b->published, 0U, memory_order_relaxed);
        }
        for (size_t at = 0U; at < count;) {
            if ((err = replay_prefetch_check(p)) != ESP_OK) break;
            size_t n = replay_read_slice_size(position + (uint32_t)at, count - at);
            const uint64_t pressure_before = p->pressure_us;
            err = replay_read_storage_enter_profiled(p->ws, p->epoch, &p->cancel,
                &p->pressure_us, &p->storage_stats);
            if (offline_replay_wait_relaxes(p->pressure_us - pressure_before,
                    portTICK_PERIOD_MS * 1000U)) offline_replay_budget_relax(&budget);
            if (err != ESP_OK) break;
            uint64_t work_us = 0U;
            const uint64_t read_before = p->read_us;
            err = replay_read_validate(p->file, b->data + at, &n, &p->validator,
                p->total, &p->header, &p->footer, &p->read_us, &p->validate_us,
                &work_us, &p->read_stats);
            if (!p->read_stats.storage_released) replay_read_storage_leave();
            if (err != ESP_OK) break;
            /* A slow SD read already blocked this worker. Credit that wait,
             * then charge the just-finished copy/CRC work and bytes normally.
             * Fast reads never reset the fairness budget. */
            if (offline_replay_wait_relaxes(p->read_us - read_before,
                                           portTICK_PERIOD_MS * 1000U)) {
                offline_replay_budget_relax(&budget);
                p->read_wait_credits++;
            }
            at += n;
            if (upload) {
                atomic_store_explicit(&b->published, (unsigned)at, memory_order_release);
                /* Publish metadata once. Never re-arm ready after the last
                 * prefix: the sender may already have ACKed and released it. */
                if (at == n) atomic_store_explicit(&b->ready, true, memory_order_release);
                xSemaphoreGive(p->data_signal);
            }
            if (offline_replay_budget_due(&budget, n, work_us)) {
                vTaskDelay(1U);
            }
        }
        if (err != ESP_OK) break;
        p->checkpoint.states[p->checkpoint.next++ % REPLAY_CHECKPOINT_COUNT] = p->validator;
        if (p->lane_probe && position == p->resume) {
            p->probe_read_wall_us = esp_timer_get_time() - probe_started;
            atomic_store_explicit(&p->probe_read_done, true, memory_order_release);
            xSemaphoreGive(p->data_signal);
            /* Do not read the next window during the TX-only measurement. */
            while (atomic_load_explicit(&b->ready, memory_order_acquire)) {
                if ((err = replay_prefetch_check(p)) != ESP_OK) break;
                if (replay_prefetch_wait_signal(p->space_signal)) offline_replay_budget_relax(&budget);
            }
            if (err != ESP_OK) break;
        }
        if (position >= p->resume) {
            slot ^= 1U;
        }
        position += count;
    }
    replay_read_release(&p->read_stats);
    ESP_LOGI(TAG, "offline replay capture pressure reader_wait_ms=%llu",
             (unsigned long long)(p->pressure_us / 1000U));
    ESP_LOGI(TAG, "offline replay reader work yields=%lu direct_frames=%lu copied_bytes=%lu read_wait_credits=%lu idle_credits=%lu ui_credits=%lu ui_handoffs=%lu",
             (unsigned long)budget.yields, (unsigned long)p->validator.direct_frames,
             (unsigned long)p->validator.copied_bytes, (unsigned long)p->read_wait_credits,
             (unsigned long)budget.idle_credits, (unsigned long)budget.ui_credits,
             0UL);
    ESP_LOGI(TAG, "offline replay read path=descriptor validator=header_stream state_bytes=%u internal_bytes=%lu fallback_bytes=%lu copy_ms=%llu allocation_ms=%llu",
        (unsigned)sizeof(replay_stream_t),
        (unsigned long)p->read_stats.internal_bytes, (unsigned long)p->read_stats.fallback_bytes,
        (unsigned long long)(p->read_stats.copy_us / 1000U), (unsigned long long)(p->read_stats.allocation_us / 1000U));
    atomic_store_explicit(&p->error, err, memory_order_release);
    replay_read_log_fallback(&p->read_stats);
    xSemaphoreGive(p->data_signal);
    /* Never self-free: the owner waits for this state, then deletes with the
     * matching WithCaps API. No deferred idle-task stack reclamation per file. */
    vTaskSuspend(NULL);
}

static esp_err_t replay_prefetch_wait_bytes(replay_prefetch_t *p,
    replay_read_slot_t *slot, size_t required, uint64_t *wait_us, bool *blocked)
{
    if (required > slot->count) return ESP_ERR_INVALID_SIZE;
    int64_t started = 0;
    esp_err_t err;
    for (;;) {
        err = replay_prefetch_check(p);
        if (err == ESP_OK) err = atomic_load_explicit(&p->error, memory_order_acquire);
        if (err != ESP_OK || atomic_load_explicit(&slot->published, memory_order_acquire) >= required) break;
        if (!started) started = esp_timer_get_time();
        if (replay_prefetch_wait_signal(p->data_signal)) *blocked = true;
    }
    if (started) *wait_us += (uint64_t)(esp_timer_get_time() - started);
    return err;
}

static esp_err_t replay_prefetch_wait_probe(replay_prefetch_t *p)
{
    while (!atomic_load_explicit(&p->probe_read_done, memory_order_acquire)) {
        esp_err_t err = replay_prefetch_check(p);
        if (err == ESP_OK) err = atomic_load_explicit(&p->error, memory_order_acquire);
        if (err != ESP_OK) return err;
        replay_prefetch_wait_signal(p->data_signal);
    }
    return ESP_OK;
}

static replay_prefetch_t *replay_prefetch_start_context(FILE *file, nuwax_ws_client_t *ws,
    uint32_t epoch, uint32_t total, uint32_t resume, size_t capacity, uint8_t *buffer,
    const archive_header_t *header, const archive_footer_t *footer, bool lane_probe,
    const char *segment, const atomic_bool *owner_cancel)
{
    replay_prefetch_t *p = heap_caps_malloc(sizeof(*p), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) return NULL;
    memset(p, 0, sizeof(*p));
    p->file = file; p->ws = ws; p->epoch = epoch; p->total = total;
    p->owner_cancel = owner_cancel; /* Published before the reader can run. */
    p->resume = resume; p->capacity = capacity; p->header = *header; p->footer = *footer;
    p->lane_probe = lane_probe;
    replay_checkpoint_prepare(&p->checkpoint, segment, total, header, footer);
    for (unsigned i = 0U; i < REPLAY_CHECKPOINT_COUNT; i++) {
        const replay_stream_t *v = &p->checkpoint.states[i];
        if (v->position <= resume && v->position > p->validator.position) p->validator = *v;
    }
    if (p->validator.position &&
        lseek(fileno(file), p->validator.position, SEEK_SET) != (off_t)p->validator.position) {
        /* No worker owns FILE yet. Rewind before the serial fallback. */
        replay_checkpoint_invalidate();
        (void)replay_read_rewind(file);
        heap_caps_free(p);
        return NULL;
    }
    ESP_LOGI(TAG, "offline replay checkpoint resume=%lu validated_prefix=%lu reread_prefix=%lu",
        (unsigned long)resume, (unsigned long)p->validator.position,
        (unsigned long)(resume - p->validator.position));
    atomic_init(&p->probe_read_done, false);
    p->slots[0].data = buffer; p->slots[1].data = buffer + capacity;
    atomic_init(&p->slots[0].ready, false); atomic_init(&p->slots[1].ready, false);
    atomic_init(&p->slots[0].published, 0U); atomic_init(&p->slots[1].published, 0U);
    atomic_init(&p->cancel, false); atomic_init(&p->error, ESP_OK);
    p->data_signal = xSemaphoreCreateBinaryStatic(&p->data_signal_storage);
    p->space_signal = xSemaphoreCreateBinaryStatic(&p->space_signal_storage);
    if (xTaskCreatePinnedToCoreWithCaps(replay_prefetch_task, "sd_prefetch", 8192U, p, OFFLINE_REPLAY_READER_PRIORITY,
                            &p->task, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        vSemaphoreDelete(p->data_signal);
        vSemaphoreDelete(p->space_signal);
        (void)replay_read_rewind(file);
        heap_caps_free(p);
        return NULL;
    }
    return p;
}

static replay_prefetch_t *replay_prefetch_start(FILE *file, nuwax_ws_client_t *ws,
    uint32_t epoch, uint32_t total, uint32_t resume, size_t capacity, uint8_t *buffer,
    const archive_header_t *header, const archive_footer_t *footer, bool lane_probe,
    const char *segment)
{
    return replay_prefetch_start_context(file, ws, epoch, total, resume, capacity,
        buffer, header, footer, lane_probe, segment, NULL);
}

static esp_err_t replay_prefetch_join_profiled(replay_prefetch_t *p, bool cancel,
    uint64_t *read_us, uint64_t *validate_us, replay_storage_stats_t *storage_stats)
{
    if (cancel) {
        atomic_store_explicit(&p->cancel, true, memory_order_release);
        xSemaphoreGive(p->space_signal);
    }
    /* SD commands have driver timeouts. Never force-delete a worker inside
     * FatFS: that could strand a filesystem mutex or access freed buffers. */
    while (eTaskGetState(p->task) != eSuspended) vTaskDelay(1U);
    vTaskDeleteWithCaps(p->task);
    const esp_err_t err = atomic_load_explicit(&p->error, memory_order_acquire);
    /* Ordinary mode preemption changes no sealed bytes. Media generation
     * still fences recovery/remount, and CRC/I/O failures discard validation. */
    if ((err == ESP_OK || err == ESP_ERR_INVALID_STATE ||
         err == ESP_ERR_NOT_FINISHED) &&
        p->checkpoint.generation == atomic_load_explicit(&s_replay_checkpoint_generation, memory_order_relaxed)) {
        replay_checkpoint_store(&p->checkpoint);
    } else {
        replay_checkpoint_invalidate();
    }
    *read_us = p->read_us; *validate_us = p->validate_us;
    if (storage_stats) *storage_stats = p->storage_stats;
    vSemaphoreDelete(p->data_signal);
    vSemaphoreDelete(p->space_signal);
    heap_caps_free(p);
    return err;
}

static esp_err_t replay_prefetch_join(replay_prefetch_t *p, bool cancel,
    uint64_t *read_us, uint64_t *validate_us)
{
    return replay_prefetch_join_profiled(p, cancel, read_us, validate_us, NULL);
}
