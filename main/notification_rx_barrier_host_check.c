/* Host-only state check. This file is excluded from firmware CMake sources. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "conversation/notification_rx_barrier.h"

#define EXPECT_TRUE(name, expression)                                      \
    do {                                                                   \
        const bool passed = (expression);                                  \
        printf("[%s] %s\n", passed ? "PASS" : "FAIL", (name));          \
        failures += passed ? 0 : 1;                                        \
    } while (0)

int main(void) {
    int failures = 0;
    nuwax_notification_rx_barrier_t barrier;
    nuwax_notification_rx_barrier_init(&barrier);

    EXPECT_TRUE("initial barrier is idle",
                nuwax_notification_rx_barrier_state_for_epoch(&barrier, 7U) ==
                    NUWAX_NOTIFICATION_RX_IDLE);
    EXPECT_TRUE("normal begin from idle arms an ordered pending barrier",
                nuwax_notification_rx_barrier_arm_normal(
                    &barrier,
                    7U,
                    50U,
                    3000U,
                    "turn_direct",
                    "pb_direct") &&
                    nuwax_notification_rx_barrier_state_for_epoch(
                        &barrier, 7U) ==
                        NUWAX_NOTIFICATION_RX_NORMAL_PENDING &&
                    !nuwax_notification_rx_barrier_admit_normal(
                        &barrier, 7U, "turn_other", "pb_direct") &&
                    nuwax_notification_rx_barrier_admit_normal(
                        &barrier, 7U, "turn_direct", "pb_direct") &&
                    nuwax_notification_rx_barrier_state_for_epoch(
                        &barrier, 7U) == NUWAX_NOTIFICATION_RX_IDLE);
    EXPECT_TRUE("notification safely takes over normal pending before binary",
                nuwax_notification_rx_barrier_arm_normal(
                    &barrier,
                    7U,
                    60U,
                    3000U,
                    "turn_superseded",
                    "pb_superseded") &&
                    nuwax_notification_rx_barrier_arm(
                        &barrier,
                        7U,
                        70U,
                        3000U,
                        "ntf_selected",
                        "pb_selected") &&
                    nuwax_notification_rx_barrier_state_for_epoch(
                        &barrier, 7U) == NUWAX_NOTIFICATION_RX_PENDING &&
                    barrier.owner ==
                        NUWAX_NOTIFICATION_RX_OWNER_NOTIFICATION &&
                    !nuwax_notification_rx_barrier_admit_normal(
                        &barrier,
                        7U,
                        "turn_superseded",
                        "pb_superseded") &&
                    nuwax_notification_rx_barrier_admit(
                        &barrier,
                        7U,
                        "ntf_selected",
                        "pb_selected"));
    nuwax_notification_rx_barrier_reset(&barrier);
    EXPECT_TRUE("begin arms epoch-bound pending barrier",
                nuwax_notification_rx_barrier_arm(
                    &barrier,
                    7U,
                    100U,
                    3000U,
                    "ntf_1",
                    "pb_1") &&
                    nuwax_notification_rx_barrier_state_for_epoch(
                        &barrier, 7U) == NUWAX_NOTIFICATION_RX_PENDING &&
                    !nuwax_notification_rx_barrier_deadline_expired(
                        &barrier, 3099U) &&
                    nuwax_notification_rx_barrier_deadline_expired(
                        &barrier, 3100U));
    EXPECT_TRUE("different epoch cannot admit pending notification",
                !nuwax_notification_rx_barrier_admit(
                    &barrier, 8U, "ntf_1", "pb_1") &&
                    nuwax_notification_rx_barrier_admit(
                        &barrier, 7U, "ntf_1", "pb_1"));
    EXPECT_TRUE("duplicate begin returns to pending validation",
                nuwax_notification_rx_barrier_arm(
                    &barrier,
                    7U,
                    200U,
                    3000U,
                    "ntf_1",
                    "pb_1") &&
                    nuwax_notification_rx_barrier_state_for_epoch(
                        &barrier, 7U) == NUWAX_NOTIFICATION_RX_PENDING);
    EXPECT_TRUE("conflicting begin cannot reuse pending barrier",
                !nuwax_notification_rx_barrier_arm(
                    &barrier,
                    7U,
                    210U,
                    3000U,
                    "ntf_2",
                    "pb_2") &&
                    nuwax_notification_rx_barrier_state_for_epoch(
                        &barrier, 7U) == NUWAX_NOTIFICATION_RX_PENDING);
    EXPECT_TRUE("rejection remains epoch-bound and requests recovery once",
                !nuwax_notification_rx_barrier_reject(
                    &barrier, 8U, 300U, 10000U) &&
                    nuwax_notification_rx_barrier_reject(
                        &barrier, 7U, 300U, 10000U) &&
                    nuwax_notification_rx_barrier_state_for_epoch(
                        &barrier, 7U) == NUWAX_NOTIFICATION_RX_REJECTED &&
                    !nuwax_notification_rx_barrier_deadline_expired(
                        &barrier, 10299U) &&
                    nuwax_notification_rx_barrier_deadline_expired(
                        &barrier, 10300U) &&
                    !nuwax_notification_rx_barrier_take_rejected_timeout(
                        &barrier, 8U, 10300U) &&
                    nuwax_notification_rx_barrier_take_rejected_timeout(
                        &barrier, 7U, 10300U) &&
                    !nuwax_notification_rx_barrier_take_rejected_timeout(
                        &barrier, 7U, 10301U) &&
                    nuwax_notification_rx_barrier_state_for_epoch(
                        &barrier, 7U) == NUWAX_NOTIFICATION_RX_REJECTED);
    EXPECT_TRUE("rejected stream cannot be overwritten by notification begin",
                !nuwax_notification_rx_barrier_arm(
                    &barrier,
                    7U,
                    400U,
                    3000U,
                    "ntf_2",
                    "pb_2") &&
                    nuwax_notification_rx_barrier_state_for_epoch(
                        &barrier, 7U) == NUWAX_NOTIFICATION_RX_REJECTED);
    EXPECT_TRUE("rejected stream cannot hand off to normal playback",
                !nuwax_notification_rx_barrier_arm_normal(
                        &barrier,
                        7U,
                        510U,
                        3000U,
                        "turn_1",
                        "pb_normal_1") &&
                    nuwax_notification_rx_barrier_state_for_epoch(
                        &barrier, 7U) == NUWAX_NOTIFICATION_RX_REJECTED);
    EXPECT_TRUE("binary END release permits the next notification stream",
                nuwax_notification_rx_barrier_reset_if_epoch(&barrier, 7U) &&
                    nuwax_notification_rx_barrier_arm(
                    &barrier,
                    7U,
                    600U,
                    3000U,
                    "ntf_4",
                    "pb_4") &&
                    nuwax_notification_rx_barrier_state_for_epoch(
                        &barrier, 7U) == NUWAX_NOTIFICATION_RX_PENDING);
    EXPECT_TRUE("stale epoch cannot reset current barrier",
                !nuwax_notification_rx_barrier_reset_if_epoch(&barrier, 8U) &&
                    nuwax_notification_rx_barrier_reset_if_epoch(
                        &barrier, 7U) &&
                    nuwax_notification_rx_barrier_state_for_epoch(
                        &barrier, 7U) == NUWAX_NOTIFICATION_RX_IDLE);

    printf("notification RX barrier host check: %s (%d failures)\n",
           failures == 0 ? "PASS" : "FAIL",
           failures);
    return failures == 0 ? 0 : 1;
}
