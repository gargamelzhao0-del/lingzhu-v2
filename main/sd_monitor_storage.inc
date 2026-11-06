/* The supervisor reclaims ahead of capture. This code never takes the MIC,
 * sends network traffic or waits for an upload. All filesystem work uses the
 * spool lock; the capture task continues feeding its bounded PCM queue. */
#if CONFIG_NUWAX_SD_SESSION_ENABLE
static esp_err_t card_monitor_make_room_locked(bool ahead, uint32_t budget_ms)
{
    const int64_t deadline = esp_timer_get_time() + (int64_t)budget_ms * 1000;
    for (unsigned step = 0; step < 64U; ++step) {
        uint64_t total = 0, free_bytes = 0;
        esp_err_t err = esp_vfs_fat_info("/sdcard", &total, &free_bytes);
        if (err != ESP_OK) return err;
        atomic_store(&s_space_blocked,
            free_bytes < NUWAX_OFFLINE_AUDIO_FREE_RESERVE_BYTES + CONFIG_NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES ||
            nuwax_offline_quota_blocked(true, s.used_bytes, ARCHIVE_QUOTA_BYTES,
                CONFIG_NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES));
        const uint64_t minimum = sd_monitor_required_bytes(total, CONFIG_NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES, false);
        const uint64_t target = sd_monitor_required_bytes(total, CONFIG_NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES, ahead);
        /* Do not latch a stop while bounded metadata maintenance is in progress.
         * Admission below and the writer remain authoritative at the low mark. */
        if (free_bytes >= minimum) atomic_store(&s_monitor_space_blocked, false);
        if (free_bytes >= target) return ESP_OK;
        uint64_t removed = 0; char path[128];
        err = sd_audio_catalog_reclaim_confirmed(s_card_session.owner, &removed, path);
        if (removed) {
            s.used_bytes = s.used_bytes >= removed ? s.used_bytes - removed : 0;
            atomic_store(&s_directory_dirty, true);
            archive_remove_empty_bucket(path);
            ESP_LOGW(TAG, "SD monitor retention reclaimed path=%s bytes=%llu free_before=%llu reserve=%llu",
                path, (unsigned long long)removed, (unsigned long long)free_bytes,
                (unsigned long long)sd_monitor_reserve_bytes(total));
        }
        if (err == ESP_ERR_NOT_FOUND) {
            /* The bounded ACK cleanup may have freed files during its scan.
             * Do not decide from the capacity sampled before that cleanup. */
            err = esp_vfs_fat_info("/sdcard", &total, &free_bytes);
            if (err != ESP_OK) return err;
            atomic_store(&s_space_blocked,
                free_bytes < NUWAX_OFFLINE_AUDIO_FREE_RESERVE_BYTES + CONFIG_NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES ||
                nuwax_offline_quota_blocked(true, s.used_bytes, ARCHIVE_QUOTA_BYTES,
                    CONFIG_NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES));
            if (free_bytes >= minimum) {
                atomic_store(&s_monitor_space_blocked, false);
                return ESP_OK;
            }
            atomic_store(&s_monitor_space_blocked, true);
            return ESP_ERR_NO_MEM; /* All remaining data is protected. */
        }
        if (err != ESP_OK && err != ESP_ERR_NOT_FINISHED) return err;
        if (esp_timer_get_time() >= deadline) return ESP_ERR_NOT_FINISHED;
    }
    return ESP_ERR_NOT_FINISHED;
}

static esp_err_t card_monitor_prepare_space(void)
{
    if (!nuwax_sd_session_monitoring()) return ESP_OK;
    const int64_t local_deadline = esp_timer_get_time() + 5000000;
    const int64_t deadline = s_card_start_deadline_us && s_card_start_deadline_us < local_deadline
        ? s_card_start_deadline_us : local_deadline;
    esp_err_t err = ESP_ERR_NOT_FINISHED;
    while (esp_timer_get_time() < deadline && !atomic_load(&s.capture_stop) &&
           !atomic_load(&s_shutdown_requested)) {
        if (xSemaphoreTake(s.lock, pdMS_TO_TICKS(100U)) == pdTRUE) {
            err = card_monitor_make_room_locked(false, 50U);
            xSemaphoreGive(s.lock);
            if (err != ESP_ERR_NOT_FINISHED) return err;
        }
        vTaskDelay(pdMS_TO_TICKS(10U));
    }
    return ESP_ERR_TIMEOUT;
}

static void card_monitor_retention_service(void)
{
    if (!nuwax_sd_session_monitoring() || !atomic_load(&s_card_created) ||
        atomic_load(&s_card_finishing) || !atomic_load(&s_media_ready) ||
        atomic_load(&s.media_recovery_requested) || atomic_load(&s_shutdown_requested)) return;
    if (xSemaphoreTake(s.lock, 0) != pdTRUE) return;
    /* Owner may have changed while this task waited to run. */
    esp_err_t err = ESP_OK;
    if (nuwax_sd_session_monitoring() && atomic_load(&s_card_created) && !atomic_load(&s_card_finishing))
        err = card_monitor_make_room_locked(true, 50U);
    xSemaphoreGive(s.lock);
    if (err != ESP_OK && err != ESP_ERR_NOT_FINISHED) {
        static uint32_t last_log;
        const uint32_t now = offline_now_ms();
        if (!last_log || (uint32_t)(now - last_log) >= 10000U) {
            last_log = now;
            ESP_LOGW(TAG, "SD monitor retention unable to reclaim err=%s; protected files retained", esp_err_to_name(err));
        }
    }
}
#endif
