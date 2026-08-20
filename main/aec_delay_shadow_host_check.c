#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "conversation/aec_delay_shadow.h"

#define FRAME_SAMPLES 256U

static uint32_t s_rng = 0x13579bdfU;

static uint32_t next_random(void) {
    uint32_t value = s_rng;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    s_rng = value;
    return value;
}

static int32_t random_signed(int32_t limit) {
    return (int32_t)(next_random() % (uint32_t)(limit * 2 + 1)) - limit;
}

static int16_t clamp_pcm16(int32_t value) {
    if (value > INT16_MAX) return INT16_MAX;
    if (value < INT16_MIN) return INT16_MIN;
    return (int16_t)value;
}

static void make_correlated_frame(int16_t *history,
                                  size_t history_samples,
                                  int16_t *mic,
                                  uint32_t max_delay,
                                  uint32_t target_delay) {
    for (size_t i = 0; i < history_samples; i++) {
        history[i] = (int16_t)random_signed(6000);
    }
    const size_t primary = (size_t)(max_delay - target_delay);
    const size_t secondary = target_delay + 11U <= max_delay ? primary - 11U : primary;
    for (size_t i = 0; i < FRAME_SAMPLES; i++) {
        const int32_t value =
            ((int32_t)history[primary + i] * 3 +
             (int32_t)history[secondary + i]) /
                4 +
            random_signed(900);
        mic[i] = clamp_pcm16(value);
    }
}

static int run_known_delay(uint32_t min_delay,
                           uint32_t max_delay,
                           uint32_t target_delay,
                           uint32_t generation) {
    nuwax_aec_delay_shadow_config_t config;
    nuwax_aec_delay_shadow_default_config(&config);
    config.min_delay_samples = min_delay;
    config.max_delay_samples = max_delay;
    config.cadence_frames = 1;
    config.minimum_stability_updates = 4;

    nuwax_aec_delay_shadow_state_t *state = calloc(1, sizeof(*state));
    const size_t history_samples =
        FRAME_SAMPLES + (size_t)max_delay - (size_t)min_delay;
    int16_t *history = calloc(history_samples, sizeof(*history));
    int16_t mic[FRAME_SAMPLES] = {0};
    if (!state || !history ||
        nuwax_aec_delay_shadow_init(state, &config, generation) !=
            NUWAX_AEC_DELAY_SHADOW_OK) {
        free(history);
        free(state);
        return 1;
    }

    nuwax_aec_delay_shadow_result_t result = {0};
    for (uint32_t frame = 0; frame < 32U; frame++) {
        make_correlated_frame(history,
                              history_samples,
                              mic,
                              max_delay,
                              target_delay);
        const nuwax_aec_delay_shadow_status_t status =
            nuwax_aec_delay_shadow_process(state,
                                           mic,
                                           FRAME_SAMPLES,
                                           history,
                                           history_samples,
                                           generation,
                                           &result);
        if (status != NUWAX_AEC_DELAY_SHADOW_OK) {
            free(history);
            free(state);
            return 1;
        }
    }
    const uint32_t error = result.best_delay_samples >= target_delay
                               ? result.best_delay_samples - target_delay
                               : target_delay - result.best_delay_samples;
    const uint32_t stable_error = result.stable_delay_samples >= target_delay
                                      ? result.stable_delay_samples - target_delay
                                      : target_delay - result.stable_delay_samples;
    const bool pass = result.accepted && error <= 1U && stable_error <= 2U;
    printf("[%s] known_delay min=%lu max=%lu target=%lu best=%lu stable=%lu corr=%.3f margin=%.3f updates=%lu\n",
           pass ? "PASS" : "FAIL",
           (unsigned long)min_delay,
           (unsigned long)max_delay,
           (unsigned long)target_delay,
           (unsigned long)result.best_delay_samples,
           (unsigned long)result.stable_delay_samples,
           (double)result.best_abs_correlation,
           (double)result.peak_margin,
           (unsigned long)result.stability_updates);
    free(history);
    free(state);
    return pass ? 0 : 1;
}

static int check_uncorrelated_rejected(void) {
    nuwax_aec_delay_shadow_config_t config;
    nuwax_aec_delay_shadow_default_config(&config);
    config.cadence_frames = 1;
    config.minimum_stability_updates = 3;
    const size_t history_samples =
        FRAME_SAMPLES + (size_t)config.max_delay_samples - config.min_delay_samples;
    nuwax_aec_delay_shadow_state_t *state = calloc(1, sizeof(*state));
    int16_t *history = calloc(history_samples, sizeof(*history));
    int16_t mic[FRAME_SAMPLES] = {0};
    if (!state || !history ||
        nuwax_aec_delay_shadow_init(state, &config, 9U) != NUWAX_AEC_DELAY_SHADOW_OK) {
        free(history);
        free(state);
        return 1;
    }
    bool accepted = false;
    nuwax_aec_delay_shadow_result_t result = {0};
    for (uint32_t frame = 0; frame < 12U; frame++) {
        for (size_t i = 0; i < history_samples; i++) history[i] = (int16_t)random_signed(6000);
        for (size_t i = 0; i < FRAME_SAMPLES; i++) mic[i] = (int16_t)random_signed(6000);
        (void)nuwax_aec_delay_shadow_process(state,
                                             mic,
                                             FRAME_SAMPLES,
                                             history,
                                             history_samples,
                                             9U,
                                             &result);
        accepted |= result.accepted;
    }
    printf("[%s] uncorrelated_rejected accepted=%d best_corr=%.3f stability=%.3f\n",
           !accepted ? "PASS" : "FAIL",
           accepted ? 1 : 0,
           (double)result.best_abs_correlation,
           (double)result.stability_confidence);
    free(history);
    free(state);
    return accepted ? 1 : 0;
}

static int check_generation_reset(void) {
    nuwax_aec_delay_shadow_config_t config;
    nuwax_aec_delay_shadow_default_config(&config);
    config.cadence_frames = 1;
    const size_t history_samples =
        FRAME_SAMPLES + (size_t)config.max_delay_samples - config.min_delay_samples;
    nuwax_aec_delay_shadow_state_t *state = calloc(1, sizeof(*state));
    int16_t *history = calloc(history_samples, sizeof(*history));
    int16_t mic[FRAME_SAMPLES] = {0};
    if (!state || !history ||
        nuwax_aec_delay_shadow_init(state, &config, 1U) != NUWAX_AEC_DELAY_SHADOW_OK) {
        free(history);
        free(state);
        return 1;
    }
    make_correlated_frame(history, history_samples, mic, config.max_delay_samples, 1403U);
    nuwax_aec_delay_shadow_result_t result = {0};
    (void)nuwax_aec_delay_shadow_process(state,
                                         mic,
                                         FRAME_SAMPLES,
                                         history,
                                         history_samples,
                                         2U,
                                         &result);
    const bool pass = result.generation_reset &&
                      !result.estimate_ready &&
                      result.stability_updates == 0U &&
                      state->consensus_frame_count == 1U;
    printf("[%s] generation_reset reset=%d updates=%lu\n",
           pass ? "PASS" : "FAIL",
           result.generation_reset ? 1 : 0,
           (unsigned long)result.stability_updates);
    free(history);
    free(state);
    return pass ? 0 : 1;
}

int main(void) {
    int failures = 0;
    failures += run_known_delay(512U, 3584U, 1313U, 3U);
    failures += run_known_delay(1155U, 3584U, 2308U, 4U);
    failures += check_uncorrelated_rejected();
    failures += check_generation_reset();
    printf("AEC delay shadow host check: %s (%d failures)\n",
           failures == 0 ? "PASS" : "FAIL",
           failures);
    return failures == 0 ? 0 : 1;
}
