#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_state.h"

typedef struct {
    bool pending;
    nuwax_app_state_t state;
    uint32_t conversation_generation;
    uint32_t speaker_generation;
    uint32_t connection_epoch;
} nuwax_deferred_state_pending_t;

static inline void nuwax_deferred_state_submit(
    nuwax_deferred_state_pending_t *pending,
    nuwax_app_state_t state,
    uint32_t conversation_generation,
    uint32_t speaker_generation,
    uint32_t connection_epoch) {
    if (!pending) {
        return;
    }
    pending->pending = true;
    pending->state = state;
    pending->conversation_generation = conversation_generation;
    pending->speaker_generation = speaker_generation;
    pending->connection_epoch = connection_epoch;
}

static inline bool nuwax_deferred_state_owner_matches(
    const nuwax_deferred_state_pending_t *pending,
    nuwax_app_state_t current_state,
    uint32_t conversation_generation,
    uint32_t speaker_generation,
    uint32_t speaker_connection_epoch,
    uint32_t active_connection_epoch) {
    return pending && pending->pending &&
           current_state == NUWAX_APP_STATE_SPEAKING &&
           pending->conversation_generation == conversation_generation &&
           pending->speaker_generation != 0U &&
           pending->speaker_generation == speaker_generation &&
           pending->connection_epoch != 0U &&
           pending->connection_epoch == speaker_connection_epoch &&
           pending->connection_epoch == active_connection_epoch;
}

static inline void nuwax_deferred_state_clear(
    nuwax_deferred_state_pending_t *pending) {
    if (!pending) {
        return;
    }
    *pending = (nuwax_deferred_state_pending_t){0};
}
