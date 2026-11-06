#pragma once
#include "audio_mode_identity.h"
#include "sd_audio_control_sync.h"
#include "audio_command_deadline.h"

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "ws_transport.h"
#include "mic_input.h"

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
    /* Application owner only, after validating a new cloud start. */
    esp_err_t (*prepare_cloud_start)(void *ctx, nuwax_ws_client_t *ws,
                                    bool sd_local, const char *audio_session_id);
    esp_err_t (*acquire_mic)(void *ctx, nuwax_mic_input_t *mic, nuwax_mic_owner_t owner);
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
    NUWAX_MONITORING_EVENT_REQUEST_ACK,
} nuwax_monitoring_event_type_t;

typedef struct {
    nuwax_monitoring_event_type_t type;
    uint32_t ws_connection_epoch;
    uint32_t rx_order;
    uint32_t received_ms;
    uint32_t local_stop_generation;
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
    _Atomic uint32_t queued_sync_acks;
    _Atomic uint32_t local_stop_generation;
    _Atomic uint32_t control_rx_order;
    _Atomic uint32_t control_rx_epoch;
    nuwax_sd_control_sync_t sd_sync;
    nuwax_audio_command_deadline_t command_deadline;
    bool initialized;
    bool capture_running;
    _Atomic bool sd_local;
    uint32_t sd_start_ms;
    bool pending_command_ack;
    bool pending_command_enabled;
    bool pending_ui_close;
    bool close_survives_disconnect;
    bool offline_resume_required;
    nuwax_audio_mode_identity_t identity;
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
    char close_request_id[NUWAX_MONITORING_REQUEST_ID_BYTES];
    uint32_t close_request_epoch;
    uint32_t close_retry_ms;
    uint32_t close_started_ms;
    unsigned close_attempts;
    char close_reason[NUWAX_MONITORING_REASON_BYTES];
    char pending_command_request_id[NUWAX_MONITORING_REQUEST_ID_BYTES];
    char audio_session_id[NUWAX_MONITORING_AUDIO_SESSION_ID_BYTES];
    char stop_reason[NUWAX_MONITORING_REASON_BYTES];
} nuwax_monitoring_mode_t;

esp_err_t nuwax_monitoring_mode_init(
    nuwax_monitoring_mode_t *mode,
    const nuwax_monitoring_mode_callbacks_t *callbacks);
void nuwax_monitoring_mode_deinit(nuwax_monitoring_mode_t *mode);
/* Connection/application owner only; reports state without starting/stopping. */
void nuwax_monitoring_mode_sync_current_state(nuwax_monitoring_mode_t *mode,
                                             nuwax_ws_client_t *ws);

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
bool nuwax_monitoring_mode_take_offline_resume(nuwax_monitoring_mode_t *mode);
bool nuwax_monitoring_mode_offline_resume_pending(
    const nuwax_monitoring_mode_t *mode);

/* Requests that the cloud re-admit monitoring after a connection epoch
 * change.  The mode remains disabled until the cloud sends its normal
 * set_audio_mode/enabled command and the command is processed. */
esp_err_t nuwax_monitoring_mode_request_ui(nuwax_monitoring_mode_t *mode,
                                           nuwax_ws_client_t *ws,
                                           bool enabled);

/* Called by the WebSocket RX callback. Returns true if payload was consumed. */
bool nuwax_monitoring_mode_handle_protocol_text(nuwax_monitoring_mode_t *mode,
                                                nuwax_ws_client_t *ws,
                                                const char *payload,
                                                int len);

/* Application owner only; processes controls without acquiring the MIC. */
void nuwax_monitoring_mode_poll_control_events(nuwax_monitoring_mode_t *mode,
                                             nuwax_ws_client_t *ws);
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

/* Nonblocking owner tick, also serviced while offline. */
void nuwax_monitoring_mode_service_sd(nuwax_monitoring_mode_t *mode, nuwax_ws_client_t *ws);

/* Owner tick, including while another mode is STOPPING. Never performs SD I/O. */
void nuwax_monitoring_mode_service_command_deadline(nuwax_monitoring_mode_t *mode, nuwax_ws_client_t *ws);
