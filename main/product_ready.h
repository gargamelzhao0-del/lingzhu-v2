#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    NUWAX_PRODUCT_READY_BLOCK_CORE_TASKS = 1U << 0,
    NUWAX_PRODUCT_READY_BLOCK_WIFI = 1U << 1,
    NUWAX_PRODUCT_READY_BLOCK_SPEAKER = 1U << 2,
    NUWAX_PRODUCT_READY_BLOCK_SECURITY = 1U << 3,
} nuwax_product_ready_blocker_t;

typedef enum {
    NUWAX_PRODUCT_READY_DEGRADED_UI = 1U << 0,
    NUWAX_PRODUCT_READY_DEGRADED_TOUCH = 1U << 1,
    NUWAX_PRODUCT_READY_DEGRADED_WAKE_WORD = 1U << 2,
    NUWAX_PRODUCT_READY_DEGRADED_LIVE_AEC = 1U << 3,
    NUWAX_PRODUCT_READY_DEGRADED_HARDWARE_REF = 1U << 4,
    NUWAX_PRODUCT_READY_DEGRADED_SECURITY = 1U << 5,
} nuwax_product_ready_degraded_t;

typedef struct {
    bool core_tasks_ready;
    bool wifi_ready;
    bool speaker_required;
    bool speaker_ready;
    bool ui_required;
    bool ui_ready;
    bool touch_required;
    bool touch_ready;
    bool wake_word_required;
    bool wake_word_ready;
    bool live_aec_required;
    bool live_aec_ready;
    bool hardware_ref_required;
    bool hardware_ref_released;
} nuwax_product_ready_inputs_t;

typedef struct {
    bool ready;
    bool degraded;
    bool secure_boot_enabled;
    bool flash_encryption_enabled;
    uint32_t blockers;
    uint32_t degraded_mask;
} nuwax_product_ready_snapshot_t;

esp_err_t nuwax_product_ready_publish(
    const nuwax_product_ready_inputs_t *inputs,
    nuwax_product_ready_snapshot_t *out_snapshot);
