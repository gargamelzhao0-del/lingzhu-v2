/* Host-only logic check. This file is intentionally excluded from firmware CMake sources. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "conversation/preroll_window.h"

typedef struct {
    const uint32_t *sequences;
} sequence_fixture_t;

static uint32_t fixture_sequence_at(void *ctx, uint32_t offset) {
    const sequence_fixture_t *fixture = (const sequence_fixture_t *)ctx;
    return fixture->sequences[offset];
}

static int expect_window(const char *name,
                         const uint32_t *sequences,
                         uint32_t count,
                         uint32_t onset,
                         uint32_t live,
                         uint32_t lead,
                         uint32_t fallback,
                         bool expected_ok,
                         uint32_t expected_first,
                         uint32_t expected_count,
                         bool expected_onset_included,
                         bool expected_gap_trimmed) {
    sequence_fixture_t fixture = {.sequences = sequences};
    nuwax_preroll_window_t window = {0};
    const bool ok = nuwax_preroll_select_targeted_window(count,
                                                          onset,
                                                          live,
                                                          lead,
                                                          fallback,
                                                          fixture_sequence_at,
                                                          &fixture,
                                                          &window);
    const bool passed =
        ok == expected_ok &&
        (!ok || (window.first_sequence == expected_first &&
                 window.frame_count == expected_count &&
                 window.onset_included == expected_onset_included &&
                 window.gap_trimmed == expected_gap_trimmed));
    printf("[%s] %s ok=%d first=%u count=%u onset_included=%d gap_trimmed=%d\n",
           passed ? "PASS" : "FAIL",
           name,
           ok ? 1 : 0,
           window.first_sequence,
           window.frame_count,
           window.onset_included ? 1 : 0,
           window.gap_trimmed ? 1 : 0);
    return passed ? 0 : 1;
}

int main(void) {
    int failures = 0;
    const bool confirmed_onset_owns_round =
        nuwax_preroll_choose_confirmed_onset(
            true, 892U, true, 159U, 912U) == 892U;
    const bool pending_candidate_remains_fallback =
        nuwax_preroll_choose_confirmed_onset(
            false, 0U, true, 159U, 912U) == 159U;
    const bool live_frame_is_last_resort =
        nuwax_preroll_choose_confirmed_onset(
            false, 0U, false, 0U, 912U) == 912U;
    const bool onset_ownership_ok = confirmed_onset_owns_round &&
                                    pending_candidate_remains_fallback &&
                                    live_frame_is_last_resort;
    printf("[%s] confirmed_onset_supersedes_stale_candidate\n",
           onset_ownership_ok ? "PASS" : "FAIL");
    failures += onset_ownership_ok ? 0 : 1;

    uint32_t long_history[128] = {0};
    for (uint32_t i = 0; i < 128; i++) {
        long_history[i] = 1000U + i;
    }
    failures += expect_window("one_point_five_second_lead",
                              long_history, 128, 1100, 1128, 94, 106,
                              true, 1006, 122, true, false);

    const uint32_t normal[] = {
        100, 101, 102, 103, 104, 105, 106, 107, 108, 109,
        110, 111, 112, 113, 114, 115, 116, 117, 118, 119,
        120, 121, 122, 123, 124, 125, 126, 127, 128, 129,
        130, 131, 132, 133, 134, 135, 136, 137, 138, 139,
    };
    failures += expect_window("onset_minus_19",
                              normal, 40, 128, 140, 19, 31,
                              true, 109, 31, true, false);

    const uint32_t short_history[] = {200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210};
    failures += expect_window("short_history",
                              short_history, 11, 202, 211, 19, 31,
                              true, 200, 11, true, false);

    const uint32_t wrapped[] = {UINT32_MAX - 5U, UINT32_MAX - 4U, UINT32_MAX - 3U,
                                UINT32_MAX - 2U, UINT32_MAX - 1U, UINT32_MAX, 0, 1};
    failures += expect_window("sequence_wrap",
                              wrapped, 8, UINT32_MAX, 2, 3, 6,
                              true, UINT32_MAX - 3U, 6, true, false);

    const uint32_t gap_before_onset[] = {10, 11, 12, 20, 21, 22, 23};
    failures += expect_window("trim_gap_before_onset",
                              gap_before_onset, 7, 21, 24, 19, 6,
                              true, 20, 4, true, true);

    const uint32_t gap_after_onset[] = {10, 11, 12, 13, 20, 21};
    failures += expect_window("gap_after_onset_rejected",
                              gap_after_onset, 6, 12, 22, 19, 4,
                              false, 0, 0, false, false);

    const uint32_t missing_onset[] = {10, 11, 12, 13, 14, 15};
    failures += expect_window("missing_onset_rejected",
                              missing_onset, 6, 99, 16, 19, 4,
                              false, 0, 0, false, false);
    failures += expect_window("live_sequence_gap_rejected",
                              missing_onset, 6, 12, 20, 19, 4,
                              false, 0, 0, false, false);

    const uint32_t gap_between_onset_and_confirmation[] = {
        100, 101, 102, 110, 111,
    };
    failures += expect_window("gap_between_onset_and_confirmation_rejected",
                              gap_between_onset_and_confirmation, 5, 101, 112,
                              1, 4, false, 0, 0, false, false);

    const uint32_t gap_before_onset_with_lead[] = {10, 11, 20, 21, 22};
    failures += expect_window("gap_before_onset_keeps_contiguous_lead",
                              gap_before_onset_with_lead, 5, 21, 23, 3, 4,
                              true, 20, 3, true, true);

    nuwax_preroll_window_t window = {0};
    sequence_fixture_t empty_fixture = {.sequences = missing_onset};
    const bool empty_ok = nuwax_preroll_select_targeted_window(0, 0, 0, 0, 0,
                                                               fixture_sequence_at,
                                                               &empty_fixture,
                                                               &window);
    printf("[%s] empty_buffer_rejected\n", empty_ok ? "FAIL" : "PASS");
    failures += empty_ok ? 1 : 0;

    const uint32_t exact_onset[] = {400, 401, 402, 403, 404, 405};
    failures += expect_window("barge_lead_preserves_early_phoneme",
                              exact_onset, 6, 402, 406, 4, 8,
                              true, 400, 6, true, false);

    uint32_t repaired_gap_frames = 0U;
#define EXPECT_GAP_POLICY(name, confirmed, uploaded, confirmation_seq, previous_seq, current_seq, expected, expected_gap) \
    do { \
        repaired_gap_frames = UINT32_MAX; \
        const bool allowed = nuwax_preroll_confirmed_gap_repair_allowed( \
            (confirmed), (uploaded), (confirmation_seq), (previous_seq), \
            (current_seq), &repaired_gap_frames); \
        const bool passed = allowed == (expected) && \
                            repaired_gap_frames == (expected_gap); \
        printf("[%s] %s allowed=%d gap=%u\n", \
               passed ? "PASS" : "FAIL", (name), allowed ? 1 : 0, \
               repaired_gap_frames); \
        failures += passed ? 0 : 1; \
    } while (0)

    EXPECT_GAP_POLICY("confirmed_gap_one_frame_allowed",
                      true, false, 100U, 100U, 102U, true, 1U);
    EXPECT_GAP_POLICY("confirmed_gap_five_frames_allowed",
                      true, false, 100U, 110U, 116U, true, 5U);
    EXPECT_GAP_POLICY("confirmed_gap_eight_frame_boundary_allowed",
                      true, false, 100U, 110U, 119U, true, 8U);
    EXPECT_GAP_POLICY("confirmed_gap_nine_frames_rejected",
                      true, false, 100U, 110U, 120U, false, 0U);
    EXPECT_GAP_POLICY("gap_before_confirmation_rejected",
                      true, false, 110U, 105U, 108U, false, 0U);
    EXPECT_GAP_POLICY("unconfirmed_gap_rejected",
                      false, false, 100U, 110U, 113U, false, 0U);
    EXPECT_GAP_POLICY("uploaded_gap_rejected",
                      true, true, 100U, 110U, 113U, false, 0U);
    EXPECT_GAP_POLICY("confirmed_gap_sequence_wrap_allowed",
                      true, false, UINT32_MAX - 2U, UINT32_MAX, 3U,
                      true, 3U);
#undef EXPECT_GAP_POLICY

    const bool confirmation_silence_policy_ok =
        !nuwax_preroll_targeted_frame_preserves_confirmation(
            true, true, true) &&
        !nuwax_preroll_targeted_frame_preserves_confirmation(
            true, false, true) &&
        nuwax_preroll_targeted_frame_preserves_confirmation(
            false, true, true);
    printf("[%s] forced_silence_only_after_confirmation\n",
           confirmation_silence_policy_ok ? "PASS" : "FAIL");
    failures += confirmation_silence_policy_ok ? 0 : 1;

    const bool clean_overlap_silenced =
        nuwax_preroll_overlap_frame_should_silence(true, true, 6000, 12000);
    const bool correlated_overlap_silenced =
        nuwax_preroll_overlap_frame_should_silence(true, true, 12000, 12000);
    const bool untrusted_overlap_silenced =
        nuwax_preroll_overlap_frame_should_silence(true, false, 1000, 12000);
    const bool post_stop_clean_silenced =
        nuwax_preroll_overlap_frame_should_silence(false, false, 32768, 12000);
    const bool overlap_policy_ok =
        !clean_overlap_silenced && correlated_overlap_silenced &&
        untrusted_overlap_silenced && !post_stop_clean_silenced;
    printf("[%s] barge_overlap_safety_policy\n", overlap_policy_ok ? "PASS" : "FAIL");
    failures += overlap_policy_ok ? 0 : 1;

    const nuwax_preroll_audio_trust_t aec_trust = {
        .reference_generation = 7U,
        .playback_generation = 11U,
        .volume_generation = 3U,
        .volume_percent = 70U,
        .playback_state_stable = true,
        .volume_state_stable = true,
        .reference_snapshot = true,
        .reference_generation_valid = true,
        .reference_contiguous = true,
        .reference_active = true,
        .reference_locked = true,
        .aec_processed_ok = true,
    };
    const bool trusted_aec_preserved =
        nuwax_preroll_audio_trust_allows_preserve(
            &aec_trust, false, false, false);
    nuwax_preroll_audio_trust_t underrun_trust = aec_trust;
    underrun_trust.output_underrun_delta = 1U;
    const bool underrun_rejected =
        !nuwax_preroll_audio_trust_allows_preserve(
            &underrun_trust, false, false, false);
    nuwax_preroll_audio_trust_t repaired_trust = aec_trust;
    repaired_trust.aec_processed_ok = false;
    const bool trusted_repair_preserved =
        nuwax_preroll_audio_trust_allows_preserve(
            &repaired_trust, true, true, true);
    repaired_trust.reference_active = false;
    repaired_trust.reference_locked = false;
    const bool inactive_high_volume_repair_rejected =
        !nuwax_preroll_audio_trust_allows_preserve(
            &repaired_trust, true, true, true);
    nuwax_preroll_audio_trust_t silent_trust = aec_trust;
    silent_trust.volume_percent = 5U;
    silent_trust.reference_active = false;
    silent_trust.reference_locked = true;
    silent_trust.aec_processed_ok = false;
    silent_trust.verified_silent_passthrough = true;
    const bool trusted_silence_preserved =
        nuwax_preroll_audio_trust_allows_preserve(
            &silent_trust, false, false, false);
    silent_trust.volume_percent = 6U;
    const bool unverified_volume_rejected =
        !nuwax_preroll_audio_trust_allows_preserve(
            &silent_trust, false, false, false);
    nuwax_preroll_audio_trust_t unstable_playback_trust = aec_trust;
    unstable_playback_trust.playback_state_stable = false;
    const bool unstable_playback_rejected =
        !nuwax_preroll_audio_trust_allows_preserve(
            &unstable_playback_trust, false, false, false);
    nuwax_preroll_audio_trust_t unstable_volume_trust = silent_trust;
    unstable_volume_trust.volume_percent = 5U;
    unstable_volume_trust.volume_state_stable = false;
    const bool unstable_volume_rejected =
        !nuwax_preroll_audio_trust_allows_preserve(
            &unstable_volume_trust, false, false, false);
    const bool trust_policy_ok =
        trusted_aec_preserved && underrun_rejected &&
        trusted_repair_preserved &&
        inactive_high_volume_repair_rejected &&
        trusted_silence_preserved &&
        unverified_volume_rejected &&
        unstable_playback_rejected && unstable_volume_rejected;
    printf("[%s] barge_preroll_audio_trust_policy\n",
           trust_policy_ok ? "PASS" : "FAIL");
    failures += trust_policy_ok ? 0 : 1;

    printf("preroll window host check: %s (%d failures)\n",
           failures == 0 ? "PASS" : "FAIL",
           failures);
    return failures == 0 ? 0 : 1;
}
