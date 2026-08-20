/* Host-only logic check. This file is intentionally excluded from firmware CMake sources. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "conversation/playback_terminal_guard.h"

static int expect(const char *name, bool actual, bool expected) {
    const bool passed = actual == expected;
    printf("[%s] %s actual=%d\n",
           passed ? "PASS" : "FAIL", name, actual ? 1 : 0);
    return passed ? 0 : 1;
}

int main(void) {
    int failures = 0;
    nuwax_playback_terminal_guard_t guard = {0};

    failures += expect("arm",
                       nuwax_playback_terminal_guard_arm(
                           &guard,
                           7U,
                           3U,
                           "turn-a",
                           "playback-a",
                           100U,
                           1000U),
                       true);
    failures += expect("armed_playback_blocks_resume_immediately",
                       nuwax_playback_terminal_guard_waiting(&guard),
                       true);
    failures += expect("armed_liveness_deadline_not_expired",
                       nuwax_playback_terminal_guard_deadline_expired(
                           &guard, 1099U),
                       false);
    failures += expect("armed_liveness_deadline_expires",
                       nuwax_playback_terminal_guard_deadline_expired(
                           &guard, 1100U),
                       true);
    failures += expect("accepted_pcm_refreshes_liveness",
                       nuwax_playback_terminal_guard_refresh_progress(
                           &guard, 7U, 3U, 500U, 1000U) &&
                           guard.deadline_ms == 1500U,
                       true);
    failures += expect("stale_pcm_cannot_refresh_liveness",
                       nuwax_playback_terminal_guard_refresh_progress(
                           &guard, 7U, 4U, 600U, 1000U),
                       false);
    failures += expect("active_playback_blocks_resume",
                       nuwax_playback_terminal_guard_ready(&guard), false);
    failures += expect("new_generation_cannot_replace_active_guard",
                       nuwax_playback_terminal_guard_arm(
                           &guard,
                           7U,
                           4U,
                           "turn-new",
                           "playback-new",
                           600U,
                           1000U),
                       false);
    failures += expect("logical_end_alone_not_ready",
                       nuwax_playback_terminal_guard_mark_logical_end(
                           &guard, 7U, "turn-a", "playback-a", true) &&
                           nuwax_playback_terminal_guard_ready(&guard),
                       false);
    failures += expect(
        "completed_logical_end_matches_natural_drain",
        nuwax_playback_terminal_guard_natural_drain_matches(
            &guard, 7U, 3U, "turn-a", "playback-a"),
        true);
    failures += expect(
        "natural_drain_rejects_stale_generation",
        nuwax_playback_terminal_guard_natural_drain_matches(
            &guard, 7U, 4U, "turn-a", "playback-a"),
        false);
    failures += expect("normal_physical_and_both_terminals_ready",
                       nuwax_playback_terminal_guard_mark_physical_done(
                           &guard, 7U, 3U, true, 100U, 15000U) &&
                           nuwax_playback_terminal_guard_ready(&guard),
                       true);
    failures += expect("physical_terminal_rejects_pcm_refresh",
                       nuwax_playback_terminal_guard_refresh_progress(
                           &guard, 7U, 3U, 700U, 1000U),
                       false);

    nuwax_playback_terminal_guard_clear(&guard);
    (void)nuwax_playback_terminal_guard_arm(
        &guard, 8U, 4U, "turn-b", "playback-b", 100U, 15000U);
    failures += expect("fault_marked",
                       nuwax_playback_terminal_guard_mark_fault(
                           &guard, 8U, 4U, 200U, 15000U),
                       true);
    failures += expect("fault_local_stop_waits_for_old_stream",
                       nuwax_playback_terminal_guard_mark_physical_done(
                           &guard, 8U, 4U, false, 250U, 15000U) &&
                           nuwax_playback_terminal_guard_ready(&guard),
                       false);
    failures += expect("stale_epoch_end_rejected",
                       nuwax_playback_terminal_guard_mark_stream_end(
                           &guard, 7U, 4U),
                       false);
    failures += expect("stale_generation_end_rejected",
                       nuwax_playback_terminal_guard_mark_stream_end(
                           &guard, 8U, 3U),
                       false);
    failures += expect("late_cancelled_stream_end_releases_fault",
                       nuwax_playback_terminal_guard_mark_stream_end(
                           &guard, 8U, 4U) &&
                           nuwax_playback_terminal_guard_ready(&guard),
                       true);

    nuwax_playback_terminal_guard_clear(&guard);
    (void)nuwax_playback_terminal_guard_arm(
        &guard,
        11U,
        19U,
        "turn-cancel",
        "pb-cancel",
        50U,
        15000U);
    (void)nuwax_playback_terminal_guard_mark_fault(
        &guard, 11U, 19U, 100U, 15000U);
    failures += expect("fault_cancel_sent_bound",
                       nuwax_playback_terminal_guard_mark_cancel_sent(
                           &guard,
                           11U,
                           19U,
                           "fault_cancel_1",
                           200U,
                           3000U),
                       true);
    failures += expect("fault_cancel_shortens_deadline",
                       guard.deadline_ms == 3200U,
                       true);
    failures += expect(
        "fault_cancel_stale_epoch_ack_rejected",
        nuwax_playback_terminal_guard_apply_cancel_ack(
            &guard,
            10U,
            "fault_cancel_1",
            "turn-cancel",
            "pb-cancel",
            true) == NUWAX_PLAYBACK_CANCEL_ACK_UNMATCHED,
        true);
    failures += expect(
        "fault_cancel_stale_request_ack_rejected",
        nuwax_playback_terminal_guard_apply_cancel_ack(
            &guard,
            11U,
            "fault_cancel_other",
            "turn-cancel",
            "pb-cancel",
            true) == NUWAX_PLAYBACK_CANCEL_ACK_UNMATCHED,
        true);
    failures += expect(
        "fault_cancel_matching_rejection_detected",
        nuwax_playback_terminal_guard_apply_cancel_ack(
            &guard,
            11U,
            "fault_cancel_1",
            "turn-cancel",
            "pb-cancel",
            false) == NUWAX_PLAYBACK_CANCEL_ACK_REJECTED,
        true);
    failures += expect(
        "fault_cancel_matching_ack_accepted",
        nuwax_playback_terminal_guard_apply_cancel_ack(
            &guard,
            11U,
            "fault_cancel_1",
            "turn-cancel",
            "pb-cancel",
            true) == NUWAX_PLAYBACK_CANCEL_ACK_ACCEPTED,
        true);
    failures += expect("fault_cancel_ack_waits_physical_stop",
                       nuwax_playback_terminal_guard_ready(&guard),
                       false);
    (void)nuwax_playback_terminal_guard_mark_physical_done(
        &guard, 11U, 19U, false, 250U, 15000U);
    failures += expect("fault_cancel_ack_and_physical_ready",
                       nuwax_playback_terminal_guard_ready(&guard),
                       true);

    nuwax_playback_terminal_guard_clear(&guard);
    (void)nuwax_playback_terminal_guard_arm(
        &guard,
        12U,
        20U,
        "turn-legacy",
        "pb-legacy",
        100U,
        15000U);
    (void)nuwax_playback_terminal_guard_mark_fault(
        &guard, 12U, 20U, 500U, 15000U);
    (void)nuwax_playback_terminal_guard_mark_cancel_sent(
        &guard, 12U, 20U, "fault_cancel_2", 550U, 3000U);
    failures += expect(
        "fault_cancel_legacy_ack_matches_single_turn",
        nuwax_playback_terminal_guard_apply_cancel_ack(
            &guard,
            12U,
            "",
            "turn-legacy",
            "",
            true) == NUWAX_PLAYBACK_CANCEL_ACK_ACCEPTED,
        true);

    nuwax_playback_terminal_guard_clear(&guard);
    (void)nuwax_playback_terminal_guard_arm(
        &guard, 9U, 5U, "turn-c", "playback-c", 100U, 15000U);
    (void)nuwax_playback_terminal_guard_mark_fault(
        &guard, 9U, 5U, 1000U, 15000U);
    (void)nuwax_playback_terminal_guard_mark_physical_done(
        &guard, 9U, 5U, false, 1100U, 15000U);
    failures += expect("fault_matching_logical_end_releases",
                       nuwax_playback_terminal_guard_mark_logical_end(
                           &guard, 9U, "turn-c", "playback-c", false) &&
                           nuwax_playback_terminal_guard_ready(&guard),
                       true);
    failures += expect(
        "faulted_end_cannot_preserve_natural_drain",
        nuwax_playback_terminal_guard_natural_drain_matches(
            &guard, 9U, 5U, "turn-c", "playback-c"),
        false);

    nuwax_playback_terminal_guard_clear(&guard);
    (void)nuwax_playback_terminal_guard_arm(
        &guard,
        13U,
        21U,
        "turn-local",
        "pb-local",
        100U,
        3000U);
    (void)nuwax_playback_terminal_guard_mark_fault(
        &guard, 13U, 21U, 1000U, 3000U);
    failures += expect("local_recovery_requires_physical_stop",
                       nuwax_playback_terminal_guard_commit_local_recovery(
                           &guard, 13U, 21U),
                       false);
    (void)nuwax_playback_terminal_guard_mark_physical_done(
        &guard, 13U, 21U, false, 1100U, 3000U);
    failures += expect("local_recovery_rejects_stale_generation",
                       nuwax_playback_terminal_guard_commit_local_recovery(
                           &guard, 13U, 20U),
                       false);
    failures += expect("local_recovery_commits_matching_quarantine",
                       nuwax_playback_terminal_guard_commit_local_recovery(
                           &guard, 13U, 21U),
                       true);
    failures += expect("local_recovery_and_physical_stop_ready",
                       nuwax_playback_terminal_guard_ready(&guard),
                       true);

    nuwax_playback_terminal_guard_clear(&guard);
    (void)nuwax_playback_terminal_guard_arm(
        &guard,
        14U,
        22U,
        "turn-missing-end",
        "pb-missing-end",
        1000U,
        3000U);
    (void)nuwax_playback_terminal_guard_mark_physical_done(
        &guard, 14U, 22U, false, 2000U, 3000U);
    failures += expect("normal_missing_terminal_can_be_quarantined",
                       nuwax_playback_terminal_guard_commit_local_recovery(
                           &guard, 14U, 22U) &&
                           nuwax_playback_terminal_guard_ready(&guard),
                       true);
    failures += expect("deadline_not_expired",
                       nuwax_playback_terminal_guard_deadline_expired(
                           &guard, 4999U),
                       false);
    failures += expect("deadline_expired",
                       nuwax_playback_terminal_guard_deadline_expired(
                           &guard, 5000U),
                       true);
    failures += expect("reconnect_only_once_first",
                       nuwax_playback_terminal_guard_mark_reconnect_requested(
                           &guard),
                       true);
    failures += expect("reconnect_only_once_second",
                       nuwax_playback_terminal_guard_mark_reconnect_requested(
                           &guard),
                       false);

    nuwax_playback_terminal_guard_clear(&guard);
    (void)nuwax_playback_terminal_guard_arm(
        &guard, 10U, 6U, "turn-d", "playback-d", 100U, 1000U);
    failures += expect("committed_interrupt_stale_epoch_rejected",
                       nuwax_playback_terminal_guard_release_committed_interrupt(
                           &guard, 9U, 6U, "turn-d", "playback-d"),
                       false);
    failures += expect("committed_interrupt_stale_generation_rejected",
                       nuwax_playback_terminal_guard_release_committed_interrupt(
                           &guard, 10U, 7U, "turn-d", "playback-d"),
                       false);
    failures += expect("committed_interrupt_stale_ids_rejected",
                       nuwax_playback_terminal_guard_release_committed_interrupt(
                           &guard, 10U, 6U, "turn-other", "playback-d"),
                       false);
    failures += expect("committed_interrupt_matching_owner_releases",
                       nuwax_playback_terminal_guard_release_committed_interrupt(
                           &guard, 10U, 6U, "turn-d", "playback-d"),
                       true);
    failures += expect("committed_interrupt_removes_block", guard.active, false);

    failures += expect("remote_stop_arm",
                       nuwax_playback_terminal_guard_arm(
                           &guard,
                           8U,
                           18U,
                           "turn-stop",
                           "pb-stop",
                           300U,
                           1000U),
                       true);
    failures += expect("remote_stop_stale_scope_rejected",
                       nuwax_playback_terminal_guard_mark_committed_stop(
                           &guard, 8U, 18U, "turn-other", "pb-stop"),
                       false);
    failures += expect("remote_stop_terminal_marked",
                       nuwax_playback_terminal_guard_mark_committed_stop(
                           &guard, 8U, 18U, "turn-stop", "pb-stop"),
                       true);
    failures += expect(
        "committed_stop_is_not_natural_drain",
        nuwax_playback_terminal_guard_natural_drain_matches(
            &guard, 8U, 18U, "turn-stop", "pb-stop"),
        false);
    failures += expect("remote_stop_waits_physical",
                       nuwax_playback_terminal_guard_ready(&guard),
                       false);
    failures += expect("remote_stop_physical_done",
                       nuwax_playback_terminal_guard_mark_physical_done(
                           &guard, 8U, 18U, false, 400U, 1000U),
                       true);
    failures += expect("remote_stop_ready",
                       nuwax_playback_terminal_guard_ready(&guard),
                       true);

    nuwax_playback_terminal_guard_clear(&guard);
    failures += expect("disconnect_clear_removes_block", guard.active, false);

    printf("playback terminal guard host check: %s (%d failures)\n",
           failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
