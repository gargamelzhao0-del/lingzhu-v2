#include "conversation_state.h"

bool nuwax_conversation_state_set(nuwax_conversation_state_t *state,
                                  nuwax_app_state_t next,
                                  uint32_t now_ms,
                                  uint32_t *out_generation) {
    if (!state) {
        return false;
    }
    if (state->initialized && state->current == next) {
        return false;
    }
    state->initialized = true;
    state->current = next;
    uint32_t generation = ++state->generation;
    if (next == NUWAX_APP_STATE_IDLE) {
        state->idle_since_ms = now_ms;
    } else if (next != NUWAX_APP_STATE_SLEEPING) {
        state->idle_since_ms = 0;
    }
    if (out_generation) {
        *out_generation = generation;
    }
    return true;
}

nuwax_app_state_t nuwax_conversation_state_current(const nuwax_conversation_state_t *state) {
    return state ? (nuwax_app_state_t)state->current : NUWAX_APP_STATE_BOOTING;
}

uint32_t nuwax_conversation_state_generation(const nuwax_conversation_state_t *state) {
    return state ? state->generation : 0;
}

uint32_t nuwax_conversation_state_idle_since_ms(const nuwax_conversation_state_t *state) {
    return state ? state->idle_since_ms : 0;
}

bool nuwax_conversation_state_idle_sleep_due(const nuwax_conversation_state_t *state,
                                             uint32_t now_ms,
                                             uint32_t timeout_ms) {
    if (!state || state->current != NUWAX_APP_STATE_IDLE || state->idle_since_ms == 0) {
        return false;
    }
    return (uint32_t)(now_ms - state->idle_since_ms) >= timeout_ms;
}
