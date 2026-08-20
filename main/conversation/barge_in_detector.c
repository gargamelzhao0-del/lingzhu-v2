#include "barge_in_detector.h"

#define NUWAX_BARGE_NEARFIELD_MIN_MIC1_AVG_ABS 1400U
#define NUWAX_BARGE_NEARFIELD_MIN_MIC1_PEAK 6000U
#define NUWAX_BARGE_NEARFIELD_MIN_MIC2_AVG_ABS 600U
#define NUWAX_BARGE_NEARFIELD_MIN_MIC2_PEAK 2000U
#define NUWAX_BARGE_NEARFIELD_MIN_DIFF_AVG_ABS 600U
#define NUWAX_BARGE_NEARFIELD_MIN_DIFF_PEAK 2500U
#define NUWAX_BARGE_NEARFIELD_MIN_DIFF_ENERGY_Q15 5243U
#define NUWAX_BARGE_NEARFIELD_MAX_BALANCE_Q15 25000U
#define NUWAX_BARGE_NEARFIELD_MAX_REF_CORR_Q15 6000U

#define NUWAX_BARGE_SYMMETRIC_MIN_MIC1_AVG_ABS 1200U
#define NUWAX_BARGE_SYMMETRIC_MIN_MIC1_PEAK 5200U
#define NUWAX_BARGE_SYMMETRIC_MIN_MIC2_AVG_ABS 1000U
#define NUWAX_BARGE_SYMMETRIC_MIN_MIC2_PEAK 4500U
#define NUWAX_BARGE_SYMMETRIC_MIN_DIFF_AVG_ABS 400U
#define NUWAX_BARGE_SYMMETRIC_MIN_DIFF_PEAK 1300U
#define NUWAX_BARGE_SYMMETRIC_MIN_DIFF_ENERGY_Q15 3500U
#define NUWAX_BARGE_SYMMETRIC_MIN_DIFF_TO_MIC1_AVG_Q15 11400U
#define NUWAX_BARGE_SYMMETRIC_MIN_BALANCE_Q15 28000U
#define NUWAX_BARGE_SYMMETRIC_MAX_BALANCE_Q15 32768U
#define NUWAX_BARGE_SYMMETRIC_MAX_REF_CORR_Q15 6500U
#define NUWAX_BARGE_SYMMETRIC_HIGH_CORR_MAX_REF_CORR_Q15 8000U
#define NUWAX_BARGE_SYMMETRIC_HIGH_CORR_MIN_DIFF_ENERGY_Q15 6000U
#define NUWAX_BARGE_SYMMETRIC_HIGH_CORR_MIN_DIFF_TO_MIC1_AVG_Q15 14500U

nuwax_barge_in_nearfield_kind_t nuwax_barge_in_nearfield_classify(
    const nuwax_barge_in_nearfield_features_t *features) {
    if (!features ||
        !features->seed_evidence ||
        (!features->ref_active && !features->silent_reference_expected) ||
        features->echo_candidate) {
        return NUWAX_BARGE_IN_NEARFIELD_NONE;
    }

    const bool imbalanced_nearfield =
        features->voice_like &&
        features->mic1_avg_abs >= NUWAX_BARGE_NEARFIELD_MIN_MIC1_AVG_ABS &&
        features->mic1_peak >= NUWAX_BARGE_NEARFIELD_MIN_MIC1_PEAK &&
        features->mic2_avg_abs >= NUWAX_BARGE_NEARFIELD_MIN_MIC2_AVG_ABS &&
        features->mic2_peak >= NUWAX_BARGE_NEARFIELD_MIN_MIC2_PEAK &&
        features->diff_avg_abs >= NUWAX_BARGE_NEARFIELD_MIN_DIFF_AVG_ABS &&
        features->diff_peak >= NUWAX_BARGE_NEARFIELD_MIN_DIFF_PEAK &&
        features->diff_energy_q15 >= NUWAX_BARGE_NEARFIELD_MIN_DIFF_ENERGY_Q15 &&
        features->balance_q15 > 0 &&
        features->balance_q15 <= NUWAX_BARGE_NEARFIELD_MAX_BALANCE_Q15 &&
        features->ref_corr_q15 <= NUWAX_BARGE_NEARFIELD_MAX_REF_CORR_Q15;

    /* A user directly in front of the device can reach both microphones at
     * nearly the same level. In that geometry the generic voice_like test is
     * false, so admit only high-energy, low-reference-correlation evidence. */
    const bool symmetric_reference_separated =
        features->ref_corr_q15 <= NUWAX_BARGE_SYMMETRIC_MAX_REF_CORR_Q15 ||
        (features->ref_corr_q15 <=
             NUWAX_BARGE_SYMMETRIC_HIGH_CORR_MAX_REF_CORR_Q15 &&
         features->diff_energy_q15 >=
             NUWAX_BARGE_SYMMETRIC_HIGH_CORR_MIN_DIFF_ENERGY_Q15 &&
         (uint64_t)features->diff_avg_abs * 32768ULL >=
             (uint64_t)features->mic1_avg_abs *
                 NUWAX_BARGE_SYMMETRIC_HIGH_CORR_MIN_DIFF_TO_MIC1_AVG_Q15);
    const bool symmetric_high_energy_nearfield =
        features->mic1_avg_abs >= NUWAX_BARGE_SYMMETRIC_MIN_MIC1_AVG_ABS &&
        features->mic1_peak >= NUWAX_BARGE_SYMMETRIC_MIN_MIC1_PEAK &&
        features->mic2_avg_abs >= NUWAX_BARGE_SYMMETRIC_MIN_MIC2_AVG_ABS &&
        features->mic2_peak >= NUWAX_BARGE_SYMMETRIC_MIN_MIC2_PEAK &&
        features->diff_avg_abs >= NUWAX_BARGE_SYMMETRIC_MIN_DIFF_AVG_ABS &&
        features->diff_peak >= NUWAX_BARGE_SYMMETRIC_MIN_DIFF_PEAK &&
        features->diff_energy_q15 >= NUWAX_BARGE_SYMMETRIC_MIN_DIFF_ENERGY_Q15 &&
        (uint64_t)features->diff_avg_abs * 32768ULL >=
            (uint64_t)features->mic1_avg_abs *
                NUWAX_BARGE_SYMMETRIC_MIN_DIFF_TO_MIC1_AVG_Q15 &&
        features->balance_q15 >= NUWAX_BARGE_SYMMETRIC_MIN_BALANCE_Q15 &&
        features->balance_q15 <= NUWAX_BARGE_SYMMETRIC_MAX_BALANCE_Q15 &&
        symmetric_reference_separated;

    if (imbalanced_nearfield) {
        return NUWAX_BARGE_IN_NEARFIELD_IMBALANCED;
    }
    if (symmetric_high_energy_nearfield) {
        return NUWAX_BARGE_IN_NEARFIELD_SYMMETRIC_HIGH_ENERGY;
    }
    return NUWAX_BARGE_IN_NEARFIELD_NONE;
}

bool nuwax_barge_in_cloud_guarded_high_recall_anchor(
    nuwax_barge_in_nearfield_kind_t kind,
    bool enabled) {
    return enabled && kind != NUWAX_BARGE_IN_NEARFIELD_NONE;
}

bool nuwax_barge_in_nearfield_anchor(
    const nuwax_barge_in_nearfield_features_t *features) {
    return nuwax_barge_in_nearfield_classify(features) !=
           NUWAX_BARGE_IN_NEARFIELD_NONE;
}

static bool tracker_update_with_policy(
    nuwax_barge_in_nearfield_tracker_t *tracker,
    bool candidate,
    uint32_t sequence,
    uint32_t confirm_count,
    uint32_t confirm_window_frames) {
    if (!tracker) {
        return false;
    }

    if (!candidate) {
        if (tracker->have_candidate &&
            sequence - tracker->last_candidate_sequence >
                confirm_window_frames) {
            tracker->have_candidate = false;
            tracker->first_candidate_sequence = 0;
            tracker->candidate_count = 0;
        }
        return false;
    }

    if (!tracker->have_candidate ||
        sequence - tracker->last_candidate_sequence >
            confirm_window_frames) {
        tracker->first_candidate_sequence = sequence;
        tracker->candidate_count = 1;
    } else if (tracker->candidate_count < UINT32_MAX) {
        tracker->candidate_count++;
    }
    tracker->have_candidate = true;
    tracker->last_candidate_sequence = sequence;
    return tracker->candidate_count >= confirm_count;
}

bool nuwax_barge_in_nearfield_tracker_update(
    nuwax_barge_in_nearfield_tracker_t *tracker,
    bool candidate,
    uint32_t sequence) {
    return tracker_update_with_policy(
        tracker,
        candidate,
        sequence,
        NUWAX_BARGE_IN_NEARFIELD_CONFIRM_COUNT,
        NUWAX_BARGE_IN_NEARFIELD_CONFIRM_WINDOW_FRAMES);
}

bool nuwax_barge_in_symmetric_tracker_update(
    nuwax_barge_in_nearfield_tracker_t *tracker,
    bool candidate,
    uint32_t sequence) {
    return nuwax_barge_in_symmetric_tracker_update_with_support(
        tracker, candidate, candidate, sequence);
}
bool nuwax_barge_in_symmetric_tracker_update_with_support(
    nuwax_barge_in_nearfield_tracker_t *tracker,
    bool strict_candidate,
    bool current_frame_support,
    uint32_t sequence) {
    if (!tracker) {
        return false;
    }

    if (!tracker->continuity_armed) {
        if (strict_candidate) {
            tracker->have_candidate = true;
            tracker->first_candidate_sequence = sequence;
            tracker->last_candidate_sequence = sequence;
            tracker->candidate_count = 1;
            tracker->continuity_armed = true;
            tracker->continuity_support_count = 0;
            tracker->last_continuity_sequence = sequence;
        }
        return false;
    }

    const bool next_frame =
        sequence - tracker->last_continuity_sequence == 1U;
    if (!current_frame_support || !next_frame) {
        tracker->have_candidate = false;
        tracker->first_candidate_sequence = 0;
        tracker->last_candidate_sequence = 0;
        tracker->candidate_count = 0;
        tracker->continuity_armed = false;
        tracker->continuity_support_count = 0;
        tracker->last_continuity_sequence = 0;
        if (strict_candidate) {
            tracker->have_candidate = true;
            tracker->first_candidate_sequence = sequence;
            tracker->last_candidate_sequence = sequence;
            tracker->candidate_count = 1;
            tracker->continuity_armed = true;
            tracker->last_continuity_sequence = sequence;
        }
        return false;
    }

    tracker->last_continuity_sequence = sequence;
    if (strict_candidate) {
        tracker->last_candidate_sequence = sequence;
        if (tracker->candidate_count < UINT32_MAX) {
            tracker->candidate_count++;
        }
    }
    if (tracker->continuity_support_count < UINT32_MAX) {
        tracker->continuity_support_count++;
    }
    const bool continuity_confirmed =
        tracker->continuity_support_count >=
        NUWAX_BARGE_IN_SYMMETRIC_SUPPORT_CONFIRM_COUNT;
    if (continuity_confirmed) {
        tracker->have_candidate = false;
        tracker->continuity_armed = false;
    }
    return continuity_confirmed;
}


void nuwax_barge_in_nearfield_tracker_reset(
    nuwax_barge_in_nearfield_tracker_t *tracker) {
    if (tracker) {
        *tracker = (nuwax_barge_in_nearfield_tracker_t){0};
    }
}
