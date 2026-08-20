#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t idle_deadline_ms;
    uint32_t idle_timeout_ms;
    uint32_t paused_remaining_ms;
    uint32_t generation;
    bool active;
    bool waiting;
} nuwax_awake_session_state_t;

void nuwax_awake_session_state_clear(nuwax_awake_session_state_t *state);
void nuwax_awake_session_state_begin(nuwax_awake_session_state_t *state,
                                     uint32_t idle_timeout_ms);
void nuwax_awake_session_state_pause(nuwax_awake_session_state_t *state,
                                     uint32_t now_ms);
void nuwax_awake_session_state_resume_waiting(nuwax_awake_session_state_t *state,
                                              uint32_t now_ms);
void nuwax_awake_session_state_reset_waiting(nuwax_awake_session_state_t *state,
                                             uint32_t now_ms);
void nuwax_awake_session_state_reset_paused(nuwax_awake_session_state_t *state);
bool nuwax_awake_session_state_is_active(const nuwax_awake_session_state_t *state);
bool nuwax_awake_session_state_is_waiting(const nuwax_awake_session_state_t *state);
bool nuwax_awake_session_state_is_expired(const nuwax_awake_session_state_t *state,
                                          uint32_t now_ms);
uint32_t nuwax_awake_session_state_remaining_ms(const nuwax_awake_session_state_t *state,
                                                uint32_t now_ms);
uint32_t nuwax_awake_session_state_generation(const nuwax_awake_session_state_t *state);
bool nuwax_awake_session_state_generation_matches(
    const nuwax_awake_session_state_t *state,
    uint32_t generation);
bool nuwax_awake_session_state_can_enter_idle_sleep(
    const nuwax_awake_session_state_t *state,
    uint32_t now_ms,
    bool listening,
    bool turn_active,
    bool playback_active,
    bool speech_candidate_active);
bool nuwax_awake_session_state_can_commit_deferred_idle_sleep(
    const nuwax_awake_session_state_t *state,
    uint32_t request_generation,
    uint32_t now_ms,
    bool listening,
    bool turn_active,
    bool playback_active,
    bool speech_candidate_active);
