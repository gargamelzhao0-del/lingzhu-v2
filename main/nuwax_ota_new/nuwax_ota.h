/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * OTA state machine and boot-validation API.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_ota_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NUWAX_OTA_STATE_IDLE = 0,
    NUWAX_OTA_STATE_DOWNLOADING,
    NUWAX_OTA_STATE_WRITING,
    NUWAX_OTA_STATE_VERIFYING,
    NUWAX_OTA_STATE_DONE,
    NUWAX_OTA_STATE_FAILED,
} nuwax_ota_state_t;

const char *nuwax_ota_state_name(nuwax_ota_state_t state);

typedef struct {
    void *ctx;
    void (*on_progress)(void *ctx, nuwax_ota_state_t state, int pct,
                        const char *detail);
    void (*on_state_change)(void *ctx, nuwax_ota_state_t state,
                            const char *reason);
    void (*on_reboot_pending)(void *ctx);
} nuwax_ota_callbacks_t;

typedef struct {
    nuwax_ota_state_t state;
    int pct;
    uint32_t bytes_downloaded;
    uint32_t total_bytes;
    uint32_t started_ms;
    uint32_t elapsed_ms;
    char last_reason[64];
    char target_version[32];
    char target_sha256[65];
} nuwax_ota_stats_t;

esp_err_t nuwax_ota_bind(const nuwax_ota_callbacks_t *cbs);
esp_err_t nuwax_ota_init(void);
nuwax_ota_state_t nuwax_ota_state(void);

/* Start only arms the state machine. The transport is started separately. */
esp_err_t nuwax_ota_start(const char *manifest_json, uint32_t total_bytes);

/* Cooperative cancellation. The transport checks this before every write. */
esp_err_t nuwax_ota_cancel(const char *reason);
bool nuwax_ota_cancel_requested(void);

/* Returns true while an update or failure dwell period is active. */
bool nuwax_ota_tick(uint32_t now_ms);

/* Reboot into the staged image after the caller has acknowledged DONE. */
esp_err_t nuwax_ota_ack_and_reboot(void);

/* First-boot health policy. These never auto-confirm a new image. */
esp_err_t nuwax_ota_get_running_image_state(esp_ota_img_states_t *out_state);
esp_err_t nuwax_ota_running_image_pending_verify(bool *out_pending);
esp_err_t nuwax_ota_confirm_running_image(void);
esp_err_t nuwax_ota_rollback_running_image_and_reboot(void);

void nuwax_ota_fail(const char *reason);
void nuwax_ota_get_stats(nuwax_ota_stats_t *out);

/* Transport-facing state/progress hooks. */
esp_err_t nuwax_ota_enter_writing(int pct);
esp_err_t nuwax_ota_enter_verifying(int pct);
esp_err_t nuwax_ota_enter_done(int pct);
void nuwax_ota_set_total_bytes(uint32_t total);
void nuwax_ota_set_downloaded_bytes(uint32_t got);
void nuwax_ota_set_target_version(const char *version);
void nuwax_ota_set_target_sha256(const char *sha256_hex);

#ifdef __cplusplus
}
#endif
