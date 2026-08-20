#include <stdio.h>

#include "conversation/speaker_generation_policy.h"

#define CHECK(condition, message)        \
    do {                                 \
        if (!(condition)) {              \
            puts("[FAIL] " message);     \
            return 1;                    \
        }                                \
    } while (0)

int main(void) {
    CHECK(nuwax_speaker_owner_loss_matches_turn(7, true, 7, 0),
          "matching active generation owner loss was ignored");
    CHECK(nuwax_speaker_owner_loss_matches_turn(7, true, 0, 7),
          "matching pending START owner loss was ignored");
    CHECK(!nuwax_speaker_owner_loss_matches_turn(7, true, 8, 9),
          "old generation owner loss invalidated a newer turn");
    CHECK(!nuwax_speaker_owner_loss_matches_turn(0, true, 0, 0),
          "unbound owner loss invalidated a waiting-text turn");
    CHECK(!nuwax_speaker_owner_loss_matches_turn(7, false, 7, 0),
          "owner loss invalidated an inactive turn");

    CHECK(nuwax_speaker_start_submission_allowed(0, 0, 0, 0, 0, false),
          "START was blocked without an END completion owner");
    CHECK(!nuwax_speaker_start_submission_allowed(0, 7, 0, 0, 0, false),
          "START bypassed an END completion owner");
    CHECK(!nuwax_speaker_start_submission_allowed(7, 0, 0, 0, 0, false),
          "START bypassed an in-flight PCM action owner");
    CHECK(!nuwax_speaker_start_submission_allowed(0, 0, 7, 0, 0, false),
          "START bypassed idle output maintenance");
    CHECK(!nuwax_speaker_start_submission_allowed(0, 0, 0, 7, 0, false),
          "START bypassed a pending cancel");
    CHECK(!nuwax_speaker_start_submission_allowed(0, 0, 0, 0, 7, false),
          "START bypassed an in-progress output stop");
    CHECK(!nuwax_speaker_start_submission_allowed(0, 0, 0, 0, 0, true),
          "START bypassed runtime reset");
    CHECK(nuwax_speaker_finish_claim_allowed(7, 7, false, true, 0, 7),
          "current END could not claim completion");
    CHECK(!nuwax_speaker_finish_claim_allowed(8, 7, false, true, 0, 7),
          "old END survived a submitted START");
    CHECK(!nuwax_speaker_finish_claim_allowed(7, 7, true, true, 0, 7),
          "cancelled END claimed completion");
    CHECK(!nuwax_speaker_finish_claim_allowed(7, 7, false, false, 0, 7),
          "duplicate END claimed completion");
    CHECK(!nuwax_speaker_finish_claim_allowed(7, 7, false, true, 7, 7),
          "duplicate END bypassed the completion owner");
    CHECK(!nuwax_speaker_completion_owner_can_release(7, 8),
          "non-owner released END completion");
    CHECK(nuwax_speaker_completion_owner_can_release(7, 7),
          "END completion owner could not release its claim");

    CHECK(nuwax_speaker_cancel_expected_matches(8, false, 8),
          "current submitted generation could not be cancelled");
    CHECK(!nuwax_speaker_cancel_expected_matches(9, false, 8),
          "old fade cancel matched a newer START");
    CHECK(!nuwax_speaker_cancel_expected_matches(8, true, 8),
          "fade cancel bypassed an existing cancel");
    CHECK(nuwax_speaker_natural_drain_preservable(
              8, 3, 8, 3, false, true, 8, true, false, false, false, 3, 8),
          "matching completed stream could not preserve natural drain");
    CHECK(!nuwax_speaker_natural_drain_preservable(
              8, 3, 8, 3, false, true, 8, false, false, false, false, 3, 8),
          "natural drain was preserved before binary END admission");
    CHECK(!nuwax_speaker_natural_drain_preservable(
              8, 2, 8, 2, false, true, 8, true, false, false, false, 3, 8),
          "natural drain crossed a WebSocket epoch");
    CHECK(!nuwax_speaker_natural_drain_preservable(
              8, 3, 8, 3, false, true, 8, true, true, false, false, 3, 8),
          "protocol-faulted stream bypassed scoped stop");
    CHECK(!nuwax_speaker_natural_drain_preservable(
              8, 3, 8, 3, false, true, 8, true, false, false, true, 3, 8),
          "active cancellation was replaced by natural drain");
    CHECK(nuwax_speaker_live_cancel_generation(
              18, true, true, 19, false, 0, true, 4, 19) == 19,
          "stale turn guard overrode the live playback generation");
    CHECK(nuwax_speaker_live_cancel_generation(
              19, true, true, 19, false, 0, true, 4, 19) == 19,
          "matching turn guard did not preserve the live generation");
    CHECK(nuwax_speaker_live_cancel_generation(
              0, false, false, 0, true, 20, true, 1, 20) == 20,
          "pending START generation was not selected for cancellation");
    CHECK(nuwax_speaker_live_cancel_generation(
              17, false, false, 21, false, 0, true, 2, 21) == 21,
          "queued live stream fell back to a stale guard generation");
    CHECK(nuwax_speaker_live_cancel_generation(
              17, false, false, 21, false, 0, false, 0, 21) == 0,
          "inactive historical generations were treated as live playback");
    CHECK(nuwax_speaker_unconditional_cancel_join_target(
              0, 8, 0, 7) == 7,
          "unconditional cancel did not join G7->G8 pending stop");
    CHECK(nuwax_speaker_unconditional_cancel_join_target(
              0, 8, 8, 7) == 7,
          "unconditional cancel replaced an owned G7 stop transaction");
    CHECK(nuwax_speaker_unconditional_cancel_join_target(
              7, 0, 0, 0) == 7,
          "unconditional cancel bypassed natural END completion");
    CHECK(nuwax_speaker_unconditional_cancel_join_target(
              0, 0, 0, 0) == 0,
          "unconditional cancel joined a nonexistent stop transaction");

    CHECK(nuwax_speaker_probe_duck_allowed(8, 8, false, true, 8),
          "current generation could not start a probe duck");
    CHECK(!nuwax_speaker_probe_duck_allowed(9, 9, false, true, 8),
          "old generation started a probe duck on new playback");
    CHECK(!nuwax_speaker_probe_duck_allowed(8, 8, true, true, 8),
          "cancelled playback started a probe duck");
    CHECK(!nuwax_speaker_probe_duck_allowed(8, 8, false, false, 8),
          "probe duck bypassed an unavailable output");
    CHECK(nuwax_speaker_probe_output_percent(100, 12) == 25,
          "12 dB probe attenuation mismatch");
    CHECK(nuwax_speaker_probe_output_percent(100, 15) == 18,
          "15 dB probe attenuation mismatch");
    CHECK(nuwax_speaker_probe_output_percent(100, 18) == 13,
          "18 dB probe attenuation mismatch");
    CHECK(nuwax_speaker_probe_output_percent(1, 18) == 1,
          "non-zero logical volume was muted by probe rounding");
    CHECK(nuwax_speaker_probe_output_percent(0, 12) == 0,
          "muted logical volume became audible during probe");

    CHECK(nuwax_speaker_write_claim_allowed(9, 9, false, 0, 0, 9),
          "current PCM could not claim I/O");
    CHECK(!nuwax_speaker_write_claim_allowed(10, 9, false, 0, 0, 9),
          "old PCM survived a submitted START");
    CHECK(!nuwax_speaker_write_claim_allowed(9, 9, false, 0, 8, 9),
          "PCM bypassed an in-progress output stop");

    CHECK(nuwax_speaker_stop_claim_decide(12, 12, true, 0, 12) ==
              NUWAX_SPEAKER_STOP_CLAIMED,
          "current cancel could not claim output stop");
    CHECK(nuwax_speaker_stop_claim_decide(13, 13, true, 12, 13) ==
              NUWAX_SPEAKER_STOP_WAIT_OWNER,
          "second cancel bypassed the existing stop owner");
    CHECK(nuwax_speaker_stop_claim_decide(14, 13, false, 0, 13) ==
              NUWAX_SPEAKER_STOP_SUPERSEDED,
          "superseded cancel retained stop permission");
    CHECK(nuwax_speaker_stop_recovery_claim_allowed(13, 13, true, 0, 13),
          "timed-out claim wait could not recover output-stop ownership");
    CHECK(!nuwax_speaker_stop_recovery_claim_allowed(13, 13, true, 12, 13),
          "recovery stole output-stop ownership from another transaction");
    CHECK(!nuwax_speaker_stop_owner_can_release(12, 13),
          "non-owner released output stop");
    CHECK(nuwax_speaker_stop_owner_can_release(12, 12),
          "owner could not release output stop");

    puts("speaker generation policy host check: PASS");
    return 0;
}
