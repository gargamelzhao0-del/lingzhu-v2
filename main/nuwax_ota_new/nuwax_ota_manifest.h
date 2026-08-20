/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Strict OTA manifest parsing and streaming SHA-256 helpers.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "psa/crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NUWAX_OTA_MANIFEST_URL_MAX_LEN       256
#define NUWAX_OTA_MANIFEST_VERSION_MAX_LEN   32
#define NUWAX_OTA_MANIFEST_ID_MAX_LEN        64
#define NUWAX_OTA_MANIFEST_SHA256_LEN         65
#define NUWAX_OTA_MANIFEST_MAX_SIZE           (4U * 1024U * 1024U)

typedef struct {
    char update_id[NUWAX_OTA_MANIFEST_ID_MAX_LEN];
    char url[NUWAX_OTA_MANIFEST_URL_MAX_LEN];
    char version[NUWAX_OTA_MANIFEST_VERSION_MAX_LEN];
    uint32_t size_bytes;
    char image_sha256[NUWAX_OTA_MANIFEST_SHA256_LEN];
} nuwax_ota_manifest_t;

esp_err_t nuwax_ota_manifest_parse(const char *json, size_t json_len,
                                   nuwax_ota_manifest_t *out);
esp_err_t nuwax_ota_manifest_validate(const nuwax_ota_manifest_t *manifest);
int nuwax_ota_manifest_version_compare(const char *a, const char *b);

typedef struct {
    psa_hash_operation_t ctx;
    bool started;
} nuwax_ota_sha256_t;

esp_err_t nuwax_ota_sha256_init(nuwax_ota_sha256_t *hash);
esp_err_t nuwax_ota_sha256_update(nuwax_ota_sha256_t *hash,
                                  const uint8_t *data, size_t len);
esp_err_t nuwax_ota_sha256_final(
    nuwax_ota_sha256_t *hash,
    char out_hex[NUWAX_OTA_MANIFEST_SHA256_LEN]);
void nuwax_ota_sha256_abort(nuwax_ota_sha256_t *hash);

#ifdef __cplusplus
}
#endif
