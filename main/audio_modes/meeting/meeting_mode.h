#pragma once
#include "audio_mode_identity.h"
#include "sd_audio_control_sync.h"
#include "audio_command_deadline.h"

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>

#include "audio_uplink.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "mic_input.h"
#include "opus_encoder_wrapper.h"
#include "ws_transport.h"

#include "meeting_protocol.h"
#include "meeting_processor.h"

typedef enum {
    NUWAX_MEETING_STATE_DISABLED = 0,
    NUWAX_MEETING_STATE_STARTING,
    NUWAX_MEETING_STATE_ACTIVE,
    NUWAX_MEETING_STATE_STOPPING,
    NUWAX_MEETING_STATE_ERROR,
} nuwax_meeting_state_t;

typedef struct {
    void *ctx;
    bool (*can_start)(void *ctx);
    /* Application owner only, after validating a new cloud start. Failure
     * leaves the current audio owner untouched. Never called for duplicates. */
    esp_err_t (*prepare_cloud_start)(void *ctx, nuwax_ws_client_t *ws,
                                    bool sd_local, const char *audio_session_id);
    esp_err_t (*acquire_mic)(void *ctx, nuwax_mic_input_t *mic, nuwax_mic_owner_t owner);
    bool (*is_playback_active)(void *ctx);
    void (*fade_and_cancel_playback)(void *ctx, uint32_t fade_ms, const char *reason);
    void (*poll_controls)(void *ctx, nuwax_ws_client_t *ws);
    void (*on_state_changed)(void *ctx,
                             nuwax_meeting_state_t state,
                             const char *reason);
} nuwax_meeting_mode_callbacks_t;

typedef enum {
    NUWAX_MEETING_EVENT_REQUEST_ACK = 0,
    NUWAX_MEETING_EVENT_CLOUD_ENABLE,
    NUWAX_MEETING_EVENT_CLOUD_DISABLE,
} nuwax_meeting_event_type_t;

typedef struct {
    nuwax_meeting_event_type_t type;
    uint32_t ws_connection_epoch;
    uint32_t rx_order;
    uint32_t received_ms;
    uint32_t local_stop_generation;
    char request_id[NUWAX_MEETING_REQUEST_ID_BYTES];
    char audio_session_id[NUWAX_MEETING_AUDIO_SESSION_ID_BYTES];
    char state[NUWAX_MEETING_STATE_BYTES];
    char reason[NUWAX_MEETING_REASON_BYTES];
    bool ok;
    bool interrupt_current_turn;
    bool current_turn_cancelled;
} nuwax_meeting_event_t;

typedef struct {
    QueueHandle_t events;
    QueueHandle_t request_acks;
    nuwax_meeting_mode_callbacks_t callbacks;
    _Atomic nuwax_meeting_state_t state;
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
    bool cloud_authorized;
    bool pending_command_ack;
    bool pending_command_enabled;
    bool pending_ui_close;
    bool close_survives_disconnect;
    bool offline_resume_required;
    nuwax_audio_mode_identity_t identity;
    bool interrupt_current_turn;
    uint32_t request_sequence;
    uint32_t start_deadline_ms;
    uint32_t pending_command_ack_attempts;
    uint32_t pending_command_ack_deadline_ms;
    uint32_t capture_frames;
    uint32_t uploaded_frames;
    uint32_t dropped_frames;
    uint32_t consecutive_read_timeouts;
    char pending_request_id[NUWAX_MEETING_REQUEST_ID_BYTES];
    /* Closure acknowledgement outlives local capture teardown. */
    char close_request_id[NUWAX_MEETING_REQUEST_ID_BYTES];
    uint32_t close_request_epoch;
    uint32_t close_retry_ms;
    uint32_t close_started_ms;
    unsigned close_attempts;
    char close_reason[NUWAX_MEETING_REASON_BYTES];
    char pending_command_request_id[NUWAX_MEETING_REQUEST_ID_BYTES];
    char audio_session_id[NUWAX_MEETING_AUDIO_SESSION_ID_BYTES];
    char stop_reason[NUWAX_MEETING_REASON_BYTES];
    nuwax_meeting_processor_t processor;
    nuwax_meeting_inactivity_tracker_t inactivity;
} nuwax_meeting_mode_t;

esp_err_t nuwax_meeting_mode_init(nuwax_meeting_mode_t *mode,
                                  const nuwax_meeting_mode_callbacks_t *callbacks);
void nuwax_meeting_mode_deinit(nuwax_meeting_mode_t *mode);
/* Connection/application owner only; reports state without starting/stopping. */
void nuwax_meeting_mode_sync_current_state(nuwax_meeting_mode_t *mode,
                                          nuwax_ws_client_t *ws);

nuwax_meeting_state_t nuwax_meeting_mode_state(const nuwax_meeting_mode_t *mode);
bool nuwax_meeting_mode_has_pending(const nuwax_meeting_mode_t *mode);
bool nuwax_meeting_mode_has_audio_claim(const nuwax_meeting_mode_t *mode);
bool nuwax_meeting_mode_blocks_ota(const nuwax_meeting_mode_t *mode);
bool nuwax_meeting_mode_should_preempt_normal(const nuwax_meeting_mode_t *mode);
bool nuwax_meeting_mode_take_offline_resume(nuwax_meeting_mode_t *mode);
bool nuwax_meeting_mode_offline_resume_pending(
    const nuwax_meeting_mode_t *mode);

esp_err_t nuwax_meeting_mode_request_ui(nuwax_meeting_mode_t *mode,
                                        nuwax_ws_client_t *ws,
                                        bool enabled);

/* Called by the WebSocket RX callback.  Returns true if payload was consumed. */
bool nuwax_meeting_mode_handle_protocol_text(nuwax_meeting_mode_t *mode,
                                             nuwax_ws_client_t *ws,
                                             const char *payload,
                                             int len);

/* Drain controls on the application owner without starting capture. This lets
 * an admitted cross-mode start request the current owner's normal stop. */
void nuwax_meeting_mode_poll_control_events(nuwax_meeting_mode_t *mode,
                                          nuwax_ws_client_t *ws);
/* Runs the pending/active meeting session on the application task. */
esp_err_t nuwax_meeting_mode_run(nuwax_meeting_mode_t *mode,
                                 nuwax_ws_client_t *ws);

/* Requests an ordered stop so another exclusive audio mode can take over the
 * same WebSocket epoch. Active capture is stopped by its owner task; queued
 * controls are rejected without starting a transient capture. */
esp_err_t nuwax_meeting_mode_request_handoff_stop(
    nuwax_meeting_mode_t *mode,
    nuwax_ws_client_t *ws,
    const char *reason);

/* Owner-task recovery hook. It only rejects queued controls while the mode
 * is DISABLED; an active capture owner is never torn down by this API. */
esp_err_t nuwax_meeting_mode_abort_pending_controls(
    nuwax_meeting_mode_t *mode,
    nuwax_ws_client_t *ws,
    uint32_t ws_connection_epoch,
    const char *reason);

void nuwax_meeting_mode_notify_ws_disconnected(nuwax_meeting_mode_t *mode);

/* Nonblocking owner tick, also serviced while offline. */
void nuwax_meeting_mode_service_sd(nuwax_meeting_mode_t *mode, nuwax_ws_client_t *ws);

/* Owner tick, including while another mode is STOPPING. Never performs SD I/O. */
void nuwax_meeting_mode_service_command_deadline(nuwax_meeting_mode_t *mode, nuwax_ws_client_t *ws);
