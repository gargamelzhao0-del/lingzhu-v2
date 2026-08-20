#include "session_dispatch.h"

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "NuwaxPartner6";

#ifndef CONFIG_NUWAX_VOICE_OUTPUT_SAMPLE_RATE
#define CONFIG_NUWAX_VOICE_OUTPUT_SAMPLE_RATE 16000
#endif

#define NUWAX_P3_SOFTREF_AB_DEFAULT_DELAY_SAMPLES 240U
#define NUWAX_COMMAND_LEDGER_CAPACITY 8U
#define NUWAX_COMMAND_LEDGER_MESSAGE_BYTES 160U

typedef struct {
    bool occupied;
    bool ack_ready;
    bool ack_ok;
    uint32_t ws_epoch;
    uint32_t fingerprint;
    char request_id[sizeof(((nuwax_session_command_t *)0)->request_id)];
    char command[sizeof(((nuwax_session_command_t *)0)->command)];
    char message[NUWAX_COMMAND_LEDGER_MESSAGE_BYTES];
} nuwax_command_ledger_entry_t;

static nuwax_command_ledger_entry_t
    s_command_ledger[NUWAX_COMMAND_LEDGER_CAPACITY];
static uint32_t s_command_ledger_next;

static uint32_t command_payload_fingerprint(
    const nuwax_session_command_t *command) {
    const uint8_t *bytes = (const uint8_t *)command;
    uint32_t hash = 2166136261U;
    for (size_t i = 0U; command && i < sizeof(*command); ++i) {
        hash ^= bytes[i];
        hash *= 16777619U;
    }
    return hash ? hash : 1U;
}

static nuwax_command_ledger_entry_t *command_ledger_find(
    uint32_t ws_epoch,
    const char *request_id) {
    if (ws_epoch == 0U || !request_id || request_id[0] == '\0') {
        return NULL;
    }
    for (size_t i = 0U; i < NUWAX_COMMAND_LEDGER_CAPACITY; ++i) {
        if (s_command_ledger[i].occupied &&
            s_command_ledger[i].ws_epoch == ws_epoch &&
            strcmp(s_command_ledger[i].request_id, request_id) == 0) {
            return &s_command_ledger[i];
        }
    }
    return NULL;
}

static nuwax_command_ledger_entry_t *command_ledger_reserve(
    uint32_t ws_epoch,
    const nuwax_session_command_t *command,
    uint32_t fingerprint) {
    nuwax_command_ledger_entry_t *entry =
        &s_command_ledger[s_command_ledger_next %
                          NUWAX_COMMAND_LEDGER_CAPACITY];
    s_command_ledger_next++;
    memset(entry, 0, sizeof(*entry));
    entry->occupied = true;
    entry->ws_epoch = ws_epoch;
    entry->fingerprint = fingerprint;
    snprintf(entry->request_id,
             sizeof(entry->request_id),
             "%s",
             command->request_id);
    snprintf(entry->command,
             sizeof(entry->command),
             "%s",
             command->command);
    return entry;
}

static esp_err_t send_command_ack_raw(nuwax_ws_client_t *ws,
                                      const nuwax_session_command_t *command,
                                      bool ok,
                                      const char *message) {
    const uint32_t ws_epoch = nuwax_ws_connection_epoch(ws);
    return nuwax_session_send_command_ack_until_if_epoch(
        ws,
        command->request_id,
        command->command,
        ok,
        message,
        esp_timer_get_time() + 500000LL,
        ws_epoch);
}

static void send_command_ack(nuwax_ws_client_t *ws,
                             const nuwax_session_command_t *command,
                             bool ok,
                             const char *message) {
    const uint32_t ws_epoch = nuwax_ws_connection_epoch(ws);
    const uint32_t fingerprint = command_payload_fingerprint(command);
    nuwax_command_ledger_entry_t *entry =
        command_ledger_find(ws_epoch, command->request_id);
    if (entry && entry->fingerprint == fingerprint) {
        entry->ack_ready = true;
        entry->ack_ok = ok;
        snprintf(entry->message,
                 sizeof(entry->message),
                 "%s",
                 message ? message : "");
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        send_command_ack_raw(ws, command, ok, message));
}

static bool command_ledger_consumed_duplicate(
    nuwax_ws_client_t *ws,
    const nuwax_session_command_t *command) {
    const uint32_t ws_epoch = nuwax_ws_connection_epoch(ws);
    const uint32_t fingerprint = command_payload_fingerprint(command);
    nuwax_command_ledger_entry_t *entry =
        command_ledger_find(ws_epoch, command->request_id);
    if (!entry) {
        (void)command_ledger_reserve(ws_epoch, command, fingerprint);
        return false;
    }
    if (entry->fingerprint != fingerprint) {
        ESP_LOGE(TAG,
                 "device_command request_id payload conflict request=%s old_command=%s new_command=%s",
                 command->request_id,
                 entry->command,
                 command->command);
        ESP_ERROR_CHECK_WITHOUT_ABORT(send_command_ack_raw(
            ws, command, false, "request_id payload conflict"));
        return true;
    }
    if (!entry->ack_ready) {
        if (strcmp(command->command, "sleep_conversation") == 0 ||
            (strcmp(command->command, "set_state") == 0 &&
             (strcmp(command->state, "sleeping") == 0 ||
              strcmp(command->state, "wake_only") == 0))) {
            return false;
        }
        ESP_ERROR_CHECK_WITHOUT_ABORT(send_command_ack_raw(
            ws, command, false, "request still in progress"));
        return true;
    }
    ESP_LOGI(TAG,
             "device_command duplicate replay request=%s command=%s ok=%d",
             command->request_id,
             command->command,
             entry->ack_ok ? 1 : 0);
    ESP_ERROR_CHECK_WITHOUT_ABORT(send_command_ack_raw(
        ws, command, entry->ack_ok, entry->message));
    return true;
}

static void dispatch_sleep_transaction(
    nuwax_ws_client_t *ws,
    const nuwax_session_command_t *command,
    const nuwax_session_dispatch_callbacks_t *callbacks) {
    if (!callbacks || !callbacks->request_sleep_transaction) {
        send_command_ack(ws, command, false,
                         "sleep transaction unavailable");
        return;
    }

    const nuwax_sleep_request_result_t request_result =
        callbacks->request_sleep_transaction(callbacks->ctx,
                                             ws,
                                             command->request_id,
                                             command->command,
                                             command->turn_id);
    switch (request_result) {
        case NUWAX_SLEEP_REQUEST_STARTED:
        case NUWAX_SLEEP_REQUEST_DUPLICATE_ACTIVE:
            /* The conversation owner sends ACK only after local SLEEPING is
             * committed. A duplicate joins the same transaction. */
            return;
        case NUWAX_SLEEP_REQUEST_DUPLICATE_COMMITTED:
        case NUWAX_SLEEP_REQUEST_ALREADY_SLEEPING:
            send_command_ack(ws, command, true, "sleeping");
            return;
        case NUWAX_SLEEP_REQUEST_BUSY:
            send_command_ack(ws, command, false,
                              "another sleep transaction is active");
            return;
        case NUWAX_SLEEP_REQUEST_STALE_SCOPE:
            send_command_ack(ws, command, false,
                             "sleep command turn is stale");
            return;
        case NUWAX_SLEEP_REQUEST_BUSY_EXCLUSIVE:
            send_command_ack(ws, command, false,
                             "exclusive audio mode is active");
            return;
        case NUWAX_SLEEP_REQUEST_INVALID:
        default:
            send_command_ack(ws, command, false,
                             "sleep transaction rejected");
            return;
    }
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
             "device_command command=%s request=%s turn=%s state=%s",
             command->command,
             command->request_id,
             command->turn_id[0] ? command->turn_id : "-",
             command->state[0] ? command->state : "-");
    if (command->request_id[0] == '\0') {
        if (strcmp(command->command, "set_wake_word") != 0) {
            ESP_LOGE(TAG,
                     "device_command rejected without request_id command=%s",
                     command->command);
            return;
        }
        ESP_LOGW(TAG,
                 "accepting legacy unenveloped set_wake_word without request_id; retries cannot be correlated");
    } else if (command_ledger_consumed_duplicate(ws, command)) {
        return;
    }
    if (strcmp(command->command, "start_ble_provisioning") == 0) {
        if (callbacks && callbacks->request_ble_reprovision &&
            callbacks->request_ble_reprovision(callbacks->ctx)) {
            send_command_ack(ws, command, true, "restarting into BLE provisioning");
        } else {
            send_command_ack(ws, command, false,
                             "BLE provisioning restart busy or unavailable");
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
        const bool accepted = callbacks && callbacks->request_wifi_scan &&
                              callbacks->request_wifi_scan(
                                  callbacks->ctx,
                                  nuwax_ws_connection_epoch(ws),
                                  command->request_id);
        send_command_ack(ws,
                         command,
                         accepted,
                         accepted ? "scanning" : "scan busy or unavailable");
        return;
    }

    if (strcmp(command->command, "sleep_conversation") == 0) {
        ESP_LOGI(TAG,
                 "sleep_conversation request=%s turn=%s text_len=%u",
                 command->request_id,
                 command->turn_id[0] ? command->turn_id : "-",
                 (unsigned)strlen(command->text));
        dispatch_sleep_transaction(ws, command, callbacks);
        return;
    }

    if (strcmp(command->command, "set_state") == 0) {
        nuwax_app_state_t remote_state = NUWAX_APP_STATE_IDLE;
        if (!parse_remote_state(command->state, &remote_state)) {
            send_command_ack(ws, command, false, "unknown state");
            return;
        }
        if (remote_state == NUWAX_APP_STATE_SLEEPING) {
            dispatch_sleep_transaction(ws, command, callbacks);
            return;
        }
        ESP_LOGI(TAG,
                 "remote state ignored; local transport/audio lifecycle owns state=%s",
                 command->state);
        send_command_ack(ws,
                         command,
                         false,
                         "state isolated by local lifecycle");
        return;
    }

    if (strcmp(command->command, "set_emotion") == 0) {
        if (command->emotion[0] == '\0') {
            send_command_ack(ws, command, false, "missing emotion");
            return;
        }
        if (command->has_duration_ms &&
            (command->duration_ms < 500U || command->duration_ms > 30000U)) {
            send_command_ack(ws, command, false,
                             "duration_ms must be between 500 and 30000");
            return;
        }
        bool ok = false;
        if (callbacks && callbacks->set_emotion_with_context) {
            ok = callbacks->set_emotion_with_context(
                callbacks->ctx,
                command->emotion,
                command->has_duration_ms,
                command->duration_ms,
                command->request_id,
                command->turn_id);
        } else if (callbacks && callbacks->set_emotion) {
            ok = callbacks->set_emotion(callbacks->ctx,
                                        command->emotion,
                                        command->has_duration_ms,
                                        command->duration_ms);
        }
        const char *message = ok ? command->emotion : "emotion rejected";
        if (!ok && callbacks && callbacks->set_emotion_failure_reason) {
            const char *diagnostic =
                callbacks->set_emotion_failure_reason(callbacks->ctx);
            if (diagnostic && diagnostic[0] != '\0') {
                message = diagnostic;
            }
        }
        send_command_ack(ws, command, ok, message);
        return;
    }

    if (strcmp(command->command, "stop_playback") == 0) {
        if (command->turn_id[0] == '\0' ||
            command->playback_id[0] == '\0') {
            send_command_ack(ws,
                             command,
                             false,
                             "turn_id and playback_id required");
            return;
        }
        if (callbacks &&
            callbacks->active_turn_matches_playback &&
            !callbacks->active_turn_matches_playback(callbacks->ctx,
                                                     command->turn_id,
                                                     command->playback_id)) {
            const nuwax_session_playback_stop_result_t cleanup_result =
                callbacks->speaker_cancel_matching_playback
                    ? callbacks->speaker_cancel_matching_playback(
                          callbacks->ctx,
                          command->turn_id,
                          command->playback_id,
                          "stale_stop_playback")
                    : NUWAX_SESSION_PLAYBACK_STOP_REJECTED;
            const bool cleanup_armed =
                cleanup_result != NUWAX_SESSION_PLAYBACK_STOP_REJECTED;
            ESP_LOGI(TAG,
                     "stale stop_playback isolated turn=%s playback=%s cleanup_armed=%d",
                     command->turn_id,
                     command->playback_id,
                     cleanup_armed ? 1 : 0);
            send_command_ack(ws, command, true, "stale playback isolated");
            return;
        }
        if (callbacks &&
            callbacks->speaker_playback_matches &&
            !callbacks->speaker_playback_matches(callbacks->ctx,
                                                 command->turn_id,
                                                 command->playback_id)) {
            ESP_LOGI(TAG,
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
        const nuwax_session_playback_stop_result_t stop_result =
            callbacks && callbacks->speaker_cancel_matching_playback
                ? callbacks->speaker_cancel_matching_playback(
                      callbacks->ctx,
                      command->turn_id,
                      command->playback_id,
                      preserve_capture ? "stop_playback_barge_in"
                                       : "stop_playback")
                : NUWAX_SESSION_PLAYBACK_STOP_REJECTED;
        if (stop_result == NUWAX_SESSION_PLAYBACK_STOP_REJECTED) {
            send_command_ack(ws,
                             command,
                             false,
                             "matching playback could not be stopped");
            return;
        }
        if (stop_result == NUWAX_SESSION_PLAYBACK_STOP_NATURAL_DRAINING) {
            ESP_LOGI(TAG,
                     "stop_playback treated as idempotent natural drain turn=%s playback=%s",
                     command->turn_id,
                     command->playback_id);
            send_command_ack(ws, command, true, "already draining");
            return;
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
            ESP_LOGI(TAG,
                     "stop_playback preserved active during-playback capture while physical stop converges");
        }
        send_command_ack(ws,
                         command,
                         true,
                         preserve_capture ? "stopping capture_preserved=1"
                                          : "stopping");
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

    if (strcmp(command->command, "set_wake_word") == 0) {
        if (command->wake_word_invalid ||
            (command->wake_word_is_array
                 ? command->wake_word_config.count == 0U
                 : command->wake_word_config.count != 1U ||
                       command->wake_word_config.variants[0][0] == '\0')) {
            send_command_ack(ws,
                             command,
                             false,
                             "wake_word must be a non-empty ASCII pinyin string or array");
            return;
        }
        if (!callbacks ||
            (command->wake_word_is_array
                 ? callbacks->set_wake_words == NULL
                 : callbacks->set_wake_word == NULL)) {
            send_command_ack(ws, command, false, "set_wake_word unsupported");
            return;
        }
        const bool persist = command->has_wake_word_persist
                                 ? command->wake_word_persist
                                 : true;
        nuwax_wake_word_config_t effective_config =
            command->wake_word_config;
        if (command->wake_word_is_array &&
            !nuwax_wake_word_config_normalize(
                &command->wake_word_config,
                &effective_config)) {
            send_command_ack(ws,
                             command,
                             false,
                             "invalid retained wake_word; use 2..63 ASCII pinyin strings");
            return;
        }
        const esp_err_t err = command->wake_word_is_array
                                  ? callbacks->set_wake_words(
                                        callbacks->ctx,
                                        &effective_config,
                                        persist)
                                  : callbacks->set_wake_word(callbacks->ctx,
                                                             command->wake_word_config.variants[0],
                                                             persist);
        char message[128];
        if (err == ESP_OK) {
            if (command->wake_word_is_array) {
                snprintf(message,
                         sizeof(message),
                         "wake_words=%u primary=%s persist=%d truncated=%d",
                         (unsigned)effective_config.count,
                         effective_config.variants[0],
                         persist ? 1 : 0,
                         command->wake_word_truncated ? 1 : 0);
            } else {
                snprintf(message,
                         sizeof(message),
                         "wake_word=%s persist=%d",
                         command->wake_word_config.variants[0],
                         persist ? 1 : 0);
            }
        } else if (err == ESP_ERR_INVALID_ARG) {
            snprintf(message,
                     sizeof(message),
                     "invalid retained wake_word; use 2..63 ASCII pinyin strings");
        } else {
            snprintf(message,
                     sizeof(message),
                     "set_wake_word failed err=%s",
                     esp_err_to_name(err));
        }
        send_command_ack(ws, command, err == ESP_OK, message);
        return;
    }

    if (strcmp(command->command, "reset_wake_word") == 0) {
        if (!callbacks || !callbacks->reset_wake_word) {
            send_command_ack(ws,
                             command,
                             false,
                             "reset_wake_word unsupported");
            return;
        }
        const esp_err_t err =
            callbacks->reset_wake_word(callbacks->ctx);
        char message[96];
        if (err == ESP_OK) {
            snprintf(message,
                     sizeof(message),
                     "wake_words=10 source=factory_default");
        } else {
            snprintf(message,
                     sizeof(message),
                     "reset_wake_word failed err=%s",
                     esp_err_to_name(err));
        }
        send_command_ack(ws, command, err == ESP_OK, message);
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
