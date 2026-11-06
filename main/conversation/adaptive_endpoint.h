#pragma once

#include <stdbool.h>
#include <stdint.h>

#define NUWAX_ADAPTIVE_ENDPOINT_MIN_COMMIT_MS 1200U
#define NUWAX_ADAPTIVE_ENDPOINT_TRUSTED_SHORT_COMMIT_MS 1200U
#define NUWAX_ADAPTIVE_ENDPOINT_SHORT_COMMIT_MS 1200U
#define NUWAX_ADAPTIVE_ENDPOINT_MEDIUM_COMMIT_MS 1700U
#define NUWAX_ADAPTIVE_ENDPOINT_MEDIUM_FIRST_PAUSE_COMMIT_MS 1800U
#define NUWAX_ADAPTIVE_ENDPOINT_LONG_COMMIT_MS 2200U
#define NUWAX_ADAPTIVE_ENDPOINT_VERY_LONG_COMMIT_MS 2500U
#define NUWAX_ADAPTIVE_ENDPOINT_MAX_COMMIT_MS 2500U
#define NUWAX_ADAPTIVE_ENDPOINT_UNKNOWN_HARD_COMMIT_MS 2600U
#define NUWAX_ADAPTIVE_ENDPOINT_MIXED_NOISE_HARD_COMMIT_MS 3200U
#define NUWAX_ADAPTIVE_ENDPOINT_UNCERTAIN_FORCE_COMMIT_MS \
    NUWAX_ADAPTIVE_ENDPOINT_UNKNOWN_HARD_COMMIT_MS
#define NUWAX_ADAPTIVE_ENDPOINT_TRUSTED_UNKNOWN_COMMIT_MS \
    NUWAX_ADAPTIVE_ENDPOINT_UNKNOWN_HARD_COMMIT_MS
#define NUWAX_ADAPTIVE_ENDPOINT_STRICT_TAIL_MS 300U
#define NUWAX_ADAPTIVE_ENDPOINT_RESUME_CONFIRM_MS 80U
#define NUWAX_ADAPTIVE_ENDPOINT_WEAK_RESUME_CONFIRM_MS 80U
#define NUWAX_ADAPTIVE_ENDPOINT_ENERGY_RESUME_CONFIRM_MS 120U
#define NUWAX_ADAPTIVE_ENDPOINT_POST_WEAK_STABLE_TAIL_MS 300U
#define NUWAX_ADAPTIVE_ENDPOINT_PAUSE_MARGIN_MS 200U
#define NUWAX_ADAPTIVE_ENDPOINT_SHORT_VOICED_MAX_MS 1200U
#define NUWAX_ADAPTIVE_ENDPOINT_SHORT_SPAN_MAX_MS 1800U
#define NUWAX_ADAPTIVE_ENDPOINT_MEDIUM_VOICED_MAX_MS 3200U
#define NUWAX_ADAPTIVE_ENDPOINT_MEDIUM_SPAN_MAX_MS 6000U
#define NUWAX_ADAPTIVE_ENDPOINT_LONG_VOICED_MAX_MS 12000U
#define NUWAX_ADAPTIVE_ENDPOINT_LONG_SPAN_MAX_MS 12000U
#define NUWAX_ADAPTIVE_ENDPOINT_MAX_FRAME_MS 100U

_Static_assert(NUWAX_ADAPTIVE_ENDPOINT_MIXED_NOISE_HARD_COMMIT_MS >
                   NUWAX_ADAPTIVE_ENDPOINT_UNKNOWN_HARD_COMMIT_MS,
               "mixed-noise hard bound must preserve the unknown fast path");

typedef enum {
    NUWAX_ADAPTIVE_ENDPOINT_FRAME_STRONG_SPEECH = 0,
    NUWAX_ADAPTIVE_ENDPOINT_FRAME_WEAK_SPEECH,
    NUWAX_ADAPTIVE_ENDPOINT_FRAME_STABLE_NOISE,
    NUWAX_ADAPTIVE_ENDPOINT_FRAME_UNKNOWN,
    NUWAX_ADAPTIVE_ENDPOINT_FRAME_SPEECH =
        NUWAX_ADAPTIVE_ENDPOINT_FRAME_STRONG_SPEECH,
    NUWAX_ADAPTIVE_ENDPOINT_FRAME_STRICT_SILENCE =
        NUWAX_ADAPTIVE_ENDPOINT_FRAME_STABLE_NOISE,
    NUWAX_ADAPTIVE_ENDPOINT_FRAME_UNCERTAIN =
        NUWAX_ADAPTIVE_ENDPOINT_FRAME_UNKNOWN,
} nuwax_adaptive_endpoint_frame_class_t;

typedef enum {
    NUWAX_ADAPTIVE_ENDPOINT_NONE = 0,
    NUWAX_ADAPTIVE_ENDPOINT_CANDIDATE,
    NUWAX_ADAPTIVE_ENDPOINT_COMMIT,
} nuwax_adaptive_endpoint_decision_t;

typedef enum {
    NUWAX_ADAPTIVE_ENDPOINT_RESUME_NONE = 0,
    NUWAX_ADAPTIVE_ENDPOINT_RESUME_STRONG,
    NUWAX_ADAPTIVE_ENDPOINT_RESUME_TRUSTED_WEAK,
    NUWAX_ADAPTIVE_ENDPOINT_RESUME_ENERGY_WEAK,
} nuwax_adaptive_endpoint_resume_kind_t;

typedef struct {
    bool strong_speech;
    bool trusted_weak_speech;
    bool energy_weak_speech;
} nuwax_adaptive_endpoint_resume_evidence_t;

typedef struct {
    uint32_t voiced_ms;
    uint32_t elapsed_since_first_speech_ms;
    uint32_t utterance_span_ms;
    /* Effective non-candidate time, retained for pause learning/diagnostics. */
    uint32_t pause_ms;
    /* Every frame since this pause began, including unconfirmed candidates. */
    uint32_t pause_elapsed_ms;
    uint32_t strict_silence_tail_ms;
    uint32_t pending_resumed_pause_ms;
    uint32_t max_resumed_pause_ms;
    uint16_t frozen_commit_ms;
    uint16_t resume_speech_streak_ms;
    uint16_t weak_speech_streak_ms;
    uint16_t unknown_streak_ms;
    uint16_t candidate_cancel_count;
    uint8_t resume_candidate_kind;
    bool speech_started;
    bool pause_active;
    bool predicted_commit;
    bool would_have_truncated;
    bool pending_prediction_cancel;
    bool uncertain_pause_seen;
    bool weak_pause_seen;
    bool trusted_short_utterance;
    bool policy_v2_enabled;
} nuwax_adaptive_endpoint_t;

typedef struct {
    nuwax_adaptive_endpoint_decision_t decision;
    uint16_t commit_ms;
    uint16_t candidate_ms;
    bool valid;
    bool prediction_started;
    bool prediction_canceled;
    bool resumed_pause_learned;
    bool truncation_confirmed;
    bool resume_candidate_active;
} nuwax_adaptive_endpoint_observation_t;

_Static_assert(sizeof(nuwax_adaptive_endpoint_t) <= 64U,
               "adaptive endpoint state must remain stack-safe");

void nuwax_adaptive_endpoint_reset(nuwax_adaptive_endpoint_t *endpoint);

nuwax_adaptive_endpoint_observation_t nuwax_adaptive_endpoint_seed(
    nuwax_adaptive_endpoint_t *endpoint,
    uint32_t voiced_ms,
    uint32_t utterance_span_ms,
    uint32_t trailing_stable_silence_ms,
    bool trusted_short_utterance,
    uint32_t frame_ms);

bool nuwax_adaptive_endpoint_state_valid(
    const nuwax_adaptive_endpoint_t *endpoint,
    uint32_t frame_ms);

nuwax_adaptive_endpoint_observation_t nuwax_adaptive_endpoint_observe(
    nuwax_adaptive_endpoint_t *endpoint,
    nuwax_adaptive_endpoint_frame_class_t frame_class,
    uint32_t frame_ms);

nuwax_adaptive_endpoint_observation_t nuwax_adaptive_endpoint_observe_v3(
    nuwax_adaptive_endpoint_t *endpoint,
    nuwax_adaptive_endpoint_frame_class_t frame_class,
    const nuwax_adaptive_endpoint_resume_evidence_t *resume_evidence,
    uint32_t frame_ms);
