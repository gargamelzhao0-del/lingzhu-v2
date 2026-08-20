#include "awake_session_state.h"

void nuwax_awake_session_state_clear(nuwax_awake_session_state_t *state) {
    if (!state) {
        return;
    }
    state->idle_deadline_ms = 0;
    state->idle_timeout_ms = 0;
    state->paused_remaining_ms = 0;
    state->active = false;
    state->waiting = false;
}

void nuwax_awake_session_state_begin(nuwax_awake_session_state_t *state,
                                     uint32_t idle_timeout_ms) {
    if (!state) {
        return;
    }
    if (idle_timeout_ms == 0) {
        nuwax_awake_session_state_clear(state);
        return;
    }
    state->active = false;
    state->waiting = false;
    state->generation++;
    if (state->generation == 0) {
        state->generation++;
    }
    state->idle_deadline_ms = 0;
    state->idle_timeout_ms = idle_timeout_ms;
    state->paused_remaining_ms = idle_timeout_ms;
    state->active = true;
}

void nuwax_awake_session_state_pause(nuwax_awake_session_state_t *state,
                                     uint32_t now_ms) {
    if (!state || !state->active || !state->waiting) {
        return;
    }
    const uint32_t deadline_ms = state->idle_deadline_ms;
    state->waiting = false;
    if ((int32_t)(now_ms - deadline_ms) >= 0) {
        state->paused_remaining_ms = 0;
    } else {
        state->paused_remaining_ms = deadline_ms - now_ms;
    }
    state->idle_deadline_ms = 0;
}

void nuwax_awake_session_state_resume_waiting(nuwax_awake_session_state_t *state,
                                              uint32_t now_ms) {
    if (!state || !state->active || state->waiting ||
        state->paused_remaining_ms == 0) {
        return;
    }
    state->idle_deadline_ms = now_ms + state->paused_remaining_ms;
    state->waiting = true;
}

void nuwax_awake_session_state_reset_waiting(nuwax_awake_session_state_t *state,
                                             uint32_t now_ms) {
    if (!state || !state->active || state->idle_timeout_ms == 0) {
        return;
    }
    state->paused_remaining_ms = state->idle_timeout_ms;
    state->idle_deadline_ms = now_ms + state->idle_timeout_ms;
    state->waiting = true;
}

void nuwax_awake_session_state_reset_paused(nuwax_awake_session_state_t *state) {
    if (!state || !state->active || state->idle_timeout_ms == 0) {
        return;
    }
    state->waiting = false;
    state->idle_deadline_ms = 0;
    state->paused_remaining_ms = state->idle_timeout_ms;
}

bool nuwax_awake_session_state_is_active(const nuwax_awake_session_state_t *state) {
    return state && state->active;
}

bool nuwax_awake_session_state_is_waiting(const nuwax_awake_session_state_t *state) {
    return state && state->active && state->waiting;
}

bool nuwax_awake_session_state_is_expired(const nuwax_awake_session_state_t *state,
                                          uint32_t now_ms) {
    return state && state->active && state->waiting &&
           (int32_t)(now_ms - state->idle_deadline_ms) >= 0;
}

uint32_t nuwax_awake_session_state_remaining_ms(const nuwax_awake_session_state_t *state,
                                                uint32_t now_ms) {
    if (!state || !state->active) {
        return 0;
    }
    if (!state->waiting) {
        return state->paused_remaining_ms;
    }
    if (nuwax_awake_session_state_is_expired(state, now_ms)) {
        return 0;
    }
    return state->idle_deadline_ms - now_ms;
}

uint32_t nuwax_awake_session_state_generation(const nuwax_awake_session_state_t *state) {
    return state ? state->generation : 0;
}

bool nuwax_awake_session_state_generation_matches(
    const nuwax_awake_session_state_t *state,
    uint32_t generation) {
    return state && state->active && generation != 0 &&
           state->generation == generation;
}

bool nuwax_awake_session_state_can_enter_idle_sleep(
    const nuwax_awake_session_state_t *state,
    uint32_t now_ms,
    bool listening,
    bool turn_active,
    bool playback_active,
    bool speech_candidate_active) {
    return listening && !turn_active && !playback_active &&
           !speech_candidate_active &&
           nuwax_awake_session_state_is_expired(state, now_ms);
}

bool nuwax_awake_session_state_can_commit_deferred_idle_sleep(
    const nuwax_awake_session_state_t *state,
    uint32_t request_generation,
    uint32_t now_ms,
    bool listening,
    bool turn_active,
    bool playback_active,
    bool speech_candidate_active) {
    return nuwax_awake_session_state_generation_matches(
               state, request_generation) &&
           nuwax_awake_session_state_can_enter_idle_sleep(
               state, now_ms, listening, turn_active, playback_active,
               speech_candidate_active);
}
