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

