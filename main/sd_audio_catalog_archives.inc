/* Local recovery metadata only. NWXOFF1/NWXCH1 and all cloud fields stay
 * unchanged. A .ready.meta file binds adjacent CRC slots to the immutable
 * scene owner/start/boot. It preserves capture-origin and durable file ACKs
 * independently of the central index. The uploader still validates every
 * audio record and the full CRC before committing reconstructed entries. */
#define CATALOG_BACKUP_HEADER_BYTES 112U

static uint64_t catalog_u64(const uint8_t *p)
{
    return (uint64_t)u32(p) | (uint64_t)u32(p + 4U) << 32U;
}

static bool catalog_backup_path(const char *audio, char path[160])
{
    if (!audio || strnlen(audio, 128U) >= 128U) return false;
    const int n = snprintf(path, 160U, "%s.meta", audio);
    return n > 0 && n < 160;
}

static bool catalog_audio_name(const char *path, char id[48])
{
    const char *name = strrchr(path, '/');
    if (!name || strncmp(++name, "card_", 5U)) return false;
    const char *tail = strrchr(name + 5U, '_');
    if (!tail || tail == name + 5U || (size_t)(tail - name - 5U) >= 48U) return false;
    memcpy(id, name + 5U, (size_t)(tail - name - 5U));
    id[tail - name - 5U] = 0;
    uint32_t order;
    return valid_id(id) && retention_path(path, id, &order);
}

static void catalog_backup_header(const sd_audio_session_t *scene, uint8_t header[112])
{
    memset(header, 0, CATALOG_BACKUP_HEADER_BYTES);
    memcpy(header, "NWXMETA1", 8U);
    memcpy(header + 8U, scene->owner, 32U);
    memcpy(header + 40U, scene->session_id, 48U);
    memcpy(header + 88U, &scene->start_ms, 8U);
    memcpy(header + 96U, &scene->boot_id, 4U);
    header[100] = scene->mode;
    const uint32_t crc = checksum(header, 108U);
    memcpy(header + 108U, &crc, 4U);
}

static esp_err_t catalog_backup_header_read(FILE *file, const sd_audio_session_t *scene)
{
    uint8_t actual[112], expected[112];
    if (fseek(file, 0L, SEEK_SET) != 0) return ESP_FAIL;
    const size_t got = fread(actual, 1U, sizeof(actual), file);
    if (ferror(file)) return ESP_FAIL;
    if (got != sizeof(actual) || memcmp(actual, "NWXMETA1", 8U) ||
        u32(actual + 108U) != checksum(actual, 108U)) return ESP_ERR_INVALID_CRC;
    catalog_backup_header(scene, expected);
    return memcmp(actual, expected, sizeof(actual)) ? ESP_ERR_INVALID_STATE : ESP_OK;
}

typedef struct { const char *path; uint32_t index; } catalog_backup_key_t;
static bool catalog_backup_matches(const void *record, const void *context)
{
    const sd_audio_segment_t *part = record;
    const catalog_backup_key_t *key = context;
    return part->index < 1000000U &&
        (key->index == UINT32_MAX || key->index == part->index) &&
        segment_record_matches(record, &part->index) &&
        !strcmp(part->path, key->path) && (part->sealed & 1U) && !part->missing;
}

static esp_err_t catalog_backup_read(const sd_audio_session_t *scene, const char *audio,
                                     uint32_t index, sd_audio_segment_t *part)
{
    char path[160];
    if (!catalog_backup_path(audio, path)) return ESP_ERR_INVALID_ARG;
    FILE *file = fopen(path, "rb");
    if (!file) return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    esp_err_t err = catalog_backup_header_read(file, scene);
    if (err == ESP_OK) {
        const catalog_backup_key_t key = {audio, index};
        err = read_pair_matching(file, CATALOG_BACKUP_HEADER_BYTES, part, sizeof(*part),
            offsetof(sd_audio_segment_t, crc), SEGMENT_MAGIC, catalog_backup_matches, &key);
    }
    if (fclose(file) != 0 && err == ESP_OK) err = ESP_FAIL;
    return err;
}

static esp_err_t catalog_backup_quarantine(const char *path)
{
    for (unsigned i = 0U; i < 16U; ++i) {
        char bad[184]; struct stat st;
        const int n = snprintf(bad, sizeof(bad), "%s.bad.%u", path, i);
        if (n < 0 || (size_t)n >= sizeof(bad)) return ESP_ERR_INVALID_SIZE;
        if (stat(bad, &st) == 0) continue;
        if (errno != ENOENT) return ESP_FAIL;
        return rename(path, bad) == 0 ? ESP_OK : ESP_FAIL;
    }
    return ESP_FAIL;
}

static esp_err_t catalog_segment_backup_write(const char *id, const sd_audio_segment_t *segment)
{
    uint32_t order;
    /* Existing host/legacy non-card archives do not acquire a sidecar. */
    if (!retention_path(segment->path, id, &order)) return ESP_OK;
    sd_audio_session_t scene;
    esp_err_t err = sd_audio_catalog_load(id, &scene);
    if (err != ESP_OK) return err;
    char path[160];
    if (!catalog_backup_path(segment->path, path)) return ESP_ERR_INVALID_ARG;
    FILE *file = fopen(path, "r+b");
    bool fresh = false;
    if (!file) {
        if (errno != ENOENT) return ESP_FAIL;
        fresh = true;
    } else {
        err = catalog_backup_header_read(file, &scene);
        if (err != ESP_OK) {
            if (fclose(file) != 0) return ESP_FAIL;
            file = NULL;
            if (err == ESP_FAIL || err == ESP_ERR_INVALID_STATE) return err;
            err = catalog_backup_quarantine(path);
            if (err != ESP_OK) return err;
            fresh = true;
        }
    }
    if (fresh) {
        file = fopen(path, "w+b");
        if (!file) return ESP_FAIL;
        uint8_t header[112]; catalog_backup_header(&scene, header);
        if (fwrite(header, 1U, sizeof(header), file) != sizeof(header) || fflush(file) != 0) {
            (void)fclose(file); return ESP_FAIL;
        }
    }
    sd_audio_segment_t copy = *segment, old;
    copy.generation = 0U;
    const catalog_backup_key_t key = {segment->path, segment->index};
    err = read_pair_matching(file, CATALOG_BACKUP_HEADER_BYTES, &old, sizeof(old),
        offsetof(sd_audio_segment_t, crc), SEGMENT_MAGIC, catalog_backup_matches, &key);
    if (err == ESP_FAIL) { (void)fclose(file); return err; }
    if (err == ESP_OK) {
        if (old.total_bytes != copy.total_bytes || old.payload_crc != copy.payload_crc ||
            old.start_ms != copy.start_ms || old.realtime != copy.realtime) {
            (void)fclose(file); return ESP_ERR_INVALID_STATE;
        }
        copy.generation = old.generation;
        /* A later index fallback must never downgrade a durable backup ACK. */
        if (old.acknowledged) copy.acknowledged = 1U;
    }
    copy.magic = SEGMENT_MAGIC;
    err = write_pair(file, CATALOG_BACKUP_HEADER_BYTES, &copy, sizeof(copy),
                     offsetof(sd_audio_segment_t, crc));
    if (fclose(file) != 0 && err == ESP_OK) err = ESP_FAIL;
    return err;
}

esp_err_t sd_audio_catalog_drop_backup(const char *archive_path)
{
    char id[48], path[160];
    if (!catalog_audio_name(archive_path, id)) return ESP_OK;
    if (!catalog_backup_path(archive_path, path)) return ESP_ERR_INVALID_ARG;
    return remove(path) == 0 || errno == ENOENT ? ESP_OK : ESP_FAIL;
}

/* Keep the large file reconciliation frame out of the directory walker's
 * two bounded directory levels. This reserves stack for FatFs/VFS calls. */
#include "sd_audio_catalog_absence.inc"
#include "sd_audio_catalog_discard.inc"

typedef struct {
    char id[48];
    uint64_t start_ms;
    uint32_t index, total_bytes, payload_crc, last_timestamp;
    uint8_t mode;
    bool valid;
} catalog_archive_probe_t;

/* Read-only evidence collection. Incremental repair never publishes metadata
 * until its complete directory pass has ruled out competing identities. */
static __attribute__((noinline)) esp_err_t catalog_probe_ready(const char *path,
    const char *only_id, uint32_t only_index, catalog_archive_probe_t *probe)
{
    *probe = (catalog_archive_probe_t){0};
    char id[48];
    if (!catalog_audio_name(path, id)) { catalog_absence_other_source(path); return ESP_OK; }
    if (only_id && strcmp(id, only_id)) return ESP_OK;
    if (strlen(path) >= sizeof(((sd_audio_segment_t *)0)->path)) return ESP_ERR_INVALID_SIZE;
    FILE *file = fopen(path, "rb");
    if (!file) return errno == ENOENT ? ESP_OK : errno == ENOMEM ? ESP_ERR_NO_MEM :
        (errno == EMFILE || errno == ENFILE) ? ESP_ERR_NOT_FINISHED : ESP_FAIL;
    uint8_t header[96], footer[32], last[26];
    esp_err_t err = ESP_OK;
    const size_t got = fread(header, 1U, sizeof(header), file);
    bool valid = got == sizeof(header) && !memcmp(header, "NWXOFF1\0", 8U) &&
        header[8] == 1U && !header[9] && header[10] == 96U && !header[11] &&
        header[12] <= 1U && header[13] == 1U && header[14] == 1U && !header[15] &&
        u32(header + 16U) == 16000U && header[20] == 20U && !header[21] &&
        header[22] == 2U && !header[23] && memchr(header + 36U, 0, 48U) &&
        !strcmp((const char *)header + 36U, id) && u32(header + 24U) < 1000000U;
    const uint32_t index = valid ? u32(header + 24U) : UINT32_MAX;
    if (only_index != UINT32_MAX && index != only_index) valid = false;
    long bytes = -1;
    if (valid) {
        if (fseek(file, 0L, SEEK_END) != 0 || (bytes = ftell(file)) < 0) err = ESP_FAIL;
        if (err == ESP_OK && bytes >= 96 + 666 + 32) {
            if (fseek(file, -32L, SEEK_END) != 0 || fread(footer, 1U, 32U, file) != 32U ||
                fseek(file, -(32L + 666L), SEEK_END) != 0 || fread(last, 1U, 26U, file) != 26U)
                err = ferror(file) ? ESP_FAIL : ESP_ERR_INVALID_SIZE;
            if (err == ESP_OK) valid = !memcmp(footer, "NWXEND1\0", 8U) &&
                footer[8] == 1U && !footer[9] && footer[10] == 32U && !footer[11] &&
                u32(footer + 12U) > 0U && footer[28] <= 1U &&
                (uint64_t)bytes == 128ULL + (uint64_t)u32(footer + 12U) * 666ULL &&
                catalog_u64(footer + 16U) == (uint64_t)u32(footer + 12U) * 662ULL &&
                u32(last) == 662U && last[4] == 'N' && last[5] == 'A';
        } else valid = false;
    }
    if (ferror(file)) err = ESP_FAIL;
    if (fclose(file) != 0) err = ESP_FAIL;
    if (err == ESP_ERR_INVALID_SIZE || !valid) catalog_absence_note(id);
    if (err == ESP_ERR_INVALID_SIZE) return ESP_OK; /* Preserve malformed source. */
    if (err != ESP_OK || !valid) return err;

    if ((uint64_t)bytes > UINT32_MAX) return ESP_ERR_INVALID_SIZE;
    memcpy(probe->id, id, sizeof(probe->id));
    probe->index = index; probe->mode = header[12];
    probe->total_bytes = (uint32_t)bytes; probe->start_ms = catalog_u64(header + 28U);
    probe->payload_crc = u32(footer + 24U); probe->last_timestamp = u32(last + 18U);
    probe->valid = true;
    return ESP_OK;
}

static __attribute__((noinline)) esp_err_t catalog_recover_ready(const char *path,
    const char *only_id, uint32_t only_index, bool *changed)
{
    catalog_archive_probe_t probe;
    esp_err_t err = catalog_probe_ready(path, only_id, only_index, &probe);
    if (err != ESP_OK || !probe.valid) return err;
    char id[48], manifest[128];
    memcpy(id, probe.id, sizeof(id));
    const uint32_t index = probe.index;
    const long bytes = (long)probe.total_bytes;
    if (!catalog_path(manifest, id, "session")) return ESP_ERR_INVALID_ARG;
    sd_audio_session_t scene;
    err = load_recovery_session(manifest, id, &scene);
    if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_CRC ||
        err == ESP_ERR_INVALID_STATE || err == ESP_ERR_INVALID_SIZE) return ESP_OK;
    if (err != ESP_OK) return err;
    const uint64_t start = probe.start_ms;
    if (scene.mode != probe.mode || start < scene.start_ms ||
        (scene.ended && scene.ended_ms && start > scene.ended_ms)) {
        catalog_absence_note(id); return ESP_OK;
    }
    sd_audio_segment_t old = {0}, proof = {0};
    const esp_err_t indexed = sd_audio_catalog_read(id, index, &old);
    if (indexed == ESP_FAIL) return indexed;
    if (indexed == ESP_OK && (old.acknowledged || old.missing >= SD_AUDIO_SEGMENT_RETIRING)) return ESP_OK;
    if (indexed == ESP_OK && !old.missing && (old.sealed & 1U) && !strcmp(old.path, path) &&
        old.total_bytes == (uint64_t)bytes && old.payload_crc == probe.payload_crc) return ESP_OK;
    if (indexed == ESP_OK && old.path[0] && strcmp(old.path, path)) {
        struct stat st;
        if (stat(old.path, &st) == 0)
            return catalog_discard_record(&scene, index, NULL, NULL);
        if (errno != ENOENT) return ESP_FAIL;
    }
    const esp_err_t backup = catalog_backup_read(&scene, path, index, &proof);
    if (backup == ESP_FAIL) return backup;
    if (backup == ESP_ERR_INVALID_STATE)
        return catalog_discard_record(&scene, index, NULL, NULL);
    /* Without a surviving independent backup, the sealed index remains the
     * original witness. Retain it on a disagreeing read and let the fresh
     * recovery/replay checks decide. A valid same-owner backup still proves
     * legitimate index reconstruction through the existing path below. */
    const bool original_index = indexed == ESP_OK && (old.sealed & 1U) && !strcmp(old.path, path);
    if (original_index && backup != ESP_OK &&
        (old.total_bytes != (uint64_t)bytes || old.payload_crc != probe.payload_crc)) return ESP_OK;
    if (backup == ESP_OK && (proof.total_bytes != (uint64_t)bytes ||
        proof.payload_crc != probe.payload_crc || proof.start_ms != start)) {
        if (original_index) return ESP_OK;
        return catalog_discard_record(&scene, index, NULL, NULL);
    }
    sd_audio_segment_t part = {0};
    if (indexed == ESP_OK) part.generation = old.generation;
    strcpy(part.path, path); part.index = index; part.start_ms = start;
    part.end_ms = start + (uint32_t)(probe.last_timestamp - (uint32_t)start) + 20U;
    part.total_bytes = (uint32_t)bytes; part.payload_crc = probe.payload_crc;
    part.sealed = 1U | SD_AUDIO_SEGMENT_ATTEMPTED;
    if (backup == ESP_OK) { part.realtime = proof.realtime; part.acknowledged = proof.acknowledged; }
    else if (indexed == ESP_OK && old.path[0]) part.realtime = old.realtime;
    /* Pre-sidecar damaged monitor indexes have no surviving origin bit.
     * Recover their audio conservatively as history; never fabricate live
     * processing or a complete recording. Healthy entries are unchanged. */
    if (!part.acknowledged) {
        scene.done = scene.complete = 0U;
        if (scene.segment_count <= index) scene.segment_count = index + 1U;
        if (scene.first_pending > index) scene.first_pending = index;
        err = persist_repaired_session(manifest, &scene);
        if (err != ESP_OK) return err;
    }
    err = sd_audio_catalog_write(id, &part);
    if (err == ESP_OK) {
        *changed = true;
#ifdef ESP_PLATFORM
        ESP_LOGD("sd_audio_catalog", "reindexed retained audio session=%s index=%lu ack=%u origin=%s proof=%u; recording completeness not promoted",
            id, (unsigned long)index, part.acknowledged, part.realtime ? "realtime" : "history",
            backup == ESP_OK ? 1U : 0U);
#endif
    }
    return err;
}

esp_err_t sd_audio_catalog_reconcile_file(const sd_audio_session_t *expected,
    const sd_audio_segment_t *expected_part, bool *changed)
{
    if (!expected || !expected_part || !changed ||
        !valid_id(expected->session_id) ||
        !memchr(expected_part->path, 0, sizeof(expected_part->path))) return ESP_ERR_INVALID_ARG;
    *changed = false;
    sd_audio_session_t current;
    sd_audio_segment_t part;
    esp_err_t err = sd_audio_catalog_load(expected->session_id, &current);
    if (err != ESP_OK) return err;
    if (current.done || current.mode != expected->mode || current.boot_id != expected->boot_id ||
        current.start_ms != expected->start_ms || memcmp(current.owner, expected->owner, 32U))
        return ESP_ERR_INVALID_STATE;
    err = sd_audio_catalog_read(expected->session_id, expected_part->index, &part);
    if (err != ESP_OK) return err;
    if (memcmp(&part, expected_part, sizeof(part)) || part.acknowledged || part.missing ||
        !(part.sealed & 1U)) return ESP_ERR_INVALID_STATE;
    return catalog_recover_ready(part.path, current.session_id, part.index, changed);
}

static bool catalog_bucket(const char *name)
{
    if (strlen(name) != 8U) return false;
    for (unsigned i = 0U; i < 8U; ++i)
        if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f') ||
              (name[i] >= 'A' && name[i] <= 'F'))) return false;
    return true;
}

static __attribute__((noinline)) esp_err_t catalog_recover_backup(const char *path, const char *only_id,
                                       uint32_t only_index, bool *changed)
{
    const size_t length = strlen(path);
    if (length < 5U || length - 5U >= 128U) return ESP_OK;
    char audio[128], id[48], manifest[128];
    memcpy(audio, path, length - 5U); audio[length - 5U] = 0;
    if (!catalog_audio_name(audio, id)) { catalog_absence_other_source(audio); return ESP_OK; }
    if (only_id && strcmp(id, only_id)) return ESP_OK;
    struct stat st;
    const bool present = stat(audio, &st) == 0;
    if (!present && errno != ENOENT) return ESP_FAIL;
    if (!catalog_path(manifest, id, "session")) return ESP_ERR_INVALID_ARG;
    sd_audio_session_t scene;
    esp_err_t err = load_recovery_session(manifest, id, &scene);
    if (err == ESP_FAIL) return err;
    if (err != ESP_OK) return ESP_OK;
    sd_audio_segment_t part;
    err = catalog_backup_read(&scene, audio, only_index, &part);
    if (err == ESP_FAIL) return err;
    if (err != ESP_OK) { catalog_absence_note(id); return ESP_OK; }
    if ((!present && !part.acknowledged) || part.start_ms < scene.start_ms) {
        catalog_absence_note(id); return ESP_OK;
    }
    sd_audio_segment_t old;
    const esp_err_t indexed = sd_audio_catalog_read(id, part.index, &old);
    if (indexed == ESP_FAIL) return indexed;
    if (indexed == ESP_OK && (old.acknowledged || old.missing >= SD_AUDIO_SEGMENT_RETIRING)) return ESP_OK;
    if (indexed == ESP_OK && !part.acknowledged && !old.missing) return ESP_OK;
    if (indexed != ESP_OK || !old.acknowledged || old.total_bytes != part.total_bytes ||
        old.payload_crc != part.payload_crc || strcmp(old.path, audio)) {
        if (scene.segment_count <= part.index || scene.first_pending > scene.segment_count) {
            if (scene.segment_count <= part.index) scene.segment_count = part.index + 1U;
            if (scene.first_pending > scene.segment_count) scene.first_pending = scene.segment_count;
            err = persist_repaired_session(manifest, &scene);
            if (err != ESP_OK) return err;
        }
        part.generation = indexed == ESP_OK ? old.generation : 0U;
        err = sd_audio_catalog_write(id, &part);
        if (err != ESP_OK) return err;
        *changed = true;
    }
    /* Only durable ACK proof may be removed. A restored unconfirmed tuple
     * still needs its independent backup until normal upload/discard. */
    return part.acknowledged ? sd_audio_catalog_drop_backup(audio) : ESP_OK;
}

static esp_err_t catalog_walk_audio(const char *root, bool buckets, const char *only_id,
                                   uint32_t only_index, bool *changed)
{
    DIR *dir = opendir(root);
    if (!dir) return errno == ENOENT ? ESP_OK : ESP_FAIL;
    esp_err_t err = ESP_OK;
    unsigned work = 0U;
    for (;;) {
        errno = 0; struct dirent *entry = readdir(dir);
        if (!entry) { if (errno) err = ESP_FAIL; break; }
        char path[160];
        if (entry->d_name[0] == '.') continue;
        const size_t length = strlen(entry->d_name);
        const bool ready = length >= 6U && !strcmp(entry->d_name + length - 6U, ".ready");
        const bool backup = length >= 11U && !strcmp(entry->d_name + length - 11U, ".ready.meta");
        const int n = snprintf(path, sizeof(path), "%s/%s", root, entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(path)) { err = ESP_ERR_INVALID_SIZE; break; }
        if (buckets ? !catalog_bucket(entry->d_name) : (!ready && !backup)) {
            catalog_absence_other_source(path); continue;
        }
        err = buckets ? catalog_walk_audio(path, false, only_id, only_index, changed) :
            ready ? catalog_recover_ready(path, only_id, only_index, changed) :
                    catalog_recover_backup(path, only_id, only_index, changed);
        if (err != ESP_OK) break;
#ifdef ESP_PLATFORM
        if ((++work % 16U) == 0U) vTaskDelay(1U);
#else
        (void)work;
#endif
    }
    if (closedir(dir) != 0) err = ESP_FAIL;
    return err;
}

static esp_err_t catalog_recover_ready_archives(const char *only_id, uint32_t only_index, bool *changed)
{
    *changed = false;
    esp_err_t err = catalog_walk_audio(SD_AUDIO_ARCHIVE_ROOT, false, only_id, only_index, changed);
    if (err == ESP_OK) err = catalog_walk_audio(SD_AUDIO_ARCHIVE_NEW_ROOT, true, only_id, only_index, changed);
    return err;
}

#include "sd_audio_catalog_reconcile.inc"
