#include <stdio.h>

#include "nuwax_ota_new/nuwax_firmware_report_policy.h"

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
    nuwax_firmware_report_policy_t policy;
    nuwax_firmware_report_policy_init(&policy);

    failures += expect_true("first authenticated connection sends",
                            nuwax_firmware_report_policy_should_send(
                                &policy, 1U, 100U));
    nuwax_firmware_report_policy_note_attempt(&policy, 1U, 100U);
    failures += expect_false("same connection does not duplicate early",
                             nuwax_firmware_report_policy_should_send(
                                 &policy, 1U, 10099U));
    failures += expect_true("same logical report retries after timeout",
                            nuwax_firmware_report_policy_should_send(
                                &policy, 1U, 10100U));
    nuwax_firmware_report_policy_note_attempt(&policy, 1U, 10100U);
    failures += expect_false("second retry uses exponential delay",
                             nuwax_firmware_report_policy_should_send(
                                 &policy, 1U, 30099U));
    failures += expect_true("second retry deadline is inclusive",
                            nuwax_firmware_report_policy_should_send(
                                &policy, 1U, 30100U));
    failures += expect_true("reconnect retransmits immediately before ACK",
                            nuwax_firmware_report_policy_should_send(
                                &policy, 2U, 10200U));
    nuwax_firmware_report_policy_note_attempt(&policy, 2U, 10200U);
    failures += expect_false("stale connection ACK is rejected",
                             nuwax_firmware_report_policy_accept_ack(
                                 &policy, 1U, true));
    failures += expect_false("non-persisted ACK is rejected",
                             nuwax_firmware_report_policy_accept_ack(
                                 &policy, 2U, false));
    failures += expect_true("matching persisted ACK completes boot report",
                            nuwax_firmware_report_policy_accept_ack(
                                &policy, 2U, true));
    failures += expect_false("ACKed report survives later reconnects",
                             nuwax_firmware_report_policy_should_send(
                                 &policy, 3U, 70000U));

    nuwax_firmware_report_policy_init(&policy);
    nuwax_firmware_report_policy_note_attempt(&policy, 9U, UINT32_MAX - 100U);
    failures += expect_true("retry deadline tolerates millisecond wrap",
                            nuwax_firmware_report_policy_should_send(
                                &policy, 9U, 9900U));

    if (failures != 0) {
        printf("firmware report policy failures=%d\n", failures);
        return 1;
    }
    printf("firmware report policy: PASS\n");
    return 0;
}
