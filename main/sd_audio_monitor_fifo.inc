/* Included after the catalog implementation. All state belongs to the storage
 * lock; no network, retained DIR/FILE handle, or clock-based scene ordering. */
typedef struct {
    uint8_t owner[32];
    char head[48], after[48];
    bool owner_valid, scanning, scan_done, have_current, have_best, waiting, scan_dirty;
    uint32_t generation, index, best_order;
    unsigned best_kind;
    sd_audio_session_t current, best;
    struct { bool active, proven; uint32_t next; sd_audio_session_t scene; } end;
} monitor_fifo_state_t;
static monitor_fifo_state_t monitor_fifo;
/* The second lane lives in PSRAM; don't spend another ~1 KiB of scarce
 * internal DMA headroom merely to remember meeting ordering. */
#ifdef ESP_PLATFORM
static monitor_fifo_state_t *meeting_fifo;
static monitor_fifo_state_t *meeting_fifo_get(void)
{
    if (!meeting_fifo) {
        meeting_fifo = heap_caps_malloc(sizeof(*meeting_fifo), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (meeting_fifo) memset(meeting_fifo, 0, sizeof(*meeting_fifo));
    }
    return meeting_fifo;
}
#else
static monitor_fifo_state_t meeting_fifo_storage;
static monitor_fifo_state_t *meeting_fifo = &meeting_fifo_storage;
static monitor_fifo_state_t *meeting_fifo_get(void) { return meeting_fifo; }
#endif
static void fifo_reset(monitor_fifo_state_t *fifo) { memset(fifo, 0, sizeof(*fifo)); }
void sd_monitor_fifo_reset(void)
{
    fifo_reset(&monitor_fifo);
    if (meeting_fifo) fifo_reset(meeting_fifo);
}
static void monitor_fifo_metadata_changed(const char *id)
{
    if (!id) return;
    if (monitor_fifo.end.active && !strcmp(id, monitor_fifo.end.scene.session_id))
        monitor_fifo.end.active = monitor_fifo.end.proven = false;
    if (meeting_fifo && meeting_fifo->end.active && !strcmp(id, meeting_fifo->end.scene.session_id))
        meeting_fifo->end.active = meeting_fifo->end.proven = false;
}

static bool monitor_archive_order(const sd_audio_session_t *scene,
    const sd_audio_segment_t *part, uint32_t *order)
{
    /* The path suffix is the physical global allocation number, while
     * part->index is the independent, per-scene wire index. */
    return retention_path(part->path, scene->session_id, order);
}

static void monitor_scan_reset(monitor_fifo_state_t *fifo)
{
    fifo->scanning = true; fifo->scan_done = false;
    fifo->have_current = fifo->have_best = fifo->waiting = fifo->scan_dirty = false;
    fifo->after[0] = 0;
    fifo->generation = replay_scan_generation;
}

/* One directory walk per slice, no media handle crosses lock release. */
static esp_err_t monitor_scan_session(monitor_fifo_state_t *fifo, unsigned mode, unsigned *budget, bool *exhausted)
{
    *exhausted = false;
    DIR *dir = opendir(SD_AUDIO_CATALOG_ROOT);
    if (!dir) return ESP_FAIL;
    bool reached = !fifo->after[0];
    esp_err_t result = ESP_OK;
    struct dirent *entry;
    for (;;) {
        errno = 0;
        entry = readdir(dir);
        if (!entry) { if (errno) result = ESP_FAIL; break; }
        const size_t n = strlen(entry->d_name);
        if (n <= 8U || n - 8U >= 48U || strcmp(entry->d_name + n - 8U, ".session")) continue;
        char id[48] = {0}; memcpy(id, entry->d_name, n - 8U);
        if (!reached) { if (!strcmp(id, fifo->after)) reached = true; continue; }
        if (done_cache_skip(id)) continue;
        if (!*budget) { result = ESP_ERR_NOT_FINISHED; break; }
        --*budget;
        result = load_replay_session(id, &fifo->current);
        if (result == ESP_ERR_NOT_FOUND) { result = ESP_OK; continue; }
        if (result != ESP_OK) break;
        /* A transient load failure must retry this entry, not advance past it. */
        memcpy(fifo->after, id, sizeof(id));
        const sd_audio_session_t *s = &fifo->current;
        if (s->done || s->mode != mode || memcmp(s->owner, fifo->owner, 32U)) continue;
        fifo->index = s->first_pending;
        fifo->scan_dirty = false;
        fifo->have_current = true;
        break;
    }
    if (!entry && result == ESP_OK && reached && !fifo->have_current) *exhausted = true;
    if (!reached && result == ESP_OK) {
        /* A quarantined/removed cursor is not EOF. Restart once rather than
         * searching forever for a name that no longer exists. */
        monitor_scan_reset(fifo);
        result = ESP_ERR_NOT_FINISHED;
    }
    if (closedir(dir) != 0 && result == ESP_OK) result = ESP_FAIL;
    return result;
}

static esp_err_t monitor_consider(monitor_fifo_state_t *fifo, unsigned kind, uint32_t order)
{
    if (kind == 2U && fifo->have_best && fifo->best_kind == 2U &&
        order == fifo->best_order &&
        strcmp(fifo->current.session_id, fifo->best.session_id))
        return ESP_ERR_INVALID_STATE; /* Imported/ambiguous order is not chronology. */
    const bool earlier = !fifo->have_best || kind < fifo->best_kind ||
        (kind == fifo->best_kind && (order < fifo->best_order ||
         (order == fifo->best_order &&
          strcmp(fifo->current.session_id, fifo->best.session_id) < 0)));
    if (earlier) {
        fifo->best = fifo->current;
        fifo->best_kind = kind; fifo->best_order = order;
        fifo->have_best = true;
    }
    fifo->have_current = false;
    return ESP_OK;
}

static esp_err_t monitor_flush_scan_progress(monitor_fifo_state_t *fifo)
{
    if (!fifo->scan_dirty) return ESP_OK;
    const esp_err_t err = sd_audio_catalog_save(&fifo->current);
    if (err == ESP_OK) {
        fifo->scan_dirty = false;
        /* This serialized save is our own change, not a new scene/writer. */
        fifo->generation = replay_scan_generation;
    }
    return err;
}

static esp_err_t monitor_find_head(monitor_fifo_state_t *fifo, unsigned mode, unsigned *budget)
{
    if (fifo->generation != replay_scan_generation || !fifo->scanning)
        monitor_scan_reset(fifo);
    while (*budget) {
        if (!fifo->have_current) {
            bool exhausted = false;
            esp_err_t err = monitor_scan_session(fifo, mode, budget, &exhausted);
            if (err != ESP_OK) return err;
            if (fifo->generation != replay_scan_generation) {
                monitor_scan_reset(fifo); return ESP_ERR_NOT_FINISHED;
            }
            if (exhausted) {
                fifo->scan_done = true;
                if (fifo->have_best)
                    memcpy(fifo->head, fifo->best.session_id, sizeof(fifo->head));
                return ESP_OK;
            }
        }
        sd_audio_session_t *s = &fifo->current;
        if (!s->segment_count && !s->ended) {
            fifo->waiting = true; fifo->have_current = false; continue;
        }
        while (*budget && fifo->index < s->segment_count) {
            sd_audio_segment_t part;
            --*budget;
            const esp_err_t err = catalog_read_reconciled(s->session_id, fifo->index, &part);
            if (err == ESP_FAIL) return err;
            if (err != ESP_OK) {
                const esp_err_t saved = monitor_flush_scan_progress(fifo);
                if (saved != ESP_OK) return saved;
                return err; /* Unknown index order cannot authorize overtaking. */
            }
            if (part.acknowledged || part.missing) {
                esp_err_t cleaned = ESP_OK;
                if (part.missing) s->complete = 0U;
                if (part.missing == SD_AUDIO_SEGMENT_RETIRING)
                    cleaned = finish_retirement(s->session_id, &part, NULL);
                else if (part.acknowledged) cleaned = remove_acknowledged(s->session_id, &part);
                if (cleaned != ESP_OK) return cleaned;
                s->first_pending = ++fifo->index;
                fifo->scan_dirty = true;
                continue;
            }
            uint32_t order;
            if (!monitor_archive_order(s, &part, &order)) return ESP_ERR_INVALID_STATE;
            const esp_err_t saved = monitor_flush_scan_progress(fifo);
            if (saved != ESP_OK) return saved;
            const esp_err_t considered = monitor_consider(fifo, 2U, order);
            if (considered != ESP_OK) return considered;
            break;
        }
        const esp_err_t saved = monitor_flush_scan_progress(fifo);
        if (saved != ESP_OK) return saved;
        if (fifo->have_current && fifo->index == s->segment_count) {
            if (s->ended) {
                const esp_err_t considered = monitor_consider(fifo, 0U, 0U);
                if (considered != ESP_OK) return considered; /* Outstanding scene-end first. */
            }
            else { fifo->waiting = true; fifo->have_current = false; }
        }
    }
    return ESP_ERR_NOT_FINISHED;
}

/* A power loss can leave an ended scene's durable reservation without any
 * audio ever having been created. Only proven absence may release that hole;
 * present .part/.ready/quarantined sources keep their original order. */
static esp_err_t fifo_empty_reservation(sd_audio_session_t *scene, const sd_audio_segment_t *part)
{
    if (!scene->ended || (part->sealed & 1U)) return ESP_OK;
    const size_t length = strlen(part->path);
    if (length < 6U || strcmp(part->path + length - 6U, ".ready")) return ESP_ERR_INVALID_STATE;
    char path[160]; struct stat st;
    for (unsigned kind = 0U; kind < 4U; ++kind) {
        strcpy(path, part->path);
        if (kind & 1U) strcpy(path + length - 6U, ".part");
        if (kind >= 2U) strcat(path, ".bad");
        if (stat(path, &st) == 0) return ESP_OK;
        if (errno != ENOENT) return ESP_FAIL;
    }
    /* A retained sidecar can prove an earlier cloud ACK even without audio.
     * Restore that proof before considering a genuinely empty reservation. */
    snprintf(path, sizeof(path), "%s.meta", part->path);
    bool changed = false;
    esp_err_t err = catalog_recover_backup(path, scene->session_id, part->index, &changed);
    if (err != ESP_OK) return err;
    if (!changed) err = sd_audio_catalog_mark_missing(scene->session_id, part->index);
    /* Exactly one exceptional repair per lock slice, even for a large backlog. */
    return err == ESP_OK ? ESP_ERR_NOT_FINISHED : err;
}

static esp_err_t monitor_head_next(monitor_fifo_state_t *fifo, unsigned mode, sd_audio_session_t *scene, sd_audio_segment_t *part,
    bool *found, bool *file, bool *waiting, unsigned *budget)
{
    if (!*budget) return ESP_ERR_NOT_FINISHED;
    esp_err_t err = sd_audio_catalog_load(fifo->head, scene);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_CRC ||
            err == ESP_ERR_INVALID_STATE || err == ESP_ERR_INVALID_SIZE) {
            fifo->head[0] = 0;
            monitor_scan_reset(fifo);
        }
        return err;
    }
    if (scene->mode != mode || memcmp(scene->owner, fifo->owner, 32U)) {
        fifo->head[0] = 0; monitor_scan_reset(fifo);
        return ESP_ERR_INVALID_STATE;
    }
    if (scene->done) {
        fifo->head[0] = 0; monitor_scan_reset(fifo); return ESP_ERR_NOT_FINISHED;
    }
    if (session_blocked(scene->session_id)) { *waiting = true; return ESP_OK; }
    const uint32_t before = scene->first_pending;
    while (scene->first_pending < scene->segment_count && *budget) {
        --*budget;
        const uint32_t i = scene->first_pending;
        err = catalog_read_reconciled(scene->session_id, i, part);
        if (err == ESP_FAIL) return err;
        /* Unknown metadata is not evidence that audio is absent. The
         * reconciler either restores the original tuple or defers/fails. */
        if (err != ESP_OK) return err;
        if (part->missing || part->acknowledged) {
            if (part->missing) scene->complete = 0;
            if (part->missing == SD_AUDIO_SEGMENT_RETIRING)
                err = finish_retirement(scene->session_id, part, NULL);
            else if (part->acknowledged) err = remove_acknowledged(scene->session_id, part);
            if (err != ESP_OK) return err;
            ++scene->first_pending; continue;
        }
        /* Never pass an earlier unsealed file, even in an abnormal index. */
        if (!(part->sealed & 1U)) {
            err = fifo_empty_reservation(scene, part);
            if (err != ESP_OK) return err;
            *waiting = true; break;
        }
        *found = *file = true; break;
    }
    if (before != scene->first_pending) {
        err = sd_audio_catalog_save(scene);
        if (err != ESP_OK) return err;
    }
    if (*found) {
        selected_session = *scene; selected_segment = *part; selected_valid = true;
        return ESP_OK;
    }
    if (scene->first_pending == scene->segment_count) {
        if (scene->ended) *found = true;
        else *waiting = true;
        return ESP_OK;
    }
    return *waiting ? ESP_OK : ESP_ERR_NOT_FINISHED;
}

static esp_err_t fifo_next(monitor_fifo_state_t *fifo, unsigned mode, const uint8_t owner[32], sd_audio_session_t *scene,
    sd_audio_segment_t *part, bool *found, bool *file, bool *waiting)
{
    if (!fifo || !owner || !scene || !part || !found || !file || !waiting) return ESP_ERR_INVALID_ARG;
    *found = *file = *waiting = false; selected_valid = false;
    if (!fifo->owner_valid || memcmp(owner, fifo->owner, 32U)) {
        fifo_reset(fifo); memcpy(fifo->owner, owner, 32U);
        fifo->owner_valid = true;
    }
    unsigned budget = CATALOG_INDEX_SLICE;
    if (!fifo->head[0]) {
        if (!fifo->scan_done || fifo->generation != replay_scan_generation) {
            const esp_err_t err = monitor_find_head(fifo, mode, &budget);
            if (err != ESP_OK) return err;
        }
        if (!fifo->head[0]) { *waiting = fifo->waiting; return ESP_OK; }
    }
    return monitor_head_next(fifo, mode, scene, part, found, file, waiting, &budget);
}

/* Unlike EOF, a directory error proves nothing about retained index pages.
 * Keep this end-gate scan local so the cursor is published only on success. */
static esp_err_t monitor_end_first_index(const sd_audio_session_t *scene, uint32_t *first)
{
    bool paged = false;
    esp_err_t err = catalog_is_paged(scene->session_id, &paged);
    if (err != ESP_OK) return err;
    if (!paged) { *first = 0U; return ESP_OK; }
    DIR *dir = opendir(SD_AUDIO_CATALOG_ROOT);
    if (!dir) return ESP_FAIL;
    uint32_t candidate = scene->segment_count;
    char prefix[64]; snprintf(prefix, sizeof(prefix), "%s.index.", scene->session_id);
    const size_t length = strlen(prefix);
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry) { if (errno) err = ESP_FAIL; break; }
        if (strncmp(entry->d_name, prefix, length) || strlen(entry->d_name + length) != 8U) continue;
        char *end = NULL;
        errno = 0;
        const unsigned long page = strtoul(entry->d_name + length, &end, 16);
        if (errno || !end || *end || page >= (scene->segment_count + CATALOG_PAGE_RECORDS - 1U) / CATALOG_PAGE_RECORDS) continue;
        const uint32_t index = (uint32_t)page * CATALOG_PAGE_RECORDS;
        if (index < candidate) candidate = index;
    }
    if (closedir(dir) != 0) err = ESP_FAIL;
    if (err == ESP_OK) *first = candidate;
    return err;
}

static esp_err_t fifo_end_ready(monitor_fifo_state_t *fifo, unsigned mode, sd_audio_session_t *scene, bool *ready)
{
    if (!scene || !ready) return ESP_ERR_INVALID_ARG;
    *ready = false;
    sd_audio_session_t current;
    esp_err_t err = sd_audio_catalog_load(scene->session_id, &current);
    if (err != ESP_OK) return err;
    if (current.mode != mode || memcmp(current.owner, scene->owner, 32U)) return ESP_ERR_INVALID_STATE;
    if (current.done || !current.ended || current.first_pending != current.segment_count)
        return ESP_OK;
    if (!fifo->end.active ||
        memcmp(&current, &fifo->end.scene, sizeof(current))) {
        fifo->end.active = fifo->end.proven = false;
        uint32_t first = 0U;
        err = monitor_end_first_index(&current, &first);
        if (err != ESP_OK) return err;
        fifo->end.scene = current;
        fifo->end.next = first;
        fifo->end.active = true;
    }
    unsigned budget = CATALOG_INDEX_SLICE;
    while (budget-- && fifo->end.next < current.segment_count) {
        sd_audio_segment_t part;
        err = sd_audio_catalog_read(current.session_id, fifo->end.next, &part);
        if (err != ESP_OK) return err;
        if (!part.acknowledged && !part.missing) return ESP_ERR_INVALID_STATE;
        if (part.missing && current.complete) return ESP_ERR_INVALID_STATE;
        ++fifo->end.next;
    }
    if (fifo->end.next != current.segment_count) return ESP_ERR_NOT_FINISHED;
    fifo->end.proven = true;
    *scene = current; *ready = true; return ESP_OK;
}

static esp_err_t fifo_finish(monitor_fifo_state_t *fifo, unsigned mode, sd_audio_session_t *scene)
{
    if (!scene || !fifo->end.proven ||
        strcmp(scene->session_id, fifo->end.scene.session_id)) return ESP_ERR_INVALID_STATE;
    sd_audio_session_t current;
    esp_err_t err = sd_audio_catalog_load(scene->session_id, &current);
    if (err != ESP_OK) return err;
    if (current.mode != mode || current.generation != fifo->end.scene.generation ||
        !current.ended || current.first_pending != current.segment_count ||
        memcmp(current.owner, scene->owner, 32U)) return ESP_ERR_INVALID_STATE;
    current.done = 1U;
    err = sd_audio_catalog_save(&current);
    if (err == ESP_OK) { *scene = current; fifo->head[0] = 0; monitor_scan_reset(fifo); }
    return err;
}

/* Both modes share immutable-file FIFO and the same bounded end proof. */
esp_err_t sd_monitor_fifo_next(const uint8_t owner[32], sd_audio_session_t *scene,
    sd_audio_segment_t *part, bool *found, bool *file, bool *waiting)
{ return fifo_next(&monitor_fifo, 1U, owner, scene, part, found, file, waiting); }
esp_err_t sd_meeting_fifo_next(const uint8_t owner[32], sd_audio_session_t *scene,
    sd_audio_segment_t *part, bool *found, bool *file, bool *waiting)
{
    monitor_fifo_state_t *fifo = meeting_fifo_get();
    return fifo ? fifo_next(fifo, 0U, owner, scene, part, found, file, waiting) : ESP_ERR_NO_MEM;
}
esp_err_t sd_monitor_fifo_end_ready(sd_audio_session_t *scene, bool *ready)
{ return fifo_end_ready(&monitor_fifo, 1U, scene, ready); }
esp_err_t sd_meeting_fifo_end_ready(sd_audio_session_t *scene, bool *ready)
{
    monitor_fifo_state_t *fifo = meeting_fifo_get();
    return fifo ? fifo_end_ready(fifo, 0U, scene, ready) : ESP_ERR_NO_MEM;
}
esp_err_t sd_monitor_fifo_finish(sd_audio_session_t *scene)
{ return fifo_finish(&monitor_fifo, 1U, scene); }
esp_err_t sd_meeting_fifo_finish(sd_audio_session_t *scene)
{
    monitor_fifo_state_t *fifo = meeting_fifo_get();
    return fifo ? fifo_finish(fifo, 0U, scene) : ESP_ERR_NO_MEM;
}
