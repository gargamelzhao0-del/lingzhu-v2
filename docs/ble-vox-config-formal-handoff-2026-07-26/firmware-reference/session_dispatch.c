#include "session_dispatch.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "NuwaxPartner6";

#ifndef CONFIG_NUWAX_VOICE_OUTPUT_SAMPLE_RATE
#define CONFIG_NUWAX_VOICE_OUTPUT_SAMPLE_RATE 16000
#endif

#define NUWAX_P3_SOFTREF_AB_DEFAULT_DELAY_SAMPLES 240U
#define NUWAX_BARGE_IN_FADE_CANCEL_MS 200U

static void send_command_ack(nuwax_ws_client_t *ws,
                             const nuwax_session_command_t *command,
                             bool ok,
                             const char *message) {
    ESP_ERROR_CHECK_WITHOUT_ABORT(nuwax_session_send_command_ack(ws,
                                                                 command->request_id,
                                                                 command->command,
                                                                 ok,
                                                                 message));
}

static bool parse_remote_state(const char *value, nuwax_app_state_t *out_state) {
    if (!value || !out_state) {
        return false;
    }
    if (strcmp(value, "booting") == 0) {
        *out_state = NUWAX_APP_STATE_BOOTING;
        return true;
    }
    if (strcmp(value, "wifi_connecting") == 0) {
        *out_state = NUWAX_APP_STATE_WIFI_CONNECTING;
        return true;
    }
    if (strcmp(value, "ws_connecting") == 0) {
        *out_state = NUWAX_APP_STATE_WS_CONNECTING;
        return true;
    }
    if (strcmp(value, "idle") == 0) {
        *out_state = NUWAX_APP_STATE_IDLE;
        return true;
    }
    if (strcmp(value, "sleeping") == 0 || strcmp(value, "wake_only") == 0) {
        *out_state = NUWAX_APP_STATE_SLEEPING;
        return true;
    }
    if (strcmp(value, "listening") == 0) {
        *out_state = NUWAX_APP_STATE_LISTENING;
        return true;
    }
    if (strcmp(value, "thinking") == 0) {
        *out_state = NUWAX_APP_STATE_THINKING;
        return true;
    }
    if (strcmp(value, "speaking") == 0) {
        *out_state = NUWAX_APP_STATE_SPEAKING;
        return true;
    }
    if (strcmp(value, "error") == 0) {
        *out_state = NUWAX_APP_STATE_ERROR;
        return true;
    }
    return false;
}

void nuwax_session_dispatch_command(nuwax_ws_client_t *ws,
                                    const nuwax_session_command_t *command,
                                    const nuwax_session_dispatch_callbacks_t *callbacks) {
    if (!command || !command->has_command) {
        return;
    }

    ESP_LOGI(TAG,
             "device_command command=%s request=%s turn=%s",
             command->command,
             command->request_id,
             command->turn_id[0] ? command->turn_id : "-");
    if (command->turn_id[0] != '\0' && callbacks && callbacks->set_active_turn_id) {
        callbacks->set_active_turn_id(callbacks->ctx, command->turn_id);
    }

    if (strcmp(command->command, "start_ble_provisioning") == 0) {
        if (callbacks && callbacks->request_ble_reprovision &&
            callbacks->request_ble_reprovision(callbacks->ctx)) {
            send_command_ack(ws, command, true, "restarting into BLE provisioning");
        } else {
            send_command_ack(ws, command, false, "BLE provisioning restart already pending");
        }
        return;
    }

    if (strcmp(command->command, "wifi_connect") == 0) {
        if (command->ssid[0] == '\0') {
            send_command_ack(ws, command, false, "missing ssid");
            return;
        }
        if (callbacks && callbacks->queue_wifi_connect &&
            callbacks->queue_wifi_connect(callbacks->ctx,
                                          command->ssid,
                                          command->password,
                                          command->request_id)) {
            send_command_ack(ws, command, true, "connecting");
        } else {
            send_command_ack(ws, command, false, "wifi queue full");
        }
        return;
    }

    if (strcmp(command->command, "wifi_scan") == 0) {
        send_command_ack(ws, command, true, "scanning");
        if (callbacks && callbacks->request_wifi_scan) {
            callbacks->request_wifi_scan(callbacks->ctx);
        }
        return;
    }

    if (strcmp(command->command, "sleep_conversation") == 0) {
        ESP_LOGI(TAG,
                 "sleep_conversation request=%s turn=%s text_len=%u",
                 command->request_id,
                 command->turn_id[0] ? command->turn_id : "-",
                 (unsigned)strlen(command->text));
        if (callbacks && callbacks->cancel_auto_followup_capture) {
            callbacks->cancel_auto_followup_capture(callbacks->ctx, "remote_sleep");
        }
#if CONFIG_NUWAX_SPEAKER_ENABLE
        if (callbacks && callbacks->speaker_has_pending && callbacks->speaker_has_pending(callbacks->ctx) &&
            callbacks->speaker_cancel_now) {
            callbacks->speaker_cancel_now(callbacks->ctx, "sleep_conversation");
        }
#endif
        if (callbacks && callbacks->enter_sleeping_conversation_state) {
            callbacks->enter_sleeping_conversation_state(callbacks->ctx, "semantic_sleep");
        }
        send_command_ack(ws, command, true, "sleeping");
        return;
    }

    if (strcmp(command->command, "set_state") == 0) {
        nuwax_app_state_t remote_state = NUWAX_APP_STATE_IDLE;
        if (!parse_remote_state(command->state, &remote_state)) {
            send_command_ack(ws, command, false, "unknown state");
            return;
        }
#if CONFIG_NUWAX_SPEAKER_ENABLE
        if (remote_state == NUWAX_APP_STATE_IDLE &&
            callbacks && callbacks->speaker_has_pending && callbacks->speaker_has_pending(callbacks->ctx)) {
            ESP_LOGI(TAG, "defer remote idle while speaker playback is active");
            send_command_ack(ws, command, true, "idle deferred until playback end");
            return;
        }
#endif
#if CONFIG_NUWAX_AUDIO_SOURCE_I2S_MIC
        if (remote_state == NUWAX_APP_STATE_IDLE &&
            callbacks && callbacks->local_blocks_remote_idle &&
            callbacks->local_blocks_remote_idle(callbacks->ctx)) {
            ESP_LOGI(TAG, "ignore remote idle while local conversation state is active");
            send_command_ack(ws, command, true, "idle ignored by local conversation state");
            return;
        }
#endif
        if (callbacks && callbacks->set_state) {
            callbacks->set_state(callbacks->ctx, remote_state);
        }
        send_command_ack(ws, command, true, command->state);
        return;
    }

    if (strcmp(command->command, "set_emotion") == 0) {
        if (command->emotion[0] == '\0') {
            send_command_ack(ws, command, false, "missing emotion");
            return;
        }
        bool ok = callbacks && callbacks->set_emotion &&
                  callbacks->set_emotion(callbacks->ctx, command->emotion);
        send_command_ack(ws, command, ok, ok ? command->emotion : "unsupported emotion");
        return;
    }

    if (strcmp(command->command, "stop_playback") == 0) {
        const bool scoped_playback =
            command->turn_id[0] != '\0' || command->playback_id[0] != '\0';
        if (scoped_playback &&
            callbacks &&
            callbacks->speaker_playback_matches &&
            !callbacks->speaker_playback_matches(callbacks->ctx,
                                                 command->turn_id,
                                                 command->playback_id)) {
            ESP_LOGW(TAG,
                     "stale stop_playback ignored turn=%s playback=%s",
                     command->turn_id,
                     command->playback_id);
            send_command_ack(ws, command, true, "stale playback ignored");
            return;
        }
        ESP_LOGI(TAG, "stop_playback turn=%s title=%s", command->turn_id, command->title);
        const bool preserve_capture =
            callbacks && callbacks->preserve_capture_on_stop_playback &&
            callbacks->preserve_capture_on_stop_playback(callbacks->ctx);
#if CONFIG_NUWAX_SPEAKER_ENABLE
        if (preserve_capture && callbacks && callbacks->speaker_request_fade_cancel) {
            callbacks->speaker_request_fade_cancel(callbacks->ctx,
                                                   "stop_playback_barge_in",
                                                   NUWAX_BARGE_IN_FADE_CANCEL_MS);
        } else if (callbacks && callbacks->speaker_cancel_now) {
            callbacks->speaker_cancel_now(callbacks->ctx, "stop_playback");
        }
#endif
        if (callbacks && callbacks->set_conversation_status) {
            callbacks->set_conversation_status(callbacks->ctx,
                                               preserve_capture
                                                   ? "VOICE"
                                                   : (command->title[0] ? command->title : "VOICE"),
                                               preserve_capture ? "LISTENING" : "CANCEL",
                                               preserve_capture
                                                   ? "listening"
                                                   : (command->text[0] ? command->text : "playback stopped"),
                                               true);
        }
        if (preserve_capture) {
            if (callbacks && callbacks->set_state) {
                callbacks->set_state(callbacks->ctx, NUWAX_APP_STATE_LISTENING);
            }
            ESP_LOGI(TAG, "stop_playback preserved active during-playback capture");
        } else if (callbacks && callbacks->return_to_listening_after_cancel) {
            callbacks->return_to_listening_after_cancel(callbacks->ctx, "cancel_followup");
        }
        send_command_ack(ws, command, true, preserve_capture ? "stopped capture_preserved=1" : "stopped");
        return;
    }

    if (strcmp(command->command, "show_status") == 0) {
        const char *title = command->title[0] ? command->title : "PC TASK";
        ESP_LOGI(TAG,
                 "show_status title=%s line0=%s line1=%s line2=%s text_len=%u",
                 title,
                 command->lines[0],
                 command->lines[1],
                 command->lines[2],
                 (unsigned)strlen(command->text));
        if (callbacks && callbacks->show_status) {
            callbacks->show_status(callbacks->ctx, title, command->lines, command->text);
        }
        send_command_ack(ws, command, true, "shown");
        return;
    }

    if (strcmp(command->command, "set_volume") == 0) {
        if (!command->has_volume_percent) {
            send_command_ack(ws, command, false, "volume_percent is required");
            return;
        }
        if (command->volume_percent < 0 || command->volume_percent > 100) {
            send_command_ack(ws, command, false, "volume_percent must be 0..100");
            return;
        }
        if (!callbacks || !callbacks->apply_volume_percent) {
            send_command_ack(ws, command, false, "set_volume unsupported");
            return;
        }
        const bool persist = command->has_volume_persist ? command->volume_persist : false;
        callbacks->apply_volume_percent(callbacks->ctx, command->volume_percent, persist);
        char message[48];
        snprintf(message, sizeof(message), "volume=%d persist=%d", command->volume_percent, persist ? 1 : 0);
        send_command_ack(ws, command, true, message);
        return;
    }

    if (strcmp(command->command, "set_audio_cleaner") == 0) {
        if (!command->has_audio_cleaner_enabled) {
            send_command_ack(ws, command, false, "enabled is required");
            return;
        }
        if (!callbacks || !callbacks->set_audio_cleaner_enabled) {
            send_command_ack(ws, command, false, "set_audio_cleaner unsupported");
            return;
        }
        esp_err_t err = callbacks->set_audio_cleaner_enabled(callbacks->ctx, command->audio_cleaner_enabled);
        char message[48];
        snprintf(message,
                 sizeof(message),
                 "audio_cleaner_enabled=%d",
                 command->audio_cleaner_enabled ? 1 : 0);
        send_command_ack(ws, command, err == ESP_OK, message);
        return;
    }

    if (strcmp(command->command, "set_l3_interrupt") == 0) {
        if (!callbacks || !callbacks->set_l3_interrupt_enabled) {
            send_command_ack(ws, command, false, "set_l3_interrupt unsupported");
            return;
        }
        const bool enabled = command->has_interrupt_enabled ? command->interrupt_enabled : true;
        /* persist=false means runtime-only (no NVS write). Default true so
         * set_l3_interrupt without an explicit persist still persists. */
        const bool persist = command->has_interrupt_persist ? command->interrupt_persist : true;
        esp_err_t err = callbacks->set_l3_interrupt_enabled(callbacks->ctx, enabled, persist);
        char message[48];
        snprintf(message, sizeof(message), "l3_enabled=%d persist=%d", enabled ? 1 : 0, persist ? 1 : 0);
        send_command_ack(ws, command, err == ESP_OK, message);
        return;
    }

    if (strcmp(command->command, "p3_ref_health") == 0) {
        const float volume_db = command->has_volume_db ? command->volume_db : -18.0f;
        const uint32_t duration_ms = command->has_duration_ms ? command->duration_ms : 2000U;
        bool passed = false;
        if (!callbacks || !callbacks->run_p3_ref_health) {
            send_command_ack(ws, command, false, "p3 ref health unsupported");
            return;
        }
        esp_err_t err = callbacks->run_p3_ref_health(callbacks->ctx,
                                                     volume_db,
                                                     duration_ms,
                                                     command->run_ref_probe,
                                                     &passed);
        char message[96];
        snprintf(message,
                 sizeof(message),
                 "err=%s passed=%d volume_db=%.1f duration_ms=%lu",
                 esp_err_to_name(err),
                 passed ? 1 : 0,
                 (double)volume_db,
                 (unsigned long)duration_ms);
        send_command_ack(ws, command, err == ESP_OK && passed, message);
        return;
    }

    if (strcmp(command->command, "p3_softref_ab") == 0) {
        const uint32_t duration_ms = command->has_duration_ms ? command->duration_ms : 1000U;
        uint32_t delay_samples = NUWAX_P3_SOFTREF_AB_DEFAULT_DELAY_SAMPLES;
        if (command->has_delay_samples) {
            delay_samples = command->delay_samples;
        } else if (command->has_delay_ms) {
            delay_samples = (uint32_t)(((uint64_t)CONFIG_NUWAX_VOICE_OUTPUT_SAMPLE_RATE *
                                        (uint64_t)command->delay_ms) /
                                       1000ULL);
        }
        bool ready = false;
        if (!callbacks || !callbacks->run_p3_softref_ab) {
            send_command_ack(ws, command, false, "p3 softref ab unsupported");
            return;
        }
        esp_err_t err = callbacks->run_p3_softref_ab(callbacks->ctx,
                                                     duration_ms,
                                                     delay_samples,
                                                     &ready);
        char message[96];
        snprintf(message,
                 sizeof(message),
                 "err=%s ready=%d duration_ms=%lu delay_samples=%lu",
                 esp_err_to_name(err),
                 ready ? 1 : 0,
                 (unsigned long)duration_ms,
                 (unsigned long)delay_samples);
        send_command_ack(ws, command, err == ESP_OK, message);
        return;
    }

    if (strcmp(command->command, "p3_hardref_ab") == 0) {
        const uint32_t duration_ms = command->has_duration_ms ? command->duration_ms : 1000U;
        bool ready = false;
        if (!callbacks || !callbacks->run_p3_hardref_ab) {
            send_command_ack(ws, command, false, "p3 hardref ab unsupported");
            return;
        }
        esp_err_t err = callbacks->run_p3_hardref_ab(callbacks->ctx, duration_ms, &ready);
        char message[112];
        snprintf(message,
                 sizeof(message),
                 "err=%s ready=%d duration_ms=%lu ref_slot=2",
                 esp_err_to_name(err),
                 ready ? 1 : 0,
                 (unsigned long)duration_ms);
        send_command_ack(ws, command, err == ESP_OK, message);
        return;
    }

    if (strcmp(command->command, "p3_nearend_candidate") == 0) {
        const uint32_t duration_ms = command->has_duration_ms ? command->duration_ms : 1200U;
        uint32_t delay_samples = NUWAX_P3_SOFTREF_AB_DEFAULT_DELAY_SAMPLES;
        if (command->has_delay_samples) {
            delay_samples = command->delay_samples;
        } else if (command->has_delay_ms) {
            delay_samples = (uint32_t)(((uint64_t)CONFIG_NUWAX_VOICE_OUTPUT_SAMPLE_RATE *
                                        (uint64_t)command->delay_ms) /
                                       1000ULL);
        }
        bool candidate = false;
        if (!callbacks || !callbacks->run_p3_nearend_candidate) {
            send_command_ack(ws, command, false, "p3 nearend candidate unsupported");
            return;
        }
        esp_err_t err = callbacks->run_p3_nearend_candidate(callbacks->ctx,
                                                            duration_ms,
                                                            delay_samples,
                                                            &candidate);
        char message[112];
        snprintf(message,
                 sizeof(message),
                 "err=%s candidate=%d duration_ms=%lu delay_samples=%lu record_only=1",
                 esp_err_to_name(err),
                 candidate ? 1 : 0,
                 (unsigned long)duration_ms,
                 (unsigned long)delay_samples);
        send_command_ack(ws, command, err == ESP_OK, message);
        return;
    }

    if (strcmp(command->command, "p3_interrupt_timing") == 0) {
        const uint32_t duration_ms = command->has_duration_ms ? command->duration_ms : 1200U;
        uint32_t delay_samples = NUWAX_P3_SOFTREF_AB_DEFAULT_DELAY_SAMPLES;
        if (command->has_delay_samples) {
            delay_samples = command->delay_samples;
        } else if (command->has_delay_ms) {
            delay_samples = (uint32_t)(((uint64_t)CONFIG_NUWAX_VOICE_OUTPUT_SAMPLE_RATE *
                                        (uint64_t)command->delay_ms) /
                                       1000ULL);
        }
        const char *mode = command->mode[0] ? command->mode : "record_only";
        bool candidate = false;
        bool cancel_sent = false;
        if (!callbacks || !callbacks->run_p3_interrupt_timing) {
            send_command_ack(ws, command, false, "p3 interrupt timing unsupported");
            return;
        }
        esp_err_t err = callbacks->run_p3_interrupt_timing(callbacks->ctx,
                                                           ws,
                                                           mode,
                                                           command->request_id,
                                                           duration_ms,
                                                           delay_samples,
                                                           &candidate,
                                                           &cancel_sent);
        char message[144];
        snprintf(message,
                 sizeof(message),
                 "err=%s mode=%s candidate=%d cancel_sent=%d duration_ms=%lu delay_samples=%lu",
                 esp_err_to_name(err),
                 mode,
                 candidate ? 1 : 0,
                 cancel_sent ? 1 : 0,
                 (unsigned long)duration_ms,
                 (unsigned long)delay_samples);
        send_command_ack(ws, command, err == ESP_OK, message);
        return;
    }

    if (strcmp(command->command, "p5_fullduplex_overlap_probe") == 0) {
        const uint32_t duration_ms = command->has_duration_ms ? command->duration_ms : 1200U;
        bool passed = false;
        if (!callbacks || !callbacks->run_p5_fullduplex_overlap_probe) {
            send_command_ack(ws, command, false, "p5 full-duplex overlap unsupported");
            return;
        }
        esp_err_t err = callbacks->run_p5_fullduplex_overlap_probe(callbacks->ctx,
                                                                   ws,
                                                                   command->request_id,
                                                                   command->audio_session_id,
                                                                   duration_ms,
                                                                   &passed);
        char message[112];
        snprintf(message,
                 sizeof(message),
                 "err=%s passed=%d duration_ms=%lu audio_session_id=%s",
                 esp_err_to_name(err),
                 passed ? 1 : 0,
                 (unsigned long)duration_ms,
                 command->audio_session_id[0] ? command->audio_session_id : "-");
        send_command_ack(ws, command, err == ESP_OK && passed, message);
        return;
    }

    if (strcmp(command->command, "p5_during_playback_candidate") == 0) {
        const uint32_t duration_ms = command->has_duration_ms ? command->duration_ms : 5000U;
        uint32_t delay_samples = NUWAX_P3_SOFTREF_AB_DEFAULT_DELAY_SAMPLES;
        if (command->has_delay_samples) {
            delay_samples = command->delay_samples;
        } else if (command->has_delay_ms) {
            delay_samples = (uint32_t)(((uint64_t)CONFIG_NUWAX_VOICE_OUTPUT_SAMPLE_RATE *
                                        (uint64_t)command->delay_ms) /
                                       1000ULL);
        }
        const char *mode = command->mode[0] ? command->mode : "record_only";
        bool candidate = false;
        bool cancel_sent = false;
        bool passed = false;
        if (!callbacks || !callbacks->run_p5_during_playback_candidate) {
            send_command_ack(ws, command, false, "p5 during-playback candidate unsupported");
            return;
        }
        esp_err_t err = callbacks->run_p5_during_playback_candidate(callbacks->ctx,
                                                                    ws,
                                                                    mode,
                                                                    command->request_id,
                                                                    command->audio_session_id,
                                                                    duration_ms,
                                                                    delay_samples,
                                                                    &candidate,
                                                                    &cancel_sent,
                                                                    &passed);
        char message[192];
        snprintf(message,
                 sizeof(message),
                 "err=%s mode=%s passed=%d candidate=%d cancel_sent=%d duration_ms=%lu delay_samples=%lu audio_session_id=%s",
                 esp_err_to_name(err),
                 mode,
                 passed ? 1 : 0,
                 candidate ? 1 : 0,
                 cancel_sent ? 1 : 0,
                 (unsigned long)duration_ms,
                 (unsigned long)delay_samples,
                 command->audio_session_id[0] ? command->audio_session_id : "-");
        /* P6-GRAYCONFIRM-003-2A: gray_confirm ack 表示命令执行完成,不需要 passed=true */
        const bool gray_confirm_mode = strcmp(mode, "gray_confirm") == 0;
        const bool gray_cancel_mode  = strcmp(mode, "gray_cancel") == 0;
        const bool ack_ok = err == ESP_OK &&
                            (gray_confirm_mode ||
                             passed ||
                             (gray_cancel_mode && cancel_sent));
        /* gray_cancel 路径不允许 cancel_sent=0 时 ack ok(旧约束保留) */
        (void)gray_cancel_mode;
        send_command_ack(ws, command, ack_ok, message);
        return;
    }

    send_command_ack(ws, command, false, "unknown command");
}
