#include "sd_audio_session.h"
#include "meeting_mode.h"

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
#include "meeting_protocol.h"
#include "session_protocol.h"
#if CONFIG_NUWAX_OFFLINE_AUDIO_ENABLE && !CONFIG_NUWAX_SD_SESSION_ENABLE
#include "offline_audio_spool.h"
#include "board/sd_card_driver.h"
#endif

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

static void retire_active_result(nuwax_meeting_mode_t *mode);
static void send_ui_close_request(nuwax_meeting_mode_t *mode, nuwax_ws_client_t *ws);

static void set_state(nuwax_meeting_mode_t *mode,
                      nuwax_meeting_state_t state,
                      const char *reason) {
    if (!mode) {
        return;
    }
    if (state != NUWAX_MEETING_STATE_ACTIVE) retire_active_result(mode);
    if (state == NUWAX_MEETING_STATE_STARTING) {
        /* A delayed, unscoped close must never stop a subsequent capture. */
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
    if (mode->identity.resuming && mode->identity.session_id[0]) {
        copy_text(mode->audio_session_id, sizeof(mode->audio_session_id), mode->identity.session_id);
    }
    if (mode->audio_session_id[0] != '\0') {
        nuwax_audio_mode_identity_remember(&mode->identity, mode->audio_session_id);
        return;
    }
    snprintf(mode->audio_session_id,
             sizeof(mode->audio_session_id),
             "meeting_stream_%lu_%08lx",
             (unsigned long)mode->request_sequence,
             (unsigned long)esp_random());
    nuwax_audio_mode_identity_remember(&mode->identity, mode->audio_session_id);
}

static void select_audio_session(nuwax_meeting_mode_t *mode, const char *requested)
{
    const char *selected = requested;
    if (mode->identity.resuming && mode->identity.session_id[0]) {
        selected = mode->identity.session_id;
        if (requested && requested[0] && strcmp(requested, selected) != 0) {
            ESP_LOGW(TAG, "meeting resume retains session=%s cloud_requested=%s",
                     selected, requested);
        }
    }
    copy_text(mode->audio_session_id, sizeof(mode->audio_session_id), selected);
}

static bool is_state_sync_ack(const nuwax_meeting_event_t *event) {
    static const char prefix[] = "meeting_sync_";
    return event && event->type == NUWAX_MEETING_EVENT_REQUEST_ACK &&
        strncmp(event->request_id, prefix, sizeof(prefix) - 1U) == 0;
}

static void complete_queued_enable_event(
    nuwax_meeting_mode_t *mode,
    const nuwax_meeting_event_t *event) {
    if (!mode || !event) return;
    if (is_state_sync_ack(event)) {
        uint32_t queued = atomic_load_explicit(&mode->queued_sync_acks, memory_order_acquire);
        while (queued > 0U) {
            if (atomic_compare_exchange_weak_explicit(&mode->queued_sync_acks,
                    &queued, queued - 1U, memory_order_acq_rel, memory_order_acquire)) return;
        }
        ESP_LOGE(TAG, "meeting state-sync ACK accounting underflow");
        return;
    }
    if (event->type != NUWAX_MEETING_EVENT_CLOUD_ENABLE) return;
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
        if (is_state_sync_ack(event))
            atomic_fetch_add_explicit(&mode->queued_sync_acks, 1U, memory_order_acq_rel);
        if (!mode->request_acks ||
            xQueueSend(mode->request_acks, event, 0) != pdTRUE) {
            complete_queued_enable_event(mode, event);
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
    unsigned drained = 0;
    if (mode->request_acks) {
        while (drained < 4U && xQueueReceive(mode->request_acks, &event, 0) == pdTRUE) {
            complete_queued_enable_event(mode, &event);
            ++drained;
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
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    nuwax_sd_session_release_start(NUWAX_OFFLINE_AUDIO_MODE_MEETING);
#endif
    mode->sd_local = false;
    mode->sd_start_ms = 0U;
    memset(&mode->sd_sync, 0, sizeof(mode->sd_sync));
    mode->capture_running = false;
    mode->cloud_authorized = false;
    mode->pending_command_ack = false;
    mode->pending_command_enabled = false;
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
    if (strcmp(reason, "ws_disconnected") != 0 &&
        (!mode->identity.resuming ||
         (strcmp(reason, "request_send_failed") != 0 &&
          strcmp(reason, "start_timeout") != 0))) {
        nuwax_audio_mode_identity_clear(&mode->identity);
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
    /* A local stop is independent of a delayed opening ACK. */
    mode->pending_ui_close = true;
    mode->close_survives_disconnect = mode->sd_local;
    mode->close_request_id[0] = '\0';
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

static esp_err_t send_event_command_ack(nuwax_meeting_mode_t *mode, nuwax_ws_client_t *ws,
    const nuwax_meeting_event_t *event, bool enabled, bool ok, const char *message, const char *session_id)
{
    const int64_t now = esp_timer_get_time();
    nuwax_audio_command_deadline_t *c = mode ? &mode->command_deadline : NULL;
    const bool same = c && nuwax_audio_command_matches(c, event->request_id);
    const int64_t received = same ? c->received_us :
        (now / 1000 - (uint32_t)((uint32_t)(now / 1000) - event->received_ms)) * 1000;
    const int64_t limit = received + (int64_t)NUWAX_AUDIO_COMMAND_ACK_MS * 1000;
    if (now >= limit || (same && c->attempts >= NUWAX_MEETING_COMMAND_ACK_MAX_ATTEMPTS)) return ESP_ERR_TIMEOUT;
    const int64_t deadline = limit - now < NUWAX_AUDIO_COMMAND_SEND_SLICE_US
        ? limit : now + NUWAX_AUDIO_COMMAND_SEND_SLICE_US;
    if (same) ++c->attempts;
    const esp_err_t err = nuwax_meeting_protocol_send_command_ack_until(ws, event->request_id,
        enabled, ok, message, session_id, deadline, event->ws_connection_epoch);
    if (same && err == ESP_OK) c->ack_pending = false;
    return err;
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

static void retire_active_result(nuwax_meeting_mode_t *mode)
{
    nuwax_audio_command_deadline_t *c = &mode->command_deadline;
    nuwax_audio_command_retire_active(c);
    nuwax_sd_control_retire_enabled(&mode->sd_sync, c, esp_timer_get_time());
    if (c->retired && nuwax_audio_command_matches(c, mode->pending_command_request_id))
        clear_pending_command_ack(mode);
}

static void service_command_result(nuwax_meeting_mode_t *mode, nuwax_ws_client_t *ws)
{
    nuwax_audio_command_deadline_t *c = &mode->command_deadline;
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    if (mode->sd_local && !nuwax_offline_audio_capture_running()) retire_active_result(mode);
#endif
    if (c->ok && c->enabled && (c->retired || mode->state != NUWAX_MEETING_STATE_ACTIVE ||
        !mode->capture_running || (c->session_id[0] && strcmp(c->session_id, mode->audio_session_id)))) {
        retire_active_result(mode);
        return;
    }
    if (!c->tracked || !c->decided || !c->ack_pending) return;
    const int64_t now = esp_timer_get_time();
    const int64_t deadline = nuwax_audio_command_send_deadline(c, now);
    if (!deadline || c->attempts >= NUWAX_MEETING_COMMAND_ACK_MAX_ATTEMPTS) {
        c->ack_pending = false;
        if (nuwax_audio_command_matches(c, mode->pending_command_request_id)) clear_pending_command_ack(mode);
        return;
    }
    if (ws && nuwax_ws_is_connected(ws) && c->ok &&
        (!c->enabled || mode->state == NUWAX_MEETING_STATE_ACTIVE))
        c->epoch = nuwax_ws_connection_epoch(ws);
    if (!ws || !nuwax_ws_is_connected(ws) || nuwax_ws_connection_epoch(ws) != c->epoch ||
        (c->attempts && (int32_t)(now_ms() - c->retry_at_ms) < 0)) return;
    ++c->attempts;
    const esp_err_t err = nuwax_meeting_protocol_send_command_ack_until(ws, c->request_id,
        c->enabled, c->ok, c->message, c->session_id[0] ? c->session_id : NULL, deadline, c->epoch);
    c->retry_at_ms = (uint32_t)(now / 1000) + NUWAX_AUDIO_COMMAND_RETRY_MS;
    if (nuwax_audio_command_matches(c, mode->pending_command_request_id)) {
        mode->pending_command_ack_attempts = c->attempts;
        mode->pending_command_ack_deadline_ms = c->retry_at_ms;
    }
    if (err == ESP_OK) {
        c->ack_pending = false;
        if (nuwax_audio_command_matches(c, mode->pending_command_request_id)) clear_pending_command_ack(mode);
        ESP_LOGI(TAG, "command result delivered request=%s enabled=%d ok=%d epoch=%lu attempts=%u",
                 c->request_id, c->enabled, c->ok, (unsigned long)c->epoch, c->attempts);
    }
    ESP_LOGI(TAG, "command result request=%s ok=%d elapsed_ms=%lu attempt=%u err=%s",
        c->request_id, c->ok, (unsigned long)((esp_timer_get_time() - c->received_us) / 1000),
        c->attempts, esp_err_to_name(err));
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
    if (nuwax_audio_command_matches(&mode->command_deadline, mode->pending_command_request_id)) {
        nuwax_meeting_mode_service_command_deadline(mode, ws);
        if (mode->state != NUWAX_MEETING_STATE_ACTIVE) return;
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
    const esp_err_t ack_err = nuwax_meeting_protocol_send_command_ack_with_session(
        ws,
        mode->pending_command_request_id,
        true,
        true,
        "active", mode->sd_local ? mode->audio_session_id : NULL);
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

#if CONFIG_NUWAX_SD_SESSION_ENABLE
static void service_sd_cloud_sync(nuwax_meeting_mode_t *mode, nuwax_ws_client_t *ws)
{
    if (!ws || !nuwax_ws_is_connected(ws)) return;
    const bool enabled = mode->state == NUWAX_MEETING_STATE_ACTIVE;
    if (mode->state != NUWAX_MEETING_STATE_DISABLED && !enabled) return;
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
        ESP_LOGW(TAG, "SD meeting control sync timeout epoch=%lu state=%s session=%s; local state retained, commands remain available",
                 (unsigned long)sync->epoch, enabled ? "enabled" : "disabled",
                 enabled ? mode->audio_session_id : "-");
    }
    if (!nuwax_sd_control_sync_due(sync, now)) return;
    if (!sync->request_id[0])
        snprintf(sync->request_id, sizeof(sync->request_id), "meeting_sync_%lu_%08lx",
                 (unsigned long)++mode->request_sequence, (unsigned long)esp_random());
    const esp_err_t err = enabled
        ? nuwax_meeting_protocol_send_resume_request(ws, sync->request_id, mode->audio_session_id)
        : nuwax_meeting_protocol_send_request(ws, sync->request_id, false, false, "device_state_sync");
    nuwax_sd_control_sync_note_send(sync, now_ms(), err == ESP_OK);
    ESP_LOGI(TAG, "SD meeting control sync request=%s epoch=%lu state=%s session=%s attempt=%u announced=%u send_failures=%u err=%s",
             sync->request_id, (unsigned long)sync->epoch, enabled ? "enabled" : "disabled",
             enabled ? mode->audio_session_id : "-", sync->attempts,
             (unsigned)sync->announced, (unsigned)sync->send_failures, esp_err_to_name(err));
}


static bool handle_sd_sync_ack(nuwax_meeting_mode_t *mode, const nuwax_meeting_event_t *event)
{
    nuwax_sd_control_sync_t *sync = &mode->sd_sync;
    if (!sync->request_id[0] ||
        event->ws_connection_epoch != sync->epoch ||
        strcmp(event->request_id, sync->request_id) != 0) return false;
    const bool accepted = event->ok &&
        strcmp(event->state, sync->reported_enabled ? "enabled" : "disabled") == 0;
    if (accepted)
        nuwax_sd_control_sync_confirm_reply(sync, event->rx_order, event->audio_session_id);
    ESP_LOGI(TAG, "SD meeting control sync reply request=%s epoch=%lu accepted=%d session=%s cloud_session=%s control_session=%s; capture retained",
             event->request_id, (unsigned long)sync->epoch, accepted,
             mode->audio_session_id, event->audio_session_id[0] ? event->audio_session_id : "-",
             sync->reply_session_id[0] ? sync->reply_session_id : "-");
    return true;
}

static bool reject_sd_cloud_disable(nuwax_meeting_mode_t *mode, nuwax_ws_client_t *ws,
                                    const nuwax_meeting_event_t *event)
{
    /* Handoffs are owner-generated events without a cloud request ID. */
    if (!mode->sd_local || !event->request_id[0] ||
        (mode->state != NUWAX_MEETING_STATE_ACTIVE && mode->state != NUWAX_MEETING_STATE_STARTING)) return false;
    const char *reason = event->local_stop_generation != atomic_load_explicit(
        &mode->local_stop_generation, memory_order_acquire) ? "local_stop_superseded" :
        nuwax_sd_control_disable_rejection(&mode->sd_sync, event->rx_order, event->received_ms,
            event->request_id, event->audio_session_id, mode->audio_session_id);
    if (!reason) return false;
    ESP_LOGW(TAG, "SD meeting cloud disable rejected request=%s epoch=%lu target_session=%s local_session=%s reason=%s",
             event->request_id, (unsigned long)event->ws_connection_epoch,
             event->audio_session_id[0] ? event->audio_session_id : "-",
             mode->audio_session_id[0] ? mode->audio_session_id : "-", reason);
    (void)send_event_command_ack(mode, ws, event, true, false, reason, NULL);
    return true;
}
#endif

static void handle_event(nuwax_meeting_mode_t *mode,
                         nuwax_ws_client_t *ws,
                         const nuwax_meeting_event_t *event) {
    if (!mode || !event) {
        return;
    }

    if (event->type == NUWAX_MEETING_EVENT_CLOUD_ENABLE ||
        event->type == NUWAX_MEETING_EVENT_CLOUD_DISABLE)
        ESP_LOGI(TAG, "meeting cloud control request=%s state=%s epoch=%lu target_session=%s local_session=%s sd=%d",
                 event->request_id[0] ? event->request_id : "-", event->state,
                 (unsigned long)event->ws_connection_epoch,
                 event->audio_session_id[0] ? event->audio_session_id : "-",
                 mode->audio_session_id[0] ? mode->audio_session_id : "-", (int)mode->sd_local);

    if (event->type == NUWAX_MEETING_EVENT_REQUEST_ACK) {
#if CONFIG_NUWAX_SD_SESSION_ENABLE
        if (handle_sd_sync_ack(mode, event)) return;
#endif
        if (mode->close_request_id[0] && event->ws_connection_epoch == mode->close_request_epoch &&
            strcmp(event->request_id, mode->close_request_id) == 0) {
            mode->pending_ui_close = false;
            if (event->ok && strcmp(event->state, "disabled") == 0)
                ESP_LOGI(TAG, "meeting close acknowledged request=%s", event->request_id);
            else
                ESP_LOGW(TAG, "meeting close rejected request=%s reason=%s", event->request_id, event->reason);
            mode->close_request_id[0] = '\0';
            mode->close_request_epoch = 0;
            /* A late close ACK must never stop a newer local meeting. */
            return;
        }
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
            select_audio_session(mode, event->audio_session_id);
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

    if (event->type == NUWAX_MEETING_EVENT_CLOUD_ENABLE && event->local_stop_generation !=
        atomic_load(&mode->local_stop_generation)) {
        (void)send_event_command_ack(mode, ws, event, false, false, "local_stop_superseded", NULL);
        return;
    }
    if (event->request_id[0]) {
        nuwax_audio_command_deadline_t *c = &mode->command_deadline;
        if (nuwax_audio_command_matches(c, event->request_id) && c->retired) return;
        if (nuwax_audio_command_matches(c, event->request_id) &&
            (!c->decided || !c->ok || mode->state != NUWAX_MEETING_STATE_ACTIVE)) {
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
            if (deadline) (void)nuwax_meeting_protocol_send_command_ack_until(ws, event->request_id,
                false, false, "mode_handoff_failed", NULL, deadline, event->ws_connection_epoch);
            return;
        }
    }

    if (event->type == NUWAX_MEETING_EVENT_CLOUD_ENABLE) {
        if (event->local_stop_generation != atomic_load_explicit(
                &mode->local_stop_generation, memory_order_acquire)) {
            if (ws && event->request_id[0]) {
                (void)send_event_command_ack(mode, ws, event, false, false, "local_stop_superseded", NULL);
            }
            ESP_LOGI(TAG, "queued meeting enable cancelled by local stop request=%s",
                     event->request_id);
            return;
        }
        bool sd_admission = false;
        (void)sd_admission;
#if CONFIG_NUWAX_SD_SESSION_ENABLE
        sd_admission = mode->sd_local ||
            (mode->state == NUWAX_MEETING_STATE_DISABLED && !mode->identity.resuming &&
             nuwax_sd_session_prefers_local());
        if (sd_admission) {
            const char *rejection = nuwax_sd_control_enable_rejection(mode->sd_local,
                mode->audio_session_id, &mode->sd_sync, event->audio_session_id);
            if (rejection) {
                ESP_LOGW(TAG, "SD meeting enable rejected request=%s reason=%s", event->request_id, rejection);
                (void)send_event_command_ack(mode, ws, event, mode->state == NUWAX_MEETING_STATE_ACTIVE, false, rejection, NULL);
                return;
            }
        }
#endif
        if (mode->state == NUWAX_MEETING_STATE_ACTIVE) {
#if CONFIG_NUWAX_SD_SESSION_ENABLE
            if (mode->sd_local) {
                nuwax_sd_control_track_enabled(&mode->sd_sync, event->request_id);
                /* Accepted cloud enable also reconciles a locally running
                 * scene when the legacy resume request is not supported. */
                nuwax_sd_control_sync_connect(&mode->sd_sync, event->ws_connection_epoch,
                                               now_ms(), event->rx_order);
                const bool was_confirmed = mode->sd_sync.confirmed;
                nuwax_sd_control_sync_confirm(&mode->sd_sync, event->rx_order, event->audio_session_id);
                if (!was_confirmed)
                    ESP_LOGI(TAG, "SD meeting control confirmed by cloud enable request=%s epoch=%lu session=%s",
                             event->request_id, (unsigned long)event->ws_connection_epoch, mode->audio_session_id);
            }
#endif
            if (ws && event->request_id[0]) {
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
                (void)send_event_command_ack(mode, ws, event, false, false, "meeting_busy", NULL);
            }
            ESP_LOGW(TAG, "cloud meeting enable rejected while state=%s", state_name(mode->state));
            return;
        }
        if (!can_start(mode)) {
            if (ws && event->request_id[0] != '\0') {
                (void)send_event_command_ack(mode, ws, event, false, false, "audio_mode_busy", NULL);
            }
            ESP_LOGW(TAG,
                     "cloud meeting enable rejected because another audio mode owns the transition");
            return;
        }
        /* Reject a known unhealthy medium before UI/MIC/playback handoff.
         * Store a terminal command result so retries cannot revive it. */
#if CONFIG_NUWAX_SD_SESSION_ENABLE
        const esp_err_t storage_fault = sd_admission ? nuwax_sd_session_start_fault() : ESP_OK;
        if (storage_fault != ESP_OK) {
            nuwax_audio_command_begin(&mode->command_deadline, event->request_id, event->received_ms,
                event->ws_connection_epoch, event->local_stop_generation, esp_timer_get_time());
            nuwax_audio_command_decide(&mode->command_deadline, false, false, "sd_start_failed", NULL);
            ESP_LOGW(TAG, "SD meeting start refused before handoff request=%s media_err=%s",
                event->request_id, esp_err_to_name(storage_fault));
            service_command_result(mode, ws);
            return;
        }
#endif
        reset_transient_state(mode);
#if CONFIG_NUWAX_SD_SESSION_ENABLE
        /* Latch the checked route before draining another queued command. */
        mode->sd_local = sd_admission;
        if (sd_admission) mode->sd_start_ms = now_ms();
#endif
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
        select_audio_session(mode, event->audio_session_id);
        mode->cloud_authorized = true;
        mode->interrupt_current_turn = event->interrupt_current_turn ||
                                        event->current_turn_cancelled;
#if CONFIG_NUWAX_SD_SESSION_ENABLE
        nuwax_sd_control_sync_connect(&mode->sd_sync, event->ws_connection_epoch, now_ms(), event->rx_order);
        nuwax_sd_control_sync_confirm(&mode->sd_sync, event->rx_order, event->audio_session_id);
        mode->sd_sync.announced = true; /* Fresh cloud opening, not a reconnect report. */
        mode->sd_sync.reported_enabled = true;
        if (sd_admission && nuwax_sd_control_prepare_legacy_id(&mode->sd_sync, mode->audio_session_id, "meeting"))
            ESP_LOGI(TAG, "SD meeting assigned recording ID request=%s cloud_session=%s local_session=%s",
                event->request_id, mode->sd_sync.cloud_session_id[0] ? mode->sd_sync.cloud_session_id : "<missing>",
                mode->audio_session_id);
#endif
        nuwax_audio_command_begin(&mode->command_deadline, event->request_id, event->received_ms,
            event->ws_connection_epoch, event->local_stop_generation, esp_timer_get_time());
        set_state(mode, NUWAX_MEETING_STATE_STARTING, "cloud_enable");
        nuwax_meeting_mode_service_command_deadline(mode, ws);
        if (mode->state != NUWAX_MEETING_STATE_STARTING) return;
        if (mode->callbacks.is_playback_active &&
            mode->callbacks.is_playback_active(mode->callbacks.ctx) &&
            mode->callbacks.fade_and_cancel_playback) {
            mode->callbacks.fade_and_cancel_playback(mode->callbacks.ctx,
                                                     NUWAX_MEETING_PLAYBACK_FADE_MS,
                                                     "meeting_cloud_enable");
        }
        return;
    }

#if CONFIG_NUWAX_SD_SESSION_ENABLE
    if (reject_sd_cloud_disable(mode, ws, event)) return;
#endif
    if (mode->state == NUWAX_MEETING_STATE_DISABLED) {
        if (ws && event->request_id[0]) {
            (void)send_event_command_ack(mode, ws, event, false, true, "already_disabled", NULL);
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
            (void)send_event_command_ack(mode, ws, event, false, false, "meeting_busy", NULL);
        }
        return;
    }
    retire_active_result(mode);
    if (mode->pending_command_ack && mode->pending_command_enabled &&
        mode->pending_command_request_id[0] != '\0' && ws &&
        nuwax_ws_is_connected(ws)) {
        esp_err_t ack_err = ESP_OK;
        if (nuwax_audio_command_matches(&mode->command_deadline, mode->pending_command_request_id)) {
            nuwax_audio_command_decide(&mode->command_deadline, false, false, "superseded_by_disable", NULL);
            service_command_result(mode, ws);
        } else {
            ack_err = nuwax_meeting_protocol_send_command_ack(ws, mode->pending_command_request_id,
                false, false, "superseded_by_disable");
        }
        if (ack_err != ESP_OK) {
            ESP_LOGW(TAG,
                     "meeting superseded enable ACK failed request=%s err=%s",
                     mode->pending_command_request_id,
                     esp_err_to_name(ack_err));
        }
    }
    if (nuwax_audio_command_matches(&mode->command_deadline, mode->pending_command_request_id)) {
        nuwax_audio_command_decide(&mode->command_deadline, false, false, "superseded_by_disable", NULL);
        service_command_result(mode, ws);
    }
    if (event->request_id[0]) {
        nuwax_audio_command_begin(&mode->command_deadline, event->request_id, event->received_ms,
            event->ws_connection_epoch, event->local_stop_generation, esp_timer_get_time());
        mode->command_deadline.enabled = false;
        mode->command_deadline.prepared = true;
    }
    begin_cloud_stop(mode, event);
}

static void drain_events(nuwax_meeting_mode_t *mode, nuwax_ws_client_t *ws) {
    if (!mode) {
        return;
    }
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    if (mode->sd_local && ws && nuwax_ws_is_connected(ws))
        nuwax_sd_control_sync_connect(&mode->sd_sync, nuwax_ws_connection_epoch(ws), now_ms(),
                                      atomic_load_explicit(&mode->control_rx_order, memory_order_relaxed));
#endif
    nuwax_meeting_event_t event;
    unsigned drained = 0;
    if (mode->request_acks) {
        while (drained < 4U && xQueueReceive(mode->request_acks, &event, 0) == pdTRUE) {
            complete_queued_enable_event(mode, &event);
            ++drained;
            if (!event_matches_connection(&event, ws)) {
                continue;
            }
            nuwax_meeting_mode_service_command_deadline(mode, ws);
            handle_event(mode, ws, &event);
            nuwax_meeting_mode_service_command_deadline(mode, ws);
        }
    }
    if (mode->events) {
        while (drained < 4U && xQueueReceive(mode->events, &event, 0) == pdTRUE) {
            ++drained;
            if (event_matches_connection(&event, ws)) {
                nuwax_meeting_mode_service_command_deadline(mode, ws);
                handle_event(mode, ws, &event);
                nuwax_meeting_mode_service_command_deadline(mode, ws);
            }
            complete_queued_enable_event(mode, &event);
        }
    }
}

void nuwax_meeting_mode_sync_current_state(nuwax_meeting_mode_t *mode,
                                          nuwax_ws_client_t *ws)
{
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    if (!mode || !mode->initialized || !ws || !nuwax_ws_is_connected(ws)) return;
    /* Only consume passive reports here. Actual controls and opening/closing
     * ACKs keep their existing handoff/deadline dispatch path and order. RX
     * only appends; the application owner is the sole queue consumer. */
    nuwax_meeting_event_t event;
    for (unsigned i = 0U; i < 4U && mode->request_acks &&
         xQueuePeek(mode->request_acks, &event, 0) == pdTRUE &&
         is_state_sync_ack(&event); ++i) {
        if (xQueueReceive(mode->request_acks, &event, 0) != pdTRUE) break;
        complete_queued_enable_event(mode, &event);
        if (event_matches_connection(&event, ws)) (void)handle_sd_sync_ack(mode, &event);
    }
    service_sd_cloud_sync(mode, ws);
#else
    (void)mode; (void)ws;
#endif
}

static void send_ui_close_request(nuwax_meeting_mode_t *mode, nuwax_ws_client_t *ws)
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
        snprintf(mode->close_request_id, sizeof(mode->close_request_id), "meeting_close_%lu_%08lx",
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
    const esp_err_t err = nuwax_meeting_protocol_send_request(ws, mode->close_request_id,
        false, false, mode->close_reason);
    mode->close_retry_ms = now_ms() + NUWAX_AUDIO_COMMAND_RETRY_MS;
    if (err == ESP_OK) mode->pending_ui_close = false;
    ESP_LOGI(TAG, "local close request=%s attempt=%u err=%s", mode->close_request_id,
        mode->close_attempts, esp_err_to_name(err));
}

static const char *start_failure_message(const nuwax_meeting_mode_t *mode) {
    if (mode && (!strcmp(mode->stop_reason, "audio_session_required") ||
                 !strcmp(mode->stop_reason, "invalid_audio_session_id"))) return mode->stop_reason;
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
    if (mode->sd_local && mode->state == NUWAX_MEETING_STATE_ACTIVE) {
        mode->pending_ui_close = true;
        mode->close_survives_disconnect = true;
        mode->close_request_id[0] = '\0';
    }
    retire_active_result(mode);
    if (mode->pending_ui_close) {
        send_ui_close_request(mode, ws);
    }
    if (mode->pending_command_ack && mode->pending_command_request_id[0] != '\0') {
        /* An enable command is acknowledged successfully as soon as capture
         * becomes ACTIVE.  If it is still pending here, activation was
         * cancelled or failed.  A disable command succeeds only after the
         * capture/uplink teardown above completed cleanly. */
        bool stopped_ok = capture_ok;
#if CONFIG_NUWAX_SD_SESSION_ENABLE
        if (mode->sd_local && !mode->pending_command_enabled)
            stopped_ok = nuwax_sd_session_stopped();
#endif
        const bool command_ok = !mode->pending_command_enabled && stopped_ok;
        const char *message = mode->pending_command_enabled
                                  ? (capture_ok ? "start_cancelled"
                                                : start_failure_message(mode))
                                  : (stopped_ok ? "disabled" : "capture_failed");
        esp_err_t ack_err = ESP_OK;
        if (nuwax_audio_command_matches(&mode->command_deadline, mode->pending_command_request_id)) {
            nuwax_audio_command_decide(&mode->command_deadline, false, command_ok, message, NULL);
            service_command_result(mode, ws);
        } else if (ws && nuwax_ws_is_connected(ws)) {
            ack_err = nuwax_meeting_protocol_send_command_ack(ws, mode->pending_command_request_id,
                false, command_ok, message);
        }
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
    const bool retry_start = mode->identity.resuming && !capture_ok &&
                            mode->capture_frames == 0U &&
                            strcmp(final_reason, "cloud_stop") != 0 &&
                            strcmp(final_reason, "ui_close") != 0;
    if (!retry_start && (!mode->offline_resume_required || (ws && nuwax_ws_is_connected(ws)))) {
        nuwax_audio_mode_identity_clear(&mode->identity);
    }
    reset_transient_state(mode);
    set_state(mode,
              capture_ok ? NUWAX_MEETING_STATE_DISABLED : NUWAX_MEETING_STATE_ERROR,
              final_reason);
    if (!capture_ok) {
        /* An error is reported locally, then the controller becomes reusable. */
        (void)nuwax_device_error_report_enqueue(
            "error", "meeting", "capture", ESP_FAIL,
            "meeting", 0U, NULL);
        set_state(mode, NUWAX_MEETING_STATE_DISABLED, "capture_error_reset");
    }
}

void nuwax_meeting_mode_service_command_deadline(nuwax_meeting_mode_t *mode, nuwax_ws_client_t *ws)
{
    if (!mode || !mode->initialized) return;
    if (mode->state == NUWAX_MEETING_STATE_DISABLED) send_ui_close_request(mode, ws);
    nuwax_audio_command_deadline_t *c = &mode->command_deadline;
    /* Once admitted, a stop belongs to the local capture owner. Losing its
     * connection cannot cancel sealing or turn it into a failed start. The
     * final result is still subject to the existing bounded ACK deadline. */
    const bool closing = !c->enabled && mode->state == NUWAX_MEETING_STATE_STOPPING;
    if (c->tracked && !c->decided && !closing) {
        const bool stale = !ws || !nuwax_ws_is_connected(ws) ||
            nuwax_ws_connection_epoch(ws) != c->epoch ||
            atomic_load(&mode->local_stop_generation) != c->local_stop_generation;
        bool failed = nuwax_audio_command_expired(c, esp_timer_get_time()) || stale;
        if (!failed && !c->prepared && mode->state == NUWAX_MEETING_STATE_STARTING) {
            esp_err_t err = ESP_OK;
#if CONFIG_NUWAX_SD_SESSION_ENABLE
            if (mode->sd_local && !mode->sd_sync.identity_ready) {
                err = nuwax_sd_session_poll_identity(NUWAX_OFFLINE_AUDIO_MODE_MEETING,
                    mode->audio_session_id, nuwax_audio_command_execution_deadline(c));
                if (err == ESP_ERR_INVALID_STATE && nuwax_sd_control_rotate_used_id(
                        &mode->sd_sync, mode->audio_session_id, "meeting")) {
                    ESP_LOGI(TAG, "SD meeting replaced used recording ID request=%s local_session=%s",
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
                /* SD launch gets its own existing readiness window after
                 * asynchronous preflight; the total command deadline stays. */
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
                set_state(mode, NUWAX_MEETING_STATE_STOPPING, "mode_handoff_failed");
            } else {
                reset_to_disabled(mode, "mode_handoff_failed", false);
            }
        }
    }
    service_command_result(mode, ws);
}

static esp_err_t fail_capture_start(nuwax_meeting_mode_t *mode,
                                     nuwax_ws_client_t *ws,
                                     esp_err_t err,
                                     const char *reason) {
    if (!mode) {
        return err;
    }
    set_stop_reason(mode, reason ? reason : "capture_start_failed");
    (void)nuwax_device_error_report_enqueue(
        "error", "meeting", reason && reason[0] ? reason : "capture_start",
        err, "meeting", 0U, NULL);
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
#if CONFIG_NUWAX_OFFLINE_AUDIO_ENABLE && !CONFIG_NUWAX_SD_SESSION_ENABLE
    nuwax_offline_audio_mirror_t archive = {0};
#endif
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

    if (mode->state == NUWAX_MEETING_STATE_STOPPING || mode->state == NUWAX_MEETING_STATE_DISABLED ||
        nuwax_audio_command_expired(&mode->command_deadline, esp_timer_get_time())) {
        nuwax_audio_command_decide(&mode->command_deadline, false, false, "mode_handoff_failed", NULL);
        set_stop_reason(mode, "mode_handoff_failed");
        mode->offline_resume_required = false;
        nuwax_audio_mode_identity_clear(&mode->identity);
        err = ESP_ERR_TIMEOUT;
        goto cleanup;
    }
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



    err = mode->callbacks.acquire_mic
        ? mode->callbacks.acquire_mic(mode->callbacks.ctx, &mic, NUWAX_MIC_OWNER_MEETING_CAPTURE)
        : nuwax_mic_input_init_with_owner(&mic, NUWAX_MIC_OWNER_MEETING_CAPTURE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "meeting mic init failed: %s", esp_err_to_name(err));
        goto cleanup;
    }
    mic_ready = true;
    if (mode->state == NUWAX_MEETING_STATE_STOPPING || mode->state == NUWAX_MEETING_STATE_DISABLED ||
        nuwax_audio_command_expired(&mode->command_deadline, esp_timer_get_time())) {
        nuwax_audio_command_decide(&mode->command_deadline, false, false, "mode_handoff_failed", NULL);
        set_stop_reason(mode, "mode_handoff_failed");
        mode->offline_resume_required = false;
        nuwax_audio_mode_identity_clear(&mode->identity);
        err = ESP_ERR_TIMEOUT;
        goto cleanup;
    }


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

        /* Reserve the physical frame's sequence before either sink. If the
         * previous uplink frame fails, this frame may already be on SD. */
        const uint32_t current_sequence = mode->identity.next_capture_sequence++;
        (void)current_sequence; /* Cursor also advances in builds without SD. */
        mode->capture_frames++;
#if CONFIG_NUWAX_OFFLINE_AUDIO_ENABLE && !CONFIG_NUWAX_SD_SESSION_ENABLE
        (void)nuwax_offline_audio_mirror_service(
            &archive, NUWAX_OFFLINE_AUDIO_MODE_MEETING, mode->audio_session_id,
            CONFIG_NUWAX_VOICE_SAMPLE_RATE, CONFIG_NUWAX_VOICE_CHANNELS,
            CONFIG_NUWAX_VOICE_FRAME_DURATION_MS, now_ms());
        const nuwax_offline_audio_frame_t archive_frame = {
            .sequence = current_sequence, .timestamp_ms = now_ms(),
            .flags = 0U, .samples = current, .sample_count = samples_read,
        };
        (void)nuwax_offline_audio_mirror_append(&archive, &archive_frame, now_ms());
#endif

        (void)nuwax_meeting_inactivity_update(
            &mode->inactivity,
            &features,
            CONFIG_NUWAX_VOICE_FRAME_DURATION_MS,
            NUWAX_MEETING_NO_SPEECH_TIMEOUT_MS);

        if (have_pending) {
            const uint8_t flags = frame_flags(&pending_frame.features,
                                              mode->capture_frames == 2U,
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
        /* Each listen opens a new wire stream. The SD capture cursor above
         * spans reconnects, but the uploader requires this stream to start at 0. */
        pending_frame.sequence = mode->capture_frames - 1U;
        pending_frame.features = features;
        have_pending = true;

        if ((mode->capture_frames % NUWAX_MEETING_LOG_INTERVAL_FRAMES) == 0U) {
            ESP_LOGI(TAG,
                     "meeting capture frames=%lu enqueued=%lu avg_abs=%lu peak=%lu speech=%d inactive_ms=%lu speech_activity_ms=%lu sound_activity_ms=%lu",
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
                                          mode->capture_frames == 1U,
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
#if CONFIG_NUWAX_OFFLINE_AUDIO_ENABLE && !CONFIG_NUWAX_SD_SESSION_ENABLE
    (void)nuwax_offline_audio_mirror_request_close(
        &archive, err == ESP_OK && end_committed, now_ms());
#endif
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
    const bool ws_disconnected = !ws || !nuwax_ws_is_connected(ws);
    const bool intentional_stop =
        strcmp(mode->stop_reason, "ui_close") == 0 ||
        strcmp(mode->stop_reason, "cloud_stop") == 0 ||
        (mode->pending_command_ack && !mode->pending_command_enabled);
    const bool transport_boundary_reason =
        ws_disconnected && !intentional_stop;
    if (transport_boundary_reason) {
        /* The outer connection owner will start the SD-only capture after
         * this function releases MIC.  Keep this marker outside transient
         * state so finish_state cannot erase the handoff intent. */
        mode->pending_ui_close = false;
        mode->offline_resume_required = true;
        nuwax_audio_mode_identity_remember(&mode->identity, mode->audio_session_id);
        mode->identity.resuming = mode->identity.session_id[0] != '\0';
    }
    /* A WebSocket loss is the ownership boundary for the SD-only handoff,
     * not a meeting capture fault.  The offline marker is set immediately
     * above after MIC/uplink cleanup; preserve real local failures while
     * suppressing the transient ERROR state caused solely by transport loss. */
    const bool offline_transport_handoff =
        ws_disconnected && mode->offline_resume_required &&
        transport_boundary_reason;
    if (offline_transport_handoff && err != ESP_OK) {
        ESP_LOGI(TAG,
                 "meeting transport boundary reserved for offline handoff; suppressing transient capture error err=%s",
                 esp_err_to_name(err));
    }
    const bool capture_ok = err == ESP_OK || offline_transport_handoff;
    ESP_LOGI(TAG,
             "meeting capture done ok=%d reason=%s frames=%lu enqueued=%lu dropped=%lu inactive_ms=%lu read_discontinuities=%lu discarded_partial_samples=%llu err=%s",
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


#if CONFIG_NUWAX_SD_SESSION_ENABLE
/* Connection-owner only, also called by the offline SD tick. A valid stop
 * already received for this scene must survive transport teardown. No starts
 * or unrelated request ACKs are executed on a disconnected transport. */
static void preserve_sd_stop_on_disconnect(nuwax_meeting_mode_t *mode)
{
    if (!mode || !mode->sd_local ||
        (mode->state != NUWAX_MEETING_STATE_ACTIVE &&
         mode->state != NUWAX_MEETING_STATE_STARTING)) return;
    nuwax_meeting_event_t event;
    for (unsigned i = 0; i < 4U && mode->request_acks &&
         xQueueReceive(mode->request_acks, &event, 0) == pdTRUE; ++i) {
        complete_queued_enable_event(mode, &event);
        (void)handle_sd_sync_ack(mode, &event);
    }
    while (mode->events && xQueueReceive(mode->events, &event, 0) == pdTRUE) {
        complete_queued_enable_event(mode, &event);
        if (event.type != NUWAX_MEETING_EVENT_CLOUD_DISABLE ||
            mode->state == NUWAX_MEETING_STATE_STOPPING) continue;
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
            ESP_LOGW(TAG, "SD meeting disconnected stop rejected request=%s reason=%s",
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
        ESP_LOGI(TAG, "SD meeting stop retained across disconnect request=%s session=%s",
                 event.request_id[0] ? event.request_id : "-", mode->audio_session_id);
    }
}
#endif

void nuwax_meeting_mode_service_sd(nuwax_meeting_mode_t *mode, nuwax_ws_client_t *ws)
{
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    if (!mode || !mode->initialized) return;
    if (!mode->sd_local) {
        if (mode->state != NUWAX_MEETING_STATE_STARTING || !nuwax_sd_session_prefers_local() || mode->identity.resuming || !mode->cloud_authorized) return;
        mode->sd_local = true;
        mode->sd_start_ms = now_ms();
        /* A card can become available after cloud-command admission. Apply
         * the same temporary mapping before owner handoff/catalog validation. */
        if (mode->pending_command_enabled &&
            nuwax_sd_control_prepare_legacy_id(&mode->sd_sync, mode->audio_session_id, "meeting"))
            ESP_LOGI(TAG, "SD meeting assigned recording ID after card detection request=%s cloud_session=%s local_session=%s",
                mode->pending_command_request_id,
                mode->sd_sync.cloud_session_id[0] ? mode->sd_sync.cloud_session_id : "<missing>", mode->audio_session_id);
        ESP_LOGI(TAG, "SD capture route selected ready=%d wait_limit_ms=5000", nuwax_sd_session_available());
        if (mode->command_deadline.tracked && !mode->command_deadline.decided)
            mode->command_deadline.prepared = false;
    }
    if (ws && nuwax_ws_is_connected(ws)) drain_events(mode, ws);
    else preserve_sd_stop_on_disconnect(mode);
    nuwax_meeting_mode_service_command_deadline(mode, ws);
    if (!mode->sd_local) return;
    if (mode->state == NUWAX_MEETING_STATE_STOPPING) {
        if (mode->capture_running) nuwax_sd_session_stop(true);
        if ((!mode->capture_running || !nuwax_sd_session_active()) && nuwax_sd_session_stopped()) {
            mode->capture_running = false;
            finish_state(mode, ws, nuwax_sd_session_complete());
        }
        return;
    }
    if (mode->state == NUWAX_MEETING_STATE_STARTING && !mode->capture_running) {
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
        /* Control IDs are optional; admission assigns a valid recording ID
         * before preparing the SD owner. Keep the storage boundary strict. */
        if (mode->pending_command_enabled && !sd_audio_identity_valid(mode->audio_session_id)) {
            (void)fail_capture_start(mode, ws, ESP_ERR_INVALID_ARG,
                mode->audio_session_id[0] ? "invalid_audio_session_id" : "audio_session_required");
            return;
        }
        const int64_t start_deadline = mode->command_deadline.tracked && !mode->command_deadline.decided
            ? nuwax_audio_command_execution_deadline(&mode->command_deadline) : 0;
        esp_err_t err = nuwax_sd_session_prepare_start(NUWAX_OFFLINE_AUDIO_MODE_MEETING, start_deadline);
        if (err == ESP_OK) err = start_deadline
            ? nuwax_sd_session_start_until(NUWAX_OFFLINE_AUDIO_MODE_MEETING, mode->audio_session_id, start_deadline)
            : nuwax_sd_session_start(NUWAX_OFFLINE_AUDIO_MODE_MEETING, mode->audio_session_id);
        /* Recheck after the bounded launch lock. If a task was admitted on
         * the boundary, retain its owner until its normal cancellation seals. */
        if (err == ESP_OK) mode->capture_running = true;
        nuwax_meeting_mode_service_command_deadline(mode, ws);
        if (mode->state != NUWAX_MEETING_STATE_STARTING) return;
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
    if (mode->capture_running && mode->state == NUWAX_MEETING_STATE_STARTING &&
        nuwax_offline_capture_health_is_recording(nuwax_offline_audio_capture_health())) {
        set_state(mode, NUWAX_MEETING_STATE_ACTIVE, "sd_recording");
        nuwax_sd_session_release_start(NUWAX_OFFLINE_AUDIO_MODE_MEETING);
        ESP_LOGI(TAG, "SD meeting started session=%s network_independent=1", mode->audio_session_id);
    }
    try_send_active_command_ack(mode, ws, false);
    service_sd_cloud_sync(mode, ws);
#else
    (void)mode; (void)ws;
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
    atomic_init(&mode->queued_sync_acks, 0U);
    atomic_init(&mode->local_stop_generation, 0U);
    atomic_init(&mode->control_rx_order, 0U);
    atomic_init(&mode->control_rx_epoch, 0U);
    mode->initialized = true;
    return ESP_OK;
}

void nuwax_meeting_mode_deinit(nuwax_meeting_mode_t *mode) {
    if (!mode) {
        return;
    }
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    nuwax_sd_session_release_start(NUWAX_OFFLINE_AUDIO_MODE_MEETING);
#endif
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
    if (!mode || !mode->initialized) return false;
    if (mode->state != NUWAX_MEETING_STATE_DISABLED ||
        atomic_load_explicit(&mode->queued_enable_events, memory_order_acquire) > 0U) return true;
    const uint32_t queued = (mode->events ? (uint32_t)uxQueueMessagesWaiting(mode->events) : 0U) +
        (mode->request_acks ? (uint32_t)uxQueueMessagesWaiting(mode->request_acks) : 0U);
    /* State-report ACKs do not reserve MIC or interrupt normal conversation.
     * Other commands/ACKs retain the existing owner-dispatch behavior. */
    return queued > atomic_load_explicit(&mode->queued_sync_acks, memory_order_acquire);
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
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    if (enabled && !mode->identity.resuming && nuwax_sd_session_prefers_local() && mode->state == NUWAX_MEETING_STATE_DISABLED && can_start(mode)) {
        reset_transient_state(mode);
        mode->sd_local = true;
        mode->sd_start_ms = now_ms();
        ESP_LOGI(TAG, "SD capture route selected ready=%d wait_limit_ms=5000", nuwax_sd_session_available());
        mode->cloud_authorized = true;
        set_state(mode, NUWAX_MEETING_STATE_STARTING, "local_sd_enable");
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
        if (mode->state == NUWAX_MEETING_STATE_DISABLED) {
            nuwax_audio_mode_identity_clear(&mode->identity);
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
    esp_err_t err = mode->identity.resuming
        ? nuwax_meeting_protocol_send_resume_request(ws, mode->pending_request_id,
                                                     mode->identity.session_id)
        : nuwax_meeting_protocol_send_request(ws, mode->pending_request_id, true,
                                               mode->interrupt_current_turn, NULL);
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
    const uint32_t local_stop_generation = atomic_load_explicit(
        &mode->local_stop_generation, memory_order_acquire);
    const uint32_t rx_order = atomic_fetch_add_explicit(&mode->control_rx_order, 1U, memory_order_relaxed) + 1U;
    const uint32_t received_ms = now_ms();
    nuwax_meeting_protocol_event_t parsed = {0};
    if (!nuwax_meeting_protocol_parse(payload, len, &parsed)) {
        return false;
    }
    if (parsed.type == NUWAX_MEETING_PROTOCOL_EVENT_SET_COMMAND &&
        strcmp(parsed.state, "enabled") == 0 &&
        mode->state == NUWAX_MEETING_STATE_DISABLED && !can_start(mode)) {
        nuwax_meeting_event_t rejected = {.received_ms = received_ms,
            .ws_connection_epoch = ws ? nuwax_ws_connection_epoch(ws) : 0U};
        copy_text(rejected.request_id, sizeof(rejected.request_id), parsed.request_id);
        (void)send_event_command_ack(NULL, ws, &rejected, false, false, "audio_mode_busy", NULL);
        return true;
    }
    nuwax_meeting_event_t event = {0};
    event.local_stop_generation = local_stop_generation;
    event.rx_order = rx_order;
    event.received_ms = received_ms;
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
    if (event.type == NUWAX_MEETING_EVENT_CLOUD_ENABLE || event.type == NUWAX_MEETING_EVENT_CLOUD_DISABLE) {
        atomic_store_explicit(&mode->control_rx_epoch, event.ws_connection_epoch, memory_order_release);
        ESP_LOGI(TAG, "command received request=%s enabled=%d epoch=%lu target_session=%s",
                 event.request_id, event.type == NUWAX_MEETING_EVENT_CLOUD_ENABLE,
                 (unsigned long)event.ws_connection_epoch,
                 event.audio_session_id[0] ? event.audio_session_id : "-");
    }
    if (!queue_event(mode, &event)) {
        if (is_state_sync_ack(&event)) {
            /* Passive state confirmation must never tear down live audio.
             * A queued reply or the bounded report retry can confirm it. */
            ESP_LOGW(TAG, "meeting state-sync ACK dropped while queue full request=%s", event.request_id);
            return true;
        }
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
                esp_err_t ack_err = send_event_command_ack(NULL, ws, &event, enabled, false, "control_queue_full", NULL);
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

void nuwax_meeting_mode_poll_control_events(nuwax_meeting_mode_t *mode,
                                          nuwax_ws_client_t *ws)
{
    if (mode && mode->initialized && ws && nuwax_ws_is_connected(ws)) drain_events(mode, ws);
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
        nuwax_meeting_mode_service_command_deadline(mode, ws);
        if (mode->state == NUWAX_MEETING_STATE_STARTING && mode->command_deadline.tracked &&
            !mode->command_deadline.decided && !mode->command_deadline.prepared) {
            vTaskDelay(pdMS_TO_TICKS(20U)); return ESP_OK;
        }
#if CONFIG_NUWAX_SD_SESSION_ENABLE
        nuwax_meeting_mode_service_sd(mode, ws);
        if (mode->sd_local) { vTaskDelay(pdMS_TO_TICKS(20U)); return ESP_OK; }
#endif
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
            complete_queued_enable_event(mode, &event);
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
                    send_event_command_ack(mode, ws, &event, enabled, false,
                        reason && reason[0] ? reason : "device_recovery_timeout", NULL);
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
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    preserve_sd_stop_on_disconnect(mode);
    /* Publish the nonblocking capture cancellation before any reconnect
     * work. The storage owner still drains/seals before a success ACK. */
    if (mode && mode->sd_local && mode->state == NUWAX_MEETING_STATE_STOPPING &&
        mode->capture_running) nuwax_sd_session_stop(true);
#endif
    if (mode && !mode->close_survives_disconnect) mode->pending_ui_close = false;
    nuwax_meeting_mode_service_command_deadline(mode, NULL);
    if (mode) { mode->close_request_id[0] = '\0'; mode->close_request_epoch = 0; }
#if CONFIG_NUWAX_SD_SESSION_ENABLE
    if (mode && mode->sd_local) {
        clear_event_queue(mode);
        /* The active SD scene survives disconnect. Retry its original opening
         * ACK on reconnect; finalization clears it if the scene ends offline. */
        return;
    }
#endif
    if (!mode || !mode->initialized) {
        return;
    }
    if (mode->state != NUWAX_MEETING_STATE_DISABLED && !mode->pending_ui_close &&
        strcmp(mode->stop_reason, "cloud_stop") != 0) {
        nuwax_audio_mode_identity_remember(&mode->identity, mode->audio_session_id);
        mode->identity.resuming = mode->identity.session_id[0] != '\0';
    }
    clear_event_queue(mode);
    reset_to_disabled(mode, "ws_disconnected", false);
}

bool nuwax_meeting_mode_take_offline_resume(nuwax_meeting_mode_t *mode)
{
    if (!mode) {
        return false;
    }
    const bool required = mode->offline_resume_required;
    mode->offline_resume_required = false;
    return required;
}

bool nuwax_meeting_mode_offline_resume_pending(
    const nuwax_meeting_mode_t *mode)
{
    return mode && mode->initialized && mode->offline_resume_required;
}
