static void reset_offline_capture_retry_state(void)
{
    s_offline_capture_retry_next_ms = 0U;
    s_offline_capture_retry_backoff_ms = 0U;
    s_offline_capture_retry_attempts = 0U;
}

static nuwax_audio_mode_identity_t *offline_mode_identity(void)
{
    return offline_mode_kind() == NUWAX_OFFLINE_AUDIO_MODE_MONITORING
        ? &s_monitoring_mode.identity : &s_meeting_mode.identity;
}

static bool service_offline_duration_limit(void)
{
    if (!offline_mode_lease_pending() ||
        !nuwax_offline_audio_limit_reached()) {
        return false;
    }
    const bool monitoring =
        offline_mode_kind() == NUWAX_OFFLINE_AUDIO_MODE_MONITORING;
    offline_mode_lease_store(false);
    s_offline_mode_resume_session[0] = '\0';
    nuwax_audio_mode_identity_clear(offline_mode_identity());
    s_offline_mode_capture_started = false;
    reset_offline_capture_retry_state();
    atomic_store_explicit(&s_offline_mode_cancel_to_sleep, true, memory_order_release);
    ESP_LOGI(TAG,
             "offline %s limit reached; automatic resume cancelled",
             monitoring ? "monitoring" : "meeting");
    return true;
}

static void service_offline_capture_retry_disconnected(void)
{
    if (service_offline_duration_limit()) {
        return;
    }
    if (!offline_mode_lease_pending() ||
        nuwax_offline_audio_capture_running() ||
        s_offline_mode_resume_session[0] == '\0' ||
        atomic_load_explicit(&s_reprovision_quiesce_requested,
                             memory_order_acquire)) {
        return;
    }
#if CONFIG_NUWAX_OTA_ENABLE
    if (nuwax_ota_device_is_active()) {
        return;
    }
#endif

    const uint32_t now = monotonic_ms();
    if (s_offline_capture_retry_next_ms != 0U &&
        !elapsed_ms(now, s_offline_capture_retry_next_ms)) {
        return;
    }

    /* A previous reader may have stopped after consuming frames (for example
     * after card failure). Carry its cursor into the retry as well. */
    if (s_offline_mode_capture_started) {
        offline_mode_identity()->next_capture_sequence =
            nuwax_offline_audio_next_capture_sequence();
        s_offline_mode_capture_started = false;
    }
    esp_err_t err = nuwax_offline_audio_init();
    if (err == ESP_OK) {
        err = nuwax_offline_audio_start_capture_continuation(
            offline_mode_kind(),
            s_offline_mode_resume_session,
            CONFIG_NUWAX_VOICE_SAMPLE_RATE,
            CONFIG_NUWAX_VOICE_CHANNELS,
            CONFIG_NUWAX_VOICE_FRAME_DURATION_MS,
            offline_mode_identity()->next_capture_sequence);
    }
    if (err == ESP_OK) {
        const uint32_t attempt = s_offline_capture_retry_attempts;
        s_offline_mode_capture_started = true;
        atomic_store_explicit(&s_offline_mode_capture_failed, false,
                              memory_order_release);
        reset_offline_capture_retry_state();
        ESP_LOGI(TAG,
                 "offline capture retry succeeded mode=%s attempt=%lu session=%s",
                 offline_mode_kind() ==
                         NUWAX_OFFLINE_AUDIO_MODE_MONITORING
                     ? "monitoring"
                     : "meeting",
                 (unsigned long)attempt,
                 s_offline_mode_resume_session);
        return;
    }

    atomic_store_explicit(&s_offline_mode_capture_failed, true,
                          memory_order_release);
    if (s_offline_capture_retry_attempts < UINT32_MAX) {
        s_offline_capture_retry_attempts++;
    }
    if (s_offline_capture_retry_backoff_ms == 0U) {
        s_offline_capture_retry_backoff_ms =
            NUWAX_OFFLINE_CAPTURE_RETRY_INITIAL_MS;
    } else if (s_offline_capture_retry_backoff_ms <
               NUWAX_OFFLINE_CAPTURE_RETRY_MAX_MS / 2U) {
        s_offline_capture_retry_backoff_ms *= 2U;
    } else {
        s_offline_capture_retry_backoff_ms =
            NUWAX_OFFLINE_CAPTURE_RETRY_MAX_MS;
    }
    s_offline_capture_retry_next_ms =
        now + s_offline_capture_retry_backoff_ms;
    const nuwax_audio_runtime_memory_t memory =
        exclusive_audio_memory_snapshot();
    char detail[NUWAX_DEVICE_ERROR_RESULT_BYTES];
    snprintf(detail,
             sizeof(detail),
             "%s;attempt=%lu;retry_ms=%lu;internal=%u;largest=%u;dma=%u",
             esp_err_to_name(err),
             (unsigned long)s_offline_capture_retry_attempts,
             (unsigned long)s_offline_capture_retry_backoff_ms,
             (unsigned)memory.internal_free,
             (unsigned)memory.internal_largest,
             (unsigned)memory.dma_largest);
    (void)nuwax_device_warning_report_enqueue_detail(
        "audio", "offline_capture_retry", err, detail, NULL, 0U, NULL);
    ESP_LOGW(TAG,
             "offline capture retry deferred mode=%s attempt=%lu err=%s next_ms=%lu internal=%u largest=%u dma=%u",
             offline_mode_kind() == NUWAX_OFFLINE_AUDIO_MODE_MONITORING
                 ? "monitoring"
                 : "meeting",
             (unsigned long)s_offline_capture_retry_attempts,
             esp_err_to_name(err),
             (unsigned long)s_offline_capture_retry_next_ms,
             (unsigned)memory.internal_free,
             (unsigned)memory.internal_largest,
             (unsigned)memory.dma_largest);
}

static void sync_exclusive_audio_mode_ui_state(void) {
    static uint32_t published_ui_generation = UINT32_MAX;
    static nuwax_meeting_state_t published_meeting =
        (nuwax_meeting_state_t)-1;
    static nuwax_monitoring_state_t published_monitoring =
        (nuwax_monitoring_state_t)-1;

    if (!ui_ready_snapshot()) {
        return;
    }

    const uint32_t ui_generation = (uint32_t)atomic_load_explicit(
        &s_ui_lifecycle_generation, memory_order_acquire);
    if (published_ui_generation != ui_generation) {
        published_ui_generation = ui_generation;
        published_meeting = (nuwax_meeting_state_t)-1;
        published_monitoring = (nuwax_monitoring_state_t)-1;
    }

    nuwax_meeting_state_t meeting =
        nuwax_meeting_mode_state(&s_meeting_mode);
    nuwax_monitoring_state_t monitoring =
        nuwax_monitoring_mode_state(&s_monitoring_mode);
#if CONFIG_NUWAX_OFFLINE_AUDIO_ENABLE
    /* The cloud-bound controllers intentionally reset on a disconnected
     * epoch, while the SD capture retains the user's meeting/monitoring
     * lease. Publish that durable local owner as ACTIVE so the face and mode
     * controls do not lie about a capture that is still running. */
    if (meeting == NUWAX_MEETING_STATE_DISABLED &&
        monitoring == NUWAX_MONITORING_STATE_DISABLED &&
        (offline_mode_lease_pending() ||
        nuwax_meeting_mode_offline_resume_pending(&s_meeting_mode) ||
        nuwax_monitoring_mode_offline_resume_pending(&s_monitoring_mode) ||
        nuwax_offline_audio_capture_running())) {
        const bool monitoring_resume =
            nuwax_monitoring_mode_offline_resume_pending(&s_monitoring_mode);
        const bool meeting_resume =
            nuwax_meeting_mode_offline_resume_pending(&s_meeting_mode);
        if (monitoring_resume || (!meeting_resume && offline_mode_kind() ==
            NUWAX_OFFLINE_AUDIO_MODE_MONITORING)) {
            monitoring = NUWAX_MONITORING_STATE_ACTIVE;
            meeting = NUWAX_MEETING_STATE_DISABLED;
        } else {
            meeting = NUWAX_MEETING_STATE_ACTIVE;
            monitoring = NUWAX_MONITORING_STATE_DISABLED;
        }
    }
#endif
    if (published_meeting != meeting) {
        nuwax_app_shell_set_meeting_state((nuwax_meeting_ui_state_t)meeting);
        published_meeting = meeting;
        ESP_LOGI(TAG,
                 "meeting UI reconciled controller_state=%d ui_generation=%lu",
                 (int)meeting,
                 (unsigned long)ui_generation);
    }
    if (published_monitoring != monitoring) {
        nuwax_app_shell_set_monitoring_state(
            (nuwax_monitoring_ui_state_t)monitoring);
        published_monitoring = monitoring;
        ESP_LOGI(TAG,
                 "monitoring UI reconciled controller_state=%d ui_generation=%lu",
                 (int)monitoring,
                 (unsigned long)ui_generation);
    }
}