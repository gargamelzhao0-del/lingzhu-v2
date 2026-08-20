#include "board/battery_insights.h"

#include <limits.h>
#include <string.h>

#define BATTERY_ESTIMATE_MAX_MINUTES (7U * 24U * 60U)

static bool config_valid(const nuwax_battery_insights_config_t *config)
{
    return config && config->critical_low_c < config->warning_low_c &&
           config->warning_low_c < config->warning_high_c &&
           config->warning_high_c < config->critical_high_c;
}

esp_err_t nuwax_battery_compute_insights(
    const nuwax_battery_service_snapshot_t *snapshot,
    const nuwax_battery_insights_config_t *config,
    nuwax_battery_insights_t *out)
{
    if (!snapshot || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    const nuwax_battery_insights_config_t resolved =
        config ? *config : NUWAX_BATTERY_INSIGHTS_DEFAULT_CONFIG();
    if (!config_valid(&resolved)) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->data_stale = snapshot->stale;
    out->valid = snapshot->valid && !snapshot->stale &&
                 snapshot->battery_present && snapshot->gauge_ready;
    out->current_percent_valid = snapshot->sample_available &&
                                 !snapshot->stale &&
                                 snapshot->battery_present &&
                                 snapshot->gauge_ready &&
                                 snapshot->percent <= 100U;
    out->current_percent = snapshot->percent;
    out->cycle_count = snapshot->cycle_count;
    /* Once low battery is latched, an I2C outage must not silently clear the
     * product's low-power recommendation. A fresh sample above the service's
     * exit threshold is required to release it. */
    out->low_power_recommended = snapshot->sample_available &&
                                 snapshot->battery_present && snapshot->low;

    out->temperature_valid = snapshot->sample_available &&
                             snapshot->battery_present &&
                             snapshot->temperature_c >= -30 &&
                             snapshot->temperature_c <= 90;
    out->temperature_stale = out->temperature_valid && snapshot->stale;
    out->temperature_c = snapshot->temperature_c;
    if (out->temperature_valid) {
        if (snapshot->temperature_c <= resolved.critical_low_c) {
            out->temperature_state = NUWAX_BATTERY_TEMPERATURE_COLD_CRITICAL;
            out->temperature_critical = true;
        } else if (snapshot->temperature_c <= resolved.warning_low_c) {
            out->temperature_state = NUWAX_BATTERY_TEMPERATURE_COLD_WARNING;
            out->temperature_warning = true;
        } else if (snapshot->temperature_c >= resolved.critical_high_c) {
            out->temperature_state = NUWAX_BATTERY_TEMPERATURE_HOT_CRITICAL;
            out->temperature_critical = true;
        } else if (snapshot->temperature_c > resolved.warning_high_c) {
            out->temperature_state = NUWAX_BATTERY_TEMPERATURE_HOT_WARNING;
            out->temperature_warning = true;
        } else {
            out->temperature_state = NUWAX_BATTERY_TEMPERATURE_NORMAL;
        }
    }

    if (out->valid && snapshot->soc_trusted &&
        snapshot->design_capacity_mah > 0U &&
        snapshot->full_charge_mah > 0U &&
        snapshot->full_charge_mah <= snapshot->design_capacity_mah * 2U) {
        uint32_t health = ((uint32_t)snapshot->full_charge_mah * 100U) /
                          snapshot->design_capacity_mah;
        if (health > 100U) health = 100U;
        out->health_valid = true;
        out->health_percent = (uint8_t)health;
    }

    const int32_t average_current_ma = snapshot->average_current_ma;
    if (out->valid && snapshot->average_current_valid &&
        average_current_ma < -20 &&
        snapshot->remaining_mah > 0U) {
        const uint32_t minutes =
            ((uint32_t)snapshot->remaining_mah * 60U) /
            (uint32_t)(-average_current_ma);
        if (minutes > 0U && minutes <= BATTERY_ESTIMATE_MAX_MINUTES) {
            out->remaining_time_valid = true;
            out->remaining_minutes = minutes;
        }
    } else if (out->valid && snapshot->charging &&
               snapshot->average_current_valid &&
               average_current_ma > 20 &&
               snapshot->full_charge_mah > snapshot->remaining_mah) {
        const uint32_t to_full_mah =
            (uint32_t)snapshot->full_charge_mah - snapshot->remaining_mah;
        const uint32_t minutes =
            (to_full_mah * 60U) / (uint32_t)average_current_ma;
        if (minutes > 0U && minutes <= BATTERY_ESTIMATE_MAX_MINUTES) {
            out->charge_time_valid = true;
            out->charge_minutes = minutes;
        }
    }

    return ESP_OK;
}
