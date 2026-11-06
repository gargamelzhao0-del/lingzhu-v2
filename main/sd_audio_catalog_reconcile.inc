/* Storage-lock protected, bounded metadata repair. Directory cursors may
 * survive a yield, never FILE/DMA buffers. Remount/cancel explicitly closes
 * them. Probing is read-only: no record changes until the whole pass finishes. */
#ifndef SD_AUDIO_REPAIR_NOW_US
#ifdef ESP_PLATFORM
#define SD_AUDIO_REPAIR_NOW_US() esp_timer_get_time()
#else
#include <time.h>
#define SD_AUDIO_REPAIR_NOW_US() ((int64_t)clock() * 1000000LL / CLOCKS_PER_SEC)
#endif
#endif
#define CATALOG_REPAIR_SLICE_US 4000LL
#define CATALOG_REPAIR_ENTRIES 16U
#define CATALOG_REPAIR_FILES 2U
typedef struct {
    sd_audio_session_t scene;
    uint32_t index;
    DIR *root, *bucket;
    unsigned phase, pending_kind, candidate_kind;
    bool active, conflict, commit_pending;
    char bucket_name[9], pending[160], candidate[128];
} catalog_repair_scan_t;
static catalog_repair_scan_t *catalog_repairs;
static int64_t catalog_repair_deadline;

void sd_audio_catalog_repair_set_deadline(int64_t until_us)
{
    catalog_repair_deadline = until_us;
}

static esp_err_t catalog_repair_close(catalog_repair_scan_t *scan)
{
    esp_err_t err = ESP_OK;
    if (scan->bucket && closedir(scan->bucket) != 0) err = ESP_FAIL;
    if (scan->root && closedir(scan->root) != 0) err = ESP_FAIL;
    memset(scan, 0, sizeof(*scan));
    return err;
}

esp_err_t sd_audio_catalog_repair_reset(void)
{
    esp_err_t err = ESP_OK;
    if (catalog_repairs) for (unsigned i = 0U; i < 2U; ++i)
        if (catalog_repair_close(&catalog_repairs[i]) != ESP_OK) err = ESP_FAIL;
    catalog_repair_deadline = 0;
    return err;
}

static catalog_repair_scan_t *catalog_repair_get(unsigned mode)
{
    if (!catalog_repairs) {
#ifdef ESP_PLATFORM
        catalog_repairs = heap_caps_calloc(2U, sizeof(*catalog_repairs), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
        catalog_repairs = calloc(2U, sizeof(*catalog_repairs));
#endif
    }
    return catalog_repairs ? &catalog_repairs[mode] : NULL;
}

static bool catalog_repair_identity(const sd_audio_session_t *a, const sd_audio_session_t *b)
{
    return !strcmp(a->session_id, b->session_id) && a->mode == b->mode &&
        a->boot_id == b->boot_id && a->start_ms == b->start_ms && !memcmp(a->owner, b->owner, 32U);
}

static bool catalog_repair_due(int64_t started)
{
    const int64_t now = SD_AUDIO_REPAIR_NOW_US();
    return now - started >= CATALOG_REPAIR_SLICE_US ||
        (catalog_repair_deadline && now >= catalog_repair_deadline);
}

static esp_err_t catalog_repair_next(catalog_repair_scan_t *scan,
    unsigned *entries, int64_t started, bool *done)
{
    *done = false;
    while (*entries && !catalog_repair_due(started)) {
        if (scan->phase == 2U) { *done = true; return ESP_OK; }
        if (!scan->root) {
            scan->root = opendir(scan->phase ? SD_AUDIO_ARCHIVE_NEW_ROOT : SD_AUDIO_ARCHIVE_ROOT);
            if (!scan->root) {
                if (errno == ENOENT) { ++scan->phase; continue; }
                return errno == ENOMEM ? ESP_ERR_NO_MEM :
                    (errno == EMFILE || errno == ENFILE) ? ESP_ERR_NOT_FINISHED : ESP_FAIL;
            }
        }
        --*entries;
        errno = 0;
        struct dirent *entry = readdir(scan->bucket ? scan->bucket : scan->root);
        if (!entry) {
            if (errno) return ESP_FAIL;
            if (scan->bucket) {
                DIR *dir = scan->bucket; scan->bucket = NULL;
                if (closedir(dir) != 0) return ESP_FAIL;
            } else {
                DIR *dir = scan->root; scan->root = NULL; ++scan->phase;
                if (closedir(dir) != 0) return ESP_FAIL;
            }
            continue;
        }
        if (scan->phase && !scan->bucket) {
            if (!catalog_bucket(entry->d_name)) continue;
            char path[160];
            const int n = snprintf(path, sizeof(path), "%s/%s", SD_AUDIO_ARCHIVE_NEW_ROOT, entry->d_name);
            if (n < 0 || n >= (int)sizeof(path)) return ESP_ERR_INVALID_SIZE;
            memcpy(scan->bucket_name, entry->d_name, 8U); scan->bucket_name[8] = 0;
            scan->bucket = opendir(path);
            if (!scan->bucket && errno != ENOENT) return errno == ENOMEM ? ESP_ERR_NO_MEM :
                (errno == EMFILE || errno == ENFILE) ? ESP_ERR_NOT_FINISHED : ESP_FAIL;
            continue;
        }
        const size_t n = strlen(entry->d_name);
        const bool ready = n >= 6U && !strcmp(entry->d_name + n - 6U, ".ready");
        const bool backup = n >= 11U && !strcmp(entry->d_name + n - 11U, ".ready.meta");
        if (!ready && !backup) continue;
        char path[160], audio[128], id[48];
        const int length = scan->phase ? snprintf(path, sizeof(path), "%s/%s/%s",
            SD_AUDIO_ARCHIVE_NEW_ROOT, scan->bucket_name, entry->d_name) :
            snprintf(path, sizeof(path), "%s/%s", SD_AUDIO_ARCHIVE_ROOT, entry->d_name);
        if (length < 0 || length >= (int)sizeof(path)) return ESP_ERR_INVALID_SIZE;
        const size_t bytes = (size_t)length - (backup ? 5U : 0U);
        if (bytes >= sizeof(audio)) return ESP_ERR_INVALID_SIZE;
        memcpy(audio, path, bytes); audio[bytes] = 0;
        if (!catalog_audio_name(audio, id) || strcmp(id, scan->scene.session_id)) continue;
        strcpy(scan->pending, path); scan->pending_kind = backup ? 2U : 1U;
        return ESP_OK;
    }
    return ESP_ERR_NOT_FINISHED;
}

static esp_err_t catalog_repair_probe(catalog_repair_scan_t *scan)
{
    bool valid = false;
    char audio[128];
    const size_t n = strlen(scan->pending) - (scan->pending_kind == 2U ? 5U : 0U);
    if (n >= sizeof(audio)) return ESP_ERR_INVALID_SIZE;
    memcpy(audio, scan->pending, n); audio[n] = 0;
    if (scan->pending_kind == 1U) {
        catalog_archive_probe_t probe;
        const esp_err_t err = catalog_probe_ready(audio, scan->scene.session_id, scan->index, &probe);
        if (err != ESP_OK) return err;
        valid = probe.valid && probe.mode == scan->scene.mode && probe.start_ms >= scan->scene.start_ms &&
            (!scan->scene.ended || !scan->scene.ended_ms || probe.start_ms <= scan->scene.ended_ms);
    } else {
        struct stat st;
        const bool present = stat(audio, &st) == 0;
        if (!present && errno != ENOENT) return ESP_FAIL;
        sd_audio_segment_t part;
        const esp_err_t err = catalog_backup_read(&scan->scene, audio, scan->index, &part);
        if (err == ESP_FAIL || err == ESP_ERR_NO_MEM || err == ESP_ERR_NOT_FINISHED) return err;
        valid = err == ESP_OK && (present || part.acknowledged) && part.start_ms >= scan->scene.start_ms;
    }
    if (valid) {
        if (scan->candidate_kind && strcmp(scan->candidate, audio)) scan->conflict = true;
        else { strcpy(scan->candidate, audio); scan->candidate_kind = scan->pending_kind; }
    }
    return ESP_OK;
}

static esp_err_t catalog_repair_step(catalog_repair_scan_t *scan, bool *done)
{
    const int64_t started = SD_AUDIO_REPAIR_NOW_US();
    unsigned entries = CATALOG_REPAIR_ENTRIES, files = CATALOG_REPAIR_FILES;
    *done = false;
    while (!catalog_repair_due(started)) {
        if (!scan->pending_kind) {
            const esp_err_t err = catalog_repair_next(scan, &entries, started, done);
            if (err != ESP_OK || *done) return err;
        }
        if (!files || catalog_repair_due(started)) return ESP_ERR_NOT_FINISHED;
        --files;
        const esp_err_t err = catalog_repair_probe(scan);
        if (err != ESP_OK) return err; /* Keep this pending name through a retry. */
        scan->pending_kind = 0U;
        if (scan->conflict) { *done = true; return ESP_OK; }
    }
    return ESP_ERR_NOT_FINISHED;
}

static esp_err_t catalog_read_reconciled(const char *id, uint32_t index, sd_audio_segment_t *part)
{
    const esp_err_t original = sd_audio_catalog_read(id, index, part);
    if (original == ESP_OK) {
        if (part->missing == SD_AUDIO_SEGMENT_RETIRED) {
            sd_audio_session_t scene;
            if (sd_audio_catalog_load(id, &scene) == ESP_OK) catalog_discard_unlink(&scene, part);
        }
        if (catalog_repairs) for (unsigned i = 0U; i < 2U; ++i)
            if (catalog_repairs[i].active && catalog_repairs[i].index == index &&
                !strcmp(catalog_repairs[i].scene.session_id, id)) {
                const esp_err_t closed = catalog_repair_close(&catalog_repairs[i]);
                if (closed != ESP_OK) return closed;
            }
        return ESP_OK;
    }
    if (original != ESP_ERR_NOT_FOUND && original != ESP_ERR_INVALID_CRC &&
        original != ESP_ERR_INVALID_STATE && original != ESP_ERR_INVALID_SIZE) return original;
    sd_audio_session_t current;
    esp_err_t err = sd_audio_catalog_load(id, &current);
    if (err != ESP_OK) return err;
    if (current.done || session_blocked(id)) return ESP_ERR_INVALID_STATE;
    catalog_repair_scan_t *scan = catalog_repair_get(current.mode);
    if (!scan) return ESP_ERR_NO_MEM;
    if (scan->active && (scan->index != index || !catalog_repair_identity(&scan->scene, &current))) {
        err = catalog_repair_close(scan);
        if (err != ESP_OK) return err;
    }
    if (!scan->active) { scan->active = true; scan->index = index; }
    scan->scene = current;
    bool owns_deadline = false, done = false;
#ifdef ESP_PLATFORM
    if (catalog_repair_deadline) {
        if (esp_timer_get_time() >= catalog_repair_deadline) return ESP_ERR_NOT_FINISHED;
        err = nuwax_sd_card_io_deadline_begin(catalog_repair_deadline);
        if (err != ESP_OK) return ESP_ERR_NOT_FINISHED;
        owns_deadline = true;
    }
#endif
    if (scan->commit_pending) { err = ESP_OK; done = true; }
    else err = catalog_repair_step(scan, &done);
    /* Commit gets its own admission slice. Reopening the selected candidate
     * must not add another native read to an already consumed probe budget. */
    if (err == ESP_OK && done && !scan->commit_pending) {
        scan->commit_pending = true;
        err = ESP_ERR_NOT_FINISHED;
    }
    if (err == ESP_OK && done) {
        char candidate[160]; strcpy(candidate, scan->candidate);
        const unsigned kind = scan->candidate_kind;
        const bool conflict = scan->conflict;
        err = catalog_repair_close(scan);
        if (err == ESP_OK && conflict) {
            err = catalog_discard_record(&current, index, NULL, part);
        } else if (err == ESP_OK && kind) {
            bool changed = false;
            if (kind == 2U) strcat(candidate, ".meta");
            err = kind == 1U ? catalog_recover_ready(candidate, id, index, &changed) :
                catalog_recover_backup(candidate, id, index, &changed);
            if (err == ESP_OK) err = session_blocked(id) ? ESP_ERR_INVALID_STATE :
                sd_audio_catalog_read(id, index, part);
        } else if (err == ESP_OK) {
            /* A complete, error-free source pass found nothing repairable.
             * The user policy discards this one number, never the scene. */
            err = catalog_discard_record(&current, index, NULL, part);
        }
    }
#ifdef ESP_PLATFORM
    const bool expired = owns_deadline && esp_timer_get_time() >= catalog_repair_deadline;
    if (owns_deadline) nuwax_sd_card_io_deadline_end();
    if (expired) err = ESP_ERR_NOT_FINISHED;
#else
    (void)owns_deadline;
#endif
    return err;
}
