#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    NUWAX_SPEAKER_STREAM_OK = 0,
    NUWAX_SPEAKER_STREAM_INACTIVE,
    NUWAX_SPEAKER_STREAM_WRONG_GENERATION,
    NUWAX_SPEAKER_STREAM_INVALID_FLAGS,
    NUWAX_SPEAKER_STREAM_START_REQUIRED,
    NUWAX_SPEAKER_STREAM_INVALID_START,
    NUWAX_SPEAKER_STREAM_POST_END,
    NUWAX_SPEAKER_STREAM_BAD_SEQUENCE,
    NUWAX_SPEAKER_STREAM_BAD_FRAME_SAMPLES,
    NUWAX_SPEAKER_STREAM_TOTAL_EXCEEDED,
    NUWAX_SPEAKER_STREAM_TOTAL_MISMATCH,
} nuwax_speaker_stream_result_t;

typedef struct {
    bool active;
    bool started;
    bool end_accepted;
    uint32_t generation;
    uint32_t next_sequence;
    size_t frame_samples;
    uint32_t pcm_frames;
    uint64_t pcm_bytes;
    uint32_t expected_frames;
    uint32_t expected_bytes;
} nuwax_speaker_stream_policy_t;

/* Binary audio has no turn/playback identifier. After a canceled cloud turn,
 * tail PCM or an END-only marker can legally arrive while the next ASR turn
 * already owns the WebSocket. Without an admitted START there is no speaker
 * generation to fault, so the frame is stale transport residue and must not
 * invalidate the new turn. */
static inline bool nuwax_speaker_stream_frame_is_orphan(bool start,
                                                        bool stream_active) {
    return !start && !stream_active;
}

static inline void nuwax_speaker_stream_reset(
    nuwax_speaker_stream_policy_t *policy) {
    if (policy) {
        *policy = (nuwax_speaker_stream_policy_t){0};
    }
}

static inline void nuwax_speaker_stream_begin_generation(
    nuwax_speaker_stream_policy_t *policy,
    uint32_t generation,
    uint32_t expected_frames,
    uint32_t expected_bytes) {
    if (!policy) {
        return;
    }
    *policy = (nuwax_speaker_stream_policy_t){
        .active = generation != 0U,
        .generation = generation,
        .expected_frames = expected_frames,
        .expected_bytes = expected_bytes,
    };
}

static inline bool nuwax_speaker_stream_bind_expected(
    nuwax_speaker_stream_policy_t *policy,
    uint32_t generation,
    uint32_t expected_frames,
    uint32_t expected_bytes) {
    if (!policy || !policy->active || generation == 0U ||
        policy->generation != generation) {
        return false;
    }
    if ((expected_frames != 0U && policy->expected_frames != 0U &&
         expected_frames != policy->expected_frames) ||
        (expected_bytes != 0U && policy->expected_bytes != 0U &&
         expected_bytes != policy->expected_bytes)) {
        return false;
    }
    if (expected_frames != 0U) {
        policy->expected_frames = expected_frames;
    }
    if (expected_bytes != 0U) {
        policy->expected_bytes = expected_bytes;
    }
    if ((policy->expected_frames != 0U &&
         policy->pcm_frames > policy->expected_frames) ||
        (policy->expected_bytes != 0U &&
         policy->pcm_bytes > policy->expected_bytes)) {
        return false;
    }
    if (policy->end_accepted &&
        ((policy->expected_frames != 0U &&
          policy->pcm_frames != policy->expected_frames) ||
         (policy->expected_bytes != 0U &&
          policy->pcm_bytes != policy->expected_bytes))) {
        return false;
    }
    return true;
}

static inline nuwax_speaker_stream_result_t
nuwax_speaker_stream_validate(const nuwax_speaker_stream_policy_t *policy,
                              uint32_t generation,
                              uint32_t sequence,
                              uint8_t flags,
                              uint8_t allowed_flags,
                              uint8_t start_flag,
                              uint8_t end_flag,
                              size_t sample_count) {
    if (!policy || !policy->active) {
        return NUWAX_SPEAKER_STREAM_INACTIVE;
    }
    if (generation == 0U || policy->generation != generation) {
        return NUWAX_SPEAKER_STREAM_WRONG_GENERATION;
    }
    if ((flags & (uint8_t)~allowed_flags) != 0U) {
        return NUWAX_SPEAKER_STREAM_INVALID_FLAGS;
    }
    if (policy->end_accepted) {
        return NUWAX_SPEAKER_STREAM_POST_END;
    }

    const bool start = (flags & start_flag) != 0U;
    const bool end = (flags & end_flag) != 0U;
    if (!policy->started) {
        if (!start) {
            return NUWAX_SPEAKER_STREAM_START_REQUIRED;
        }
        if (sequence != 0U || sample_count == 0U) {
            return NUWAX_SPEAKER_STREAM_INVALID_START;
        }
    } else {
        if (start) {
            return NUWAX_SPEAKER_STREAM_INVALID_START;
        }
        if (sequence != policy->next_sequence) {
            return NUWAX_SPEAKER_STREAM_BAD_SEQUENCE;
        }
        if (sample_count == 0U && !end) {
            return NUWAX_SPEAKER_STREAM_BAD_FRAME_SAMPLES;
        }
        /* The cloud packetization cadence is fixed for a generation. A
         * shorter final PCM frame is required for non-aligned streaming TTS
         * tails; all other PCM size changes are protocol errors. */
        if (sample_count > 0U && sample_count != policy->frame_samples &&
            !(end && sample_count < policy->frame_samples)) {
            return NUWAX_SPEAKER_STREAM_BAD_FRAME_SAMPLES;
        }
    }

    const uint64_t next_frames =
        (uint64_t)policy->pcm_frames + (sample_count > 0U ? 1ULL : 0ULL);
    const uint64_t next_bytes =
        policy->pcm_bytes + (uint64_t)sample_count * sizeof(int16_t);
    if ((policy->expected_frames != 0U &&
         next_frames > policy->expected_frames) ||
        (policy->expected_bytes != 0U &&
         next_bytes > policy->expected_bytes)) {
        return NUWAX_SPEAKER_STREAM_TOTAL_EXCEEDED;
    }
    if (end &&
        ((policy->expected_frames != 0U &&
          next_frames != policy->expected_frames) ||
         (policy->expected_bytes != 0U &&
          next_bytes != policy->expected_bytes))) {
        return NUWAX_SPEAKER_STREAM_TOTAL_MISMATCH;
    }
    return NUWAX_SPEAKER_STREAM_OK;
}

static inline void nuwax_speaker_stream_commit(
    nuwax_speaker_stream_policy_t *policy,
    uint32_t sequence,
    uint8_t flags,
    uint8_t end_flag,
    size_t sample_count) {
    if (!policy) {
        return;
    }
    if (!policy->started) {
        policy->started = true;
        policy->frame_samples = sample_count;
    }
    policy->next_sequence = sequence + 1U;
    if (sample_count > 0U) {
        policy->pcm_frames++;
        policy->pcm_bytes += (uint64_t)sample_count * sizeof(int16_t);
    }
    policy->end_accepted = (flags & end_flag) != 0U;
}
