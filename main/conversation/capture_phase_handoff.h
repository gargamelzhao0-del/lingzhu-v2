#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    NUWAX_CAPTURE_PHASE_IDLE = 0,
    NUWAX_CAPTURE_PHASE_AWAKE_PRELISTEN,
    NUWAX_CAPTURE_PHASE_AWAKE_UPLINK,
    NUWAX_CAPTURE_PHASE_OLD_UPLINK_DRAINING,
    NUWAX_CAPTURE_PHASE_DURING_PLAYBACK_PRELISTEN,
    NUWAX_CAPTURE_PHASE_FAILED,
} nuwax_capture_phase_t;

typedef struct {
    nuwax_capture_phase_t phase;
    uint32_t transition_capture_sequence;
    uint32_t frozen_old_wire_frames;
} nuwax_capture_phase_handoff_t;

typedef enum {
    NUWAX_CAPTURE_PLAYBACK_COMPLETE_NO_HANDOFF = 0,
    NUWAX_CAPTURE_PLAYBACK_COMPLETE_KEEP_AWAKE_OWNER,
    NUWAX_CAPTURE_PLAYBACK_COMPLETE_DEFER_CANCEL_REARM,
} nuwax_capture_playback_complete_action_t;

static inline void nuwax_capture_phase_handoff_begin_awake_prelisten(
    nuwax_capture_phase_handoff_t *state) {
    if (!state) {
        return;
    }
    state->phase = NUWAX_CAPTURE_PHASE_AWAKE_PRELISTEN;
    state->transition_capture_sequence = 0U;
    state->frozen_old_wire_frames = 0U;
}

static inline nuwax_capture_playback_complete_action_t
nuwax_capture_phase_resolve_early_playback_complete(
    bool handoff_pending,
    bool seamless_requested,
    bool capture_active,
    bool capture_cancelled,
    bool awake_session_active) {
    if (!handoff_pending) {
        return NUWAX_CAPTURE_PLAYBACK_COMPLETE_NO_HANDOFF;
    }
    if (seamless_requested && capture_active && !capture_cancelled &&
        awake_session_active) {
        return NUWAX_CAPTURE_PLAYBACK_COMPLETE_KEEP_AWAKE_OWNER;
    }
    return NUWAX_CAPTURE_PLAYBACK_COMPLETE_DEFER_CANCEL_REARM;
}

static inline bool nuwax_capture_phase_cancel_recovery_needs_resume(
    bool capture_active,
    bool during_playback_mode,
    bool barge_in_mode) {
    return capture_active && (during_playback_mode || barge_in_mode);
}

/* A pending awake-to-playback handoff is consumed only after the next frame
 * reaches the processed-frame boundary. Preserve that transition frame even
 * when control/UI work briefly makes it older than the normal VAD latency
 * budget; dropping it would manufacture a sequence gap in the committed
 * uplink and turn a local phase change into a failed conversation round. */
static inline bool nuwax_capture_phase_preserve_stale_transition(
    bool persistent_conversation_mode,
    bool playback_handoff_pending) {
    return persistent_conversation_mode && playback_handoff_pending;
}

/* Until a playback-period candidate has started an uplink it owns no cloud
 * transaction. Local barge confirmation may already have stopped playback,
 * but it does not make stale preroll valid or commit audio to the cloud. A
 * capture gap may therefore discard that local candidate and continue from a
 * fresh boundary on the same capture/WebSocket session. */
static inline bool nuwax_capture_phase_may_discard_uncommitted_candidate(
    bool during_playback_mode,
    bool cloud_transaction_started) {
    return during_playback_mode && !cloud_transaction_started;
}

/* A control command that explicitly ends the active capture does not need one
 * more microphone frame: audio_uplink_finish() supplies the terminal binary
 * END marker. The only exception is an in-flight seamless handoff, whose old
 * END/listen-stop transaction must first finish on its owner task. */
static inline bool nuwax_capture_phase_stop_before_next_frame(
    bool stop_requested,
    bool explicit_control_stop,
    bool old_session_closing) {
    return stop_requested && explicit_control_stop && !old_session_closing;
}

static inline void nuwax_capture_phase_handoff_begin_awake(
    nuwax_capture_phase_handoff_t *state) {
    if (!state) {
        return;
    }
    state->phase = NUWAX_CAPTURE_PHASE_AWAKE_UPLINK;
    state->transition_capture_sequence = 0U;
    state->frozen_old_wire_frames = 0U;
}

static inline void nuwax_capture_phase_handoff_begin_during_playback(
    nuwax_capture_phase_handoff_t *state) {
    if (!state) {
        return;
    }
    state->phase = NUWAX_CAPTURE_PHASE_DURING_PLAYBACK_PRELISTEN;
    state->transition_capture_sequence = 0U;
    state->frozen_old_wire_frames = 0U;
}

/* A local prelisten may not own a cloud transaction yet. A playback command
 * must still move the capture owner into the during-playback phase without
 * attempting END/listen-stop on a nonexistent uplink. */
static inline bool nuwax_capture_phase_handoff_request_playback_without_session(
    nuwax_capture_phase_handoff_t *state,
    uint32_t transition_capture_sequence) {
    if (!state || state->phase != NUWAX_CAPTURE_PHASE_AWAKE_PRELISTEN) {
        return false;
    }
    state->phase = NUWAX_CAPTURE_PHASE_DURING_PLAYBACK_PRELISTEN;
    state->transition_capture_sequence = transition_capture_sequence;
    state->frozen_old_wire_frames = 0U;
    return true;
}

static inline bool nuwax_capture_phase_handoff_promote_awake_uplink(
    nuwax_capture_phase_handoff_t *state) {
    if (!state || state->phase != NUWAX_CAPTURE_PHASE_AWAKE_PRELISTEN) {
        return false;
    }
    state->phase = NUWAX_CAPTURE_PHASE_AWAKE_UPLINK;
    return true;
}

/* Called exactly once at the processed RT-capture frame boundary.  The
 * transition frame belongs to the new preroll timeline and must never be
 * appended to the old wire stream. */
static inline bool nuwax_capture_phase_handoff_request(
    nuwax_capture_phase_handoff_t *state,
    uint32_t transition_capture_sequence,
    uint32_t frozen_old_wire_frames) {
    if (!state || state->phase != NUWAX_CAPTURE_PHASE_AWAKE_UPLINK) {
        return false;
    }
    state->phase = NUWAX_CAPTURE_PHASE_OLD_UPLINK_DRAINING;
    state->transition_capture_sequence = transition_capture_sequence;
    state->frozen_old_wire_frames = frozen_old_wire_frames;
    return true;
}

static inline bool nuwax_capture_phase_handoff_request_playback_session_close(
    nuwax_capture_phase_handoff_t *state,
    uint32_t transition_capture_sequence,
    uint32_t frozen_old_wire_frames) {
    if (!state ||
        state->phase != NUWAX_CAPTURE_PHASE_DURING_PLAYBACK_PRELISTEN) {
        return false;
    }
    state->phase = NUWAX_CAPTURE_PHASE_OLD_UPLINK_DRAINING;
    state->transition_capture_sequence = transition_capture_sequence;
    state->frozen_old_wire_frames = frozen_old_wire_frames;
    return true;
}

static inline void nuwax_capture_phase_handoff_mark_old_session_closed(
    nuwax_capture_phase_handoff_t *state) {
    if (state && state->phase == NUWAX_CAPTURE_PHASE_OLD_UPLINK_DRAINING) {
        state->phase = NUWAX_CAPTURE_PHASE_DURING_PLAYBACK_PRELISTEN;
    }
}

static inline void nuwax_capture_phase_handoff_fail(
    nuwax_capture_phase_handoff_t *state) {
    if (state) {
        state->phase = NUWAX_CAPTURE_PHASE_FAILED;
    }
}

static inline bool nuwax_capture_phase_handoff_request_awake(
    nuwax_capture_phase_handoff_t *state,
    uint32_t transition_capture_sequence) {
    if (!state ||
        state->phase != NUWAX_CAPTURE_PHASE_DURING_PLAYBACK_PRELISTEN) {
        return false;
    }
    state->phase = NUWAX_CAPTURE_PHASE_AWAKE_PRELISTEN;
    state->transition_capture_sequence = transition_capture_sequence;
    state->frozen_old_wire_frames = 0U;
    return true;
}

static inline bool nuwax_capture_phase_routes_to_preroll(
    const nuwax_capture_phase_handoff_t *state) {
    return state &&
           (state->phase == NUWAX_CAPTURE_PHASE_OLD_UPLINK_DRAINING ||
            state->phase == NUWAX_CAPTURE_PHASE_DURING_PLAYBACK_PRELISTEN);
}

/* A second listen transaction is forbidden until END and listen-stop for the
 * old transaction are both committed. */
static inline bool nuwax_capture_phase_allows_new_session(
    const nuwax_capture_phase_handoff_t *state) {
    return state &&
           state->phase == NUWAX_CAPTURE_PHASE_DURING_PLAYBACK_PRELISTEN;
}
