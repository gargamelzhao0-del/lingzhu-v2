/* Audit probe: invokes production policy headers, without changing firmware. */
#include <stdio.h>
#include "board/battery_policy.h"
#include "nuwax_ota_new/nuwax_ota_power_policy.h"

int main(void)
{
    const nuwax_battery_low_policy_input_t sample = {
        .battery_present = true,
        .soc_trusted = false,
        .gauge_low_alarm = false,
        .percent = 60,
        .voltage_mv = 3650,
        .low_enter_percent = 10,
        .low_exit_percent = 15,
        .hard_low_enter_mv = 3400,
        .hard_low_exit_mv = 3550,
        .estimated_low_enter_mv = 3600,
        .estimated_low_exit_mv = 3700,
    };
    const nuwax_battery_low_policy_result_t low =
        nuwax_battery_low_policy_update(false, NUWAX_BATTERY_LOW_REASON_NONE,
                                       &sample);
    /* Battery service permits valid=true with profile_verified=false.
     * Both OTA callers pass valid/low/percent but omit soc_trusted. */
    const nuwax_ota_power_result_t ota = nuwax_ota_power_evaluate(
        true, false, sample.battery_present, false, true, low.low,
        sample.percent, 30);
    printf("production policy observation: soc_trusted=0 percent=60 voltage=3650 low=%d ota_allowed=%d\n",
           low.low, ota == NUWAX_OTA_POWER_ALLOWED);
    puts("This reproduces policy admission, not physical battery capacity or an OTA power failure.");
    return (!low.low && ota == NUWAX_OTA_POWER_ALLOWED) ? 0 : 1;
}
