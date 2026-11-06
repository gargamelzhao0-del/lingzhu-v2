#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NUWAX_BATTERY_POWER_UNKNOWN = 0,
    NUWAX_BATTERY_POWER_EXTERNAL,
    NUWAX_BATTERY_POWER_BATTERY,
    NUWAX_BATTERY_POWER_CHARGING,
    NUWAX_BATTERY_POWER_LOW,
} nuwax_battery_power_state_t;

typedef struct {
    /* valid means a fresh, bounded measurement is available from an initialized
     * gauge with a battery present. Only soc_trusted means the percentage came
     * from an authenticated cell profile. OTA admission may use valid telemetry
     * without profile authentication, with a separate hard-voltage guard. */
    bool valid;
    bool sample_available;
    bool stale;
    bool battery_present;
    bool gauge_ready;
    bool soc_trusted;
    bool power_valid;
    bool power_stale;
    uint64_t power_timestamp_ms;
    bool charging;
    bool full;
    bool low;
    bool hard_low_voltage;
    uint8_t percent;
    uint8_t display_percent;
    uint16_t voltage_mv;
    int16_t current_ma;
    bool average_current_valid;
    int16_t average_current_ma;
    int16_t temperature_c;
    uint16_t remaining_mah;
    uint16_t full_charge_mah;
    uint16_t design_capacity_mah;
    uint16_t cycle_count;
    nuwax_battery_power_state_t power_state;
    uint64_t sample_timestamp_ms;
    uint64_t last_attempt_timestamp_ms;
    uint32_t consecutive_errors;
    esp_err_t last_error;
} nuwax_battery_service_snapshot_t;

/* Called in the battery-service task. The snapshot is only valid for the
 * duration of the call, so consumers must copy it. UI consumers must marshal
 * the update through the application's LVGL locking path. */
typedef void (*nuwax_battery_service_listener_t)(
    const nuwax_battery_service_snapshot_t *snapshot,
    void *user_data);

typedef struct {
    /* Zero selects the defaults. Non-zero sampling intervals must stay in the
     * product range of 5-10 seconds. Valid SOC <=2% temporarily samples every
     * 500 ms to detect a charger during the low-battery shutdown reminder. */
    uint32_t sample_interval_ms;
    uint32_t error_backoff_initial_ms;
    uint32_t error_backoff_max_ms;
    uint8_t low_enter_percent;
    uint8_t low_exit_percent;
    uint16_t hard_low_enter_mv;
    uint16_t hard_low_exit_mv;
    uint16_t estimated_low_enter_mv;
    uint16_t estimated_low_exit_mv;
    uint16_t design_capacity_mah;
    nuwax_battery_service_listener_t listener;
    void *listener_user_data;
} nuwax_battery_service_config_t;

#define NUWAX_BATTERY_SERVICE_DEFAULT_CONFIG() \
    ((nuwax_battery_service_config_t){         \
        .sample_interval_ms = 8000,             \
        .error_backoff_initial_ms = 5000,       \
        .error_backoff_max_ms = 60000,          \
        .low_enter_percent = 10,                \
        .low_exit_percent = 15,                 \
        .hard_low_enter_mv = 3400,              \
        .hard_low_exit_mv = 3550,               \
        .estimated_low_enter_mv = 3600,         \
        .estimated_low_exit_mv = 3700,          \
        .design_capacity_mah = 1200,            \
        .listener = NULL,                       \
        .listener_user_data = NULL,             \
    })

esp_err_t nuwax_battery_service_start(const nuwax_battery_service_config_t *config);
esp_err_t nuwax_battery_service_stop(uint32_t timeout_ms);
/* Returned samples are marked stale when their age exceeds three configured
 * sampling periods, even if the sampling task stopped without reporting an
 * I2C error. */
esp_err_t nuwax_battery_service_get_snapshot(nuwax_battery_service_snapshot_t *snapshot);
bool nuwax_battery_service_is_running(void);

#ifdef __cplusplus
}
#endif
