#pragma once

#include <stdbool.h>
#include <stdint.h>

#define NUWAX_HEAD_TOUCH_AGENT_AUDIO_SESSION_ID_LEN 48U
#define NUWAX_HEAD_TOUCH_AGENT_REQUEST_ID_LEN 48U

typedef enum {
    NUWAX_HEAD_TOUCH_AGENT_STATE_IDLE = 0,
    NUWAX_HEAD_TOUCH_AGENT_STATE_ARBITRATING,
    NUWAX_HEAD_TOUCH_AGENT_STATE_QUIESCING,
    NUWAX_HEAD_TOUCH_AGENT_STATE_READY_DRAIN,
    NUWAX_HEAD_TOUCH_AGENT_STATE_READY_TO_SEND,
    NUWAX_HEAD_TOUCH_AGENT_STATE_SENT,
    NUWAX_HEAD_TOUCH_AGENT_STATE_CANCELLED,
} nuwax_head_touch_agent_state_t;

typedef enum {
    NUWAX_HEAD_TOUCH_AGENT_SOURCE_NONE = 0,
    NUWAX_HEAD_TOUCH_AGENT_SOURCE_LISTENING,
    NUWAX_HEAD_TOUCH_AGENT_SOURCE_PLAYBACK,
    NUWAX_HEAD_TOUCH_AGENT_SOURCE_SLEEPING,
    NUWAX_HEAD_TOUCH_AGENT_SOURCE_POST_END_ASR,
    NUWAX_HEAD_TOUCH_AGENT_SOURCE_CLOUD_WAIT,
} nuwax_head_touch_agent_source_t;

typedef enum {
    NUWAX_AGENT_INTERACTION_HEAD_TOUCH = 0,
    NUWAX_AGENT_INTERACTION_SHAKE,
} nuwax_agent_interaction_t;

typedef enum {
    NUWAX_HEAD_TOUCH_AGENT_FRAME_WAIT = 0,
    NUWAX_HEAD_TOUCH_AGENT_FRAME_USER_WON,
    NUWAX_HEAD_TOUCH_AGENT_FRAME_BEGIN_QUIESCE,
} nuwax_head_touch_agent_frame_action_t;

typedef struct {
    nuwax_head_touch_agent_state_t state;
    nuwax_head_touch_agent_source_t source;
    nuwax_agent_interaction_t interaction;
    uint32_t sequence;
    uint32_t token;
    uint32_t ws_epoch;
    uint32_t awake_generation;
    uint32_t speaker_generation;
    uint32_t quiet_guard_ms;
    uint32_t quiet_deadline_ms;
    bool speaker_required;
    bool speaker_released;
    bool capture_required;
    bool capture_released;
    bool terminal_required;
    bool terminal_released;
    bool cancel_ack_required;
    bool cancel_ack_received;
    bool cancel_ack_ok;
    uint32_t cancel_ack_deadline_ms;
    char audio_session_id[NUWAX_HEAD_TOUCH_AGENT_AUDIO_SESSION_ID_LEN];
    char cancel_request_id[NUWAX_HEAD_TOUCH_AGENT_REQUEST_ID_LEN];
    char cancel_turn_id[NUWAX_HEAD_TOUCH_AGENT_REQUEST_ID_LEN];
    char request_id[NUWAX_HEAD_TOUCH_AGENT_REQUEST_ID_LEN];
} nuwax_head_touch_agent_intent_t;

void nuwax_head_touch_agent_intent_init(
    nuwax_head_touch_agent_intent_t *intent);

void nuwax_head_touch_agent_intent_clear(
    nuwax_head_touch_agent_intent_t *intent);

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
    uint32_t quiet_guard_ms);

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
    uint32_t quiet_guard_ms);

bool nuwax_head_touch_agent_intent_owner_matches(
    const nuwax_head_touch_agent_intent_t *intent,
    uint32_t ws_epoch,
    uint32_t awake_generation);

bool nuwax_head_touch_agent_intent_audio_matches(
    const nuwax_head_touch_agent_intent_t *intent,
    const char *audio_session_id);

nuwax_head_touch_agent_frame_action_t
nuwax_head_touch_agent_intent_observe_frame(
    nuwax_head_touch_agent_intent_t *intent,
    uint32_t now_ms,
    bool user_speech_confirmed,
    bool user_speech_candidate);

bool nuwax_head_touch_agent_intent_begin_quiesce(
    nuwax_head_touch_agent_intent_t *intent);

bool nuwax_head_touch_agent_intent_bind_capture_transaction(
    nuwax_head_touch_agent_intent_t *intent,
    const char *audio_session_id,
    bool terminal_required);

bool nuwax_head_touch_agent_intent_mark_speaker_released(
    nuwax_head_touch_agent_intent_t *intent,
    uint32_t ws_epoch,
    uint32_t speaker_generation);

bool nuwax_head_touch_agent_intent_mark_capture_released(
    nuwax_head_touch_agent_intent_t *intent,
    uint32_t ws_epoch,
    uint32_t awake_generation,
    const char *audio_session_id);

bool nuwax_head_touch_agent_intent_mark_terminal_released(
    nuwax_head_touch_agent_intent_t *intent,
    uint32_t ws_epoch,
    const char *audio_session_id);

bool nuwax_head_touch_agent_intent_mark_cancel_ack(
    nuwax_head_touch_agent_intent_t *intent,
    bool ok);

bool nuwax_head_touch_agent_intent_bind_cancel_ack(
    nuwax_head_touch_agent_intent_t *intent,
    uint32_t ws_epoch,
    const char *request_id,
    const char *turn_id,
    uint32_t deadline_ms);

bool nuwax_head_touch_agent_intent_apply_cancel_ack(
    nuwax_head_touch_agent_intent_t *intent,
    uint32_t ws_epoch,
    const char *request_id,
    const char *turn_id,
    bool ok);

bool nuwax_head_touch_agent_intent_cancel_ack_expired(
    const nuwax_head_touch_agent_intent_t *intent,
    uint32_t now_ms);

bool nuwax_head_touch_agent_intent_advance_release_barrier(
    nuwax_head_touch_agent_intent_t *intent);

bool nuwax_head_touch_agent_intent_advance_drain_barrier(
    nuwax_head_touch_agent_intent_t *intent);

bool nuwax_head_touch_agent_intent_mark_sent(
    nuwax_head_touch_agent_intent_t *intent,
    const char *request_id);

void nuwax_head_touch_agent_intent_cancel(
    nuwax_head_touch_agent_intent_t *intent);

const char *nuwax_head_touch_agent_state_name(
    nuwax_head_touch_agent_state_t state);

const char *nuwax_head_touch_agent_source_name(
    nuwax_head_touch_agent_source_t source);
