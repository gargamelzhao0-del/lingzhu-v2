#include "aec_delay_shadow.h"

#include <math.h>
#include <string.h>

typedef struct {
    int64_t sum;
    uint64_t sum_sq;
} diff_moments_t;

static float clamp01(float value) {
    if (value <= 0.0f) {
        return 0.0f;
    }
    if (value >= 1.0f) {
        return 1.0f;
    }
    return value;
}

static uint32_t abs_distance_u32(uint32_t first, uint32_t second) {
    return first >= second ? first - second : second - first;
}

static uint32_t round_nonnegative(float value) {
    if (!(value > 0.0f)) {
        return 0;
    }
    if (value >= (float)UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t)(value + 0.5f);
}

static diff_moments_t measure_diff(const int16_t *samples,
                                   size_t sample_count,
                                   uint32_t sample_stride) {
    diff_moments_t result = {0};
    for (size_t i = 1; i < sample_count; i += sample_stride) {
        const int32_t value = (int32_t)samples[i] - (int32_t)samples[i - 1U];
        result.sum += value;
        result.sum_sq += (uint64_t)((int64_t)value * (int64_t)value);
    }
    return result;
}

static float moments_rms(diff_moments_t moments, size_t pair_count) {
    if (pair_count == 0) {
        return 0.0f;
    }
    return sqrtf((float)moments.sum_sq / (float)pair_count);
}

static float pearson_diff(const int16_t *mic,
                          const int16_t *ref,
                          size_t sample_count,
                          uint32_t sample_stride,
                          diff_moments_t mic_moments,
                          float *out_ref_rms) {
    const size_t pair_count = sample_count > 1U
                                  ? ((sample_count - 2U) / sample_stride) + 1U
                                  : 0;
    if (pair_count == 0) {
        if (out_ref_rms) {
            *out_ref_rms = 0.0f;
        }
        return 0.0f;
    }

    int64_t sum_y = 0;
    uint64_t sum_yy = 0;
    int64_t sum_xy = 0;
    for (size_t i = 1; i < sample_count; i += sample_stride) {
        const int32_t x = (int32_t)mic[i] - (int32_t)mic[i - 1U];
        const int32_t y = (int32_t)ref[i] - (int32_t)ref[i - 1U];
        sum_y += y;
        sum_yy += (uint64_t)((int64_t)y * (int64_t)y);
        sum_xy += (int64_t)x * (int64_t)y;
    }
    if (out_ref_rms) {
        *out_ref_rms = sqrtf((float)sum_yy / (float)pair_count);
    }

    const float count = (float)pair_count;
    const float covariance = (float)sum_xy -
                             ((float)mic_moments.sum * (float)sum_y) / count;
    const float variance_x = (float)mic_moments.sum_sq -
                             ((float)mic_moments.sum * (float)mic_moments.sum) / count;
    const float variance_y = (float)sum_yy - ((float)sum_y * (float)sum_y) / count;
    if (!(variance_x > 0.0f) || !(variance_y > 0.0f)) {
        return 0.0f;
    }
    float correlation = covariance / sqrtf(variance_x * variance_y);
    if (correlation > 1.0f) {
        correlation = 1.0f;
    } else if (correlation < -1.0f) {
        correlation = -1.0f;
    }
    return correlation;
}

static bool candidate_exists(const nuwax_aec_delay_shadow_state_t *state,
                             uint32_t delay) {
    for (uint32_t i = 0; i < state->candidate_count; i++) {
        if (state->candidate_delays[i] == delay) {
            return true;
        }
    }
    return false;
}

static bool append_candidate(nuwax_aec_delay_shadow_state_t *state,
                             uint32_t delay,
                             const int16_t *mic,
                             size_t frame_samples,
                             const int16_t *history,
                             diff_moments_t mic_moments) {
    if (state->candidate_count >= NUWAX_AEC_DELAY_SHADOW_MAX_CANDIDATES ||
        candidate_exists(state, delay)) {
        return false;
    }
    const uint32_t index = state->candidate_count++;
    const size_t offset = (size_t)(state->config.max_delay_samples - delay);
    float ref_rms = 0.0f;
    const float correlation = pearson_diff(mic,
                                           history + offset,
                                           frame_samples,
                                           state->config.sample_stride,
                                           mic_moments,
                                           &ref_rms);
    state->candidate_delays[index] = delay;
    state->candidate_correlations[index] = correlation;
    state->candidate_ref_diff_rms[index] = ref_rms;
    return true;
}

static uint32_t best_candidate_index(const nuwax_aec_delay_shadow_state_t *state) {
    uint32_t best = 0;
    float best_abs = -1.0f;
    for (uint32_t i = 0; i < state->candidate_count; i++) {
        const float value = fabsf(state->candidate_correlations[i]);
        if (value > best_abs) {
            best_abs = value;
            best = i;
        }
    }
    return best;
}

static void clear_consensus(nuwax_aec_delay_shadow_state_t *state) {
    memset(state->consensus_correlation_sum,
           0,
           sizeof(state->consensus_correlation_sum));
    memset(state->consensus_ref_rms_sum,
           0,
           sizeof(state->consensus_ref_rms_sum));
    memset(state->consensus_observations,
           0,
           sizeof(state->consensus_observations));
    state->consensus_frame_count = 0;
    state->consensus_mic_rms_sum = 0.0f;
}

void nuwax_aec_delay_shadow_default_config(nuwax_aec_delay_shadow_config_t *config) {
    if (!config) {
        return;
    }
    *config = (nuwax_aec_delay_shadow_config_t){
        .min_delay_samples = 768,
        .max_delay_samples = 3072,
        .coarse_step_samples = 32,
        .fine_radius_samples = 32,
        .cadence_frames = 4,
        .sample_stride = 2,
        .consensus_window_frames = 8,
        .minimum_sample_pairs = 512,
        .minimum_mic_diff_rms = 16.0f,
        .minimum_ref_diff_rms = 16.0f,
        .minimum_abs_correlation = 0.18f,
        .minimum_peak_margin = 0.015f,
        .second_peak_exclusion_samples = 16,
        .stability_tolerance_samples = 12,
        .minimum_stability_updates = 3,
        .stability_alpha = 0.25f,
        .minimum_stability_confidence = 0.60f,
    };
}

nuwax_aec_delay_shadow_status_t nuwax_aec_delay_shadow_validate_config(
    const nuwax_aec_delay_shadow_config_t *config) {
    if (!config) {
        return NUWAX_AEC_DELAY_SHADOW_INVALID_ARGUMENT;
    }
    if (config->min_delay_samples > config->max_delay_samples ||
        config->coarse_step_samples == 0 || config->cadence_frames == 0 ||
        config->sample_stride == 0 ||
        config->consensus_window_frames == 0 ||
        config->minimum_sample_pairs == 0 || config->stability_alpha <= 0.0f ||
        config->minimum_mic_diff_rms <= 0.0f ||
        config->minimum_ref_diff_rms <= 0.0f ||
        config->stability_alpha > 1.0f || config->minimum_abs_correlation < 0.0f ||
        config->minimum_abs_correlation > 1.0f || config->minimum_peak_margin < 0.0f ||
        config->minimum_peak_margin > 1.0f ||
        config->minimum_stability_confidence < 0.0f ||
        config->minimum_stability_confidence > 1.0f ||
        config->stability_tolerance_samples == 0) {
        return NUWAX_AEC_DELAY_SHADOW_INVALID_CONFIG;
    }
    const uint64_t range = (uint64_t)config->max_delay_samples -
                           (uint64_t)config->min_delay_samples;
    if (range > NUWAX_AEC_DELAY_SHADOW_MAX_TRACKED_DELAY_SPAN) {
        return NUWAX_AEC_DELAY_SHADOW_INVALID_CONFIG;
    }
    const uint64_t coarse = range / config->coarse_step_samples + 2ULL;
    const uint64_t fine = (uint64_t)config->fine_radius_samples * 2ULL + 1ULL;
    if (coarse + fine > NUWAX_AEC_DELAY_SHADOW_MAX_CANDIDATES) {
        return NUWAX_AEC_DELAY_SHADOW_INVALID_CONFIG;
    }
    return NUWAX_AEC_DELAY_SHADOW_OK;
}

nuwax_aec_delay_shadow_status_t nuwax_aec_delay_shadow_init(
    nuwax_aec_delay_shadow_state_t *state,
    const nuwax_aec_delay_shadow_config_t *config,
    uint32_t generation) {
    if (!state || !config) {
        return NUWAX_AEC_DELAY_SHADOW_INVALID_ARGUMENT;
    }
    const nuwax_aec_delay_shadow_status_t status =
        nuwax_aec_delay_shadow_validate_config(config);
    if (status != NUWAX_AEC_DELAY_SHADOW_OK) {
        return status;
    }
    memset(state, 0, sizeof(*state));
    state->config = *config;
    state->generation = generation;
    state->initialized = true;
    return NUWAX_AEC_DELAY_SHADOW_OK;
}

void nuwax_aec_delay_shadow_reset(nuwax_aec_delay_shadow_state_t *state,
                                  uint32_t generation) {
    if (!state || !state->initialized) {
        return;
    }
    const nuwax_aec_delay_shadow_config_t config = state->config;
    memset(state, 0, sizeof(*state));
    state->config = config;
    state->generation = generation;
    state->initialized = true;
}

nuwax_aec_delay_shadow_status_t nuwax_aec_delay_shadow_process(
    nuwax_aec_delay_shadow_state_t *state,
    const int16_t *mic,
    size_t frame_samples,
    const int16_t *history,
    size_t history_samples,
    uint32_t generation,
    nuwax_aec_delay_shadow_result_t *result) {
    if (result) {
        memset(result, 0, sizeof(*result));
        result->best_delay_samples = NUWAX_AEC_DELAY_SHADOW_INVALID_DELAY;
        result->second_delay_samples = NUWAX_AEC_DELAY_SHADOW_INVALID_DELAY;
        result->stable_delay_samples = NUWAX_AEC_DELAY_SHADOW_INVALID_DELAY;
    }
    if (!state || !state->initialized || !mic || !history || !result || frame_samples < 2U) {
        return NUWAX_AEC_DELAY_SHADOW_INVALID_ARGUMENT;
    }

    if (state->generation != generation) {
        nuwax_aec_delay_shadow_reset(state, generation);
        result->generation_reset = true;
    }
    const uint32_t frame_index = state->frame_count++;
    result->generation = generation;
    result->frame_index = frame_index;
    if ((frame_index % state->config.cadence_frames) != 0U) {
        if (state->has_stable_delay) {
            result->stable_delay_samples_f = state->stable_delay_samples;
            result->stable_delay_samples = round_nonnegative(state->stable_delay_samples);
            result->stability_updates = state->stability_updates;
            result->stability_confidence = state->stability_ewma;
        }
        return NUWAX_AEC_DELAY_SHADOW_SKIPPED;
    }

    const uint64_t required64 = (uint64_t)frame_samples +
                                (uint64_t)state->config.max_delay_samples -
                                (uint64_t)state->config.min_delay_samples;
    if (required64 > SIZE_MAX || history_samples < (size_t)required64) {
        return NUWAX_AEC_DELAY_SHADOW_HISTORY_TOO_SHORT;
    }
    result->sampled = true;
    result->sampled_frame_index = state->sampled_frame_count++;
    state->candidate_count = 0;

    const diff_moments_t mic_moments =
        measure_diff(mic, frame_samples, state->config.sample_stride);
    const size_t pair_count =
        ((frame_samples - 2U) / state->config.sample_stride) + 1U;
    result->sample_pairs = pair_count;
    result->mic_diff_rms = moments_rms(mic_moments, pair_count);

    uint32_t delay = state->config.min_delay_samples;
    while (true) {
        (void)append_candidate(state, delay, mic, frame_samples, history, mic_moments);
        if (delay >= state->config.max_delay_samples ||
            state->config.max_delay_samples - delay < state->config.coarse_step_samples) {
            break;
        }
        delay += state->config.coarse_step_samples;
    }
    if (delay != state->config.max_delay_samples) {
        (void)append_candidate(state,
                               state->config.max_delay_samples,
                               mic,
                               frame_samples,
                               history,
                               mic_moments);
    }

    const uint32_t coarse_best_index = best_candidate_index(state);
    const uint32_t coarse_best_delay = state->candidate_delays[coarse_best_index];
    const uint32_t fine_center = state->has_stable_delay
                                     ? round_nonnegative(state->stable_delay_samples)
                                     : coarse_best_delay;
    const uint32_t fine_start =
        fine_center > state->config.fine_radius_samples
            ? fine_center - state->config.fine_radius_samples
            : state->config.min_delay_samples;
    uint32_t bounded_fine_start = fine_start < state->config.min_delay_samples
                                      ? state->config.min_delay_samples
                                      : fine_start;
    uint32_t fine_end = fine_center + state->config.fine_radius_samples;
    if (fine_end < fine_center || fine_end > state->config.max_delay_samples) {
        fine_end = state->config.max_delay_samples;
    }
    for (delay = bounded_fine_start; delay <= fine_end; delay++) {
        (void)append_candidate(state, delay, mic, frame_samples, history, mic_moments);
        if (delay == UINT32_MAX) {
            break;
        }
    }

    bool consensus_frame_observed = false;
    if (result->mic_diff_rms >= state->config.minimum_mic_diff_rms) {
        for (uint32_t i = 0; i < state->candidate_count; i++) {
            if (state->candidate_ref_diff_rms[i] <
                state->config.minimum_ref_diff_rms) {
                continue;
            }
            const uint32_t tracked_delay = state->candidate_delays[i];
            const uint32_t tracked_index =
                tracked_delay - state->config.min_delay_samples;
            if (tracked_index > NUWAX_AEC_DELAY_SHADOW_MAX_TRACKED_DELAY_SPAN) {
                continue;
            }
            state->consensus_correlation_sum[tracked_index] +=
                fabsf(state->candidate_correlations[i]);
            state->consensus_ref_rms_sum[tracked_index] +=
                state->candidate_ref_diff_rms[i];
            if (state->consensus_observations[tracked_index] < UINT16_MAX) {
                state->consensus_observations[tracked_index]++;
            }
            consensus_frame_observed = true;
        }
    }
    if (consensus_frame_observed) {
        state->consensus_frame_count++;
        state->consensus_mic_rms_sum += result->mic_diff_rms;
    }
    if (state->consensus_frame_count < state->config.consensus_window_frames) {
        if (state->has_stable_delay) {
            result->stable_delay_samples_f = state->stable_delay_samples;
            result->stable_delay_samples = round_nonnegative(state->stable_delay_samples);
            result->stability_updates = state->stability_updates;
            result->stability_confidence = state->stability_ewma;
        }
        return NUWAX_AEC_DELAY_SHADOW_OK;
    }

    const uint32_t consensus_frames = state->consensus_frame_count;
    const uint32_t minimum_observations =
        (state->config.consensus_window_frames + 1U) / 2U;
    const uint32_t tracked_span =
        state->config.max_delay_samples - state->config.min_delay_samples;
    state->candidate_count = 0;
    for (uint32_t tracked_index = 0; tracked_index <= tracked_span; tracked_index++) {
        const uint16_t observations = state->consensus_observations[tracked_index];
        if (observations < minimum_observations ||
            state->candidate_count >= NUWAX_AEC_DELAY_SHADOW_MAX_CANDIDATES) {
            continue;
        }
        const uint32_t candidate = state->candidate_count++;
        state->candidate_delays[candidate] =
            state->config.min_delay_samples + tracked_index;
        state->candidate_correlations[candidate] =
            state->consensus_correlation_sum[tracked_index] / observations;
        state->candidate_ref_diff_rms[candidate] =
            state->consensus_ref_rms_sum[tracked_index] / observations;
        state->candidate_observations[candidate] = observations;
    }
    result->mic_diff_rms = state->consensus_mic_rms_sum / consensus_frames;
    clear_consensus(state);
    if (state->candidate_count == 0) {
        return NUWAX_AEC_DELAY_SHADOW_OK;
    }
    result->estimate_ready = true;

    const uint32_t best_index = best_candidate_index(state);
    const uint32_t best_delay = state->candidate_delays[best_index];
    const float best_corr = state->candidate_correlations[best_index];
    const float best_abs = fabsf(best_corr);
    uint32_t second_index = UINT32_MAX;
    float second_abs = -1.0f;
    for (uint32_t i = 0; i < state->candidate_count; i++) {
        if (abs_distance_u32(state->candidate_delays[i], best_delay) <=
            state->config.second_peak_exclusion_samples) {
            continue;
        }
        const float value = fabsf(state->candidate_correlations[i]);
        if (value > second_abs) {
            second_abs = value;
            second_index = i;
        }
    }

    result->evaluated_candidates = state->candidate_count;
    result->best_delay_samples = best_delay;
    result->best_correlation = best_corr;
    result->best_abs_correlation = best_abs;
    result->ref_diff_rms = state->candidate_ref_diff_rms[best_index];
    result->sample_pairs =
        (size_t)state->candidate_observations[best_index] * pair_count;
    if (second_index != UINT32_MAX) {
        result->second_delay_samples = state->candidate_delays[second_index];
        result->second_correlation = state->candidate_correlations[second_index];
        result->second_abs_correlation = second_abs;
    } else {
        second_abs = 0.0f;
    }
    result->peak_margin = best_abs > second_abs ? best_abs - second_abs : 0.0f;
    result->sample_confidence = clamp01((float)result->sample_pairs /
                                        (float)state->config.minimum_sample_pairs);
    const float mic_energy_conf = clamp01(result->mic_diff_rms /
                                          state->config.minimum_mic_diff_rms);
    const float ref_energy_conf = clamp01(result->ref_diff_rms /
                                          state->config.minimum_ref_diff_rms);
    result->energy_confidence = mic_energy_conf < ref_energy_conf
                                    ? mic_energy_conf
                                    : ref_energy_conf;
    const float corr_conf = clamp01(best_abs / state->config.minimum_abs_correlation);
    const float margin_conf = state->config.minimum_peak_margin > 0.0f
                                  ? clamp01(result->peak_margin /
                                            state->config.minimum_peak_margin)
                                  : 1.0f;
    result->candidate_confidence = corr_conf < margin_conf ? corr_conf : margin_conf;

    const bool base_valid = result->sample_pairs >= state->config.minimum_sample_pairs &&
                            result->mic_diff_rms >= state->config.minimum_mic_diff_rms &&
                            result->ref_diff_rms >= state->config.minimum_ref_diff_rms &&
                            best_abs >= state->config.minimum_abs_correlation;
    if (base_valid) {
        if (!state->has_stable_delay) {
            state->has_stable_delay = true;
            state->stable_delay_samples = (float)best_delay;
            state->stability_ewma = 1.0f;
        } else {
            const float delta = fabsf((float)best_delay - state->stable_delay_samples);
            const float instant = clamp01(1.0f - delta /
                                           (2.0f * state->config.stability_tolerance_samples));
            state->stability_ewma =
                (1.0f - state->config.stability_alpha) * state->stability_ewma +
                state->config.stability_alpha * instant;
            state->stable_delay_samples +=
                state->config.stability_alpha *
                ((float)best_delay - state->stable_delay_samples);
        }
        state->stability_updates++;
    }

    if (state->has_stable_delay) {
        result->stable_delay_samples_f = state->stable_delay_samples;
        result->stable_delay_samples = round_nonnegative(state->stable_delay_samples);
        result->stability_updates = state->stability_updates;
        result->stability_confidence = state->stability_ewma;
    }
    float confidence = result->candidate_confidence;
    if (result->sample_confidence < confidence) confidence = result->sample_confidence;
    if (result->energy_confidence < confidence) confidence = result->energy_confidence;
    if (result->stability_confidence < confidence) confidence = result->stability_confidence;
    result->overall_confidence = confidence;
    result->accepted = base_valid &&
                       result->peak_margin >= state->config.minimum_peak_margin &&
                       state->stability_updates >= state->config.minimum_stability_updates &&
                       state->stability_ewma >= state->config.minimum_stability_confidence &&
                       fabsf((float)best_delay - state->stable_delay_samples) <=
                           (float)state->config.stability_tolerance_samples;
    return NUWAX_AEC_DELAY_SHADOW_OK;
}
