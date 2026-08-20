#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "battery_service.h"
#include "esp_err.h"

typedef enum {
    NUWAX_BATTERY_TEMPERATURE_UNKNOWN = 0,
    NUWAX_BATTERY_TEMPERATURE_NORMAL,
    NUWAX_BATTERY_TEMPERATURE_COLD_WARNING,
    NUWAX_BATTERY_TEMPERATURE_HOT_WARNING,
    NUWAX_BATTERY_TEMPERATURE_COLD_CRITICAL,
    NUWAX_BATTERY_TEMPERATURE_HOT_CRITICAL,
} nuwax_battery_temperature_state_t;

typedef struct {
    int16_t warning_low_c;
    int16_t warning_high_c;
    int16_t critical_low_c;
    int16_t critical_high_c;
} nuwax_battery_insights_config_t;

typedef struct {
    bool valid;
    bool data_stale;
    bool current_percent_valid;
    uint8_t current_percent;
    bool remaining_time_valid;
    uint32_t remaining_minutes;
    bool charge_time_valid;
    uint32_t charge_minutes;
    bool health_valid;
    uint8_t health_percent;
    uint16_t cycle_count;
    bool low_power_recommended;
    bool temperature_valid;
    bool temperature_stale;
    int16_t temperature_c;
    nuwax_battery_temperature_state_t temperature_state;
    bool temperature_warning;
    bool temperature_critical;
} nuwax_battery_insights_t;

#define NUWAX_BATTERY_INSIGHTS_DEFAULT_CONFIG() \
    ((nuwax_battery_insights_config_t){       \
        .warning_low_c = 0,                   \
        .warning_high_c = 45,                 \
        .critical_low_c = -10,                \
        .critical_high_c = 60,                \
    })

/* Pure derivation from one bounded battery-service snapshot. It never writes
 * gauge data, changes power state, or touches the UI. */
esp_err_t nuwax_battery_compute_insights(
    const nuwax_battery_service_snapshot_t *snapshot,
    const nuwax_battery_insights_config_t *config,
    nuwax_battery_insights_t *out);
