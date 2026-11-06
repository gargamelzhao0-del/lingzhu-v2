#if CONFIG_NUWAX_SD_SESSION_ENABLE && defined(ESP_PLATFORM)
/* One offline reader, one descriptor in flight. Persistent optional cursors
 * recover pending work without an unbounded RAM queue or one task per file. */
static uint32_t s_verify_seen_revision, s_verify_seen_media, s_verify_retry_ms;
static uint8_t s_verify_seen_owner[32];

static bool card_verify_cancelled(void)
{
    return atomic_load_explicit(&s_offline_verify_cancel, memory_order_acquire) ||
        atomic_load(&s_card_online) || atomic_load(&s_shutdown_requested) ||
        replay_ota_active() ||
        atomic_load(&s_card_finishing) || atomic_load(&s_card_io_recovery.pending) ||
        sd_capture_admission_held(&s_capture_admission, offline_now_ms()) ||
        nuwax_power_ctrl_shutdown_preparing() || !atomic_load(&s_media_ready);
}

static esp_err_t card_verify_storage_enter(void)
{
    while (!card_verify_cancelled()) {
        if (!atomic_load(&s.writer_busy) && uxQueueMessagesWaiting(s.queue) <= 2U &&
            xSemaphoreTake(s.lock, 0U) == pdTRUE) {
            if (!card_verify_cancelled() && !atomic_load(&s.writer_busy) &&
                uxQueueMessagesWaiting(s.queue) <= 2U) return ESP_OK;
            xSemaphoreGive(s.lock);
        }
        vTaskDelay(1U);
    }
    return ESP_ERR_NOT_FINISHED;
}

static esp_err_t card_verify_open(const sd_audio_session_t *scene, const sd_audio_segment_t *part,
    FILE **out, archive_header_t *header, archive_footer_t *footer)
{
    *out = fopen(part->path, "rb");
    if (!*out) return errno == ENOMEM ? ESP_ERR_NO_MEM :
        (errno == EMFILE || errno == ENFILE) ? ESP_ERR_NOT_FINISHED : ESP_FAIL;
    FILE *file = *out;
    struct stat st;
    if (setvbuf(file, NULL, _IONBF, 0U) != 0 || fstat(fileno(file), &st) != 0) return ESP_FAIL;
    if (st.st_size != (off_t)part->total_bytes || st.st_size < (off_t)(HEADER_BYTES + FOOTER_BYTES))
        return ESP_ERR_INVALID_SIZE;
    if (fread(header, 1U, sizeof(*header), file) != sizeof(*header) ||
        fseek(file, -(long)FOOTER_BYTES, SEEK_END) != 0 ||
        fread(footer, 1U, sizeof(*footer), file) != sizeof(*footer)) return ESP_FAIL;
    if (memcmp(header->magic, "NWXOFF1", 7U) || header->version != 1U ||
        header->header_bytes != HEADER_BYTES || header->mode != scene->mode ||
        header->codec != NUWAX_AUDIO_CODEC_PCM16 || header->channels != 1U ||
        header->sample_rate != 16000U || header->frame_duration_ms != 20U ||
        header->segment_index != part->index ||
        memcmp(header->session_id, scene->session_id, sizeof(header->session_id)) ||
        memcmp(footer->magic, "NWXEND1", 7U) || footer->version != 1U ||
        footer->footer_bytes != FOOTER_BYTES || footer->crc32 != part->payload_crc)
        return ESP_ERR_INVALID_CRC;
    return replay_read_rewind(file);
}

static void card_offline_verify_task(void *unused)
{
    (void)unused;
    uint8_t *buffer = heap_caps_malloc(4096U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const uint32_t revision = sd_audio_catalog_live_revision();
    const uint32_t media = atomic_load_explicit(&s_media_generation, memory_order_acquire);
    uint32_t verified = 0U;
    uint8_t exhausted_owner[32] = {0};
    esp_err_t result = buffer ? ESP_OK : ESP_ERR_NO_MEM;
    bool exhausted = false;
    while (result == ESP_OK && !card_verify_cancelled() &&
        atomic_load_explicit(&s_media_generation, memory_order_acquire) == media) {
        sd_audio_session_t scene;
        sd_audio_segment_t part;
        sd_audio_lease_token_t lease = 0U;
        if (card_verify_storage_enter() != ESP_OK) break;
        uint8_t owner[32];
        result = card_owner(owner) ? sd_audio_catalog_verify_next(owner,
            s_offline_verify_nonce, media, &scene, &part) : ESP_ERR_INVALID_STATE;
        if (result == ESP_OK)
            result = sd_audio_catalog_lease_acquire(&scene, &part, SD_AUDIO_LEASE_VERIFY,
                media, s.capture_generation, &lease);
        xSemaphoreGive(s.lock);
        if (result == ESP_ERR_NOT_FOUND) {
            memcpy(exhausted_owner, owner, sizeof(exhausted_owner));
            exhausted = true; break;
        }
        if (result == ESP_ERR_NOT_FINISHED && !lease) {
            result = ESP_OK; vTaskDelay(1U); continue;
        }
        if (result != ESP_OK) break;
        FILE *file = NULL;
        archive_header_t header;
        archive_footer_t footer;
        result = card_verify_storage_enter();
        if (result == ESP_OK) {
            result = card_verify_open(&scene, &part, &file, &header, &footer);
            xSemaphoreGive(s.lock);
        }
        replay_stream_t stream = {0};
        replay_read_stats_t stats = {0};
        uint64_t read_us = 0U, validation_us = 0U, work_us = 0U;
        while (result == ESP_OK && stream.position < part.total_bytes) {
            result = card_verify_storage_enter();
            if (result != ESP_OK) break;
            if (atomic_load_explicit(&s_media_generation, memory_order_acquire) != media) {
                xSemaphoreGive(s.lock); result = ESP_ERR_NOT_FINISHED; break;
            }
            size_t bytes = part.total_bytes - stream.position;
            if (bytes > 4096U) bytes = 4096U;
            result = replay_read_validate(file, buffer, &bytes, &stream, part.total_bytes,
                &header, &footer, &read_us, &validation_us, &work_us, &stats);
            xSemaphoreGive(s.lock);
            vTaskDelay(1U);
        }
        /* The worker closes its own FILE before metadata/rename/publication.
         * Cancellation never releases the lease underneath a native read. */
        while (xSemaphoreTake(s.lock, pdMS_TO_TICKS(100U)) != pdTRUE) vTaskDelay(1U);
        if (file && fclose(file) != 0) result = ESP_FAIL;
        const bool current = !card_verify_cancelled() &&
            sd_audio_catalog_lease_matches(lease, SD_AUDIO_LEASE_VERIFY,
                atomic_load_explicit(&s_media_generation, memory_order_acquire)) &&
            card_owner(owner) && !memcmp(owner, scene.owner, 32U);
        if (current && result == ESP_OK) {
            sd_audio_session_t latest;
            sd_audio_segment_t actual;
            result = sd_audio_catalog_load(scene.session_id, &latest);
            if (result == ESP_OK) result = sd_audio_catalog_read(scene.session_id, part.index, &actual);
            if (result == ESP_OK && (actual.acknowledged || actual.missing || !(actual.sealed & 1U) ||
                actual.total_bytes != part.total_bytes || actual.payload_crc != part.payload_crc ||
                strcmp(actual.path, part.path) || memcmp(latest.owner, scene.owner, 32U) ||
                latest.boot_id != scene.boot_id || latest.start_ms != scene.start_ms))
                result = ESP_ERR_INVALID_STATE;
            if (result == ESP_OK) {
                result = sd_audio_catalog_verify_checkpoint(&latest,
                    s_offline_verify_nonce, media, part.index + 1U, lease);
                if (result == ESP_OK) {
                    ++verified;
                    if (nuwax_sd_session_active() && !strcmp(scene.session_id, s_card_session.session_id))
                        sd_audio_damage_healthy(&s_card_damage, part.index);
                }
            }
        } else if (current && (result == ESP_ERR_INVALID_CRC || result == ESP_ERR_INVALID_SIZE)) {
            result = card_confirm_missing_locked(&scene, &part, true);
        } else if (current && result == ESP_FAIL) {
            struct stat st;
            if (stat(part.path, &st) != 0 && errno == ENOENT)
                result = card_confirm_missing_locked(&scene, &part, false);
            else {
                nuwax_sd_card_mark_io_fault(result);
                atomic_store_explicit(&s_replay_media_fault, true, memory_order_release);
            }
        }
        (void)sd_audio_catalog_lease_release(lease, SD_AUDIO_LEASE_VERIFY);
        xSemaphoreGive(s.lock);
    }
    heap_caps_free(buffer);
    while (xSemaphoreTake(s.lock, pdMS_TO_TICKS(100U)) != pdTRUE) vTaskDelay(1U);
    sd_audio_catalog_verify_reset();
    xSemaphoreGive(s.lock);
    if (exhausted) {
        s_verify_seen_revision = revision; s_verify_seen_media = media;
        memcpy(s_verify_seen_owner, exhausted_owner, sizeof(s_verify_seen_owner));
    }
    s_verify_retry_ms = offline_now_ms() + (exhausted ? 1000U : 5000U);
    if (verified || (!exhausted && result != ESP_OK && result != ESP_ERR_NOT_FINISHED))
        ESP_LOGI(TAG, "SD offline verification files=%lu exhausted=%u err=%s media=%lu",
            (unsigned long)verified, exhausted, esp_err_to_name(result), (unsigned long)media);
    atomic_store_explicit(&s_offline_verify_running, false, memory_order_release);
    nuwax_offline_audio_supervisor_wake();
    vTaskDeleteWithCaps(NULL);
}

static void card_offline_verify_service(void)
{
    /* Eligibility and publication share the recording/shutdown admission gate.
     * Otherwise a creator paused before running=true could resume after another
     * owner has already observed a quiescent store. Never join under this gate. */
    if (!s.admission_lock || xSemaphoreTake(s.admission_lock, 0U) != pdTRUE) return;
    if (atomic_load_explicit(&s_offline_verify_running, memory_order_acquire)) goto done;
    if (atomic_load(&s_card_online) || atomic_load(&s_shutdown_requested) || replay_ota_active() ||
        atomic_load(&s_card_finishing) || atomic_load(&s_card_io_recovery.pending) ||
        sd_capture_admission_held(&s_capture_admission, offline_now_ms()) ||
        nuwax_offline_audio_replay_running() || !atomic_load(&s_media_ready) ||
        nuwax_power_ctrl_shutdown_preparing() ||
        !offline_deadline_elapsed(offline_now_ms(), s_verify_retry_ms)) goto done;
    /* A completed inventory is scoped to the binding it actually scanned.
     * Rebinding alone need not write metadata or remount the physical card. */
    uint8_t owner[32];
    if (!card_owner(owner)) goto done;
    if (sd_audio_catalog_live_revision() == s_verify_seen_revision &&
        atomic_load(&s_media_generation) == s_verify_seen_media &&
        !memcmp(owner, s_verify_seen_owner, sizeof(owner))) goto done;
    if (!s_offline_verify_nonce) s_offline_verify_nonce =
        ((uint64_t)esp_random() << 32U) | esp_random() | UINT64_C(1);
    atomic_store(&s_offline_verify_cancel, false);
    atomic_store_explicit(&s_offline_verify_running, true, memory_order_release);
    if (xTaskCreateWithCaps(card_offline_verify_task, "sd_offline_verify", 8192U,
        NULL, 1U, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        atomic_store_explicit(&s_offline_verify_running, false, memory_order_release);
        s_verify_retry_ms = offline_now_ms() + 5000U;
    }
done:
    xSemaphoreGive(s.admission_lock);
}
#elif CONFIG_NUWAX_SD_SESSION_ENABLE
static void card_offline_verify_service(void) {}
#endif
