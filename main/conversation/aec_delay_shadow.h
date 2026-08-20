#ifndef NUWAX_AEC_DELAY_SHADOW_H
#define NUWAX_AEC_DELAY_SHADOW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Maximum number of delays evaluated by one sampled frame. The state owns
 * all scratch storage so callers can place it in PSRAM and keep task stacks
 * small.
 */
#define NUWAX_AEC_DELAY_SHADOW_MAX_CANDIDATES 416U
#define NUWAX_AEC_DELAY_SHADOW_MAX_TRACKED_DELAY_SPAN 4096U
#define NUWAX_AEC_DELAY_SHADOW_INVALID_DELAY UINT32_MAX

typedef enum {
    NUWAX_AEC_DELAY_SHADOW_OK = 0,
    NUWAX_AEC_DELAY_SHADOW_SKIPPED = 1,
    NUWAX_AEC_DELAY_SHADOW_INVALID_ARGUMENT = -1,
    NUWAX_AEC_DELAY_SHADOW_INVALID_CONFIG = -2,
    NUWAX_AEC_DELAY_SHADOW_HISTORY_TOO_SHORT = -3,
} nuwax_aec_delay_shadow_status_t;

typedef struct {
    uint32_t min_delay_samples;
    uint32_t max_delay_samples;
    uint32_t coarse_step_samples;
    uint32_t fine_radius_samples;
    uint32_t cadence_frames;
    uint32_t sample_stride;
    uint32_t consensus_window_frames;

    size_t minimum_sample_pairs;
    float minimum_mic_diff_rms;
    float minimum_ref_diff_rms;
    float minimum_abs_correlation;
    float minimum_peak_margin;

    uint32_t second_peak_exclusion_samples;
    uint32_t stability_tolerance_samples;
    uint32_t minimum_stability_updates;
    float stability_alpha;
    float minimum_stability_confidence;
} nuwax_aec_delay_shadow_config_t;

typedef struct {
    bool sampled;
    bool estimate_ready;
    bool generation_reset;
    bool accepted;

    uint32_t generation;
    uint32_t frame_index;
    uint32_t sampled_frame_index;
    uint32_t evaluated_candidates;
    size_t sample_pairs;

    uint32_t best_delay_samples;
    float best_correlation;
    float best_abs_correlation;
    uint32_t second_delay_samples;
    float second_correlation;
    float second_abs_correlation;
    float peak_margin;

    float mic_diff_rms;
    float ref_diff_rms;
    float candidate_confidence;
    float sample_confidence;
    float energy_confidence;

    uint32_t stable_delay_samples;
    float stable_delay_samples_f;
    uint32_t stability_updates;
    float stability_confidence;
    float overall_confidence;
} nuwax_aec_delay_shadow_result_t;

typedef struct {
    nuwax_aec_delay_shadow_config_t config;
    bool initialized;
    bool has_stable_delay;
    uint32_t generation;
    uint32_t frame_count;
    uint32_t sampled_frame_count;
    uint32_t stability_updates;
    float stable_delay_samples;
    float stability_ewma;
    uint32_t consensus_frame_count;
    float consensus_mic_rms_sum;
    float consensus_correlation_sum[NUWAX_AEC_DELAY_SHADOW_MAX_TRACKED_DELAY_SPAN + 1U];
    float consensus_ref_rms_sum[NUWAX_AEC_DELAY_SHADOW_MAX_TRACKED_DELAY_SPAN + 1U];
    uint16_t consensus_observations[NUWAX_AEC_DELAY_SHADOW_MAX_TRACKED_DELAY_SPAN + 1U];


    uint32_t candidate_count;
    uint16_t candidate_observations[NUWAX_AEC_DELAY_SHADOW_MAX_CANDIDATES];
    uint32_t candidate_delays[NUWAX_AEC_DELAY_SHADOW_MAX_CANDIDATES];
    float candidate_correlations[NUWAX_AEC_DELAY_SHADOW_MAX_CANDIDATES];
    float candidate_ref_diff_rms[NUWAX_AEC_DELAY_SHADOW_MAX_CANDIDATES];
} nuwax_aec_delay_shadow_state_t;

/* A conservative starting point for a 16 kHz device-side shadow probe. */
void nuwax_aec_delay_shadow_default_config(nuwax_aec_delay_shadow_config_t *config);

nuwax_aec_delay_shadow_status_t nuwax_aec_delay_shadow_validate_config(
    const nuwax_aec_delay_shadow_config_t *config);

nuwax_aec_delay_shadow_status_t nuwax_aec_delay_shadow_init(
    nuwax_aec_delay_shadow_state_t *state,
    const nuwax_aec_delay_shadow_config_t *config,
    uint32_t generation);

/* Reset temporal consensus while preserving the validated configuration. */
void nuwax_aec_delay_shadow_reset(nuwax_aec_delay_shadow_state_t *state,
                                  uint32_t generation);

/*
 * Estimate delay without changing mic or history.
 *
 * history contains frame_samples + max_delay - min_delay samples, ordered
 * oldest to newest. For candidate delay d, its reference frame begins at:
 *
 *     history + (config.max_delay_samples - d)
 *
 * Consequently d=min_delay selects the newest frame and d=max_delay selects
 * the oldest frame. Correlation is Pearson-normalized after first
 * differencing both the microphone and reference frame.
 */
nuwax_aec_delay_shadow_status_t nuwax_aec_delay_shadow_process(
    nuwax_aec_delay_shadow_state_t *state,
    const int16_t *mic,
    size_t frame_samples,
    const int16_t *history,
    size_t history_samples,
    uint32_t generation,
    nuwax_aec_delay_shadow_result_t *result);

#ifdef __cplusplus
}
#endif

#endif
