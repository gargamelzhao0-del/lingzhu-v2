#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define NUWAX_EMOTION_UI_NAME_CAPACITY 24
#define NUWAX_EMOTION_UI_SOURCE_CAPACITY 24

typedef struct {
    char emotion[NUWAX_EMOTION_UI_NAME_CAPACITY];
    char source[NUWAX_EMOTION_UI_SOURCE_CAPACITY];
    uint32_t revision;
    uint32_t duration_ms;
    bool finite_lease;
    bool pending;
} nuwax_emotion_ui_pending_t;

static inline void nuwax_emotion_ui_copy(char *dst,
                                         size_t dst_size,
                                         const char *src) {
    if (!dst || dst_size == 0) {
        return;
    }
    const char *value = src ? src : "";
    size_t length = strlen(value);
    if (length >= dst_size) {
        length = dst_size - 1;
    }
    memcpy(dst, value, length);
    dst[length] = '\0';
}

static inline uint32_t nuwax_emotion_ui_submit_leased(
    nuwax_emotion_ui_pending_t *state,
    const char *emotion,
    const char *source,
    uint32_t duration_ms,
    bool finite_lease) {
    if (!state || !emotion || emotion[0] == '\0') {
        return 0;
    }
    state->revision++;
    if (state->revision == 0) {
        state->revision++;
    }
    nuwax_emotion_ui_copy(state->emotion, sizeof(state->emotion), emotion);
    nuwax_emotion_ui_copy(state->source, sizeof(state->source), source);
    state->duration_ms = duration_ms;
    state->finite_lease = finite_lease;
    state->pending = true;
    return state->revision;
}

static inline uint32_t nuwax_emotion_ui_submit(
    nuwax_emotion_ui_pending_t *state,
    const char *emotion,
    const char *source) {
    return nuwax_emotion_ui_submit_leased(
        state, emotion, source, 0U, false);
}

static inline bool nuwax_emotion_ui_matches(
    const nuwax_emotion_ui_pending_t *state,
    uint32_t revision) {
    return state && state->pending && revision != 0 &&
           state->revision == revision;
}

/* Completion consumes pending work without revoking its published owner.
 * Only submit/invalidate advance the revision and make a publisher stale. */
static inline bool nuwax_emotion_ui_revision_current(
    const nuwax_emotion_ui_pending_t *state, uint32_t revision) {
    return state && revision != 0U && state->revision == revision;
}

static inline bool nuwax_emotion_ui_complete(
    nuwax_emotion_ui_pending_t *state,
    uint32_t revision) {
    if (!nuwax_emotion_ui_matches(state, revision)) {
        return false;
    }
    state->pending = false;
    return true;
}

static inline void nuwax_emotion_ui_invalidate(
    nuwax_emotion_ui_pending_t *state) {
    if (!state) {
        return;
    }
    state->revision++;
    if (state->revision == 0) {
        state->revision++;
    }
    state->emotion[0] = '\0';
    state->source[0] = '\0';
    state->duration_ms = 0U;
    state->finite_lease = false;
    state->pending = false;
}

static inline bool nuwax_emotion_ui_cancel_source(
    nuwax_emotion_ui_pending_t *state,
    const char *source) {
    if (!state || !state->pending || !source || source[0] == '\0' ||
        strcmp(state->source, source) != 0) {
        return false;
    }
    /* Advancing the revision invalidates a snapshot already copied by the UI
     * owner, while the exact source match protects unrelated cloud updates. */
    nuwax_emotion_ui_invalidate(state);
    return true;
}

static inline bool nuwax_emotion_ui_cancel_turn_scoped(
    nuwax_emotion_ui_pending_t *state) {
    if (!state || !state->pending ||
        strstr(state->source, "text_response") == NULL) {
        return false;
    }
    nuwax_emotion_ui_invalidate(state);
    return true;
}
