#include <stdio.h>

#include "board/battery_policy.h"

static int expect_bool(const char *name, bool actual, bool expected)
{
    if (actual == expected) {
        printf("[PASS] %s\n", name);
        return 0;
    }
    printf("[FAIL] %s actual=%d expected=%d\n",
           name, actual ? 1 : 0, expected ? 1 : 0);
    return 1;
}

static nuwax_battery_low_policy_input_t sample(void)
{
    return (nuwax_battery_low_policy_input_t){
        .battery_present = true,
        .soc_trusted = false,
        .gauge_low_alarm = false,
        .percent = 50U,
        .voltage_mv = 3800U,
        .low_enter_percent = 10U,
        .low_exit_percent = 15U,
        .hard_low_enter_mv = 3400U,
        .hard_low_exit_mv = 3550U,
        .estimated_low_enter_mv = 3600U,
        .estimated_low_exit_mv = 3700U,
    };
}

int main(void)
{
    int failures = 0;
    nuwax_battery_low_policy_input_t input = sample();

    input.percent = 8U;
    input.voltage_mv = 3800U;
    failures += expect_bool(
        "untrusted SOC alone does not assert product low",
        nuwax_battery_low_policy_update(
            false, NUWAX_BATTERY_LOW_REASON_NONE, &input).low,
        false);

    input.voltage_mv = 3580U;
    failures += expect_bool(
        "untrusted SOC plus low voltage asserts product low",
        nuwax_battery_low_policy_update(
            false, NUWAX_BATTERY_LOW_REASON_NONE, &input).low,
        true);

    input.percent = 80U;
    input.voltage_mv = 3390U;
    failures += expect_bool(
        "hard voltage floor overrides optimistic SOC",
        nuwax_battery_low_policy_update(
            false, NUWAX_BATTERY_LOW_REASON_NONE, &input).low,
        true);
    input.voltage_mv = 3540U;
    failures += expect_bool(
        "hard voltage remains latched below hard recovery",
        nuwax_battery_low_policy_update(
            true, NUWAX_BATTERY_LOW_REASON_HARD_VOLTAGE, &input).low,
        true);
    input.voltage_mv = 3560U;
    failures += expect_bool(
        "hard voltage clears at its own recovery threshold",
        nuwax_battery_low_policy_update(
            true, NUWAX_BATTERY_LOW_REASON_HARD_VOLTAGE, &input).low,
        false);

    input = sample();
    input.soc_trusted = true;
    input.percent = 10U;
    failures += expect_bool(
        "trusted low SOC asserts product low",
        nuwax_battery_low_policy_update(
            false, NUWAX_BATTERY_LOW_REASON_NONE, &input).low,
        true);

    input = sample();
    input.percent = 16U;
    input.voltage_mv = 3690U;
    failures += expect_bool(
        "estimated low remains latched below voltage recovery",
        nuwax_battery_low_policy_update(
            true, NUWAX_BATTERY_LOW_REASON_ESTIMATED_SOC_VOLTAGE,
            &input).low,
        true);
    input.voltage_mv = 3710U;
    failures += expect_bool(
        "estimated low clears after SOC and voltage recover",
        nuwax_battery_low_policy_update(
            true, NUWAX_BATTERY_LOW_REASON_ESTIMATED_SOC_VOLTAGE,
            &input).low,
        false);

    uint8_t inactive = 0U;
    bool charging = nuwax_battery_charging_latch_update(
        false, true, &inactive, 2U);
    failures += expect_bool("charging enters immediately", charging, true);
    charging = nuwax_battery_charging_latch_update(
        charging, false, &inactive, 2U);
    failures += expect_bool("single idle sample retains charging", charging, true);
    charging = nuwax_battery_charging_latch_update(
        charging, false, &inactive, 2U);
    failures += expect_bool("second idle sample exits charging", charging, false);

    failures += expect_bool(
        "display smoothing limits upward jump",
        nuwax_battery_display_percent_update(
            true, false, 50U, 70U, 2U) == 52U,
        true);
    failures += expect_bool(
        "first display sample is not delayed",
        nuwax_battery_display_percent_update(
            false, false, 0U, 76U, 2U) == 76U,
        true);
    failures += expect_bool(
        "trusted profile bypasses redundant display lag",
        nuwax_battery_display_percent_update(
            true, true, 50U, 70U, 2U) == 70U,
        true);

    if (failures != 0) {
        printf("Battery policy failures=%d\n", failures);
        return 1;
    }
    printf("Battery policy: PASS\n");
    return 0;
}
