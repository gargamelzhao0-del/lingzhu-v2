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

/* Admission uses fresh, plausible telemetry, independently of CEDV profile
 * authentication and the UI's higher low-battery warning threshold. */
static inline nuwax_ota_power_result_t nuwax_ota_power_evaluate_admission(
    bool sample_available, bool stale, bool battery_present, bool valid,
    bool hard_low_voltage, uint8_t percent, uint8_t minimum_percent)
{
    if (!sample_available || stale || !battery_present || !valid)
        return NUWAX_OTA_POWER_UNKNOWN;
    if (percent > 100U) return NUWAX_OTA_POWER_UNKNOWN;
    if (hard_low_voltage) return NUWAX_OTA_POWER_LOW;
    return nuwax_ota_power_evaluate(true, false, true, false, true, false,
                                     percent, minimum_percent);
}

/* Confirming an already-written image is not admission to another download.
 * Require fresh physical power telemetry, but do not turn SOC model drift or
 * a percentage crossing the upgrade threshold into an image-health failure. */
static inline nuwax_ota_power_result_t nuwax_ota_boot_power_evaluate(
    bool power_valid, bool power_stale, bool hard_low_voltage)
{
    if (!power_valid || power_stale) return NUWAX_OTA_POWER_UNKNOWN;
    return hard_low_voltage ? NUWAX_OTA_POWER_LOW : NUWAX_OTA_POWER_ALLOWED;
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
