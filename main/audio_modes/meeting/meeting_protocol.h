#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "ws_transport.h"

#define NUWAX_MEETING_REQUEST_ID_BYTES 48
#define NUWAX_MEETING_AUDIO_SESSION_ID_BYTES 48
#define NUWAX_MEETING_MODE_BYTES 24
#define NUWAX_MEETING_STATE_BYTES 16
#define NUWAX_MEETING_REASON_BYTES 32

typedef enum {
    NUWAX_MEETING_PROTOCOL_EVENT_NONE = 0,
    NUWAX_MEETING_PROTOCOL_EVENT_REQUEST_ACK,
    NUWAX_MEETING_PROTOCOL_EVENT_SET_COMMAND,
} nuwax_meeting_protocol_event_type_t;

typedef struct {
    nuwax_meeting_protocol_event_type_t type;
    char request_id[NUWAX_MEETING_REQUEST_ID_BYTES];
    char mode[NUWAX_MEETING_MODE_BYTES];
    char state[NUWAX_MEETING_STATE_BYTES];
    char audio_session_id[NUWAX_MEETING_AUDIO_SESSION_ID_BYTES];
    char reason[NUWAX_MEETING_REASON_BYTES];
    bool ok;
    bool interrupt_current_turn;
    bool current_turn_cancelled;
} nuwax_meeting_protocol_event_t;

/* Returns true when payload is a valid meeting-mode control message. */
bool nuwax_meeting_protocol_parse(const char *payload,
                                  int len,
                                  nuwax_meeting_protocol_event_t *event);

esp_err_t nuwax_meeting_protocol_send_request(nuwax_ws_client_t *ws,
                                              const char *request_id,
                                              bool enabled,
                                              bool interrupt_current_turn,
                                              const char *reason);

esp_err_t nuwax_meeting_protocol_send_command_ack(nuwax_ws_client_t *ws,
                                                  const char *request_id,
                                                  bool enabled,
                                                  bool ok,
                                                  const char *message);

esp_err_t nuwax_meeting_protocol_send_command_ack_with_session(nuwax_ws_client_t *ws,
    const char *request_id, bool enabled, bool ok, const char *message,
    const char *audio_session_id);

esp_err_t nuwax_meeting_protocol_send_resume_request(nuwax_ws_client_t *ws,
    const char *request_id, const char *audio_session_id);

/* Same wire ACK, bounded by the original command deadline and connection. */
esp_err_t nuwax_meeting_protocol_send_command_ack_until(nuwax_ws_client_t *ws,
    const char *request_id, bool enabled, bool ok, const char *message,
    const char *audio_session_id, int64_t deadline_us, uint32_t expected_epoch);
