#pragma once

#include <stdbool.h>
#include <stdint.h>

#define NUWAX_LISTEN_RESUME_MODE_MAX_LEN 24
#define NUWAX_LISTEN_RESUME_AUDIO_SESSION_ID_MAX_LEN 48

typedef struct {
    bool no_speech_resumable;
    uint32_t deadline_ms;
    uint32_t awake_generation;
    char mode[NUWAX_LISTEN_RESUME_MODE_MAX_LEN];
    char audio_session_id[NUWAX_LISTEN_RESUME_AUDIO_SESSION_ID_MAX_LEN];
    bool short_audio_resumable;
    /* Binary END was delivered on ws_connection_epoch. Cloud output can be
     * attributed to this round even if JSON listen-stop is still unwinding. */
    bool post_end_terminal_committed;
    bool post_end_asr_waiting;
    /* 60-second boundary is waiting for a cloud turn after END. */
    bool idle_expiry_pending;
    uint32_t post_end_asr_deadline_ms;
    uint32_t ws_connection_epoch;
    uint32_t post_end_total_deadline_ms;
    uint32_t progress_frames;
    uint32_t progress_text_hash;
    uint8_t progress_asr_phase;
    bool post_end_total_bounded;
} nuwax_listen_resume_state_t;

void nuwax_listen_resume_state_clear(nuwax_listen_resume_state_t *state);
void nuwax_listen_resume_state_arm_no_speech(nuwax_listen_resume_state_t *state,
                                             const char *mode,
                                             uint32_t deadline_ms);
void nuwax_listen_resume_state_bind_round(nuwax_listen_resume_state_t *state,
                                          uint32_t awake_generation,
                                          const char *audio_session_id);
bool nuwax_listen_resume_state_has_no_speech(const nuwax_listen_resume_state_t *state);
uint32_t nuwax_listen_resume_state_deadline_ms(const nuwax_listen_resume_state_t *state);
const char *nuwax_listen_resume_state_mode_or(const nuwax_listen_resume_state_t *state,
                                              const char *fallback);
void nuwax_listen_resume_state_set_short_audio(nuwax_listen_resume_state_t *state,
                                               bool resumable);
bool nuwax_listen_resume_state_has_short_audio(const nuwax_listen_resume_state_t *state);
bool nuwax_listen_resume_state_mark_post_end_terminal(
    nuwax_listen_resume_state_t *state,
    uint32_t ws_connection_epoch);
bool nuwax_listen_resume_state_has_post_end_terminal(
    const nuwax_listen_resume_state_t *state);
bool nuwax_listen_resume_state_arm_post_end_asr(
    nuwax_listen_resume_state_t *state,
    uint32_t ws_connection_epoch,
    uint32_t deadline_ms);
bool nuwax_listen_resume_state_arm_post_end_asr_bounded(
    nuwax_listen_resume_state_t *state, uint32_t ws_connection_epoch,
    uint32_t now_ms, uint32_t idle_timeout_ms, uint32_t total_timeout_ms);
/* Phase 0: received audio frames; 1: partial text; 2: final text.
 * Duplicate counters/text cannot extend the post-END response watchdog. */
bool nuwax_listen_resume_state_note_progress(
    nuwax_listen_resume_state_t *state, int frames,
    const char *text, uint8_t phase);
void nuwax_listen_resume_state_refresh_post_end_asr(
    nuwax_listen_resume_state_t *state,
    uint32_t deadline_ms);
void nuwax_listen_resume_state_disarm_post_end_asr(
    nuwax_listen_resume_state_t *state);
void nuwax_listen_resume_state_set_idle_expiry_pending(
    nuwax_listen_resume_state_t *state,
    bool pending);
bool nuwax_listen_resume_state_is_idle_expiry_pending(
    const nuwax_listen_resume_state_t *state);
bool nuwax_listen_resume_state_holds_idle_expiry_boundary(
    const nuwax_listen_resume_state_t *state);
bool nuwax_listen_resume_state_is_waiting_post_end_asr(
    const nuwax_listen_resume_state_t *state);
bool nuwax_listen_resume_state_post_end_asr_expired(
    const nuwax_listen_resume_state_t *state,
    uint32_t now_ms);
uint32_t nuwax_listen_resume_state_ws_epoch(
    const nuwax_listen_resume_state_t *state);
bool nuwax_listen_resume_state_event_matches(
    const nuwax_listen_resume_state_t *state,
    uint32_t awake_generation,
    const char *audio_session_id);
/* A cloud turn may be bootstrapped without asr_result only while it can still
 * be tied to the current bound capture round. During capture the envelope is
 * already bound to the active WebSocket; after END the stored epoch provides
 * the equivalent ordering boundary. */
bool nuwax_listen_resume_state_can_bootstrap_cloud_turn(
    const nuwax_listen_resume_state_t *state,
    uint32_t awake_generation,
    uint32_t ws_connection_epoch,
    bool capture_runtime_active);
