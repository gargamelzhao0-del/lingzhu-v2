#pragma once

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "ws_transport.h"

#include "monitoring_protocol.h"

typedef enum {
    NUWAX_MONITORING_STATE_DISABLED = 0,
    NUWAX_MONITORING_STATE_STARTING,
    NUWAX_MONITORING_STATE_ACTIVE,
    NUWAX_MONITORING_STATE_STOPPING,
    NUWAX_MONITORING_STATE_ERROR,
} nuwax_monitoring_state_t;

/* PAUSED is published only after the previous uplink has committed END and
 * drained. PAUSED and RESUME_PENDING must never admit uplink audio. */
typedef enum {
    NUWAX_MONITORING_CAPTURE_INACTIVE = 0,
    NUWAX_MONITORING_CAPTURE_STREAMING,
    NUWAX_MONITORING_CAPTURE_PAUSE_PENDING,
    NUWAX_MONITORING_CAPTURE_PAUSED,
    NUWAX_MONITORING_CAPTURE_RESUME_PENDING,
} nuwax_monitoring_capture_phase_t;

typedef struct {
    void *ctx;
    bool (*can_start)(void *ctx);
    bool (*is_playback_active)(void *ctx);
    void (*fade_and_cancel_playback)(void *ctx, uint32_t fade_ms, const char *reason);
    void (*poll_controls)(void *ctx, nuwax_ws_client_t *ws);
    void (*on_state_changed)(void *ctx,
                             nuwax_monitoring_state_t state,
                             const char *reason);
} nuwax_monitoring_mode_callbacks_t;

typedef enum {
    NUWAX_MONITORING_EVENT_CAPTURE_CONTROL_ACK = 0,
    NUWAX_MONITORING_EVENT_CLOUD_ENABLE,
    NUWAX_MONITORING_EVENT_CLOUD_DISABLE,
} nuwax_monitoring_event_type_t;

typedef struct {
    nuwax_monitoring_event_type_t type;
    uint32_t ws_connection_epoch;
    char request_id[NUWAX_MONITORING_REQUEST_ID_BYTES];
    char state[NUWAX_MONITORING_STATE_BYTES];
    char audio_session_id[NUWAX_MONITORING_AUDIO_SESSION_ID_BYTES];
    char reason[NUWAX_MONITORING_REASON_BYTES];
    bool ok;
} nuwax_monitoring_event_t;

typedef struct {
    QueueHandle_t events;
    QueueHandle_t capture_control_acks;
    nuwax_monitoring_mode_callbacks_t callbacks;
    _Atomic nuwax_monitoring_state_t state;
    _Atomic nuwax_monitoring_capture_phase_t capture_phase;
    _Atomic uint32_t queued_enable_events;
    bool initialized;
    bool capture_running;
    bool pending_command_ack;
    bool pending_command_enabled;
    uint32_t request_sequence;
    uint32_t stream_sequence;
    uint32_t pending_command_ack_attempts;
    uint32_t pending_command_ack_deadline_ms;
    uint32_t capture_frames;
    uint32_t uploaded_frames;
    uint32_t dropped_frames;
    uint32_t pause_requests;
    uint32_t resume_requests;
    uint32_t capture_control_retries;
    uint32_t consecutive_read_timeouts;
    char pending_command_request_id[NUWAX_MONITORING_REQUEST_ID_BYTES];
    char audio_session_id[NUWAX_MONITORING_AUDIO_SESSION_ID_BYTES];
    char stop_reason[NUWAX_MONITORING_REASON_BYTES];
} nuwax_monitoring_mode_t;

esp_err_t nuwax_monitoring_mode_init(
    nuwax_monitoring_mode_t *mode,
    const nuwax_monitoring_mode_callbacks_t *callbacks);
void nuwax_monitoring_mode_deinit(nuwax_monitoring_mode_t *mode);

nuwax_monitoring_state_t nuwax_monitoring_mode_state(
    const nuwax_monitoring_mode_t *mode);
nuwax_monitoring_capture_phase_t nuwax_monitoring_mode_capture_phase(
    const nuwax_monitoring_mode_t *mode);
bool nuwax_monitoring_mode_has_pending(const nuwax_monitoring_mode_t *mode);
bool nuwax_monitoring_mode_has_audio_claim(
    const nuwax_monitoring_mode_t *mode);
bool nuwax_monitoring_mode_blocks_ota(const nuwax_monitoring_mode_t *mode);
bool nuwax_monitoring_mode_should_preempt_normal(
    const nuwax_monitoring_mode_t *mode);

/* Called by the WebSocket RX callback. Returns true if payload was consumed. */
bool nuwax_monitoring_mode_handle_protocol_text(nuwax_monitoring_mode_t *mode,
                                                nuwax_ws_client_t *ws,
                                                const char *payload,
                                                int len);

/* Runs the pending/active monitoring session on the application task. */
esp_err_t nuwax_monitoring_mode_run(nuwax_monitoring_mode_t *mode,
                                    nuwax_ws_client_t *ws);

/* Requests an ordered stop so another exclusive audio mode can take over the
 * same WebSocket epoch. Active capture is stopped by its owner task; queued
 * controls are rejected without starting a transient capture. */
esp_err_t nuwax_monitoring_mode_request_handoff_stop(
    nuwax_monitoring_mode_t *mode,
    nuwax_ws_client_t *ws,
    const char *reason);

/* Owner-task recovery hook. It only rejects queued controls while the mode
 * is DISABLED; an active capture owner is never torn down by this API. */
esp_err_t nuwax_monitoring_mode_abort_pending_controls(
    nuwax_monitoring_mode_t *mode,
    nuwax_ws_client_t *ws,
    uint32_t ws_connection_epoch,
    const char *reason);

void nuwax_monitoring_mode_notify_ws_disconnected(nuwax_monitoring_mode_t *mode);
