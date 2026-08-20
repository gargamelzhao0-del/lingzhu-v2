#pragma once

#include <stdbool.h>
#include <stdint.h>

#define NUWAX_SHORT_SPEECH_WINDOW_MS 480U
#define NUWAX_SHORT_SPEECH_NEARFIELD_STRONG_MS 180U
#define NUWAX_SHORT_SPEECH_STRONG_MS 220U
#define NUWAX_SHORT_SPEECH_MIXED_MS 160U
#define NUWAX_SHORT_SPEECH_TRUSTED_WEAK_MS 180U
#define NUWAX_SHORT_SPEECH_UNTRUSTED_WEAK_MS 180U
#define NUWAX_SHORT_SPEECH_CONVERGENCE_VOICED_MS 100U
#define NUWAX_SHORT_SPEECH_CONVERGENCE_SILENCE_MS 300U
#define NUWAX_SHORT_SPEECH_NORMAL_RESET_SILENCE_MS 200U
#define NUWAX_SHORT_SPEECH_TRUSTED_RESET_SILENCE_MS 320U
#define NUWAX_SHORT_SPEECH_MAX_FRAME_MS 100U

typedef enum {
    NUWAX_SHORT_SPEECH_CONFIRM_NONE = 0,
    NUWAX_SHORT_SPEECH_CONFIRM_NEARFIELD_STRONG,
    NUWAX_SHORT_SPEECH_CONFIRM_STRONG_ENERGY,
    NUWAX_SHORT_SPEECH_CONFIRM_MIXED,
    NUWAX_SHORT_SPEECH_CONFIRM_WEAK,
    NUWAX_SHORT_SPEECH_CONFIRM_CONVERGENCE,
} nuwax_short_speech_confirmation_reason_t;

typedef struct {
    uint32_t weak_bits;
    uint32_t strong_bits;
    uint32_t trusted_bits;
    uint32_t first_voiced_sequence;
    uint32_t last_voiced_sequence;
    uint32_t ws_epoch;
    uint16_t candidate_span_frames;
    uint16_t silence_run_frames;
    uint16_t stable_silence_run_frames;
    uint16_t strong_run_frames;
    uint16_t trusted_strong_run_frames;
    uint8_t observed_frames;
    bool candidate_active;
    bool confirmed;
} nuwax_short_speech_tracker_t;

typedef struct {
    uint16_t voiced_ms;
    uint16_t strong_ms;
    uint16_t weak_ms;
    uint16_t candidate_span_ms;
    uint16_t trailing_silence_ms;
    uint32_t first_voiced_sequence;
    uint32_t last_voiced_sequence;
    nuwax_short_speech_confirmation_reason_t reason;
    bool valid;
    bool candidate_active;
    bool candidate_started;
    bool candidate_reset;
    bool confirmed_now;
    bool trusted_evidence_seen;
} nuwax_short_speech_observation_t;

_Static_assert(sizeof(nuwax_short_speech_tracker_t) <= 64U,
               "short speech tracker must remain stack-safe");

void nuwax_short_speech_tracker_reset(
    nuwax_short_speech_tracker_t *tracker);

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
    uint32_t frame_ms);

const char *nuwax_short_speech_confirmation_reason_name(
    nuwax_short_speech_confirmation_reason_t reason);
