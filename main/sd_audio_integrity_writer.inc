#if CONFIG_NUWAX_SD_INTEGRITY_DIAGNOSTICS
static bool writer_integrity_live(void)
{
    return card_recording_locked() && s.integrity && s.integrity->active && !s.integrity->frozen;
}

static void writer_integrity_fault(uint32_t stage, uint32_t expected, uint32_t observed)
{
    /* A confirmed integrity failure must control the safety decision even
     * after an earlier, recoverable I/O error latched its diagnostic label.
     * This classification also works if optional snapshot allocation failed. */
    switch (stage) {
    case SD_INTEGRITY_QUEUE: s_writer_fault_stage = "frame_queue_crc"; break;
    case SD_INTEGRITY_WRITE: s_writer_fault_stage = "frame_write_crc"; break;
    case SD_INTEGRITY_RECORD: s_writer_fault_stage = "record_readback_crc"; break;
    case SD_INTEGRITY_SEAL: s_writer_fault_stage = "seal_readback_crc"; break;
    case SD_INTEGRITY_SEAL_SIZE: s_writer_fault_stage = "seal_readback_size"; break;
    default: return;
    }
    s_writer_fault_errno = 0;
    if (!writer_integrity_live()) return;
    sd_integrity_t *diag = s.integrity;
    nuwax_sd_integrity_io_freeze(&diag->io);
#if CONFIG_NUWAX_SD_COMMAND_HISTORY_ENABLE
    nuwax_sd_command_history_dump(diag->io.bad_lba, diag->io.mismatch_seen);
#endif
    sd_integrity_freeze(diag, stage, offline_now_ms(), expected, observed);
    ESP_LOGE(TAG, "SD integrity fault revision=213 stage=%lu session=%s index=%lu offset=%llu bytes=%lu queue_valid=%u queue_crc=%08lx before_crc=%08lx after_crc=%08lx expected_crc=%08lx observed_crc=%08lx shadow_crc=%08lx snapshot_crc=%08lx covered=%u",
        (unsigned long)stage, diag->session_id, (unsigned long)diag->index,
        (unsigned long long)diag->offset, (unsigned long)diag->shadow_bytes, diag->queue_valid,
        (unsigned long)diag->queue_crc, (unsigned long)diag->before_crc, (unsigned long)diag->after_crc,
        (unsigned long)expected, (unsigned long)observed, (unsigned long)diag->shadow_crc,
        (unsigned long)diag->snapshot_crc, diag->shadow_covered);
    ESP_LOGE(TAG, "SD integrity difference first_relative=%lu first_file_offset=%llu differing_bytes=%lu observed_bytes=%lu first_sequence=%lu last_sequence=%lu records=%lu; raw audio retained in RAM, no payload log",
        (unsigned long)diag->first_difference,
        (unsigned long long)(diag->first_difference == UINT32_MAX ? UINT64_MAX : diag->offset + diag->first_difference),
        (unsigned long)diag->differing_bytes, (unsigned long)diag->observed_bytes,
        (unsigned long)diag->first_sequence, (unsigned long)diag->last_sequence, (unsigned long)diag->records);
    const nuwax_sd_integrity_io_report_t *io = &diag->io;
    ESP_LOGE(TAG, "SD integrity IO writes=%lu reads=%lu sdk_errors=%lu foreign_writes=%lu covered=%lu unknown=%lu matched=%lu mismatched=%lu evicted=%lu source_mutations=%lu lba=%llu write_crc=%08lx read_crc=%08lx raw_attempts=%lu raw_valid=%u raw_crc=%08lx raw_err=%s",
        (unsigned long)io->write_calls, (unsigned long)io->read_calls, (unsigned long)io->sdk_errors,
        (unsigned long)io->foreign_writes, (unsigned long)io->covered_sectors,
        (unsigned long)io->unknown_sectors, (unsigned long)io->matched_sectors,
        (unsigned long)io->mismatched_sectors, (unsigned long)io->evictions,
        (unsigned long)io->source_mutations, (unsigned long long)io->bad_lba,
        (unsigned long)io->expected_crc, (unsigned long)io->observed_crc,
        (unsigned long)io->raw_attempts, io->raw_valid, (unsigned long)io->raw_crc, esp_err_to_name(io->raw_error));
}

static bool writer_integrity_before(const spool_item_t *item, uint32_t *before)
{
    *before = crc32_update(0U, item->data, item->bytes);
    if (writer_integrity_live()) sd_integrity_record(s.integrity, (uint32_t)item->bytes,
        item->data, item->queued_crc, item->queued_crc_valid, *before);
    if (item->queued_crc_valid && item->queued_crc != *before) {
        writer_note_fault("frame_queue_crc", 0);
        writer_integrity_fault(SD_INTEGRITY_QUEUE, item->queued_crc, *before);
        return false;
    }
    return true;
}

static bool writer_integrity_after(const spool_item_t *item, uint32_t before)
{
    const uint32_t after = crc32_update(0U, item->data, item->bytes);
    if (writer_integrity_live()) s.integrity->after_crc = after;
    if (before != after) {
        if (writer_integrity_live() && s.integrity->shadow_covered && s.integrity->shadow_bytes >= item->bytes) {
            const uint32_t at = s.integrity->shadow_bytes - (uint32_t)item->bytes;
            for (size_t n = 0U; n < item->bytes; n += 512U) {
                const size_t count = item->bytes - n < 512U ? item->bytes - n : 512U;
                sd_integrity_compare(s.integrity, item->data + n, count, at + (uint32_t)n);
            }
        }
        writer_note_fault("frame_write_crc", 0);
        writer_integrity_fault(SD_INTEGRITY_WRITE, before, after);
        return false;
    }
    return true;
}

static esp_err_t writer_integrity_check_seal(void)
{
    if (!writer_integrity_live() || s.integrity->head_bytes != 512U || s.integrity->tail_bytes != 512U)
        return ESP_OK;
    sd_integrity_t *diag = s.integrity;
    uint8_t block[512];
    esp_err_t err = ESP_OK;
    uint32_t expected_crc = 0U, observed_crc = 0U;
    nuwax_sd_integrity_io_read_phase(true);
    for (unsigned attempt = 0U; attempt < 2U; ++attempt) {
        const bool independent = attempt != 0U;
        if (independent && !nuwax_sd_bulk_read_recovery_begin(1500U)) break;
        expected_crc = observed_crc = 0U;
        sd_integrity_read_attempt(diag);
        FILE *file = fopen(s.open_path, "rb");
        if (!file) {
            if (independent) nuwax_sd_bulk_read_recovery_end(false);
            err = ESP_FAIL; break;
        }
        struct stat st;
        err = ESP_OK;
        if (setvbuf(file, s.write_buffer + ARCHIVE_STDIO_BUFFER_BYTES, _IOFBF, ARCHIVE_STDIO_BUFFER_BYTES) != 0 ||
            fstat(fileno(file), &st) != 0 || st.st_size < 0) err = ESP_FAIL;
        else {
            diag->observed_file_size = (uint64_t)st.st_size;
            if ((uint64_t)st.st_size != diag->body_end + FOOTER_BYTES) err = ESP_ERR_INVALID_SIZE;
        }
        if (err == ESP_OK) for (unsigned edge = 0U; edge < 2U; ++edge) {
            const uint64_t at = edge ? diag->body_end + FOOTER_BYTES - sizeof(block) : 0U;
            const uint8_t *expected = edge ? diag->tail : diag->head;
            if (at > INT32_MAX || fseek(file, (long)at, SEEK_SET) != 0 ||
                fread(block, 1U, sizeof(block), file) != sizeof(block)) { err = ESP_FAIL; break; }
            if (memcmp(expected, block, sizeof(block))) {
                sd_integrity_seal_difference(diag, at, expected, block, sizeof(block));
                expected_crc = crc32_update(0U, expected, sizeof(block));
                observed_crc = crc32_update(0U, block, sizeof(block));
                err = ESP_ERR_INVALID_CRC; break;
            }
        }
        if (ferror(file)) err = ESP_FAIL;
        if (fclose(file) != 0) err = ESP_FAIL;
        if (independent) {
            nuwax_sd_bulk_read_recovery_end(err == ESP_OK);
            if (err == ESP_OK) ESP_LOGW(TAG,
                "SD independent read recovered revision=217 stage=seal index=%lu; original edges matched, single-sector reads retained",
                (unsigned long)s.segment_index);
        }
        if (err != ESP_ERR_INVALID_CRC && err != ESP_ERR_INVALID_SIZE) break;
    }
    nuwax_sd_integrity_io_read_phase(false);
    if (err == ESP_ERR_INVALID_CRC || err == ESP_ERR_INVALID_SIZE) {
        if (err == ESP_ERR_INVALID_SIZE) {
            diag->shadow_bytes = diag->shadow_crc = 0U;
            diag->offset = 0U;
        }
        writer_note_fault(err == ESP_ERR_INVALID_CRC ? "seal_readback_crc" : "seal_readback_size", 0);
        writer_integrity_fault(err == ESP_ERR_INVALID_CRC ? SD_INTEGRITY_SEAL : SD_INTEGRITY_SEAL_SIZE,
            expected_crc, observed_crc);
    } else if (err != ESP_OK) writer_note_fault("seal_readback_io", errno);
    else { nuwax_sd_integrity_io_freeze(NULL); diag->active = false; }
    return err;
}

static bool writer_integrity_export_yield(void *ctx)
{
    const int64_t started = *(const int64_t *)ctx;
    return esp_timer_get_time() - started >= 750000LL ||
        sd_capture_admission_held(&s_capture_admission, offline_now_ms()) ||
        nuwax_offline_audio_storage_busy();
}

static bool writer_integrity_export_media_fault(const sd_integrity_t *diag, esp_err_t error)
{
    if (error == ESP_OK || error == ESP_ERR_NO_MEM || error == ESP_ERR_NOT_FINISHED) return false;
    /* Namespace/permission/space failures affect this optional artifact, not
     * all audio storage. An uncertain close still requires ordered recovery. */
    if (diag->save_stage && !strcmp(diag->save_stage, "close")) return true;
    const int reason = diag->save_errno;
    return reason == EIO || reason == ENODEV || reason == ETIMEDOUT || reason == EILSEQ ||
        (!reason && diag->save_stage && (!strcmp(diag->save_stage, "write") || !strcmp(diag->save_stage, "sync")));
}

static void writer_integrity_save_if_idle(void)
{
    sd_integrity_t *diag = s.integrity;
    if (!diag || !diag->frozen || diag->saved || diag->save_attempts >= 4U || s.file ||
        atomic_load(&s.active) || atomic_load(&s.capture_running) || atomic_load(&s.writer_busy) || nuwax_sd_session_active() ||
        nuwax_offline_audio_replay_running() || !nuwax_sd_card_is_mounted() ||
        !atomic_load(&s_media_ready) || nuwax_offline_audio_storage_busy() ||
        sd_capture_admission_held(&s_capture_admission, offline_now_ms())) return;
    const int64_t started = esp_timer_get_time();
    ++diag->save_attempts;
    char path[192] = {0};
    const esp_err_t err = sd_integrity_save_cooperative(diag, "/sdcard/sd_integrity", path, sizeof(path),
        writer_integrity_export_yield, (void *)&started);
    ESP_LOGW(TAG, "SD integrity evidence revision=213 saved=%u path=%s bytes=%lu attempt=%lu stage=%s errno=%d elapsed_ms=%lu yielded=%u err=%s; no cloud upload",
        diag->saved, path[0] ? path : "-", (unsigned long)(512U + diag->shadow_bytes + diag->observed_bytes),
        (unsigned long)diag->save_attempts, diag->save_stage ? diag->save_stage : "-", diag->save_errno,
        (unsigned long)((esp_timer_get_time()-started)/1000), err == ESP_ERR_NOT_FINISHED, esp_err_to_name(err));
    if (writer_integrity_export_media_fault(diag, err)) {
        atomic_store(&s_media_ready, false);
        nuwax_sd_card_mark_io_fault(err);
    }
}

/* Only the background storage supervisor calls this service, after recovery.
 * It never participates in a controller's start/recovery return value. */
static void writer_integrity_service(void)
{
    if (!s.integrity || !s.integrity->frozen || s.integrity->saved || s.integrity->save_attempts >= 4U) return;
    if (xSemaphoreTake(s.admission_lock, 0U) != pdTRUE) return;
    if (xSemaphoreTake(s.writer_gate, 0U) != pdTRUE) { xSemaphoreGive(s.admission_lock); return; }
    if (xSemaphoreTake(s.lock, 0U) == pdTRUE) {
        writer_integrity_save_if_idle();
        xSemaphoreGive(s.lock);
    }
    xSemaphoreGive(s.writer_gate); xSemaphoreGive(s.admission_lock);
}
#endif
