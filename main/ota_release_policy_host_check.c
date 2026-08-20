#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>

#include "nuwax_ota_new/nuwax_ota_release_policy.h"
#include "nuwax_ota_new/nuwax_ota_result_ack_policy.h"

static int expect_u64(const char *name, uint64_t actual, uint64_t expected)
{
    if (actual == expected) {
        printf("[PASS] %s=%" PRIu64 "\n", name, actual);
        return 0;
    }
    printf("[FAIL] %s actual=%" PRIu64 " expected=%" PRIu64 "\n",
           name, actual, expected);
    return 1;
}

static int expect_bool(const char *name, bool actual, bool expected)
{
    if (actual == expected) {
        printf("[PASS] %s=%d\n", name, actual ? 1 : 0);
        return 0;
    }
    printf("[FAIL] %s actual=%d expected=%d\n", name,
           actual ? 1 : 0, expected ? 1 : 0);
    return 1;
}

int main(void)
{
    int failures = 0;
    failures += expect_u64("protocol_floor", NUWAX_OTA_BASE_RELEASE_SEQUENCE,
                           7000000ULL);
    failures += expect_u64("firmware_sequence",
                           NUWAX_OTA_FIRMWARE_RELEASE_SEQUENCE, 7000088ULL);
    failures += expect_u64("clean_nvs_floor",
                           nuwax_ota_release_floor(7000000ULL), 7000088ULL);
    failures += expect_u64("old_nvs_floor",
                           nuwax_ota_release_floor(7000009ULL), 7000088ULL);
    failures += expect_u64("newer_nvs_floor",
                           nuwax_ota_release_floor(7000089ULL), 7000089ULL);
    failures += expect_bool("clean_nvs_can_update",
                            nuwax_ota_release_can_start(7000000ULL, 7000088ULL),
                            true);
    failures += expect_bool("old_nvs_can_update",
                            nuwax_ota_release_can_start(7000009ULL, 7000088ULL),
                            true);
    failures += expect_bool("uncommitted_higher_floor_blocks",
                            nuwax_ota_release_can_start(7000009ULL, 7000089ULL),
                            false);
    failures += expect_bool("matching_pending",
                            nuwax_ota_release_pending_matches_firmware(7000088ULL),
                            true);
    failures += expect_bool("lower_pending_mismatch",
                            nuwax_ota_release_pending_matches_firmware(7000012ULL),
                            false);
    failures += expect_bool("higher_pending_mismatch",
                            nuwax_ota_release_pending_matches_firmware(7000089ULL),
                            false);
    failures += expect_bool("pending_newer_than_old",
                            nuwax_ota_release_pending_is_newer(7000013ULL,
                                                               7000012ULL),
                            true);
    failures += expect_bool("pending_not_newer_than_equal",
                            nuwax_ota_release_pending_is_newer(7000013ULL,
                                                               7000013ULL),
                            false);
    failures += expect_bool("same_release_reinstall_allowed",
                            nuwax_ota_release_running_meets_floor(7000088ULL,
                                                                 7000088ULL),
                            true);
    failures += expect_bool("newer_release_allowed",
                            nuwax_ota_release_running_meets_floor(7000089ULL,
                                                                 7000088ULL),
                            true);
    failures += expect_bool("lower_release_rejected",
                            nuwax_ota_release_running_meets_floor(7000087ULL,
                                                                 7000088ULL),
                            false);
    failures += expect_bool("recover_missing_sequence",
                            nuwax_ota_release_pending_may_recover(7000013ULL,
                                                                  7000012ULL),
                            true);
    failures += expect_bool("recover_missing_identity",
                            nuwax_ota_release_pending_may_recover(7000013ULL,
                                                                  7000013ULL),
                            true);
    failures += expect_bool("stale_recovery_rejected",
                            nuwax_ota_release_pending_may_recover(7000013ULL,
                                                                  7000015ULL),
                            false);
    failures += expect_u64("transport_does_not_decrease",
                           nuwax_ota_release_advance(7000088ULL, 7000013ULL),
                           7000088ULL);
    failures += expect_u64("transport_advances",
                           nuwax_ota_release_advance(7000013ULL, 7000088ULL),
                           7000088ULL);
    failures += expect_bool(
        "result_ack_success_strict",
        nuwax_ota_result_ack_policy_decide(true, true) ==
            NUWAX_OTA_RESULT_ACK_STRICT_ECHO,
        true);
    failures += expect_bool(
        "result_ack_failure_strict",
        nuwax_ota_result_ack_policy_decide(false, false) ==
            NUWAX_OTA_RESULT_ACK_STRICT_ECHO,
        true);
    failures += expect_bool(
        "result_ack_failure_persisted_compat",
        nuwax_ota_result_ack_policy_decide(false, true) ==
            NUWAX_OTA_RESULT_ACK_PERSISTED_SUCCESS_COMPAT,
        true);
    failures += expect_bool(
        "result_ack_success_negative_rejected",
        nuwax_ota_result_ack_policy_decide(true, false) ==
            NUWAX_OTA_RESULT_ACK_REJECT,
        true);
    return failures == 0 ? 0 : 1;
}
