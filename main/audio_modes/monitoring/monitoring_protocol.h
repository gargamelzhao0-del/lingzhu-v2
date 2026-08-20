#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "ws_transport.h"

#define NUWAX_MONITORING_REQUEST_ID_BYTES 48
#define NUWAX_MONITORING_AUDIO_SESSION_ID_BYTES 48
#define NUWAX_MONITORING_MODE_BYTES 16
#define NUWAX_MONITORING_STATE_BYTES 16
#define NUWAX_MONITORING_REASON_BYTES 32

typedef enum {
    NUWAX_MONITORING_PROTOCOL_EVENT_NONE = 0,
    NUWAX_MONITORING_PROTOCOL_EVENT_CAPTURE_CONTROL_ACK,
    NUWAX_MONITORING_PROTOCOL_EVENT_SET_COMMAND,
} nuwax_monitoring_protocol_event_type_t;

typedef struct {
    nuwax_monitoring_protocol_event_type_t type;
    char request_id[NUWAX_MONITORING_REQUEST_ID_BYTES];
    char mode[NUWAX_MONITORING_MODE_BYTES];
    char state[NUWAX_MONITORING_STATE_BYTES];
    char audio_session_id[NUWAX_MONITORING_AUDIO_SESSION_ID_BYTES];
    char reason[NUWAX_MONITORING_REASON_BYTES];
    bool enabled;
    bool ok;
} nuwax_monitoring_protocol_event_t;

/* Returns true for a valid monitoring command or capture-control ACK. */
bool nuwax_monitoring_protocol_parse(const char *payload,
                                     int len,
                                     nuwax_monitoring_protocol_event_t *event);

esp_err_t nuwax_monitoring_protocol_send_capture_control(
    nuwax_ws_client_t *ws,
    const char *request_id,
    bool pause,
    const char *audio_session_id,
    const char *reason,
    uint32_t silence_ms);

esp_err_t nuwax_monitoring_protocol_send_command_ack(nuwax_ws_client_t *ws,
                                                      const char *request_id,
                                                      bool enabled,
                                                      bool ok,
                                                      const char *message);
