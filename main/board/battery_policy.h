#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    NUWAX_BATTERY_LOW_REASON_NONE = 0,
    NUWAX_BATTERY_LOW_REASON_TRUSTED_SOC,
    NUWAX_BATTERY_LOW_REASON_GAUGE_ALARM,
    NUWAX_BATTERY_LOW_REASON_ESTIMATED_SOC_VOLTAGE,
    NUWAX_BATTERY_LOW_REASON_HARD_VOLTAGE,
} nuwax_battery_low_reason_t;

typedef struct {
    bool battery_present;
    bool soc_trusted;
    bool gauge_low_alarm;
    uint8_t percent;
    uint16_t voltage_mv;
    uint8_t low_enter_percent;
    uint8_t low_exit_percent;
    uint16_t hard_low_enter_mv;
    uint16_t hard_low_exit_mv;
    uint16_t estimated_low_enter_mv;
    uint16_t estimated_low_exit_mv;
} nuwax_battery_low_policy_input_t;

typedef struct {
    bool low;
    bool hard_low_voltage;
    nuwax_battery_low_reason_t reason;
} nuwax_battery_low_policy_result_t;

static inline nuwax_battery_low_policy_result_t
nuwax_battery_low_policy_update(
    bool previously_low,
    nuwax_battery_low_reason_t previous_reason,
    const nuwax_battery_low_policy_input_t *input)
{
    nuwax_battery_low_policy_result_t result = {0};
    if (!input || !input->battery_present) {
        return result;
    }

    result.hard_low_voltage =
        input->voltage_mv <= input->hard_low_enter_mv;
    if (result.hard_low_voltage) {
        result.low = true;
        result.reason = NUWAX_BATTERY_LOW_REASON_HARD_VOLTAGE;
        return result;
    }

    if (input->soc_trusted) {
        if (input->gauge_low_alarm) {
            result.low = true;
            result.reason = NUWAX_BATTERY_LOW_REASON_GAUGE_ALARM;
            return result;
        }
        if (input->percent <= input->low_enter_percent) {
            result.low = true;
            result.reason = NUWAX_BATTERY_LOW_REASON_TRUSTED_SOC;
            return result;
        }
        const bool hold_hard_low =
            previous_reason == NUWAX_BATTERY_LOW_REASON_HARD_VOLTAGE &&
            input->voltage_mv < input->hard_low_exit_mv;
        const bool hold_soc_low =
            previous_reason != NUWAX_BATTERY_LOW_REASON_HARD_VOLTAGE &&
            (input->percent < input->low_exit_percent ||
             input->voltage_mv < input->hard_low_exit_mv);
        if (previously_low && (hold_hard_low || hold_soc_low)) {
            result.low = true;
            result.reason = hold_hard_low
                                ? NUWAX_BATTERY_LOW_REASON_HARD_VOLTAGE
                                : NUWAX_BATTERY_LOW_REASON_TRUSTED_SOC;
        }
        return result;
    }

    const bool estimated_low =
        (input->percent <= input->low_enter_percent ||
         input->gauge_low_alarm) &&
        input->voltage_mv <= input->estimated_low_enter_mv;
    if (estimated_low) {
        result.low = true;
        result.reason = input->gauge_low_alarm
                            ? NUWAX_BATTERY_LOW_REASON_GAUGE_ALARM
                            : NUWAX_BATTERY_LOW_REASON_ESTIMATED_SOC_VOLTAGE;
        return result;
    }

    const bool hold_hard_low =
        previous_reason == NUWAX_BATTERY_LOW_REASON_HARD_VOLTAGE &&
        input->voltage_mv < input->hard_low_exit_mv;
    const bool hold_estimated_low =
        previous_reason != NUWAX_BATTERY_LOW_REASON_HARD_VOLTAGE &&
        (input->percent < input->low_exit_percent ||
         input->voltage_mv < input->estimated_low_exit_mv);
    if (previously_low && (hold_hard_low || hold_estimated_low)) {
        result.low = true;
        result.reason = hold_hard_low
                            ? NUWAX_BATTERY_LOW_REASON_HARD_VOLTAGE
                            : NUWAX_BATTERY_LOW_REASON_ESTIMATED_SOC_VOLTAGE;
    }
    return result;
}

static inline bool nuwax_battery_charging_latch_update(
    bool previously_charging,
    bool raw_charging,
    uint8_t *inactive_samples,
    uint8_t exit_samples)
{
    if (!inactive_samples || exit_samples == 0U) {
        return raw_charging;
    }
    if (raw_charging) {
        *inactive_samples = 0U;
        return true;
    }
    if (!previously_charging) {
        *inactive_samples = 0U;
        return false;
    }
    if (*inactive_samples < UINT8_MAX) {
        (*inactive_samples)++;
    }
    if (*inactive_samples >= exit_samples) {
        *inactive_samples = 0U;
        return false;
    }
    return true;
}

static inline uint8_t nuwax_battery_display_percent_update(
    bool initialized,
    bool authoritative,
    uint8_t previous,
    uint8_t raw_percent,
    uint8_t max_step)
{
    if (raw_percent > 100U) raw_percent = 100U;
    if (!initialized || authoritative || max_step == 0U) return raw_percent;
    if (raw_percent > previous) {
        const uint8_t delta = raw_percent - previous;
        return delta > max_step ? (uint8_t)(previous + max_step)
                                : raw_percent;
    }
    const uint8_t delta = previous - raw_percent;
    return delta > max_step ? (uint8_t)(previous - max_step)
                            : raw_percent;
}
