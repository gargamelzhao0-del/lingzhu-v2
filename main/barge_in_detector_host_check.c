#include <stdbool.h>
#include <stdio.h>

#include "conversation/barge_in_detector.h"

typedef struct {
    const char *name;
    nuwax_barge_in_nearfield_features_t features;
    bool expected;
} test_case_t;

static const test_case_t s_cases[] = {
    {
        .name = "s5_real_nearfield_seq1292",
        .features = {
            .seed_evidence = true,
            .ref_active = true,
            .voice_like = true,
            .mic1_avg_abs = 1577,
            .mic1_peak = 6468,
            .mic2_avg_abs = 1184,
            .mic2_peak = 6551,
            .diff_avg_abs = 648,
            .diff_peak = 2971,
            .diff_energy_q15 = 5904,
            .balance_q15 = 24601,
            .ref_corr_q15 = 3484,
        },
        .expected = true,
    },
    {
        .name = "s4a_faronly_balance_boundary",
        .features = {
            .seed_evidence = true,
            .ref_active = true,
            .voice_like = true,
            .mic1_avg_abs = 3200,
            .mic1_peak = 9000,
            .mic2_avg_abs = 2525,
            .mic2_peak = 7000,
            .diff_avg_abs = 1815,
            .diff_peak = 5000,
            .diff_energy_q15 = 32564,
            .balance_q15 = 25850,
            .ref_corr_q15 = 2000,
        },
        .expected = false,
    },
    {
        .name = "s10_retry_real_symmetric_seq346",
        .features = {
            .seed_evidence = true,
            .ref_active = true,
            .mic1_avg_abs = 1277,
            .mic1_peak = 5501,
            .mic2_avg_abs = 1260,
            .mic2_peak = 5633,
            .diff_avg_abs = 620,
            .diff_peak = 2663,
            .diff_energy_q15 = 7359,
            .balance_q15 = 32331,
            .ref_corr_q15 = 7044,
        },
        .expected = true,
    },
    {
        .name = "s10_retry_real_symmetric_seq657",
        .features = {
            .seed_evidence = true,
            .ref_active = true,
            .mic1_avg_abs = 1213,
            .mic1_peak = 5729,
            .mic2_avg_abs = 1242,
            .mic2_peak = 5903,
            .diff_avg_abs = 427,
            .diff_peak = 1359,
            .diff_energy_q15 = 3599,
            .balance_q15 = 32002,
            .ref_corr_q15 = 332,
        },
        .expected = true,
    },
    {
        .name = "s21_nohuman_symmetric_seq32_rejected",
        .features = {
            .seed_evidence = true,
            .ref_active = true,
            .mic1_avg_abs = 1339,
            .mic1_peak = 7408,
            .mic2_avg_abs = 1465,
            .mic2_peak = 8928,
            .diff_avg_abs = 600,
            .diff_peak = 2744,
            .diff_energy_q15 = 5208,
            .balance_q15 = 29949,
            .ref_corr_q15 = 7967,
        },
        .expected = false,
    },
    {
        .name = "s22_nohuman_symmetric_seq184_rejected",
        .features = {
            .seed_evidence = true,
            .ref_active = true,
            .mic1_avg_abs = 2403,
            .mic1_peak = 9016,
            .mic2_avg_abs = 2467,
            .mic2_peak = 10482,
            .diff_avg_abs = 942,
            .diff_peak = 3244,
            .diff_energy_q15 = 4638,
            .balance_q15 = 31917,
            .ref_corr_q15 = 6766,
        },
        .expected = false,
    },
    {
        .name = "s9_nohuman_strongest_logged_frame_rejected",
        .features = {
            .seed_evidence = true,
            .ref_active = true,
            .mic1_avg_abs = 829,
            .mic1_peak = 4127,
            .mic2_avg_abs = 1028,
            .mic2_peak = 4915,
            .diff_avg_abs = 797,
            .diff_peak = 3428,
            .diff_energy_q15 = 18880,
            .balance_q15 = 26424,
            .ref_corr_q15 = 5789,
        },
        .expected = false,
    },
    {
        .name = "s9_nohuman_symmetric_high_diff_frame_rejected",
        .features = {
            .seed_evidence = true,
            .ref_active = true,
            .mic1_avg_abs = 892,
            .mic1_peak = 2920,
            .mic2_avg_abs = 968,
            .mic2_peak = 3736,
            .diff_avg_abs = 1197,
            .diff_peak = 4119,
            .diff_energy_q15 = 32768,
            .balance_q15 = 30195,
            .ref_corr_q15 = 762,
        },
        .expected = false,
    },
    {
        .name = "s11_real_symmetric_seq298",
        .features = {
            .seed_evidence = true,
            .ref_active = true,
            .mic1_avg_abs = 2453,
            .mic1_peak = 8779,
            .mic2_avg_abs = 2236,
            .mic2_peak = 9033,
            .diff_avg_abs = 955,
            .diff_peak = 3053,
            .diff_energy_q15 = 4697,
            .balance_q15 = 29869,
            .ref_corr_q15 = 1127,
        },
        .expected = true,
    },
    {
        .name = "s12_real_symmetric_seq112",
        .features = {
            .seed_evidence = true,
            .ref_active = true,
            .mic1_avg_abs = 2114,
            .mic1_peak = 8956,
            .mic2_avg_abs = 2103,
            .mic2_peak = 9187,
            .diff_avg_abs = 1017,
            .diff_peak = 3518,
            .diff_energy_q15 = 6626,
            .balance_q15 = 32597,
            .ref_corr_q15 = 406,
        },
        .expected = true,
    },
    {
        .name = "s12_retry_self_barge_1_rejected",
        .features = {
            .seed_evidence = true,
            .ref_active = true,
            .mic1_avg_abs = 2339,
            .mic1_peak = 5843,
            .mic2_avg_abs = 2095,
            .mic2_peak = 5773,
            .diff_avg_abs = 700,
            .diff_peak = 2155,
            .diff_energy_q15 = 3151,
            .balance_q15 = 29349,
            .ref_corr_q15 = 6399,
        },
        .expected = false,
    },
    {
        .name = "s12_retry_self_barge_2_rejected",
        .features = {
            .seed_evidence = true,
            .ref_active = true,
            .mic1_avg_abs = 1882,
            .mic1_peak = 6805,
            .mic2_avg_abs = 1823,
            .mic2_peak = 7576,
            .diff_avg_abs = 597,
            .diff_peak = 2123,
            .diff_energy_q15 = 3558,
            .balance_q15 = 31740,
            .ref_corr_q15 = 1770,
        },
        .expected = false,
    },
    {
        .name = "s12_retry_self_barge_3_rejected",
        .features = {
            .seed_evidence = true,
            .ref_active = true,
            .mic1_avg_abs = 4431,
            .mic1_peak = 11987,
            .mic2_avg_abs = 4470,
            .mic2_peak = 11047,
            .diff_avg_abs = 1473,
            .diff_peak = 4257,
            .diff_energy_q15 = 3526,
            .balance_q15 = 32482,
            .ref_corr_q15 = 1002,
        },
        .expected = false,
    },
    {
        .name = "symmetric_diff_ratio_below_boundary_rejected",
        .features = {
            .seed_evidence = true,
            .ref_active = true,
            .mic1_avg_abs = 1200,
            .mic1_peak = 5200,
            .mic2_avg_abs = 1000,
            .mic2_peak = 4500,
            .diff_avg_abs = 402,
            .diff_peak = 1300,
            .diff_energy_q15 = 3500,
            .balance_q15 = 28000,
            .ref_corr_q15 = 8000,
        },
        .expected = false,
    },
    {
        .name = "s13_startup_tts_strict_frame_rejected",
        .features = {
            .seed_evidence = true,
            .ref_active = true,
            .mic1_avg_abs = 1475,
            .mic1_peak = 6470,
            .mic2_avg_abs = 1473,
            .mic2_peak = 8380,
            .diff_avg_abs = 504,
            .diff_peak = 2253,
            .diff_energy_q15 = 3534,
            .balance_q15 = 32723,
            .ref_corr_q15 = 4179,
        },
        .expected = false,
    },
    {
        .name = "echo_candidate_rejected",
        .features = {
            .seed_evidence = true,
            .ref_active = true,
            .echo_candidate = true,
            .voice_like = true,
            .mic1_avg_abs = 2000,
            .mic1_peak = 8000,
            .mic2_avg_abs = 1000,
            .mic2_peak = 4000,
            .diff_avg_abs = 800,
            .diff_peak = 3500,
            .diff_energy_q15 = 8000,
            .balance_q15 = 20000,
            .ref_corr_q15 = 4000,
        },
        .expected = false,
    },
    {
        .name = "reference_correlation_too_high",
        .features = {
            .seed_evidence = true,
            .ref_active = true,
            .voice_like = true,
            .mic1_avg_abs = 2000,
            .mic1_peak = 8000,
            .mic2_avg_abs = 1000,
            .mic2_peak = 4000,
            .diff_avg_abs = 800,
            .diff_peak = 3500,
            .diff_energy_q15 = 8000,
            .balance_q15 = 20000,
            .ref_corr_q15 = 6001,
        },
        .expected = false,
    },
    {
        .name = "single_mic_impulse_rejected",
        .features = {
            .seed_evidence = true,
            .ref_active = true,
            .voice_like = true,
            .mic1_avg_abs = 5000,
            .mic1_peak = 20000,
            .mic2_avg_abs = 100,
            .mic2_peak = 500,
            .diff_avg_abs = 4500,
            .diff_peak = 19000,
            .diff_energy_q15 = 30000,
            .balance_q15 = 1000,
            .ref_corr_q15 = 1000,
        },
        .expected = false,
    },
    {
        .name = "weak_differential_energy_rejected",
        .features = {
            .seed_evidence = true,
            .ref_active = true,
            .voice_like = true,
            .mic1_avg_abs = 2000,
            .mic1_peak = 8000,
            .mic2_avg_abs = 1000,
            .mic2_peak = 4000,
            .diff_avg_abs = 800,
            .diff_peak = 3500,
            .diff_energy_q15 = 5242,
            .balance_q15 = 20000,
            .ref_corr_q15 = 4000,
        },
        .expected = false,
    },
    {
        .name = "exact_thresholds_pass",
        .features = {
            .seed_evidence = true,
            .ref_active = true,
            .voice_like = true,
            .mic1_avg_abs = 1400,
            .mic1_peak = 6000,
            .mic2_avg_abs = 600,
            .mic2_peak = 2000,
            .diff_avg_abs = 600,
            .diff_peak = 2500,
            .diff_energy_q15 = 5243,
            .balance_q15 = 25000,
            .ref_corr_q15 = 6000,
        },
        .expected = true,
    },
    {
        .name = "symmetric_exact_thresholds_pass_without_voice_like",
        .features = {
            .seed_evidence = true,
            .ref_active = true,
            .mic1_avg_abs = 1200,
            .mic1_peak = 5200,
            .mic2_avg_abs = 1000,
            .mic2_peak = 4500,
            .diff_avg_abs = 418,
            .diff_peak = 1300,
            .diff_energy_q15 = 3500,
            .balance_q15 = 28000,
            .ref_corr_q15 = 6500,
        },
        .expected = true,
    },
    {
        .name = "symmetric_balance_below_boundary_rejected",
        .features = {
            .seed_evidence = true,
            .ref_active = true,
            .mic1_avg_abs = 1200,
            .mic1_peak = 5200,
            .mic2_avg_abs = 1000,
            .mic2_peak = 4500,
            .diff_avg_abs = 418,
            .diff_peak = 1300,
            .diff_energy_q15 = 3500,
            .balance_q15 = 27999,
            .ref_corr_q15 = 8000,
        },
        .expected = false,
    },
    {
        .name = "symmetric_reference_correlation_too_high",
        .features = {
            .seed_evidence = true,
            .ref_active = true,
            .mic1_avg_abs = 1200,
            .mic1_peak = 5200,
            .mic2_avg_abs = 1000,
            .mic2_peak = 4500,
            .diff_avg_abs = 418,
            .diff_peak = 1300,
            .diff_energy_q15 = 3500,
            .balance_q15 = 28000,
            .ref_corr_q15 = 8001,
        },
        .expected = false,
    },
    {
        .name = "seed_evidence_required",
        .features = {
            .ref_active = true,
            .voice_like = true,
            .mic1_avg_abs = 2000,
            .mic1_peak = 8000,
            .mic2_avg_abs = 1000,
            .mic2_peak = 4000,
            .diff_avg_abs = 800,
            .diff_peak = 3500,
            .diff_energy_q15 = 8000,
            .balance_q15 = 20000,
            .ref_corr_q15 = 4000,
        },
        .expected = false,
    },
    {
        .name = "verified_low_volume_silent_reference_reachable",
        .features = {
            .seed_evidence = true,
            .silent_reference_expected = true,
            .voice_like = true,
            .mic1_avg_abs = 1400,
            .mic1_peak = 6000,
            .mic2_avg_abs = 600,
            .mic2_peak = 2000,
            .diff_avg_abs = 600,
            .diff_peak = 2500,
            .diff_energy_q15 = 5243,
            .balance_q15 = 25000,
            .ref_corr_q15 = 0,
        },
        .expected = true,
    },
    {
        .name = "inactive_reference_without_silent_contract_rejected",
        .features = {
            .seed_evidence = true,
            .voice_like = true,
            .mic1_avg_abs = 1400,
            .mic1_peak = 6000,
            .mic2_avg_abs = 600,
            .mic2_peak = 2000,
            .diff_avg_abs = 600,
            .diff_peak = 2500,
            .diff_energy_q15 = 5243,
            .balance_q15 = 25000,
            .ref_corr_q15 = 0,
        },
        .expected = false,
    },
};

static int check_tracker(void) {
    int failures = 0;
    nuwax_barge_in_nearfield_tracker_t tracker = {0};

    const bool first = nuwax_barge_in_nearfield_tracker_update(&tracker, true, 100);
    const bool second = nuwax_barge_in_nearfield_tracker_update(&tracker, true, 120);
    printf("[%s] two_candidates_within_window first=%d second=%d\n",
           !first && second ? "PASS" : "FAIL",
           first ? 1 : 0,
           second ? 1 : 0);
    failures += first || !second;
    failures += tracker.first_candidate_sequence != 100U;

    tracker = (nuwax_barge_in_nearfield_tracker_t){0};
    const bool isolated = nuwax_barge_in_nearfield_tracker_update(&tracker, true, 100);
    const bool expired = nuwax_barge_in_nearfield_tracker_update(&tracker, true, 165);
    printf("[%s] isolated_candidates_expire first=%d second=%d\n",
           !isolated && !expired ? "PASS" : "FAIL",
           isolated ? 1 : 0,
           expired ? 1 : 0);
    failures += isolated || expired;
    failures += tracker.first_candidate_sequence != 165U;

    tracker = (nuwax_barge_in_nearfield_tracker_t){0};
    (void)nuwax_barge_in_nearfield_tracker_update(&tracker, true, UINT32_MAX - 10U);
    const bool wrapped = nuwax_barge_in_nearfield_tracker_update(&tracker, true, 5U);
    printf("[%s] sequence_wrap_confirmed result=%d\n",
           wrapped ? "PASS" : "FAIL",
           wrapped ? 1 : 0);
    failures += !wrapped;
    failures += tracker.first_candidate_sequence != UINT32_MAX - 10U;

    return failures;
}

static int check_symmetric_tracker(void) {
    int failures = 0;
    nuwax_barge_in_nearfield_tracker_t tracker = {0};

    const bool first = nuwax_barge_in_symmetric_tracker_update(&tracker, true, 100);
    const bool second = nuwax_barge_in_symmetric_tracker_update(&tracker, true, 101);
    const bool third = nuwax_barge_in_symmetric_tracker_update(&tracker, true, 102);
    printf("[%s] symmetric_three_contiguous_candidates first=%d second=%d third=%d\n",
           !first && !second && third ? "PASS" : "FAIL",
           first ? 1 : 0,
           second ? 1 : 0,
           third ? 1 : 0);
    failures += first || second || !third;

    tracker = (nuwax_barge_in_nearfield_tracker_t){0};
    const bool isolated = nuwax_barge_in_symmetric_tracker_update(&tracker, true, 100);
    const bool expired = nuwax_barge_in_symmetric_tracker_update(&tracker, true, 105);
    printf("[%s] symmetric_candidate_expires_after_short_window first=%d second=%d\n",
           !isolated && !expired ? "PASS" : "FAIL",
           isolated ? 1 : 0,
           expired ? 1 : 0);
    failures += isolated || expired;
    failures += tracker.first_candidate_sequence != 105U;

    nuwax_barge_in_nearfield_tracker_t imbalanced_tracker = {0};
    nuwax_barge_in_nearfield_tracker_t symmetric_tracker = {0};
    const bool imbalanced_first =
        nuwax_barge_in_nearfield_tracker_update(&imbalanced_tracker, true, 200);
    const bool symmetric_first =
        nuwax_barge_in_symmetric_tracker_update(&symmetric_tracker, true, 201);
    printf("[%s] independent_paths_do_not_cross_confirm imbalanced=%d symmetric=%d\n",
           !imbalanced_first && !symmetric_first ? "PASS" : "FAIL",
           imbalanced_first ? 1 : 0,
           symmetric_first ? 1 : 0);
    failures += imbalanced_first || symmetric_first;

    tracker = (nuwax_barge_in_nearfield_tracker_t){0};
    const bool wrap_first =
        nuwax_barge_in_symmetric_tracker_update(&tracker, true, UINT32_MAX - 1U);
    const bool wrap_second =
        nuwax_barge_in_symmetric_tracker_update(&tracker, true, UINT32_MAX);
    const bool wrapped = nuwax_barge_in_symmetric_tracker_update(&tracker, true, 0U);
    printf("[%s] symmetric_sequence_wrap_confirmed results=%d/%d/%d\n",
           !wrap_first && !wrap_second && wrapped ? "PASS" : "FAIL",
           wrap_first ? 1 : 0,
           wrap_second ? 1 : 0,
           wrapped ? 1 : 0);
    failures += wrap_first || wrap_second || !wrapped;

    tracker = (nuwax_barge_in_nearfield_tracker_t){0};
    const bool before_reset =
        nuwax_barge_in_symmetric_tracker_update(&tracker, true, 300);
    nuwax_barge_in_nearfield_tracker_reset(&tracker);
    const bool after_reset =
        nuwax_barge_in_symmetric_tracker_update(&tracker, true, 301);
    printf("[%s] playback_inactive_reset_discards_old_candidate before=%d after=%d\n",
           !before_reset && !after_reset && tracker.candidate_count == 1U
               ? "PASS"
               : "FAIL",
           before_reset ? 1 : 0,
           after_reset ? 1 : 0);
    failures += before_reset || after_reset || tracker.candidate_count != 1U;

    return failures;
}
static int check_symmetric_continuity_tracker(void) {
    int failures = 0;
    nuwax_barge_in_nearfield_tracker_t tracker = {0};

    const bool onset = nuwax_barge_in_symmetric_tracker_update_with_support(
        &tracker, true, true, 112U);
    const bool support1 = nuwax_barge_in_symmetric_tracker_update_with_support(
        &tracker, false, true, 113U);
    const bool support2 = nuwax_barge_in_symmetric_tracker_update_with_support(
        &tracker, false, true, 114U);
    printf("[%s] strict_plus_two_contiguous_support onset=%d support1=%d support2=%d\n",
           !onset && !support1 && support2 ? "PASS" : "FAIL",
           onset ? 1 : 0,
           support1 ? 1 : 0,
           support2 ? 1 : 0);
    failures += onset || support1 || !support2;
    failures += tracker.first_candidate_sequence != 112U;
    const bool repeated_support =
        nuwax_barge_in_symmetric_tracker_update_with_support(
            &tracker, false, true, 115U);
    printf("[%s] supported_confirmation_is_one_shot repeated=%d\n",
           !repeated_support && !tracker.have_candidate ? "PASS" : "FAIL",
           repeated_support ? 1 : 0);
    failures += repeated_support || tracker.have_candidate;


    tracker = (nuwax_barge_in_nearfield_tracker_t){0};
    bool isolated_confirmed = nuwax_barge_in_symmetric_tracker_update_with_support(
        &tracker, true, true, 200U);
    for (uint32_t sequence = 201U; sequence <= 205U; sequence++) {
        isolated_confirmed |= nuwax_barge_in_symmetric_tracker_update_with_support(
            &tracker, false, false, sequence);
    }
    printf("[%s] nohuman_isolated_strict_candidate_rejected confirmed=%d\n",
           !isolated_confirmed && !tracker.have_candidate ? "PASS" : "FAIL",
           isolated_confirmed ? 1 : 0);
    failures += isolated_confirmed || tracker.have_candidate;

    tracker = (nuwax_barge_in_nearfield_tracker_t){0};
    const bool interrupted_onset =
        nuwax_barge_in_symmetric_tracker_update_with_support(
            &tracker, true, true, 300U);
    const bool interrupted_support1 =
        nuwax_barge_in_symmetric_tracker_update_with_support(
            &tracker, false, true, 301U);
    const bool interrupted_gap =
        nuwax_barge_in_symmetric_tracker_update_with_support(
            &tracker, false, false, 302U);
    const bool interrupted_support2 =
        nuwax_barge_in_symmetric_tracker_update_with_support(
            &tracker, false, true, 303U);
    printf("[%s] interrupted_support_run_rejected results=%d/%d/%d/%d\n",
           !interrupted_onset && !interrupted_support1 && !interrupted_gap &&
                   !interrupted_support2 && !tracker.continuity_armed
               ? "PASS"
               : "FAIL",
           interrupted_onset ? 1 : 0,
           interrupted_support1 ? 1 : 0,
           interrupted_gap ? 1 : 0,
           interrupted_support2 ? 1 : 0);
    failures += interrupted_onset || interrupted_support1 || interrupted_gap ||
                interrupted_support2 || tracker.continuity_armed;

    tracker = (nuwax_barge_in_nearfield_tracker_t){0};
    const bool unarmed_support1 =
        nuwax_barge_in_symmetric_tracker_update_with_support(
            &tracker, false, true, 400U);
    const bool unarmed_support2 =
        nuwax_barge_in_symmetric_tracker_update_with_support(
            &tracker, false, true, 401U);
    printf("[%s] support_without_strict_onset_rejected results=%d/%d\n",
           !unarmed_support1 && !unarmed_support2 ? "PASS" : "FAIL",
           unarmed_support1 ? 1 : 0,
           unarmed_support2 ? 1 : 0);
    failures += unarmed_support1 || unarmed_support2;

    tracker = (nuwax_barge_in_nearfield_tracker_t){0};
    const bool strict1 = nuwax_barge_in_symmetric_tracker_update_with_support(
        &tracker, true, true, 500U);
    const bool strict2 = nuwax_barge_in_symmetric_tracker_update_with_support(
        &tracker, true, true, 501U);
    const bool final_support = nuwax_barge_in_symmetric_tracker_update_with_support(
        &tracker, false, true, 502U);
    printf("[%s] two_strict_frames_still_require_third_support results=%d/%d/%d\n",
           !strict1 && !strict2 && final_support ? "PASS" : "FAIL",
           strict1 ? 1 : 0,
           strict2 ? 1 : 0,
           final_support ? 1 : 0);
    failures += strict1 || strict2 || !final_support;

    tracker = (nuwax_barge_in_nearfield_tracker_t){0};
    const bool wrap_onset = nuwax_barge_in_symmetric_tracker_update_with_support(
        &tracker, true, true, UINT32_MAX - 1U);
    const bool wrap_support1 = nuwax_barge_in_symmetric_tracker_update_with_support(
        &tracker, false, true, UINT32_MAX);
    const bool wrap_support2 = nuwax_barge_in_symmetric_tracker_update_with_support(
        &tracker, false, true, 0U);
    printf("[%s] contiguous_support_survives_sequence_wrap results=%d/%d/%d\n",
           !wrap_onset && !wrap_support1 && wrap_support2 ? "PASS" : "FAIL",
           wrap_onset ? 1 : 0,
           wrap_support1 ? 1 : 0,
           wrap_support2 ? 1 : 0);
    failures += wrap_onset || wrap_support1 || !wrap_support2;

    tracker = (nuwax_barge_in_nearfield_tracker_t){0};
    (void)nuwax_barge_in_symmetric_tracker_update_with_support(
        &tracker, true, true, 600U);
    nuwax_barge_in_nearfield_tracker_reset(&tracker);
    const bool after_reset_support =
        nuwax_barge_in_symmetric_tracker_update_with_support(
            &tracker, false, true, 601U);
    printf("[%s] reset_discards_continuity_arm result=%d\n",
           !after_reset_support && !tracker.continuity_armed ? "PASS" : "FAIL",
           after_reset_support ? 1 : 0);
    failures += after_reset_support || tracker.continuity_armed;

    return failures;
}


static int check_cloud_guarded_high_recall_policy(void) {
    const bool symmetric_enabled =
        nuwax_barge_in_cloud_guarded_high_recall_anchor(
            NUWAX_BARGE_IN_NEARFIELD_SYMMETRIC_HIGH_ENERGY,
            true);
    const bool imbalanced_enabled =
        nuwax_barge_in_cloud_guarded_high_recall_anchor(
            NUWAX_BARGE_IN_NEARFIELD_IMBALANCED,
            true);
    const bool none_rejected =
        nuwax_barge_in_cloud_guarded_high_recall_anchor(
            NUWAX_BARGE_IN_NEARFIELD_NONE,
            true);
    const bool disabled_rejected =
        nuwax_barge_in_cloud_guarded_high_recall_anchor(
            NUWAX_BARGE_IN_NEARFIELD_SYMMETRIC_HIGH_ENERGY,
            false);
    const bool passed = symmetric_enabled && imbalanced_enabled &&
                        !none_rejected && !disabled_rejected;
    printf("[%s] cloud_guarded_high_recall_policy symmetric=%d imbalanced=%d none=%d disabled=%d\n",
           passed ? "PASS" : "FAIL",
           symmetric_enabled ? 1 : 0,
           imbalanced_enabled ? 1 : 0,
           none_rejected ? 1 : 0,
           disabled_rejected ? 1 : 0);
    return passed ? 0 : 1;
}

int main(void) {
    int failures = 0;
    const size_t count = sizeof(s_cases) / sizeof(s_cases[0]);
    for (size_t i = 0; i < count; i++) {
        const bool actual = nuwax_barge_in_nearfield_anchor(&s_cases[i].features);
        printf("[%s] %s actual=%d expected=%d\n",
               actual == s_cases[i].expected ? "PASS" : "FAIL",
               s_cases[i].name,
               actual ? 1 : 0,
               s_cases[i].expected ? 1 : 0);
        if (actual != s_cases[i].expected) {
            failures++;
        }
    }
    failures += check_tracker();
    failures += check_symmetric_tracker();
    failures += check_symmetric_continuity_tracker();
    failures += check_cloud_guarded_high_recall_policy();
    printf("barge-in detector host check: %s (%d failures, %u cases)\n",
           failures == 0 ? "PASS" : "FAIL",
           failures,
           (unsigned)count);
    return failures == 0 ? 0 : 1;
}
