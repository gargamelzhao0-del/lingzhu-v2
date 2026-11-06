/* Included only by the serialized recording writer. Repair never allocates a
 * second audio shadow, remounts, reserves IDs, or changes recording counters. */
#if CONFIG_NUWAX_SD_BATCH_REPAIR_ENABLE
#include "sd_audio_file_fault.h"
/* A 4 MiB prefix already takes several seconds on the measured card path.
 * Keep its original whole-file CRC check, with a deadline below the 15 s
 * capture retention grace instead of an unattainable 1.5 s full-prefix limit. */
enum { SD_BATCH_REPAIR_LIMIT_US = NUWAX_SD_REPAIR_BUDGET_MS * 1000U, SD_BATCH_REPAIR_CHUNK = 4096 };

/* Called only by serialized idle recovery/finalization, with FILE users stopped. It
 * must run before generic .part salvage can recompute a checksum from known
 * damaged PCM. Retain the frozen snapshot even after a successful rename. */
static esp_err_t writer_repair_quarantine_known(const char *path, bool *handled)
{
    const sd_integrity_t *diag = s.integrity;
    const bool frozen = diag && diag->frozen && !strcmp(diag->path, path);
    const bool current_fault = sd_audio_stage_is_integrity_fault(s_writer_fault_stage) &&
        s.open_path[0] && !strcmp(s.open_path, path);
    *handled = frozen || current_fault;
    if (!*handled) return ESP_OK;
    nuwax_sd_card_identity_t current;
    const char *session = frozen ? diag->session_id : s.session_id;
    if (strcmp(s_card_session.session_id, session) ||
        !s_card_io_recovery.identity_valid || !nuwax_sd_card_get_identity(&current) ||
        memcmp(&current, &s_card_io_recovery.identity, sizeof(current))) {
        ESP_LOGW(TAG, "SD known damaged file retained without recovery after identity change path=%s", path);
        return ESP_OK; /* Never rename a different card's same-named file. */
    }
    char bad[176];
    if (strlen(path) + sizeof(".bad") > sizeof(bad)) return ESP_ERR_INVALID_SIZE;
    snprintf(bad, sizeof(bad), "%s.bad", path);
    struct stat st;
    if (stat(bad, &st) == 0) {
        ESP_LOGW(TAG, "SD known damaged file retained; quarantine already exists path=%s", path);
        return ESP_OK; /* Keep both originals; never overwrite an evidence file. */
    }
    if (errno != ENOENT || rename(path, bad) != 0) return ESP_FAIL;
    ESP_LOGW(TAG, "SD known damaged file quarantined path=%s; original bytes retained, no replacement CRC", bad);
    return ESP_OK;
}

typedef struct {
    uint32_t generation, media;
    int64_t started;
    FILE *file;
} writer_repair_guard_t;

static bool writer_repair_allowed(const writer_repair_guard_t *guard)
{
    if (esp_timer_get_time() - guard->started >= SD_BATCH_REPAIR_LIMIT_US ||
        atomic_load(&s_shutdown_requested) || nuwax_power_ctrl_shutdown_preparing() ||
        !atomic_load(&s.active) || atomic_load(&s.degraded) || atomic_load(&s.writer_error) ||
        atomic_load(&s_card_failed) || !atomic_load(&s_media_ready) ||
        !card_recording_locked() || !nuwax_sd_card_is_mounted() || s.file != guard->file ||
        s.capture_generation != guard->generation || atomic_load(&s_media_generation) != guard->media ||
        !s_card_io_recovery.identity_valid || !writer_integrity_live() ||
        s.integrity->generation != guard->generation || s.integrity->index != s.segment_index ||
        strcmp(s.integrity->session_id, s.session_id) || strcmp(s.integrity->path, s.open_path)) return false;
    nuwax_sd_card_identity_t current;
    return nuwax_sd_card_get_identity(&current) &&
        !memcmp(&current, &s_card_io_recovery.identity, sizeof(current));
}

/* A fresh FILE, independent read cache and the original aggregate PCM-frame
 * CRC validate ALL accepted records, including batches before the repaired one.
 * This runs only after a repair and before sealing a repaired file. */
static esp_err_t writer_repair_verify_prefix(const writer_repair_guard_t *guard, bool sealed)
{
    if (!writer_repair_allowed(guard)) return ESP_ERR_INVALID_STATE;
    FILE *file = fopen(s.open_path, "rb");
    if (!file) return ESP_FAIL;
    archive_header_t header;
    _Alignas(int16_t) uint8_t frame[662];
    struct stat st;
    uint32_t frames = 0U, crc = 0U, last_sequence = 0U;
    uint64_t bytes = 0U;
    long offset = HEADER_BYTES;
    size_t work = 0U;
    bool sequence_seen = false;
    esp_err_t err = ESP_OK;
    if (setvbuf(file, s.write_buffer + ARCHIVE_STDIO_BUFFER_BYTES, _IOFBF, ARCHIVE_STDIO_BUFFER_BYTES) != 0 ||
        fstat(fileno(file), &st) != 0 || fread(&header, 1U, sizeof(header), file) != sizeof(header)) err = ESP_FAIL;
    if (err == ESP_OK && (st.st_size != s.last_good_offset + (sealed ? (long)FOOTER_BYTES : 0L) ||
        s.integrity->head_bytes < sizeof(header) || memcmp(&header, s.integrity->head, sizeof(header))))
        err = ESP_ERR_INVALID_CRC;
    while (err == ESP_OK && offset < s.last_good_offset) {
        uint32_t length = 0U;
        if (s.last_good_offset - offset < (long)(sizeof(length) + sizeof(frame)) ||
            fread(&length, 1U, sizeof(length), file) != sizeof(length) || length != sizeof(frame)) {
            err = ESP_ERR_INVALID_SIZE; break;
        }
        if (fread(frame, 1U, length, file) != length) { err = ESP_FAIL; break; }
        err = validate_archive_frame(frame, length, &header, &last_sequence, &sequence_seen);
        if (err != ESP_OK) break;
        crc = crc32_update(crc, frame, length);
        bytes += length; ++frames; offset += sizeof(length) + length;
        work += sizeof(length) + length;
        if (work >= 32768U) {
            vTaskDelay(1U); work = 0U;
            if (!writer_repair_allowed(guard)) err = ESP_ERR_INVALID_STATE;
        }
    }
    if (err == ESP_OK && (offset != s.last_good_offset || frames != s.frame_count ||
        bytes != s.payload_bytes || crc != s.payload_crc32)) err = ESP_ERR_INVALID_CRC;
    if (ferror(file)) err = ESP_FAIL;
    if (fclose(file) != 0) err = ESP_FAIL;
    if (err == ESP_OK && !writer_repair_allowed(guard)) err = ESP_ERR_INVALID_STATE;
    if (err != ESP_OK) ESP_LOGE(TAG,
        "SD repair prefix invalid session=%s index=%lu sealed=%u offset=%ld frames=%lu/%lu bytes=%llu/%llu crc=%08lx/%08lx elapsed_ms=%lu err=%s",
        s.session_id, (unsigned long)s.segment_index, sealed, offset,
        (unsigned long)frames, (unsigned long)s.frame_count, (unsigned long long)bytes,
        (unsigned long long)s.payload_bytes, (unsigned long)crc, (unsigned long)s.payload_crc32,
        (unsigned long)((esp_timer_get_time() - guard->started) / 1000), esp_err_to_name(err));
    return err;
}

static void writer_repair_log_native(void)
{
    const nuwax_sd_write_verify_stats_t stats = nuwax_sd_write_verify_stats();
    ESP_LOGW(TAG, "SD batch native writes=%lu sectors=%lu rewrites=%lu recovered=%lu failures=%lu memory_denied=%lu max_us=%lu",
        (unsigned long)stats.writes, (unsigned long)stats.verified_sectors, (unsigned long)stats.rewrites,
        (unsigned long)stats.recovered, (unsigned long)stats.failures,
        (unsigned long)stats.memory_denied, (unsigned long)stats.max_us);
}

static esp_err_t writer_repair_batch(uint32_t observed_crc)
{
    writer_repair_guard_t guard = {s.capture_generation, atomic_load(&s_media_generation), esp_timer_get_time(), s.file};
    sd_integrity_t *diag = s.integrity;
    if (!writer_repair_allowed(&guard) || diag->repair_attempted || diag->seal_prepared ||
        !diag->shadow_covered || !diag->records || s.sample_rate != 16000U ||
        s.channels != 1U || s.frame_duration_ms != 20U || s.verify_offset < 0 ||
        diag->offset != (uint64_t)s.verify_offset || diag->body_end != (uint64_t)s.last_good_offset ||
        diag->shadow_bytes != s.verify_bytes || diag->shadow_bytes > sizeof(diag->shadow) ||
        s.last_good_offset - s.verify_offset != (long)s.verify_bytes || ftell(s.file) != s.last_good_offset ||
        diag->shadow_crc != s.verify_crc32 ||
        crc32_update(0U, diag->shadow, diag->shadow_bytes) != s.verify_crc32) return ESP_ERR_INVALID_CRC;
    /* A failed attempt is never made eligible again by commit/close. Only a
     * newly verified batch resets repair_attempted. */
    diag->repair_attempted = true;
    if (!nuwax_sd_integrity_io_repair_begin()) return ESP_ERR_INVALID_CRC;
    diag->repaired_file = true;
    ESP_LOGW(TAG, "SD batch repair revision=214 begin session=%s index=%lu offset=%ld bytes=%lu expected_crc=%08lx observed_crc=%08lx first_relative=%lu differing_bytes=%lu",
        s.session_id, (unsigned long)s.segment_index, s.verify_offset, (unsigned long)s.verify_bytes,
        (unsigned long)s.verify_crc32, (unsigned long)observed_crc,
        (unsigned long)diag->first_difference, (unsigned long)diag->differing_bytes);
    writer_repair_log_native();
    const char *phase = "seek";
    esp_err_t err = fseek(s.file, s.verify_offset, SEEK_SET) == 0 ? ESP_OK : ESP_FAIL;
    uint32_t written = 0U;
    phase = "rewrite";
    while (err == ESP_OK && written < diag->shadow_bytes) {
        if (!writer_repair_allowed(&guard)) { err = ESP_ERR_INVALID_STATE; break; }
        const uint32_t remaining = diag->shadow_bytes - written;
        const size_t count = remaining < SD_BATCH_REPAIR_CHUNK ? remaining : SD_BATCH_REPAIR_CHUNK;
        if (fwrite(diag->shadow + written, 1U, count, s.file) != count) { err = ESP_FAIL; break; }
        written += count;
    }
    if (err == ESP_OK) {
        phase = "sync";
        if (!writer_repair_allowed(&guard)) err = ESP_ERR_INVALID_STATE;
        else if (!sync_archive_file(s.file, "batch_repair_sync")) err = ESP_FAIL;
    }
    if (err == ESP_OK && (ftell(s.file) != s.last_good_offset ||
        crc32_update(0U, diag->shadow, diag->shadow_bytes) != s.verify_crc32)) err = ESP_ERR_INVALID_CRC;
    if (err == ESP_OK) { phase = "prefix"; err = writer_repair_verify_prefix(&guard, false); }
    if (err != ESP_OK) {
        /* Do not let a buffered fclose retry writes after card/power/identity
         * rejection. Keep the sticky SDK write fence until FILE cleanup ends. */
        nuwax_sd_integrity_io_repair_abort();
        const int closed = fclose(s.file); s.file = NULL;
        if (closed) writer_note_fault("batch_repair_close", errno);
    }
    nuwax_sd_integrity_io_repair_end();
    if (err != ESP_OK) {
        /* Aborting a buffered FILE can leave FatFs state uncertain. Require
         * ordered idle recovery before any subsequent archive/export user. */
        atomic_store(&s_media_ready, false);
        nuwax_sd_card_mark_io_fault(err);
    }
    ESP_LOGW(TAG, "SD batch repair revision=214 result session=%s index=%lu phase=%s written=%lu elapsed_ms=%lu err=%s",
        s.session_id, (unsigned long)s.segment_index, phase, (unsigned long)written,
        (unsigned long)((esp_timer_get_time() - guard.started) / 1000), esp_err_to_name(err));
    writer_repair_log_native();
    return err;
}

static esp_err_t writer_repair_check_seal(void)
{
    if (!card_recording_locked() || !s.integrity || !s.integrity->repaired_file) return ESP_OK;
    /* An unsaved frozen snapshot can intentionally outlive its file/scene.
     * Its repair flag must never block a new recording that it did not track. */
    if (s.integrity->generation != s.capture_generation || s.integrity->index != s.segment_index ||
        strcmp(s.integrity->session_id, s.session_id) || strcmp(s.integrity->path, s.open_path)) return ESP_OK;
    writer_repair_guard_t guard = {s.capture_generation, atomic_load(&s_media_generation), esp_timer_get_time(), s.file};
    /* Closing/sealing is a separate phase from batch repair. Bound its fresh
     * whole-prefix read at the SDK transaction level as well as in the loop;
     * the mounted descriptor and other callers' timeouts stay unchanged. */
    const bool bounded = nuwax_sd_integrity_io_budget_begin(NUWAX_SD_REPAIR_BUDGET_MS);
    const esp_err_t err = bounded ? writer_repair_verify_prefix(&guard, true) : ESP_ERR_INVALID_STATE;
    if (bounded) nuwax_sd_integrity_io_budget_end();
    if (err != ESP_OK) {
        writer_note_fault("repair_prefix_readback", errno);
        writer_integrity_fault(SD_INTEGRITY_RECORD, s.payload_crc32, 0U);
        ESP_LOGE(TAG, "SD repaired file prefix rejected before publish session=%s index=%lu err=%s",
            s.session_id, (unsigned long)s.segment_index, esp_err_to_name(err));
    }
    return err;
}
#endif
