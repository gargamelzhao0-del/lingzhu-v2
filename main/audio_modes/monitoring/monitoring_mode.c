#include "sd_audio_session.h"
#include "monitoring_mode.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "sd_audio_control_compat.h"

#include "audio_frame.h"
#include "conversation/audio_terminal_recovery.h"
#include "device_error_report.h"
#include "audio_uplink.h"
#include "mic_input.h"
#include "monitoring_speech_policy.h"
#include "opus_encoder_wrapper.h"
#include "session_protocol.h"
#if CONFIG_NUWAX_OFFLINE_AUDIO_ENABLE && !CONFIG_NUWAX_SD_SESSION_ENABLE
#include "offline_audio_spool.h"
#include "offline_audio_voice.h"
#endif

static const char *TAG = "nuwax_monitoring";

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
#ifndef CONFIG_NUWAX_MONITORING_SILENCE_STOP_MS
#define CONFIG_NUWAX_MONITORING_SILENCE_STOP_MS 120000
#endif
#ifndef CONFIG_NUWAX_MONITORING_PREROLL_MS
#define CONFIG_NUWAX_MONITORING_PREROLL_MS 500
#endif
#ifndef CONFIG_NUWAX_MONITORING_SPEECH_CONFIRM_FRAMES
#define CONFIG_NUWAX_MONITORING_SPEECH_CONFIRM_FRAMES 3
#endif
#ifndef CONFIG_NUWAX_MONITORING_RESUME_CONFIRM_FRAMES
#define CONFIG_NUWAX_MONITORING_RESUME_CONFIRM_FRAMES 10
#endif
#ifndef CONFIG_NUWAX_MONITORING_RESUME_GUARD_MS
#define CONFIG_NUWAX_MONITORING_RESUME_GUARD_MS 300
#endif
#ifndef CONFIG_NUWAX_MONITORING_RESUME_QUIET_FRAMES
#define CONFIG_NUWAX_MONITORING_RESUME_QUIET_FRAMES 10
#endif
#ifndef CONFIG_NUWAX_MONITORING_SPEECH_REFRESH_FRAMES
#define CONFIG_NUWAX_MONITORING_SPEECH_REFRESH_FRAMES 10
#endif
#ifndef CONFIG_NUWAX_MONITORING_SPEECH_MIN_AVG_ABS
#define CONFIG_NUWAX_MONITORING_SPEECH_MIN_AVG_ABS 208
#endif
#ifndef CONFIG_NUWAX_MONITORING_SPEECH_MIN_PEAK
#define CONFIG_NUWAX_MONITORING_SPEECH_MIN_PEAK 780
#endif
#ifndef CONFIG_NUWAX_MONITORING_SPEECH_NOISE_MARGIN_ABS
#define CONFIG_NUWAX_MONITORING_SPEECH_NOISE_MARGIN_ABS 104
#endif
#ifndef CONFIG_NUWAX_MONITORING_CAPTURE_ACK_TIMEOUT_MS
#define CONFIG_NUWAX_MONITORING_CAPTURE_ACK_TIMEOUT_MS 1500
#endif
#ifndef CONFIG_NUWAX_MONITORING_CAPTURE_ACK_MAX_RETRIES
#define CONFIG_NUWAX_MONITORING_CAPTURE_ACK_MAX_RETRIES 3
#endif
#ifndef CONFIG_NUWAX_MONITORING_ESP_VAD_ENABLE
#define CONFIG_NUWAX_MONITORING_ESP_VAD_ENABLE 1
#endif

#include "monitoring_speech_detector.h"

#define NUWAX_MONITORING_EVENT_QUEUE_DEPTH 8U
#define NUWAX_MONITORING_ACK_QUEUE_DEPTH 2U
#define NUWAX_MONITORING_PLAYBACK_FADE_MS 200U
#define NUWAX_MONITORING_PLAYBACK_SETTLE_TIMEOUT_MS 2000U
#define NUWAX_MONITORING_CONTROL_SEND_TIMEOUT_US 1000000LL
#define NUWAX_MONITORING_READ_STALL_TIMEOUT_MS 5000U
#define NUWAX_MONITORING_LOG_INTERVAL_FRAMES 250U
#define NUWAX_MONITORING_DISCONTINUITY_LOG_INTERVAL 100U
#define NUWAX_MONITORING_COMMAND_ACK_RETRY_INTERVAL_MS 500U
#define NUWAX_MONITORING_COMMAND_ACK_MAX_ATTEMPTS 5U
/* Keep control-plane bursts from delaying the 20 ms RX consumer. Remaining
 * events are handled on the next capture iteration. */
#define NUWAX_MONITORING_MAX_EVENTS_PER_LOOP 4U

#define NUWAX_MONITORING_FRAME_SAMPLES \
    ((CONFIG_NUWAX_VOICE_SAMPLE_RATE * CONFIG_NUWAX_VOICE_FRAME_DURATION_MS) / 1000)

typedef struct {
    int16_t *samples;
    size_t sample_count;
    uint32_t sequence;
    bool speech;
} monitoring_pending_frame_t;

#if CONFIG_NUWAX_AUDIO_SOURCE_I2S_MIC


typedef struct {
    bool active;
    bool pause;
    uint32_t ws_connection_epoch;
    uint32_t attempts;
    uint32_t deadline_ms;
    uint32_t silence_ms;
    char request_id[NUWAX_MONITORING_REQUEST_ID_BYTES];
    char audio_session_id[NUWAX_MONITORING_AUDIO_SESSION_ID_BYTES];
} monitoring_capture_control_t;

typedef struct {
    int16_t *samples;
    uint8_t *speech;
    size_t frame_samples;
    size_t capacity_frames;
    size_t count;
    size_t write_index;
} monitoring_preroll_t;

typedef struct {
    nuwax_audio_uplink_t *uplink;
    monitoring_pending_frame_t pending;
    uint32_t next_sequence;
    uint32_t first_sequence;
    bool uplink_ready;
    bool have_pending;
    bool listen_started;
    uint32_t ws_epoch;
#if CONFIG_NUWAX_VOICE_UPLINK_CODEC_OPUS
    nuwax_opus_encoder_t opus_encoder;
    bool opus_ready;
#endif
} monitoring_stream_t;

typedef struct {
    nuwax_monitoring_capture_phase_t phase;
    uint32_t silent_frames;
    uint32_t resume_guard_remaining_frames;
    uint32_t resume_quiet_frames;
    bool resume_rearmed;
} monitoring_capture_policy_t;
#endif

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static bool deadline_expired(uint32_t now, uint32_t deadline) {
    return (int32_t)(now - deadline) >= 0;
}

static void copy_text(char *dst, size_t size, const char *src) {
    if (!dst || size == 0U) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t copy_len = 0U;
    while (copy_len + 1U < size && src[copy_len] != '\0') {
        copy_len++;
    }
    memcpy(dst, src, copy_len);
    dst[copy_len] = '\0';
}

static const char *state_name(nuwax_monitoring_state_t state) {
    switch (state) {
        case NUWAX_MONITORING_STATE_DISABLED:
            return "disabled";
        case NUWAX_MONITORING_STATE_STARTING:
            return "starting";
        case NUWAX_MONITORING_STATE_ACTIVE:
            return "active";
        case NUWAX_MONITORING_STATE_STOPPING:
            return "stopping";
        case NUWAX_MONITORING_STATE_ERROR:
            return "error";
        default:
            return "unknown";
    }
}

static void retire_active_result(nuwax_monitoring_mode_t *mode);


static void send_ui_close_request(nuwax_monitoring_mode_t *mode, nuwax_ws_client_t *ws);

static void set_state(nuwax_monitoring_mode_t *mode,
                      nuwax_monitoring_state_t state,
                      const char *reason) {
    if (!mode) {
        return;
    }
    if (state != NUWAX_MONITORING_STATE_ACTIVE) retire_active_result(mode);
    if (state == NUWAX_MONITORING_STATE_STARTING) {
        mode->pending_ui_close = false;
        mode->close_request_id[0] = '\0';
        mode->close_request_epoch = 0;
        mode->close_survives_disconnect = false;
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

static bool can_start(const nuwax_monitoring_mode_t *mode) {
    return !mode || !mode->callbacks.can_start ||
           mode->callbacks.can_start(mode->callbacks.ctx);
}

static bool is_state_sync_ack(const nuwax_monitoring_event_t *event) {
    static const char prefix[] = "monitoring_sync_";
    return event && event->type == NUWAX_MONITORING_EVENT_REQUEST_ACK &&
        strncmp(event->request_id, prefix, sizeof(prefix) - 1U) == 0;
}

static void complete_queued_enable_event(
    nuwax_monitoring_mode_t *mode,
    const nuwax_monitoring_event_t *event) {
    if (!mode || !event) return;
    if (is_state_sync_ack(event)) {
        uint32_t queued = atomic_load_explicit(&mode->queued_sync_acks, memory_order_acquire);
        while (queued > 0U) {
            if (atomic_compare_exchange_weak_explicit(&mode->queued_sync_acks,
                    &queued, queued - 1U, memory_order_acq_rel, memory_order_acquire)) return;
        }
        ESP_LOGE(TAG, "monitoring state-sync ACK accounting underflow");
        return;
    }
    if (event->type != NUWAX_MONITORING_EVENT_CLOUD_ENABLE) return;
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
    ESP_LOGE(TAG, "monitoring enable queue accounting underflow");
}

static bool queue_event(nuwax_monitoring_mode_t *mode,
                        const nuwax_monitoring_event_t *event) {
    if (!mode || !event) {
        return false;
    }
    if (event->type == NUWAX_MONITORING_EVENT_CAPTURE_CONTROL_ACK) {
        if (!mode->capture_control_acks ||
            xQueueSend(mode->capture_control_acks, event, 0) != pdTRUE) {
            ESP_LOGE(TAG,
                     "monitoring capture-control ACK queue unavailable or full");
            return false;
        }
        return true;
    }
    if (!mode->events) {
        return false;
    }
    if (is_state_sync_ack(event))
        atomic_fetch_add_explicit(&mode->queued_sync_acks, 1U, memory_order_acq_rel);
    if (event->type == NUWAX_MONITORING_EVENT_CLOUD_ENABLE) {
        atomic_fetch_add_explicit(&mode->queued_enable_events,
                                  1U,
                                  memory_order_acq_rel);
    }
    if (xQueueSend(mode->events, event, 0) != pdTRUE) {
        complete_queued_enable_event(mode, event);
        ESP_LOGW(TAG, "monitoring control event queue full type=%d", (int)event->type);
        return false;
    }
    return true;
}

static bool event_matches_connection(const nuwax_monitoring_event_t *event,
                                     const nuwax_ws_client_t *ws) {
    if (!event || !ws) {
        return false;
    }
    const uint32_t active_epoch = nuwax_ws_connection_epoch(ws);
    if (event->ws_connection_epoch == 0U || active_epoch == 0U ||
        event->ws_connection_epoch != active_epoch) {
        ESP_LOGW(TAG,
                 "stale monitoring event ignored type=%d request=%s event_epoch=%lu active_epoch=%lu",
                 (int)event->type,
                 event->request_id[0] ? event->request_id : "-",
                 (unsigned long)event->ws_connection_epoch,
                 (unsigned long)active_epoch);
        return false;
    }
    return true;
}

static void clear_event_queue(nuwax_monitoring_mode_t *mode) {
    if (!mode) {
        return;
    }
    nuwax_monitoring_event_t event;
    if (mode->capture_control_acks) {
        while (xQueueReceive(mode->capture_control_acks, &event, 0) == pdTRUE) {
        }
    }
    if (mode->events) {
        while (xQueueReceive(mode->events, &event, 0) == pdTRUE) {
            complete_queued_enable_event(mode, &event);
        }
    }
}

static void select_audio_session(nuwax_monitoring_mode_t *mode, const char *requested)
{
    const char *selected = requested;
    if (mode->identity.session_id[0]) {
        selected = mode->identity.session_id;
        if (requested && requested[0] && strcmp(requested, selected) != 0) {
            ESP_LOGW(TAG, "monitoring retains session=%s cloud_requested=%s",
                     selected, requested);
        }
    }
    copy_text(mode->audio_session_id, sizeof(mode->audio_session_id), selected);
}

static void reset_transient_state(nuwax_monitoring_mode_t *mode) {
    if (!mode) {
        return;
    }
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    nuwax_sd_session_release_start(NUWAX_OFFLINE_AUDIO_MODE_MONITORING);
#endif
    mode->sd_local = false;
    mode->sd_start_ms = 0U;
    memset(&mode->sd_sync, 0, sizeof(mode->sd_sync));
    mode->capture_running = false;
    mode->pending_command_ack = false;
    mode->pending_command_enabled = false;
    mode->pending_command_ack_attempts = 0U;
    mode->pending_command_ack_deadline_ms = 0U;
    mode->capture_frames = 0U;
    mode->uploaded_frames = 0U;
    mode->dropped_frames = 0U;
    mode->pause_requests = 0U;
    mode->resume_requests = 0U;
    mode->capture_control_retries = 0U;
    mode->consecutive_read_timeouts = 0U;
    atomic_store_explicit(&mode->capture_phase,
                          NUWAX_MONITORING_CAPTURE_INACTIVE,
                          memory_order_release);
    mode->pending_command_request_id[0] = '\0';
    mode->audio_session_id[0] = '\0';
    mode->stop_reason[0] = '\0';
}

static void reset_to_disabled(nuwax_monitoring_mode_t *mode,
                              const char *reason,
                              bool report_error) {
    if (!mode) {
        return;
    }
    if (report_error) {
        set_state(mode, NUWAX_MONITORING_STATE_ERROR, reason);
    }
    reset_transient_state(mode);
    set_state(mode, NUWAX_MONITORING_STATE_DISABLED, reason);
}

static void set_stop_reason(nuwax_monitoring_mode_t *mode, const char *reason) {
    if (!mode) {
        return;
    }
    copy_text(mode->stop_reason,
              sizeof(mode->stop_reason),
              reason ? reason : "capture_failed");
}

static void make_audio_session_id(nuwax_monitoring_mode_t *mode) {
    if (!mode) return;
    if (mode->identity.session_id[0]) {
        copy_text(mode->audio_session_id, sizeof(mode->audio_session_id), mode->identity.session_id);
        return;
    }
    if (mode->audio_session_id[0] != '\0') {
        nuwax_audio_mode_identity_remember(&mode->identity, mode->audio_session_id);
        return;
    }
    const uint32_t sequence = ++mode->stream_sequence;
    snprintf(mode->audio_session_id,
             sizeof(mode->audio_session_id),
             "monitoring_stream_%lu_%08lx",
             (unsigned long)sequence,
             (unsigned long)esp_random());
    nuwax_audio_mode_identity_remember(&mode->identity, mode->audio_session_id);
}

static void make_capture_control_request_id(
    nuwax_monitoring_mode_t *mode,
    bool pause,
    char out[NUWAX_MONITORING_REQUEST_ID_BYTES]) {
    if (!mode || !out) {
        return;
    }
    const uint32_t sequence = ++mode->request_sequence;
    snprintf(out,
             NUWAX_MONITORING_REQUEST_ID_BYTES,
             "mon_%s_%lu_%08lx",
             pause ? "pause" : "resume",
             (unsigned long)sequence,
             (unsigned long)esp_random());
}

static void begin_cloud_stop(nuwax_monitoring_mode_t *mode,
                             const nuwax_monitoring_event_t *event) {
    if (!mode) {
        return;
    }
    mode->pending_ui_close = false;
    set_stop_reason(mode,
                    event && event->reason[0] ? event->reason
                                              : "cloud_disable");
    mode->pending_command_ack = event && event->request_id[0] != '\0';
    mode->pending_command_enabled = false;
    if (event) {
        copy_text(mode->pending_command_request_id,
                  sizeof(mode->pending_command_request_id),
                  event->request_id);
    }
    set_state(mode, NUWAX_MONITORING_STATE_STOPPING, mode->stop_reason);
}

static esp_err_t send_event_command_ack(nuwax_monitoring_mode_t *mode, nuwax_ws_client_t *ws,
    const nuwax_monitoring_event_t *event, bool enabled, bool ok, const char *message, const char *session_id)
{
    const int64_t now = esp_timer_get_time();
    nuwax_audio_command_deadline_t *c = mode ? &mode->command_deadline : NULL;
    const bool same = c && nuwax_audio_command_matches(c, event->request_id);
    const int64_t received = same ? c->received_us :
        (now / 1000 - (uint32_t)((uint32_t)(now / 1000) - event->received_ms)) * 1000;
    const int64_t limit = received + (int64_t)NUWAX_AUDIO_COMMAND_ACK_MS * 1000;
    if (now >= limit || (same && c->attempts >= NUWAX_MONITORING_COMMAND_ACK_MAX_ATTEMPTS)) return ESP_ERR_TIMEOUT;
    const int64_t deadline = limit - now < NUWAX_AUDIO_COMMAND_SEND_SLICE_US
        ? limit : now + NUWAX_AUDIO_COMMAND_SEND_SLICE_US;
    if (same) ++c->attempts;
    const esp_err_t err = nuwax_monitoring_protocol_send_command_ack_until(ws, event->request_id,
        enabled, ok, message, session_id, deadline, event->ws_connection_epoch);
    if (same && err == ESP_OK) c->ack_pending = false;
    return err;
}

static void clear_pending_command_ack(nuwax_monitoring_mode_t *mode) {
    if (!mode) {
        return;
    }
    mode->pending_command_ack = false;
    mode->pending_command_enabled = false;
    mode->pending_command_ack_attempts = 0U;
    mode->pending_command_ack_deadline_ms = 0U;
    mode->pending_command_request_id[0] = '\0';
}

static void retire_active_result(nuwax_monitoring_mode_t *mode)
{
    nuwax_audio_command_deadline_t *c = &mode->command_deadline;
    nuwax_audio_command_retire_active(c);
    nuwax_sd_control_retire_enabled(&mode->sd_sync, c, esp_timer_get_time());
    if (c->retired && nuwax_audio_command_matches(c, mode->pending_command_request_id))
        clear_pending_command_ack(mode);
}

static void service_command_result(nuwax_monitoring_mode_t *mode, nuwax_ws_client_t *ws)
{
    nuwax_audio_command_deadline_t *c = &mode->command_deadline;
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    if (mode->sd_local && !nuwax_offline_audio_capture_running()) retire_active_result(mode);
#endif
    if (c->ok && c->enabled && (c->retired || mode->state != NUWAX_MONITORING_STATE_ACTIVE ||
        !mode->capture_running || (c->session_id[0] && strcmp(c->session_id, mode->audio_session_id)))) {
        retire_active_result(mode);
        return;
    }
    if (!c->tracked || !c->decided || !c->ack_pending) return;
    const int64_t now = esp_timer_get_time();
    const int64_t deadline = nuwax_audio_command_send_deadline(c, now);
    if (!deadline || c->attempts >= NUWAX_MONITORING_COMMAND_ACK_MAX_ATTEMPTS) {
        c->ack_pending = false;
        if (nuwax_audio_command_matches(c, mode->pending_command_request_id)) clear_pending_command_ack(mode);
        return;
    }
    if (ws && nuwax_ws_is_connected(ws) && c->ok &&
        (!c->enabled || mode->state == NUWAX_MONITORING_STATE_ACTIVE))
        c->epoch = nuwax_ws_connection_epoch(ws);
    if (!ws || !nuwax_ws_is_connected(ws) || nuwax_ws_connection_epoch(ws) != c->epoch ||
        (c->attempts && (int32_t)(now_ms() - c->retry_at_ms) < 0)) return;
    ++c->attempts;
    const esp_err_t err = nuwax_monitoring_protocol_send_command_ack_until(ws, c->request_id,
        c->enabled, c->ok, c->message, c->session_id[0] ? c->session_id : NULL, deadline, c->epoch);
    c->retry_at_ms = (uint32_t)(now / 1000) + NUWAX_AUDIO_COMMAND_RETRY_MS;
    if (nuwax_audio_command_matches(c, mode->pending_command_request_id)) {
        mode->pending_command_ack_attempts = c->attempts;
        mode->pending_command_ack_deadline_ms = c->retry_at_ms;
    }
    if (err == ESP_OK) {
        c->ack_pending = false;
        if (nuwax_audio_command_matches(c, mode->pending_command_request_id)) clear_pending_command_ack(mode);
    }
    ESP_LOGI(TAG, "command result request=%s ok=%d elapsed_ms=%lu attempt=%u err=%s",
        c->request_id, c->ok, (unsigned long)((esp_timer_get_time() - c->received_us) / 1000),
        c->attempts, esp_err_to_name(err));
}

static void try_send_active_command_ack(nuwax_monitoring_mode_t *mode,
                                        nuwax_ws_client_t *ws,
                                        bool force) {
    if (!mode || !ws || mode->state != NUWAX_MONITORING_STATE_ACTIVE ||
        !mode->pending_command_ack || !mode->pending_command_enabled ||
        mode->pending_command_request_id[0] == '\0' ||
        !nuwax_ws_is_connected(ws)) {
        return;
    }
    if (nuwax_audio_command_matches(&mode->command_deadline, mode->pending_command_request_id)) {
        nuwax_monitoring_mode_service_command_deadline(mode, ws);
        if (mode->state != NUWAX_MONITORING_STATE_ACTIVE) return;
        nuwax_audio_command_decide(&mode->command_deadline, true, true, "active",
            mode->sd_local ? mode->audio_session_id : NULL);
        service_command_result(mode, ws);
        return;
    }
    const uint32_t now = now_ms();
    if (!force &&
        !deadline_expired(now, mode->pending_command_ack_deadline_ms)) {
        return;
    }

    mode->pending_command_ack_attempts++;
    const esp_err_t ack_err = nuwax_monitoring_protocol_send_command_ack_with_session(
        ws,
        mode->pending_command_request_id,
        true,
        true,
        "active", mode->sd_local ? mode->audio_session_id : NULL);
    if (ack_err == ESP_OK) {
        ESP_LOGI(TAG,
                 "monitoring active command ACK delivered request=%s attempts=%lu",
                 mode->pending_command_request_id,
                 (unsigned long)mode->pending_command_ack_attempts);
        clear_pending_command_ack(mode);
        return;
    }

    if (mode->pending_command_ack_attempts >=
        NUWAX_MONITORING_COMMAND_ACK_MAX_ATTEMPTS) {
        ESP_LOGE(TAG,
                 "monitoring active command ACK exhausted request=%s attempts=%lu err=%s; capture retained",
                 mode->pending_command_request_id,
                 (unsigned long)mode->pending_command_ack_attempts,
                 esp_err_to_name(ack_err));
        clear_pending_command_ack(mode);
        return;
    }

    mode->pending_command_ack_deadline_ms =
        now + NUWAX_MONITORING_COMMAND_ACK_RETRY_INTERVAL_MS;
    ESP_LOGW(TAG,
             "monitoring active command ACK retry scheduled request=%s attempt=%lu/%u err=%s",
             mode->pending_command_request_id,
             (unsigned long)mode->pending_command_ack_attempts,
             (unsigned int)NUWAX_MONITORING_COMMAND_ACK_MAX_ATTEMPTS,
             esp_err_to_name(ack_err));
}

#if CONFIG_NUWAX_SD_SESSION_ENABLE
static void service_sd_cloud_sync(nuwax_monitoring_mode_t *mode, nuwax_ws_client_t *ws)
{
    if (!ws || !nuwax_ws_is_connected(ws)) return;
    const bool enabled = mode->state == NUWAX_MONITORING_STATE_ACTIVE;
    if (mode->state != NUWAX_MONITORING_STATE_DISABLED && !enabled) return;
    /* No-card active streams use their existing opening/resume request.
     * Never add a control send in the main-owned realtime capture loop. */
    if (enabled && (!mode->sd_local || !mode->audio_session_id[0])) return;
    if (!enabled && (mode->offline_resume_required || mode->identity.resuming)) return;
    nuwax_sd_control_sync_t *sync = &mode->sd_sync;
    const uint32_t now = now_ms();
    nuwax_sd_control_sync_connect(sync, nuwax_ws_connection_epoch(ws), now,
                                  atomic_load_explicit(&mode->control_rx_order, memory_order_relaxed));
    if (sync->request_id[0] && sync->reported_enabled != enabled) {
        sync->request_id[0] = '\0';
        sync->confirmed = sync->reply_confirmed = sync->announced = false;
        sync->timeout_reported = false;
        sync->attempts = sync->send_failures = 0U;
        sync->started_ms = sync->next_send_ms = now;
    }
    sync->reported_enabled = enabled;
    if (sync->announced && !sync->confirmed && !sync->timeout_reported &&
        (uint32_t)(now - sync->started_ms) >= NUWAX_SD_CONTROL_SYNC_MS) {
        sync->timeout_reported = true;
        ESP_LOGW(TAG, "SD monitoring control sync timeout epoch=%lu state=%s session=%s; local state retained, commands remain available",
                 (unsigned long)sync->epoch, enabled ? "enabled" : "disabled",
                 enabled ? mode->audio_session_id : "-");
    }
    if (!nuwax_sd_control_sync_due(sync, now)) return;
    if (!sync->request_id[0])
        snprintf(sync->request_id, sizeof(sync->request_id), "monitoring_sync_%lu_%08lx",
                 (unsigned long)++mode->request_sequence, (unsigned long)esp_random());
    const esp_err_t err = enabled
        ? nuwax_monitoring_protocol_send_resume_request(ws, sync->request_id, mode->audio_session_id)
        : nuwax_monitoring_protocol_send_request(ws, sync->request_id, false, "device_state_sync");
    nuwax_sd_control_sync_note_send(sync, now_ms(), err == ESP_OK);
    ESP_LOGI(TAG, "SD monitoring control sync request=%s epoch=%lu state=%s session=%s attempt=%u announced=%u send_failures=%u err=%s",
             sync->request_id, (unsigned long)sync->epoch, enabled ? "enabled" : "disabled",
             enabled ? mode->audio_session_id : "-", sync->attempts,
             (unsigned)sync->announced, (unsigned)sync->send_failures, esp_err_to_name(err));
}


static bool handle_sd_sync_ack(nuwax_monitoring_mode_t *mode, const nuwax_monitoring_event_t *event)
{
    nuwax_sd_control_sync_t *sync = &mode->sd_sync;
    if (!sync->request_id[0] ||
        event->ws_connection_epoch != sync->epoch ||
        strcmp(event->request_id, sync->request_id) != 0) return false;
    const bool accepted = event->ok &&
        strcmp(event->state, sync->reported_enabled ? "enabled" : "disabled") == 0;
    if (accepted)
        nuwax_sd_control_sync_confirm_reply(sync, event->rx_order, event->audio_session_id);
    ESP_LOGI(TAG, "SD monitoring control sync reply request=%s epoch=%lu accepted=%d session=%s cloud_session=%s control_session=%s; capture retained",
             event->request_id, (unsigned long)sync->epoch, accepted,
             mode->audio_session_id, event->audio_session_id[0] ? event->audio_session_id : "-",
             sync->reply_session_id[0] ? sync->reply_session_id : "-");
    return true;
}

static bool reject_sd_cloud_disable(nuwax_monitoring_mode_t *mode, nuwax_ws_client_t *ws,
                                    const nuwax_monitoring_event_t *event)
{
    /* Handoffs are owner-generated events without a cloud request ID. */
    if (!mode->sd_local || !event->request_id[0] ||
        (mode->state != NUWAX_MONITORING_STATE_ACTIVE && mode->state != NUWAX_MONITORING_STATE_STARTING)) return false;
    const char *reason = event->local_stop_generation != atomic_load_explicit(
        &mode->local_stop_generation, memory_order_acquire) ? "local_stop_superseded" :
        nuwax_sd_control_disable_rejection(&mode->sd_sync, event->rx_order, event->received_ms,
            event->request_id, event->audio_session_id, mode->audio_session_id);
    if (!reason) return false;
    ESP_LOGW(TAG, "SD monitoring cloud disable rejected request=%s epoch=%lu target_session=%s local_session=%s reason=%s",
             event->request_id, (unsigned long)event->ws_connection_epoch,
             event->audio_session_id[0] ? event->audio_session_id : "-",
             mode->audio_session_id[0] ? mode->audio_session_id : "-", reason);
    (void)send_event_command_ack(mode, ws, event, true, false, reason, NULL);
    return true;
}
#endif




static void handle_event(nuwax_monitoring_mode_t *mode,
                         nuwax_ws_client_t *ws,
                         const nuwax_monitoring_event_t *event) {
    if (!mode || !event) {
        return;
    }

    if (event->type == NUWAX_MONITORING_EVENT_CLOUD_ENABLE ||
        event->type == NUWAX_MONITORING_EVENT_CLOUD_DISABLE)
        ESP_LOGI(TAG, "monitoring cloud control request=%s state=%s epoch=%lu target_session=%s local_session=%s sd=%d",
                 event->request_id[0] ? event->request_id : "-", event->state,
                 (unsigned long)event->ws_connection_epoch,
                 event->audio_session_id[0] ? event->audio_session_id : "-",
                 mode->audio_session_id[0] ? mode->audio_session_id : "-", (int)mode->sd_local);

    if (event->type == NUWAX_MONITORING_EVENT_REQUEST_ACK) {
        if (mode->close_request_id[0] && event->ws_connection_epoch == mode->close_request_epoch &&
            !strcmp(event->request_id, mode->close_request_id)) {
            mode->pending_ui_close = false;
            mode->close_request_id[0] = '\0';
            mode->close_request_epoch = 0;
            return;
        }
#if CONFIG_NUWAX_SD_SESSION_ENABLE
        (void)handle_sd_sync_ack(mode, event);
#endif
        return;
    }

    if (event->type == NUWAX_MONITORING_EVENT_CAPTURE_CONTROL_ACK) {
        return;
    }

    if (event->type == NUWAX_MONITORING_EVENT_CLOUD_ENABLE && event->local_stop_generation !=
        atomic_load(&mode->local_stop_generation)) {
        (void)send_event_command_ack(mode, ws, event, false, false, "local_stop_superseded", NULL);
        return;
    }
    if (event->request_id[0]) {
        nuwax_audio_command_deadline_t *c = &mode->command_deadline;
        if (nuwax_audio_command_matches(c, event->request_id) && c->retired) return;
        if (nuwax_audio_command_matches(c, event->request_id) &&
            (!c->decided || !c->ok || mode->state != NUWAX_MONITORING_STATE_ACTIVE)) {
            /* Retry keeps the first RX time and the immutable terminal result. */
            if (c->decided && !c->ack_pending && nuwax_audio_command_send_deadline(c, esp_timer_get_time())) {
                c->ack_pending = true;
                c->retry_at_ms = now_ms();
            }
            service_command_result(mode, ws);
            return;
        }
        if (nuwax_audio_command_failed(c, event->request_id)) return;
        if ((uint32_t)(now_ms() - event->received_ms) >= NUWAX_AUDIO_COMMAND_EXECUTE_MS) {
            /* The event never acquired ownership. Do not stop the active mode. */
            nuwax_audio_command_deadline_t expired = {0};
            nuwax_audio_command_begin(&expired, event->request_id, event->received_ms,
                event->ws_connection_epoch, event->local_stop_generation, esp_timer_get_time());
            nuwax_audio_command_failure_t *f = &c->failed[c->failed_next++ % NUWAX_AUDIO_COMMAND_FAILURE_CACHE];
            copy_text(f->request_id, sizeof(f->request_id), event->request_id);
            f->received_us = expired.received_us;
            const int64_t deadline = nuwax_audio_command_send_deadline(&expired, esp_timer_get_time());
            if (deadline) (void)nuwax_monitoring_protocol_send_command_ack_until(ws, event->request_id,
                false, false, "mode_handoff_failed", NULL, deadline, event->ws_connection_epoch);
            return;
        }
    }

    if (event->type == NUWAX_MONITORING_EVENT_CLOUD_ENABLE) {
        if (event->local_stop_generation != atomic_load_explicit(
                &mode->local_stop_generation, memory_order_acquire)) {
            if (ws && event->request_id[0]) {
                (void)send_event_command_ack(mode, ws, event, false, false, "local_stop_superseded", NULL);
            }
            ESP_LOGI(TAG, "queued monitoring enable cancelled by local stop request=%s",
                     event->request_id);
            return;
        }
        bool sd_admission = false;
        (void)sd_admission;
#if CONFIG_NUWAX_SD_SESSION_ENABLE
        sd_admission = mode->sd_local ||
            (mode->state == NUWAX_MONITORING_STATE_DISABLED && !mode->identity.resuming &&
             nuwax_sd_session_prefers_local());
        if (sd_admission) {
            const char *rejection = nuwax_sd_control_enable_rejection(mode->sd_local,
                mode->audio_session_id, &mode->sd_sync, event->audio_session_id);
            if (rejection) {
                ESP_LOGW(TAG, "SD monitoring enable rejected request=%s reason=%s", event->request_id, rejection);
                (void)send_event_command_ack(mode, ws, event, mode->state == NUWAX_MONITORING_STATE_ACTIVE, false, rejection, NULL);
                return;
            }
        }
#endif
        if (mode->state == NUWAX_MONITORING_STATE_ACTIVE) {
#if CONFIG_NUWAX_SD_SESSION_ENABLE
            if (mode->sd_local) {
                nuwax_sd_control_track_enabled(&mode->sd_sync, event->request_id);
                /* An accepted enable is itself an authoritative state sync.
                 * Legacy clouds may reject the parallel resume request. Do
                 * not keep rejecting newer App stops after agreeing enabled. */
                nuwax_sd_control_sync_connect(&mode->sd_sync, event->ws_connection_epoch,
                                               now_ms(), event->rx_order);
                const bool was_confirmed = mode->sd_sync.confirmed;
                nuwax_sd_control_sync_confirm(&mode->sd_sync, event->rx_order, event->audio_session_id);
                if (!was_confirmed)
                    ESP_LOGI(TAG, "SD monitoring control confirmed by cloud enable request=%s epoch=%lu session=%s",
                             event->request_id, (unsigned long)event->ws_connection_epoch, mode->audio_session_id);
            }
#endif
            if (ws && event->request_id[0] != '\0') {
                const esp_err_t ack_err =
                    send_event_command_ack(mode, ws, event, true, true, "already_active", mode->sd_local ? mode->audio_session_id : NULL);
                if (ack_err == ESP_OK && mode->pending_command_ack &&
                    mode->pending_command_enabled &&
                    strcmp(event->request_id,
                           mode->pending_command_request_id) == 0) {
                    clear_pending_command_ack(mode);
                }
            }
            return;
        }
        if (mode->state == NUWAX_MONITORING_STATE_STARTING &&
            mode->pending_command_enabled &&
            strcmp(event->request_id, mode->pending_command_request_id) == 0) {
            ESP_LOGI(TAG,
                     "duplicate cloud monitoring enable ignored request=%s",
                     event->request_id);
            return;
        }
        if (mode->state != NUWAX_MONITORING_STATE_DISABLED || !can_start(mode)) {
            (void)send_event_command_ack(mode, ws, event, false, false, "audio_mode_busy", NULL);
            ESP_LOGW(TAG,
                     "cloud monitoring enable rejected state=%s exclusive_owner_busy=%d",
                     state_name(mode->state),
                     can_start(mode) ? 0 : 1);
            return;
        }

        /* Preserve the normal foreground and immutable request outcome when
         * the card has already failed its write/readback qualification. */
#if CONFIG_NUWAX_SD_SESSION_ENABLE
        const esp_err_t storage_fault = sd_admission ? nuwax_sd_session_start_fault() : ESP_OK;
        if (storage_fault != ESP_OK) {
            nuwax_audio_command_begin(&mode->command_deadline, event->request_id, event->received_ms,
                event->ws_connection_epoch, event->local_stop_generation, esp_timer_get_time());
            nuwax_audio_command_decide(&mode->command_deadline, false, false, "sd_start_failed", NULL);
            ESP_LOGW(TAG, "SD monitoring start refused before handoff request=%s media_err=%s",
                event->request_id, esp_err_to_name(storage_fault));
            service_command_result(mode, ws);
            return;
        }
#endif
        reset_transient_state(mode);
#if CONFIG_NUWAX_SD_SESSION_ENABLE
        mode->sd_local = sd_admission;
        if (sd_admission) mode->sd_start_ms = now_ms();
#endif
        if (mode->sd_local)
            copy_text(mode->audio_session_id, sizeof(mode->audio_session_id), event->audio_session_id);
        else
            select_audio_session(mode, event->audio_session_id);
        mode->pending_command_ack = event->request_id[0] != '\0';
        mode->pending_command_enabled = true;
        copy_text(mode->pending_command_request_id,
                  sizeof(mode->pending_command_request_id),
                  event->request_id);
#if CONFIG_NUWAX_SD_SESSION_ENABLE
        nuwax_sd_control_sync_connect(&mode->sd_sync, event->ws_connection_epoch, now_ms(), event->rx_order);
        nuwax_sd_control_sync_confirm(&mode->sd_sync, event->rx_order, event->audio_session_id);
        mode->sd_sync.announced = true; /* Fresh cloud opening, not a reconnect report. */
        mode->sd_sync.reported_enabled = true;
        if (sd_admission && nuwax_sd_control_prepare_legacy_id(&mode->sd_sync, mode->audio_session_id, "monitor"))
            ESP_LOGI(TAG, "SD monitoring assigned recording ID request=%s cloud_session=%s local_session=%s",
                event->request_id, mode->sd_sync.cloud_session_id[0] ? mode->sd_sync.cloud_session_id : "<missing>",
                mode->audio_session_id);
#endif
        nuwax_audio_command_begin(&mode->command_deadline, event->request_id, event->received_ms,
            event->ws_connection_epoch, event->local_stop_generation, esp_timer_get_time());
        set_state(mode, NUWAX_MONITORING_STATE_STARTING, "cloud_enable");
        nuwax_monitoring_mode_service_command_deadline(mode, ws);
        if (mode->state != NUWAX_MONITORING_STATE_STARTING) return;
        if (mode->callbacks.is_playback_active &&
            mode->callbacks.is_playback_active(mode->callbacks.ctx) &&
            mode->callbacks.fade_and_cancel_playback) {
            mode->callbacks.fade_and_cancel_playback(mode->callbacks.ctx,
                                                     NUWAX_MONITORING_PLAYBACK_FADE_MS,
                                                     "monitoring_cloud_enable");
        }
        return;
    }

#if CONFIG_NUWAX_SD_SESSION_ENABLE
    if (reject_sd_cloud_disable(mode, ws, event)) return;
#endif
    if (mode->state == NUWAX_MONITORING_STATE_DISABLED) {
        if (ws && event->request_id[0] != '\0') {
            (void)send_event_command_ack(mode, ws, event, false, true, "already_disabled", NULL);
        }
        return;
    }
    if (mode->state == NUWAX_MONITORING_STATE_STOPPING &&
        event->request_id[0] != '\0' &&
        strcmp(event->request_id, mode->pending_command_request_id) == 0) {
        ESP_LOGI(TAG,
                 "duplicate cloud monitoring disable ignored request=%s",
                 event->request_id);
        return;
    }
    if (mode->state == NUWAX_MONITORING_STATE_STOPPING) {
        (void)send_event_command_ack(mode, ws, event, false, false, "audio_mode_busy", NULL);
        return;
    }
    retire_active_result(mode);
    if (mode->pending_command_ack && mode->pending_command_enabled &&
        mode->pending_command_request_id[0] != '\0' && ws &&
        nuwax_ws_is_connected(ws)) {
        if (nuwax_audio_command_matches(&mode->command_deadline, mode->pending_command_request_id)) {
            nuwax_audio_command_decide(&mode->command_deadline, false, false, "superseded_by_disable", NULL);
            service_command_result(mode, ws);
        } else {
            (void)nuwax_monitoring_protocol_send_command_ack(ws, mode->pending_command_request_id,
                false, false, "superseded_by_disable");
        }
    }
    if (event->request_id[0]) {
        nuwax_audio_command_begin(&mode->command_deadline, event->request_id, event->received_ms,
            event->ws_connection_epoch, event->local_stop_generation, esp_timer_get_time());
        mode->command_deadline.enabled = false;
        mode->command_deadline.prepared = true;
    }
    begin_cloud_stop(mode, event);
}

static void drain_events(nuwax_monitoring_mode_t *mode, nuwax_ws_client_t *ws) {
    if (!mode || !mode->events) {
        return;
    }
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    if (mode->sd_local && ws && nuwax_ws_is_connected(ws))
        nuwax_sd_control_sync_connect(&mode->sd_sync, nuwax_ws_connection_epoch(ws), now_ms(),
                                      atomic_load_explicit(&mode->control_rx_order, memory_order_relaxed));
#endif
    nuwax_monitoring_event_t event;
    uint32_t drained = 0U;
    while (drained < NUWAX_MONITORING_MAX_EVENTS_PER_LOOP &&
           xQueueReceive(mode->events, &event, 0) == pdTRUE) {
        drained++;
        if (event_matches_connection(&event, ws)) {
            nuwax_monitoring_mode_service_command_deadline(mode, ws);
            handle_event(mode, ws, &event);
            nuwax_monitoring_mode_service_command_deadline(mode, ws);
        }
        complete_queued_enable_event(mode, &event);
    }
}

void nuwax_monitoring_mode_sync_current_state(nuwax_monitoring_mode_t *mode,
                                             nuwax_ws_client_t *ws)
{
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    if (!mode || !mode->initialized || !ws || !nuwax_ws_is_connected(ws)) return;
    /* Do not execute controls from the reporting service. A control at the
     * queue head keeps its original owner/handoff path; ACKs behind it wait. */
    nuwax_monitoring_event_t event;
    for (unsigned i = 0U; i < 4U && mode->events &&
         xQueuePeek(mode->events, &event, 0) == pdTRUE &&
         is_state_sync_ack(&event); ++i) {
        if (xQueueReceive(mode->events, &event, 0) != pdTRUE) break;
        complete_queued_enable_event(mode, &event);
        if (event_matches_connection(&event, ws)) (void)handle_sd_sync_ack(mode, &event);
    }
    service_sd_cloud_sync(mode, ws);
#else
    (void)mode; (void)ws;
#endif
}

static void send_ui_close_request(nuwax_monitoring_mode_t *mode, nuwax_ws_client_t *ws)
{
    if (!mode->pending_ui_close) return;
    if (!mode->close_request_id[0] && mode->stop_reason[0])
        copy_text(mode->close_reason, sizeof(mode->close_reason), mode->stop_reason);
    if (!ws || !nuwax_ws_is_connected(ws)) {
        if (!mode->close_survives_disconnect) mode->pending_ui_close = false;
        return;
    }
    const uint32_t now = now_ms(), epoch = nuwax_ws_connection_epoch(ws);
    if (mode->close_survives_disconnect && epoch != mode->close_request_epoch)
        mode->close_request_id[0] = '\0';
    if (!mode->close_request_id[0]) {
        snprintf(mode->close_request_id, sizeof(mode->close_request_id), "monitoring_close_%lu_%08lx",
            (unsigned long)++mode->request_sequence, (unsigned long)esp_random());
        mode->close_request_epoch = epoch;
        mode->close_started_ms = now;
        mode->close_retry_ms = now;
        mode->close_attempts = 0;
        if (!mode->close_reason[0]) copy_text(mode->close_reason, sizeof(mode->close_reason), "capture_done");
    }
    if (epoch != mode->close_request_epoch || mode->close_attempts >= 5U ||
        (uint32_t)(now - mode->close_started_ms) >= 5000U) {
        mode->pending_ui_close = false; return;
    }
    if ((int32_t)(now - mode->close_retry_ms) < 0) return;
    ++mode->close_attempts;
    const esp_err_t err = nuwax_monitoring_protocol_send_request(ws, mode->close_request_id,
        false, mode->close_reason);
    mode->close_retry_ms = now_ms() + NUWAX_AUDIO_COMMAND_RETRY_MS;
    if (err == ESP_OK) mode->pending_ui_close = false;
    ESP_LOGI(TAG, "local close request=%s attempt=%u err=%s", mode->close_request_id,
        mode->close_attempts, esp_err_to_name(err));
}

static const char *start_failure_message(const nuwax_monitoring_mode_t *mode) {
    if (mode && (!strcmp(mode->stop_reason, "audio_session_required") ||
                 !strcmp(mode->stop_reason, "invalid_audio_session_id"))) return mode->stop_reason;
    if (mode && strcmp(mode->stop_reason, "playback_stop_failed") == 0) {
        return "playback_stop_failed";
    }
    return "capture_failed";
}

static void finish_state(nuwax_monitoring_mode_t *mode,
                         nuwax_ws_client_t *ws,
                         bool capture_ok) {
    if (!mode) {
        return;
    }
    if (mode->sd_local && mode->state == NUWAX_MONITORING_STATE_ACTIVE) {
        mode->pending_ui_close = true;
        mode->close_survives_disconnect = true;
        mode->close_request_id[0] = '\0';
    }
    retire_active_result(mode);
    send_ui_close_request(mode, ws);
    if (mode->pending_command_ack && mode->pending_command_request_id[0] != '\0') {
        bool stopped_ok = capture_ok;
#if CONFIG_NUWAX_SD_SESSION_ENABLE
        if (mode->sd_local && !mode->pending_command_enabled)
            stopped_ok = nuwax_sd_session_stopped();
#endif
        const bool command_ok = !mode->pending_command_enabled && stopped_ok;
        const char *message = mode->pending_command_enabled
                                  ? start_failure_message(mode)
                                  : (stopped_ok ? "disabled" : "capture_failed");
        esp_err_t ack_err = ESP_OK;
        if (nuwax_audio_command_matches(&mode->command_deadline, mode->pending_command_request_id)) {
            nuwax_audio_command_decide(&mode->command_deadline, false, command_ok, message, NULL);
            service_command_result(mode, ws);
        } else if (ws && nuwax_ws_is_connected(ws)) {
            ack_err = nuwax_monitoring_protocol_send_command_ack(ws, mode->pending_command_request_id,
                false, command_ok, message);
        }
        if (ack_err != ESP_OK) {
            ESP_LOGW(TAG,
                     "monitoring final command ACK failed request=%s err=%s",
                     mode->pending_command_request_id,
                     esp_err_to_name(ack_err));
        }
    }

    char final_reason[NUWAX_MONITORING_REASON_BYTES] = {0};
    copy_text(final_reason,
              sizeof(final_reason),
              mode->stop_reason[0] ? mode->stop_reason : "capture_done");
    const bool retry_start = mode->identity.resuming && !capture_ok &&
                            mode->capture_frames == 0U &&
                            strcmp(final_reason, "cloud_stop") != 0 &&
                            strcmp(final_reason, "ui_close") != 0;
    if (!retry_start && (!mode->offline_resume_required || (ws && nuwax_ws_is_connected(ws)))) {
        nuwax_audio_mode_identity_clear(&mode->identity);
    }
    reset_transient_state(mode);
    set_state(mode,
              capture_ok ? NUWAX_MONITORING_STATE_DISABLED
                         : NUWAX_MONITORING_STATE_ERROR,
              final_reason);
    if (!capture_ok) {
        (void)nuwax_device_error_report_enqueue(
            "error", "monitoring", "capture", ESP_FAIL,
            "monitoring", 0U, NULL);
        set_state(mode, NUWAX_MONITORING_STATE_DISABLED, "capture_error_reset");
    }
}

void nuwax_monitoring_mode_service_command_deadline(nuwax_monitoring_mode_t *mode, nuwax_ws_client_t *ws)
{
    if (!mode || !mode->initialized) return;
    if (mode->state == NUWAX_MONITORING_STATE_DISABLED) send_ui_close_request(mode, ws);
    nuwax_audio_command_deadline_t *c = &mode->command_deadline;
    /* An admitted stop must complete locally even if its connection is lost. */
    const bool closing = !c->enabled && mode->state == NUWAX_MONITORING_STATE_STOPPING;
    if (c->tracked && !c->decided && !closing) {
        const bool stale = !ws || !nuwax_ws_is_connected(ws) ||
            nuwax_ws_connection_epoch(ws) != c->epoch ||
            atomic_load(&mode->local_stop_generation) != c->local_stop_generation;
        bool failed = nuwax_audio_command_expired(c, esp_timer_get_time()) || stale;
        if (!failed && !c->prepared && mode->state == NUWAX_MONITORING_STATE_STARTING) {
            esp_err_t err = ESP_OK;
#if CONFIG_NUWAX_SD_SESSION_ENABLE
            if (mode->sd_local && !mode->sd_sync.identity_ready) {
                err = nuwax_sd_session_poll_identity(NUWAX_OFFLINE_AUDIO_MODE_MONITORING,
                    mode->audio_session_id, nuwax_audio_command_execution_deadline(c));
                if (err == ESP_ERR_INVALID_STATE && nuwax_sd_control_rotate_used_id(
                        &mode->sd_sync, mode->audio_session_id, "monitor")) {
                    ESP_LOGI(TAG, "SD monitoring replaced used recording ID request=%s local_session=%s",
                        c->request_id, mode->audio_session_id);
                    err = ESP_ERR_NOT_FINISHED;
                }
                if (err == ESP_OK) mode->sd_sync.identity_ready = true;
            }
#endif
            if (err == ESP_OK && mode->callbacks.prepare_cloud_start)
                err = mode->callbacks.prepare_cloud_start(mode->callbacks.ctx, ws, mode->sd_local, mode->audio_session_id);
            if (err == ESP_OK) {
                c->prepared = true;
                /* Keep preflight time out of the SD readiness window, while
                 * retaining the original total command execution deadline. */
                if (mode->sd_local) mode->sd_start_ms = now_ms();
            }
            else if (err != ESP_ERR_NOT_FINISHED) failed = true;
            failed = failed || nuwax_audio_command_expired(c, esp_timer_get_time()) ||
                !nuwax_ws_is_connected(ws) || nuwax_ws_connection_epoch(ws) != c->epoch ||
                atomic_load(&mode->local_stop_generation) != c->local_stop_generation;
        }
        if (failed) {
            const bool cancelled_start = c->enabled;
            (void)cancelled_start;
            nuwax_audio_command_decide(c, false, false, "mode_handoff_failed", NULL);
            ESP_LOGW(TAG, "command cancelled request=%s elapsed_ms=%lu capture=%d",
                c->request_id, (unsigned long)((esp_timer_get_time() - c->received_us) / 1000), mode->capture_running);
            clear_pending_command_ack(mode);
            mode->offline_resume_required = false;
            nuwax_audio_mode_identity_clear(&mode->identity);
            if (mode->capture_running) {
#if CONFIG_NUWAX_SD_SESSION_ENABLE
                if (mode->sd_local) nuwax_sd_session_stop(!cancelled_start);
#endif
                set_stop_reason(mode, "mode_handoff_failed");
                set_state(mode, NUWAX_MONITORING_STATE_STOPPING, "mode_handoff_failed");
            } else {
                reset_to_disabled(mode, "mode_handoff_failed", false);
            }
        }
    }
    service_command_result(mode, ws);
}

static esp_err_t fail_capture_start(nuwax_monitoring_mode_t *mode,
                                    nuwax_ws_client_t *ws,
                                    esp_err_t err,
                                    const char *reason) {
    if (!mode) {
        return err;
    }
    set_stop_reason(mode, reason ? reason : "capture_failed");
    (void)nuwax_device_error_report_enqueue(
        "error", "monitoring", reason && reason[0] ? reason : "capture_start",
        err, "monitoring", 0U, NULL);
    finish_state(mode, ws, false);
    return err;
}

static void *monitoring_alloc(size_t bytes) {
    void *ptr = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) {
        ptr = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    }
    return ptr;
}

static void monitoring_free(void *ptr) {
    if (ptr) {
        heap_caps_free(ptr);
    }
}

static uint8_t frame_flags(bool first, bool final) {
    uint8_t flags = 0U;
    if (first) {
        flags |= NUWAX_AUDIO_FLAG_START;
    }
    if (final) {
        flags |= NUWAX_AUDIO_FLAG_END;
    }
    return flags;
}

#if CONFIG_NUWAX_AUDIO_SOURCE_I2S_MIC


static const char *capture_phase_name(nuwax_monitoring_capture_phase_t phase) {
    switch (phase) {
        case NUWAX_MONITORING_CAPTURE_INACTIVE:
            return "inactive";
        case NUWAX_MONITORING_CAPTURE_STREAMING:
            return "streaming";
        case NUWAX_MONITORING_CAPTURE_PAUSE_PENDING:
            return "pause_pending";
        case NUWAX_MONITORING_CAPTURE_PAUSED:
            return "paused";
        case NUWAX_MONITORING_CAPTURE_RESUME_PENDING:
            return "resume_pending";
        default:
            return "unknown";
    }
}

static void set_capture_phase(nuwax_monitoring_mode_t *mode,
                              nuwax_monitoring_capture_phase_t phase,
                              const char *reason) {
    if (!mode) {
        return;
    }
    (void)reason;
    const nuwax_monitoring_capture_phase_t previous =
        atomic_exchange_explicit(&mode->capture_phase,
                                 phase,
                                 memory_order_acq_rel);
    if (previous != phase) {
        ESP_LOGI(TAG,
                 "monitoring capture phase %s -> %s reason=%s",
                 capture_phase_name(previous),
                 capture_phase_name(phase),
                 reason ? reason : "-");
    }
}

static bool capture_phase_allows_uplink(
    nuwax_monitoring_capture_phase_t phase) {
    return phase == NUWAX_MONITORING_CAPTURE_STREAMING ||
           phase == NUWAX_MONITORING_CAPTURE_PAUSE_PENDING;
}

static bool capture_phase_buffers_preroll(
    nuwax_monitoring_capture_phase_t phase) {
    return phase == NUWAX_MONITORING_CAPTURE_PAUSED ||
           phase == NUWAX_MONITORING_CAPTURE_RESUME_PENDING;
}

static bool monitoring_resume_speech_confirmed(
    const monitoring_speech_detector_t *detector) {
    return detector &&
           detector->consecutive_speech_frames >=
               CONFIG_NUWAX_MONITORING_RESUME_CONFIRM_FRAMES;
}

static bool monitoring_stream_activity_confirmed(
    const monitoring_speech_detector_t *detector) {
    return detector && nuwax_monitoring_stream_activity_confirmed(
                           detector->consecutive_speech_frames,
                           CONFIG_NUWAX_MONITORING_SPEECH_CONFIRM_FRAMES,
                           CONFIG_NUWAX_MONITORING_SPEECH_REFRESH_FRAMES);
}

static esp_err_t monitoring_capture_control_send(
    nuwax_monitoring_mode_t *mode,
    nuwax_ws_client_t *ws,
    monitoring_capture_control_t *control) {
    if (!mode || !ws || !control || !control->active ||
        control->request_id[0] == '\0' ||
        control->audio_session_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    const bool retry = control->attempts > 0U;
    const esp_err_t err = nuwax_monitoring_protocol_send_capture_control(
        ws,
        control->request_id,
        control->pause,
        control->audio_session_id,
        control->pause ? "no_speech" : "speech_detected",
        control->silence_ms);
    if (err != ESP_OK) {
        return err;
    }
    control->attempts++;
    control->deadline_ms =
        now_ms() + CONFIG_NUWAX_MONITORING_CAPTURE_ACK_TIMEOUT_MS;
    if (retry) {
        mode->capture_control_retries++;
    }
    ESP_LOGI(TAG,
             "monitoring capture %s request sent request=%s session=%s attempt=%lu",
             control->pause ? "pause" : "resume",
             control->request_id,
             control->audio_session_id,
             (unsigned long)control->attempts);
    return ESP_OK;
}

static esp_err_t monitoring_capture_control_begin(
    nuwax_monitoring_mode_t *mode,
    nuwax_ws_client_t *ws,
    monitoring_capture_control_t *control,
    bool pause,
    const char *audio_session_id,
    uint32_t silence_ms) {
    if (!mode || !control || !audio_session_id || audio_session_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    memset(control, 0, sizeof(*control));
    control->active = true;
    control->pause = pause;
    control->ws_connection_epoch =
        ws && nuwax_ws_is_connected(ws) ? nuwax_ws_connection_epoch(ws) : 0U;
    if (control->ws_connection_epoch == 0U) {
        control->active = false;
        return ESP_ERR_INVALID_STATE;
    }
    control->silence_ms = silence_ms;
    make_capture_control_request_id(mode, pause, control->request_id);
    copy_text(control->audio_session_id,
              sizeof(control->audio_session_id),
              audio_session_id);
    if (pause) {
        mode->pause_requests++;
    } else {
        mode->resume_requests++;
    }
    return monitoring_capture_control_send(mode, ws, control);
}

static bool monitoring_capture_control_take_ack(
    nuwax_monitoring_mode_t *mode,
    const monitoring_capture_control_t *control,
    nuwax_monitoring_event_t *ack_out) {
    if (!mode || !mode->capture_control_acks || !control || !control->active ||
        !ack_out) {
        return false;
    }
    nuwax_monitoring_event_t ack = {0};
    bool matched = false;
    while (xQueueReceive(mode->capture_control_acks, &ack, 0) == pdTRUE) {
        const bool action_matches =
            strcmp(ack.state, control->pause ? "pause" : "resume") == 0;
        if (ack.ws_connection_epoch == control->ws_connection_epoch &&
            strcmp(ack.request_id, control->request_id) == 0 &&
            strcmp(ack.audio_session_id, control->audio_session_id) == 0 &&
            action_matches) {
            if (!matched) {
                *ack_out = ack;
                matched = true;
            } else {
                ESP_LOGI(TAG,
                         "duplicate monitoring capture ACK ignored request=%s state=%s",
                         ack.request_id,
                         ack.state);
            }
            continue;
        }
        ESP_LOGW(TAG,
                 "stale monitoring capture ACK ignored request=%s expected=%s state=%s",
                 ack.request_id,
                 control->request_id,
                 ack.state);
    }
    return matched;
}

static void monitoring_preroll_reset(monitoring_preroll_t *preroll) {
    if (!preroll) {
        return;
    }
    preroll->count = 0U;
    preroll->write_index = 0U;
}

static void monitoring_capture_policy_set_phase(
    nuwax_monitoring_mode_t *mode,
    monitoring_capture_policy_t *policy,
    nuwax_monitoring_capture_phase_t phase,
    const char *reason) {
    if (!policy) {
        return;
    }
    policy->phase = phase;
    set_capture_phase(mode, phase, reason);
}

static void monitoring_capture_control_rollback(
    nuwax_monitoring_mode_t *mode,
    monitoring_capture_control_t *control,
    monitoring_capture_policy_t *policy,
    monitoring_speech_detector_t *detector,
    monitoring_preroll_t *preroll,
    uint32_t resume_guard_frames,
    size_t *current_samples,
    const char *reason) {
    if (!mode || !control || !policy) {
        return;
    }

    const bool pause = control->pause;
    if (pause) {
        policy->silent_frames = 0U;
        monitoring_capture_policy_set_phase(
            mode,
            policy,
            NUWAX_MONITORING_CAPTURE_STREAMING,
            reason ? reason : "pause_control_failed");
    } else {
        mode->audio_session_id[0] = '\0';
        if (current_samples) {
            *current_samples = 0U;
        }
        policy->resume_guard_remaining_frames = resume_guard_frames;
        policy->resume_quiet_frames = 0U;
        policy->resume_rearmed = false;
        monitoring_preroll_reset(preroll);
        monitoring_capture_policy_set_phase(
            mode,
            policy,
            NUWAX_MONITORING_CAPTURE_PAUSED,
            reason ? reason : "resume_control_failed");
    }
    monitoring_speech_detector_reset_decision(detector);
    ESP_LOGW(TAG,
             "monitoring capture %s control rolled back to %s reason=%s",
             pause ? "pause" : "resume",
             capture_phase_name(policy->phase),
             reason ? reason : "control_failed");
    memset(control, 0, sizeof(*control));
}

static void monitoring_preroll_push(monitoring_preroll_t *preroll,
                                    const int16_t *samples,
                                    bool speech) {
    if (!preroll || !preroll->samples || !preroll->speech || !samples ||
        preroll->capacity_frames == 0U || preroll->frame_samples == 0U) {
        return;
    }
    memcpy(&preroll->samples[preroll->write_index * preroll->frame_samples],
           samples,
           preroll->frame_samples * sizeof(int16_t));
    preroll->speech[preroll->write_index] = speech ? 1U : 0U;
    preroll->write_index =
        (preroll->write_index + 1U) % preroll->capacity_frames;
    if (preroll->count < preroll->capacity_frames) {
        preroll->count++;
    }
}

static esp_err_t monitoring_stream_enqueue(monitoring_stream_t *stream,
                                           nuwax_monitoring_mode_t *mode,
                                           const int16_t *samples,
                                           size_t sample_count,
                                           bool speech) {
    if (!stream || !mode || !stream->uplink_ready || !samples ||
        sample_count == 0U) {
        return ESP_ERR_INVALID_STATE;
    }
    const nuwax_monitoring_capture_phase_t phase =
        nuwax_monitoring_mode_capture_phase(mode);
    if (!capture_phase_allows_uplink(phase)) {
        ESP_LOGE(TAG,
                 "monitoring uplink rejected outside streaming phase=%s",
                 capture_phase_name(phase));
        return ESP_ERR_INVALID_STATE;
    }

    if (stream->have_pending) {
        uint8_t flags = frame_flags(stream->pending.sequence == stream->first_sequence, false);
        if (stream->pending.speech) {
            flags |= NUWAX_AUDIO_FLAG_VAD_SPEECH;
        }
        const esp_err_t enqueue_err = nuwax_audio_uplink_enqueue_frame(
            stream->uplink,
#if CONFIG_NUWAX_VOICE_UPLINK_CODEC_OPUS
            &stream->opus_encoder,
#else
            NULL,
#endif
            stream->pending.samples,
            stream->pending.sample_count,
            stream->pending.sequence,
            flags,
            false);
        if (enqueue_err != ESP_OK) {
            mode->dropped_frames++;
            return enqueue_err;
        }
        mode->uploaded_frames++;
    }

    memcpy(stream->pending.samples, samples, sample_count * sizeof(int16_t));
    stream->pending.sample_count = sample_count;
    stream->pending.sequence = stream->next_sequence++;
    stream->pending.speech = speech;
    stream->have_pending = true;

    return ESP_OK;
}

static esp_err_t monitoring_preroll_flush(monitoring_preroll_t *preroll,
                                          monitoring_stream_t *stream,
                                          nuwax_monitoring_mode_t *mode) {
    if (!preroll || !stream || !mode) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t oldest =
        (preroll->write_index + preroll->capacity_frames - preroll->count) %
        preroll->capacity_frames;
    for (size_t i = 0U; i < preroll->count; ++i) {
        const size_t index = (oldest + i) % preroll->capacity_frames;
        const esp_err_t err = monitoring_stream_enqueue(
            stream,
            mode,
            &preroll->samples[index * preroll->frame_samples],
            preroll->frame_samples,
            preroll->speech[index] != 0U);
        if (err != ESP_OK) {
            return err;
        }
    }
    monitoring_preroll_reset(preroll);
    return ESP_OK;
}

static esp_err_t monitoring_stream_open(monitoring_stream_t *stream,
                                        nuwax_monitoring_mode_t *mode,
                                        nuwax_ws_client_t *ws,
                                        size_t frame_samples,
                                        const char *request_id,
                                        const char *audio_session_id) {
    if (!stream || !mode || !ws || !nuwax_ws_is_connected(ws)) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t ws_epoch = nuwax_ws_connection_epoch(ws);
    if (ws_epoch == 0U) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Pause/resume and reconnect both open a fresh listen transport. */
    stream->next_sequence = 0U;
    stream->first_sequence = stream->next_sequence;
    stream->have_pending = false;
    stream->listen_started = false;

    stream->ws_epoch = ws_epoch;
    if (audio_session_id && audio_session_id[0] != '\0') {
        select_audio_session(mode, audio_session_id);
        nuwax_audio_mode_identity_remember(&mode->identity, mode->audio_session_id);
    } else {
        make_audio_session_id(mode);
    }

    esp_err_t err = ESP_OK;
#if CONFIG_NUWAX_VOICE_UPLINK_CODEC_OPUS
    err = nuwax_opus_encoder_init(&stream->opus_encoder,
                                  CONFIG_NUWAX_VOICE_SAMPLE_RATE,
                                  CONFIG_NUWAX_VOICE_CHANNELS,
                                  CONFIG_NUWAX_VOICE_FRAME_DURATION_MS);
    if (err != ESP_OK) {
        goto prepare_failed;
    }
    stream->opus_ready = true;
    if (nuwax_opus_encoder_frame_samples(&stream->opus_encoder) != frame_samples) {
        err = ESP_ERR_INVALID_SIZE;
        goto prepare_failed;
    }
    err = nuwax_audio_uplink_ensure_encoded_frame_capacity(
        nuwax_opus_encoder_output_bytes(&stream->opus_encoder));
    if (err != ESP_OK) {
        goto prepare_failed;
    }
#else
    (void)frame_samples;
#endif

    stream->uplink = nuwax_audio_uplink_create();
    if (!stream->uplink) {
        err = ESP_ERR_NO_MEM;
        goto prepare_failed;
    }
    err = nuwax_audio_uplink_start_if_epoch(stream->uplink, ws, ws_epoch);
    if (err != ESP_OK) {
        goto prepare_failed;
    }
    stream->uplink_ready = true;
    err = nuwax_audio_uplink_bind_terminal_recovery(
        stream->uplink,
        "monitoring",
        mode->audio_session_id,
        NULL,
        NUWAX_AUDIO_TERMINAL_RECOVERY_DEFAULT_ATTEMPT_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "monitoring terminal recovery binding failed before listen start session=%s err=%s",
                 mode->audio_session_id,
                 esp_err_to_name(err));
        const esp_err_t discard_err =
            nuwax_audio_uplink_discard_uncommitted(stream->uplink);
        if (discard_err != ESP_OK) {
            ESP_LOGE(TAG,
                     "monitoring prepared uplink cleanup failed after recovery bind error: %s",
                     esp_err_to_name(discard_err));
        }
        stream->uplink_ready = false;
        goto prepare_failed;
    }

    err = nuwax_session_send_listen_with_audio_session_until_if_epoch(
        ws,
        "start",
        "monitoring",
        mode->audio_session_id,
        request_id,
        esp_timer_get_time() + NUWAX_MONITORING_CONTROL_SEND_TIMEOUT_US,
        ws_epoch);
    if (err != ESP_OK) {
        const esp_err_t discard_err =
            nuwax_audio_uplink_discard_uncommitted(stream->uplink);
        if (discard_err != ESP_OK) {
            (void)nuwax_ws_request_close_if_epoch(ws, ws_epoch);
            ESP_LOGE(TAG,
                     "monitoring uncommitted uplink could not be discarded: %s",
                     esp_err_to_name(discard_err));
            /* The sender is heap-owned; detach it for asynchronous cleanup
             * instead of unwinding a live task or restarting the device. */
            (void)nuwax_audio_uplink_destroy_owned(&stream->uplink);
        }
        if (stream->uplink) {
            (void)nuwax_audio_uplink_destroy_owned(&stream->uplink);
        }
        stream->uplink_ready = false;
        goto prepare_failed;
    }
    stream->listen_started = true;

    return ESP_OK;

prepare_failed:
#if CONFIG_NUWAX_VOICE_UPLINK_CODEC_OPUS
    if (stream->opus_ready) {
        nuwax_opus_encoder_deinit(&stream->opus_encoder);
        stream->opus_ready = false;
    }
#endif
    if (stream->uplink) {
        (void)nuwax_audio_uplink_destroy_owned(&stream->uplink);
    }
    mode->audio_session_id[0] = '\0';
    stream->ws_epoch = 0U;
    return err;
}

static esp_err_t monitoring_stream_close(monitoring_stream_t *stream,
                                         nuwax_monitoring_mode_t *mode,
                                         nuwax_ws_client_t *ws) {
    if (!stream || !mode) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = ESP_OK;
    bool end_committed = false;
    const uint32_t ws_epoch = stream->ws_epoch;
    if (stream->have_pending && stream->uplink_ready) {
        uint8_t flags = frame_flags(stream->pending.sequence == stream->first_sequence, true);
        if (stream->pending.speech) {
            flags |= NUWAX_AUDIO_FLAG_VAD_SPEECH;
        }
        const esp_err_t final_err = nuwax_audio_uplink_enqueue_frame(
            stream->uplink,
#if CONFIG_NUWAX_VOICE_UPLINK_CODEC_OPUS
            &stream->opus_encoder,
#else
            NULL,
#endif
            stream->pending.samples,
            stream->pending.sample_count,
            stream->pending.sequence,
            flags,
            true);
        if (final_err != ESP_OK) {
            mode->dropped_frames++;
            result = final_err;
        } else {
            mode->uploaded_frames++;
        }
    }

    if (stream->uplink_ready) {
        const esp_err_t finish_err = nuwax_audio_uplink_finish(stream->uplink);
        end_committed = nuwax_audio_uplink_end_committed(stream->uplink);
        if (result == ESP_OK && finish_err != ESP_OK) {
            result = finish_err;
        }
        if (finish_err != ESP_OK &&
            !nuwax_audio_terminal_recovery_pending() && ws &&
            ws_epoch != 0U) {
            (void)nuwax_ws_request_close_if_epoch(ws, ws_epoch);
        }
        nuwax_audio_uplink_stats_t stats = {0};
        nuwax_audio_uplink_get_stats(stream->uplink, &stats);
        ESP_LOGI(TAG,
                 "monitoring stream closed session=%s queued=%lu sent=%lu dropped=%lu finish=%s",
                 mode->audio_session_id,
                 (unsigned long)stats.queued_frames,
                 (unsigned long)stats.sent_frames,
                 (unsigned long)stats.dropped_frames,
                 esp_err_to_name(finish_err));
        (void)nuwax_audio_uplink_destroy_owned(&stream->uplink);
        stream->uplink_ready = false;
    }
#if CONFIG_NUWAX_VOICE_UPLINK_CODEC_OPUS
    if (stream->opus_ready) {
        nuwax_opus_encoder_deinit(&stream->opus_encoder);
        stream->opus_ready = false;
    }
#endif
    if (stream->listen_started && end_committed &&
        !nuwax_audio_terminal_recovery_pending() && ws &&
        nuwax_ws_is_connected(ws) && ws_epoch != 0U &&
        nuwax_ws_connection_epoch(ws) == ws_epoch) {
        const esp_err_t stop_err =
            nuwax_session_send_listen_stop_retry_if_epoch(
                ws,
                "monitoring",
                mode->audio_session_id,
                NULL,
                (uint32_t)(NUWAX_MONITORING_CONTROL_SEND_TIMEOUT_US / 1000LL),
                ws_epoch);
        if (result == ESP_OK && stop_err != ESP_OK) {
            result = stop_err;
        }
        if (stop_err != ESP_OK) {
            const esp_err_t recovery_arm_err =
                nuwax_audio_terminal_recovery_arm(
                    ws,
                    ws_epoch,
                    NULL,
                    0U,
                    true,
                    "monitoring",
                    mode->audio_session_id,
                    NULL,
                    NUWAX_AUDIO_TERMINAL_RECOVERY_DEFAULT_ATTEMPT_TIMEOUT_MS);
            if (recovery_arm_err != ESP_OK) {
                (void)nuwax_ws_request_close_if_epoch(ws, ws_epoch);
            }
        }
    } else if (stream->listen_started && !end_committed) {
        if (!nuwax_audio_terminal_recovery_pending() &&
            ws && ws_epoch != 0U) {
            (void)nuwax_ws_request_close_if_epoch(ws, ws_epoch);
        }
        ESP_LOGW(TAG,
                 "monitoring listen stop suppressed because binary END was not committed session=%s epoch=%lu",
                 mode->audio_session_id,
                 (unsigned long)ws_epoch);
    }
    stream->listen_started = false;

    stream->ws_epoch = 0U;
    stream->have_pending = false;
    stream->next_sequence = 0U;
    mode->audio_session_id[0] = '\0';
    return result;
}
#endif

static esp_err_t capture_session(nuwax_monitoring_mode_t *mode,
                                 nuwax_ws_client_t *ws) {
#if !CONFIG_NUWAX_AUDIO_SOURCE_I2S_MIC
    ESP_LOGW(TAG, "monitoring capture requires CONFIG_NUWAX_AUDIO_SOURCE_I2S_MIC");
    return fail_capture_start(mode, ws, ESP_ERR_NOT_SUPPORTED, "i2s_mic_disabled");
#else
    if (!mode || !ws || !nuwax_ws_is_connected(ws)) {
        return fail_capture_start(mode, ws, ESP_ERR_INVALID_STATE, "capture_invalid_state");
    }
    const size_t frame_samples = NUWAX_MONITORING_FRAME_SAMPLES;
    if (frame_samples == 0U ||
        frame_samples > CONFIG_NUWAX_VOICE_MAX_SAMPLES_PER_FRAME) {
        return fail_capture_start(mode, ws, ESP_ERR_INVALID_SIZE, "frame_size_invalid");
    }

    if (mode->callbacks.is_playback_active) {
        const uint32_t settle_deadline =
            now_ms() + NUWAX_MONITORING_PLAYBACK_SETTLE_TIMEOUT_MS;
        while (mode->callbacks.is_playback_active(mode->callbacks.ctx) &&
               !deadline_expired(now_ms(), settle_deadline) &&
               nuwax_ws_is_connected(ws)) {
            if (mode->callbacks.poll_controls) {
                mode->callbacks.poll_controls(mode->callbacks.ctx, ws);
            }
            drain_events(mode, ws);
            if (mode->state == NUWAX_MONITORING_STATE_STOPPING ||
                mode->state == NUWAX_MONITORING_STATE_DISABLED) {
                finish_state(mode, ws, true);
                return ESP_OK;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (mode->callbacks.is_playback_active(mode->callbacks.ctx)) {
            ESP_LOGE(TAG,
                     "monitoring playback did not stop before capture deadline");
            return fail_capture_start(mode,
                                      ws,
                                      ESP_ERR_TIMEOUT,
                                      "playback_stop_failed");
        }
    }

    const size_t preroll_frames =
        (CONFIG_NUWAX_MONITORING_PREROLL_MS +
         CONFIG_NUWAX_VOICE_FRAME_DURATION_MS - 1U) /
        CONFIG_NUWAX_VOICE_FRAME_DURATION_MS;
    const uint32_t silence_limit_frames =
        (CONFIG_NUWAX_MONITORING_SILENCE_STOP_MS +
         CONFIG_NUWAX_VOICE_FRAME_DURATION_MS - 1U) /
        CONFIG_NUWAX_VOICE_FRAME_DURATION_MS;
    const uint32_t resume_guard_frames =
        (CONFIG_NUWAX_MONITORING_RESUME_GUARD_MS +
         CONFIG_NUWAX_VOICE_FRAME_DURATION_MS - 1U) /
        CONFIG_NUWAX_VOICE_FRAME_DURATION_MS;
    if (preroll_frames == 0U || silence_limit_frames == 0U ||
        CONFIG_NUWAX_MONITORING_RESUME_CONFIRM_FRAMES == 0U ||
        CONFIG_NUWAX_MONITORING_RESUME_QUIET_FRAMES == 0U ||
        preroll_frames > SIZE_MAX / frame_samples ||
        preroll_frames * frame_samples > SIZE_MAX / sizeof(int16_t)) {
        return fail_capture_start(mode, ws, ESP_ERR_INVALID_SIZE, "monitoring_config_invalid");
    }

    esp_err_t err = ESP_OK;
    nuwax_mic_input_t mic = {0};
    int16_t *current = (int16_t *)monitoring_alloc(frame_samples * sizeof(int16_t));
    int16_t *pending = (int16_t *)monitoring_alloc(frame_samples * sizeof(int16_t));
    int16_t *preroll_samples = (int16_t *)monitoring_alloc(
        preroll_frames * frame_samples * sizeof(int16_t));
    uint8_t *preroll_speech = (uint8_t *)monitoring_alloc(preroll_frames);
    if (!current || !pending || !preroll_samples || !preroll_speech) {
        monitoring_free(current);
        monitoring_free(pending);
        monitoring_free(preroll_samples);
        monitoring_free(preroll_speech);
        return fail_capture_start(mode,
                                  ws,
                                  ESP_ERR_NO_MEM,
                                  "capture_buffer_alloc_failed");
    }

    bool mic_ready = false;
    make_audio_session_id(mode);
#if CONFIG_NUWAX_OFFLINE_AUDIO_ENABLE && !CONFIG_NUWAX_SD_SESSION_ENABLE
    nuwax_offline_voice_t voice_archive = {0};
    bool voice_archive_ready = false;
    const esp_err_t voice_init_err =
        nuwax_offline_voice_init(&voice_archive, mode->audio_session_id,
                                 CONFIG_NUWAX_VOICE_SAMPLE_RATE,
                                 CONFIG_NUWAX_VOICE_FRAME_DURATION_MS);
    if (voice_init_err == ESP_OK) {
        voice_archive_ready = true;
    } else {
        /* Cloud monitoring may continue when the optional local archive
         * cannot reserve PSRAM, but never silently claim that local capture
         * is active. Keep the degradation observable without changing the
         * cloud control protocol or preventing the online stream. */
        ESP_LOGW(TAG,
                 "monitoring local voice archive unavailable err=%s; continuing cloud-only",
                 esp_err_to_name(voice_init_err));
        (void)nuwax_device_error_report_enqueue(
            "warning", "monitoring", "offline_voice_archive_unavailable",
            voice_init_err, mode->audio_session_id, 0U, NULL);
    }
#endif
    size_t current_samples = 0U;
    uint32_t read_discontinuities = 0U;
    uint32_t consecutive_read_discontinuities = 0U;
    uint64_t discarded_partial_samples = 0U;
    monitoring_capture_policy_t policy = {
        .phase = NUWAX_MONITORING_CAPTURE_INACTIVE,
    };
    monitoring_speech_detector_t detector = {0};
    monitoring_capture_control_t capture_control = {0};
    monitoring_preroll_t preroll = {
        .samples = preroll_samples,
        .speech = preroll_speech,
        .frame_samples = frame_samples,
        .capacity_frames = preroll_frames,
    };
    monitoring_stream_t stream = {
        .pending = {
            .samples = pending,
        },
    };
    char first_stream_request_id[NUWAX_MONITORING_REQUEST_ID_BYTES] = {0};
    copy_text(first_stream_request_id,
              sizeof(first_stream_request_id),
              mode->pending_command_request_id);

    monitoring_speech_detector_init(&detector);
    mode->capture_running = true;

    if (mode->state == NUWAX_MONITORING_STATE_STOPPING || mode->state == NUWAX_MONITORING_STATE_DISABLED ||
        nuwax_audio_command_expired(&mode->command_deadline, esp_timer_get_time())) {
        nuwax_audio_command_decide(&mode->command_deadline, false, false, "mode_handoff_failed", NULL);
        set_stop_reason(mode, "mode_handoff_failed");
        mode->offline_resume_required = false;
        nuwax_audio_mode_identity_clear(&mode->identity);
        err = ESP_ERR_TIMEOUT;
        goto cleanup;
    }
    err = monitoring_stream_open(&stream,
                                 mode,
                                 ws,
                                 frame_samples,
                                 first_stream_request_id[0] != '\0'
                                     ? first_stream_request_id
                                     : NULL,
                                 NULL);
    if (err != ESP_OK) {
        set_stop_reason(mode, "monitoring_stream_start_failed");
        goto cleanup;
    }
    first_stream_request_id[0] = '\0';
    monitoring_capture_policy_set_phase(mode,
                                        &policy,
                                        NUWAX_MONITORING_CAPTURE_STREAMING,
                                        "stream_opened");
    err = mode->callbacks.acquire_mic
        ? mode->callbacks.acquire_mic(mode->callbacks.ctx, &mic, NUWAX_MIC_OWNER_MONITORING_CAPTURE)
        : nuwax_mic_input_init_with_owner(&mic, NUWAX_MIC_OWNER_MONITORING_CAPTURE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "monitoring mic init failed: %s", esp_err_to_name(err));
        set_stop_reason(mode, "mic_init_failed");
        goto cleanup;
    }
    mic_ready = true;
    if (mode->state == NUWAX_MONITORING_STATE_STOPPING || mode->state == NUWAX_MONITORING_STATE_DISABLED ||
        nuwax_audio_command_expired(&mode->command_deadline, esp_timer_get_time())) {
        nuwax_audio_command_decide(&mode->command_deadline, false, false, "mode_handoff_failed", NULL);
        set_stop_reason(mode, "mode_handoff_failed");
        mode->offline_resume_required = false;
        nuwax_audio_mode_identity_clear(&mode->identity);
        err = ESP_ERR_TIMEOUT;
        goto cleanup;
    }


    set_state(mode, NUWAX_MONITORING_STATE_ACTIVE, "streaming");
    try_send_active_command_ack(mode, ws, true);

    uint32_t last_successful_read_ms = now_ms();
    while (nuwax_ws_is_connected(ws) &&
           mode->state != NUWAX_MONITORING_STATE_STOPPING) {
        if (mode->callbacks.poll_controls) {
            mode->callbacks.poll_controls(mode->callbacks.ctx, ws);
        }
        drain_events(mode, ws);
        if (mode->state == NUWAX_MONITORING_STATE_STOPPING) {
            break;
        }
        try_send_active_command_ack(mode, ws, false);

        if (capture_control.active) {
            nuwax_monitoring_event_t control_ack = {0};
            if (monitoring_capture_control_take_ack(mode,
                                                    &capture_control,
                                                    &control_ack)) {
                if (!control_ack.ok) {
                    ESP_LOGW(TAG,
                             "monitoring capture %s rejected request=%s reason=%s",
                             capture_control.pause ? "pause" : "resume",
                             capture_control.request_id,
                             control_ack.reason[0] ? control_ack.reason
                                                   : "cloud_rejected");
                    monitoring_capture_control_rollback(
                        mode,
                        &capture_control,
                        &policy,
                        &detector,
                        &preroll,
                        resume_guard_frames,
                        &current_samples,
                        control_ack.reason[0] ? control_ack.reason
                                              : "cloud_rejected");
                } else if (capture_control.pause) {
                    ESP_LOGI(TAG,
                             "monitoring capture pause ACK received request=%s session=%s",
                             capture_control.request_id,
                             capture_control.audio_session_id);
                    err = monitoring_stream_close(&stream, mode, ws);
                    if (err != ESP_OK) {
                        set_stop_reason(mode, "monitoring_stream_stop_failed");
                        set_state(mode,
                                  NUWAX_MONITORING_STATE_STOPPING,
                                  mode->stop_reason);
                        break;
                    }
                    policy.silent_frames = 0U;
                    current_samples = 0U;
                    policy.resume_guard_remaining_frames = resume_guard_frames;
                    policy.resume_quiet_frames = 0U;
                    policy.resume_rearmed = false;
                    monitoring_speech_detector_reset_decision(&detector);
                    monitoring_preroll_reset(&preroll);
                    monitoring_capture_policy_set_phase(
                        mode,
                        &policy,
                        NUWAX_MONITORING_CAPTURE_PAUSED,
                        "pause_acknowledged_and_stream_drained");
                    memset(&capture_control, 0, sizeof(capture_control));
                    ESP_LOGI(TAG,
                             "monitoring capture paused guard_ms=%u resume_confirm_ms=%u",
                             (unsigned int)CONFIG_NUWAX_MONITORING_RESUME_GUARD_MS,
                             (unsigned int)(
                                 CONFIG_NUWAX_MONITORING_RESUME_CONFIRM_FRAMES *
                                 CONFIG_NUWAX_VOICE_FRAME_DURATION_MS));
                } else {
                    ESP_LOGI(TAG,
                             "monitoring capture resume ACK received request=%s session=%s",
                             capture_control.request_id,
                             capture_control.audio_session_id);
                    err = monitoring_stream_open(&stream,
                                                 mode,
                                                 ws,
                                                 frame_samples,
                                                 NULL,
                                                 capture_control.audio_session_id);
                    if (err == ESP_OK) {
                        monitoring_capture_policy_set_phase(
                            mode,
                            &policy,
                            NUWAX_MONITORING_CAPTURE_STREAMING,
                            "resume_acknowledged_and_stream_opened");
                        err = monitoring_preroll_flush(&preroll,
                                                       &stream,
                                                       mode);
                    }
                    if (err != ESP_OK) {
                        set_stop_reason(mode, "monitoring_stream_resume_failed");
                        set_state(mode,
                                  NUWAX_MONITORING_STATE_STOPPING,
                                  mode->stop_reason);
                        break;
                    }
                    policy.silent_frames = 0U;
                    policy.resume_guard_remaining_frames = 0U;
                    policy.resume_quiet_frames = 0U;
                    policy.resume_rearmed = false;
                    memset(&capture_control, 0, sizeof(capture_control));
                }
            } else if (deadline_expired(now_ms(), capture_control.deadline_ms)) {
                if (capture_control.attempts >=
                    CONFIG_NUWAX_MONITORING_CAPTURE_ACK_MAX_RETRIES) {
                    ESP_LOGW(TAG,
                             "monitoring capture %s ACK timeout request=%s attempts=%lu",
                             capture_control.pause ? "pause" : "resume",
                             capture_control.request_id,
                             (unsigned long)capture_control.attempts);
                    monitoring_capture_control_rollback(
                        mode,
                        &capture_control,
                        &policy,
                        &detector,
                        &preroll,
                        resume_guard_frames,
                        &current_samples,
                        capture_control.pause ? "pause_ack_timeout"
                                              : "resume_ack_timeout");
                } else {
                    err = monitoring_capture_control_send(mode,
                                                          ws,
                                                          &capture_control);
                    if (err != ESP_OK) {
                        monitoring_capture_control_rollback(
                            mode,
                            &capture_control,
                            &policy,
                            &detector,
                            &preroll,
                            resume_guard_frames,
                            &current_samples,
                            "capture_control_retry_failed");
                        err = ESP_OK;
                    }
                }
            }
        }

        const size_t samples_remaining = frame_samples - current_samples;
        size_t samples_read = 0U;
        esp_err_t read_err = nuwax_mic_input_read_pcm16(&mic,
                                                        &current[current_samples],
                                                        samples_remaining,
                                                        CONFIG_NUWAX_MIC_READ_TIMEOUT_MS,
                                                        &samples_read);
        if (samples_read > samples_remaining) {
            set_stop_reason(mode, "mic_frame_too_large");
            set_state(mode, NUWAX_MONITORING_STATE_STOPPING, mode->stop_reason);
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        if (read_err == ESP_ERR_INVALID_RESPONSE && samples_read == 0U) {
            read_discontinuities++;
            consecutive_read_discontinuities++;
            discarded_partial_samples += current_samples;
            current_samples = 0U;
            monitoring_speech_detector_reset_decision(&detector);
            monitoring_preroll_reset(&preroll);
#if CONFIG_NUWAX_OFFLINE_AUDIO_ENABLE && !CONFIG_NUWAX_SD_SESSION_ENABLE
            nuwax_offline_voice_reset(&voice_archive);
#endif
            mode->consecutive_read_timeouts++;

            const uint32_t stalled_ms =
                (uint32_t)(now_ms() - last_successful_read_ms);
            if (read_discontinuities == 1U ||
                (read_discontinuities %
                 NUWAX_MONITORING_DISCONTINUITY_LOG_INTERVAL) == 0U) {
                ESP_LOGW(TAG,
                         "monitoring microphone discontinuity recovered without leaving mode events=%lu consecutive=%lu discarded_partial_samples=%llu stalled_ms=%lu",
                         (unsigned long)read_discontinuities,
                         (unsigned long)consecutive_read_discontinuities,
                         (unsigned long long)discarded_partial_samples,
                         (unsigned long)stalled_ms);
            }
            if (stalled_ms >= NUWAX_MONITORING_READ_STALL_TIMEOUT_MS) {
                set_stop_reason(mode, "mic_read_discontinuity_stall");
                set_state(mode, NUWAX_MONITORING_STATE_STOPPING,
                          mode->stop_reason);
                err = ESP_ERR_TIMEOUT;
                break;
            }
            continue;
        }
        if (read_err != ESP_OK && read_err != ESP_ERR_TIMEOUT) {
            set_stop_reason(mode, "mic_read_failed");
            set_state(mode, NUWAX_MONITORING_STATE_STOPPING, mode->stop_reason);
            err = read_err;
            break;
        }
        if (samples_read == 0U) {
            mode->consecutive_read_timeouts++;
            if ((uint32_t)(now_ms() - last_successful_read_ms) >=
                NUWAX_MONITORING_READ_STALL_TIMEOUT_MS) {
                set_stop_reason(mode, "mic_read_timeout");
                set_state(mode, NUWAX_MONITORING_STATE_STOPPING, mode->stop_reason);
                err = ESP_ERR_TIMEOUT;
                break;
            }
            continue;
        }
        mode->consecutive_read_timeouts = 0U;
        if (consecutive_read_discontinuities > 0U) {
            ESP_LOGI(TAG,
                     "monitoring microphone stream recovered consecutive_discontinuities=%lu total_discontinuities=%lu",
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
        uint32_t avg_abs = 0U;
        uint32_t peak = 0U;
        const bool speech = monitoring_detect_speech(
            &detector, current, samples_read, &avg_abs, &peak);
        const bool resume_speech_confirmed =
            monitoring_resume_speech_confirmed(&detector);
        const bool stream_activity_confirmed =
            monitoring_stream_activity_confirmed(&detector);
#if CONFIG_NUWAX_OFFLINE_AUDIO_ENABLE && !CONFIG_NUWAX_SD_SESSION_ENABLE
        /* Runs once per physical frame, independently of cloud ACK/phase.
         * Uplink preroll replay must never duplicate these archive samples. */
        if (voice_archive_ready) {
            nuwax_offline_voice_process(&voice_archive, current, samples_read,
                                        speech, mode->identity.next_capture_sequence, now_ms());
        }
#endif
        mode->capture_frames++;
        mode->identity.next_capture_sequence++;

        if (capture_phase_buffers_preroll(policy.phase)) {
            monitoring_preroll_push(&preroll, current, speech);
            if (policy.phase == NUWAX_MONITORING_CAPTURE_PAUSED &&
                policy.resume_guard_remaining_frames > 0U) {
                policy.resume_guard_remaining_frames--;
                if (policy.resume_guard_remaining_frames == 0U) {
                    monitoring_speech_detector_reset_decision(&detector);
                    ESP_LOGI(TAG,
                             "monitoring resume guard complete; waiting for quiet rearm");
                }
            } else if (policy.phase == NUWAX_MONITORING_CAPTURE_PAUSED &&
                       !policy.resume_rearmed) {
                if (!speech) {
                    if (policy.resume_quiet_frames < UINT32_MAX) {
                        policy.resume_quiet_frames++;
                    }
                } else {
                    policy.resume_quiet_frames = 0U;
                }
                if (policy.resume_quiet_frames >=
                    CONFIG_NUWAX_MONITORING_RESUME_QUIET_FRAMES) {
                    policy.resume_rearmed = true;
                    monitoring_speech_detector_reset_decision(&detector);
                    ESP_LOGI(TAG,
                             "monitoring quiet rearm complete quiet_ms=%u; waiting for new speech",
                             (unsigned int)(
                                 CONFIG_NUWAX_MONITORING_RESUME_QUIET_FRAMES *
                                 CONFIG_NUWAX_VOICE_FRAME_DURATION_MS));
                }
            } else if (policy.phase == NUWAX_MONITORING_CAPTURE_PAUSED &&
                       resume_speech_confirmed) {
                make_audio_session_id(mode);
                err = monitoring_capture_control_begin(
                    mode,
                    ws,
                    &capture_control,
                    false,
                    mode->audio_session_id,
                    0U);
                if (err != ESP_OK) {
                    monitoring_capture_control_rollback(
                        mode,
                        &capture_control,
                        &policy,
                        &detector,
                        &preroll,
                        resume_guard_frames,
                        &current_samples,
                        "resume_request_failed");
                    err = ESP_OK;
                } else {
                    monitoring_capture_policy_set_phase(
                        mode,
                        &policy,
                        NUWAX_MONITORING_CAPTURE_RESUME_PENDING,
                        "resume_request_sent");
                    ESP_LOGI(TAG,
                             "monitoring speech confirmed; resume ACK pending session=%s pre_roll_ms=%u avg_abs=%lu peak=%lu",
                             mode->audio_session_id,
                             (unsigned int)CONFIG_NUWAX_MONITORING_PREROLL_MS,
                             (unsigned long)avg_abs,
                             (unsigned long)peak);
                }
            }
        } else if (capture_phase_allows_uplink(policy.phase)) {
            err = monitoring_stream_enqueue(
                &stream, mode, current, samples_read, speech);
            if (err != ESP_OK) {
                set_stop_reason(mode, "uplink_enqueue_failed");
                set_state(mode, NUWAX_MONITORING_STATE_STOPPING,
                          mode->stop_reason);
                break;
            }
            if (policy.phase == NUWAX_MONITORING_CAPTURE_STREAMING) {
                if (stream_activity_confirmed) {
                    policy.silent_frames = 0U;
                } else if (policy.silent_frames < UINT32_MAX) {
                    /* Candidate bursts remain part of elapsed no-speech time.
                     * Only sustained activity may erase the history. */
                    policy.silent_frames++;
                }
            }
            if (policy.phase == NUWAX_MONITORING_CAPTURE_STREAMING &&
                policy.silent_frames >= silence_limit_frames) {
                ESP_LOGI(TAG,
                         "monitoring silence timeout; pause ACK requested session=%s silence_ms=%u",
                         mode->audio_session_id,
                         (unsigned int)CONFIG_NUWAX_MONITORING_SILENCE_STOP_MS);
                err = monitoring_capture_control_begin(
                    mode,
                    ws,
                    &capture_control,
                    true,
                    mode->audio_session_id,
                    CONFIG_NUWAX_MONITORING_SILENCE_STOP_MS);
                if (err != ESP_OK) {
                    monitoring_capture_control_rollback(
                        mode,
                        &capture_control,
                        &policy,
                        &detector,
                        &preroll,
                        resume_guard_frames,
                        &current_samples,
                        "pause_request_failed");
                    err = ESP_OK;
                } else {
                    monitoring_capture_policy_set_phase(
                        mode,
                        &policy,
                        NUWAX_MONITORING_CAPTURE_PAUSE_PENDING,
                        "pause_request_sent");
                }
            }
        } else {
            ESP_LOGE(TAG,
                     "monitoring invalid capture phase=%s while processing frame",
                     capture_phase_name(policy.phase));
            set_stop_reason(mode, "capture_phase_invalid");
            set_state(mode,
                      NUWAX_MONITORING_STATE_STOPPING,
                      mode->stop_reason);
            err = ESP_ERR_INVALID_STATE;
            break;
        }

        if ((mode->capture_frames % NUWAX_MONITORING_LOG_INTERVAL_FRAMES) == 0U) {
            const uint64_t silence_ms =
                (uint64_t)policy.silent_frames *
                (uint64_t)CONFIG_NUWAX_VOICE_FRAME_DURATION_MS;
            (void)silence_ms;
            ESP_LOGI(TAG,
                     "monitoring capture phase=%s frames=%lu uploaded=%lu dropped=%lu silence_ms=%" PRIu64 " speech_run=%lu avg_abs=%lu peak=%lu noise_floor=%lu energy=%d vad=%d vad_ok=%d vad_raw=%lu vad_qualified=%lu vad_rejected=%lu resume_guard=%lu resume_quiet=%lu resume_ready=%d",
                     capture_phase_name(policy.phase),
                     (unsigned long)mode->capture_frames,
                     (unsigned long)mode->uploaded_frames,
                     (unsigned long)mode->dropped_frames,
                     silence_ms,
                     (unsigned long)detector.consecutive_speech_frames,
                     (unsigned long)avg_abs,
                     (unsigned long)peak,
                     (unsigned long)detector.noise_floor_abs,
                     detector.last_energy_speech ? 1 : 0,
                     detector.last_vad_speech ? 1 : 0,
                     detector.last_vad_qualified ? 1 : 0,
                     (unsigned long)detector.raw_vad_speech_frames,
                     (unsigned long)detector.qualified_vad_speech_frames,
                     (unsigned long)detector.rejected_vad_speech_frames,
                     (unsigned long)policy.resume_guard_remaining_frames,
                     (unsigned long)policy.resume_quiet_frames,
                     policy.resume_rearmed ? 1 : 0);
        }
    }

    if (!nuwax_ws_is_connected(ws) && err == ESP_OK) {
        set_stop_reason(mode, "ws_disconnected");
        err = ESP_ERR_INVALID_STATE;
    }

cleanup:
    if (stream.listen_started || stream.uplink_ready || stream.have_pending) {
        const esp_err_t close_err = monitoring_stream_close(&stream, mode, ws);
        if (err == ESP_OK && close_err != ESP_OK) {
            set_stop_reason(mode, "monitoring_stream_stop_failed");
            err = close_err;
        }
    }
    if (mic_ready) {
        const esp_err_t mic_deinit_err = nuwax_mic_input_deinit(&mic);
        if (mic_deinit_err != ESP_OK) {
            if (err == ESP_OK) {
                err = mic_deinit_err;
                set_stop_reason(mode, "mic_deinit_failed");
                set_state(mode, NUWAX_MONITORING_STATE_STOPPING,
                          mode->stop_reason);
            } else {
                ESP_LOGW(TAG,
                         "monitoring microphone deinit also failed err=%s; preserving root reason=%s",
                         esp_err_to_name(mic_deinit_err),
                         mode->stop_reason[0] ? mode->stop_reason
                                              : "capture_failed");
            }
        }
    }
#if CONFIG_NUWAX_OFFLINE_AUDIO_ENABLE && !CONFIG_NUWAX_SD_SESSION_ENABLE
    nuwax_offline_voice_deinit(&voice_archive, err == ESP_OK);
#endif
    monitoring_speech_detector_deinit(&detector);
    monitoring_free(current);
    monitoring_free(pending);
    monitoring_free(preroll_samples);
    monitoring_free(preroll_speech);
    mode->capture_running = false;

    const bool ws_disconnected = !ws || !nuwax_ws_is_connected(ws);
    const bool intentional_stop =
        strcmp(mode->stop_reason, "ui_close") == 0 ||
        strcmp(mode->stop_reason, "cloud_stop") == 0 ||
        (mode->pending_command_ack && !mode->pending_command_enabled);
    const bool transport_boundary_reason =
        ws_disconnected && !intentional_stop;
    if (transport_boundary_reason) {
        mode->offline_resume_required = true;
        mode->identity.resuming = mode->identity.session_id[0] != '\0';
    }
    /* A WebSocket loss is the ownership boundary for the SD-only handoff,
     * not a monitoring capture fault.  Keep genuine MIC/SD failures visible,
     * but do not publish a transient ERROR state for the expected transport
     * teardown that precedes offline capture. */
    const bool offline_transport_handoff =
        ws_disconnected && mode->offline_resume_required &&
        transport_boundary_reason;
    if (offline_transport_handoff && err != ESP_OK) {
        ESP_LOGI(TAG,
                 "monitoring transport boundary reserved for offline handoff; suppressing transient capture error err=%s",
                 esp_err_to_name(err));
    }
    const bool capture_ok = err == ESP_OK || offline_transport_handoff;
    ESP_LOGI(TAG,
             "monitoring capture done ok=%d reason=%s frames=%lu uploaded=%lu dropped=%lu pauses=%lu resumes=%lu control_retries=%lu read_discontinuities=%lu discarded_partial_samples=%llu err=%s",
             capture_ok ? 1 : 0,
             mode->stop_reason[0] ? mode->stop_reason : "normal_stop",
             (unsigned long)mode->capture_frames,
             (unsigned long)mode->uploaded_frames,
             (unsigned long)mode->dropped_frames,
             (unsigned long)mode->pause_requests,
             (unsigned long)mode->resume_requests,
             (unsigned long)mode->capture_control_retries,
             (unsigned long)read_discontinuities,
             (unsigned long long)discarded_partial_samples,
             esp_err_to_name(err));
    finish_state(mode, ws, capture_ok);
    return err;
#endif
}


#if CONFIG_NUWAX_SD_SESSION_ENABLE
/* Connection-owner only, also called by the offline SD tick. A valid stop
 * already received for this scene must survive transport teardown. No starts
 * or unrelated request ACKs are executed on a disconnected transport. */
static void preserve_sd_stop_on_disconnect(nuwax_monitoring_mode_t *mode)
{
    if (!mode || !mode->sd_local ||
        (mode->state != NUWAX_MONITORING_STATE_ACTIVE &&
         mode->state != NUWAX_MONITORING_STATE_STARTING)) return;
    nuwax_monitoring_event_t event;
    while (mode->events && xQueueReceive(mode->events, &event, 0) == pdTRUE) {
        complete_queued_enable_event(mode, &event);
        if (event.type == NUWAX_MONITORING_EVENT_REQUEST_ACK) {
            (void)handle_sd_sync_ack(mode, &event);
            continue;
        }
        if (event.type != NUWAX_MONITORING_EVENT_CLOUD_DISABLE ||
            mode->state == NUWAX_MONITORING_STATE_STOPPING) continue;
        nuwax_audio_command_deadline_t *c = &mode->command_deadline;
        const char *rejection = NULL;
        if (!event.ws_connection_epoch || event.ws_connection_epoch !=
            atomic_load_explicit(&mode->control_rx_epoch, memory_order_acquire))
            rejection = "stale_connection";
        else if (event.local_stop_generation != atomic_load(&mode->local_stop_generation))
            rejection = "local_stop_superseded";
        else if ((uint32_t)(now_ms() - event.received_ms) >= NUWAX_AUDIO_COMMAND_EXECUTE_MS)
            rejection = "command_expired";
        else if (nuwax_audio_command_failed(c, event.request_id) ||
                 (nuwax_audio_command_matches(c, event.request_id) &&
                  (c->retired || c->enabled)))
            rejection = "retired_request";
        else if (event.request_id[0])
            rejection = nuwax_sd_control_disable_rejection(&mode->sd_sync,
                event.rx_order, event.received_ms, event.request_id,
                event.audio_session_id, mode->audio_session_id);
        if (rejection) {
            ESP_LOGW(TAG, "SD monitoring disconnected stop rejected request=%s reason=%s",
                     event.request_id, rejection);
            continue;
        }
        retire_active_result(mode);
        if (c->tracked && !c->decided)
            nuwax_audio_command_decide(c, false, false, "superseded_by_disable", NULL);
        clear_pending_command_ack(mode);
        if (event.request_id[0]) {
            nuwax_audio_command_begin(c, event.request_id, event.received_ms,
                event.ws_connection_epoch, event.local_stop_generation, esp_timer_get_time());
            c->enabled = false;
            c->prepared = true;
        }
        begin_cloud_stop(mode, &event);
        ESP_LOGI(TAG, "SD monitoring stop retained across disconnect request=%s session=%s",
                 event.request_id[0] ? event.request_id : "-", mode->audio_session_id);
    }
}
#endif

void nuwax_monitoring_mode_service_sd(nuwax_monitoring_mode_t *mode, nuwax_ws_client_t *ws)
{
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    if (!mode || !mode->initialized) return;
    if (!mode->sd_local) {
        if (mode->state != NUWAX_MONITORING_STATE_STARTING || !nuwax_sd_session_prefers_local() || mode->identity.resuming) return;
        mode->sd_local = true;
        mode->sd_start_ms = now_ms();
        /* A card can become available after cloud-command admission. Apply
         * the same temporary mapping before owner handoff/catalog validation. */
        if (mode->pending_command_enabled &&
            nuwax_sd_control_prepare_legacy_id(&mode->sd_sync, mode->audio_session_id, "monitor"))
            ESP_LOGI(TAG, "SD monitoring assigned recording ID after card detection request=%s cloud_session=%s local_session=%s",
                mode->pending_command_request_id,
                mode->sd_sync.cloud_session_id[0] ? mode->sd_sync.cloud_session_id : "<missing>", mode->audio_session_id);
        ESP_LOGI(TAG, "SD capture route selected ready=%d wait_limit_ms=5000", nuwax_sd_session_available());
        if (mode->command_deadline.tracked && !mode->command_deadline.decided)
            mode->command_deadline.prepared = false;
    }
    if (ws && nuwax_ws_is_connected(ws)) drain_events(mode, ws);
    else preserve_sd_stop_on_disconnect(mode);
    nuwax_monitoring_mode_service_command_deadline(mode, ws);
    if (!mode->sd_local) return;
    if (mode->state == NUWAX_MONITORING_STATE_STOPPING) {
        if (mode->capture_running) nuwax_sd_session_stop(true);
        if ((!mode->capture_running || !nuwax_sd_session_active()) && nuwax_sd_session_stopped()) {
            mode->capture_running = false;
            finish_state(mode, ws, nuwax_sd_session_complete());
        }
        return;
    }
    if (mode->state == NUWAX_MONITORING_STATE_STARTING && !mode->capture_running) {
        const esp_err_t storage_fault = nuwax_sd_session_start_fault();
        if (storage_fault != ESP_OK) {
            (void)fail_capture_start(mode, ws, storage_fault, "sd_start_failed");
            return;
        }
        if (mode->command_deadline.tracked && !mode->command_deadline.decided &&
            !mode->command_deadline.prepared) return;
        if (mode->callbacks.is_playback_active && mode->callbacks.is_playback_active(mode->callbacks.ctx)) {
            if ((uint32_t)(now_ms() - mode->sd_start_ms) >= 5000U)
                (void)fail_capture_start(mode, ws, ESP_ERR_TIMEOUT, "playback_stop_failed");
            return;
        }
        /* Admission assigns a recording ID even without a control ID. */
        if (mode->pending_command_enabled && !sd_audio_identity_valid(mode->audio_session_id)) {
            (void)fail_capture_start(mode, ws, ESP_ERR_INVALID_ARG,
                mode->audio_session_id[0] ? "invalid_audio_session_id" : "audio_session_required");
            return;
        }
        const int64_t start_deadline = mode->command_deadline.tracked && !mode->command_deadline.decided
            ? nuwax_audio_command_execution_deadline(&mode->command_deadline) : 0;
        esp_err_t err = nuwax_sd_session_prepare_start(NUWAX_OFFLINE_AUDIO_MODE_MONITORING, start_deadline);
        if (err == ESP_OK) err = start_deadline
            ? nuwax_sd_session_start_until(NUWAX_OFFLINE_AUDIO_MODE_MONITORING, mode->audio_session_id, start_deadline)
            : nuwax_sd_session_start(NUWAX_OFFLINE_AUDIO_MODE_MONITORING, mode->audio_session_id);
        /* Recheck after the bounded launch lock. If a task was admitted on
         * the boundary, retain its owner until its normal cancellation seals. */
        if (err == ESP_OK) mode->capture_running = true;
        nuwax_monitoring_mode_service_command_deadline(mode, ws);
        if (mode->state != NUWAX_MONITORING_STATE_STARTING) return;
        if (err == ESP_ERR_NOT_FINISHED && (uint32_t)(now_ms() - mode->sd_start_ms) < 5000U) return;
        if (err != ESP_OK) {
            (void)fail_capture_start(mode, ws, err, "sd_start_failed");
            return;
        }
        mode->capture_running = true;
        nuwax_audio_mode_identity_remember(&mode->identity, mode->audio_session_id);
        mode->identity.resuming = false;
    }
    if (mode->capture_running && !nuwax_sd_session_active()) {
        mode->capture_running = false;
        set_stop_reason(mode, nuwax_offline_audio_limit_reached() ? "duration_limit" : "sd_capture_ended");
        finish_state(mode, ws, nuwax_sd_session_complete() && nuwax_offline_audio_capture_health() != NUWAX_OFFLINE_CAPTURE_UNAVAILABLE);
        return;
    }
    if (mode->capture_running && mode->state == NUWAX_MONITORING_STATE_STARTING &&
        nuwax_offline_capture_health_is_recording(nuwax_offline_audio_capture_health())) {
        set_state(mode, NUWAX_MONITORING_STATE_ACTIVE, "sd_recording");
        nuwax_sd_session_release_start(NUWAX_OFFLINE_AUDIO_MODE_MONITORING);
        ESP_LOGI(TAG, "SD monitoring started session=%s network_independent=1", mode->audio_session_id);
    }
    try_send_active_command_ack(mode, ws, false);
    service_sd_cloud_sync(mode, ws);
#else
    (void)mode; (void)ws;
#endif
}

esp_err_t nuwax_monitoring_mode_init(
    nuwax_monitoring_mode_t *mode,
    const nuwax_monitoring_mode_callbacks_t *callbacks) {
    if (!mode) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(mode, 0, sizeof(*mode));
    if (callbacks) {
        mode->callbacks = *callbacks;
    }
    mode->events = xQueueCreate(NUWAX_MONITORING_EVENT_QUEUE_DEPTH,
                                sizeof(nuwax_monitoring_event_t));
    if (!mode->events) {
        return ESP_ERR_NO_MEM;
    }
    mode->capture_control_acks =
        xQueueCreate(NUWAX_MONITORING_ACK_QUEUE_DEPTH,
                     sizeof(nuwax_monitoring_event_t));
    if (!mode->capture_control_acks) {
        vQueueDelete(mode->events);
        mode->events = NULL;
        return ESP_ERR_NO_MEM;
    }
    atomic_init(&mode->state, NUWAX_MONITORING_STATE_DISABLED);
    atomic_init(&mode->capture_phase, NUWAX_MONITORING_CAPTURE_INACTIVE);
    atomic_init(&mode->queued_enable_events, 0U);
    atomic_init(&mode->queued_sync_acks, 0U);
    atomic_init(&mode->local_stop_generation, 0U);
    atomic_init(&mode->control_rx_order, 0U);
    atomic_init(&mode->control_rx_epoch, 0U);
    mode->initialized = true;
    return ESP_OK;
}

void nuwax_monitoring_mode_deinit(nuwax_monitoring_mode_t *mode) {
    if (!mode) {
        return;
    }
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    nuwax_sd_session_release_start(NUWAX_OFFLINE_AUDIO_MODE_MONITORING);
#endif
    if (mode->events) {
        vQueueDelete(mode->events);
    }
    if (mode->capture_control_acks) {
        vQueueDelete(mode->capture_control_acks);
    }
    memset(mode, 0, sizeof(*mode));
}

nuwax_monitoring_state_t nuwax_monitoring_mode_state(
    const nuwax_monitoring_mode_t *mode) {
    return mode ? mode->state : NUWAX_MONITORING_STATE_DISABLED;
}

nuwax_monitoring_capture_phase_t nuwax_monitoring_mode_capture_phase(
    const nuwax_monitoring_mode_t *mode) {
    return mode
               ? atomic_load_explicit(&mode->capture_phase,
                                      memory_order_acquire)
               : NUWAX_MONITORING_CAPTURE_INACTIVE;
}

bool nuwax_monitoring_mode_has_pending(const nuwax_monitoring_mode_t *mode) {
    if (!mode || !mode->initialized) return false;
    if (mode->state != NUWAX_MONITORING_STATE_DISABLED ||
        atomic_load_explicit(&mode->queued_enable_events, memory_order_acquire) > 0U) return true;
    const uint32_t queued = (mode->events ? (uint32_t)uxQueueMessagesWaiting(mode->events) : 0U);
    /* State-report ACKs do not reserve MIC or interrupt normal conversation.
     * Other commands/ACKs retain the existing owner-dispatch behavior. */
    return queued > atomic_load_explicit(&mode->queued_sync_acks, memory_order_acquire);
}

bool nuwax_monitoring_mode_has_audio_claim(
    const nuwax_monitoring_mode_t *mode) {
    if (!mode || !mode->initialized) {
        return false;
    }
    return nuwax_monitoring_mode_state(mode) !=
               NUWAX_MONITORING_STATE_DISABLED ||
           atomic_load_explicit(&mode->queued_enable_events,
                                memory_order_acquire) > 0U;
}

bool nuwax_monitoring_mode_blocks_ota(const nuwax_monitoring_mode_t *mode) {
    return nuwax_monitoring_mode_has_audio_claim(mode);
}

bool nuwax_monitoring_mode_should_preempt_normal(
    const nuwax_monitoring_mode_t *mode) {
    return nuwax_monitoring_mode_has_pending(mode);
}

esp_err_t nuwax_monitoring_mode_request_ui(nuwax_monitoring_mode_t *mode,
                                           nuwax_ws_client_t *ws,
                                           bool enabled) {
    if (!mode || !mode->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    if (enabled && !mode->identity.resuming && nuwax_sd_session_prefers_local() && mode->state == NUWAX_MONITORING_STATE_DISABLED && can_start(mode)) {
        reset_transient_state(mode);
        mode->sd_local = true;
        mode->sd_start_ms = now_ms();
        ESP_LOGI(TAG, "SD capture route selected ready=%d wait_limit_ms=5000", nuwax_sd_session_available());

        set_state(mode, NUWAX_MONITORING_STATE_STARTING, "local_sd_enable");
        if (mode->callbacks.fade_and_cancel_playback)
            mode->callbacks.fade_and_cancel_playback(mode->callbacks.ctx, 100U, "local_sd_enable");
        return ESP_OK;
    }
#endif
    if (!enabled) {
        /* Fence RX work already received, including a producer paused before
         * queue publication. The owner still drains each event exactly once. */
        atomic_fetch_add_explicit(&mode->local_stop_generation, 1U,
                                  memory_order_acq_rel);
        mode->offline_resume_required = false;
        if (mode->state == NUWAX_MONITORING_STATE_DISABLED) {
            nuwax_audio_mode_identity_clear(&mode->identity);
        }
        if (mode->state != NUWAX_MONITORING_STATE_DISABLED) {
            /* Local UI shutdown must also release an offline SD capture owner
             * after a reconnect boundary.  The run loop performs the actual
             * MIC/uplink teardown in order, so this only changes the state
             * machine and never blocks the UI caller. */
            mode->pending_ui_close = mode->sd_local;
            mode->close_survives_disconnect = mode->sd_local;
            mode->close_request_id[0] = '\0';
            set_stop_reason(mode, "ui_close");
            set_state(mode, NUWAX_MONITORING_STATE_STOPPING,
                      mode->stop_reason);
        }
        return ESP_OK;
    }
    if (!ws || !nuwax_ws_is_connected(ws) ||
        nuwax_ws_connection_epoch(ws) == 0U) {
        return ESP_ERR_INVALID_STATE;
    }
    if (mode->state != NUWAX_MONITORING_STATE_DISABLED) {
        return mode->state == NUWAX_MONITORING_STATE_ACTIVE
                   ? ESP_OK
                   : ESP_ERR_INVALID_STATE;
    }
    if (!can_start(mode)) {
        return ESP_ERR_INVALID_STATE;
    }

    char request_id[NUWAX_MONITORING_REQUEST_ID_BYTES] = {0};
    const uint32_t sequence = ++mode->request_sequence;
    snprintf(request_id, sizeof(request_id), "mon_open_%lu_%08lx",
             (unsigned long)sequence, (unsigned long)esp_random());
    const esp_err_t err = mode->identity.resuming
        ? nuwax_monitoring_protocol_send_resume_request(ws, request_id, mode->identity.session_id)
        : nuwax_monitoring_protocol_send_request(ws, request_id, true, "offline_audio_recovered");
    if (err == ESP_OK) {
        ESP_LOGI(TAG,
                 "monitoring resume request sent request=%s epoch=%lu",
                 request_id,
                 (unsigned long)nuwax_ws_connection_epoch(ws));
    } else {
        ESP_LOGW(TAG,
                 "monitoring resume request failed request=%s err=%s",
                 request_id,
                 esp_err_to_name(err));
    }
    return err;
}

bool nuwax_monitoring_mode_handle_protocol_text(nuwax_monitoring_mode_t *mode,
                                                nuwax_ws_client_t *ws,
                                                const char *payload,
                                                int len) {
    if (!mode || !mode->initialized) {
        return false;
    }
    const uint32_t local_stop_generation = atomic_load_explicit(
        &mode->local_stop_generation, memory_order_acquire);
    const uint32_t rx_order = atomic_fetch_add_explicit(&mode->control_rx_order, 1U, memory_order_relaxed) + 1U;
    const uint32_t received_ms = now_ms();
    nuwax_monitoring_protocol_event_t parsed = {0};
    if (!nuwax_monitoring_protocol_parse(payload, len, &parsed)) {
        return false;
    }

    if (parsed.type == NUWAX_MONITORING_PROTOCOL_EVENT_SET_COMMAND &&
        parsed.enabled && mode->state == NUWAX_MONITORING_STATE_DISABLED &&
        !can_start(mode)) {
        nuwax_monitoring_event_t rejected = {.received_ms = received_ms,
            .ws_connection_epoch = ws ? nuwax_ws_connection_epoch(ws) : 0U};
        copy_text(rejected.request_id, sizeof(rejected.request_id), parsed.request_id);
        (void)send_event_command_ack(NULL, ws, &rejected, false, false, "audio_mode_busy", NULL);
        return true;
    }

    nuwax_monitoring_event_t event = {0};
    event.local_stop_generation = local_stop_generation;
    event.rx_order = rx_order;
    event.received_ms = received_ms;
    if (parsed.type ==
        NUWAX_MONITORING_PROTOCOL_EVENT_CAPTURE_CONTROL_ACK) {
        event.type = NUWAX_MONITORING_EVENT_CAPTURE_CONTROL_ACK;
    } else if (parsed.type == NUWAX_MONITORING_PROTOCOL_EVENT_REQUEST_ACK) {
        event.type = NUWAX_MONITORING_EVENT_REQUEST_ACK;
    } else if (parsed.type == NUWAX_MONITORING_PROTOCOL_EVENT_SET_COMMAND) {
        event.type = parsed.enabled ? NUWAX_MONITORING_EVENT_CLOUD_ENABLE
                                    : NUWAX_MONITORING_EVENT_CLOUD_DISABLE;
    } else {
        return true;
    }
    event.ws_connection_epoch =
        ws && nuwax_ws_is_connected(ws) ? nuwax_ws_connection_epoch(ws) : 0U;
    if (event.ws_connection_epoch == 0U) {
        ESP_LOGW(TAG,
                 "monitoring protocol event dropped without active connection type=%d request=%s",
                 (int)event.type,
                 parsed.request_id[0] ? parsed.request_id : "-");
        return true;
    }
    copy_text(event.request_id, sizeof(event.request_id), parsed.request_id);
    copy_text(event.state, sizeof(event.state), parsed.state);
    copy_text(event.audio_session_id,
              sizeof(event.audio_session_id),
              parsed.audio_session_id);
    copy_text(event.reason, sizeof(event.reason), parsed.reason);
    event.ok = parsed.ok;
    if (event.type == NUWAX_MONITORING_EVENT_CLOUD_ENABLE || event.type == NUWAX_MONITORING_EVENT_CLOUD_DISABLE) {
        atomic_store_explicit(&mode->control_rx_epoch, event.ws_connection_epoch, memory_order_release);
        ESP_LOGI(TAG, "command received request=%s enabled=%d epoch=%lu target_session=%s",
                 event.request_id, event.type == NUWAX_MONITORING_EVENT_CLOUD_ENABLE,
                 (unsigned long)event.ws_connection_epoch,
                 event.audio_session_id[0] ? event.audio_session_id : "-");
    }
    if (!queue_event(mode, &event)) {
        if (is_state_sync_ack(&event)) {
            ESP_LOGW(TAG, "monitoring state-sync ACK dropped while queue full request=%s", event.request_id);
            return true;
        }
        if (event.type == NUWAX_MONITORING_EVENT_CAPTURE_CONTROL_ACK && ws &&
            event.ws_connection_epoch != 0U) {
            ESP_LOGE(TAG,
                     "monitoring ACK queue saturated request=%s; closing bound epoch",
                     event.request_id[0] ? event.request_id : "-");
            (void)nuwax_ws_request_close_if_epoch(
                ws, event.ws_connection_epoch);
        } else if (ws &&
            nuwax_ws_is_connected(ws)) {
            (void)send_event_command_ack(NULL, ws, &event, parsed.enabled, false, "control_queue_full", NULL);
        }
    }
    return true;
}

void nuwax_monitoring_mode_poll_control_events(nuwax_monitoring_mode_t *mode,
                                             nuwax_ws_client_t *ws)
{
    if (mode && mode->initialized && ws && nuwax_ws_is_connected(ws)) drain_events(mode, ws);
}

esp_err_t nuwax_monitoring_mode_run(nuwax_monitoring_mode_t *mode,
                                    nuwax_ws_client_t *ws) {
    if (!mode || !mode->initialized || !ws) {
        return ESP_ERR_INVALID_ARG;
    }
    while (nuwax_ws_is_connected(ws)) {
        if (mode->callbacks.poll_controls) {
            mode->callbacks.poll_controls(mode->callbacks.ctx, ws);
        }
        drain_events(mode, ws);
        nuwax_monitoring_mode_service_command_deadline(mode, ws);
        if (mode->state == NUWAX_MONITORING_STATE_STARTING && mode->command_deadline.tracked &&
            !mode->command_deadline.decided && !mode->command_deadline.prepared) {
            vTaskDelay(pdMS_TO_TICKS(20U)); return ESP_OK;
        }
#if CONFIG_NUWAX_SD_SESSION_ENABLE
        nuwax_monitoring_mode_service_sd(mode, ws);
        if (mode->sd_local) { vTaskDelay(pdMS_TO_TICKS(20U)); return ESP_OK; }
#endif
        if (mode->state == NUWAX_MONITORING_STATE_DISABLED) {
            return ESP_OK;
        }
        if (mode->state == NUWAX_MONITORING_STATE_STARTING ||
            mode->state == NUWAX_MONITORING_STATE_ACTIVE ||
            (mode->state == NUWAX_MONITORING_STATE_STOPPING &&
             mode->capture_running)) {
            return capture_session(mode, ws);
        }
        if (mode->state == NUWAX_MONITORING_STATE_STOPPING) {
            finish_state(mode, ws, true);
            return ESP_OK;
        }
        return ESP_ERR_INVALID_STATE;
    }
    nuwax_monitoring_mode_notify_ws_disconnected(mode);
    return ESP_ERR_INVALID_STATE;
}

esp_err_t nuwax_monitoring_mode_request_handoff_stop(
    nuwax_monitoring_mode_t *mode,
    nuwax_ws_client_t *ws,
    const char *reason) {
    if (!mode || !mode->initialized || !ws || !nuwax_ws_is_connected(ws)) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t ws_epoch = nuwax_ws_connection_epoch(ws);
    if (ws_epoch == 0U) {
        return ESP_ERR_INVALID_STATE;
    }
    if (mode->state == NUWAX_MONITORING_STATE_DISABLED) {
        if (!nuwax_monitoring_mode_has_pending(mode)) {
            return ESP_OK;
        }
        return nuwax_monitoring_mode_abort_pending_controls(
            mode, ws, ws_epoch,
            reason && reason[0] ? reason : "superseded_by_audio_mode");
    }
    if (mode->state == NUWAX_MONITORING_STATE_STOPPING) {
        return ESP_OK;
    }

    nuwax_monitoring_event_t event = {
        .type = NUWAX_MONITORING_EVENT_CLOUD_DISABLE,
        .ws_connection_epoch = ws_epoch,
    };
    copy_text(event.reason,
              sizeof(event.reason),
              reason && reason[0] ? reason : "superseded_by_audio_mode");
    if (!queue_event(mode, &event)) {
        ESP_LOGE(TAG,
                 "monitoring handoff stop queue full state=%s epoch=%lu reason=%s",
                 state_name(mode->state),
                 (unsigned long)ws_epoch,
                 event.reason);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG,
             "monitoring handoff stop queued state=%s epoch=%lu reason=%s",
             state_name(mode->state),
             (unsigned long)ws_epoch,
             event.reason);
    return ESP_OK;
}

esp_err_t nuwax_monitoring_mode_abort_pending_controls(
    nuwax_monitoring_mode_t *mode,
    nuwax_ws_client_t *ws,
    uint32_t ws_connection_epoch,
    const char *reason) {
    if (!mode || !mode->initialized || !ws ||
        !nuwax_ws_is_connected(ws) || ws_connection_epoch == 0U ||
        nuwax_ws_connection_epoch(ws) != ws_connection_epoch) {
        return ESP_ERR_INVALID_STATE;
    }
    if (mode->state != NUWAX_MONITORING_STATE_DISABLED) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = ESP_OK;
    nuwax_monitoring_event_t event = {0};
    if (mode->capture_control_acks) {
        while (xQueueReceive(mode->capture_control_acks, &event, 0) == pdTRUE) {
            ESP_LOGW(TAG,
                     "monitoring capture ACK discarded during reconnect recovery request=%s event_epoch=%lu active_epoch=%lu",
                     event.request_id[0] ? event.request_id : "-",
                     (unsigned long)event.ws_connection_epoch,
                     (unsigned long)ws_connection_epoch);
        }
    }
    if (mode->events) {
        for (unsigned i = 0U; i < NUWAX_MONITORING_EVENT_QUEUE_DEPTH; ++i) {
            if (xQueueReceive(mode->events, &event, 0) != pdTRUE) {
                break;
            }
            if (event.ws_connection_epoch != ws_connection_epoch) {
                ESP_LOGW(TAG,
                         "stale monitoring control discarded during reconnect recovery request=%s event_epoch=%lu active_epoch=%lu",
                         event.request_id[0] ? event.request_id : "-",
                          (unsigned long)event.ws_connection_epoch,
                          (unsigned long)ws_connection_epoch);
                complete_queued_enable_event(mode, &event);
                continue;
            }
            if ((event.type == NUWAX_MONITORING_EVENT_CLOUD_ENABLE ||
                 event.type == NUWAX_MONITORING_EVENT_CLOUD_DISABLE) &&
                event.request_id[0] != '\0') {
                const bool enabled =
                    event.type == NUWAX_MONITORING_EVENT_CLOUD_ENABLE;
                const esp_err_t ack_err =
                    send_event_command_ack(mode, ws, &event, enabled, false,
                        reason && reason[0] ? reason : "device_recovery_timeout", NULL);
                if (ack_err != ESP_OK && result == ESP_OK) {
                    result = ack_err;
                }
            }
            complete_queued_enable_event(mode, &event);
        }
    }
    if (nuwax_monitoring_mode_has_pending(mode) && result == ESP_OK) {
        result = ESP_ERR_TIMEOUT;
    }
    return result;
}

void nuwax_monitoring_mode_notify_ws_disconnected(nuwax_monitoring_mode_t *mode) {
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    preserve_sd_stop_on_disconnect(mode);
    /* Publish the nonblocking capture cancellation before any reconnect
     * work. The storage owner still drains/seals before a success ACK. */
    if (mode && mode->sd_local && mode->state == NUWAX_MONITORING_STATE_STOPPING &&
        mode->capture_running) nuwax_sd_session_stop(true);
#endif
    if (mode && !mode->close_survives_disconnect) mode->pending_ui_close = false;
    nuwax_monitoring_mode_service_command_deadline(mode, NULL);
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    if (mode && mode->sd_local) {
        clear_event_queue(mode);
        /* Keep the original opening ACK while this SD scene remains active. */
        return;
    }
#endif
    if (!mode || !mode->initialized) {
        return;
    }
    if (mode->state != NUWAX_MONITORING_STATE_DISABLED &&
        strcmp(mode->stop_reason, "cloud_stop") != 0 && strcmp(mode->stop_reason, "ui_close") != 0) {
        nuwax_audio_mode_identity_remember(&mode->identity, mode->audio_session_id);
        mode->identity.resuming = mode->identity.session_id[0] != '\0';
    }
    clear_event_queue(mode);
    reset_to_disabled(mode, "ws_disconnected", false);
}

bool nuwax_monitoring_mode_take_offline_resume(nuwax_monitoring_mode_t *mode)
{
    if (!mode) {
        return false;
    }
    const bool required = mode->offline_resume_required;
    mode->offline_resume_required = false;
    return required;
}

bool nuwax_monitoring_mode_offline_resume_pending(
    const nuwax_monitoring_mode_t *mode)
{
    return mode && mode->initialized && mode->offline_resume_required;
}
