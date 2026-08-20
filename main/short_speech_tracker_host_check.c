#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "conversation/short_speech_tracker.h"

static uint32_t s_sequence;

static nuwax_short_speech_observation_t feed(
    nuwax_short_speech_tracker_t *tracker,
    bool strong,
    bool weak,
    bool trusted,
    uint32_t duration_ms) {
    nuwax_short_speech_observation_t observation = {0};
    assert(duration_ms % 20U == 0U);
    for (uint32_t elapsed_ms = 0U; elapsed_ms < duration_ms;
         elapsed_ms += 20U) {
        observation = nuwax_short_speech_tracker_observe(
            tracker,
            strong,
            weak,
            trusted,
            !strong && !weak,
            true,
            true,
            ++s_sequence,
            7U,
            20U);
        assert(observation.valid);
    }
    return observation;
}

static void test_mixed_short_phrase_closes_split_confirmation_blind_spot(void) {
    nuwax_short_speech_tracker_t tracker = {0};
    nuwax_short_speech_observation_t observation =
        feed(&tracker, true, false, false, 120U);
    assert(!observation.confirmed_now);
    observation = feed(&tracker, false, true, false, 20U);
    assert(!observation.confirmed_now);
    observation = feed(&tracker, false, true, false, 20U);
    assert(observation.confirmed_now);
    assert(observation.reason == NUWAX_SHORT_SPEECH_CONFIRM_MIXED);
    assert(observation.voiced_ms == 160U);
}

static void test_nearfield_and_energy_thresholds_are_independent(void) {
    nuwax_short_speech_tracker_t tracker = {0};
    nuwax_short_speech_observation_t observation =
        feed(&tracker, true, false, true, 160U);
    assert(!observation.confirmed_now);
    observation = feed(&tracker, true, false, true, 20U);
    assert(observation.confirmed_now);
    assert(observation.reason ==
           NUWAX_SHORT_SPEECH_CONFIRM_NEARFIELD_STRONG);

    nuwax_short_speech_tracker_reset(&tracker);
    observation = feed(&tracker, true, false, false, 200U);
    assert(!observation.confirmed_now);
    observation = feed(&tracker, true, false, false, 20U);
    assert(observation.confirmed_now);
    assert(observation.reason == NUWAX_SHORT_SPEECH_CONFIRM_STRONG_ENERGY);
}

static void test_pure_weak_and_trusted_convergence(void) {
    nuwax_short_speech_tracker_t tracker = {0};
    nuwax_short_speech_observation_t observation =
        feed(&tracker, false, true, true, 160U);
    assert(!observation.confirmed_now);
    observation = feed(&tracker, false, true, true, 20U);
    assert(observation.confirmed_now);
    assert(observation.reason == NUWAX_SHORT_SPEECH_CONFIRM_WEAK);

    nuwax_short_speech_tracker_reset(&tracker);
    observation = feed(&tracker, false, true, false, 160U);
    assert(!observation.confirmed_now);
    observation = feed(&tracker, false, true, false, 20U);
    assert(observation.confirmed_now);
    assert(observation.reason == NUWAX_SHORT_SPEECH_CONFIRM_WEAK);

    nuwax_short_speech_tracker_reset(&tracker);
    observation = feed(&tracker, true, false, true, 100U);
    assert(!observation.confirmed_now);
    observation = feed(&tracker, false, false, false, 280U);
    assert(!observation.confirmed_now);
    observation = feed(&tracker, false, false, false, 20U);
    assert(observation.confirmed_now);
    assert(observation.reason == NUWAX_SHORT_SPEECH_CONFIRM_CONVERGENCE);
    assert(observation.trailing_silence_ms == 300U);
}

static void test_noise_and_integrity_fail_closed(void) {
    nuwax_short_speech_tracker_t tracker = {0};
    nuwax_short_speech_observation_t observation =
        feed(&tracker, false, true, false, 100U);
    assert(observation.candidate_active);
    observation = feed(&tracker, false, false, false, 200U);
    assert(observation.candidate_reset);
    assert(!observation.confirmed_now);

    (void)feed(&tracker, true, false, true, 100U);
    observation = nuwax_short_speech_tracker_observe(
        &tracker, false, false, false, true, false, true, ++s_sequence, 7U, 20U);
    assert(observation.valid);
    assert(observation.candidate_reset);
    assert(!tracker.candidate_active);

    (void)feed(&tracker, true, false, true, 100U);
    observation = nuwax_short_speech_tracker_observe(
        &tracker, false, false, false, true, true, true, ++s_sequence, 8U, 20U);
    assert(observation.valid);
    assert(observation.candidate_reset);
    assert(!tracker.candidate_active);
}

int main(void) {
    test_mixed_short_phrase_closes_split_confirmation_blind_spot();
    test_nearfield_and_energy_thresholds_are_independent();
    test_pure_weak_and_trusted_convergence();
    test_noise_and_integrity_fail_closed();
    return 0;
}
