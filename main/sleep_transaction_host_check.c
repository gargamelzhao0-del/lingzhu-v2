/* Host-only logic check. This file is intentionally excluded from firmware CMake sources. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "conversation/sleep_transaction.h"

#define EXPECT_TRUE(name, expression)                                           \
    do {                                                                        \
        const bool passed = (expression);                                       \
        printf("[%s] %s\n", passed ? "PASS" : "FAIL", (name));               \
        failures += passed ? 0 : 1;                                             \
    } while (0)

int main(void) {
    int failures = 0;
    nuwax_sleep_transaction_t transaction;
    nuwax_sleep_transaction_init(&transaction);

    EXPECT_TRUE("initially idle",
                transaction.state == NUWAX_SLEEP_TRANSACTION_IDLE &&
                    !nuwax_sleep_transaction_blocks_audio(&transaction));

    const nuwax_sleep_request_result_t started =
        nuwax_sleep_transaction_request(&transaction,
                                        1000U,
                                        2000U,
                                        7U,
                                        21U,
                                        "cmd_sleep_1",
                                        "sleep_conversation",
                                        "turn_1",
                                        false);
    const uint32_t token = transaction.token;
    EXPECT_TRUE("request binds transaction",
                started == NUWAX_SLEEP_REQUEST_STARTED && token != 0U &&
                    transaction.ws_epoch == 7U &&
                    transaction.speaker_generation == 21U &&
                    transaction.deadline_ms == 3000U &&
                    nuwax_sleep_transaction_blocks_audio(&transaction));
    EXPECT_TRUE("same request is idempotent while active",
                nuwax_sleep_transaction_request(&transaction,
                                                1100U,
                                                2000U,
                                                7U,
                                                21U,
                                                "cmd_sleep_1",
                                                "sleep_conversation",
                                                "turn_1",
                                                false) ==
                    NUWAX_SLEEP_REQUEST_DUPLICATE_ACTIVE);
    EXPECT_TRUE("different request cannot replace active transaction",
                nuwax_sleep_transaction_request(&transaction,
                                                1100U,
                                                2000U,
                                                7U,
                                                21U,
                                                "cmd_sleep_2",
                                                "sleep_conversation",
                                                "turn_2",
                                                false) ==
                    NUWAX_SLEEP_REQUEST_BUSY);
    EXPECT_TRUE("quiescing begins once",
                nuwax_sleep_transaction_begin_quiescing(&transaction, token));
    EXPECT_TRUE("wrong token cannot release capture",
                !nuwax_sleep_transaction_mark_capture_released(
                    &transaction, token + 1U));
    EXPECT_TRUE("capture release is token bound",
                nuwax_sleep_transaction_mark_capture_released(&transaction,
                                                               token));
    EXPECT_TRUE("wrong epoch cannot close protocol",
                !nuwax_sleep_transaction_mark_protocol_closed(
                    &transaction, token, 8U));
    EXPECT_TRUE("protocol close is epoch bound",
                nuwax_sleep_transaction_mark_protocol_closed(&transaction,
                                                              token,
                                                              7U));
    EXPECT_TRUE("wrong speaker generation cannot complete stop",
                !nuwax_sleep_transaction_mark_speaker_stopped(
                    &transaction, token, 7U, 22U));
    EXPECT_TRUE("wrong speaker epoch cannot complete stop",
                !nuwax_sleep_transaction_mark_speaker_stopped(
                    &transaction, token, 8U, 21U));
    EXPECT_TRUE("two conditions do not permit commit",
                !nuwax_sleep_transaction_ready_to_commit(&transaction));
    EXPECT_TRUE("matching speaker completion permits commit",
                nuwax_sleep_transaction_mark_speaker_stopped(
                    &transaction, token, 7U, 21U) &&
                    nuwax_sleep_transaction_ready_to_commit(&transaction));
    EXPECT_TRUE("commit transitions through committing",
                nuwax_sleep_transaction_begin_committing(&transaction, token) &&
                    nuwax_sleep_transaction_commit_sleeping(&transaction,
                                                            token));
    EXPECT_TRUE("committed sleep remains an audio barrier",
                transaction.state == NUWAX_SLEEP_TRANSACTION_SLEEPING &&
                    !transaction.active &&
                    nuwax_sleep_transaction_blocks_audio(&transaction));
    EXPECT_TRUE("committed sleep detects local state drift",
                nuwax_sleep_transaction_requires_local_sleep_reconcile(
                    &transaction, false));
    EXPECT_TRUE("committed sleep accepts matching local state",
                !nuwax_sleep_transaction_requires_local_sleep_reconcile(
                    &transaction, true));
    EXPECT_TRUE("same committed request is idempotent",
                nuwax_sleep_transaction_request(&transaction,
                                                3100U,
                                                2000U,
                                                7U,
                                                0U,
                                                "cmd_sleep_1",
                                                "sleep_conversation",
                                                "turn_1",
                                                true) ==
                    NUWAX_SLEEP_REQUEST_DUPLICATE_COMMITTED);
    EXPECT_TRUE("new request while locally sleeping is already satisfied",
                nuwax_sleep_transaction_request(&transaction,
                                                3100U,
                                                2000U,
                                                7U,
                                                0U,
                                                "cmd_sleep_2",
                                                "sleep_conversation",
                                                "turn_2",
                                                true) ==
                    NUWAX_SLEEP_REQUEST_ALREADY_SLEEPING);
    EXPECT_TRUE("ACK binding rejects another epoch",
                !nuwax_sleep_transaction_mark_ack_sent(&transaction,
                                                       token,
                                                       8U) &&
                    nuwax_sleep_transaction_mark_ack_sent(&transaction,
                                                          token,
                                                          7U));

    nuwax_sleep_transaction_reset_for_wake(&transaction);
    EXPECT_TRUE("wake reset invalidates old token",
                transaction.state == NUWAX_SLEEP_TRANSACTION_IDLE &&
                    transaction.token != token &&
                    !nuwax_sleep_transaction_mark_capture_released(
                        &transaction, token));

    EXPECT_TRUE("deadline uses wrap-safe comparison",
                nuwax_sleep_transaction_request(&transaction,
                                                UINT32_MAX - 100U,
                                                200U,
                                                9U,
                                                0U,
                                                "cmd_wrap",
                                                "sleep_conversation",
                                                "",
                                                false) ==
                    NUWAX_SLEEP_REQUEST_STARTED &&
                    !nuwax_sleep_transaction_deadline_expired(&transaction,
                                                              50U) &&
                    nuwax_sleep_transaction_deadline_expired(&transaction,
                                                             99U));
    EXPECT_TRUE("no speaker generation starts physically stopped",
                transaction.speaker_stopped);
    const uint32_t timeout_token = transaction.token;
    EXPECT_TRUE("timeout cannot enter recovery before deadline",
                nuwax_sleep_transaction_begin_quiescing(
                    &transaction, timeout_token) &&
                    !nuwax_sleep_transaction_begin_recovery_after_timeout(
                        &transaction, timeout_token, 98U, 3000U));
    EXPECT_TRUE("timeout enters recovery without fabricating completion",
                nuwax_sleep_transaction_begin_recovery_after_timeout(
                    &transaction, timeout_token, 99U, 3000U) &&
                    transaction.state ==
                        NUWAX_SLEEP_TRANSACTION_RECOVERING &&
                    transaction.deadline_ms == 3099U &&
                    !transaction.capture_released &&
                    transaction.speaker_stopped &&
                    !transaction.protocol_closed &&
                    !nuwax_sleep_transaction_ready_to_commit(&transaction));
    EXPECT_TRUE("recovery accepts real completion events",
                nuwax_sleep_transaction_mark_capture_released(
                    &transaction, timeout_token) &&
                    nuwax_sleep_transaction_mark_protocol_closed(
                        &transaction, timeout_token, 9U) &&
                    nuwax_sleep_transaction_ready_to_commit(&transaction));

    nuwax_sleep_transaction_t abort_transaction;
    nuwax_sleep_transaction_init(&abort_transaction);
    EXPECT_TRUE("exclusive takeover can abort committing transaction",
                nuwax_sleep_transaction_request(
                    &abort_transaction,
                    4000U,
                    2000U,
                    10U,
                    0U,
                    "cmd_abort",
                    "sleep_conversation",
                    "turn_abort",
                    false) == NUWAX_SLEEP_REQUEST_STARTED &&
                    nuwax_sleep_transaction_begin_quiescing(
                        &abort_transaction, abort_transaction.token) &&
                    nuwax_sleep_transaction_mark_capture_released(
                        &abort_transaction, abort_transaction.token) &&
                    nuwax_sleep_transaction_mark_protocol_closed(
                        &abort_transaction,
                        abort_transaction.token,
                        10U) &&
                    nuwax_sleep_transaction_begin_committing(
                        &abort_transaction, abort_transaction.token));
    const uint32_t aborted_token = abort_transaction.token;
    EXPECT_TRUE("abort invalidates token and releases audio barrier",
                nuwax_sleep_transaction_abort(&abort_transaction,
                                              aborted_token) &&
                    abort_transaction.state == NUWAX_SLEEP_TRANSACTION_IDLE &&
                    !abort_transaction.active &&
                    abort_transaction.token != aborted_token &&
                    !nuwax_sleep_transaction_blocks_audio(
                        &abort_transaction) &&
                    !nuwax_sleep_transaction_abort(&abort_transaction,
                                                   aborted_token));

    nuwax_deferred_sleep_intent_t deferred;
    nuwax_deferred_sleep_intent_init(&deferred);
    EXPECT_TRUE("deferred sleep starts without becoming an audio barrier",
                nuwax_deferred_sleep_intent_request(
                    &deferred,
                    5000U,
                    60000U,
                    12U,
                    "cmd_after_playback",
                    "sleep_conversation",
                    "turn_reply",
                    false) == NUWAX_SLEEP_REQUEST_STARTED &&
                    deferred.active && deferred.token != 0U);
    const uint32_t deferred_token = deferred.token;
    EXPECT_TRUE("deferred sleep request is idempotent",
                nuwax_deferred_sleep_intent_request(
                    &deferred,
                    5100U,
                    60000U,
                    12U,
                    "cmd_after_playback",
                    "sleep_conversation",
                    "turn_reply",
                    false) ==
                    NUWAX_SLEEP_REQUEST_DUPLICATE_ACTIVE);
    EXPECT_TRUE("different deferred request cannot replace intent",
                nuwax_deferred_sleep_intent_request(
                    &deferred,
                    5100U,
                    60000U,
                    12U,
                    "cmd_other",
                    "sleep_conversation",
                    "turn_other",
                    false) == NUWAX_SLEEP_REQUEST_BUSY);
    EXPECT_TRUE("active reply keeps deferred sleep pending",
                !nuwax_deferred_sleep_intent_ready_to_execute(
                    &deferred, 10000U, true, true, true));
    EXPECT_TRUE("playback progress extends deferred safety deadline",
                nuwax_deferred_sleep_intent_extend(
                    &deferred, deferred_token, 60000U, 60000U) &&
                    !nuwax_deferred_sleep_intent_ready_to_execute(
                        &deferred, 119999U, true, true, true) &&
                    nuwax_deferred_sleep_intent_ready_to_execute(
                        &deferred, 120000U, true, true, true));
    EXPECT_TRUE("physical and protocol completion releases deferred sleep",
                nuwax_deferred_sleep_intent_ready_to_execute(
                    &deferred, 10000U, false, false, false));
    EXPECT_TRUE("stale speaking UI is not deferred sleep turn ownership",
                !nuwax_deferred_sleep_turn_owner_active(
                    false,
                    NUWAX_APP_STATE_SPEAKING,
                    false,
                    false));
    EXPECT_TRUE("thinking state remains deferred sleep turn ownership",
                nuwax_deferred_sleep_turn_owner_active(
                    false,
                    NUWAX_APP_STATE_THINKING,
                    false,
                    false));
    EXPECT_TRUE("real owners still dominate a stale speaking UI",
                nuwax_deferred_sleep_turn_owner_active(
                    true,
                    NUWAX_APP_STATE_SPEAKING,
                    false,
                    false) &&
                    nuwax_deferred_sleep_turn_owner_active(
                        false,
                        NUWAX_APP_STATE_SPEAKING,
                        true,
                        false) &&
                    nuwax_deferred_sleep_turn_owner_active(
                        false,
                        NUWAX_APP_STATE_SPEAKING,
                        false,
                        true));
    EXPECT_TRUE("deadline converts lost terminal into executable fallback",
                nuwax_deferred_sleep_intent_ready_to_execute(
                    &deferred, 120000U, true, true, true));
    EXPECT_TRUE("wrong token cannot consume deferred sleep",
                !nuwax_deferred_sleep_intent_consume(
                    &deferred, deferred_token + 1U));
    EXPECT_TRUE("matching token consumes deferred sleep once",
                nuwax_deferred_sleep_intent_consume(
                    &deferred, deferred_token) &&
                    !deferred.active && deferred.token == deferred_token);
    EXPECT_TRUE("deferred deadline comparison is wrap safe",
                nuwax_deferred_sleep_intent_request(
                    &deferred,
                    UINT32_MAX - 100U,
                    200U,
                    13U,
                    "cmd_deferred_wrap",
                    "sleep_conversation",
                    "turn_wrap",
                    false) == NUWAX_SLEEP_REQUEST_STARTED &&
                    !nuwax_deferred_sleep_intent_ready_to_execute(
                        &deferred, 50U, true, true, true) &&
                    nuwax_deferred_sleep_intent_ready_to_execute(
                        &deferred, 99U, true, true, true));

    printf("sleep transaction host check: %s (%d failures)\n",
           failures == 0 ? "PASS" : "FAIL",
           failures);
    return failures == 0 ? 0 : 1;
}
