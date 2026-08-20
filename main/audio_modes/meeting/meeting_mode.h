#pragma once

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
    bool initialized;
    bool capture_running;
    bool cloud_authorized;
    bool pending_command_ack;
    bool pending_command_enabled;
    bool pending_ui_close;
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
    char pending_command_request_id[NUWAX_MEETING_REQUEST_ID_BYTES];
    char audio_session_id[NUWAX_MEETING_AUDIO_SESSION_ID_BYTES];
    char stop_reason[NUWAX_MEETING_REASON_BYTES];
    nuwax_meeting_processor_t processor;
    nuwax_meeting_inactivity_tracker_t inactivity;
} nuwax_meeting_mode_t;

esp_err_t nuwax_meeting_mode_init(nuwax_meeting_mode_t *mode,
                                  const nuwax_meeting_mode_callbacks_t *callbacks);
void nuwax_meeting_mode_deinit(nuwax_meeting_mode_t *mode);

nuwax_meeting_state_t nuwax_meeting_mode_state(const nuwax_meeting_mode_t *mode);
bool nuwax_meeting_mode_has_pending(const nuwax_meeting_mode_t *mode);
bool nuwax_meeting_mode_has_audio_claim(const nuwax_meeting_mode_t *mode);
bool nuwax_meeting_mode_blocks_ota(const nuwax_meeting_mode_t *mode);
bool nuwax_meeting_mode_should_preempt_normal(const nuwax_meeting_mode_t *mode);

esp_err_t nuwax_meeting_mode_request_ui(nuwax_meeting_mode_t *mode,
                                        nuwax_ws_client_t *ws,
                                        bool enabled);

/* Called by the WebSocket RX callback.  Returns true if payload was consumed. */
bool nuwax_meeting_mode_handle_protocol_text(nuwax_meeting_mode_t *mode,
                                             nuwax_ws_client_t *ws,
                                             const char *payload,
                                             int len);

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
