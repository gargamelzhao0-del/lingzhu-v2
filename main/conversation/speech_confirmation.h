#pragma once

#include <stdbool.h>
#include <stdint.h>

#define NUWAX_SPEECH_CONFIRMATION_MAX_WINDOW_FRAMES 32U

typedef struct {
    uint16_t window_ms;
    uint16_t required_voiced_ms;
    uint16_t candidate_reset_silence_ms;
} nuwax_speech_confirmation_policy_t;

typedef struct {
    uint32_t speech_bits;
    uint16_t candidate_span_frames;
    uint8_t observed_frames;
    uint8_t voiced_frames;
    uint8_t silence_run_frames;
    bool candidate_active;
    bool confirmed;
} nuwax_speech_confirmation_t;

typedef struct {
    uint16_t voiced_ms;
    uint16_t candidate_span_ms;
    bool valid;
    bool candidate_active;
    bool candidate_started;
    bool candidate_reset;
    bool confirmed_now;
} nuwax_speech_confirmation_observation_t;

_Static_assert(sizeof(nuwax_speech_confirmation_t) <= 24U,
               "speech confirmation state must remain stack-safe");

void nuwax_speech_confirmation_reset(
    nuwax_speech_confirmation_t *state);

nuwax_speech_confirmation_observation_t
nuwax_speech_confirmation_observe(
    nuwax_speech_confirmation_t *state,
    const nuwax_speech_confirmation_policy_t *policy,
    bool frame_is_speech,
    uint32_t frame_ms);

/* Count only frame_is_speech toward the voiced requirement. A continuity
 * frame keeps an already active candidate from being reset as silence but
 * cannot start or confirm a candidate by itself. */
nuwax_speech_confirmation_observation_t
nuwax_speech_confirmation_observe_with_continuity(
    nuwax_speech_confirmation_t *state,
    const nuwax_speech_confirmation_policy_t *policy,
    bool frame_is_speech,
    bool frame_has_continuity,
    uint32_t frame_ms);
