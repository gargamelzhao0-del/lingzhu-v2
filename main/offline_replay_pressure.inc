/* Only wait between FILE reads or complete WebSocket frames, never with a
 * partially written frame or a microphone/archive ownership lock held. */
static esp_err_t replay_wait_capture_headroom(nuwax_ws_client_t *ws, uint32_t epoch,
                                             const atomic_bool *cancel, uint64_t *wait_us)
{
    int64_t started = 0;
    int64_t next_report = 0;
    uint32_t threshold = 60U;
    bool writer_pressure = false;
    esp_err_t err = ESP_OK;
    for (;;) {
#if CONFIG_NUWAX_SD_SESSION_ENABLE
        if (!atomic_load(&s_card_online)) { err = ESP_ERR_NOT_FINISHED; break; }
#endif
#if CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE
        if (atomic_load(&s_formal_replay_cancel_requested)) { err = ESP_ERR_NOT_FINISHED; break; }
#endif
        if ((cancel && atomic_load_explicit(cancel, memory_order_acquire)) ||
            nuwax_offline_audio_storage_busy()) { err = ESP_ERR_NOT_FINISHED; break; }
        if (!nuwax_ws_is_connected(ws) || nuwax_ws_connection_epoch(ws) != epoch) {
            err = ESP_ERR_INVALID_STATE; break;
        }
        const uint32_t queued = replay_capture_queue_depth();
        /* TX uses validated RAM and retains the original high-water guard.
         * The reader has separate, earlier 8/2 SD admission below. */
        if (queued >= QUEUE_DEPTH / 2U) writer_pressure = true;
        if (queued <= QUEUE_DEPTH / 4U) writer_pressure = false;
        const bool wake_pressure = nuwax_wake_word_capture_backlog_ms() >= threshold;
        if (!writer_pressure && !wake_pressure) break;
        const int64_t now = esp_timer_get_time();
        if (!started) { started = now; next_report = now + 5000000LL; }
        if (now >= next_report) {
            ESP_LOGI(TAG, "offline replay paused for capture backlog_ms=%lu writer_queue=%lu wait_ms=%llu; connection retained",
                     (unsigned long)nuwax_wake_word_capture_backlog_ms(),
                     (unsigned long)queued,
                     (unsigned long long)((now - started) / 1000U));
            next_report = now + 5000000LL;
        }
        if (wake_pressure) threshold = 31U; /* Resume at <= 30 ms after entering at >= 60 ms. */
        /* Backpressure is not a failed transfer. Keep FILE/CRC/ACK slots and
         * the healthy connection alive until capture catches up. Every tick
         * remains cancellable; RX/keepalive and the watchdog can still run.
         * A one-second abort here previously caused reconnect/re-read loops. */
        if (writer_pressure) vTaskDelay(1U);
        else nuwax_wake_word_wait_capture_headroom(portTICK_PERIOD_MS);
    }
    if (started && wait_us) *wait_us += (uint64_t)(esp_timer_get_time() - started);
    return err;
}

/* Same lock as writer, acquired before the read DMA allocation. It is held
 * until validation/copy completes and the DMA loan is freed. No network I/O
 * owns it: writer/read bounce buffers cannot exhaust internal RAM together. */
typedef struct {
    uint64_t total_us;
    uint64_t writer_us, queue_us, wake_us, mutex_us;
    uint32_t signals, timeouts, mutex_waits, retries;
} replay_storage_stats_t;

static esp_err_t replay_read_storage_status(nuwax_ws_client_t *ws, uint32_t epoch,
                                            const atomic_bool *cancel)
{
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    if (!atomic_load(&s_card_online)) return ESP_ERR_NOT_FINISHED;
#endif
#if CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE
    if (atomic_load_explicit(&s_formal_replay_cancel_requested, memory_order_acquire))
        return ESP_ERR_NOT_FINISHED;
#endif
    if ((cancel && atomic_load_explicit(cancel, memory_order_acquire)) ||
        nuwax_offline_audio_storage_busy()) return ESP_ERR_NOT_FINISHED;
    return nuwax_ws_is_connected(ws) && nuwax_ws_connection_epoch(ws) == epoch
        ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t replay_read_storage_enter_profiled(nuwax_ws_client_t *ws, uint32_t epoch,
    const atomic_bool *cancel, uint64_t *wait_us, replay_storage_stats_t *stats)
{
    int64_t started = 0;
    bool pressure = false;
    uint32_t wake_threshold = 60U;
    esp_err_t err = ESP_OK;
    for (;;) {
#ifdef ESP_PLATFORM
        /* Clear BEFORE arming and checking predicates. A release before arm
         * is seen by the checks; a release after them leaves a retained bit. */
        if (s.events) xEventGroupClearBits(s.events, REPLAY_STORAGE_PROGRESS_BIT);
        atomic_store_explicit(&s_storage_wait_armed, true, memory_order_release);
#endif
        err = replay_read_storage_status(ws, epoch, cancel);
        if (err != ESP_OK) break;
        const uint32_t queued = replay_capture_queue_depth();
        if (queued >= 8U) pressure = true;
        if (queued <= 2U) pressure = false;
        const bool wake_pressure = nuwax_wake_word_capture_backlog_ms() >= wake_threshold;
        if (wake_pressure) wake_threshold = 31U;
        if (!pressure && !wake_pressure) {
#ifdef ESP_PLATFORM
            if (!atomic_load_explicit(&s.writer_busy, memory_order_acquire) &&
                xSemaphoreTake(s.lock, 0U) == pdTRUE) {
                if (!atomic_load_explicit(&s.writer_busy, memory_order_acquire) &&
                    replay_capture_queue_depth() < 8U) break;
                xSemaphoreGive(s.lock);
                if (replay_capture_queue_depth() >= 8U) pressure = true;
            }
#else
            break;
#endif
        }
        if (!started) started = esp_timer_get_time();
        if (stats) stats->retries++;
        const int64_t before = esp_timer_get_time();
#ifdef ESP_PLATFORM
        if (pressure || atomic_load_explicit(&s.writer_busy, memory_order_acquire)) {
            /* Only the serialized reader waits on this otherwise unused bit.
             * Timeout retains bounded cancellation/epoch observation. */
            const EventBits_t bits = s.events ? xEventGroupWaitBits(s.events,
                REPLAY_STORAGE_PROGRESS_BIT, pdTRUE, pdFALSE, 1U) : 0U;
            if (!s.events) vTaskDelay(1U);
            if (stats) {
                if (bits & REPLAY_STORAGE_PROGRESS_BIT) stats->signals++;
                else stats->timeouts++;
                if (pressure) stats->queue_us += (uint64_t)(esp_timer_get_time() - before);
                else stats->writer_us += (uint64_t)(esp_timer_get_time() - before);
            }
        } else if (wake_pressure) {
            nuwax_wake_word_wait_capture_headroom(portTICK_PERIOD_MS);
            if (stats) stats->wake_us += (uint64_t)(esp_timer_get_time() - before);
        } else {
            /* Register as a mutex waiter, so any owner (not only the writer)
             * can wake us on release. Never sleep holding a DMA loan. */
            const bool acquired = xSemaphoreTake(s.lock, 1U) == pdTRUE;
            if (stats) {
                stats->mutex_waits++;
                stats->mutex_us += (uint64_t)(esp_timer_get_time() - before);
            }
            if (acquired) {
                err = replay_read_storage_status(ws, epoch, cancel);
                if (err == ESP_OK && !atomic_load_explicit(&s.writer_busy, memory_order_acquire) &&
                    replay_capture_queue_depth() < 8U &&
                    nuwax_wake_word_capture_backlog_ms() < wake_threshold) break;
                xSemaphoreGive(s.lock);
                if (err != ESP_OK) break;
            }
        }
#else
        vTaskDelay(1U);
        if (stats) {
            if (pressure) stats->queue_us += (uint64_t)(esp_timer_get_time() - before);
            else stats->wake_us += (uint64_t)(esp_timer_get_time() - before);
        }
#endif
    }
#ifdef ESP_PLATFORM
    atomic_store_explicit(&s_storage_wait_armed, false, memory_order_release);
#endif
    if (started) {
        const uint64_t elapsed = (uint64_t)(esp_timer_get_time() - started);
        if (wait_us) *wait_us += elapsed;
        if (stats) stats->total_us += elapsed;
    }
    return err;
}

static esp_err_t replay_read_storage_enter(nuwax_ws_client_t *ws, uint32_t epoch,
    const atomic_bool *cancel, uint64_t *wait_us)
{
    return replay_read_storage_enter_profiled(ws, epoch, cancel, wait_us, NULL);
}

static void replay_read_storage_leave(void)
{
#ifdef ESP_PLATFORM
    xSemaphoreGive(s.lock);
#endif
}
