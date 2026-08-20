#include "adaptive_endpoint.h"

#include <limits.h>
#include <string.h>

static uint32_t saturating_add_u32(uint32_t value, uint32_t increment) {
    return UINT32_MAX - value < increment ? UINT32_MAX : value + increment;
}

static uint16_t saturating_add_u16(uint16_t value, uint32_t increment) {
    return increment > (uint32_t)UINT16_MAX - (uint32_t)value
               ? UINT16_MAX
               : (uint16_t)((uint32_t)value + increment);
}

static uint32_t round_up_ms(uint32_t value_ms, uint32_t frame_ms) {
    const uint32_t remainder = value_ms % frame_ms;
    return remainder == 0U
               ? value_ms
               : saturating_add_u32(value_ms, frame_ms - remainder);
}

static uint32_t maximum_frame_aligned_commit_ms(uint32_t frame_ms) {
    return round_up_ms(NUWAX_ADAPTIVE_ENDPOINT_MAX_COMMIT_MS, frame_ms);
}

static uint16_t bounded_commit_ms(uint32_t requested_ms, uint32_t frame_ms) {
    const uint32_t minimum_ms =
        round_up_ms(NUWAX_ADAPTIVE_ENDPOINT_MIN_COMMIT_MS, frame_ms);
    const uint32_t maximum_ms = maximum_frame_aligned_commit_ms(frame_ms);
    uint32_t commit_ms = round_up_ms(requested_ms, frame_ms);
    if (commit_ms < minimum_ms) {
        commit_ms = minimum_ms;
    }
    if (commit_ms > maximum_ms) {
        commit_ms = maximum_ms;
    }
    return (uint16_t)commit_ms;
}

static void expire_trusted_short_if_needed(
    nuwax_adaptive_endpoint_t *endpoint) {
    if (endpoint->trusted_short_utterance &&
        (endpoint->voiced_ms > NUWAX_ADAPTIVE_ENDPOINT_SHORT_VOICED_MAX_MS ||
         endpoint->elapsed_since_first_speech_ms >
             NUWAX_ADAPTIVE_ENDPOINT_SHORT_SPAN_MAX_MS)) {
        endpoint->trusted_short_utterance = false;
    }
}

static uint16_t select_commit_ms(const nuwax_adaptive_endpoint_t *endpoint,
                                 uint32_t frame_ms) {
    uint32_t base_ms = NUWAX_ADAPTIVE_ENDPOINT_VERY_LONG_COMMIT_MS;
    if (endpoint->voiced_ms <= NUWAX_ADAPTIVE_ENDPOINT_SHORT_VOICED_MAX_MS &&
        endpoint->utterance_span_ms <=
            NUWAX_ADAPTIVE_ENDPOINT_SHORT_SPAN_MAX_MS) {
        base_ms = endpoint->policy_v2_enabled &&
                          endpoint->trusted_short_utterance
                      ? NUWAX_ADAPTIVE_ENDPOINT_TRUSTED_SHORT_COMMIT_MS
                      : NUWAX_ADAPTIVE_ENDPOINT_SHORT_COMMIT_MS;
    } else if (endpoint->voiced_ms <=
                   NUWAX_ADAPTIVE_ENDPOINT_MEDIUM_VOICED_MAX_MS &&
               endpoint->utterance_span_ms <=
                   NUWAX_ADAPTIVE_ENDPOINT_MEDIUM_SPAN_MAX_MS) {
        base_ms = endpoint->max_resumed_pause_ms == 0U
                      ? NUWAX_ADAPTIVE_ENDPOINT_MEDIUM_FIRST_PAUSE_COMMIT_MS
                      : NUWAX_ADAPTIVE_ENDPOINT_MEDIUM_COMMIT_MS;
    } else if (endpoint->voiced_ms <=
                   NUWAX_ADAPTIVE_ENDPOINT_LONG_VOICED_MAX_MS &&
               endpoint->utterance_span_ms <=
                   NUWAX_ADAPTIVE_ENDPOINT_LONG_SPAN_MAX_MS) {
        base_ms = NUWAX_ADAPTIVE_ENDPOINT_LONG_COMMIT_MS;
    }

    const uint32_t learned_ms = saturating_add_u32(
        endpoint->max_resumed_pause_ms,
        NUWAX_ADAPTIVE_ENDPOINT_PAUSE_MARGIN_MS);
    if (learned_ms > base_ms) {
        base_ms = learned_ms;
    }
    return bounded_commit_ms(base_ms, frame_ms);
}

static uint16_t effective_pause_commit_ms(
    const nuwax_adaptive_endpoint_t *endpoint,
    uint32_t frame_ms) {
    return bounded_commit_ms(endpoint->frozen_commit_ms, frame_ms);
}

static nuwax_adaptive_endpoint_observation_t observe_pause_decision(
    nuwax_adaptive_endpoint_t *endpoint,
    uint32_t frame_ms) {
    nuwax_adaptive_endpoint_observation_t observation = {.valid = true};
    if (!endpoint->pause_active) {
        return observation;
    }

    observation.commit_ms = effective_pause_commit_ms(endpoint, frame_ms);
    if (endpoint->unknown_streak_ms > 0U) {
        const uint16_t unknown_commit_ms = (uint16_t)round_up_ms(
            NUWAX_ADAPTIVE_ENDPOINT_UNKNOWN_HARD_COMMIT_MS, frame_ms);
        if (unknown_commit_ms > observation.commit_ms) {
            observation.commit_ms = unknown_commit_ms;
        }
    }
    const uint32_t strict_tail_ms =
        round_up_ms(NUWAX_ADAPTIVE_ENDPOINT_STRICT_TAIL_MS, frame_ms);
    observation.candidate_ms = endpoint->frozen_commit_ms > strict_tail_ms
                                   ? (uint16_t)(endpoint->frozen_commit_ms -
                                                strict_tail_ms)
                                   : endpoint->frozen_commit_ms;
    if (endpoint->resume_candidate_kind !=
        NUWAX_ADAPTIVE_ENDPOINT_RESUME_NONE) {
        observation.resume_candidate_active = true;
        if (endpoint->pause_ms >= observation.candidate_ms) {
            observation.decision = NUWAX_ADAPTIVE_ENDPOINT_CANDIDATE;
        }
        return observation;
    }
    const bool unknown_hard_commit =
        endpoint->unknown_streak_ms >=
        round_up_ms(NUWAX_ADAPTIVE_ENDPOINT_UNKNOWN_HARD_COMMIT_MS,
                    frame_ms);
    const bool mixed_noise_hard_commit =
        endpoint->uncertain_pause_seen &&
        endpoint->pause_ms >=
            round_up_ms(NUWAX_ADAPTIVE_ENDPOINT_MIXED_NOISE_HARD_COMMIT_MS,
                        frame_ms);
    const bool commit = unknown_hard_commit || mixed_noise_hard_commit ||
        (endpoint->pause_ms >= observation.commit_ms &&
         endpoint->strict_silence_tail_ms >= strict_tail_ms);
    if (commit) {
        observation.decision = NUWAX_ADAPTIVE_ENDPOINT_COMMIT;
        if (!endpoint->predicted_commit) {
            endpoint->predicted_commit = true;
            observation.prediction_started = true;
        }
    } else if (endpoint->pause_ms >= observation.candidate_ms) {
        observation.decision = NUWAX_ADAPTIVE_ENDPOINT_CANDIDATE;
    }
    return observation;
}

void nuwax_adaptive_endpoint_reset(nuwax_adaptive_endpoint_t *endpoint) {
    if (endpoint) {
        memset(endpoint, 0, sizeof(*endpoint));
    }
}

bool nuwax_adaptive_endpoint_state_valid(
    const nuwax_adaptive_endpoint_t *endpoint,
    uint32_t frame_ms) {
    if (!endpoint || frame_ms == 0U ||
        frame_ms > NUWAX_ADAPTIVE_ENDPOINT_MAX_FRAME_MS ||
        maximum_frame_aligned_commit_ms(frame_ms) <
            round_up_ms(NUWAX_ADAPTIVE_ENDPOINT_MIN_COMMIT_MS, frame_ms)) {
        return false;
    }
    if (!endpoint->speech_started) {
        return endpoint->voiced_ms == 0U &&
               endpoint->elapsed_since_first_speech_ms == 0U &&
               endpoint->utterance_span_ms == 0U && endpoint->pause_ms == 0U &&
               endpoint->strict_silence_tail_ms == 0U &&
               endpoint->pending_resumed_pause_ms == 0U &&
               endpoint->max_resumed_pause_ms == 0U &&
               endpoint->frozen_commit_ms == 0U &&
               endpoint->resume_speech_streak_ms == 0U &&
               endpoint->weak_speech_streak_ms == 0U &&
               endpoint->unknown_streak_ms == 0U &&
               endpoint->resume_candidate_kind ==
                   NUWAX_ADAPTIVE_ENDPOINT_RESUME_NONE &&
               !endpoint->pause_active && !endpoint->predicted_commit &&
               !endpoint->would_have_truncated &&
               !endpoint->pending_prediction_cancel &&
               !endpoint->uncertain_pause_seen &&
               !endpoint->weak_pause_seen &&
               !endpoint->trusted_short_utterance &&
               !endpoint->policy_v2_enabled;
    }
    if (endpoint->voiced_ms > endpoint->elapsed_since_first_speech_ms ||
        endpoint->utterance_span_ms >
            endpoint->elapsed_since_first_speech_ms ||
        endpoint->strict_silence_tail_ms > endpoint->pause_ms) {
        return false;
    }
    if (endpoint->pause_active) {
        if (endpoint->frozen_commit_ms == 0U) {
            return false;
        }
    } else if (endpoint->pause_ms != 0U ||
               endpoint->strict_silence_tail_ms != 0U ||
               endpoint->frozen_commit_ms != 0U ||
               endpoint->uncertain_pause_seen || endpoint->weak_pause_seen) {
        return false;
    }
    const bool resume_candidate_active =
        endpoint->resume_candidate_kind !=
        NUWAX_ADAPTIVE_ENDPOINT_RESUME_NONE;
    if (resume_candidate_active !=
            (endpoint->resume_speech_streak_ms != 0U) ||
        (resume_candidate_active && !endpoint->pause_active) ||
        (!resume_candidate_active &&
         (endpoint->pending_resumed_pause_ms != 0U ||
          endpoint->pending_prediction_cancel))) {
        return false;
    }
    return true;
}

static nuwax_adaptive_endpoint_observation_t observe_direct_speech(
    nuwax_adaptive_endpoint_t *endpoint,
    uint32_t frame_ms,
    uint32_t voiced_credit_ms) {
    nuwax_adaptive_endpoint_observation_t observation = {.valid = true};
    if (!endpoint->speech_started) {
        endpoint->speech_started = true;
    }
    endpoint->elapsed_since_first_speech_ms = saturating_add_u32(
        endpoint->elapsed_since_first_speech_ms, frame_ms);
    expire_trusted_short_if_needed(endpoint);

    endpoint->voiced_ms =
        saturating_add_u32(endpoint->voiced_ms, voiced_credit_ms);
    endpoint->utterance_span_ms = endpoint->elapsed_since_first_speech_ms;
    return observation;
}

static uint32_t resume_confirm_ms(
    nuwax_adaptive_endpoint_resume_kind_t kind) {
    switch (kind) {
        case NUWAX_ADAPTIVE_ENDPOINT_RESUME_STRONG:
            return NUWAX_ADAPTIVE_ENDPOINT_RESUME_CONFIRM_MS;
        case NUWAX_ADAPTIVE_ENDPOINT_RESUME_TRUSTED_WEAK:
            return NUWAX_ADAPTIVE_ENDPOINT_WEAK_RESUME_CONFIRM_MS;
        case NUWAX_ADAPTIVE_ENDPOINT_RESUME_ENERGY_WEAK:
            return NUWAX_ADAPTIVE_ENDPOINT_ENERGY_RESUME_CONFIRM_MS;
        case NUWAX_ADAPTIVE_ENDPOINT_RESUME_NONE:
        default:
            return UINT32_MAX;
    }
}

static nuwax_adaptive_endpoint_resume_kind_t select_resume_kind(
    const nuwax_adaptive_endpoint_t *endpoint,
    const nuwax_adaptive_endpoint_resume_evidence_t *evidence) {
    if (!endpoint || !evidence) return NUWAX_ADAPTIVE_ENDPOINT_RESUME_NONE;
    if (evidence->strong_speech) {
        return NUWAX_ADAPTIVE_ENDPOINT_RESUME_STRONG;
    }
    if (evidence->trusted_weak_speech) {
        return NUWAX_ADAPTIVE_ENDPOINT_RESUME_TRUSTED_WEAK;
    }
    if (evidence->energy_weak_speech) {
        return NUWAX_ADAPTIVE_ENDPOINT_RESUME_ENERGY_WEAK;
    }
    return NUWAX_ADAPTIVE_ENDPOINT_RESUME_NONE;
}

static void start_pause_if_needed(nuwax_adaptive_endpoint_t *endpoint,
                                  uint32_t frame_ms) {
    if (endpoint->pause_active) return;
    endpoint->pause_active = true;
    endpoint->pause_ms = 0U;
    endpoint->strict_silence_tail_ms = 0U;
    endpoint->unknown_streak_ms = 0U;
    endpoint->uncertain_pause_seen = false;
    endpoint->weak_pause_seen = false;
    endpoint->frozen_commit_ms = select_commit_ms(endpoint, frame_ms);
}

static void clear_resume_candidate(nuwax_adaptive_endpoint_t *endpoint) {
    endpoint->pending_resumed_pause_ms = 0U;
    endpoint->resume_speech_streak_ms = 0U;
    endpoint->pending_prediction_cancel = false;
    endpoint->resume_candidate_kind =
        NUWAX_ADAPTIVE_ENDPOINT_RESUME_NONE;
}

static bool resume_kinds_share_streak(
    nuwax_adaptive_endpoint_resume_kind_t current,
    nuwax_adaptive_endpoint_resume_kind_t next) {
    return current != NUWAX_ADAPTIVE_ENDPOINT_RESUME_NONE &&
           next != NUWAX_ADAPTIVE_ENDPOINT_RESUME_NONE;
}

static nuwax_adaptive_endpoint_resume_kind_t conservative_resume_kind(
    nuwax_adaptive_endpoint_resume_kind_t current,
    nuwax_adaptive_endpoint_resume_kind_t next) {
    if (current == NUWAX_ADAPTIVE_ENDPOINT_RESUME_NONE) {
        return next;
    }
    if (current == NUWAX_ADAPTIVE_ENDPOINT_RESUME_ENERGY_WEAK ||
        next == NUWAX_ADAPTIVE_ENDPOINT_RESUME_ENERGY_WEAK) {
        return NUWAX_ADAPTIVE_ENDPOINT_RESUME_ENERGY_WEAK;
    }
    return next;
}

static nuwax_adaptive_endpoint_observation_t observe_resume_candidate(
    nuwax_adaptive_endpoint_t *endpoint,
    nuwax_adaptive_endpoint_resume_kind_t kind,
    uint32_t frame_ms) {
    nuwax_adaptive_endpoint_observation_t observation = {.valid = true};
    start_pause_if_needed(endpoint, frame_ms);
    const nuwax_adaptive_endpoint_resume_kind_t current_kind =
        (nuwax_adaptive_endpoint_resume_kind_t)
            endpoint->resume_candidate_kind;
    if (current_kind != kind &&
        !resume_kinds_share_streak(current_kind, kind)) {
        endpoint->resume_candidate_kind = (uint8_t)kind;
        endpoint->pending_resumed_pause_ms = endpoint->pause_ms;
        endpoint->resume_speech_streak_ms = 0U;
        endpoint->pending_prediction_cancel = endpoint->predicted_commit;
    } else if (current_kind != kind) {
        /* Frame classification can alternate at the weak-speech boundary.
         * Preserve the continuous evidence, while any energy-only frame keeps
         * the whole run on the conservative 120 ms confirmation threshold. */
        endpoint->resume_candidate_kind = (uint8_t)
            conservative_resume_kind(current_kind, kind);
    }
    endpoint->resume_speech_streak_ms = saturating_add_u16(
        endpoint->resume_speech_streak_ms, frame_ms);
    observation.resume_candidate_active = true;

    const nuwax_adaptive_endpoint_resume_kind_t effective_kind =
        (nuwax_adaptive_endpoint_resume_kind_t)
            endpoint->resume_candidate_kind;
    const uint32_t required_ms = round_up_ms(
        resume_confirm_ms(effective_kind), frame_ms);
    if (endpoint->resume_speech_streak_ms < required_ms) {
        return observe_pause_decision(endpoint, frame_ms);
    }

    const uint32_t resumed_pause_ms = endpoint->pending_resumed_pause_ms;
    const uint16_t voiced_credit_ms = endpoint->resume_speech_streak_ms;
    const bool canceled_prediction = endpoint->pending_prediction_cancel;
    endpoint->voiced_ms = saturating_add_u32(endpoint->voiced_ms,
                                             voiced_credit_ms);
    endpoint->utterance_span_ms = endpoint->elapsed_since_first_speech_ms;
    if (resumed_pause_ms > endpoint->max_resumed_pause_ms) {
        endpoint->max_resumed_pause_ms = resumed_pause_ms;
    }
    if (canceled_prediction) {
        endpoint->would_have_truncated = true;
        observation.truncation_confirmed = true;
        if (endpoint->candidate_cancel_count < UINT16_MAX) {
            endpoint->candidate_cancel_count++;
        }
    }
    clear_resume_candidate(endpoint);
    endpoint->pause_active = false;
    endpoint->pause_ms = 0U;
    endpoint->strict_silence_tail_ms = 0U;
    endpoint->unknown_streak_ms = 0U;
    endpoint->frozen_commit_ms = 0U;
    endpoint->uncertain_pause_seen = false;
    endpoint->weak_pause_seen = false;
    endpoint->predicted_commit = false;
    observation.prediction_canceled = canceled_prediction;
    observation.resumed_pause_learned = true;
    observation.resume_candidate_active = false;
    return observation;
}

nuwax_adaptive_endpoint_observation_t nuwax_adaptive_endpoint_seed(
    nuwax_adaptive_endpoint_t *endpoint,
    uint32_t voiced_ms,
    uint32_t utterance_span_ms,
    uint32_t trailing_stable_silence_ms,
    bool trusted_short_utterance,
    uint32_t frame_ms) {
    nuwax_adaptive_endpoint_observation_t observation = {0};
    if (!endpoint || frame_ms == 0U ||
        frame_ms > NUWAX_ADAPTIVE_ENDPOINT_MAX_FRAME_MS || voiced_ms == 0U ||
        utterance_span_ms < voiced_ms || voiced_ms % frame_ms != 0U ||
        utterance_span_ms % frame_ms != 0U ||
        trailing_stable_silence_ms % frame_ms != 0U) {
        nuwax_adaptive_endpoint_reset(endpoint);
        return observation;
    }

    nuwax_adaptive_endpoint_reset(endpoint);
    endpoint->speech_started = true;
    endpoint->voiced_ms = voiced_ms;
    endpoint->utterance_span_ms = utterance_span_ms;
    endpoint->elapsed_since_first_speech_ms = saturating_add_u32(
        utterance_span_ms, trailing_stable_silence_ms);
    endpoint->trusted_short_utterance =
        trusted_short_utterance &&
        voiced_ms <= NUWAX_ADAPTIVE_ENDPOINT_SHORT_VOICED_MAX_MS &&
        utterance_span_ms <= NUWAX_ADAPTIVE_ENDPOINT_SHORT_SPAN_MAX_MS;
    endpoint->policy_v2_enabled = true;
    if (trailing_stable_silence_ms > 0U) {
        endpoint->pause_active = true;
        endpoint->pause_ms = trailing_stable_silence_ms;
        endpoint->strict_silence_tail_ms = trailing_stable_silence_ms;
        endpoint->frozen_commit_ms = select_commit_ms(endpoint, frame_ms);
    }
    if (!nuwax_adaptive_endpoint_state_valid(endpoint, frame_ms)) {
        nuwax_adaptive_endpoint_reset(endpoint);
        return observation;
    }
    return observe_pause_decision(endpoint, frame_ms);
}

nuwax_adaptive_endpoint_observation_t nuwax_adaptive_endpoint_observe_v3(
    nuwax_adaptive_endpoint_t *endpoint,
    nuwax_adaptive_endpoint_frame_class_t frame_class,
    const nuwax_adaptive_endpoint_resume_evidence_t *resume_evidence,
    uint32_t frame_ms) {
    nuwax_adaptive_endpoint_observation_t observation = {0};
    if (!nuwax_adaptive_endpoint_state_valid(endpoint, frame_ms) ||
        frame_class > NUWAX_ADAPTIVE_ENDPOINT_FRAME_UNKNOWN) {
        return observation;
    }

    if (!endpoint->speech_started) {
        if (frame_class != NUWAX_ADAPTIVE_ENDPOINT_FRAME_STRONG_SPEECH) {
            observation.valid = true;
            return observation;
        }
        return observe_direct_speech(endpoint, frame_ms, frame_ms);
    }

    endpoint->elapsed_since_first_speech_ms = saturating_add_u32(
        endpoint->elapsed_since_first_speech_ms, frame_ms);
    expire_trusted_short_if_needed(endpoint);
    const nuwax_adaptive_endpoint_resume_kind_t resume_kind =
        select_resume_kind(endpoint, resume_evidence);
    if (endpoint->pause_active &&
        resume_kind != NUWAX_ADAPTIVE_ENDPOINT_RESUME_NONE) {
        return observe_resume_candidate(endpoint, resume_kind, frame_ms);
    }
    if (!endpoint->pause_active &&
        frame_class == NUWAX_ADAPTIVE_ENDPOINT_FRAME_STRONG_SPEECH) {
        endpoint->voiced_ms = saturating_add_u32(endpoint->voiced_ms,
                                                 frame_ms);
        endpoint->utterance_span_ms = endpoint->elapsed_since_first_speech_ms;
        observation.valid = true;
        return observation;
    }
    if (!endpoint->pause_active &&
        resume_kind != NUWAX_ADAPTIVE_ENDPOINT_RESUME_NONE) {
        return observe_resume_candidate(endpoint, resume_kind, frame_ms);
    }

    if (endpoint->resume_candidate_kind !=
        NUWAX_ADAPTIVE_ENDPOINT_RESUME_NONE) {
        clear_resume_candidate(endpoint);
    }
    start_pause_if_needed(endpoint, frame_ms);
    endpoint->pause_ms = saturating_add_u32(endpoint->pause_ms, frame_ms);
    if (frame_class == NUWAX_ADAPTIVE_ENDPOINT_FRAME_STABLE_NOISE) {
        endpoint->strict_silence_tail_ms = saturating_add_u32(
            endpoint->strict_silence_tail_ms, frame_ms);
        endpoint->unknown_streak_ms = 0U;
    } else {
        endpoint->strict_silence_tail_ms = 0U;
        endpoint->uncertain_pause_seen = true;
        endpoint->unknown_streak_ms = saturating_add_u16(
            endpoint->unknown_streak_ms, frame_ms);
    }
    return observe_pause_decision(endpoint, frame_ms);
}

nuwax_adaptive_endpoint_observation_t nuwax_adaptive_endpoint_observe(
    nuwax_adaptive_endpoint_t *endpoint,
    nuwax_adaptive_endpoint_frame_class_t frame_class,
    uint32_t frame_ms) {
    const nuwax_adaptive_endpoint_resume_evidence_t evidence = {
        .strong_speech =
            frame_class == NUWAX_ADAPTIVE_ENDPOINT_FRAME_STRONG_SPEECH,
        .trusted_weak_speech =
            frame_class == NUWAX_ADAPTIVE_ENDPOINT_FRAME_WEAK_SPEECH,
        .energy_weak_speech = false,
    };
    return nuwax_adaptive_endpoint_observe_v3(
        endpoint, frame_class, &evidence, frame_ms);
}
