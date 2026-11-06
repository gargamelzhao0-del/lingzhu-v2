/* Included by the conversation owner after its lifecycle helpers. Only that
 * owner mutates pending requests. Cross-task preemption uses atomics only. */
#if CONFIG_NUWAX_MUSIC_PLAYBACK_ENABLE
static struct {
    bool valid, local;
    nuwax_ws_client_t *ws;
    uint32_t epoch, deadline;
    uint32_t output_release_retry_ms, output_release_attempts;
    char id[80], url[NUWAX_SESSION_MUSIC_URL_MAX_LEN], format[8];
} s_music_pending;
static uint32_t s_music_visual_ms;
static char s_music_visual_state[16];
static nuwax_ws_client_t *s_music_worker_ws;
static uint32_t s_music_worker_epoch;

static bool product_music_forbidden(void) {
    return s_power_shutdown_requested || nuwax_power_ctrl_shutdown_preparing() ||
           s_sleep_transaction.active || s_deferred_sleep_intent.active ||
           reprovision_request_pending() || exclusive_audio_mode_blocks_normal_audio() ||
           meeting_or_monitoring_mode_blocks_sleep() ||
           nuwax_notification_blocks_conversation(&s_notification_transaction)
#if CONFIG_NUWAX_OTA_ENABLE
           || nuwax_ota_device_blocks_audio() || nuwax_ota_device_is_active()
#endif
           ;
}
static void music_pending_terminal(const char *state, const char *reason) {
    if (!s_music_pending.valid) return;
    if (!s_music_pending.local && s_music_pending.ws &&
        nuwax_ws_is_connected(s_music_pending.ws) &&
        nuwax_ws_connection_epoch(s_music_pending.ws) == s_music_pending.epoch)
        nuwax_music_status_post(s_music_pending.ws, s_music_pending.epoch,
                                s_music_pending.id, state, reason);
#if CONFIG_NUWAX_MUSIC_CONTROL_TEST_ENABLE
    if (s_music_pending.local)
        nuwax_music_test_control_result(s_music_pending.id, false, reason);
#endif
    s_music_pending.valid = false;
}
static bool product_music_request(nuwax_ws_client_t *ws, const char *id,
                                   const char *url, const char *format, bool local) {
    if (product_music_forbidden() || atomic_load(&s_product_music_cancel) ||
        !id || !id[0] || strlen(id) >= sizeof(s_music_pending.id) ||
        !url || strlen(url) >= sizeof(s_music_pending.url) || !format ||
        (!local && (!ws || strncmp(url, "https://", 8) || strcmp(format, "mp3"))))
        return false;
    /* Retry of an already-admitted request must not restart the song. */
    if (s_music_pending.valid && s_music_pending.local == local &&
        (local || s_music_pending.epoch == nuwax_ws_connection_epoch(ws)) &&
        s_music_pending.ws == ws && !strcmp(s_music_pending.id, id))
        return !strcmp(s_music_pending.url, url) && !strcmp(s_music_pending.format, format);
    if (!s_music_pending.valid && nuwax_music_playback_active() &&
        s_music_worker_ws == (local ? NULL : ws) &&
        (local || s_music_worker_epoch == nuwax_ws_connection_epoch(ws))) {
        nuwax_music_snapshot_t active;
        nuwax_music_playback_snapshot(&active);
        if (!strcmp(active.request_id, id)) return true;
    }
    if (!local && !nuwax_music_status_reserve(ws, nuwax_ws_connection_epoch(ws), id))
        return false;
    music_pending_terminal("cancelled", "replaced");
    nuwax_music_playback_cancel("replaced");
#if CONFIG_NUWAX_MUSIC_URL_TEST_ENABLE
    nuwax_music_url_test_request_stop("formal_music");
#endif
    s_music_pending.valid = true;
    s_music_pending.local = local;
    s_music_pending.ws = ws;
    s_music_pending.epoch = ws ? nuwax_ws_connection_epoch(ws) : 0U;
    s_music_pending.deadline = monotonic_ms() + 8000U;
    s_music_pending.output_release_retry_ms = monotonic_ms();
    s_music_pending.output_release_attempts = 0U;
    strlcpy(s_music_pending.id, id, sizeof(s_music_pending.id));
    strlcpy(s_music_pending.url, url, sizeof(s_music_pending.url));
    strlcpy(s_music_pending.format, format, sizeof(s_music_pending.format));
    atomic_store_explicit(&s_product_music_owned, true, memory_order_release);
    (void)update_normal_downlink_admission(false);
    cancel_auto_followup_capture("music_start");
    clear_awake_session("music_start");
    clear_deferred_shell_listen_actions("music_start");
    (void)invalidate_all_pending_semantic_emotions("music_start");
    nuwax_sleep_transaction_reset_for_wake(&s_sleep_transaction);
    clear_sleep_request();
    s_post_playback_resume_pending = false;
    inhibit_sleep_wake_word_detector("music_start");
    nuwax_wake_word_detector_request_stop();
    if (s_wake_word_events) xQueueReset(s_wake_word_events);
    nuwax_speaker_playback_request_fade_cancel("music_start", 80U);
    ESP_LOGI(TAG, "music admitted request=%s replacement=%d local=%d",
             id, nuwax_music_playback_active(), local);
    return true;
}
static void service_product_music(nuwax_ws_client_t *ws) {
    if (atomic_exchange_explicit(&s_product_music_cancel, false, memory_order_acq_rel)) {
        music_pending_terminal("cancelled", "preempted");
        nuwax_music_playback_cancel("preempted");
    }
#if CONFIG_NUWAX_MUSIC_CONTROL_TEST_ENABLE
    nuwax_music_test_command_t command;
    if (nuwax_music_test_control_take(&command)) {
        bool accepted = true;
        if (command.stop) {
            music_pending_terminal("cancelled", "gateway_stop");
            nuwax_music_playback_cancel("gateway_stop");
        } else {
            char url[NUWAX_SESSION_MUSIC_URL_MAX_LEN];
            const int n = snprintf(url, sizeof(url), "%s%s",
                                    CONFIG_NUWAX_MUSIC_CONTROL_TEST_URL, command.path);
            accepted = n > 0 && n < sizeof(url) &&
                product_music_request(NULL, command.request_id, url, command.format, true);
        }
        nuwax_music_test_control_result(command.request_id, accepted,
                                         accepted ? "accepted" : "device_busy");
    }
#endif
    if (!product_music_blocks_audio()) return;
    if (product_music_forbidden()) {
        music_pending_terminal("cancelled", "higher_priority");
        nuwax_music_playback_cancel("higher_priority");
    }
    if (s_music_pending.valid && !s_music_pending.local &&
        (!s_music_pending.ws || !nuwax_ws_is_connected(s_music_pending.ws) ||
         nuwax_ws_connection_epoch(s_music_pending.ws) != s_music_pending.epoch))
        music_pending_terminal("cancelled", "connection_changed");
    bool worker = nuwax_music_playback_active();
    if (worker && s_music_worker_ws &&
        (!nuwax_ws_is_connected(s_music_worker_ws) ||
         nuwax_ws_connection_epoch(s_music_worker_ws) != s_music_worker_epoch))
        nuwax_music_playback_cancel("connection_changed");
    if (s_music_pending.valid &&
        (int32_t)(monotonic_ms() - s_music_pending.deadline) >= 0)
        music_pending_terminal("failed", "audio_release_timeout");
    if (s_music_pending.valid && !worker) {
        const bool old_test =
#if CONFIG_NUWAX_MUSIC_URL_TEST_ENABLE
            nuwax_music_url_test_active();
#else
            false;
#endif
        if ((int32_t)(monotonic_ms() - s_music_pending.deadline) >= 0) {
            music_pending_terminal("failed", "audio_release_timeout");
        } else if (!old_test && !conversation_capture_busy_for_sleep() &&
                   !s_prelisten.capture_active && !nuwax_audio_uplink_sender_busy() &&
                   !nuwax_wake_word_detector_is_running() &&
                   !nuwax_speaker_playback_has_pending() &&
                   (int32_t)(monotonic_ms() - s_music_pending.output_release_retry_ms) >= 0) {
            /* The ordinary idle path keeps TX leased for fast voice replies.
             * Music changes the shared clock and needs an actual release. */
            const esp_err_t release_err = nuwax_speaker_playback_deinit_output();
            s_music_pending.output_release_attempts++;
            if (release_err != ESP_OK) {
                s_music_pending.output_release_retry_ms = monotonic_ms() + 250U;
                if (s_music_pending.output_release_attempts == 1U ||
                    (s_music_pending.output_release_attempts % 8U) == 0U)
                    ESP_LOGW(TAG, "music output release deferred request=%s attempt=%lu err=%s",
                             s_music_pending.id,
                             (unsigned long)s_music_pending.output_release_attempts,
                             esp_err_to_name(release_err));
            }
            if (release_err == ESP_OK && !atomic_load(&s_product_music_cancel) &&
                !product_music_forbidden() &&
                (int32_t)(monotonic_ms() - s_music_pending.deadline) < 0 &&
                (s_music_pending.local ||
                 (nuwax_ws_is_connected(s_music_pending.ws) &&
                  nuwax_ws_connection_epoch(s_music_pending.ws) == s_music_pending.epoch))) {
                ESP_LOGI(TAG, "music audio handoff ready request=%s attempts=%lu",
                         s_music_pending.id, (unsigned long)s_music_pending.output_release_attempts);
                worker = s_music_pending.local
                    ? nuwax_music_playback_start_test(s_music_pending.id, s_music_pending.url,
                                                       s_music_pending.format)
                    : nuwax_music_playback_start(s_music_pending.ws, s_music_pending.id,
                                                  s_music_pending.url, s_music_pending.format);
                if (!worker) music_pending_terminal("failed", "music_start_failed");
                else {
                    s_music_worker_ws = s_music_pending.local ? NULL : s_music_pending.ws;
                    s_music_worker_epoch = s_music_pending.epoch;
                    if (atomic_load(&s_product_music_cancel)) nuwax_music_playback_cancel("preempted");
                    s_music_pending.valid = false;
                    set_state(NUWAX_APP_STATE_IDLE);
                    s_music_visual_state[0] = 0;
                }
            }
        }
    }
    /* DNS/TLS teardown may outlive audible playback. Once the clock and
     * audio lease are restored, normal interaction need not wait for HTTP.
     * A replacement still waits for that worker before reusing its state. */
    if ((!worker || !nuwax_music_playback_audio_active()) && !s_music_pending.valid) {
        atomic_store_explicit(&s_product_music_owned, false, memory_order_release);
        (void)nuwax_state_bridge_clear_semantic_emotion_if_owner("music", 0U);
        s_music_visual_state[0] = 0;
        if (!product_music_forbidden()) {
            (void)commit_sleeping_conversation_state("music_finished");
            request_sleep_wake_word_detector("music_finished");
        }
        return;
    }
    if (!product_music_forbidden()) {
        nuwax_music_snapshot_t snapshot;
        nuwax_music_playback_snapshot(&snapshot);
        const char *visual = worker && strcmp(snapshot.state, "playing") == 0
                                 ? "happy" : "neutral";
        if (strcmp(s_music_visual_state, visual) ||
            monotonic_ms() - s_music_visual_ms >= 2000U) {
            (void)nuwax_state_bridge_set_semantic_emotion_name_owned(visual, "music", 0U);
            strlcpy(s_music_visual_state, visual, sizeof(s_music_visual_state));
            s_music_visual_ms = monotonic_ms();
        }
    }
    (void)ws;
}
#else
static void service_product_music(nuwax_ws_client_t *ws) { (void)ws; }
#endif
