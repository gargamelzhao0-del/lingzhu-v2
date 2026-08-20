#include "conversation/head_touch_agent_intent.h"

#include <stddef.h>
#include <string.h>

static bool nonempty(const char *value) {
    return value && value[0] != '\0';
}

static void copy_id(char *out, size_t out_size, const char *value) {
    if (!out || out_size == 0U) {
        return;
    }
    if (!value) {
        value = "";
    }
    size_t length = strlen(value);
    if (length >= out_size) {
        length = out_size - 1U;
    }
    memcpy(out, value, length);
    out[length] = '\0';
}

static bool active_state(nuwax_head_touch_agent_state_t state) {
    return state >= NUWAX_HEAD_TOUCH_AGENT_STATE_ARBITRATING &&
           state <= NUWAX_HEAD_TOUCH_AGENT_STATE_SENT;
}

void nuwax_head_touch_agent_intent_init(
    nuwax_head_touch_agent_intent_t *intent) {
    if (intent) {
        memset(intent, 0, sizeof(*intent));
    }
}

void nuwax_head_touch_agent_intent_clear(
    nuwax_head_touch_agent_intent_t *intent) {
    if (!intent) {
        return;
    }
    const uint32_t sequence = intent->sequence;
    memset(intent, 0, sizeof(*intent));
    intent->sequence = sequence;
}

bool nuwax_head_touch_agent_intent_arm(
    nuwax_head_touch_agent_intent_t *intent,
    nuwax_head_touch_agent_source_t source,
    uint32_t ws_epoch,
    uint32_t awake_generation,
    uint32_t speaker_generation,
    bool capture_required,
    bool terminal_required,
    const char *audio_session_id,
    uint32_t now_ms,
    uint32_t quiet_guard_ms) {
    return nuwax_head_touch_agent_intent_arm_kind(
        intent,
        NUWAX_AGENT_INTERACTION_HEAD_TOUCH,
        source,
        ws_epoch,
        awake_generation,
        speaker_generation,
        capture_required,
        terminal_required,
        audio_session_id,
        now_ms,
        quiet_guard_ms);
}

bool nuwax_head_touch_agent_intent_arm_kind(
    nuwax_head_touch_agent_intent_t *intent,
    nuwax_agent_interaction_t interaction,
    nuwax_head_touch_agent_source_t source,
    uint32_t ws_epoch,
    uint32_t awake_generation,
    uint32_t speaker_generation,
    bool capture_required,
    bool terminal_required,
    const char *audio_session_id,
    uint32_t now_ms,
    uint32_t quiet_guard_ms) {
    if (!intent || active_state(intent->state) ||
        interaction > NUWAX_AGENT_INTERACTION_SHAKE ||
        source == NUWAX_HEAD_TOUCH_AGENT_SOURCE_NONE ||
        source > NUWAX_HEAD_TOUCH_AGENT_SOURCE_CLOUD_WAIT ||
        ws_epoch == 0U ||
        awake_generation == 0U || quiet_guard_ms == 0U ||
        (source == NUWAX_HEAD_TOUCH_AGENT_SOURCE_PLAYBACK &&
         speaker_generation == 0U) ||
        (source == NUWAX_HEAD_TOUCH_AGENT_SOURCE_LISTENING &&
         !capture_required) ||
        (source == NUWAX_HEAD_TOUCH_AGENT_SOURCE_POST_END_ASR &&
         (capture_required || terminal_required ||
          !nonempty(audio_session_id))) ||
        (terminal_required &&
         (!capture_required || !nonempty(audio_session_id)))) {
        return false;
    }

    uint32_t sequence = intent->sequence + 1U;
    if (sequence == 0U) {
        sequence = 1U;
    }
    memset(intent, 0, sizeof(*intent));
    intent->sequence = sequence;
    intent->token = sequence;
    intent->state = NUWAX_HEAD_TOUCH_AGENT_STATE_ARBITRATING;
    intent->source = source;
    intent->interaction = interaction;
    intent->ws_epoch = ws_epoch;
    intent->awake_generation = awake_generation;
    intent->speaker_generation = speaker_generation;
    intent->quiet_guard_ms = quiet_guard_ms;
    intent->quiet_deadline_ms = now_ms + quiet_guard_ms;
    if (intent->quiet_deadline_ms == 0U) {
        intent->quiet_deadline_ms = 1U;
    }
    intent->speaker_required =
        source == NUWAX_HEAD_TOUCH_AGENT_SOURCE_PLAYBACK;
    intent->speaker_released = !intent->speaker_required;
    intent->capture_required = capture_required;
    intent->capture_released = !capture_required;
    intent->terminal_required = terminal_required;
    intent->terminal_released = !terminal_required;
    copy_id(intent->audio_session_id,
            sizeof(intent->audio_session_id),
            audio_session_id);
    return true;
}

bool nuwax_head_touch_agent_intent_owner_matches(
    const nuwax_head_touch_agent_intent_t *intent,
    uint32_t ws_epoch,
    uint32_t awake_generation) {
    return intent && active_state(intent->state) && ws_epoch != 0U &&
           awake_generation != 0U && intent->ws_epoch == ws_epoch &&
           intent->awake_generation == awake_generation;
}

bool nuwax_head_touch_agent_intent_audio_matches(
    const nuwax_head_touch_agent_intent_t *intent,
    const char *audio_session_id) {
    return intent && active_state(intent->state) &&
           nonempty(intent->audio_session_id) &&
           nonempty(audio_session_id) &&
           strcmp(intent->audio_session_id, audio_session_id) == 0;
}

nuwax_head_touch_agent_frame_action_t
nuwax_head_touch_agent_intent_observe_frame(
    nuwax_head_touch_agent_intent_t *intent,
    uint32_t now_ms,
    bool user_speech_confirmed,
    bool user_speech_candidate) {
    if (!intent || intent->state !=
                       NUWAX_HEAD_TOUCH_AGENT_STATE_ARBITRATING) {
        return NUWAX_HEAD_TOUCH_AGENT_FRAME_WAIT;
    }
    if (user_speech_confirmed) {
        intent->state = NUWAX_HEAD_TOUCH_AGENT_STATE_CANCELLED;
        return NUWAX_HEAD_TOUCH_AGENT_FRAME_USER_WON;
    }
    if (user_speech_candidate) {
        /* The guard is a continuous-quiet requirement. A weak or still
         * unconfirmed utterance must restart it so one misclassified frame
         * cannot let an interaction cut across the user's speech onset. */
        intent->quiet_deadline_ms = now_ms + intent->quiet_guard_ms;
        if (intent->quiet_deadline_ms == 0U) {
            intent->quiet_deadline_ms = 1U;
        }
        return NUWAX_HEAD_TOUCH_AGENT_FRAME_WAIT;
    }
    if ((int32_t)(now_ms - intent->quiet_deadline_ms) < 0) {
        return NUWAX_HEAD_TOUCH_AGENT_FRAME_WAIT;
    }
    intent->state = NUWAX_HEAD_TOUCH_AGENT_STATE_QUIESCING;
    return NUWAX_HEAD_TOUCH_AGENT_FRAME_BEGIN_QUIESCE;
}

bool nuwax_head_touch_agent_intent_begin_quiesce(
    nuwax_head_touch_agent_intent_t *intent) {
    if (!intent || intent->state !=
                       NUWAX_HEAD_TOUCH_AGENT_STATE_ARBITRATING) {
        return false;
    }
    intent->state = NUWAX_HEAD_TOUCH_AGENT_STATE_QUIESCING;
    return true;
}

bool nuwax_head_touch_agent_intent_bind_capture_transaction(
    nuwax_head_touch_agent_intent_t *intent,
    const char *audio_session_id,
    bool terminal_required) {
    if (!intent ||
        intent->state != NUWAX_HEAD_TOUCH_AGENT_STATE_QUIESCING ||
        !intent->capture_required ||
        (terminal_required && !nonempty(audio_session_id))) {
        return false;
    }
    if (terminal_required && nonempty(intent->audio_session_id) &&
        strcmp(intent->audio_session_id, audio_session_id) != 0) {
        return false;
    }
    if (terminal_required && !nonempty(intent->audio_session_id)) {
        copy_id(intent->audio_session_id,
                sizeof(intent->audio_session_id),
                audio_session_id);
    } else if (!terminal_required) {
        intent->audio_session_id[0] = '\0';
    }
    intent->terminal_required = terminal_required;
    intent->terminal_released = !terminal_required;
    return true;
}

bool nuwax_head_touch_agent_intent_mark_speaker_released(
    nuwax_head_touch_agent_intent_t *intent,
    uint32_t ws_epoch,
    uint32_t speaker_generation) {
    if (!intent || intent->state !=
                       NUWAX_HEAD_TOUCH_AGENT_STATE_QUIESCING ||
        !intent->speaker_required || intent->ws_epoch != ws_epoch ||
        intent->speaker_generation != speaker_generation) {
        return false;
    }
    intent->speaker_released = true;
    return true;
}

bool nuwax_head_touch_agent_intent_mark_capture_released(
    nuwax_head_touch_agent_intent_t *intent,
    uint32_t ws_epoch,
    uint32_t awake_generation,
    const char *audio_session_id) {
    if (!intent || intent->state !=
                       NUWAX_HEAD_TOUCH_AGENT_STATE_QUIESCING ||
        !intent->capture_required || intent->ws_epoch != ws_epoch ||
        intent->awake_generation != awake_generation) {
        return false;
    }
    if (nonempty(intent->audio_session_id) &&
        (!nonempty(audio_session_id) ||
         strcmp(intent->audio_session_id, audio_session_id) != 0)) {
        return false;
    }
    intent->capture_released = true;
    return true;
}

bool nuwax_head_touch_agent_intent_mark_terminal_released(
    nuwax_head_touch_agent_intent_t *intent,
    uint32_t ws_epoch,
    const char *audio_session_id) {
    if (!intent || intent->state !=
                       NUWAX_HEAD_TOUCH_AGENT_STATE_QUIESCING ||
        !intent->terminal_required || intent->ws_epoch != ws_epoch ||
        !nuwax_head_touch_agent_intent_audio_matches(intent,
                                                      audio_session_id)) {
        return false;
    }
    intent->terminal_released = true;
    return true;
}

bool nuwax_head_touch_agent_intent_mark_cancel_ack(
    nuwax_head_touch_agent_intent_t *intent,
    bool ok) {
    if (!intent || !active_state(intent->state) ||
        intent->source != NUWAX_HEAD_TOUCH_AGENT_SOURCE_PLAYBACK) {
        return false;
    }
    intent->cancel_ack_received = true;
    intent->cancel_ack_ok = ok;
    return true;
}

bool nuwax_head_touch_agent_intent_bind_cancel_ack(
    nuwax_head_touch_agent_intent_t *intent,
    uint32_t ws_epoch,
    const char *request_id,
    const char *turn_id,
    uint32_t deadline_ms) {
    if (!intent ||
        intent->state != NUWAX_HEAD_TOUCH_AGENT_STATE_QUIESCING ||
        intent->source != NUWAX_HEAD_TOUCH_AGENT_SOURCE_CLOUD_WAIT ||
        intent->ws_epoch != ws_epoch || deadline_ms == 0U ||
        (!nonempty(request_id) && !nonempty(turn_id))) {
        return false;
    }
    intent->cancel_ack_required = true;
    intent->cancel_ack_received = false;
    intent->cancel_ack_ok = false;
    intent->cancel_ack_deadline_ms = deadline_ms;
    copy_id(intent->cancel_request_id,
            sizeof(intent->cancel_request_id),
            request_id);
    copy_id(intent->cancel_turn_id,
            sizeof(intent->cancel_turn_id),
            turn_id);
    return true;
}

bool nuwax_head_touch_agent_intent_apply_cancel_ack(
    nuwax_head_touch_agent_intent_t *intent,
    uint32_t ws_epoch,
    const char *request_id,
    const char *turn_id,
    bool ok) {
    if (!intent || !active_state(intent->state) ||
        !intent->cancel_ack_required || intent->ws_epoch != ws_epoch) {
        return false;
    }
    const bool request_matches =
        nonempty(intent->cancel_request_id) && nonempty(request_id) &&
        strcmp(intent->cancel_request_id, request_id) == 0;
    const bool turn_matches =
        nonempty(intent->cancel_turn_id) && nonempty(turn_id) &&
        strcmp(intent->cancel_turn_id, turn_id) == 0;
    if (!request_matches && !turn_matches) {
        return false;
    }
    intent->cancel_ack_received = true;
    intent->cancel_ack_ok = ok;
    return true;
}

bool nuwax_head_touch_agent_intent_cancel_ack_expired(
    const nuwax_head_touch_agent_intent_t *intent,
    uint32_t now_ms) {
    return intent && active_state(intent->state) &&
           intent->cancel_ack_required && !intent->cancel_ack_received &&
           intent->cancel_ack_deadline_ms != 0U &&
           (int32_t)(now_ms - intent->cancel_ack_deadline_ms) >= 0;
}

bool nuwax_head_touch_agent_intent_advance_release_barrier(
    nuwax_head_touch_agent_intent_t *intent) {
    if (!intent || intent->state !=
                       NUWAX_HEAD_TOUCH_AGENT_STATE_QUIESCING ||
        !intent->speaker_released || !intent->capture_released ||
        !intent->terminal_released ||
        (intent->cancel_ack_required &&
         (!intent->cancel_ack_received || !intent->cancel_ack_ok))) {
        return false;
    }
    intent->state = NUWAX_HEAD_TOUCH_AGENT_STATE_READY_DRAIN;
    return true;
}

bool nuwax_head_touch_agent_intent_advance_drain_barrier(
    nuwax_head_touch_agent_intent_t *intent) {
    if (!intent || intent->state !=
                       NUWAX_HEAD_TOUCH_AGENT_STATE_READY_DRAIN) {
        return false;
    }
    intent->state = NUWAX_HEAD_TOUCH_AGENT_STATE_READY_TO_SEND;
    return true;
}

bool nuwax_head_touch_agent_intent_mark_sent(
    nuwax_head_touch_agent_intent_t *intent,
    const char *request_id) {
    if (!intent || intent->state !=
                       NUWAX_HEAD_TOUCH_AGENT_STATE_READY_TO_SEND ||
        !nonempty(request_id)) {
        return false;
    }
    copy_id(intent->request_id, sizeof(intent->request_id), request_id);
    intent->state = NUWAX_HEAD_TOUCH_AGENT_STATE_SENT;
    return true;
}

void nuwax_head_touch_agent_intent_cancel(
    nuwax_head_touch_agent_intent_t *intent) {
    if (intent && active_state(intent->state)) {
        intent->state = NUWAX_HEAD_TOUCH_AGENT_STATE_CANCELLED;
    }
}

const char *nuwax_head_touch_agent_state_name(
    nuwax_head_touch_agent_state_t state) {
    switch (state) {
        case NUWAX_HEAD_TOUCH_AGENT_STATE_IDLE:
            return "idle";
        case NUWAX_HEAD_TOUCH_AGENT_STATE_ARBITRATING:
            return "arbitrating";
        case NUWAX_HEAD_TOUCH_AGENT_STATE_QUIESCING:
            return "quiescing";
        case NUWAX_HEAD_TOUCH_AGENT_STATE_READY_DRAIN:
            return "ready_drain";
        case NUWAX_HEAD_TOUCH_AGENT_STATE_READY_TO_SEND:
            return "ready_to_send";
        case NUWAX_HEAD_TOUCH_AGENT_STATE_SENT:
            return "sent";
        case NUWAX_HEAD_TOUCH_AGENT_STATE_CANCELLED:
            return "cancelled";
        default:
            return "unknown";
    }
}

const char *nuwax_head_touch_agent_source_name(
    nuwax_head_touch_agent_source_t source) {
    switch (source) {
        case NUWAX_HEAD_TOUCH_AGENT_SOURCE_LISTENING:
            return "listening";
        case NUWAX_HEAD_TOUCH_AGENT_SOURCE_PLAYBACK:
            return "playback";
        case NUWAX_HEAD_TOUCH_AGENT_SOURCE_SLEEPING:
            return "sleeping";
        case NUWAX_HEAD_TOUCH_AGENT_SOURCE_POST_END_ASR:
            return "post_end_asr";
        case NUWAX_HEAD_TOUCH_AGENT_SOURCE_CLOUD_WAIT:
            return "cloud_wait";
        case NUWAX_HEAD_TOUCH_AGENT_SOURCE_NONE:
        default:
            return "none";
    }
}
