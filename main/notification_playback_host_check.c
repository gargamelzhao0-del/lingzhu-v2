/* Host-only logic check. This file is intentionally excluded from firmware CMake sources. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "conversation/notification_playback.h"

#define EXPECT_TRUE(name, expression)                                           \
    do {                                                                        \
        const bool passed = (expression);                                       \
        printf("[%s] %s\n", passed ? "PASS" : "FAIL", (name));               \
        failures += passed ? 0 : 1;                                             \
    } while (0)

int main(void) {
    int failures = 0;
    nuwax_notification_transaction_t transaction;
    nuwax_notification_init(&transaction);

    EXPECT_TRUE("initial state is idle",
                transaction.state == NUWAX_NOTIFICATION_STATE_IDLE &&
                    !nuwax_notification_blocks_conversation(&transaction));

    EXPECT_TRUE("sleep notification requires prepare",
                nuwax_notification_request_begin(
                    &transaction, 100U, 2000U, 7U, 0U, "req_1", "ntf_1",
                    "pb_1", true) ==
                    NUWAX_NOTIFICATION_REQUEST_NOT_PREPARED);

    EXPECT_TRUE("sleep prepare binds epoch and previous speaker",
                nuwax_notification_request_prepare(
                    &transaction, 100U, 2000U, 7U, 21U, "cmd_1", "ntf_1",
                    true) == NUWAX_NOTIFICATION_REQUEST_STARTED &&
                    transaction.active && transaction.token != 0U &&
                    transaction.origin_sleeping &&
                    transaction.ws_epoch == 7U &&
                    transaction.previous_speaker_generation == 21U);
    const uint32_t prepare_token = transaction.token;
    EXPECT_TRUE("prepare duplicate is idempotent",
                nuwax_notification_request_prepare(
                    &transaction, 110U, 2000U, 7U, 21U, "cmd_1", "ntf_1",
                    true) == NUWAX_NOTIFICATION_REQUEST_DUPLICATE);
    EXPECT_TRUE("begin cannot overtake prepare ACK",
                nuwax_notification_request_begin(
                    &transaction, 120U, 2000U, 7U, 21U, "req_1", "ntf_1",
                    "pb_1", true) == NUWAX_NOTIFICATION_REQUEST_NOT_READY);
    EXPECT_TRUE("stale completion cannot satisfy prepare",
                !nuwax_notification_mark_previous_speaker_stopped(
                    &transaction, prepare_token, 8U, 21U) &&
                    !nuwax_notification_mark_previous_speaker_stopped(
                        &transaction, prepare_token, 7U, 22U));
    EXPECT_TRUE("all local owners must release before prepare ACK",
                nuwax_notification_mark_capture_released(&transaction,
                                                         prepare_token) &&
                    nuwax_notification_mark_wake_detector_released(
                        &transaction, prepare_token) &&
                    !nuwax_notification_prepare_ready(&transaction) &&
                     nuwax_notification_mark_previous_speaker_stopped(
                         &transaction, prepare_token, 7U, 21U) &&
                     nuwax_notification_mark_prepare_ready(&transaction,
                                                           prepare_token));
    EXPECT_TRUE("stale prepare ACK cannot refresh READY deadline",
                !nuwax_notification_mark_prepare_ack_sent(
                    &transaction, prepare_token + 1U, 7U, 1900U, 2000U) &&
                    !nuwax_notification_mark_prepare_ack_sent(
                        &transaction, prepare_token, 8U, 1900U, 2000U) &&
                    transaction.deadline_ms == 2100U);
    EXPECT_TRUE("prepare ACK starts a full post-ACK begin window",
                nuwax_notification_mark_prepare_ack_sent(
                    &transaction,
                    prepare_token,
                    7U,
                    1900U,
                    2000U) &&
                    transaction.deadline_ms == 3900U &&
                    !nuwax_notification_deadline_expired(&transaction,
                                                         3899U) &&
                    nuwax_notification_deadline_expired(&transaction,
                                                        3900U));
    EXPECT_TRUE("prepared notification accepts matching begin",
                nuwax_notification_request_begin(
                    &transaction, 2000U, 2000U, 7U, 0U, "req_1", "ntf_1",
                    "pb_1", true) == NUWAX_NOTIFICATION_REQUEST_STARTED &&
                    nuwax_notification_begin_ready(&transaction) &&
                    nuwax_notification_mark_streaming(&transaction,
                                                      prepare_token,
                                                      7U) &&
                    nuwax_notification_mark_begin_ack_sent(
                        &transaction, prepare_token, 7U));
    EXPECT_TRUE("begin duplicate does not create a new generation",
                nuwax_notification_request_begin(
                    &transaction, 2010U, 2000U, 7U, 0U, "req_1", "ntf_1",
                    "pb_1", true) == NUWAX_NOTIFICATION_REQUEST_DUPLICATE);
    EXPECT_TRUE("stream deadline refresh is epoch and token bound",
                !nuwax_notification_refresh_stream_deadline(
                    &transaction, prepare_token, 8U, 250U, 5000U) &&
                    !nuwax_notification_refresh_stream_deadline(
                        &transaction, prepare_token + 1U, 7U, 250U, 5000U) &&
                    nuwax_notification_refresh_stream_deadline(
                        &transaction, prepare_token, 7U, 250U, 5000U) &&
                    transaction.deadline_ms == 5250U);
    EXPECT_TRUE("speaker generation binding is immutable",
                nuwax_notification_bind_speaker_generation(
                    &transaction, prepare_token, 7U, 31U) &&
                    !nuwax_notification_bind_speaker_generation(
                        &transaction, prepare_token, 7U, 32U));
    EXPECT_TRUE("mismatched logical end is rejected",
                nuwax_notification_request_end(
                    &transaction, 300U, 10000U, 7U, "end_bad", "ntf_2",
                    "pb_1") == NUWAX_NOTIFICATION_REQUEST_CONFLICT);
    EXPECT_TRUE("matching logical end enters draining",
                nuwax_notification_request_end(
                    &transaction, 300U, 10000U, 7U, "end_1", "ntf_1",
                    "pb_1") == NUWAX_NOTIFICATION_REQUEST_STARTED);
    EXPECT_TRUE("logical end alone cannot complete",
                !nuwax_notification_ready_to_complete(&transaction));
    EXPECT_TRUE("binary and physical terminals are generation bound",
                !nuwax_notification_mark_binary_end(
                    &transaction, prepare_token, 7U, 32U) &&
                    nuwax_notification_mark_binary_end(
                        &transaction, prepare_token, 7U, 31U) &&
                    !nuwax_notification_ready_to_complete(&transaction) &&
                    nuwax_notification_mark_physical_done(
                        &transaction, prepare_token, 7U, 31U) &&
                    nuwax_notification_ready_to_complete(&transaction));
    EXPECT_TRUE("end ACK precedes committed completion",
                nuwax_notification_mark_end_ack_sent(
                    &transaction, prepare_token, 7U) &&
                    nuwax_notification_complete(&transaction,
                                                prepare_token) &&
                    transaction.state == NUWAX_NOTIFICATION_STATE_COMPLETE &&
                    !transaction.active);

    nuwax_notification_transaction_t awake_prepare;
    nuwax_notification_init(&awake_prepare);
    EXPECT_TRUE("awake no-turn prepare records a listening origin",
                nuwax_notification_request_prepare(
                    &awake_prepare,
                    900U,
                    2000U,
                    8U,
                    0U,
                    "cmd_awake",
                    "ntf_awake",
                    false) == NUWAX_NOTIFICATION_REQUEST_STARTED &&
                    awake_prepare.active &&
                    !awake_prepare.origin_sleeping);

    EXPECT_TRUE("conversation begin can start without prepare",
                nuwax_notification_request_begin(
                    &transaction, 1000U, 2000U, 9U, 41U, "req_2", "ntf_2",
                    "pb_2", false) == NUWAX_NOTIFICATION_REQUEST_STARTED);
    const uint32_t conversation_token = transaction.token;
    EXPECT_TRUE("conversation begin waits for old playback and MIC",
                nuwax_notification_mark_wake_detector_released(
                    &transaction, conversation_token) &&
                    !nuwax_notification_begin_ready(&transaction) &&
                    nuwax_notification_mark_capture_released(
                        &transaction, conversation_token) &&
                    !nuwax_notification_begin_ready(&transaction) &&
                    nuwax_notification_mark_previous_speaker_stopped(
                        &transaction, conversation_token, 9U, 41U) &&
                    nuwax_notification_begin_ready(&transaction));
    EXPECT_TRUE("cancel before notification START needs no speaker stop",
                nuwax_notification_request_cancel(
                    &transaction,
                    conversation_token,
                    9U,
                    1100U,
                    2000U) &&
                    transaction.physical_done &&
                    nuwax_notification_cancel_ready(&transaction) &&
                    nuwax_notification_mark_cancel_notified(
                        &transaction, conversation_token, 9U) &&
                    nuwax_notification_complete(&transaction,
                                                conversation_token));

    EXPECT_TRUE("deadline comparison is wrap safe",
                nuwax_notification_request_begin(
                    &transaction,
                    UINT32_MAX - 100U,
                    200U,
                    11U,
                    0U,
                    "req_wrap",
                    "ntf_wrap",
                    "pb_wrap",
                    false) == NUWAX_NOTIFICATION_REQUEST_STARTED &&
                    !nuwax_notification_deadline_expired(&transaction, 50U) &&
                    nuwax_notification_deadline_expired(&transaction, 99U));

    nuwax_notification_reset(&transaction);
    EXPECT_TRUE("binary END may arrive before logical END",
                nuwax_notification_request_begin(
                    &transaction, 2000U, 2000U, 13U, 0U, "req_3", "ntf_3",
                    "pb_3", false) == NUWAX_NOTIFICATION_REQUEST_STARTED);
    const uint32_t binary_first_token = transaction.token;
    EXPECT_TRUE("binary-first stream reaches terminal join only after JSON END",
                nuwax_notification_mark_capture_released(
                    &transaction, binary_first_token) &&
                    nuwax_notification_mark_wake_detector_released(
                        &transaction, binary_first_token) &&
                    nuwax_notification_mark_streaming(
                        &transaction, binary_first_token, 13U) &&
                    nuwax_notification_mark_begin_ack_sent(
                        &transaction, binary_first_token, 13U) &&
                    nuwax_notification_bind_speaker_generation(
                        &transaction, binary_first_token, 13U, 51U) &&
                    nuwax_notification_mark_binary_end(
                        &transaction, binary_first_token, 13U, 51U) &&
                    nuwax_notification_mark_physical_done(
                        &transaction, binary_first_token, 13U, 51U) &&
                    !nuwax_notification_ready_to_complete(&transaction) &&
                    nuwax_notification_request_end(
                        &transaction, 2100U, 10000U, 13U, "end_3", "ntf_3",
                        "pb_3") == NUWAX_NOTIFICATION_REQUEST_STARTED &&
                    nuwax_notification_ready_to_complete(&transaction));
    EXPECT_TRUE("stale WebSocket epoch cannot complete binary-first stream",
                !nuwax_notification_mark_end_ack_sent(
                    &transaction, binary_first_token, 14U) &&
                    nuwax_notification_mark_end_ack_sent(
                        &transaction, binary_first_token, 13U) &&
                    nuwax_notification_complete(&transaction,
                                                binary_first_token));

    EXPECT_TRUE("active notification cancellation waits for physical stop",
                nuwax_notification_request_begin(
                    &transaction, 3000U, 2000U, 15U, 0U, "req_4", "ntf_4",
                    "pb_4", false) == NUWAX_NOTIFICATION_REQUEST_STARTED);
    const uint32_t active_cancel_token = transaction.token;
    EXPECT_TRUE("generation-bound cancellation cannot accept a stale stop",
                nuwax_notification_mark_capture_released(
                    &transaction, active_cancel_token) &&
                    nuwax_notification_mark_wake_detector_released(
                        &transaction, active_cancel_token) &&
                    nuwax_notification_mark_streaming(
                        &transaction, active_cancel_token, 15U) &&
                    nuwax_notification_bind_speaker_generation(
                        &transaction, active_cancel_token, 15U, 61U) &&
                    nuwax_notification_request_cancel(
                        &transaction,
                        active_cancel_token,
                        15U,
                        3100U,
                        2000U) &&
                    !nuwax_notification_cancel_ready(&transaction) &&
                    !nuwax_notification_mark_physical_done(
                        &transaction, active_cancel_token, 15U, 62U) &&
                    nuwax_notification_mark_physical_done(
                        &transaction, active_cancel_token, 15U, 61U) &&
                    nuwax_notification_cancel_ready(&transaction));
    nuwax_notification_reset(&transaction);
    EXPECT_TRUE("reset invalidates delayed transaction completions",
                !nuwax_notification_mark_physical_done(
                    &transaction, active_cancel_token, 15U, 61U));

    printf("notification playback host check: %s (%d failures)\n",
           failures == 0 ? "PASS" : "FAIL",
           failures);
    return failures == 0 ? 0 : 1;
}
