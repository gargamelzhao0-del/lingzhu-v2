#include <stdio.h>

#include "nuwax_ota_new/nuwax_ota_power_policy.h"

static int expect_result(const char *name,
                         nuwax_ota_power_result_t actual,
                         nuwax_ota_power_result_t expected)
{
    if (actual == expected) {
        printf("[PASS] %s\n", name);
        return 0;
    }
    printf("[FAIL] %s actual=%d expected=%d\n", name, actual, expected);
    return 1;
}

static nuwax_ota_power_result_t battery(uint8_t percent)
{
    return nuwax_ota_power_evaluate(true, false, true, false, true, false,
                                    percent, 30U);
}

int main(void)
{
    int failures = 0;
    failures += expect_result("29 percent rejected", battery(29U),
                              NUWAX_OTA_POWER_LOW);
    failures += expect_result("30 percent accepted", battery(30U),
                              NUWAX_OTA_POWER_ALLOWED);
    failures += expect_result("31 percent accepted", battery(31U),
                              NUWAX_OTA_POWER_ALLOWED);
    failures += expect_result(
        "stale sample rejected",
        nuwax_ota_power_evaluate(true, true, true, false, true, false, 80U, 30U),
        NUWAX_OTA_POWER_UNKNOWN);
    failures += expect_result(
        "invalid sample rejected",
        nuwax_ota_power_evaluate(true, false, true, false, false, false, 80U, 30U),
        NUWAX_OTA_POWER_UNKNOWN);
    failures += expect_result(
        "verified external power accepted",
        nuwax_ota_power_evaluate(true, false, false, true, false, false, 0U, 30U),
        NUWAX_OTA_POWER_ALLOWED);
    failures += expect_result(
        "missing battery without external power rejected",
        nuwax_ota_power_evaluate(true, false, false, false, false, false, 0U, 30U),
        NUWAX_OTA_POWER_UNKNOWN);
    failures += expect_result(
        "hard-low safety state overrides optimistic SOC",
        nuwax_ota_power_evaluate(true, false, true, false, true, true, 80U, 30U),
        NUWAX_OTA_POWER_LOW);
    if (failures != 0) {
        printf("OTA power policy failures=%d\n", failures);
        return 1;
    }
    printf("OTA power policy: PASS\n");
    return 0;
}
