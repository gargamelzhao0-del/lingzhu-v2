/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Blocking HTTPS-to-OTA-partition transport. Run it from an internal-RAM task.
 */
#pragma once

#include "esp_err.h"
#include "nuwax_ota_manifest.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NUWAX_OTA_TARGET_BOARD "miaopan_v2_0_n32r8"
#define NUWAX_OTA_TARGET_CHIP "esp32s3"
#define NUWAX_OTA_PARTITION_SCHEMA 1U
#define NUWAX_OTA_BUILD_FLAVOR "product"

/*
 * Security invariants:
 * - HTTPS server identity is checked with the ESP certificate bundle.
 * - HTTP Content-Length and byte count must equal manifest.size_bytes.
 * - SHA-256 is mandatory and covers every received firmware byte.
 * - esp_ota_end() must validate the application image before boot selection.
 * - the boot partition is changed only after every check succeeds.
 */
esp_err_t nuwax_ota_transport_run(const nuwax_ota_manifest_t *manifest);

#ifdef __cplusplus
}
#endif
