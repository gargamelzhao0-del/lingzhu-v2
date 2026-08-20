#include "meeting_mode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "audio_frame.h"
#include "conversation/audio_terminal_recovery.h"
#include "meeting_protocol.h"
#include "session_protocol.h"

static const char *TAG = "nuwax_meeting";

#ifndef CONFIG_NUWAX_VOICE_SAMPLE_RATE
#define CONFIG_NUWAX_VOICE_SAMPLE_RATE 16000
#endif
#ifndef CONFIG_NUWAX_VOICE_FRAME_DURATION_MS
#define CONFIG_NUWAX_VOICE_FRAME_DURATION_MS 20
#endif
#ifndef CONFIG_NUWAX_VOICE_MAX_SAMPLES_PER_FRAME
#define CONFIG_NUWAX_VOICE_MAX_SAMPLES_PER_FRAME 960
#endif
#ifndef CONFIG_NUWAX_VOICE_UPLINK_CODEC_OPUS
#define CONFIG_NUWAX_VOICE_UPLINK_CODEC_OPUS 0
#endif
#ifndef CONFIG_NUWAX_VOICE_CHANNELS
#define CONFIG_NUWAX_VOICE_CHANNELS 1
#endif
#ifndef CONFIG_NUWAX_MIC_READ_TIMEOUT_MS
#define CONFIG_NUWAX_MIC_READ_TIMEOUT_MS 100
#endif

#define NUWAX_MEETING_EVENT_QUEUE_DEPTH 8U
#define NUWAX_MEETING_ACK_QUEUE_DEPTH 2U
#define NUWAX_MEETING_START_TIMEOUT_MS 5000U
#define NUWAX_MEETING_NO_SPEECH_TIMEOUT_MS 0U
#define NUWAX_MEETING_PLAYBACK_FADE_MS 200U
#define NUWAX_MEETING_PLAYBACK_SETTLE_TIMEOUT_MS 2000U
#define NUWAX_MEETING_READ_STALL_TIMEOUT_MS 5000U
#define NUWAX_MEETING_LOG_INTERVAL_FRAMES 250U
#define NUWAX_MEETING_DISCONTINUITY_LOG_INTERVAL 100U
#define NUWAX_MEETING_CONTROL_SEND_TIMEOUT_US 1000000LL
#define NUWAX_MEETING_COMMAND_ACK_RETRY_INTERVAL_MS 500U
#define NUWAX_MEETING_COMMAND_ACK_MAX_ATTEMPTS 5U

#define NUWAX_MEETING_FRAME_SAMPLES \
    ((CONFIG_NUWAX_VOICE_SAMPLE_RATE * CONFIG_NUWAX_VOICE_FRAME_DURATION_MS) / 1000)

typedef struct {
    int16_t *samples;
    size_t sample_count;
    uint32_t sequence;
    nuwax_meeting_frame_features_t features;
} meeting_pending_frame_t;

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static bool deadline_expired(uint32_t now, uint32_t deadline) {
    return (int32_t)(now - deadline) >= 0;
}

static void copy_text(char *dst, size_t size, const char *src) {
    if (!dst || size == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    /* Copy with an explicit bounded length so GCC does not treat a full-size
     * protocol field as an accidental truncating strncpy. */
    size_t copy_len = 0;
    while (copy_len + 1U < size && src[copy_len] != '\0') {
        copy_len++;
    }
    memcpy(dst, src, copy_len);
    dst[copy_len] = '\0';
}

static const char *state_name(nuwax_meeting_state_t state) {
    switch (state) {
        case NUWAX_MEETING_STATE_DISABLED:
            return "disabled";
        case NUWAX_MEETING_STATE_STARTING:
            return "starting";
        case NUWAX_MEETING_STATE_ACTIVE:
            return "active";
        case NUWAX_MEETING_STATE_STOPPING:
            return "stopping";
        case NUWAX_MEETING_STATE_ERROR:
            return "error";
        default:
            return "unknown";
    }
}

static bool can_start(const nuwax_meeting_mode_t *mode) {
    return !mode || !mode->callbacks.can_start ||
           mode->callbacks.can_start(mode->callbacks.ctx);
}

static void set_state(nuwax_meeting_mode_t *mode,
                      nuwax_meeting_state_t state,
                      const char *reason) {
    if (!mode) {
        return;
    }
    const bool changed = mode->state != state;
    if (changed) {
        ESP_LOGI(TAG,
                 "state %s -> %s reason=%s",
                 state_name(mode->state),
                 state_name(state),
                 reason ? reason : "-");
    }
    mode->state = state;
    if (changed && mode->callbacks.on_state_changed) {
        mode->callbacks.on_state_changed(mode->callbacks.ctx, state, reason);
    }
}

static void make_request_id(nuwax_meeting_mode_t *mode,
                            const char *prefix,
                            char out[NUWAX_MEETING_REQUEST_ID_BYTES]) {
    const uint32_t seq = ++mode->request_sequence;
    const uint32_t salt = esp_random();
    snprintf(out,
             NUWAX_MEETING_REQUEST_ID_BYTES,
             "meeting_%s_%lu_%08lx",
             prefix ? prefix : "request",
             (unsigned long)seq,
             (unsigned long)salt);
}

static void make_audio_session_id(nuwax_meeting_mode_t *mode) {
    if (mode->audio_session_id[0] != '\0') {
        return;
    }
    snprintf(mode->audio_session_id,
             sizeof(mode->audio_session_id),
             "meeting_stream_%lu_%08lx",
             (unsigned long)mode->request_sequence,
             (unsigned long)esp_random());
}

static void complete_queued_enable_event(
    nuwax_meeting_mode_t *mode,
    const nuwax_meeting_event_t *event) {
    if (!mode || !event || event->type != NUWAX_MEETING_EVENT_CLOUD_ENABLE) {
        return;
    }
    uint32_t pending = atomic_load_explicit(&mode->queued_enable_events,
                                            memory_order_acquire);
    while (pending > 0U) {
        if (atomic_compare_exchange_weak_explicit(&mode->queued_enable_events,
                                                  &pending,
                                                  pending - 1U,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            return;
        }
    }
    ESP_LOGE(TAG, "meeting enable queue accounting underflow");
}

static bool queue_event(nuwax_meeting_mode_t *mode,
                         const nuwax_meeting_event_t *event) {
    if (!mode || !event) {
        return false;
    }
    if (event->type == NUWAX_MEETING_EVENT_REQUEST_ACK) {
        if (!mode->request_acks ||
            xQueueSend(mode->request_acks, event, 0) != pdTRUE) {
            ESP_LOGE(TAG, "meeting request ACK queue unavailable or full");
            return false;
        }
        return true;
    }
    if (!mode->events) {
        return false;
    }
    if (event->type == NUWAX_MEETING_EVENT_CLOUD_ENABLE) {
        atomic_fetch_add_explicit(&mode->queued_enable_events,
                                  1U,
                                  memory_order_acq_rel);
    }
    if (xQueueSend(mode->events, event, 0) != pdTRUE) {
        complete_queued_enable_event(mode, event);
        ESP_LOGW(TAG, "meeting control event queue full type=%d", (int)event->type);
        return false;
    }
    return true;
}

static bool event_matches_connection(const nuwax_meeting_event_t *event,
                                     const nuwax_ws_client_t *ws) {
    if (!event || !ws) {
        return false;
    }
    const uint32_t active_epoch = nuwax_ws_connection_epoch(ws);
    if (event->ws_connection_epoch == 0U || active_epoch == 0U ||
        event->ws_connection_epoch != active_epoch) {
        ESP_LOGW(TAG,
                 "stale meeting event ignored type=%d request=%s event_epoch=%lu active_epoch=%lu",
                 (int)event->type,
                 event->request_id[0] ? event->request_id : "-",
                 (unsigned long)event->ws_connection_epoch,
                 (unsigned long)active_epoch);
        return false;
    }
    return true;
}

static void clear_event_queue(nuwax_meeting_mode_t *mode) {
    if (!mode) {
        return;
    }
    nuwax_meeting_event_t event;
    if (mode->request_acks) {
        while (xQueueReceive(mode->request_acks, &event, 0) == pdTRUE) {
        }
    }
    if (mode->events) {
        while (xQueueReceive(mode->events, &event, 0) == pdTRUE) {
            complete_queued_enable_event(mode, &event);
        }
    }
}

static void reset_transient_state(nuwax_meeting_mode_t *mode) {
    if (!mode) {
        return;
    }
    mode->capture_running = false;
    mode->cloud_authorized = false;
    mode->pending_command_ack = false;
    mode->pending_command_enabled = false;
    mode->pending_ui_close = false;
    mode->interrupt_current_turn = false;
    mode->start_deadline_ms = 0;
    mode->pending_command_ack_attempts = 0U;
    mode->pending_command_ack_deadline_ms = 0U;
    mode->capture_frames = 0;
    mode->uploaded_frames = 0;
    mode->dropped_frames = 0;
    mode->consecutive_read_timeouts = 0;
    mode->pending_request_id[0] = '\0';
    mode->pending_command_request_id[0] = '\0';
    mode->audio_session_id[0] = '\0';
    mode->stop_reason[0] = '\0';
    if (mode->processor.initialized) {
        nuwax_meeting_processor_reset(&mode->processor);
    }
    nuwax_meeting_inactivity_reset(&mode->inactivity);
}

static void reset_to_disabled(nuwax_meeting_mode_t *mode,
                              const char *reason,
                              bool report_error) {
    if (!mode) {
        return;
    }
    if (report_error) {
        set_state(mode, NUWAX_MEETING_STATE_ERROR, reason);
    }
    reset_transient_state(mode);
    set_state(mode, NUWAX_MEETING_STATE_DISABLED, reason);
}

static void set_stop_reason(nuwax_meeting_mode_t *mode, const char *reason) {
    copy_text(mode->stop_reason, sizeof(mode->stop_reason), reason ? reason : "local_stop");
}

static void begin_local_stop(nuwax_meeting_mode_t *mode, const char *reason) {
    if (!mode || mode->state == NUWAX_MEETING_STATE_DISABLED) {
        return;
    }
    set_stop_reason(mode, reason);
    /* A cloud command has its own ACK path.  A local stop needs the
     * audio_mode_request close message, but must not emit a second control
     * request while completing a cloud-initiated command. */
    mode->pending_ui_close = !mode->pending_command_ack;
    mode->cloud_authorized = false;
    set_state(mode, NUWAX_MEETING_STATE_STOPPING, reason);
}

static void begin_cloud_stop(nuwax_meeting_mode_t *mode,
                             const nuwax_meeting_event_t *event) {
    if (!mode) {
        return;
    }
    set_stop_reason(mode, event && event->reason[0] ? event->reason : "cloud_stop");
    mode->pending_ui_close = false;
    mode->pending_command_ack = event && event->request_id[0] != '\0';
    mode->pending_command_enabled = false;
    if (event) {
        copy_text(mode->pending_command_request_id,
                  sizeof(mode->pending_command_request_id),
                  event->request_id);
    }
    set_state(mode, NUWAX_MEETING_STATE_STOPPING, mode->stop_reason);
}

static void clear_pending_command_ack(nuwax_meeting_mode_t *mode) {
    if (!mode) {
        return;
    }
    mode->pending_command_ack = false;
    mode->pending_command_enabled = false;
    mode->pending_command_ack_attempts = 0U;
    mode->pending_command_ack_deadline_ms = 0U;
    mode->pending_command_request_id[0] = '\0';
}

static void try_send_active_command_ack(nuwax_meeting_mode_t *mode,
                                        nuwax_ws_client_t *ws,
                                        bool force) {
    if (!mode || !ws || mode->state != NUWAX_MEETING_STATE_ACTIVE ||
        !mode->pending_command_ack || !mode->pending_command_enabled ||
        mode->pending_command_request_id[0] == '\0' ||
        !nuwax_ws_is_connected(ws)) {
        return;
    }
    const uint32_t now = now_ms();
    if (!force &&
        !deadline_expired(now, mode->pending_command_ack_deadline_ms)) {
        return;
    }

    mode->pending_command_ack_attempts++;
    const esp_err_t ack_err = nuwax_meeting_protocol_send_command_ack(
        ws,
        mode->pending_command_request_id,
        true,
        true,
        "active");
    if (ack_err == ESP_OK) {
        ESP_LOGI(TAG,
                 "meeting active command ACK delivered request=%s attempts=%lu",
                 mode->pending_command_request_id,
                 (unsigned long)mode->pending_command_ack_attempts);
        clear_pending_command_ack(mode);
        return;
    }

    if (mode->pending_command_ack_attempts >=
        NUWAX_MEETING_COMMAND_ACK_MAX_ATTEMPTS) {
        ESP_LOGE(TAG,
                 "meeting active command ACK exhausted request=%s attempts=%lu err=%s; capture retained",
                 mode->pending_command_request_id,
                 (unsigned long)mode->pending_command_ack_attempts,
                 esp_err_to_name(ack_err));
        clear_pending_command_ack(mode);
        return;
    }

    mode->pending_command_ack_deadline_ms =
        now + NUWAX_MEETING_COMMAND_ACK_RETRY_INTERVAL_MS;
    ESP_LOGW(TAG,
             "meeting active command ACK retry scheduled request=%s attempt=%lu/%u err=%s",
             mode->pending_command_request_id,
             (unsigned long)mode->pending_command_ack_attempts,
             (unsigned int)NUWAX_MEETING_COMMAND_ACK_MAX_ATTEMPTS,
             esp_err_to_name(ack_err));
}

static void handle_event(nuwax_meeting_mode_t *mode,
                         nuwax_ws_client_t *ws,
                         const nuwax_meeting_event_t *event) {
    if (!mode || !event) {
        return;
    }

    if (event->type == NUWAX_MEETING_EVENT_REQUEST_ACK) {
        if (mode->state != NUWAX_MEETING_STATE_STARTING ||
            strcmp(event->request_id, mode->pending_request_id) != 0) {
            ESP_LOGW(TAG,
                     "stale meeting request ack ignored request=%s expected=%s state=%s",
                     event->request_id,
                     mode->pending_request_id,
                     state_name(mode->state));
            return;
        }
        if (!event->ok || strcmp(event->state, "enabled") != 0) {
            ESP_LOGW(TAG,
                     "meeting enable rejected request=%s reason=%s",
                     event->request_id,
                     event->reason[0] ? event->reason : "cloud_rejected");
            reset_to_disabled(mode, "cloud_rejected", true);
            return;
        }
        mode->cloud_authorized = true;
        if (event->audio_session_id[0] != '\0') {
            copy_text(mode->audio_session_id,
                      sizeof(mode->audio_session_id),
                      event->audio_session_id);
        }
        ESP_LOGI(TAG,
                 "meeting enable authorized request=%s audio_session=%s current_turn_cancelled=%d",
                 event->request_id,
                 mode->audio_session_id[0] ? mode->audio_session_id : "-",
                 event->current_turn_cancelled ? 1 : 0);
        return;
    }

    if (event->type != NUWAX_MEETING_EVENT_CLOUD_ENABLE &&
        event->type != NUWAX_MEETING_EVENT_CLOUD_DISABLE) {
        return;
    }

    if (event->type == NUWAX_MEETING_EVENT_CLOUD_ENABLE) {
        if (mode->state == NUWAX_MEETING_STATE_ACTIVE) {
            if (ws && event->request_id[0]) {
                const esp_err_t ack_err =
                    nuwax_meeting_protocol_send_command_ack(ws,
                                                            event->request_id,
                                                            true,
                                                            true,
                                                            "already_active");
                if (ack_err == ESP_OK && mode->pending_command_ack &&
                    mode->pending_command_enabled &&
                    strcmp(event->request_id,
                           mode->pending_command_request_id) == 0) {
                    clear_pending_command_ack(mode);
                }
            }
            return;
        }
        if (mode->state != NUWAX_MEETING_STATE_DISABLED) {
            if (mode->state == NUWAX_MEETING_STATE_STARTING &&
                mode->pending_command_enabled &&
                strcmp(event->request_id, mode->pending_command_request_id) == 0) {
                /* Cloud retries of the same enable command are idempotent;
                 * keep the original activation/ACK path in flight. */
                ESP_LOGI(TAG,
                         "duplicate cloud meeting enable ignored request=%s",
                         event->request_id);
                return;
            }
            if (ws && event->request_id[0] != '\0') {
                (void)nuwax_meeting_protocol_send_command_ack(ws,
                                                              event->request_id,
                                                              false,
                                                              false,
                                                              "meeting_busy");
            }
            ESP_LOGW(TAG, "cloud meeting enable rejected while state=%s", state_name(mode->state));
            return;
        }
        if (!can_start(mode)) {
            if (ws && event->request_id[0] != '\0') {
                (void)nuwax_meeting_protocol_send_command_ack(ws,
                                                              event->request_id,
                                                              false,
                                                              false,
                                                              "audio_mode_busy");
            }
            ESP_LOGW(TAG,
                     "cloud meeting enable rejected because another audio mode owns the transition");
            return;
        }
        reset_transient_state(mode);
        mode->pending_command_ack = event->request_id[0] != '\0';
        mode->pending_command_enabled = true;
        copy_text(mode->pending_command_request_id,
                  sizeof(mode->pending_command_request_id),
                  event->request_id);
        /* Reuse the command id for listen-start correlation so cloud logs can
         * join the control and audio-session messages without another ID. */
        copy_text(mode->pending_request_id,
                  sizeof(mode->pending_request_id),
                  event->request_id);
        copy_text(mode->audio_session_id,
                  sizeof(mode->audio_session_id),
                  event->audio_session_id);
        mode->cloud_authorized = true;
        mode->interrupt_current_turn = event->interrupt_current_turn ||
                                        event->current_turn_cancelled;
        set_state(mode, NUWAX_MEETING_STATE_STARTING, "cloud_enable");
        if (mode->callbacks.is_playback_active &&
            mode->callbacks.is_playback_active(mode->callbacks.ctx) &&
            mode->callbacks.fade_and_cancel_playback) {
            mode->callbacks.fade_and_cancel_playback(mode->callbacks.ctx,
                                                     NUWAX_MEETING_PLAYBACK_FADE_MS,
                                                     "meeting_cloud_enable");
        }
        return;
    }

    if (mode->state == NUWAX_MEETING_STATE_DISABLED) {
        if (ws && event->request_id[0]) {
            (void)nuwax_meeting_protocol_send_command_ack(ws,
                                                          event->request_id,
                                                          false,
                                                          true,
                                                          "already_disabled");
        }
        return;
    }
    if (mode->state == NUWAX_MEETING_STATE_STOPPING &&
        event->request_id[0] != '\0' &&
        strcmp(event->request_id, mode->pending_command_request_id) == 0) {
        ESP_LOGI(TAG,
                 "duplicate cloud meeting disable ignored request=%s",
                 event->request_id);
        return;
    }
    if (mode->state == NUWAX_MEETING_STATE_STOPPING) {
        if (ws && event->request_id[0] != '\0') {
            (void)nuwax_meeting_protocol_send_command_ack(ws,
                                                          event->request_id,
                                                          false,
                                                          false,
                                                          "meeting_busy");
        }
        return;
    }
    if (mode->pending_command_ack && mode->pending_command_enabled &&
        mode->pending_command_request_id[0] != '\0' && ws &&
        nuwax_ws_is_connected(ws)) {
        esp_err_t ack_err = nuwax_meeting_protocol_send_command_ack(
            ws,
            mode->pending_command_request_id,
            false,
            false,
            "superseded_by_disable");
        if (ack_err != ESP_OK) {
            ESP_LOGW(TAG,
                     "meeting superseded enable ACK failed request=%s err=%s",
                     mode->pending_command_request_id,
                     esp_err_to_name(ack_err));
        }
    }
    begin_cloud_stop(mode, event);
}

static void drain_events(nuwax_meeting_mode_t *mode, nuwax_ws_client_t *ws) {
    if (!mode) {
        return;
    }
    nuwax_meeting_event_t event;
    if (mode->request_acks) {
        while (xQueueReceive(mode->request_acks, &event, 0) == pdTRUE) {
            if (!event_matches_connection(&event, ws)) {
                continue;
            }
            handle_event(mode, ws, &event);
        }
    }
    if (mode->events) {
        while (xQueueReceive(mode->events, &event, 0) == pdTRUE) {
            if (event_matches_connection(&event, ws)) {
                handle_event(mode, ws, &event);
            }
            complete_queued_enable_event(mode, &event);
        }
    }
}

static void send_ui_close_request(nuwax_meeting_mode_t *mode,
                                  nuwax_ws_client_t *ws) {
    if (!mode || !ws || !nuwax_ws_is_connected(ws) || !mode->pending_ui_close) {
        return;
    }
    char request_id[NUWAX_MEETING_REQUEST_ID_BYTES] = {0};
    make_request_id(mode, "close", request_id);
    esp_err_t err = nuwax_meeting_protocol_send_request(ws,
                                                        request_id,
                                                        false,
                                                        false,
                                                        mode->stop_reason);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "meeting close request send failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG,
                 "meeting close request sent request=%s reason=%s",
                 request_id,
                 mode->stop_reason[0] ? mode->stop_reason : "-");
    }
    mode->pending_ui_close = false;
}

static const char *start_failure_message(const nuwax_meeting_mode_t *mode) {
    if (mode && strcmp(mode->stop_reason, "playback_stop_failed") == 0) {
        return "playback_stop_failed";
    }
    return "capture_failed";
}

static void finish_state(nuwax_meeting_mode_t *mode,
                          nuwax_ws_client_t *ws,
                          bool capture_ok) {
    if (!mode) {
        return;
    }
    if (mode->pending_ui_close) {
        send_ui_close_request(mode, ws);
    }
    if (mode->pending_command_ack && mode->pending_command_request_id[0] != '\0' && ws &&
        nuwax_ws_is_connected(ws)) {
        /* An enable command is acknowledged successfully as soon as capture
         * becomes ACTIVE.  If it is still pending here, activation was
         * cancelled or failed.  A disable command succeeds only after the
         * capture/uplink teardown above completed cleanly. */
        const bool command_ok = !mode->pending_command_enabled && capture_ok;
        const char *message = mode->pending_command_enabled
                                  ? (capture_ok ? "start_cancelled"
                                                : start_failure_message(mode))
                                  : (capture_ok ? "disabled" : "capture_failed");
        esp_err_t ack_err = nuwax_meeting_protocol_send_command_ack(
            ws,
            mode->pending_command_request_id,
            false,
            command_ok,
            message);
        if (ack_err != ESP_OK) {
            ESP_LOGW(TAG,
                     "meeting final command ACK failed request=%s err=%s",
                     mode->pending_command_request_id,
                     esp_err_to_name(ack_err));
        }
    }
    char final_reason[NUWAX_MEETING_REASON_BYTES] = {0};
    copy_text(final_reason,
              sizeof(final_reason),
              mode->stop_reason[0] ? mode->stop_reason : "capture_done");
    reset_transient_state(mode);
    set_state(mode,
              capture_ok ? NUWAX_MEETING_STATE_DISABLED : NUWAX_MEETING_STATE_ERROR,
              final_reason);
    if (!capture_ok) {
        /* An error is reported locally, then the controller becomes reusable. */
        set_state(mode, NUWAX_MEETING_STATE_DISABLED, "capture_error_reset");
    }
}

static esp_err_t fail_capture_start(nuwax_meeting_mode_t *mode,
                                     nuwax_ws_client_t *ws,
                                     esp_err_t err,
                                     const char *reason) {
    if (!mode) {
        return err;
    }
    set_stop_reason(mode, reason ? reason : "capture_start_failed");
    /* Reuse the common teardown path so pending cloud commands receive a
     * negative ACK and the controller cannot remain stuck in STARTING. */
    finish_state(mode, ws, false);
    return err;
}

static void *meeting_alloc(size_t bytes) {
    void *ptr = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) {
        ptr = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    }
    return ptr;
}

static void meeting_free(void *ptr) {
    if (ptr) {
        heap_caps_free(ptr);
    }
}

static uint8_t frame_flags(const nuwax_meeting_frame_features_t *features,
                           bool first,
                           bool final) {
    uint8_t flags = 0;
    if (first) {
        flags |= NUWAX_AUDIO_FLAG_START;
    }
    if (final) {
        flags |= NUWAX_AUDIO_FLAG_END;
    }
    if (features && features->speech) {
        flags |= NUWAX_AUDIO_FLAG_VAD_SPEECH;
    }
    return flags;
}

static esp_err_t capture_session(nuwax_meeting_mode_t *mode,
                                 nuwax_ws_client_t *ws) {
#if !CONFIG_NUWAX_AUDIO_SOURCE_I2S_MIC
    ESP_LOGW(TAG, "meeting capture requires CONFIG_NUWAX_AUDIO_SOURCE_I2S_MIC");
    return fail_capture_start(mode, ws, ESP_ERR_NOT_SUPPORTED, "i2s_mic_disabled");
#else
    if (!mode || !ws || !nuwax_ws_is_connected(ws)) {
        return fail_capture_start(mode, ws, ESP_ERR_INVALID_STATE, "capture_invalid_state");
    }
    const uint32_t ws_epoch = nuwax_ws_connection_epoch(ws);
    if (ws_epoch == 0U) {
        return fail_capture_start(mode,
                                  ws,
                                  ESP_ERR_INVALID_STATE,
                                  "capture_invalid_epoch");
    }
    const size_t frame_samples = NUWAX_MEETING_FRAME_SAMPLES;
    if (frame_samples == 0 || frame_samples > CONFIG_NUWAX_VOICE_MAX_SAMPLES_PER_FRAME) {
        return fail_capture_start(mode, ws, ESP_ERR_INVALID_SIZE, "frame_size_invalid");
    }

    /* The mode transition requests a non-blocking fade so the UI remains
     * responsive.  Do not open the meeting uplink until that fade has drained
     * the speaker queue, otherwise the first meeting frames can still contain
     * the previous TTS burst. */
    if (mode->callbacks.is_playback_active) {
        const uint32_t settle_deadline = now_ms() +
                                         NUWAX_MEETING_PLAYBACK_SETTLE_TIMEOUT_MS;
        while (mode->callbacks.is_playback_active(mode->callbacks.ctx) &&
               !deadline_expired(now_ms(), settle_deadline) &&
               nuwax_ws_is_connected(ws)) {
            if (mode->callbacks.poll_controls) {
                mode->callbacks.poll_controls(mode->callbacks.ctx, ws);
            }
            drain_events(mode, ws);
            if (mode->state == NUWAX_MEETING_STATE_STOPPING ||
                mode->state == NUWAX_MEETING_STATE_DISABLED) {
                finish_state(mode, ws, true);
                return ESP_OK;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (mode->callbacks.is_playback_active(mode->callbacks.ctx)) {
            ESP_LOGE(TAG,
                     "meeting playback did not stop before capture deadline");
            return fail_capture_start(mode,
                                      ws,
                                      ESP_ERR_TIMEOUT,
                                      "playback_stop_failed");
        }
    }

    esp_err_t err = ESP_OK;
    nuwax_mic_input_t mic = {0};
    nuwax_audio_uplink_t *uplink = nuwax_audio_uplink_create();
    int16_t *current = (int16_t *)meeting_alloc(frame_samples * sizeof(int16_t));
    int16_t *pending = (int16_t *)meeting_alloc(frame_samples * sizeof(int16_t));
    bool mic_ready = false;
    bool uplink_ready = false;
    bool listen_started = false;
    bool end_committed = false;
    bool have_pending = false;
    size_t current_samples = 0U;
    uint32_t read_discontinuities = 0U;
    uint32_t consecutive_read_discontinuities = 0U;
    uint64_t discarded_partial_samples = 0U;
    meeting_pending_frame_t pending_frame = {
        .samples = pending,
    };
#if CONFIG_NUWAX_VOICE_UPLINK_CODEC_OPUS
    nuwax_opus_encoder_t opus_encoder = {0};
    bool opus_ready = false;
#endif

    if (!uplink || !current || !pending) {
        err = ESP_ERR_NO_MEM;
        set_stop_reason(mode, "capture_buffer_alloc_failed");
        goto cleanup;
    }

    err = nuwax_mic_input_init_with_owner(&mic, NUWAX_MIC_OWNER_MEETING_CAPTURE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "meeting mic init failed: %s", esp_err_to_name(err));
        goto cleanup;
    }
    mic_ready = true;

#if CONFIG_NUWAX_VOICE_UPLINK_CODEC_OPUS
    err = nuwax_opus_encoder_init(&opus_encoder,
                                  CONFIG_NUWAX_VOICE_SAMPLE_RATE,
                                  CONFIG_NUWAX_VOICE_CHANNELS,
                                  CONFIG_NUWAX_VOICE_FRAME_DURATION_MS);
    if (err != ESP_OK) {
        set_stop_reason(mode, "opus_init_failed");
        goto cleanup;
    }
    opus_ready = true;
    if (nuwax_opus_encoder_frame_samples(&opus_encoder) != frame_samples) {
        err = ESP_ERR_INVALID_SIZE;
        set_stop_reason(mode, "opus_frame_size_invalid");
        goto cleanup;
    }
    err = nuwax_audio_uplink_ensure_encoded_frame_capacity(
        nuwax_opus_encoder_output_bytes(&opus_encoder));
    if (err != ESP_OK) {
        set_stop_reason(mode, "opus_buffer_failed");
        goto cleanup;
    }
#endif

    err = nuwax_audio_uplink_start_if_epoch(uplink, ws, ws_epoch);
    if (err != ESP_OK) {
        set_stop_reason(mode, "uplink_start_failed");
        goto cleanup;
    }
    uplink_ready = true;
    make_audio_session_id(mode);
    err = nuwax_audio_uplink_bind_terminal_recovery(
        uplink,
        "meeting",
        mode->audio_session_id,
        NULL,
        NUWAX_AUDIO_TERMINAL_RECOVERY_DEFAULT_ATTEMPT_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "meeting terminal recovery binding failed before listen start session=%s err=%s",
                 mode->audio_session_id,
                 esp_err_to_name(err));
        const esp_err_t discard_err =
            nuwax_audio_uplink_discard_uncommitted(uplink);
        if (discard_err != ESP_OK) {
            ESP_LOGE(TAG,
                     "meeting prepared uplink cleanup failed after recovery bind error: %s",
                     esp_err_to_name(discard_err));
        }
        uplink_ready = false;
        set_stop_reason(mode, "terminal_recovery_bind_failed");
        goto cleanup;
    }
    err = nuwax_session_send_listen_with_audio_session_until_if_epoch(
        ws,
        "start",
        "meeting",
        mode->audio_session_id,
        mode->pending_request_id[0] ? mode->pending_request_id : NULL,
        esp_timer_get_time() + NUWAX_MEETING_CONTROL_SEND_TIMEOUT_US,
        ws_epoch);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "meeting listen start failed: %s", esp_err_to_name(err));
        const esp_err_t discard_err =
            nuwax_audio_uplink_discard_uncommitted(uplink);
        if (discard_err != ESP_OK) {
            (void)nuwax_ws_request_close_if_epoch(ws, ws_epoch);
            ESP_LOGE(TAG,
                     "meeting uncommitted uplink could not be discarded: %s",
                     esp_err_to_name(discard_err));
            /* The sender is heap-owned; detach it for asynchronous cleanup
             * instead of unwinding a live task or restarting the device. */
            (void)nuwax_audio_uplink_destroy_owned(&uplink);
        }
        if (uplink) {
            (void)nuwax_audio_uplink_destroy_owned(&uplink);
        }
        uplink_ready = false;
        set_stop_reason(mode, "listen_start_failed");
        goto cleanup;
    }
    listen_started = true;

    nuwax_meeting_processor_reset(&mode->processor);
    nuwax_meeting_inactivity_reset(&mode->inactivity);
    mode->capture_running = true;
    set_state(mode, NUWAX_MEETING_STATE_ACTIVE, "capture_started");
    try_send_active_command_ack(mode, ws, true);

    uint32_t last_successful_read_ms = now_ms();
    while (nuwax_ws_is_connected(ws) && mode->state != NUWAX_MEETING_STATE_STOPPING) {
        if (mode->callbacks.poll_controls) {
            mode->callbacks.poll_controls(mode->callbacks.ctx, ws);
        }
        drain_events(mode, ws);
        if (mode->state == NUWAX_MEETING_STATE_STOPPING) {
            break;
        }
        try_send_active_command_ack(mode, ws, false);

        const size_t samples_remaining = frame_samples - current_samples;
        size_t samples_read = 0U;
        esp_err_t read_err = nuwax_mic_input_read_pcm16(&mic,
                                                        &current[current_samples],
                                                        samples_remaining,
                                                        CONFIG_NUWAX_MIC_READ_TIMEOUT_MS,
                                                        &samples_read);
        if (samples_read > samples_remaining) {
            set_stop_reason(mode, "mic_frame_too_large");
            begin_local_stop(mode, "mic_frame_too_large");
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        if (read_err == ESP_ERR_INVALID_RESPONSE && samples_read == 0U) {
            read_discontinuities++;
            consecutive_read_discontinuities++;
            discarded_partial_samples += current_samples;
            current_samples = 0U;
            nuwax_meeting_processor_reset(&mode->processor);
            mode->inactivity.speech_activity_ms = 0U;
            mode->inactivity.sound_activity_ms = 0U;
            mode->consecutive_read_timeouts++;

            const uint32_t stalled_ms =
                (uint32_t)(now_ms() - last_successful_read_ms);
            if (read_discontinuities == 1U ||
                (read_discontinuities %
                 NUWAX_MEETING_DISCONTINUITY_LOG_INTERVAL) == 0U) {
                ESP_LOGW(TAG,
                         "meeting microphone discontinuity recovered without leaving mode events=%lu consecutive=%lu discarded_partial_samples=%llu stalled_ms=%lu",
                         (unsigned long)read_discontinuities,
                         (unsigned long)consecutive_read_discontinuities,
                         (unsigned long long)discarded_partial_samples,
                         (unsigned long)stalled_ms);
            }
            if (stalled_ms >= NUWAX_MEETING_READ_STALL_TIMEOUT_MS) {
                set_stop_reason(mode, "mic_read_discontinuity_stall");
                begin_local_stop(mode, "mic_read_discontinuity_stall");
                err = ESP_ERR_TIMEOUT;
                break;
            }
            continue;
        }
        if (read_err != ESP_OK && read_err != ESP_ERR_TIMEOUT) {
            set_stop_reason(mode, "mic_read_failed");
            begin_local_stop(mode, "mic_read_failed");
            err = read_err;
            break;
        }
        if (samples_read == 0) {
            mode->consecutive_read_timeouts++;
            if ((uint32_t)(now_ms() - last_successful_read_ms) >=
                NUWAX_MEETING_READ_STALL_TIMEOUT_MS) {
                set_stop_reason(mode, "mic_read_timeout");
                begin_local_stop(mode, "mic_read_timeout");
                err = read_err == ESP_OK ? ESP_ERR_TIMEOUT : read_err;
                break;
            }
            continue;
        }
        mode->consecutive_read_timeouts = 0;
        if (consecutive_read_discontinuities > 0U) {
            ESP_LOGI(TAG,
                     "meeting microphone stream recovered consecutive_discontinuities=%lu total_discontinuities=%lu",
                     (unsigned long)consecutive_read_discontinuities,
                     (unsigned long)read_discontinuities);
            consecutive_read_discontinuities = 0U;
        }
        last_successful_read_ms = now_ms();
        current_samples += samples_read;
        if (current_samples < frame_samples) {
            continue;
        }

        samples_read = current_samples;
        current_samples = 0U;
        nuwax_meeting_frame_features_t features = {0};
        nuwax_meeting_processor_process(&mode->processor,
                                        current,
                                        samples_read,
                                        true,
                                        &features);

        (void)nuwax_meeting_inactivity_update(
            &mode->inactivity,
            &features,
            CONFIG_NUWAX_VOICE_FRAME_DURATION_MS,
            NUWAX_MEETING_NO_SPEECH_TIMEOUT_MS);

        if (have_pending) {
            const uint8_t flags = frame_flags(&pending_frame.features,
                                              pending_frame.sequence == 0U,
                                              false);
            esp_err_t enqueue_err = nuwax_audio_uplink_enqueue_frame(
                uplink,
#if CONFIG_NUWAX_VOICE_UPLINK_CODEC_OPUS
                &opus_encoder,
#else
                NULL,
#endif
                pending_frame.samples,
                pending_frame.sample_count,
                pending_frame.sequence,
                flags,
                false);
            if (enqueue_err != ESP_OK) {
                mode->dropped_frames++;
                begin_local_stop(mode, "uplink_enqueue_failed");
                err = enqueue_err;
                break;
            }
            mode->uploaded_frames++;
        }

        memcpy(pending, current, samples_read * sizeof(int16_t));
        pending_frame.sample_count = samples_read;
        pending_frame.sequence = mode->capture_frames++;
        pending_frame.features = features;
        have_pending = true;

        if ((mode->capture_frames % NUWAX_MEETING_LOG_INTERVAL_FRAMES) == 0U) {
            ESP_LOGI(TAG,
                     "meeting capture frames=%lu uploaded=%lu avg_abs=%lu peak=%lu speech=%d inactive_ms=%lu speech_activity_ms=%lu sound_activity_ms=%lu",
                     (unsigned long)mode->capture_frames,
                     (unsigned long)mode->uploaded_frames,
                     (unsigned long)features.avg_abs,
                     (unsigned long)features.peak,
                     features.speech ? 1 : 0,
                     (unsigned long)mode->inactivity.inactive_ms,
                     (unsigned long)mode->inactivity.speech_activity_ms,
                     (unsigned long)mode->inactivity.sound_activity_ms);
        }
    }

    if (have_pending && uplink_ready) {
        const uint8_t flags = frame_flags(&pending_frame.features,
                                          pending_frame.sequence == 0U,
                                          true);
        esp_err_t final_err = nuwax_audio_uplink_enqueue_frame(
            uplink,
#if CONFIG_NUWAX_VOICE_UPLINK_CODEC_OPUS
            &opus_encoder,
#else
            NULL,
#endif
            pending_frame.samples,
            pending_frame.sample_count,
            pending_frame.sequence,
            flags,
            true);
        if (final_err != ESP_OK) {
            if (err == ESP_OK) {
                err = final_err;
                begin_local_stop(mode, "uplink_final_enqueue_failed");
            }
        } else {
            mode->uploaded_frames++;
        }
    }

cleanup:
    if (uplink_ready) {
        esp_err_t finish_err = nuwax_audio_uplink_finish(uplink);
        end_committed = nuwax_audio_uplink_end_committed(uplink);
        if (finish_err != ESP_OK &&
            !nuwax_audio_terminal_recovery_pending()) {
            (void)nuwax_ws_request_close_if_epoch(ws, ws_epoch);
            if (err == ESP_OK) {
                err = finish_err;
                begin_local_stop(mode, "uplink_finish_failed");
            } else {
                ESP_LOGW(TAG,
                         "meeting uplink finish also failed err=%s; preserving root reason=%s",
                         esp_err_to_name(finish_err),
                         mode->stop_reason[0] ? mode->stop_reason : "capture_failed");
            }
        }
        nuwax_audio_uplink_stats_t uplink_stats = {0};
        nuwax_audio_uplink_get_stats(uplink, &uplink_stats);
        ESP_LOGI(TAG,
                 "meeting uplink stats queued=%lu sent=%lu dropped=%lu queue_full=%lu max_ready=%lu ready=%lu finish=%s",
                 (unsigned long)uplink_stats.queued_frames,
                 (unsigned long)uplink_stats.sent_frames,
                 (unsigned long)uplink_stats.dropped_frames,
                 (unsigned long)uplink_stats.queue_full_events,
                 (unsigned long)uplink_stats.max_ready_depth,
                 (unsigned long)uplink_stats.ready_depth,
                 esp_err_to_name(finish_err));
        (void)nuwax_audio_uplink_destroy_owned(&uplink);
        uplink_ready = false;
    }
    if (uplink) {
        (void)nuwax_audio_uplink_destroy_owned(&uplink);
    }
#if CONFIG_NUWAX_VOICE_UPLINK_CODEC_OPUS
    if (opus_ready) {
        nuwax_opus_encoder_deinit(&opus_encoder);
    }
#endif
    if (mic_ready) {
        const esp_err_t mic_deinit_err = nuwax_mic_input_deinit(&mic);
        if (mic_deinit_err != ESP_OK) {
            if (err == ESP_OK) {
                err = mic_deinit_err;
                begin_local_stop(mode, "mic_deinit_failed");
            } else {
                ESP_LOGW(TAG,
                         "meeting microphone deinit also failed err=%s; preserving root reason=%s",
                         esp_err_to_name(mic_deinit_err),
                         mode->stop_reason[0] ? mode->stop_reason
                                              : "capture_failed");
            }
        }
    }
    meeting_free(current);
    meeting_free(pending);
    mode->capture_running = false;
    if (listen_started && end_committed &&
        !nuwax_audio_terminal_recovery_pending() && ws &&
        nuwax_ws_is_connected(ws) &&
        nuwax_ws_connection_epoch(ws) == ws_epoch) {
        esp_err_t stop_err =
            nuwax_session_send_listen_stop_retry_if_epoch(
                ws,
                "meeting",
                mode->audio_session_id,
                NULL,
                (uint32_t)(NUWAX_MEETING_CONTROL_SEND_TIMEOUT_US / 1000LL),
                ws_epoch);
        if (stop_err != ESP_OK) {
            const esp_err_t recovery_arm_err =
                nuwax_audio_terminal_recovery_arm(
                    ws,
                    ws_epoch,
                    NULL,
                    0U,
                    true,
                    "meeting",
                    mode->audio_session_id,
                    NULL,
                    NUWAX_AUDIO_TERMINAL_RECOVERY_DEFAULT_ATTEMPT_TIMEOUT_MS);
            if (recovery_arm_err != ESP_OK) {
                (void)nuwax_ws_request_close_if_epoch(ws, ws_epoch);
            }
            if (err == ESP_OK) {
                err = stop_err;
            }
            begin_local_stop(mode, "listen_stop_failed");
        }
    } else if (listen_started && !end_committed) {
        if (!nuwax_audio_terminal_recovery_pending()) {
            (void)nuwax_ws_request_close_if_epoch(ws, ws_epoch);
        }
        ESP_LOGW(TAG,
                 "meeting listen stop suppressed because binary END was not committed session=%s epoch=%lu",
                 mode->audio_session_id,
                 (unsigned long)ws_epoch);
    }
    /* STOPPING is an intent, not proof that the capture completed.  Preserve
     * transport/mic/uplink failures so cloud ACKs and the UI cannot report a
     * successful meeting session after a dropped frame or read error. */
    const bool capture_ok = err == ESP_OK;
    ESP_LOGI(TAG,
             "meeting capture done ok=%d reason=%s frames=%lu uploaded=%lu dropped=%lu inactive_ms=%lu read_discontinuities=%lu discarded_partial_samples=%llu err=%s",
             capture_ok ? 1 : 0,
             mode->stop_reason[0] ? mode->stop_reason : "normal_stop",
             (unsigned long)mode->capture_frames,
             (unsigned long)mode->uploaded_frames,
             (unsigned long)mode->dropped_frames,
             (unsigned long)mode->inactivity.inactive_ms,
             (unsigned long)read_discontinuities,
             (unsigned long long)discarded_partial_samples,
             esp_err_to_name(err));
    finish_state(mode, ws, capture_ok);
    return err;
#endif
}

esp_err_t nuwax_meeting_mode_init(nuwax_meeting_mode_t *mode,
                                  const nuwax_meeting_mode_callbacks_t *callbacks) {
    if (!mode) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(mode, 0, sizeof(*mode));
    if (callbacks) {
        mode->callbacks = *callbacks;
    }
    mode->events = xQueueCreate(NUWAX_MEETING_EVENT_QUEUE_DEPTH, sizeof(nuwax_meeting_event_t));
    if (!mode->events) {
        return ESP_ERR_NO_MEM;
    }
    mode->request_acks = xQueueCreate(NUWAX_MEETING_ACK_QUEUE_DEPTH,
                                      sizeof(nuwax_meeting_event_t));
    if (!mode->request_acks) {
        vQueueDelete(mode->events);
        mode->events = NULL;
        return ESP_ERR_NO_MEM;
    }
    nuwax_meeting_processor_config_t config;
    nuwax_meeting_processor_default_config(&config);
    nuwax_meeting_processor_init(&mode->processor, &config);
    atomic_init(&mode->state, NUWAX_MEETING_STATE_DISABLED);
    atomic_init(&mode->queued_enable_events, 0U);
    mode->initialized = true;
    return ESP_OK;
}

void nuwax_meeting_mode_deinit(nuwax_meeting_mode_t *mode) {
    if (!mode) {
        return;
    }
    if (mode->events) {
        vQueueDelete(mode->events);
    }
    if (mode->request_acks) {
        vQueueDelete(mode->request_acks);
    }
    memset(mode, 0, sizeof(*mode));
}

nuwax_meeting_state_t nuwax_meeting_mode_state(const nuwax_meeting_mode_t *mode) {
    return mode ? mode->state : NUWAX_MEETING_STATE_DISABLED;
}

bool nuwax_meeting_mode_has_pending(const nuwax_meeting_mode_t *mode) {
    if (!mode || !mode->initialized) {
        return false;
    }
    return mode->state != NUWAX_MEETING_STATE_DISABLED ||
           (mode->request_acks && uxQueueMessagesWaiting(mode->request_acks) > 0U) ||
           (mode->events && uxQueueMessagesWaiting(mode->events) > 0U);
}

bool nuwax_meeting_mode_has_audio_claim(const nuwax_meeting_mode_t *mode) {
    if (!mode || !mode->initialized) {
        return false;
    }
    return mode->state != NUWAX_MEETING_STATE_DISABLED ||
           atomic_load_explicit(&mode->queued_enable_events,
                                memory_order_acquire) > 0U;
}

bool nuwax_meeting_mode_blocks_ota(const nuwax_meeting_mode_t *mode) {
    return nuwax_meeting_mode_has_audio_claim(mode);
}

bool nuwax_meeting_mode_should_preempt_normal(const nuwax_meeting_mode_t *mode) {
    return nuwax_meeting_mode_has_pending(mode);
}

esp_err_t nuwax_meeting_mode_request_ui(nuwax_meeting_mode_t *mode,
                                        nuwax_ws_client_t *ws,
                                        bool enabled) {
    if (!mode || !mode->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!enabled) {
        if (mode->state == NUWAX_MEETING_STATE_DISABLED) {
            return ESP_OK;
        }
        begin_local_stop(mode, "ui_close");
        return ESP_OK;
    }
    if (!ws || !nuwax_ws_is_connected(ws)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (mode->state != NUWAX_MEETING_STATE_DISABLED) {
        return mode->state == NUWAX_MEETING_STATE_ACTIVE ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if (!can_start(mode)) {
        return ESP_ERR_INVALID_STATE;
    }

    reset_transient_state(mode);
    make_request_id(mode, "open", mode->pending_request_id);
    mode->start_deadline_ms = now_ms() + NUWAX_MEETING_START_TIMEOUT_MS;
    set_state(mode, NUWAX_MEETING_STATE_STARTING, "ui_enable");

    if (mode->callbacks.is_playback_active &&
        mode->callbacks.is_playback_active(mode->callbacks.ctx)) {
        mode->interrupt_current_turn = true;
        if (mode->callbacks.fade_and_cancel_playback) {
            mode->callbacks.fade_and_cancel_playback(mode->callbacks.ctx,
                                                     NUWAX_MEETING_PLAYBACK_FADE_MS,
                                                     "meeting_ui_enable");
        }
    }
    esp_err_t err = nuwax_meeting_protocol_send_request(ws,
                                                        mode->pending_request_id,
                                                        true,
                                                        mode->interrupt_current_turn,
                                                        NULL);
    if (err != ESP_OK) {
        reset_to_disabled(mode, "request_send_failed", true);
    }
    return err;
}

bool nuwax_meeting_mode_handle_protocol_text(nuwax_meeting_mode_t *mode,
                                             nuwax_ws_client_t *ws,
                                             const char *payload,
                                             int len) {
    if (!mode || !mode->initialized) {
        return false;
    }
    nuwax_meeting_protocol_event_t parsed = {0};
    if (!nuwax_meeting_protocol_parse(payload, len, &parsed)) {
        return false;
    }
    if (parsed.type == NUWAX_MEETING_PROTOCOL_EVENT_SET_COMMAND &&
        strcmp(parsed.state, "enabled") == 0 &&
        mode->state == NUWAX_MEETING_STATE_DISABLED && !can_start(mode)) {
        if (ws && nuwax_ws_is_connected(ws) && parsed.request_id[0] != '\0') {
            (void)nuwax_meeting_protocol_send_command_ack(ws,
                                                          parsed.request_id,
                                                          false,
                                                          false,
                                                          "audio_mode_busy");
        }
        return true;
    }
    nuwax_meeting_event_t event = {0};
    if (parsed.type == NUWAX_MEETING_PROTOCOL_EVENT_REQUEST_ACK) {
        event.type = NUWAX_MEETING_EVENT_REQUEST_ACK;
    } else if (parsed.type == NUWAX_MEETING_PROTOCOL_EVENT_SET_COMMAND) {
        event.type = strcmp(parsed.state, "enabled") == 0
                         ? NUWAX_MEETING_EVENT_CLOUD_ENABLE
                         : NUWAX_MEETING_EVENT_CLOUD_DISABLE;
    } else {
        return true;
    }
    event.ws_connection_epoch =
        ws && nuwax_ws_is_connected(ws) ? nuwax_ws_connection_epoch(ws) : 0U;
    if (event.ws_connection_epoch == 0U) {
        ESP_LOGW(TAG,
                 "meeting protocol event dropped without active connection type=%d request=%s",
                 (int)event.type,
                 parsed.request_id[0] ? parsed.request_id : "-");
        return true;
    }
    copy_text(event.request_id, sizeof(event.request_id), parsed.request_id);
    copy_text(event.audio_session_id,
              sizeof(event.audio_session_id),
              parsed.audio_session_id);
    copy_text(event.state, sizeof(event.state), parsed.state);
    copy_text(event.reason, sizeof(event.reason), parsed.reason);
    event.ok = parsed.ok;
    event.interrupt_current_turn = parsed.interrupt_current_turn;
    event.current_turn_cancelled = parsed.current_turn_cancelled;
    if (!queue_event(mode, &event)) {
        if (event.type == NUWAX_MEETING_EVENT_REQUEST_ACK && ws &&
            event.ws_connection_epoch != 0U) {
            ESP_LOGE(TAG,
                     "meeting ACK queue saturated request=%s; closing bound epoch",
                     event.request_id[0] ? event.request_id : "-");
            (void)nuwax_ws_request_close_if_epoch(
                ws, event.ws_connection_epoch);
            return true;
        }
        if (event.type == NUWAX_MEETING_EVENT_CLOUD_ENABLE ||
            event.type == NUWAX_MEETING_EVENT_CLOUD_DISABLE) {
            const bool enabled = event.type == NUWAX_MEETING_EVENT_CLOUD_ENABLE;
            if (ws && nuwax_ws_is_connected(ws)) {
                esp_err_t ack_err = nuwax_meeting_protocol_send_command_ack(
                    ws,
                    event.request_id,
                    enabled,
                    false,
                    "control_queue_full");
                if (ack_err != ESP_OK) {
                    ESP_LOGE(TAG,
                             "meeting queue-full ACK failed request=%s err=%s",
                             event.request_id,
                             esp_err_to_name(ack_err));
                }
            }
            ESP_LOGE(TAG,
                     "meeting command rejected because control queue is full request=%s",
                     event.request_id);
        }
    }
    return true;
}

esp_err_t nuwax_meeting_mode_run(nuwax_meeting_mode_t *mode,
                                 nuwax_ws_client_t *ws) {
    if (!mode || !mode->initialized || !ws) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t wait_started = now_ms();
    while (nuwax_ws_is_connected(ws)) {
        if (mode->callbacks.poll_controls) {
            mode->callbacks.poll_controls(mode->callbacks.ctx, ws);
        }
        drain_events(mode, ws);
        if (mode->state == NUWAX_MEETING_STATE_DISABLED) {
            return ESP_OK;
        }
        if (mode->state == NUWAX_MEETING_STATE_STARTING) {
            if (mode->cloud_authorized || mode->pending_command_enabled) {
                return capture_session(mode, ws);
            }
            if (deadline_expired(now_ms(), mode->start_deadline_ms) ||
                (uint32_t)(now_ms() - wait_started) > NUWAX_MEETING_START_TIMEOUT_MS + 500U) {
                reset_to_disabled(mode, "start_timeout", true);
                return ESP_ERR_TIMEOUT;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (mode->state == NUWAX_MEETING_STATE_STOPPING && !mode->capture_running) {
            finish_state(mode, ws, true);
            return ESP_OK;
        }
        if (mode->state == NUWAX_MEETING_STATE_ACTIVE ||
            (mode->state == NUWAX_MEETING_STATE_STOPPING && mode->capture_running)) {
            return capture_session(mode, ws);
        }
        return ESP_ERR_INVALID_STATE;
    }
    nuwax_meeting_mode_notify_ws_disconnected(mode);
    return ESP_ERR_INVALID_STATE;
}

esp_err_t nuwax_meeting_mode_request_handoff_stop(
    nuwax_meeting_mode_t *mode,
    nuwax_ws_client_t *ws,
    const char *reason) {
    if (!mode || !mode->initialized || !ws || !nuwax_ws_is_connected(ws)) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t ws_epoch = nuwax_ws_connection_epoch(ws);
    if (ws_epoch == 0U) {
        return ESP_ERR_INVALID_STATE;
    }
    if (mode->state == NUWAX_MEETING_STATE_DISABLED) {
        if (!nuwax_meeting_mode_has_pending(mode)) {
            return ESP_OK;
        }
        return nuwax_meeting_mode_abort_pending_controls(
            mode, ws, ws_epoch,
            reason && reason[0] ? reason : "superseded_by_audio_mode");
    }
    if (mode->state == NUWAX_MEETING_STATE_STOPPING) {
        return ESP_OK;
    }

    nuwax_meeting_event_t event = {
        .type = NUWAX_MEETING_EVENT_CLOUD_DISABLE,
        .ws_connection_epoch = ws_epoch,
    };
    copy_text(event.reason,
              sizeof(event.reason),
              reason && reason[0] ? reason : "superseded_by_audio_mode");
    if (!queue_event(mode, &event)) {
        ESP_LOGE(TAG,
                 "meeting handoff stop queue full state=%s epoch=%lu reason=%s",
                 state_name(mode->state),
                 (unsigned long)ws_epoch,
                 event.reason);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG,
             "meeting handoff stop queued state=%s epoch=%lu reason=%s",
             state_name(mode->state),
             (unsigned long)ws_epoch,
             event.reason);
    return ESP_OK;
}

esp_err_t nuwax_meeting_mode_abort_pending_controls(
    nuwax_meeting_mode_t *mode,
    nuwax_ws_client_t *ws,
    uint32_t ws_connection_epoch,
    const char *reason) {
    if (!mode || !mode->initialized || !ws ||
        !nuwax_ws_is_connected(ws) || ws_connection_epoch == 0U ||
        nuwax_ws_connection_epoch(ws) != ws_connection_epoch) {
        return ESP_ERR_INVALID_STATE;
    }
    if (mode->state != NUWAX_MEETING_STATE_DISABLED) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = ESP_OK;
    nuwax_meeting_event_t event = {0};
    if (mode->request_acks) {
        while (xQueueReceive(mode->request_acks, &event, 0) == pdTRUE) {
            ESP_LOGW(TAG,
                     "meeting request ACK discarded during reconnect recovery request=%s event_epoch=%lu active_epoch=%lu",
                     event.request_id[0] ? event.request_id : "-",
                     (unsigned long)event.ws_connection_epoch,
                     (unsigned long)ws_connection_epoch);
        }
    }
    if (mode->events) {
        for (unsigned i = 0U; i < NUWAX_MEETING_EVENT_QUEUE_DEPTH; ++i) {
            if (xQueueReceive(mode->events, &event, 0) != pdTRUE) {
                break;
            }
            if (event.ws_connection_epoch != ws_connection_epoch) {
                ESP_LOGW(TAG,
                         "stale meeting control discarded during reconnect recovery request=%s event_epoch=%lu active_epoch=%lu",
                         event.request_id[0] ? event.request_id : "-",
                          (unsigned long)event.ws_connection_epoch,
                          (unsigned long)ws_connection_epoch);
                complete_queued_enable_event(mode, &event);
                continue;
            }
            if ((event.type == NUWAX_MEETING_EVENT_CLOUD_ENABLE ||
                 event.type == NUWAX_MEETING_EVENT_CLOUD_DISABLE) &&
                event.request_id[0] != '\0') {
                const bool enabled =
                    event.type == NUWAX_MEETING_EVENT_CLOUD_ENABLE;
                const esp_err_t ack_err =
                    nuwax_meeting_protocol_send_command_ack(
                        ws,
                        event.request_id,
                        enabled,
                        false,
                        reason && reason[0] ? reason
                                            : "device_recovery_timeout");
                if (ack_err != ESP_OK && result == ESP_OK) {
                    result = ack_err;
                }
            }
            complete_queued_enable_event(mode, &event);
        }
    }
    if (nuwax_meeting_mode_has_pending(mode) && result == ESP_OK) {
        result = ESP_ERR_TIMEOUT;
    }
    return result;
}

void nuwax_meeting_mode_notify_ws_disconnected(nuwax_meeting_mode_t *mode) {
    if (!mode || !mode->initialized) {
        return;
    }
    clear_event_queue(mode);
    reset_to_disabled(mode, "ws_disconnected", false);
}
