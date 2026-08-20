#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    NUWAX_CONVERSATION_ENDPOINT_NONE = 0,
    NUWAX_CONVERSATION_ENDPOINT_CANDIDATE,
    NUWAX_CONVERSATION_ENDPOINT_COMMIT,
} nuwax_conversation_endpoint_decision_t;

typedef struct {
    uint8_t consecutive_frames;
    uint16_t weak_only_frames;
} nuwax_conversation_weak_continuation_t;

typedef struct {
    uint16_t consecutive_ms;
} nuwax_conversation_correlated_weak_t;

#define NUWAX_CONVERSATION_NEAREND_WEAK_THRESHOLD_PERCENT 72U
#define NUWAX_CONVERSATION_UTTERANCE_SOFT_LIMIT_MS 45000U
#define NUWAX_CONVERSATION_UTTERANCE_EXTENSION_MS 15000U
#define NUWAX_CONVERSATION_UTTERANCE_RECENT_CONFIRMED_MS 4000U
#define NUWAX_CONVERSATION_CORRELATED_WEAK_CONFIRM_MS 240U

static inline uint32_t nuwax_conversation_utterance_next_limit_ms(
    uint32_t current_limit_ms,
    uint32_t confirmed_speech_age_ms,
    bool transport_healthy,
    bool uplink_queue_healthy) {
    if (confirmed_speech_age_ms >
            NUWAX_CONVERSATION_UTTERANCE_RECENT_CONFIRMED_MS ||
        !transport_healthy || !uplink_queue_healthy) {
        return current_limit_ms;
    }
    return UINT32_MAX - current_limit_ms <
                   NUWAX_CONVERSATION_UTTERANCE_EXTENSION_MS
               ? UINT32_MAX
               : current_limit_ms +
                     NUWAX_CONVERSATION_UTTERANCE_EXTENSION_MS;
}

static inline uint32_t nuwax_conversation_nearend_weak_threshold(
    uint32_t threshold) {
    return (uint32_t)(((uint64_t)threshold *
                       NUWAX_CONVERSATION_NEAREND_WEAK_THRESHOLD_PERCENT +
                       99U) /
                      100U);
}

static inline bool nuwax_conversation_weak_start_eligible(
    bool persistent_conversation_mode,
    bool during_playback_mode,
    bool playback_active,
    bool acceptance_capture_enabled,
    bool speech_confirmed,
    bool followup_warmup) {
    return persistent_conversation_mode && !during_playback_mode &&
           !playback_active && !acceptance_capture_enabled &&
           !speech_confirmed && !followup_warmup;
}

static inline bool nuwax_conversation_weak_level_evidence(
    uint32_t avg_abs,
    uint32_t peak,
    uint32_t before_avg_abs,
    uint32_t noise_floor_avg_abs,
    uint32_t fixed_guard_avg_abs,
    uint32_t fixed_guard_peak) {
    const uint32_t tuned_fixed_avg_threshold =
        nuwax_conversation_nearend_weak_threshold(fixed_guard_avg_abs);
    const uint32_t tuned_fixed_peak_threshold =
        nuwax_conversation_nearend_weak_threshold(fixed_guard_peak);
    if (avg_abs >= tuned_fixed_avg_threshold &&
        peak >= tuned_fixed_peak_threshold) {
        return true;
    }
    if (noise_floor_avg_abs == 0U) {
        return false;
    }
    const uint32_t relative_avg_threshold =
        nuwax_conversation_nearend_weak_threshold(
            noise_floor_avg_abs + noise_floor_avg_abs / 2U + 10U);
    const uint32_t relative_peak_threshold =
        nuwax_conversation_nearend_weak_threshold(
            noise_floor_avg_abs * 3U);
    return avg_abs >= relative_avg_threshold &&
           peak >= relative_peak_threshold &&
           before_avg_abs >= relative_avg_threshold;
}

static inline bool nuwax_conversation_weak_start_level_evidence(
    uint32_t avg_abs,
    uint32_t peak,
    uint32_t before_avg_abs,
    uint32_t noise_floor_avg_abs,
    uint32_t fixed_guard_avg_abs,
    uint32_t fixed_guard_peak) {
    const uint32_t fixed_avg_threshold =
        nuwax_conversation_nearend_weak_threshold(
        fixed_guard_avg_abs * 2U / 5U);
    const uint32_t fixed_peak_threshold =
        nuwax_conversation_nearend_weak_threshold(
        fixed_guard_peak * 5U / 12U);
    if (avg_abs >= fixed_avg_threshold &&
        peak >= fixed_peak_threshold &&
        before_avg_abs >= fixed_avg_threshold) {
        return true;
    }
    if (noise_floor_avg_abs == 0U) {
        return false;
    }
    const uint32_t relative_avg_threshold =
        nuwax_conversation_nearend_weak_threshold(
            noise_floor_avg_abs * 2U + 20U);
    const uint32_t relative_peak_threshold =
        nuwax_conversation_nearend_weak_threshold(
            noise_floor_avg_abs * 5U);
    return avg_abs >= relative_avg_threshold &&
           peak >= relative_peak_threshold &&
           before_avg_abs >= relative_avg_threshold;
}

static inline bool nuwax_conversation_weak_start_frame_evidence(
    bool primary_frame_is_speech,
    bool followup_warmup,
    bool weak_level_evidence,
    bool dual_mic_aec_hint,
    bool dual_mic_cleaner_hint,
    bool dual_mic_voice_hint,
    bool secondary_mic_speech_hint) {
    return !primary_frame_is_speech && !followup_warmup &&
           (weak_level_evidence || dual_mic_aec_hint ||
            dual_mic_cleaner_hint || dual_mic_voice_hint ||
            secondary_mic_speech_hint);
}

static inline void nuwax_conversation_correlated_weak_reset(
    nuwax_conversation_correlated_weak_t *state) {
    if (state) {
        state->consecutive_ms = 0U;
    }
}

static inline bool nuwax_conversation_correlated_weak_observe(
    nuwax_conversation_correlated_weak_t *state,
    bool eligible,
    bool primary_weak_level,
    bool secondary_mic_current_evidence,
    bool capture_contiguous,
    bool echo_free,
    uint32_t frame_ms) {
    if (!state || !eligible || !primary_weak_level ||
        !secondary_mic_current_evidence || !capture_contiguous || !echo_free ||
        frame_ms == 0U) {
        nuwax_conversation_correlated_weak_reset(state);
        return false;
    }
    const uint32_t next_ms = (uint32_t)state->consecutive_ms + frame_ms;
    state->consecutive_ms = next_ms > UINT16_MAX
                                ? UINT16_MAX
                                : (uint16_t)next_ms;
    return state->consecutive_ms >=
           NUWAX_CONVERSATION_CORRELATED_WEAK_CONFIRM_MS;
}

static inline bool nuwax_conversation_weak_continuation_observe(
    nuwax_conversation_weak_continuation_t *state,
    bool eligible,
    bool fresh_weak_speech_evidence,
    bool trusted_weak_speech_evidence,
    uint32_t confirmation_frames,
    uint32_t weak_only_counter_limit_frames) {
    if (!state || !eligible || confirmation_frames == 0U ||
        weak_only_counter_limit_frames == 0U) {
        if (state) {
            state->consecutive_frames = 0U;
            state->weak_only_frames = 0U;
        }
        return false;
    }
    if (!fresh_weak_speech_evidence) {
        state->consecutive_frames = 0U;
        return false;
    }
    if (trusted_weak_speech_evidence) {
        state->weak_only_frames = 0U;
    } else {
        if (state->weak_only_frames < weak_only_counter_limit_frames &&
            state->weak_only_frames < UINT16_MAX) {
            state->weak_only_frames++;
        }
    }
    if (state->consecutive_frames < UINT8_MAX) {
        state->consecutive_frames++;
    }
    if (state->consecutive_frames < confirmation_frames) {
        return false;
    }
    return true;
}

static inline void nuwax_conversation_weak_continuation_rearm(
    nuwax_conversation_weak_continuation_t *state) {
    if (state) {
        state->consecutive_frames = 0U;
        state->weak_only_frames = 0U;
    }
}

static inline bool nuwax_conversation_weak_continuation_pending(
    const nuwax_conversation_weak_continuation_t *state) {
    return state && state->consecutive_frames > 0U;
}

static inline bool nuwax_conversation_endpoint_gate_ready(
    bool upload_started,
    bool speech_confirmed,
    bool current_frame_is_speech,
    uint32_t captured_ms,
    uint32_t min_capture_ms) {
    return upload_started && speech_confirmed && !current_frame_is_speech &&
           captured_ms >= min_capture_ms;
}

static inline nuwax_conversation_endpoint_decision_t
nuwax_conversation_endpoint_decide(bool upload_started,
                                   bool speech_confirmed,
                                   bool current_frame_is_speech,
                                   uint32_t captured_ms,
                                   uint32_t silence_ms,
                                   uint32_t min_capture_ms,
                                   uint32_t candidate_silence_ms,
                                   uint32_t commit_silence_ms) {
    if (!nuwax_conversation_endpoint_gate_ready(upload_started,
                                                speech_confirmed,
                                                current_frame_is_speech,
                                                captured_ms,
                                                min_capture_ms)) {
        return NUWAX_CONVERSATION_ENDPOINT_NONE;
    }
    if (silence_ms >= commit_silence_ms) {
        return NUWAX_CONVERSATION_ENDPOINT_COMMIT;
    }
    if (silence_ms >= candidate_silence_ms) {
        return NUWAX_CONVERSATION_ENDPOINT_CANDIDATE;
    }
    return NUWAX_CONVERSATION_ENDPOINT_NONE;
}
