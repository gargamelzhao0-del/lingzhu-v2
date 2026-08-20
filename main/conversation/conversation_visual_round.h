#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define NUWAX_VISUAL_ROUND_REQUEST_ID_CAPACITY 48
#define NUWAX_VISUAL_ROUND_EMOTION_CAPACITY 24

typedef enum {
    NUWAX_VISUAL_ROUND_NONE = 0,
    NUWAX_VISUAL_ROUND_NORMAL,
    NUWAX_VISUAL_ROUND_WAKE_GREETING,
    NUWAX_VISUAL_ROUND_HEAD_TOUCH,
    NUWAX_VISUAL_ROUND_SHAKE,
} nuwax_visual_round_kind_t;

typedef struct {
    nuwax_visual_round_kind_t kind;
    uint32_t ws_epoch;
    uint32_t awake_generation;
    uint32_t speaker_generation;
    bool first_pcm_seen;
    bool emotion_pending;
    char request_id[NUWAX_VISUAL_ROUND_REQUEST_ID_CAPACITY];
    char emotion[NUWAX_VISUAL_ROUND_EMOTION_CAPACITY];
} nuwax_conversation_visual_round_t;

static inline void nuwax_visual_round_copy(char *dst,
                                           size_t dst_size,
                                           const char *src) {
    if (!dst || dst_size == 0U) {
        return;
    }
    const char *value = src ? src : "";
    size_t length = strlen(value);
    if (length >= dst_size) {
        length = dst_size - 1U;
    }
    memcpy(dst, value, length);
    dst[length] = '\0';
}

static inline void nuwax_conversation_visual_round_clear(
    nuwax_conversation_visual_round_t *round) {
    if (!round) {
        return;
    }
    memset(round, 0, sizeof(*round));
}

static inline bool nuwax_conversation_visual_round_begin(
    nuwax_conversation_visual_round_t *round,
    nuwax_visual_round_kind_t kind,
    uint32_t ws_epoch,
    uint32_t awake_generation,
    const char *request_id) {
    if (!round || kind == NUWAX_VISUAL_ROUND_NONE || ws_epoch == 0U ||
        awake_generation == 0U) {
        return false;
    }
    nuwax_conversation_visual_round_clear(round);
    round->kind = kind;
    round->ws_epoch = ws_epoch;
    round->awake_generation = awake_generation;
    nuwax_visual_round_copy(round->request_id,
                            sizeof(round->request_id),
                            request_id);
    return true;
}

static inline bool nuwax_conversation_visual_round_is_active(
    const nuwax_conversation_visual_round_t *round) {
    return round && round->kind != NUWAX_VISUAL_ROUND_NONE;
}

static inline bool nuwax_conversation_visual_round_is_interaction(
    const nuwax_conversation_visual_round_t *round) {
    return round &&
           (round->kind == NUWAX_VISUAL_ROUND_HEAD_TOUCH ||
            round->kind == NUWAX_VISUAL_ROUND_SHAKE);
}

static inline bool nuwax_conversation_visual_round_context_matches(
    const nuwax_conversation_visual_round_t *round,
    uint32_t ws_epoch,
    uint32_t awake_generation) {
    return nuwax_conversation_visual_round_is_active(round) &&
           round->ws_epoch == ws_epoch &&
           round->awake_generation == awake_generation;
}

static inline bool nuwax_conversation_visual_round_request_matches(
    const nuwax_conversation_visual_round_t *round,
    const char *request_id) {
    return nuwax_conversation_visual_round_is_active(round) &&
           round->request_id[0] != '\0' && request_id &&
           request_id[0] != '\0' &&
           strcmp(round->request_id, request_id) == 0;
}

static inline bool nuwax_conversation_visual_round_note_first_pcm(
    nuwax_conversation_visual_round_t *round,
    uint32_t ws_epoch,
    uint32_t awake_generation,
    uint32_t speaker_generation) {
    if (!nuwax_conversation_visual_round_context_matches(
            round, ws_epoch, awake_generation) ||
        speaker_generation == 0U) {
        return false;
    }
    if (round->speaker_generation != 0U &&
        round->speaker_generation != speaker_generation) {
        return false;
    }
    round->speaker_generation = speaker_generation;
    round->first_pcm_seen = true;
    return true;
}

static inline bool nuwax_conversation_visual_round_speaker_matches(
    const nuwax_conversation_visual_round_t *round,
    uint32_t ws_epoch,
    uint32_t speaker_generation) {
    return nuwax_conversation_visual_round_is_active(round) &&
           round->first_pcm_seen && round->ws_epoch == ws_epoch &&
           round->speaker_generation != 0U &&
           round->speaker_generation == speaker_generation;
}

static inline bool nuwax_conversation_visual_round_hold_emotion(
    nuwax_conversation_visual_round_t *round,
    const char *emotion) {
    if (!round || round->kind != NUWAX_VISUAL_ROUND_NORMAL || !emotion ||
        emotion[0] == '\0' || round->first_pcm_seen) {
        return false;
    }
    nuwax_visual_round_copy(round->emotion, sizeof(round->emotion), emotion);
    round->emotion_pending = true;
    return true;
}

static inline bool nuwax_conversation_visual_round_take_emotion(
    nuwax_conversation_visual_round_t *round,
    char *emotion,
    size_t emotion_size) {
    if (!round || !round->emotion_pending || !emotion ||
        emotion_size == 0U) {
        return false;
    }
    nuwax_visual_round_copy(emotion, emotion_size, round->emotion);
    round->emotion[0] = '\0';
    round->emotion_pending = false;
    return true;
}
