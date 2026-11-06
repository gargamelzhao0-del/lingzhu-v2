#if CONFIG_NUWAX_SD_SESSION_ENABLE
#include "sd_audio_file_fault.h"
#include <errno.h>
/* The MIC keeps its existing bounded staging ring. Only the storage owner
 * remounts, with admission -> writer gate -> storage lock held, no FILE or
 * replay lease alive. Never launch a capture task or change its identity. */
#define CARD_IO_RECOVERY_LIMIT_MS 10000U

static uint32_t card_io_recovery_budget_ms(uint32_t now)
{
    const uint32_t expiry = atomic_load_explicit(&s_capture_recovery_until, memory_order_acquire);
    const int32_t remaining = (int32_t)(expiry - now);
    return remaining <= 0 ? 0U : (uint32_t)remaining < CARD_IO_RECOVERY_LIMIT_MS ?
        (uint32_t)remaining : CARD_IO_RECOVERY_LIMIT_MS;
}

/* A recovered footer/CRC is not evidence that the original accepted prefix
 * survived. Compare against the writer's pre-fault RAM witness before any
 * new frame or recovered metadata can authorize continuation. */
static esp_err_t card_io_recovery_prefix_once_locked(void)
{
    if (!s_card_io_recovery.prefix_probe) return ESP_OK;
    sd_audio_segment_t segment;
    esp_err_t err = sd_audio_catalog_read(s_card_session.session_id,
        s_card_io_recovery.prefix_index, &segment);
    if (err != ESP_OK) return err;
    const uint64_t minimum = HEADER_BYTES + FOOTER_BYTES +
        (uint64_t)s_card_io_recovery.prefix_frames * sizeof(uint32_t) +
        s_card_io_recovery.prefix_payload_bytes;
    const uint64_t maximum = minimum + (s_card_io_recovery.retry_probe ?
        sizeof(uint32_t) + s_card_io_recovery.retry_bytes : 0U);
    if (segment.missing || !(segment.sealed & 1U) ||
        segment.total_bytes < minimum || segment.total_bytes > maximum)
        return ESP_ERR_INVALID_CRC;
    FILE *file = fopen(segment.path, "rb");
    if (!file) return errno == ENOMEM ? ESP_ERR_NO_MEM :
        (errno == EMFILE || errno == ENFILE) ? ESP_ERR_NOT_FINISHED : ESP_FAIL;
    uint8_t block[128];
    uint32_t crc = 0U;
    uint64_t payload = 0U;
    err = fseek(file, HEADER_BYTES, SEEK_SET) == 0 ? ESP_OK : ESP_FAIL;
    for (uint32_t i = 0U; i < s_card_io_recovery.prefix_frames && err == ESP_OK; ++i) {
        uint32_t bytes = 0U;
        if (fread(&bytes, 1U, sizeof(bytes), file) != sizeof(bytes)) { err = ESP_FAIL; break; }
        if (bytes < NUWAX_AUDIO_FRAME_HEADER_BYTES || bytes > MAX_FRAME_BYTES) {
            err = ESP_ERR_INVALID_CRC; break;
        }
        payload += bytes;
        while (bytes && err == ESP_OK) {
            const size_t n = bytes < sizeof(block) ? bytes : sizeof(block);
            if (fread(block, 1U, n, file) != n) err = ESP_FAIL;
            else { crc = crc32_update(crc, block, n); bytes -= (uint32_t)n; }
        }
    }
    if (err == ESP_OK && (payload != s_card_io_recovery.prefix_payload_bytes ||
        crc != s_card_io_recovery.prefix_crc)) err = ESP_ERR_INVALID_CRC;
    if (fclose(file) != 0) err = ESP_FAIL;
    return err;
}

static esp_err_t card_io_recovery_verify_prefix_locked(void)
{
    esp_err_t err = card_io_recovery_prefix_once_locked();
    if (err != ESP_ERR_INVALID_CRC) return err;
    const uint32_t now = offline_now_ms();
    if (offline_deadline_elapsed(now, s_card_io_recovery.deadline_ms)) return ESP_ERR_TIMEOUT;
    if (!nuwax_sd_bulk_read_recovery_begin(s_card_io_recovery.deadline_ms - now))
        return ESP_ERR_NOT_FINISHED;
    err = card_io_recovery_prefix_once_locked();
    nuwax_sd_bulk_read_recovery_end(false); /* Prefix proof is not a whole-file certification. */
    return err;
}

/* A failed fwrite/rollback may nevertheless have left the complete frame on
 * the card. Compare the recovered tail before retrying that retained slot.
 * Read in small blocks so recovery cannot consume the supervisor stack. */
static esp_err_t card_io_recovery_probe_retry_locked(void)
{
    s_card_io_recovery.skip_retry = false;
    if (!s_card_io_recovery.retry_probe) return ESP_OK;
    sd_audio_segment_t segment;
    esp_err_t err = sd_audio_catalog_read(s_card_session.session_id,
        s_card_io_recovery.retry_index, &segment);
    if (err != ESP_OK) return err;
    if (segment.missing || !(segment.sealed & 1U)) return ESP_OK;
    FILE *file = fopen(segment.path, "rb");
    if (!file) return ESP_FAIL;
    uint8_t footer[32], frame_header[NUWAX_AUDIO_FRAME_HEADER_BYTES], block[128];
    const uint32_t bytes = s_card_io_recovery.retry_bytes;
    uint32_t length = 0U, sequence = 0U, crc = 0U;
    if (segment.total_bytes < HEADER_BYTES + FOOTER_BYTES + sizeof(length) + bytes ||
        fseek(file, (long)segment.total_bytes - FOOTER_BYTES, SEEK_SET) != 0 ||
        fread(footer, 1U, sizeof(footer), file) != sizeof(footer) ||
        memcmp(footer, "NWXEND1", 7U) ||
        fseek(file, (long)segment.total_bytes - FOOTER_BYTES - sizeof(length) - bytes, SEEK_SET) != 0 ||
        fread(&length, 1U, sizeof(length), file) != sizeof(length) || length != bytes ||
        fread(frame_header, 1U, sizeof(frame_header), file) != sizeof(frame_header)) err = ESP_FAIL;
    if (err == ESP_OK) {
        memcpy(&sequence, frame_header + 10U, sizeof(sequence));
        crc = crc32_update(0U, frame_header, sizeof(frame_header));
        size_t remaining = bytes - sizeof(frame_header);
        while (remaining && err == ESP_OK) {
            const size_t want = remaining < sizeof(block) ? remaining : sizeof(block);
            if (fread(block, 1U, want, file) != want) err = ESP_FAIL;
            else { crc = crc32_update(crc, block, want); remaining -= want; }
        }
        if (err == ESP_OK && sequence == s_card_io_recovery.retry_sequence) {
            if (crc != s_card_io_recovery.retry_crc) err = ESP_ERR_INVALID_CRC;
            else s_card_io_recovery.skip_retry = true;
        }
    }
    if (fclose(file) != 0) err = ESP_FAIL;
    if (err != ESP_OK) s_card_io_recovery.skip_retry = false;
    return err;
}

static bool card_io_recovery_consume_retry_locked(const spool_item_t *item)
{
    if (!s_card_io_recovery.skip_retry || !item ||
        item->generation != s_card_io_recovery.retry_generation ||
        item->bytes != s_card_io_recovery.retry_bytes ||
        crc32_update(0U, item->data, item->bytes) != s_card_io_recovery.retry_crc) return false;
    s_card_io_recovery.skip_retry = s_card_io_recovery.retry_probe = false;
    return true;
}

static bool card_io_recovery_request_locked(esp_err_t error, const spool_item_t *item)
{
    if (!nuwax_sd_session_active() || !atomic_load(&s_card_created) ||
        !s_card_io_recovery.identity_valid || atomic_load(&s_card_failed) ||
        atomic_load(&s_shutdown_requested) || nuwax_power_ctrl_shutdown_preparing()) return false;
    const bool physical = error != ESP_ERR_NO_MEM && error != ESP_ERR_NOT_FINISHED &&
        (error != ESP_ERR_INVALID_STATE || !nuwax_sd_card_is_mounted());
    if (!physical) return false;
    atomic_store_explicit(&s_offline_verify_cancel, true, memory_order_release);
    /* Persisted PCM no longer matches the bytes accepted by the writer.
     * A scratch probe cannot restore those samples. Stop this capture with
     * an honest partial result instead of repeatedly resuming damaged audio. */
    if (sd_audio_stage_is_integrity_fault(s_writer_fault_stage)) {
        atomic_store(&s_card_failed, true);
        atomic_store(&s.capture_complete, false);
        (void)nuwax_offline_audio_request_stop(false);
        ESP_LOGW(TAG, "SD persisted audio mismatch session=%s; recording stopped, original file retained",
            s_card_session.session_id);
        return false;
    }
    const uint32_t requested_at = offline_now_ms();
    const uint32_t recovery_budget = card_io_recovery_budget_ms(requested_at);
    if (!recovery_budget) {
        atomic_store(&s_card_failed, true);
        atomic_store(&s.capture_complete, false);
        (void)nuwax_offline_audio_request_stop(false);
        return false;
    }
    s_card_io_recovery.prefix_probe = s.file && s.frame_count != 0U;
    s_card_io_recovery.prefix_index = s_card_segment.index;
    s_card_io_recovery.prefix_frames = s.frame_count;
    s_card_io_recovery.prefix_crc = s.payload_crc32;
    s_card_io_recovery.prefix_payload_bytes = s.payload_bytes;
    s_card_io_recovery.retry_probe = s.writer_frame_uncertain && !s.writer_frame_written &&
        item && item->bytes >= NUWAX_AUDIO_FRAME_HEADER_BYTES && item->bytes <= MAX_FRAME_BYTES;
    s_card_io_recovery.skip_retry = false;
    if (s_card_io_recovery.retry_probe) {
        s_card_io_recovery.retry_index = s_card_segment.index;
        s_card_io_recovery.retry_bytes = item->bytes;
        s_card_io_recovery.retry_generation = item->generation;
        memcpy(&s_card_io_recovery.retry_sequence, item->data + 10U, sizeof(uint32_t));
        s_card_io_recovery.retry_crc = crc32_update(0U, item->data, item->bytes);
    }
    if (s.file) {
        /* Closing the failed generation consumes the original budget too. */
        if (nuwax_sd_card_io_deadline_begin(esp_timer_get_time() +
                (int64_t)recovery_budget * 1000) != ESP_OK) return false;
        (void)commit_segment_locked(false);
        nuwax_sd_card_io_deadline_end();
    }
    if (!atomic_load(&s.writer_error)) writer_schedule_retry_locked(error);
    if (s.file) return false;
    atomic_store(&s_media_ready, false);
    atomic_store(&s.degraded, true);
    atomic_store(&s.capture_complete, false);
    s_card_io_recovery.started_ms = requested_at;
    s_card_io_recovery.deadline_ms = requested_at + recovery_budget;
    s_card_io_recovery.next_ms = s_card_io_recovery.started_ms;
    s_card_io_recovery.attempts = 0U;
    s_card_io_recovery.capture_generation = s.capture_generation;
    atomic_store_explicit(&s.archive_ready, false, memory_order_release);
#if CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE
    atomic_store(&s_formal_replay_cancel_requested, true);
#endif
    atomic_store_explicit(&s_card_io_recovery.pending, true, memory_order_release);
    ESP_LOGW(TAG, "SD active recovery requested session=%s stage=%s errno=%d; MIC and bounded frames retained",
        s_card_session.session_id, s_writer_fault_stage ? s_writer_fault_stage : "unspecified",
        s_writer_fault_errno);
    return true;
}

static esp_err_t card_io_recovery_abort(esp_err_t error)
{
    atomic_store(&s_card_failed, true);
    atomic_store(&s.capture_complete, false);
    (void)nuwax_offline_audio_request_stop(false);
    atomic_store_explicit(&s_card_io_recovery.pending, false, memory_order_release);
    notify_writer_task();
    ESP_LOGW(TAG, "SD active recovery stopped session=%s err=%s; saved audio retained, no late capture restart",
        s_card_session.session_id, esp_err_to_name(error));
    return error;
}

static void card_io_recovery_observe_reader_fault(void)
{
    if (!atomic_load_explicit(&s_replay_media_fault, memory_order_acquire) ||
        atomic_load(&s_card_io_recovery.pending) || !nuwax_sd_session_active() ||
        nuwax_offline_audio_replay_running()) return;
    if (xSemaphoreTake(s.admission_lock, 0U) != pdTRUE) return;
    if (xSemaphoreTake(s.writer_gate, 0U) != pdTRUE) {
        xSemaphoreGive(s.admission_lock); return;
    }
    if (xSemaphoreTake(s.lock, 0U) == pdTRUE) {
        if (!atomic_load(&s.writer_busy) && !nuwax_offline_audio_replay_running() &&
            atomic_exchange_explicit(&s_replay_media_fault, false, memory_order_acq_rel)) {
            s_writer_fault_stage = "replay_read"; s_writer_fault_errno = 0;
            if (!card_io_recovery_request_locked(ESP_FAIL, NULL))
                atomic_store(&s_replay_media_fault, true);
        }
        xSemaphoreGive(s.lock);
    }
    xSemaphoreGive(s.writer_gate); xSemaphoreGive(s.admission_lock);
}

static esp_err_t card_io_recovery_service(void)
{
    if (!atomic_load_explicit(&s_card_io_recovery.pending, memory_order_acquire)) return ESP_OK;
    const uint32_t now = offline_now_ms();
    if (!nuwax_sd_session_active() || atomic_load(&s_card_failed) ||
        atomic_load(&s_shutdown_requested) || nuwax_power_ctrl_shutdown_preparing())
        return card_io_recovery_abort(ESP_ERR_INVALID_STATE);
    if (offline_deadline_elapsed(now, s_card_io_recovery.deadline_ms))
        return card_io_recovery_abort(ESP_ERR_TIMEOUT);
#if CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE
    atomic_store(&s_formal_replay_cancel_requested, true);
#endif
    if (nuwax_offline_audio_replay_running() || atomic_load(&s_offline_verify_running) ||
        !offline_deadline_elapsed(now, s_card_io_recovery.next_ms)) return ESP_ERR_NOT_FINISHED;
    if (xSemaphoreTake(s.admission_lock, 0U) != pdTRUE) return ESP_ERR_NOT_FINISHED;
    if (xSemaphoreTake(s.writer_gate, 0U) != pdTRUE) {
        xSemaphoreGive(s.admission_lock); return ESP_ERR_NOT_FINISHED;
    }
    if (xSemaphoreTake(s.lock, 0U) != pdTRUE) {
        xSemaphoreGive(s.writer_gate); xSemaphoreGive(s.admission_lock); return ESP_ERR_NOT_FINISHED;
    }
    if (atomic_load(&s.writer_busy) || s.file || nuwax_offline_audio_replay_running()) {
        xSemaphoreGive(s.lock); xSemaphoreGive(s.writer_gate); xSemaphoreGive(s.admission_lock);
        return ESP_ERR_NOT_FINISHED;
    }
    if (s.capture_generation != s_card_io_recovery.capture_generation ||
        strcmp(s.session_id, s_card_session.session_id)) {
        xSemaphoreGive(s.lock); xSemaphoreGive(s.writer_gate); xSemaphoreGive(s.admission_lock);
        return card_io_recovery_abort(ESP_ERR_INVALID_STATE);
    }
    atomic_store(&s.media_recovery_requested, true);
    ++s_card_io_recovery.attempts;
    const uint32_t remaining = s_card_io_recovery.deadline_ms - offline_now_ms();
    esp_err_t err = (int32_t)remaining > 0 ? nuwax_sd_card_io_deadline_begin(
        esp_timer_get_time() + (int64_t)remaining * 1000) : ESP_ERR_TIMEOUT;
    const bool owns_deadline = err == ESP_OK;
    if (err == ESP_OK) err = sd_audio_catalog_prepare_remount();
    const char *stage = "quiesce";
    if (err == ESP_OK) { stage = "mount"; err = nuwax_sd_card_recover(remaining); }
    if (err == ESP_OK) reset_media_recovery_locked(true);
    nuwax_sd_card_identity_t current;
    bool changed_card = false;
    bool changed_binding = false;
    if (err == ESP_OK) stage = "identity";
    if (err == ESP_OK && (!nuwax_sd_card_get_identity(&current) ||
        memcmp(&current, &s_card_io_recovery.identity, sizeof(current)))) {
        changed_card = true; err = ESP_ERR_INVALID_STATE;
    }
    uint8_t owner[32];
    if (err == ESP_OK) stage = "binding";
    if (err == ESP_OK && (!card_owner(owner) || memcmp(owner, s_card_session.owner, sizeof(owner)))) {
        changed_binding = true; err = ESP_ERR_INVALID_STATE;
    }
    if (err == ESP_OK) { stage = "directory"; err = ensure_directory(); }
    if (err == ESP_OK) { stage = "space"; err = prepare_media_space(); }
    /* Repair the live identity BEFORE .part recovery and generic catalog
     * scans can quarantine it. The caller has fenced card, binding and MIC. */
    if (err == ESP_OK) { stage = "scene_checkpoint"; err = sd_audio_catalog_prepare_active(&s_card_session); }
    if (err == ESP_OK) { stage = "parts"; err = recover_open_segments(); }
    if (err == ESP_OK) { stage = "catalog"; err = sd_audio_catalog_recover_active(s_card_boot_id, s_card_session.session_id); }
    sd_audio_session_t restored;
    if (err == ESP_OK) { stage = "scene_reload"; err = sd_audio_catalog_load(s_card_session.session_id, &restored); }
    if (err == ESP_OK && (restored.done || restored.ended || restored.mode != s_card_session.mode ||
        restored.boot_id != s_card_session.boot_id || restored.start_ms != s_card_session.start_ms ||
        memcmp(restored.owner, s_card_session.owner, sizeof(restored.owner)))) err = ESP_ERR_INVALID_STATE;
    /* Recovery can classify an absent file before replay sees it again. Do
     * not resume a live capture after that classification skipped its bytes. */
    bool lost_audio = false;
    bool sealed_loss = false;
    const sd_audio_file_fault_t *fault = &s_card_file_fault[s_card_session.mode ? 1U : 0U];
    if (err == ESP_OK && fault->armed && !strcmp(fault->session_id, s_card_session.session_id)) {
        sd_audio_segment_t checked;
        stage = "faulted_file";
        err = sd_audio_catalog_read(s_card_session.session_id, fault->index, &checked);
        if (err == ESP_OK && checked.missing) {
            lost_audio = true;
            sealed_loss = (checked.sealed & 1U) && checked.total_bytes >= HEADER_BYTES + FOOTER_BYTES &&
                checked.path[0];
            atomic_store(&s_card_partial, true);
        }
    }
    bool prefix_lost = false;
    if (err == ESP_OK) {
        stage = "accepted_prefix";
        err = card_io_recovery_verify_prefix_locked();
        prefix_lost = err == ESP_ERR_INVALID_CRC || err == ESP_ERR_NOT_FOUND;
    }
    if (err == ESP_OK) err = card_io_recovery_probe_retry_locked();
    if (err == ESP_OK) { stage = "filename_scan"; err = refresh_next_segment_index(); }
    if (err == ESP_OK) {
        s_card_session = restored;
        if (!restored.complete) atomic_store(&s_card_partial, true);
        s.used_bytes = scan_used_bytes();
        atomic_store(&s.ready_count, scan_ready_count());
        err = refresh_capture_capacity_locked();
    }
    const uint32_t elapsed = offline_now_ms() - s_card_io_recovery.started_ms;
    const bool timed_out = offline_deadline_elapsed(offline_now_ms(), s_card_io_recovery.deadline_ms);
    const bool cancelled = !nuwax_sd_session_active() || atomic_load(&s_card_failed) ||
        atomic_load(&s_shutdown_requested) || nuwax_power_ctrl_shutdown_preparing();
    if (err == ESP_OK && (!card_owner(owner) || memcmp(owner, s_card_session.owner, sizeof(owner)))) {
        changed_binding = true; err = ESP_ERR_INVALID_STATE;
    }
    if (timed_out || cancelled || changed_card || changed_binding || prefix_lost || (lost_audio && !sealed_loss)) {
        atomic_store(&s_card_failed, true);
        atomic_store(&s.capture_complete, false);
        (void)nuwax_offline_audio_request_stop(false);
    }
    if (err == ESP_OK) {
        s.open_path[0] = s.ready_path[0] = 0;
        s.frame_count = 0U; s.payload_bytes = 0U; s.payload_crc32 = 0U;
        s.last_good_offset = 0;
        atomic_store_explicit(&s_replay_media_fault, false, memory_order_release);
        s.writer_retry_deadline_ms = s.writer_retry_backoff_ms = 0U;
        s.writer_fault_latched = false;
        atomic_store(&s.writer_error, false);
        atomic_store(&s.archive_ready, false);
        atomic_store(&s_media_ready, true);
        atomic_store(&s_card_pending_upload, true);
        atomic_store(&s_directory_dirty, false);
        s_writer_fault_stage = NULL; s_writer_fault_errno = 0;
    } else {
        atomic_store(&s_media_ready, false);
        nuwax_sd_card_mark_io_fault(err);
        const unsigned shift = s_card_io_recovery.attempts > 4U ? 3U : s_card_io_recovery.attempts - 1U;
        s_card_io_recovery.next_ms = offline_now_ms() + (250U << shift);
        ESP_LOGW(TAG, "SD active recovery deferred stage=%s session=%s err=%s attempt=%u",
            stage, s_card_session.session_id, esp_err_to_name(err), s_card_io_recovery.attempts);
    }
    atomic_store(&s.media_recovery_requested, false);
    if (owns_deadline) nuwax_sd_card_io_deadline_end();
    if (err == ESP_OK) atomic_store_explicit(&s_card_io_recovery.pending, false, memory_order_release);
    xSemaphoreGive(s.lock); xSemaphoreGive(s.writer_gate); xSemaphoreGive(s.admission_lock);
    if (prefix_lost || (lost_audio && !sealed_loss)) return card_io_recovery_abort(ESP_ERR_INVALID_CRC);
    if (changed_card || changed_binding || cancelled || timed_out)
        return card_io_recovery_abort(timed_out ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE);
    if (err != ESP_OK) return ESP_ERR_NOT_FINISHED;
    notify_writer_task();
    ESP_LOGW(TAG, "SD active recovery resumed session=%s attempts=%u elapsed_ms=%lu; same MIC/scene, earlier storage fault remains partial",
        s_card_session.session_id, s_card_io_recovery.attempts, (unsigned long)elapsed);
    return ESP_OK;
}
#else
static bool card_io_recovery_consume_retry_locked(const spool_item_t *item)
{ (void)item; return false; }
#endif
