#include "product_ready.h"

#include "esp_check.h"
#include "esp_flash_encrypt.h"
#include "esp_log.h"
#include "esp_secure_boot.h"
#include "sdkconfig.h"

#ifndef CONFIG_NUWAX_PRODUCTION_SECURITY_REQUIRED
#define CONFIG_NUWAX_PRODUCTION_SECURITY_REQUIRED 0
#endif

static const char *TAG = "nuwax_product_ready";

esp_err_t nuwax_product_ready_publish(
    const nuwax_product_ready_inputs_t *inputs,
    nuwax_product_ready_snapshot_t *out_snapshot)
{
    ESP_RETURN_ON_FALSE(inputs && out_snapshot,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "product ready input/output is NULL");
    nuwax_product_ready_snapshot_t snapshot = {
        .secure_boot_enabled = esp_secure_boot_enabled(),
        .flash_encryption_enabled = esp_flash_encryption_enabled(),
    };

    if (!inputs->core_tasks_ready) snapshot.blockers |= NUWAX_PRODUCT_READY_BLOCK_CORE_TASKS;
    if (!inputs->wifi_ready) snapshot.blockers |= NUWAX_PRODUCT_READY_BLOCK_WIFI;
    if (inputs->speaker_required && !inputs->speaker_ready) {
        snapshot.blockers |= NUWAX_PRODUCT_READY_BLOCK_SPEAKER;
    }
    if (inputs->ui_required && !inputs->ui_ready) snapshot.degraded_mask |= NUWAX_PRODUCT_READY_DEGRADED_UI;
    if (inputs->touch_required && !inputs->touch_ready) snapshot.degraded_mask |= NUWAX_PRODUCT_READY_DEGRADED_TOUCH;
    if (inputs->wake_word_required && !inputs->wake_word_ready) snapshot.degraded_mask |= NUWAX_PRODUCT_READY_DEGRADED_WAKE_WORD;
    if (inputs->live_aec_required && !inputs->live_aec_ready) snapshot.degraded_mask |= NUWAX_PRODUCT_READY_DEGRADED_LIVE_AEC;
    if (inputs->hardware_ref_required && !inputs->hardware_ref_released) {
        snapshot.degraded_mask |= NUWAX_PRODUCT_READY_DEGRADED_HARDWARE_REF;
    }
    if (!snapshot.secure_boot_enabled || !snapshot.flash_encryption_enabled) {
        snapshot.degraded_mask |= NUWAX_PRODUCT_READY_DEGRADED_SECURITY;
        if (CONFIG_NUWAX_PRODUCTION_SECURITY_REQUIRED) {
            snapshot.blockers |= NUWAX_PRODUCT_READY_BLOCK_SECURITY;
        }
    }

    snapshot.ready = snapshot.blockers == 0;
    snapshot.degraded = snapshot.degraded_mask != 0;
    *out_snapshot = snapshot;
    const uint32_t actionable_degraded_mask =
        snapshot.degraded_mask & ~NUWAX_PRODUCT_READY_DEGRADED_SECURITY;
    const esp_log_level_t ready_log_level =
        snapshot.blockers != 0U || actionable_degraded_mask != 0U
            ? ESP_LOG_WARN
            : ESP_LOG_INFO;
    ESP_LOG_LEVEL_LOCAL(
        ready_log_level,
        TAG,
        "PRODUCT_READY ready=%d degraded=%d blockers=0x%08lx degraded_mask=0x%08lx core_tasks=%d wifi=%d speaker=%d ui=%d touch=%d wake_word=%d live_aec=%d hardware_ref_required=%d hardware_ref=%d secure_boot=%d flash_encryption=%d security_required=%d",
        snapshot.ready ? 1 : 0,
        snapshot.degraded ? 1 : 0,
        (unsigned long)snapshot.blockers,
        (unsigned long)snapshot.degraded_mask,
        inputs->core_tasks_ready ? 1 : 0,
        inputs->wifi_ready ? 1 : 0,
        inputs->speaker_ready ? 1 : 0,
        inputs->ui_ready ? 1 : 0,
        inputs->touch_ready ? 1 : 0,
        inputs->wake_word_ready ? 1 : 0,
        inputs->live_aec_ready ? 1 : 0,
        inputs->hardware_ref_required ? 1 : 0,
        inputs->hardware_ref_released ? 1 : 0,
        snapshot.secure_boot_enabled ? 1 : 0,
        snapshot.flash_encryption_enabled ? 1 : 0,
        CONFIG_NUWAX_PRODUCTION_SECURITY_REQUIRED ? 1 : 0);
    return snapshot.ready ? ESP_OK : ESP_ERR_INVALID_STATE;
}
