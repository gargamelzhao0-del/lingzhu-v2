#pragma once

#include "session_protocol.h"

/* Initialize once on the application owner before admitting conversation
 * capture. The WebSocket client must outlive this permanent sender. */
esp_err_t nuwax_control_ack_tx_init(void);

/* Copies all fields and never waits for queue space or network I/O. A failed
 * admission must not fall back to synchronous I/O on the capture owner. */
esp_err_t nuwax_control_ack_tx_enqueue(nuwax_ws_client_t *ws,
                                     const nuwax_session_command_t *command,
                                     bool ok, const char *message,
                                     uint32_t epoch);
