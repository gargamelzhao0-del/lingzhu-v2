#include "listen_resume_state.h"

#include <string.h>

static void copy_truncated(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0) {
        return;
    }
    strncpy(dst, src ? src : "", dst_size - 1);
    dst[dst_size - 1] = '\0';
}

void nuwax_listen_resume_state_clear(nuwax_listen_resume_state_t *state) {
    if (!state) {
        return;
    }
    state->no_speech_resumable = false;
    state->deadline_ms = 0;
    state->awake_generation = 0;
    state->mode[0] = '\0';
    state->audio_session_id[0] = '\0';
    state->short_audio_resumable = false;
    state->post_end_terminal_committed = false;
    state->post_end_asr_waiting = false;
    state->idle_expiry_pending = false;
    state->post_end_asr_deadline_ms = 0;
    state->ws_connection_epoch = 0;
    state->post_end_total_deadline_ms = 0U;
    state->post_end_total_bounded = false;
    state->progress_frames = 0U;
    state->progress_text_hash = 0U;
    state->progress_asr_phase = 0U;
}

void nuwax_listen_resume_state_arm_no_speech(nuwax_listen_resume_state_t *state,
                                             const char *mode,
                                             uint32_t deadline_ms) {
    if (!state) {
        return;
    }
    state->no_speech_resumable = true;
    state->deadline_ms = deadline_ms;
    copy_truncated(state->mode,
                   sizeof(state->mode),
                   mode ? mode : "auto_followup");
}

void nuwax_listen_resume_state_bind_round(nuwax_listen_resume_state_t *state,
                                          uint32_t awake_generation,
                                          const char *audio_session_id) {
    if (!state) {
        return;
    }
    state->awake_generation = awake_generation;
    /* A newly bound capture round supersedes every terminal marker from the
     * previous round, including its connection epoch and watchdog. */
    state->no_speech_resumable = false;
    state->deadline_ms = 0U;
    state->mode[0] = '\0';
    state->short_audio_resumable = false;
    state->post_end_terminal_committed = false;
    state->post_end_asr_waiting = false;
    state->idle_expiry_pending = false;
    state->post_end_asr_deadline_ms = 0U;
    state->ws_connection_epoch = 0U;
    state->post_end_total_deadline_ms = 0U;
    state->post_end_total_bounded = false;
    state->progress_frames = 0U;
    state->progress_text_hash = 0U;
    state->progress_asr_phase = 0U;
    copy_truncated(state->audio_session_id,
                   sizeof(state->audio_session_id),
                   audio_session_id ? audio_session_id : "");
}

bool nuwax_listen_resume_state_has_no_speech(const nuwax_listen_resume_state_t *state) {
    return state && state->no_speech_resumable && state->deadline_ms != 0;
}

uint32_t nuwax_listen_resume_state_deadline_ms(const nuwax_listen_resume_state_t *state) {
    return state ? state->deadline_ms : 0;
}

const char *nuwax_listen_resume_state_mode_or(const nuwax_listen_resume_state_t *state,
                                              const char *fallback) {
    if (state && state->mode[0] != '\0') {
        return state->mode;
    }
    return fallback ? fallback : "";
}

void nuwax_listen_resume_state_set_short_audio(nuwax_listen_resume_state_t *state,
                                               bool resumable) {
    if (!state) {
        return;
    }
    state->short_audio_resumable = resumable;
}

bool nuwax_listen_resume_state_has_short_audio(const nuwax_listen_resume_state_t *state) {
    return state && state->short_audio_resumable;
}

bool nuwax_listen_resume_state_mark_post_end_terminal(
    nuwax_listen_resume_state_t *state,
    uint32_t ws_connection_epoch) {
    if (!state || state->awake_generation == 0U ||
        state->audio_session_id[0] == '\0' || ws_connection_epoch == 0U) {
        return false;
    }
    if (state->post_end_terminal_committed) {
        return state->ws_connection_epoch == ws_connection_epoch;
    }
    state->post_end_terminal_committed = true;
    state->ws_connection_epoch = ws_connection_epoch;
    return true;
}

bool nuwax_listen_resume_state_has_post_end_terminal(
    const nuwax_listen_resume_state_t *state) {
    return state && state->post_end_terminal_committed &&
           state->ws_connection_epoch != 0U;
}

bool nuwax_listen_resume_state_arm_post_end_asr(
    nuwax_listen_resume_state_t *state,
    uint32_t ws_connection_epoch,
    uint32_t deadline_ms) {
    if (!state || state->awake_generation == 0U ||
        state->audio_session_id[0] == '\0' || ws_connection_epoch == 0U ||
        deadline_ms == 0U) {
        return false;
    }
    if (!nuwax_listen_resume_state_mark_post_end_terminal(
            state, ws_connection_epoch)) {
        return false;
    }
    state->post_end_asr_waiting = true;
    state->post_end_asr_deadline_ms = deadline_ms;
    state->ws_connection_epoch = ws_connection_epoch;
    return true;
}

void nuwax_listen_resume_state_refresh_post_end_asr(
    nuwax_listen_resume_state_t *state,
    uint32_t deadline_ms) {
    if (!state || !state->post_end_asr_waiting || deadline_ms == 0U) {
        return;
    }
    state->post_end_asr_deadline_ms = deadline_ms;
}

bool nuwax_listen_resume_state_arm_post_end_asr_bounded(
    nuwax_listen_resume_state_t *state, uint32_t ws_connection_epoch,
    uint32_t now_ms, uint32_t idle_timeout_ms, uint32_t total_timeout_ms) {
    uint32_t deadline_ms = now_ms + idle_timeout_ms;
    /* The legacy idle deadline uses zero as "not armed". Preserve a valid
     * watchdog when uptime wraps exactly onto that sentinel (at most +1 ms). */
    if (deadline_ms == 0U) deadline_ms = 1U;
    if (idle_timeout_ms == 0U || total_timeout_ms == 0U ||
        !nuwax_listen_resume_state_arm_post_end_asr(
            state, ws_connection_epoch, deadline_ms)) return false;
    if (!state->post_end_total_bounded) {
        state->post_end_total_bounded = true;
        state->post_end_total_deadline_ms = now_ms + total_timeout_ms;
    }
    return true;
}

bool nuwax_listen_resume_state_note_progress(
    nuwax_listen_resume_state_t *state, int frames,
    const char *text, uint8_t phase) {
    if (!state) return false;
    if (phase == 0U) {
        if (frames <= 0 || (uint32_t)frames <= state->progress_frames) return false;
        state->progress_frames = (uint32_t)frames;
        return true;
    }
    if (!text || !text[0] || phase < state->progress_asr_phase) return false;
    uint32_t hash = 2166136261U;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        hash = (hash ^ *p) * 16777619U;
    }
    if (hash == state->progress_text_hash &&
        phase == state->progress_asr_phase) return false;
    state->progress_text_hash = hash;
    state->progress_asr_phase = phase;
    return true;
}

void nuwax_listen_resume_state_disarm_post_end_asr(
    nuwax_listen_resume_state_t *state) {
    if (!state) {
        return;
    }
    state->post_end_asr_waiting = false;
    state->post_end_terminal_committed = false;
    state->post_end_asr_deadline_ms = 0U;
    state->ws_connection_epoch = 0U;
    state->post_end_total_deadline_ms = 0U;
    state->post_end_total_bounded = false;
}

void nuwax_listen_resume_state_set_idle_expiry_pending(
    nuwax_listen_resume_state_t *state,
    bool pending) {
    if (!state) {
        return;
    }
    state->idle_expiry_pending = pending;
}

bool nuwax_listen_resume_state_is_idle_expiry_pending(
    const nuwax_listen_resume_state_t *state) {
    return state && state->idle_expiry_pending;
}

bool nuwax_listen_resume_state_holds_idle_expiry_boundary(
    const nuwax_listen_resume_state_t *state) {
    return nuwax_listen_resume_state_is_idle_expiry_pending(state) &&
           nuwax_listen_resume_state_is_waiting_post_end_asr(state);
}

bool nuwax_listen_resume_state_is_waiting_post_end_asr(
    const nuwax_listen_resume_state_t *state) {
    return state && state->post_end_asr_waiting &&
           state->post_end_asr_deadline_ms != 0U &&
           state->ws_connection_epoch != 0U;
}

bool nuwax_listen_resume_state_post_end_asr_expired(
    const nuwax_listen_resume_state_t *state,
    uint32_t now_ms) {
    return nuwax_listen_resume_state_is_waiting_post_end_asr(state) &&
           ((int32_t)(now_ms - state->post_end_asr_deadline_ms) >= 0 ||
            (state->post_end_total_bounded &&
             (int32_t)(now_ms - state->post_end_total_deadline_ms) >= 0));
}

uint32_t nuwax_listen_resume_state_ws_epoch(
    const nuwax_listen_resume_state_t *state) {
    return state ? state->ws_connection_epoch : 0U;
}

bool nuwax_listen_resume_state_event_matches(
    const nuwax_listen_resume_state_t *state,
    uint32_t awake_generation,
    const char *audio_session_id) {
    if (!state || awake_generation == 0 ||
        state->awake_generation != awake_generation) {
        return false;
    }
    /* Once a capture round has an identity, an ID-less event is not safe to
     * accept. A missing ID used to act as a wildcard and allowed a delayed
     * ASR event from a previous round to advance the current conversation. */
    if (state->audio_session_id[0] != '\0') {
        return audio_session_id && audio_session_id[0] != '\0' &&
               strcmp(state->audio_session_id, audio_session_id) == 0;
    }
    if (!audio_session_id || audio_session_id[0] == '\0') {
        return true;
    }
    return strcmp(state->audio_session_id, audio_session_id) == 0;
}

bool nuwax_listen_resume_state_can_bootstrap_cloud_turn(
    const nuwax_listen_resume_state_t *state,
    uint32_t awake_generation,
    uint32_t ws_connection_epoch,
    bool capture_runtime_active) {
    if (!state || awake_generation == 0U || ws_connection_epoch == 0U ||
        state->awake_generation != awake_generation ||
        state->audio_session_id[0] == '\0') {
        return false;
    }
    if (capture_runtime_active) {
        return true;
    }
    return state->post_end_terminal_committed &&
           state->ws_connection_epoch == ws_connection_epoch;
}
