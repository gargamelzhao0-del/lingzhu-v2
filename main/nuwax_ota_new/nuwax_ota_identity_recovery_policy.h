/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

typedef enum {
    NUWAX_OTA_IDENTITY_AVAILABLE = 0,
    NUWAX_OTA_IDENTITY_MISSING,
    NUWAX_OTA_IDENTITY_STALE,
    NUWAX_OTA_IDENTITY_STORAGE_FAILURE,
} nuwax_ota_identity_condition_t;

static inline bool nuwax_ota_identity_partition_fallback_allowed(
    bool pending_verify, nuwax_ota_identity_condition_t condition)
{
    return !pending_verify &&
           (condition == NUWAX_OTA_IDENTITY_MISSING ||
            condition == NUWAX_OTA_IDENTITY_STALE);
}
