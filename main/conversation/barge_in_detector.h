#pragma once

#include <stdbool.h>
#include <stdint.h>

#define NUWAX_BARGE_IN_NEARFIELD_CONFIRM_COUNT 2U
#define NUWAX_BARGE_IN_NEARFIELD_CONFIRM_WINDOW_FRAMES 64U
#define NUWAX_BARGE_IN_SYMMETRIC_CONFIRM_COUNT 3U
#define NUWAX_BARGE_IN_SYMMETRIC_CONFIRM_WINDOW_FRAMES 4U
#define NUWAX_BARGE_IN_SYMMETRIC_SUPPORT_CONFIRM_COUNT 2U

typedef enum {
    NUWAX_BARGE_IN_NEARFIELD_NONE = 0,
    NUWAX_BARGE_IN_NEARFIELD_IMBALANCED,
    NUWAX_BARGE_IN_NEARFIELD_SYMMETRIC_HIGH_ENERGY,
} nuwax_barge_in_nearfield_kind_t;

typedef struct {
    bool seed_evidence;
    bool ref_active;
    bool silent_reference_expected;
    bool echo_candidate;
    bool voice_like;
    uint32_t mic1_avg_abs;
    uint32_t mic1_peak;
    uint32_t mic2_avg_abs;
    uint32_t mic2_peak;
    uint32_t diff_avg_abs;
    uint32_t diff_peak;
    uint32_t diff_energy_q15;
    uint32_t balance_q15;
    uint32_t ref_corr_q15;
} nuwax_barge_in_nearfield_features_t;

typedef struct {
    bool have_candidate;
    uint32_t first_candidate_sequence;
    uint32_t last_candidate_sequence;
    uint32_t candidate_count;
    bool continuity_armed;
    uint32_t continuity_support_count;
    uint32_t last_continuity_sequence;
} nuwax_barge_in_nearfield_tracker_t;

bool nuwax_barge_in_nearfield_anchor(
    const nuwax_barge_in_nearfield_features_t *features);

nuwax_barge_in_nearfield_kind_t nuwax_barge_in_nearfield_classify(
    const nuwax_barge_in_nearfield_features_t *features);

bool nuwax_barge_in_cloud_guarded_high_recall_anchor(
    nuwax_barge_in_nearfield_kind_t kind,
    bool enabled);

bool nuwax_barge_in_nearfield_tracker_update(
    nuwax_barge_in_nearfield_tracker_t *tracker,
    bool candidate,
    uint32_t sequence);

bool nuwax_barge_in_symmetric_tracker_update(
    nuwax_barge_in_nearfield_tracker_t *tracker,
    bool candidate,
    uint32_t sequence);

bool nuwax_barge_in_symmetric_tracker_update_with_support(
    nuwax_barge_in_nearfield_tracker_t *tracker,
    bool strict_candidate,
    bool current_frame_support,
    uint32_t sequence);

void nuwax_barge_in_nearfield_tracker_reset(
    nuwax_barge_in_nearfield_tracker_t *tracker);
