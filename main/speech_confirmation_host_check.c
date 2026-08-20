#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "conversation/speech_confirmation.h"

static const nuwax_speech_confirmation_policy_t k_policy = {
    .window_ms = 320U,
    .required_voiced_ms = 240U,
    .candidate_reset_silence_ms = 120U,
};

static const nuwax_speech_confirmation_policy_t k_weak_start_policy = {
    .window_ms = 480U,
    .required_voiced_ms = 140U,
    .candidate_reset_silence_ms = 200U,
};

static nuwax_speech_confirmation_observation_t feed_policy(
    nuwax_speech_confirmation_t *state,
    const nuwax_speech_confirmation_policy_t *policy,
    bool speech,
    uint32_t duration_ms) {
    nuwax_speech_confirmation_observation_t observation = {0};
    assert(duration_ms % 20U == 0U);
    for (uint32_t elapsed_ms = 0U; elapsed_ms < duration_ms;
         elapsed_ms += 20U) {
        observation = nuwax_speech_confirmation_observe(
            state, policy, speech, 20U);
        assert(observation.valid);
    }
    return observation;
}

static nuwax_speech_confirmation_observation_t feed_policy_with_continuity(
    nuwax_speech_confirmation_t *state,
    const nuwax_speech_confirmation_policy_t *policy,
    bool speech,
    bool continuity,
    uint32_t duration_ms) {
    nuwax_speech_confirmation_observation_t observation = {0};
    assert(duration_ms % 20U == 0U);
    for (uint32_t elapsed_ms = 0U; elapsed_ms < duration_ms;
         elapsed_ms += 20U) {
        observation =
            nuwax_speech_confirmation_observe_with_continuity(
                state, policy, speech, continuity, 20U);
        assert(observation.valid);
    }
    return observation;
}

static nuwax_speech_confirmation_observation_t feed(
    nuwax_speech_confirmation_t *state,
    bool speech,
    uint32_t duration_ms) {
    return feed_policy(state, &k_policy, speech, duration_ms);
}

static void test_weak_far_field_speech_survives_short_syllable_gap(void) {
    nuwax_speech_confirmation_t state = {0};
    nuwax_speech_confirmation_observation_t observation =
        feed_policy(&state, &k_weak_start_policy, true, 60U);
    assert(!observation.confirmed_now);
    observation = feed_policy(
        &state, &k_weak_start_policy, false, 80U);
    assert(observation.candidate_active);
    observation = feed_policy(
        &state, &k_weak_start_policy, true, 80U);
    assert(observation.confirmed_now);
    assert(observation.voiced_ms == 140U);
}

static void test_sparse_ambient_pulses_cannot_accumulate(void) {
    nuwax_speech_confirmation_t state = {0};
    for (unsigned pulse = 0U; pulse < 4U; ++pulse) {
        nuwax_speech_confirmation_observation_t observation =
            feed_policy(&state, &k_weak_start_policy, true, 20U);
        assert(!observation.confirmed_now);
        observation = feed_policy(
            &state, &k_weak_start_policy, false, 200U);
        assert(observation.candidate_reset);
        assert(!observation.candidate_active);
    }
}

static void test_strong_frames_bridge_weak_short_utterance(void) {
    nuwax_speech_confirmation_t state = {0};
    nuwax_speech_confirmation_observation_t observation =
        feed_policy_with_continuity(
            &state, &k_weak_start_policy, true, true, 60U);
    assert(!observation.confirmed_now);
    observation = feed_policy_with_continuity(
        &state, &k_weak_start_policy, false, true, 200U);
    assert(observation.candidate_active);
    assert(observation.voiced_ms == 60U);
    assert(observation.candidate_span_ms == 260U);
    observation = feed_policy_with_continuity(
        &state, &k_weak_start_policy, true, true, 80U);
    assert(observation.confirmed_now);
    assert(observation.voiced_ms == 140U);
    assert(observation.candidate_span_ms == 340U);
}

static void test_strong_continuity_cannot_open_weak_candidate(void) {
    nuwax_speech_confirmation_t state = {0};
    const nuwax_speech_confirmation_observation_t observation =
        feed_policy_with_continuity(
            &state, &k_weak_start_policy, false, true, 300U);
    assert(!observation.confirmed_now);
    assert(!observation.candidate_active);
    assert(observation.voiced_ms == 0U);
}

static void test_real_silence_still_resets_bridged_candidate(void) {
    nuwax_speech_confirmation_t state = {0};
    nuwax_speech_confirmation_observation_t observation =
        feed_policy_with_continuity(
            &state, &k_weak_start_policy, true, true, 60U);
    assert(observation.candidate_active);
    observation = feed_policy_with_continuity(
        &state, &k_weak_start_policy, false, true, 100U);
    assert(observation.candidate_active);
    observation = feed_policy_with_continuity(
        &state, &k_weak_start_policy, false, false, 200U);
    assert(observation.candidate_reset);
    assert(!observation.candidate_active);
}

static void test_continuous_short_speech_confirms_at_240_ms(void) {
    nuwax_speech_confirmation_t state = {0};
    nuwax_speech_confirmation_observation_t observation =
        feed(&state, true, 220U);
    assert(!observation.confirmed_now);
    observation = feed(&state, true, 20U);
    assert(observation.confirmed_now);
    assert(observation.voiced_ms == 240U);
    assert(observation.candidate_span_ms == 240U);
    observation = feed(&state, true, 20U);
    assert(!observation.confirmed_now);
}

static void test_bounded_internal_gap_preserves_candidate(void) {
    nuwax_speech_confirmation_t state = {0};
    nuwax_speech_confirmation_observation_t observation =
        feed(&state, true, 120U);
    assert(observation.candidate_active);
    observation = feed(&state, false, 40U);
    assert(observation.candidate_active);
    assert(!observation.candidate_reset);
    observation = feed(&state, true, 120U);
    assert(observation.confirmed_now);
    assert(observation.voiced_ms == 240U);
    assert(observation.candidate_span_ms == 280U);
}

static void test_long_silence_resets_unconfirmed_candidate(void) {
    nuwax_speech_confirmation_t state = {0};
    nuwax_speech_confirmation_observation_t observation =
        feed(&state, true, 100U);
    assert(observation.candidate_active);
    observation = feed(&state, false, 100U);
    assert(observation.candidate_active);
    observation = feed(&state, false, 20U);
    assert(observation.candidate_reset);
    assert(!observation.candidate_active);
    observation = feed(&state, true, 140U);
    assert(!observation.confirmed_now);
    assert(observation.voiced_ms == 140U);
}

static void test_old_speech_ages_out_of_window(void) {
    nuwax_speech_confirmation_t state = {0};
    nuwax_speech_confirmation_observation_t observation =
        feed(&state, true, 100U);
    observation = feed(&state, false, 100U);
    assert(observation.candidate_active);
    observation = feed(&state, true, 100U);
    assert(!observation.confirmed_now);
    observation = feed(&state, false, 100U);
    assert(observation.candidate_active);
    observation = feed(&state, true, 100U);
    assert(!observation.confirmed_now);
    assert(observation.voiced_ms < 240U);
}

static void test_invalid_policy_fails_closed(void) {
    nuwax_speech_confirmation_t state = {0};
    const nuwax_speech_confirmation_policy_t invalid = {
        .window_ms = 1000U,
        .required_voiced_ms = 240U,
        .candidate_reset_silence_ms = 120U,
    };
    nuwax_speech_confirmation_observation_t observation =
        nuwax_speech_confirmation_observe(&state, &invalid, true, 20U);
    assert(!observation.valid);
    assert(!state.candidate_active);
    observation = nuwax_speech_confirmation_observe(
        &state, &k_policy, true, 0U);
    assert(!observation.valid);
}

int main(void) {
    test_weak_far_field_speech_survives_short_syllable_gap();
    test_sparse_ambient_pulses_cannot_accumulate();
    test_strong_frames_bridge_weak_short_utterance();
    test_strong_continuity_cannot_open_weak_candidate();
    test_real_silence_still_resets_bridged_candidate();
    test_continuous_short_speech_confirms_at_240_ms();
    test_bounded_internal_gap_preserves_candidate();
    test_long_silence_resets_unconfirmed_candidate();
    test_old_speech_ages_out_of_window();
    test_invalid_policy_fails_closed();
    return 0;
}
