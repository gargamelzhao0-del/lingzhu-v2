/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Integration facade for cloud events, controller transport and NVS status.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void *ctx;
    /* Internal event sink used by the local UI/controller only. */
    esp_err_t (*post_local_event)(void *ctx, const char *json_event);
} nuwax_ota_main_callbacks_t;

#define NUWAX_OTA_RUNNING_IDENTITY_SHA256_LEN 65

typedef struct {
    char image_sha256[NUWAX_OTA_RUNNING_IDENTITY_SHA256_LEN];
    uint64_t release_sequence;
    uint32_t target_partition_address;
    bool pending_verify;
} nuwax_ota_running_identity_t;

esp_err_t nuwax_ota_main_register(const nuwax_ota_main_callbacks_t *callbacks);
esp_err_t nuwax_ota_main_init(void);

/* Parses and persists the manifest, enters DOWNLOADING and arms transport. */
esp_err_t nuwax_ota_main_start(const char *manifest_json);

/* Runs the armed transport synchronously on the caller task. The caller must
 * provide an internal-RAM stack that remains accessible during flash writes. */
esp_err_t nuwax_ota_main_run_transport(void);

/* Cancels download/write. The short VERIFYING commit phase is not cancellable. */
esp_err_t nuwax_ota_main_cancel(const char *reason);

/* Drives timeout accounting; call from the application at about 100 ms. */
esp_err_t nuwax_ota_main_tick(uint32_t now_ms);

/* Call after the DONE event has been delivered to reboot into the new image. */
esp_err_t nuwax_ota_main_reboot_staged_image(void);

/* First boot: run health checks, then call exactly one of these decisions. */
esp_err_t nuwax_ota_main_running_image_pending_verify(bool *out_pending);
esp_err_t nuwax_ota_main_confirm_running_image(void);
esp_err_t nuwax_ota_main_rollback_running_image_and_reboot(void);
esp_err_t nuwax_ota_main_get_release_sequence(uint64_t *out_sequence);
/* Returns the manifest identity only when its durable transaction matches the
 * currently running partition, application version and release sequence. */
esp_err_t nuwax_ota_main_get_running_identity(
    nuwax_ota_running_identity_t *out_identity);
esp_err_t nuwax_ota_main_get_running_image_sha256(
    char out_sha256[NUWAX_OTA_RUNNING_IDENTITY_SHA256_LEN]);
esp_err_t nuwax_ota_main_pending_identity_matches(
    uint32_t target_partition_address,
    const char *image_sha256,
    bool *out_matches);
esp_err_t nuwax_ota_main_accepted_identity_matches(
    uint32_t target_partition_address,
    const char *image_sha256,
    bool *out_matches);

/* Worker-idle becomes true only after transport cleanup/esp_ota_abort has
 * completed. Terminal-complete additionally requires DONE or FAILED. */
bool nuwax_ota_main_worker_idle(void);
bool nuwax_ota_main_terminal_complete(void);

const char *nuwax_ota_main_last_status(void);

#ifdef __cplusplus
}
#endif
