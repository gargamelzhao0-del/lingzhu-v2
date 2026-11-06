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
    return nuwax_ota_power_evaluate_admission(true, false, true, true, false,
                                            percent, 5U);
}

int main(void)
{
    int failures = 0;
    failures += expect_result("0 percent rejected", battery(0U),
                              NUWAX_OTA_POWER_LOW);
    failures += expect_result("4 percent rejected", battery(4U),
                              NUWAX_OTA_POWER_LOW);
    failures += expect_result("5 percent accepted", battery(5U),
                              NUWAX_OTA_POWER_ALLOWED);
    failures += expect_result("10 percent accepted", battery(10U),
                              NUWAX_OTA_POWER_ALLOWED);
    failures += expect_result("100 percent accepted", battery(100U),
                              NUWAX_OTA_POWER_ALLOWED);
    failures += expect_result("out of range rejected", battery(101U),
                              NUWAX_OTA_POWER_UNKNOWN);
    failures += expect_result(
        "stale sample rejected",
        nuwax_ota_power_evaluate_admission(true, true, true, true, false, 80U, 5U),
        NUWAX_OTA_POWER_UNKNOWN);
    failures += expect_result(
        "invalid sample rejected",
        nuwax_ota_power_evaluate_admission(true, false, true, false, false, 80U, 5U),
        NUWAX_OTA_POWER_UNKNOWN);
    failures += expect_result(
        "unavailable sample rejected",
        nuwax_ota_power_evaluate_admission(false, false, true, true, false, 80U, 5U),
        NUWAX_OTA_POWER_UNKNOWN);
    failures += expect_result(
        "missing battery without external power rejected",
        nuwax_ota_power_evaluate_admission(true, false, false, true, false, 80U, 5U),
        NUWAX_OTA_POWER_UNKNOWN);
    failures += expect_result(
        "hard-low safety state overrides optimistic SOC",
        nuwax_ota_power_evaluate_admission(true, false, true, true, true, 80U, 5U),
        NUWAX_OTA_POWER_LOW);
    if (failures != 0) {
        printf("OTA power policy failures=%d\n", failures);
        return 1;
    }
    printf("OTA power policy: PASS\n");
    return 0;
}
