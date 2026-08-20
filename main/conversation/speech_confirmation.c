#include "speech_confirmation.h"

#include <limits.h>
#include <string.h>

static uint32_t frames_ceil(uint32_t duration_ms, uint32_t frame_ms) {
    return (duration_ms + frame_ms - 1U) / frame_ms;
}

static uint16_t saturating_increment_u16(uint16_t value) {
    return value == UINT16_MAX ? UINT16_MAX : (uint16_t)(value + 1U);
}

static uint16_t frames_to_ms_u16(uint32_t frames, uint32_t frame_ms) {
    const uint32_t duration_ms = frames * frame_ms;
    return duration_ms > UINT16_MAX ? UINT16_MAX : (uint16_t)duration_ms;
}

static bool policy_valid(const nuwax_speech_confirmation_policy_t *policy,
                         uint32_t frame_ms,
                         uint32_t *window_frames_out,
                         uint32_t *required_frames_out,
                         uint32_t *reset_frames_out) {
    if (!policy || !window_frames_out || !required_frames_out ||
        !reset_frames_out || frame_ms == 0U || frame_ms > 100U ||
        policy->window_ms == 0U || policy->required_voiced_ms == 0U ||
        policy->candidate_reset_silence_ms == 0U) {
        return false;
    }

    const uint32_t window_frames = frames_ceil(policy->window_ms, frame_ms);
    const uint32_t required_frames =
        frames_ceil(policy->required_voiced_ms, frame_ms);
    const uint32_t reset_frames =
        frames_ceil(policy->candidate_reset_silence_ms, frame_ms);
    if (window_frames == 0U ||
        window_frames > NUWAX_SPEECH_CONFIRMATION_MAX_WINDOW_FRAMES ||
        required_frames == 0U || required_frames > window_frames ||
        reset_frames == 0U || reset_frames > window_frames) {
        return false;
    }

    *window_frames_out = window_frames;
    *required_frames_out = required_frames;
    *reset_frames_out = reset_frames;
    return true;
}

void nuwax_speech_confirmation_reset(
    nuwax_speech_confirmation_t *state) {
    if (state) {
        memset(state, 0, sizeof(*state));
    }
}

nuwax_speech_confirmation_observation_t
nuwax_speech_confirmation_observe_with_continuity(
    nuwax_speech_confirmation_t *state,
    const nuwax_speech_confirmation_policy_t *policy,
    bool frame_is_speech,
    bool frame_has_continuity,
    uint32_t frame_ms) {
    nuwax_speech_confirmation_observation_t observation = {0};
    uint32_t window_frames = 0U;
    uint32_t required_frames = 0U;
    uint32_t reset_frames = 0U;
    if (!state || !policy_valid(policy,
                                frame_ms,
                                &window_frames,
                                &required_frames,
                                &reset_frames)) {
        nuwax_speech_confirmation_reset(state);
        return observation;
    }
    observation.valid = true;

    const bool window_full = state->observed_frames >= window_frames;
    const uint32_t oldest_bit = window_frames - 1U;
    if (window_full && ((state->speech_bits >> oldest_bit) & 1U) != 0U &&
        state->voiced_frames > 0U) {
        state->voiced_frames--;
    }
    const uint32_t window_mask =
        window_frames == NUWAX_SPEECH_CONFIRMATION_MAX_WINDOW_FRAMES
            ? UINT32_MAX
            : ((1U << window_frames) - 1U);
    state->speech_bits =
        ((state->speech_bits << 1U) | (frame_is_speech ? 1U : 0U)) &
        window_mask;
    if (!window_full) {
        state->observed_frames++;
    }
    if (frame_is_speech && state->voiced_frames < UINT8_MAX) {
        state->voiced_frames++;
    }

    const bool candidate_continues =
        frame_is_speech || frame_has_continuity;
    if (frame_is_speech) {
        if (!state->candidate_active) {
            state->candidate_active = true;
            state->candidate_span_frames = 0U;
            observation.candidate_started = true;
        }
        state->silence_run_frames = 0U;
    } else if (state->candidate_active) {
        if (candidate_continues) {
            state->silence_run_frames = 0U;
        } else if (state->silence_run_frames < UINT8_MAX) {
            state->silence_run_frames++;
        }
    }

    if (state->candidate_active) {
        state->candidate_span_frames =
            saturating_increment_u16(state->candidate_span_frames);
    }
    if (!candidate_continues && state->candidate_active &&
        state->silence_run_frames >= reset_frames && !state->confirmed) {
        nuwax_speech_confirmation_reset(state);
        observation.candidate_reset = true;
        return observation;
    }

    if (frame_is_speech && !state->confirmed &&
        state->voiced_frames >= required_frames) {
        state->confirmed = true;
        observation.confirmed_now = true;
    }
    observation.candidate_active = state->candidate_active;
    observation.voiced_ms =
        frames_to_ms_u16(state->voiced_frames, frame_ms);
    observation.candidate_span_ms =
        frames_to_ms_u16(state->candidate_span_frames, frame_ms);
    return observation;
}

nuwax_speech_confirmation_observation_t
nuwax_speech_confirmation_observe(
    nuwax_speech_confirmation_t *state,
    const nuwax_speech_confirmation_policy_t *policy,
    bool frame_is_speech,
    uint32_t frame_ms) {
    return nuwax_speech_confirmation_observe_with_continuity(
        state, policy, frame_is_speech, frame_is_speech, frame_ms);
}
