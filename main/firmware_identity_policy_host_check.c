#include <stdio.h>

#include "nuwax_ota_new/nuwax_firmware_identity_policy.h"

static int expect_true(const char *name, bool value)
{
    if (value) {
        printf("[PASS] %s\n", name);
        return 0;
    }
    printf("[FAIL] %s\n", name);
    return 1;
}

static int expect_false(const char *name, bool value)
{
    return expect_true(name, !value);
}

int main(void)
{
    int failures = 0;
    nuwax_firmware_identity_policy_t policy;
    nuwax_firmware_identity_policy_init(&policy);

    failures += expect_true(
        "empty identity accepts one request",
        nuwax_firmware_identity_policy_request(&policy, 100U));
    failures += expect_false(
        "duplicate request is coalesced",
        nuwax_firmware_identity_policy_request(&policy, 101U));
    failures += expect_true(
        "worker claims requested transaction",
        nuwax_firmware_identity_policy_begin(&policy));
    failures += expect_false(
        "running transaction cannot be claimed twice",
        nuwax_firmware_identity_policy_begin(&policy));

    nuwax_firmware_identity_policy_complete(&policy, false, 1000U);
    failures += expect_false(
        "failure observes first retry backoff",
        nuwax_firmware_identity_policy_request(&policy, 5999U));
    failures += expect_true(
        "first retry deadline is inclusive",
        nuwax_firmware_identity_policy_request(&policy, 6000U));
    failures += expect_true(
        "retry transaction starts",
        nuwax_firmware_identity_policy_begin(&policy));
    nuwax_firmware_identity_policy_complete(&policy, false, 7000U);
    failures += expect_false(
        "second failure doubles retry delay",
        nuwax_firmware_identity_policy_request(&policy, 16999U));
    failures += expect_true(
        "second retry deadline is inclusive",
        nuwax_firmware_identity_policy_request(&policy, 17000U));
    failures += expect_true(
        "second retry transaction starts",
        nuwax_firmware_identity_policy_begin(&policy));
    nuwax_firmware_identity_policy_complete(&policy, true, 18000U);
    failures += expect_false(
        "ready identity never schedules another hash",
        nuwax_firmware_identity_policy_request(&policy, UINT32_MAX));

    nuwax_firmware_identity_policy_init(&policy);
    failures += expect_true(
        "wrap test starts initial transaction",
        nuwax_firmware_identity_policy_request(&policy, UINT32_MAX - 1000U));
    failures += expect_true(
        "wrap test worker starts",
        nuwax_firmware_identity_policy_begin(&policy));
    nuwax_firmware_identity_policy_complete(&policy, false,
                                             UINT32_MAX - 1000U);
    failures += expect_false(
        "retry deadline remains pending across wrap",
        nuwax_firmware_identity_policy_request(&policy, 3998U));
    failures += expect_true(
        "retry deadline works across wrap",
        nuwax_firmware_identity_policy_request(&policy, 3999U));

    failures += expect_true(
        "retry delay is capped",
        nuwax_firmware_identity_retry_delay_ms(UINT32_MAX) ==
            NUWAX_FIRMWARE_IDENTITY_RETRY_MAX_MS);

    if (failures != 0) {
        printf("firmware identity policy failures=%d\n", failures);
        return 1;
    }
    printf("firmware identity policy: PASS\n");
    return 0;
}
