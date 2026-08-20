#include <assert.h>
#include <stdio.h>

#include "conversation/adaptive_endpoint.h"
#include "conversation/conversation_endpoint_policy.h"

static nuwax_adaptive_endpoint_observation_t feed(
    nuwax_adaptive_endpoint_t *endpoint,
    nuwax_adaptive_endpoint_frame_class_t frame_class,
    uint32_t duration_ms) {
    nuwax_adaptive_endpoint_observation_t observation = {0};
    assert(duration_ms % 20U == 0U);
    for (uint32_t elapsed_ms = 0U; elapsed_ms < duration_ms;
         elapsed_ms += 20U) {
        observation = nuwax_adaptive_endpoint_observe(
            endpoint, frame_class, 20U);
        assert(observation.valid);
    }
    return observation;
}

static nuwax_adaptive_endpoint_observation_t feed_v3(
    nuwax_adaptive_endpoint_t *endpoint,
    nuwax_adaptive_endpoint_frame_class_t frame_class,
    nuwax_adaptive_endpoint_resume_evidence_t evidence,
    uint32_t duration_ms) {
    nuwax_adaptive_endpoint_observation_t observation = {0};
    assert(duration_ms % 20U == 0U);
    for (uint32_t elapsed_ms = 0U; elapsed_ms < duration_ms;
         elapsed_ms += 20U) {
        observation = nuwax_adaptive_endpoint_observe_v3(
            endpoint, frame_class, &evidence, 20U);
        assert(observation.valid);
    }
    return observation;
}

static void test_v3_commit_thresholds(void) {
    nuwax_adaptive_endpoint_t endpoint = {0};
    nuwax_adaptive_endpoint_observation_t observation;

    (void)feed(&endpoint, NUWAX_ADAPTIVE_ENDPOINT_FRAME_STRONG_SPEECH, 300U);
    observation = feed(&endpoint,
                       NUWAX_ADAPTIVE_ENDPOINT_FRAME_STABLE_NOISE,
                       1180U);
    assert(observation.decision != NUWAX_ADAPTIVE_ENDPOINT_COMMIT);
    observation = feed(&endpoint,
                       NUWAX_ADAPTIVE_ENDPOINT_FRAME_STABLE_NOISE,
                       20U);
    assert(observation.decision == NUWAX_ADAPTIVE_ENDPOINT_COMMIT);
    assert(observation.commit_ms == 1200U);

    observation = nuwax_adaptive_endpoint_seed(
        &endpoint, 100U, 100U, 0U, true, 20U);
    assert(observation.valid);
    observation = feed(&endpoint,
                       NUWAX_ADAPTIVE_ENDPOINT_FRAME_STABLE_NOISE,
                       1200U);
    assert(observation.decision == NUWAX_ADAPTIVE_ENDPOINT_COMMIT);
    assert(observation.commit_ms == 1200U);

    nuwax_adaptive_endpoint_reset(&endpoint);
    (void)feed(&endpoint, NUWAX_ADAPTIVE_ENDPOINT_FRAME_STRONG_SPEECH, 1600U);
    observation = feed(&endpoint,
                       NUWAX_ADAPTIVE_ENDPOINT_FRAME_STABLE_NOISE,
                       1800U);
    assert(observation.decision == NUWAX_ADAPTIVE_ENDPOINT_COMMIT);
    assert(observation.commit_ms == 1800U);

    nuwax_adaptive_endpoint_reset(&endpoint);
    (void)feed(&endpoint, NUWAX_ADAPTIVE_ENDPOINT_FRAME_STRONG_SPEECH, 10000U);
    observation = feed(&endpoint,
                       NUWAX_ADAPTIVE_ENDPOINT_FRAME_STABLE_NOISE,
                       2200U);
    assert(observation.decision == NUWAX_ADAPTIVE_ENDPOINT_COMMIT);
    assert(observation.commit_ms == 2200U);

    nuwax_adaptive_endpoint_reset(&endpoint);
    (void)feed(&endpoint, NUWAX_ADAPTIVE_ENDPOINT_FRAME_STRONG_SPEECH, 15000U);
    observation = feed(&endpoint,
                       NUWAX_ADAPTIVE_ENDPOINT_FRAME_STABLE_NOISE,
                       2500U);
    assert(observation.decision == NUWAX_ADAPTIVE_ENDPOINT_COMMIT);
    assert(observation.commit_ms == 2500U);
}

static void test_single_strong_spike_preserves_pause(void) {
    nuwax_adaptive_endpoint_t endpoint = {0};
    (void)feed(&endpoint, NUWAX_ADAPTIVE_ENDPOINT_FRAME_STRONG_SPEECH, 1600U);
    (void)feed(&endpoint, NUWAX_ADAPTIVE_ENDPOINT_FRAME_STABLE_NOISE, 700U);

    nuwax_adaptive_endpoint_observation_t observation = feed(
        &endpoint, NUWAX_ADAPTIVE_ENDPOINT_FRAME_STRONG_SPEECH, 20U);
    assert(endpoint.pause_active);
    assert(endpoint.pause_ms == 700U);
    assert(endpoint.strict_silence_tail_ms == 700U);
    assert(observation.resume_candidate_active);
    assert(!observation.resumed_pause_learned);
    assert(!observation.prediction_canceled);

    observation = feed(&endpoint,
                       NUWAX_ADAPTIVE_ENDPOINT_FRAME_STABLE_NOISE,
                       20U);
    assert(endpoint.pause_active);
    assert(endpoint.pause_ms == 720U);
    assert(endpoint.strict_silence_tail_ms == 720U);
    assert(endpoint.max_resumed_pause_ms == 0U);

    observation = feed(&endpoint,
                       NUWAX_ADAPTIVE_ENDPOINT_FRAME_STRONG_SPEECH,
                       80U);
    assert(observation.resumed_pause_learned);
    assert(!endpoint.pause_active);
    assert(endpoint.max_resumed_pause_ms == 720U);
}

static void test_resume_evidence_confirmation(void) {
    const nuwax_adaptive_endpoint_resume_evidence_t trusted = {
        .trusted_weak_speech = true,
    };
    const nuwax_adaptive_endpoint_resume_evidence_t energy = {
        .energy_weak_speech = true,
    };
    nuwax_adaptive_endpoint_t endpoint = {0};
    nuwax_adaptive_endpoint_observation_t observation;

    (void)feed(&endpoint, NUWAX_ADAPTIVE_ENDPOINT_FRAME_STRONG_SPEECH, 800U);
    (void)feed(&endpoint, NUWAX_ADAPTIVE_ENDPOINT_FRAME_STABLE_NOISE, 500U);
    observation = feed_v3(&endpoint,
                          NUWAX_ADAPTIVE_ENDPOINT_FRAME_UNKNOWN,
                          trusted,
                          60U);
    assert(endpoint.pause_active && observation.resume_candidate_active);
    observation = feed_v3(&endpoint,
                          NUWAX_ADAPTIVE_ENDPOINT_FRAME_WEAK_SPEECH,
                          trusted,
                          20U);
    assert(observation.resumed_pause_learned);

    (void)feed(&endpoint, NUWAX_ADAPTIVE_ENDPOINT_FRAME_STABLE_NOISE, 500U);
    observation = feed_v3(&endpoint,
                          NUWAX_ADAPTIVE_ENDPOINT_FRAME_UNKNOWN,
                          energy,
                          100U);
    assert(endpoint.pause_active && observation.resume_candidate_active);
    observation = feed_v3(&endpoint,
                          NUWAX_ADAPTIVE_ENDPOINT_FRAME_UNKNOWN,
                          energy,
                          20U);
    assert(observation.resumed_pause_learned);
    assert(!endpoint.pause_active);
}

static void test_trusted_resume_classification_can_alternate(void) {
    const nuwax_adaptive_endpoint_resume_evidence_t strong = {
        .strong_speech = true,
    };
    const nuwax_adaptive_endpoint_resume_evidence_t trusted_weak = {
        .trusted_weak_speech = true,
    };
    nuwax_adaptive_endpoint_t endpoint = {0};
    (void)feed(&endpoint, NUWAX_ADAPTIVE_ENDPOINT_FRAME_STRONG_SPEECH, 600U);
    (void)feed(&endpoint, NUWAX_ADAPTIVE_ENDPOINT_FRAME_STABLE_NOISE, 700U);

    nuwax_adaptive_endpoint_observation_t observation = {0};
    for (unsigned i = 0; i < 4U; ++i) {
        const bool use_strong = (i % 2U) == 0U;
        observation = feed_v3(
            &endpoint,
            use_strong ? NUWAX_ADAPTIVE_ENDPOINT_FRAME_STRONG_SPEECH
                       : NUWAX_ADAPTIVE_ENDPOINT_FRAME_WEAK_SPEECH,
            use_strong ? strong : trusted_weak,
            20U);
    }
    assert(observation.resumed_pause_learned);
    assert(!endpoint.pause_active);
    assert(endpoint.max_resumed_pause_ms == 700U);
}

static void test_energy_and_trusted_resume_can_alternate(void) {
    const nuwax_adaptive_endpoint_resume_evidence_t trusted_weak = {
        .trusted_weak_speech = true,
    };
    const nuwax_adaptive_endpoint_resume_evidence_t energy_weak = {
        .energy_weak_speech = true,
    };
    nuwax_adaptive_endpoint_t endpoint = {0};
    nuwax_adaptive_endpoint_observation_t observation =
        nuwax_adaptive_endpoint_seed(
            &endpoint, 100U, 100U, 0U, true, 20U);
    assert(observation.valid);
    observation = feed(&endpoint,
                       NUWAX_ADAPTIVE_ENDPOINT_FRAME_STABLE_NOISE,
                       1100U);
    assert(observation.decision != NUWAX_ADAPTIVE_ENDPOINT_COMMIT);

    for (unsigned i = 0U; i < 6U; ++i) {
        const bool trusted = (i % 2U) != 0U;
        observation = feed_v3(
            &endpoint,
            trusted ? NUWAX_ADAPTIVE_ENDPOINT_FRAME_WEAK_SPEECH
                    : NUWAX_ADAPTIVE_ENDPOINT_FRAME_UNKNOWN,
            trusted ? trusted_weak : energy_weak,
            20U);
        if (i < 5U) {
            assert(!observation.resumed_pause_learned);
            assert(observation.decision !=
                   NUWAX_ADAPTIVE_ENDPOINT_COMMIT);
        }
    }
    assert(observation.resumed_pause_learned);
    assert(!endpoint.pause_active);
    assert(endpoint.max_resumed_pause_ms == 1100U);
}

static void test_unknown_requires_stable_tail_or_hard_bound(void) {
    nuwax_adaptive_endpoint_t endpoint = {0};
    nuwax_adaptive_endpoint_observation_t observation;
    (void)feed(&endpoint, NUWAX_ADAPTIVE_ENDPOINT_FRAME_STRONG_SPEECH, 300U);
    observation = feed(&endpoint,
                       NUWAX_ADAPTIVE_ENDPOINT_FRAME_UNKNOWN,
                       2580U);
    assert(observation.decision != NUWAX_ADAPTIVE_ENDPOINT_COMMIT);
    assert(observation.commit_ms == 2600U);
    observation = feed(&endpoint,
                       NUWAX_ADAPTIVE_ENDPOINT_FRAME_UNKNOWN,
                       20U);
    assert(observation.decision == NUWAX_ADAPTIVE_ENDPOINT_COMMIT);
    assert(endpoint.strict_silence_tail_ms == 0U);

    nuwax_adaptive_endpoint_reset(&endpoint);
    (void)feed(&endpoint, NUWAX_ADAPTIVE_ENDPOINT_FRAME_STRONG_SPEECH, 300U);
    (void)feed(&endpoint, NUWAX_ADAPTIVE_ENDPOINT_FRAME_UNKNOWN, 800U);
    observation = feed(&endpoint,
                       NUWAX_ADAPTIVE_ENDPOINT_FRAME_STABLE_NOISE,
                       280U);
    assert(observation.decision != NUWAX_ADAPTIVE_ENDPOINT_COMMIT);
    observation = feed(&endpoint,
                       NUWAX_ADAPTIVE_ENDPOINT_FRAME_STABLE_NOISE,
                       20U);
    assert(observation.decision != NUWAX_ADAPTIVE_ENDPOINT_COMMIT);
    assert(endpoint.strict_silence_tail_ms == 300U);
    observation = feed(&endpoint,
                       NUWAX_ADAPTIVE_ENDPOINT_FRAME_STABLE_NOISE,
                       100U);
    assert(observation.decision == NUWAX_ADAPTIVE_ENDPOINT_COMMIT);
    assert(endpoint.strict_silence_tail_ms == 400U);
}

static void test_mixed_noise_has_absolute_pause_bound(void) {
    nuwax_adaptive_endpoint_t endpoint = {0};
    nuwax_adaptive_endpoint_observation_t observation = {0};
    (void)feed(&endpoint, NUWAX_ADAPTIVE_ENDPOINT_FRAME_STRONG_SPEECH, 300U);
    for (uint32_t elapsed_ms = 0U;
         elapsed_ms < NUWAX_ADAPTIVE_ENDPOINT_MIXED_NOISE_HARD_COMMIT_MS;
         elapsed_ms += 20U) {
        const nuwax_adaptive_endpoint_frame_class_t frame_class =
            ((elapsed_ms / 20U) % 2U) == 0U
                ? NUWAX_ADAPTIVE_ENDPOINT_FRAME_UNKNOWN
                : NUWAX_ADAPTIVE_ENDPOINT_FRAME_STABLE_NOISE;
        observation = feed(&endpoint, frame_class, 20U);
        if (elapsed_ms + 20U <
            NUWAX_ADAPTIVE_ENDPOINT_MIXED_NOISE_HARD_COMMIT_MS) {
            assert(observation.decision != NUWAX_ADAPTIVE_ENDPOINT_COMMIT);
        }
    }
    assert(observation.decision == NUWAX_ADAPTIVE_ENDPOINT_COMMIT);
    assert(endpoint.pause_ms ==
           NUWAX_ADAPTIVE_ENDPOINT_MIXED_NOISE_HARD_COMMIT_MS);
}

static void test_isolated_weak_candidate_does_not_destroy_tail(void) {
    const nuwax_adaptive_endpoint_resume_evidence_t energy = {
        .energy_weak_speech = true,
    };
    nuwax_adaptive_endpoint_t endpoint = {0};
    (void)feed(&endpoint, NUWAX_ADAPTIVE_ENDPOINT_FRAME_STRONG_SPEECH, 300U);
    (void)feed(&endpoint, NUWAX_ADAPTIVE_ENDPOINT_FRAME_STABLE_NOISE, 700U);
    nuwax_adaptive_endpoint_observation_t observation = feed_v3(
        &endpoint, NUWAX_ADAPTIVE_ENDPOINT_FRAME_UNKNOWN, energy, 20U);
    assert(observation.resume_candidate_active);
    assert(endpoint.pause_ms == 700U);
    assert(endpoint.strict_silence_tail_ms == 700U);
    assert(!endpoint.weak_pause_seen);
    observation = feed(&endpoint,
                       NUWAX_ADAPTIVE_ENDPOINT_FRAME_STABLE_NOISE,
                       20U);
    assert(!observation.resume_candidate_active);
    assert(endpoint.pause_ms == 720U);
    assert(endpoint.strict_silence_tail_ms == 720U);
    assert(!endpoint.weak_pause_seen);
}

static void test_confirmed_energy_resume_remains_available(void) {
    const nuwax_adaptive_endpoint_resume_evidence_t energy = {
        .energy_weak_speech = true,
    };
    nuwax_adaptive_endpoint_t endpoint = {0};

    (void)feed(&endpoint, NUWAX_ADAPTIVE_ENDPOINT_FRAME_STRONG_SPEECH, 300U);
    for (unsigned i = 0U; i < 12U; ++i) {
        (void)feed(&endpoint,
                   NUWAX_ADAPTIVE_ENDPOINT_FRAME_STABLE_NOISE,
                   200U);
        const nuwax_adaptive_endpoint_observation_t observation = feed_v3(
            &endpoint,
            NUWAX_ADAPTIVE_ENDPOINT_FRAME_WEAK_SPEECH,
            energy,
            120U);
        assert(observation.resumed_pause_learned);
        assert(!endpoint.pause_active);
    }
}

static void test_prediction_cancel_waits_for_confirmed_resume(void) {
    nuwax_adaptive_endpoint_t endpoint = {0};
    (void)feed(&endpoint, NUWAX_ADAPTIVE_ENDPOINT_FRAME_STRONG_SPEECH, 300U);
    nuwax_adaptive_endpoint_observation_t observation = feed(
        &endpoint, NUWAX_ADAPTIVE_ENDPOINT_FRAME_STABLE_NOISE, 1200U);
    assert(observation.decision == NUWAX_ADAPTIVE_ENDPOINT_COMMIT);
    assert(endpoint.predicted_commit);

    observation = feed(&endpoint,
                       NUWAX_ADAPTIVE_ENDPOINT_FRAME_STRONG_SPEECH,
                       20U);
    assert(endpoint.predicted_commit);
    assert(!observation.prediction_canceled);
    assert(!endpoint.would_have_truncated);
    observation = feed(&endpoint,
                       NUWAX_ADAPTIVE_ENDPOINT_FRAME_STRONG_SPEECH,
                       60U);
    assert(observation.prediction_canceled);
    assert(observation.truncation_confirmed);
    assert(endpoint.would_have_truncated);
    assert(endpoint.candidate_cancel_count == 1U);
}

static void test_reset_invalid_and_integration_gate(void) {
    nuwax_adaptive_endpoint_t endpoint = {0};
    assert(sizeof(endpoint) <= 64U);
    assert(nuwax_adaptive_endpoint_state_valid(&endpoint, 20U));
    assert(!nuwax_adaptive_endpoint_observe(
                &endpoint,
                NUWAX_ADAPTIVE_ENDPOINT_FRAME_STRONG_SPEECH,
                0U).valid);
    assert(nuwax_conversation_endpoint_gate_ready(
        true, true, false, 700U, 700U));
    assert(!nuwax_conversation_endpoint_gate_ready(
        true, true, true, 700U, 700U));
}

int main(void) {
    test_v3_commit_thresholds();
    test_single_strong_spike_preserves_pause();
    test_resume_evidence_confirmation();
    test_trusted_resume_classification_can_alternate();
    test_energy_and_trusted_resume_can_alternate();
    test_unknown_requires_stable_tail_or_hard_bound();
    test_mixed_noise_has_absolute_pause_bound();
    test_isolated_weak_candidate_does_not_destroy_tail();
    test_confirmed_energy_resume_remains_available();
    test_prediction_cancel_waits_for_confirmed_resume();
    test_reset_invalid_and_integration_gate();
    puts("adaptive endpoint host check: PASS");
    return 0;
}
