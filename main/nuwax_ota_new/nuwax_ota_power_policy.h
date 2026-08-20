#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    NUWAX_OTA_POWER_ALLOWED = 0,
    NUWAX_OTA_POWER_UNKNOWN,
    NUWAX_OTA_POWER_LOW,
} nuwax_ota_power_result_t;

static inline nuwax_ota_power_result_t nuwax_ota_power_evaluate(
    bool sample_available,
    bool stale,
    bool battery_present,
    bool external_power,
    bool valid,
    bool product_low,
    uint8_t percent,
    uint8_t minimum_percent)
{
    if (!sample_available || stale) {
        return NUWAX_OTA_POWER_UNKNOWN;
    }
    if (!battery_present) {
        return external_power ? NUWAX_OTA_POWER_ALLOWED
                              : NUWAX_OTA_POWER_UNKNOWN;
    }
    if (!valid) {
        return NUWAX_OTA_POWER_UNKNOWN;
    }
    if (product_low) {
        return NUWAX_OTA_POWER_LOW;
    }
    return percent < minimum_percent ? NUWAX_OTA_POWER_LOW
                                     : NUWAX_OTA_POWER_ALLOWED;
}

static inline const char *nuwax_ota_power_result_code(
    nuwax_ota_power_result_t result)
{
    switch (result) {
    case NUWAX_OTA_POWER_UNKNOWN: return "power_unknown";
    case NUWAX_OTA_POWER_LOW:     return "low_power";
    case NUWAX_OTA_POWER_ALLOWED:
    default:                      return NULL;
    }
}
