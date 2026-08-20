#include "short_speech_tracker.h"

#include <limits.h>
#include <string.h>

static uint32_t frames_ceil(uint32_t duration_ms, uint32_t frame_ms) {
    return (duration_ms + frame_ms - 1U) / frame_ms;
}

static uint16_t frames_to_ms(uint32_t frames, uint32_t frame_ms) {
    const uint32_t duration_ms = frames * frame_ms;
    return duration_ms > UINT16_MAX ? UINT16_MAX : (uint16_t)duration_ms;
}

static uint16_t increment_u16(uint16_t value) {
    return value == UINT16_MAX ? UINT16_MAX : (uint16_t)(value + 1U);
}

static uint32_t window_mask(uint32_t frames) {
    return frames >= 32U ? UINT32_MAX : ((1U << frames) - 1U);
}

static uint16_t bit_count_ms(uint32_t bits, uint32_t frame_ms) {
    uint32_t count = 0U;
    while (bits != 0U) {
        bits &= bits - 1U;
        count++;
    }
    return frames_to_ms(count, frame_ms);
}

void nuwax_short_speech_tracker_reset(
    nuwax_short_speech_tracker_t *tracker) {
    if (tracker) {
        memset(tracker, 0, sizeof(*tracker));
    }
}

static bool input_valid(uint32_t frame_ms, uint32_t ws_epoch) {
    if (frame_ms == 0U || frame_ms > NUWAX_SHORT_SPEECH_MAX_FRAME_MS ||
        ws_epoch == 0U) {
        return false;
    }
    const uint32_t window_frames =
        frames_ceil(NUWAX_SHORT_SPEECH_WINDOW_MS, frame_ms);
    return window_frames > 0U && window_frames <= 32U;
}

static void fill_observation(
    const nuwax_short_speech_tracker_t *tracker,
    uint32_t frame_ms,
    nuwax_short_speech_observation_t *observation) {
    const uint32_t voiced_bits = tracker->weak_bits | tracker->strong_bits;
    observation->voiced_ms = bit_count_ms(voiced_bits, frame_ms);
    observation->strong_ms = bit_count_ms(tracker->strong_bits, frame_ms);
    observation->weak_ms = bit_count_ms(tracker->weak_bits, frame_ms);
    observation->candidate_span_ms =
        frames_to_ms(tracker->candidate_span_frames, frame_ms);
    observation->trailing_silence_ms =
        frames_to_ms(tracker->stable_silence_run_frames, frame_ms);
    observation->first_voiced_sequence = tracker->first_voiced_sequence;
    observation->last_voiced_sequence = tracker->last_voiced_sequence;
    observation->candidate_active = tracker->candidate_active;
    observation->trusted_evidence_seen = tracker->trusted_bits != 0U;
}

nuwax_short_speech_observation_t nuwax_short_speech_tracker_observe(
    nuwax_short_speech_tracker_t *tracker,
    bool strong_speech,
    bool weak_speech,
    bool trusted_evidence,
    bool stable_silence,
    bool capture_contiguous,
    bool echo_free,
    uint32_t capture_sequence,
    uint32_t ws_epoch,
    uint32_t frame_ms) {
    nuwax_short_speech_observation_t observation = {0};
    if (!tracker || !input_valid(frame_ms, ws_epoch)) {
        nuwax_short_speech_tracker_reset(tracker);
        return observation;
    }
    observation.valid = true;

    if (!capture_contiguous || !echo_free ||
        (tracker->candidate_active && tracker->ws_epoch != ws_epoch)) {
        observation.candidate_reset = tracker->candidate_active;
        nuwax_short_speech_tracker_reset(tracker);
        return observation;
    }

    const bool voiced = strong_speech || weak_speech;
    if (voiced && !tracker->candidate_active) {
        tracker->candidate_active = true;
        tracker->ws_epoch = ws_epoch;
        tracker->first_voiced_sequence = capture_sequence;
        observation.candidate_started = true;
    }

    const uint32_t window_frames =
        frames_ceil(NUWAX_SHORT_SPEECH_WINDOW_MS, frame_ms);
    const uint32_t mask = window_mask(window_frames);
    tracker->weak_bits =
        ((tracker->weak_bits << 1U) | (weak_speech && !strong_speech ? 1U : 0U)) &
        mask;
    tracker->strong_bits =
        ((tracker->strong_bits << 1U) | (strong_speech ? 1U : 0U)) & mask;
    tracker->trusted_bits =
        ((tracker->trusted_bits << 1U) |
         (voiced && trusted_evidence ? 1U : 0U)) &
        mask;
    if (tracker->observed_frames < window_frames) {
        tracker->observed_frames++;
    }

    if (voiced) {
        tracker->last_voiced_sequence = capture_sequence;
        tracker->silence_run_frames = 0U;
        tracker->stable_silence_run_frames = 0U;
        if (strong_speech) {
            tracker->strong_run_frames =
                increment_u16(tracker->strong_run_frames);
            tracker->trusted_strong_run_frames = trusted_evidence
                                                     ? increment_u16(
                                                           tracker->trusted_strong_run_frames)
                                                     : 0U;
        } else {
            tracker->strong_run_frames = 0U;
            tracker->trusted_strong_run_frames = 0U;
        }
    } else {
        tracker->strong_run_frames = 0U;
        tracker->trusted_strong_run_frames = 0U;
        if (tracker->candidate_active) {
            tracker->silence_run_frames =
                increment_u16(tracker->silence_run_frames);
            tracker->stable_silence_run_frames = stable_silence
                                                     ? increment_u16(
                                                           tracker->stable_silence_run_frames)
                                                     : 0U;
        }
    }
    if (tracker->candidate_active) {
        tracker->candidate_span_frames =
            increment_u16(tracker->candidate_span_frames);
    }

    fill_observation(tracker, frame_ms, &observation);
    if (!tracker->candidate_active || tracker->confirmed) {
        return observation;
    }

    const uint32_t trusted_strong_ms =
        (uint32_t)tracker->trusted_strong_run_frames * frame_ms;
    const uint32_t strong_run_ms =
        (uint32_t)tracker->strong_run_frames * frame_ms;
    if (trusted_strong_ms >= NUWAX_SHORT_SPEECH_NEARFIELD_STRONG_MS) {
        observation.reason = NUWAX_SHORT_SPEECH_CONFIRM_NEARFIELD_STRONG;
    } else if (strong_run_ms >= NUWAX_SHORT_SPEECH_STRONG_MS) {
        observation.reason = NUWAX_SHORT_SPEECH_CONFIRM_STRONG_ENERGY;
    } else if (observation.strong_ms > 0U && observation.weak_ms > 0U &&
               observation.voiced_ms >= NUWAX_SHORT_SPEECH_MIXED_MS) {
        observation.reason = NUWAX_SHORT_SPEECH_CONFIRM_MIXED;
    } else if (observation.strong_ms == 0U &&
               observation.weak_ms >=
                   (observation.trusted_evidence_seen
                        ? NUWAX_SHORT_SPEECH_TRUSTED_WEAK_MS
                        : NUWAX_SHORT_SPEECH_UNTRUSTED_WEAK_MS)) {
        observation.reason = NUWAX_SHORT_SPEECH_CONFIRM_WEAK;
    } else if (!voiced && stable_silence &&
               observation.trusted_evidence_seen &&
               observation.voiced_ms >=
                   NUWAX_SHORT_SPEECH_CONVERGENCE_VOICED_MS &&
               observation.trailing_silence_ms >=
                   NUWAX_SHORT_SPEECH_CONVERGENCE_SILENCE_MS) {
        observation.reason = NUWAX_SHORT_SPEECH_CONFIRM_CONVERGENCE;
    }

    if (observation.reason != NUWAX_SHORT_SPEECH_CONFIRM_NONE) {
        tracker->confirmed = true;
        observation.confirmed_now = true;
        return observation;
    }

    const uint32_t reset_silence_ms = observation.trusted_evidence_seen &&
                                              observation.voiced_ms >=
                                                  NUWAX_SHORT_SPEECH_CONVERGENCE_VOICED_MS
                                          ? NUWAX_SHORT_SPEECH_TRUSTED_RESET_SILENCE_MS
                                          : NUWAX_SHORT_SPEECH_NORMAL_RESET_SILENCE_MS;
    const uint32_t silence_run_ms =
        (uint32_t)tracker->silence_run_frames * frame_ms;
    if (!voiced && silence_run_ms >= reset_silence_ms) {
        nuwax_short_speech_tracker_reset(tracker);
        observation.candidate_active = false;
        observation.candidate_reset = true;
    }
    return observation;
}

const char *nuwax_short_speech_confirmation_reason_name(
    nuwax_short_speech_confirmation_reason_t reason) {
    switch (reason) {
        case NUWAX_SHORT_SPEECH_CONFIRM_NEARFIELD_STRONG:
            return "nearfield_strong";
        case NUWAX_SHORT_SPEECH_CONFIRM_STRONG_ENERGY:
            return "strong_energy";
        case NUWAX_SHORT_SPEECH_CONFIRM_MIXED:
            return "mixed";
        case NUWAX_SHORT_SPEECH_CONFIRM_WEAK:
            return "weak";
        case NUWAX_SHORT_SPEECH_CONFIRM_CONVERGENCE:
            return "trusted_convergence";
        case NUWAX_SHORT_SPEECH_CONFIRM_NONE:
        default:
            return "none";
    }
}
