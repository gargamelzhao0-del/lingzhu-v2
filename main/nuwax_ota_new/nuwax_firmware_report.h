#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "ws_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NUWAX_FIRMWARE_REPORT_ID_MAX_LEN        40U

/* Initializes one boot-scoped report identity. It must be called once from
 * app startup, not from the reconnect loop. */
esp_err_t nuwax_firmware_report_init(void);

/* Sends the boot-scoped report when needed. A missing running-image identity
 * is recovered by a bounded background worker and returns
 * ESP_ERR_NOT_FINISHED until ready. A reconnect may retransmit the
 * same logical report until a persisted ACK arrives; an ACKed report is never
 * sent again during this application boot. This function never gates audio. */
esp_err_t nuwax_firmware_report_service(nuwax_ws_client_t *ws,
                                        uint32_t connection_epoch,
                                        uint32_t now_ms);

/* Consumes firmware_report_ack frames, including malformed or stale ACKs.
 * Returns false for messages owned by another protocol. */
bool nuwax_firmware_report_handle_text(const char *payload,
                                       int len,
                                       uint32_t connection_epoch);

bool nuwax_firmware_report_is_acked(void);
const char *nuwax_firmware_report_id(void);

#ifdef __cplusplus
}
#endif
