/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "nuwax_ota.h"

#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

static const char *TAG = "nuwax_ota";

#define NUWAX_OTA_FAILED_DWELL_MS  (2U * 60U * 1000U)
#define NUWAX_OTA_PROGRESS_STEP_PERCENT 5

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static nuwax_ota_state_t s_state = NUWAX_OTA_STATE_IDLE;
static uint32_t s_state_entered_ms;
static bool s_abort_requested;
static int s_progress_report_high_water;
static nuwax_ota_callbacks_t s_cbs;
static nuwax_ota_stats_t s_stats;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

const char *nuwax_ota_state_name(nuwax_ota_state_t state)
{
    switch (state) {
    case NUWAX_OTA_STATE_IDLE:        return "idle";
    case NUWAX_OTA_STATE_DOWNLOADING: return "downloading";
    case NUWAX_OTA_STATE_WRITING:     return "writing";
    case NUWAX_OTA_STATE_VERIFYING:   return "verifying";
    case NUWAX_OTA_STATE_DONE:        return "done";
    case NUWAX_OTA_STATE_FAILED:      return "failed";
    default:                          return "unknown";
    }
}

static void emit_progress(int pct, const char *detail)
{
    nuwax_ota_callbacks_t cbs;

    if (pct < 0) {
        pct = 0;
    } else if (pct > 100) {
        pct = 100;
    }

    portENTER_CRITICAL(&s_lock);
    s_stats.pct = pct;
    if (detail != NULL) {
        strlcpy(s_stats.last_reason, detail, sizeof(s_stats.last_reason));
    }
    cbs = s_cbs;
    portEXIT_CRITICAL(&s_lock);

    if (cbs.on_progress != NULL) {
        cbs.on_progress(cbs.ctx, nuwax_ota_state(), pct, detail);
    }
}

static esp_err_t set_state_internal(nuwax_ota_state_t expected,
                                    nuwax_ota_state_t next,
                                    const char *reason,
                                    bool reject_if_cancelled)
{
    nuwax_ota_callbacks_t cbs;
    nuwax_ota_state_t previous;
    uint32_t entered = now_ms();

    portENTER_CRITICAL(&s_lock);
    previous = s_state;
    if (previous != expected || (reject_if_cancelled && s_abort_requested)) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_state = next;
    s_stats.state = next;
    s_state_entered_ms = entered;
    if (next == NUWAX_OTA_STATE_DOWNLOADING) {
        s_stats.started_ms = entered;
    }
    if ((next == NUWAX_OTA_STATE_DONE || next == NUWAX_OTA_STATE_FAILED) &&
        s_stats.started_ms != 0U) {
        s_stats.elapsed_ms = entered - s_stats.started_ms;
    }
    if (reason != NULL) {
        strlcpy(s_stats.last_reason, reason, sizeof(s_stats.last_reason));
    }
    cbs = s_cbs;
    portEXIT_CRITICAL(&s_lock);

    ESP_LOGI(TAG, "state %s -> %s reason=%s",
             nuwax_ota_state_name(previous), nuwax_ota_state_name(next),
             reason != NULL ? reason : "-");
    if (cbs.on_state_change != NULL) {
        cbs.on_state_change(cbs.ctx, next, reason);
    }
    return ESP_OK;
}

static esp_err_t set_state(nuwax_ota_state_t expected,
                           nuwax_ota_state_t next,
                           const char *reason)
{
    return set_state_internal(expected, next, reason, false);
}

static esp_err_t set_state_unless_cancelled(nuwax_ota_state_t expected,
                                             nuwax_ota_state_t next,
                                             const char *reason)
{
    return set_state_internal(expected, next, reason, true);
}

esp_err_t nuwax_ota_bind(const nuwax_ota_callbacks_t *cbs)
{
    if (cbs == NULL || cbs->on_progress == NULL ||
        cbs->on_state_change == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lock);
    s_cbs = *cbs;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t nuwax_ota_init(void)
{
    uint32_t initialized_at = now_ms();
    portENTER_CRITICAL(&s_lock);
    memset(&s_stats, 0, sizeof(s_stats));
    s_state = NUWAX_OTA_STATE_IDLE;
    s_stats.state = NUWAX_OTA_STATE_IDLE;
    s_state_entered_ms = initialized_at;
    s_abort_requested = false;
    s_progress_report_high_water = 0;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

nuwax_ota_state_t nuwax_ota_state(void)
{
    nuwax_ota_state_t state;
    portENTER_CRITICAL(&s_lock);
    state = s_state;
    portEXIT_CRITICAL(&s_lock);
    return state;
}

esp_err_t nuwax_ota_start(const char *manifest_json, uint32_t total_bytes)
{
    if (manifest_json == NULL || manifest_json[0] == '\0' || total_bytes == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    nuwax_ota_state_t state = nuwax_ota_state();
    if (state == NUWAX_OTA_STATE_FAILED) {
        esp_err_t err = set_state(NUWAX_OTA_STATE_FAILED,
                                  NUWAX_OTA_STATE_IDLE, "retry");
        if (err != ESP_OK) {
            return err;
        }
    } else if (state != NUWAX_OTA_STATE_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_lock);
    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.state = NUWAX_OTA_STATE_IDLE;
    s_stats.total_bytes = total_bytes;
    s_abort_requested = false;
    s_progress_report_high_water = 0;
    portEXIT_CRITICAL(&s_lock);

    esp_err_t err = set_state(NUWAX_OTA_STATE_IDLE,
                              NUWAX_OTA_STATE_DOWNLOADING, "start");
    if (err == ESP_OK) {
        emit_progress(0, "connecting");
    }
    return err;
}

esp_err_t nuwax_ota_cancel(const char *reason)
{
    esp_err_t err = ESP_OK;
    portENTER_CRITICAL(&s_lock);
    const nuwax_ota_state_t state = s_state;
    if (state == NUWAX_OTA_STATE_IDLE || state == NUWAX_OTA_STATE_FAILED) {
        err = ESP_ERR_INVALID_STATE;
    } else if (state != NUWAX_OTA_STATE_DOWNLOADING &&
               state != NUWAX_OTA_STATE_WRITING) {
        /* VERIFYING is the short, non-cancellable commit phase. */
        err = ESP_ERR_INVALID_STATE;
    } else {
        s_abort_requested = true;
        strlcpy(s_stats.last_reason,
                reason != NULL ? reason : "cancelled",
                sizeof(s_stats.last_reason));
    }
    portEXIT_CRITICAL(&s_lock);
    /* The transport worker owns the HTTP and OTA handles. It observes this
     * flag, aborts and frees them, and only then publishes FAILED. */
    return err;
}

bool nuwax_ota_cancel_requested(void)
{
    bool requested;
    portENTER_CRITICAL(&s_lock);
    requested = s_abort_requested;
    portEXIT_CRITICAL(&s_lock);
    return requested;
}

void nuwax_ota_fail(const char *reason)
{
    nuwax_ota_state_t state;

    portENTER_CRITICAL(&s_lock);
    s_abort_requested = true;
    state = s_state;
    portEXIT_CRITICAL(&s_lock);

    if (state == NUWAX_OTA_STATE_DOWNLOADING ||
        state == NUWAX_OTA_STATE_WRITING ||
        state == NUWAX_OTA_STATE_VERIFYING) {
        if (set_state(state, NUWAX_OTA_STATE_FAILED,
                      reason != NULL ? reason : "failed") == ESP_OK) {
            emit_progress(0, reason != NULL ? reason : "failed");
        }
    }
}

bool nuwax_ota_tick(uint32_t current_ms)
{
    nuwax_ota_state_t state;
    uint32_t started;
    uint32_t entered;

    portENTER_CRITICAL(&s_lock);
    state = s_state;
    started = s_stats.started_ms;
    entered = s_state_entered_ms;
    if (state != NUWAX_OTA_STATE_IDLE && state != NUWAX_OTA_STATE_DONE) {
        s_stats.elapsed_ms = current_ms - started;
    }
    portEXIT_CRITICAL(&s_lock);

    if (state == NUWAX_OTA_STATE_DOWNLOADING ||
        state == NUWAX_OTA_STATE_WRITING) {
        if ((uint32_t)(current_ms - started) >=
            (uint32_t)CONFIG_NUWAX_OTA_TOTAL_TIMEOUT_MS) {
            nuwax_ota_fail("total_timeout");
        }
        return true;
    }
    if (state == NUWAX_OTA_STATE_VERIFYING) {
        return true;
    }
    if (state == NUWAX_OTA_STATE_FAILED) {
        if ((uint32_t)(current_ms - entered) >= NUWAX_OTA_FAILED_DWELL_MS) {
            (void)set_state(NUWAX_OTA_STATE_FAILED,
                            NUWAX_OTA_STATE_IDLE, "dwell_expired");
            return false;
        }
        return true;
    }
    return false;
}

esp_err_t nuwax_ota_enter_writing(int pct)
{
    esp_err_t err = set_state_unless_cancelled(
        NUWAX_OTA_STATE_DOWNLOADING, NUWAX_OTA_STATE_WRITING,
        "download_started");
    if (err == ESP_OK) {
        emit_progress(pct >= 100 ? 99 : pct, "writing");
    }
    return err;
}

esp_err_t nuwax_ota_enter_verifying(int pct)
{
    esp_err_t err = set_state_unless_cancelled(
        NUWAX_OTA_STATE_WRITING, NUWAX_OTA_STATE_VERIFYING,
        "write_complete");
    if (err == ESP_OK) {
        emit_progress(pct >= 100 ? 99 : pct, "verifying");
    }
    return err;
}

esp_err_t nuwax_ota_enter_done(int pct)
{
    esp_err_t err = set_state(NUWAX_OTA_STATE_VERIFYING,
                              NUWAX_OTA_STATE_DONE, "verify_ok");
    if (err == ESP_OK) {
        emit_progress(pct, "done");
    }
    return err;
}

void nuwax_ota_set_total_bytes(uint32_t total)
{
    portENTER_CRITICAL(&s_lock);
    s_stats.total_bytes = total;
    portEXIT_CRITICAL(&s_lock);
}

void nuwax_ota_set_downloaded_bytes(uint32_t got)
{
    uint32_t total;
    int pct = 0;
    int report_high_water = 0;
    bool notify = false;
    nuwax_ota_callbacks_t cbs = {0};
    nuwax_ota_state_t state = NUWAX_OTA_STATE_IDLE;

    portENTER_CRITICAL(&s_lock);
    s_stats.bytes_downloaded = got;
    total = s_stats.total_bytes;
    if (total > 0U) {
        pct = (int)(((uint64_t)got * 100ULL) / total);
        if (pct >= 100) {
            pct = 99;
        }
        s_stats.pct = pct;
        report_high_water = s_progress_report_high_water;
        notify = pct > report_high_water &&
                 (pct >= 99 ||
                 pct / NUWAX_OTA_PROGRESS_STEP_PERCENT >
                     report_high_water / NUWAX_OTA_PROGRESS_STEP_PERCENT);
        if (notify) {
            s_progress_report_high_water = pct;
            cbs = s_cbs;
            state = s_state;
        }
    }
    portEXIT_CRITICAL(&s_lock);

    if (notify && cbs.on_progress != NULL) {
        cbs.on_progress(cbs.ctx, state, pct, "writing");
    }
}

void nuwax_ota_set_target_version(const char *version)
{
    if (version == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    strlcpy(s_stats.target_version, version, sizeof(s_stats.target_version));
    portEXIT_CRITICAL(&s_lock);
}

void nuwax_ota_set_target_sha256(const char *sha256_hex)
{
    if (sha256_hex == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    strlcpy(s_stats.target_sha256, sha256_hex,
            sizeof(s_stats.target_sha256));
    portEXIT_CRITICAL(&s_lock);
}

void nuwax_ota_get_stats(nuwax_ota_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    *out = s_stats;
    out->state = s_state;
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t nuwax_ota_ack_and_reboot(void)
{
    nuwax_ota_callbacks_t cbs;
    if (nuwax_ota_state() != NUWAX_OTA_STATE_DONE) {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_lock);
    cbs = s_cbs;
    portEXIT_CRITICAL(&s_lock);
    if (cbs.on_reboot_pending != NULL) {
        cbs.on_reboot_pending(cbs.ctx);
    }
    ESP_LOGW(TAG, "restarting into staged OTA image");
    esp_restart();
    return ESP_OK;
}

esp_err_t nuwax_ota_get_running_image_state(esp_ota_img_states_t *out_state)
{
    if (out_state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    return esp_ota_get_state_partition(running, out_state);
}

esp_err_t nuwax_ota_running_image_pending_verify(bool *out_pending)
{
    if (out_pending == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_pending = false;
    esp_ota_img_states_t state;
    esp_err_t err = nuwax_ota_get_running_image_state(&state);
    if (err == ESP_ERR_NOT_SUPPORTED || err == ESP_ERR_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    *out_pending = state == ESP_OTA_IMG_NEW ||
                   state == ESP_OTA_IMG_PENDING_VERIFY;
    return ESP_OK;
}

esp_err_t nuwax_ota_confirm_running_image(void)
{
    esp_ota_img_states_t state;
    esp_err_t err = nuwax_ota_get_running_image_state(&state);
    if (err != ESP_OK) {
        return err;
    }
    if (state == ESP_OTA_IMG_VALID) {
        return ESP_OK;
    }
    if (state != ESP_OTA_IMG_NEW && state != ESP_OTA_IMG_PENDING_VERIFY) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_ota_mark_app_valid_cancel_rollback();
}

esp_err_t nuwax_ota_rollback_running_image_and_reboot(void)
{
    bool pending = false;
    esp_err_t err = nuwax_ota_running_image_pending_verify(&pending);
    if (err != ESP_OK) {
        return err;
    }
    if (!pending) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_ota_mark_app_invalid_rollback_and_reboot();
}
