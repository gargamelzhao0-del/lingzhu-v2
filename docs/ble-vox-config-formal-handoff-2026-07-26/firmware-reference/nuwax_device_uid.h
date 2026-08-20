/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * nuwax_device_uid.h
 *
 * Stable, change-proof device identifier derived from the ESP32-S3 eFuse
 * unique chip ID. The raw 128-bit eFuse field is never exposed; we hash it
 * with SHA-256 and keep the first 16 bytes. The result is then persisted
 * to NVS so subsequent boots don't have to re-derive, while still being
 * fully recoverable from the immutable eFuse if NVS is wiped.
 *
 * Format:
 *   - uid_string:  "NX-AABB-CCD..." (15 chars + NUL) — human-friendly
 *   - qr_payload:  "NUWAX7:NX-AABB-CCD..." — what the companion app scans
 *
 * The string is also the value reported to the Cloud Gateway as the
 * `device_id` field of the WebSocket hello frame; it replaces the
 * Kconfig-time constant CONFIG_NUWAX_DEVICE_ID.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NUWAX_DEVICE_UID_STRING_LEN 18    /* "NX-AABB-CCDD-EEFF" + NUL */
#define NUWAX_DEVICE_UID_QR_LEN     32    /* "NUWAX7:" + uid + slack */

typedef struct {
    char    uid_string[NUWAX_DEVICE_UID_STRING_LEN];
    char    qr_payload[NUWAX_DEVICE_UID_QR_LEN];
    uint8_t raw[16];
    bool    from_nvs_cache;            /* true = read back; false = freshly derived */
} nuwax_device_uid_t;

/* Idempotent. Must be called once after nvs_flash_init() but before
 * nuwax_session_send_hello() or any BLE activity. Safe to call repeatedly. */
esp_err_t nuwax_device_uid_init(void);

/* Always valid after a successful init(). Never returns NULL. */
const char                 *nuwax_device_uid_string(void);
const char                 *nuwax_device_uid_qr_payload(void);

/* Snapshot of the last computed value, or NULL before init. */
const nuwax_device_uid_t   *nuwax_device_uid_get(void);

#ifdef __cplusplus
}
#endif
