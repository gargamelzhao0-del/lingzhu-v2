#include <assert.h>
#include <string.h>

#include "conversation/head_touch_agent_intent.h"

int main(void) {
    nuwax_head_touch_agent_intent_t intent;
    nuwax_head_touch_agent_intent_init(&intent);

    assert(nuwax_head_touch_agent_intent_arm(
        &intent,
        NUWAX_HEAD_TOUCH_AGENT_SOURCE_LISTENING,
        7U,
        3U,
        0U,
        true,
        true,
        "audio_1",
        100U,
        60U));
    assert(intent.state == NUWAX_HEAD_TOUCH_AGENT_STATE_ARBITRATING);
    assert(nuwax_head_touch_agent_intent_observe_frame(
               &intent, 159U, false, false) ==
           NUWAX_HEAD_TOUCH_AGENT_FRAME_WAIT);
    assert(nuwax_head_touch_agent_intent_observe_frame(
               &intent, 160U, false, true) ==
           NUWAX_HEAD_TOUCH_AGENT_FRAME_WAIT);
    assert(nuwax_head_touch_agent_intent_observe_frame(
               &intent, 180U, true, true) ==
           NUWAX_HEAD_TOUCH_AGENT_FRAME_USER_WON);
    assert(intent.state == NUWAX_HEAD_TOUCH_AGENT_STATE_CANCELLED);

    nuwax_head_touch_agent_intent_clear(&intent);
    assert(intent.state == NUWAX_HEAD_TOUCH_AGENT_STATE_IDLE);
    assert(intent.sequence == 1U);

    /* Candidate speech restarts the continuous-quiet window. A one-frame
     * classifier gap after a weak onset must not let the interaction win. */
    assert(nuwax_head_touch_agent_intent_arm(
        &intent,
        NUWAX_HEAD_TOUCH_AGENT_SOURCE_LISTENING,
        7U,
        3U,
        0U,
        true,
        false,
        NULL,
        100U,
        60U));
    assert(nuwax_head_touch_agent_intent_observe_frame(
               &intent, 160U, false, true) ==
           NUWAX_HEAD_TOUCH_AGENT_FRAME_WAIT);
    assert(intent.quiet_deadline_ms == 220U);
    assert(nuwax_head_touch_agent_intent_observe_frame(
               &intent, 180U, false, false) ==
           NUWAX_HEAD_TOUCH_AGENT_FRAME_WAIT);
    assert(nuwax_head_touch_agent_intent_observe_frame(
               &intent, 219U, false, false) ==
           NUWAX_HEAD_TOUCH_AGENT_FRAME_WAIT);
    assert(nuwax_head_touch_agent_intent_observe_frame(
               &intent, 220U, false, false) ==
           NUWAX_HEAD_TOUCH_AGENT_FRAME_BEGIN_QUIESCE);

    /* A local prelisten has a capture owner but no cloud listen transaction.
     * Releasing the microphone must be sufficient to send the touch turn. */
    nuwax_head_touch_agent_intent_clear(&intent);
    assert(nuwax_head_touch_agent_intent_arm(
        &intent,
        NUWAX_HEAD_TOUCH_AGENT_SOURCE_LISTENING,
        8U,
        4U,
        0U,
        true,
        false,
        NULL,
        200U,
        60U));
    assert(nuwax_head_touch_agent_intent_observe_frame(
               &intent, 260U, false, false) ==
           NUWAX_HEAD_TOUCH_AGENT_FRAME_BEGIN_QUIESCE);
    assert(nuwax_head_touch_agent_intent_bind_capture_transaction(
        &intent, NULL, false));
    assert(!intent.terminal_required);
    assert(intent.terminal_released);
    assert(intent.audio_session_id[0] == '\0');
    assert(nuwax_head_touch_agent_intent_mark_capture_released(
        &intent, 8U, 4U, NULL));
    assert(nuwax_head_touch_agent_intent_advance_release_barrier(&intent));
    assert(nuwax_head_touch_agent_intent_advance_drain_barrier(&intent));
    assert(nuwax_head_touch_agent_intent_mark_sent(&intent, "touch_local"));

    /* Once upload has actually started, the same listening path must retain
     * END/listen-stop ordering before the touch turn can be sent. */
    nuwax_head_touch_agent_intent_clear(&intent);
    assert(nuwax_head_touch_agent_intent_arm(
        &intent,
        NUWAX_HEAD_TOUCH_AGENT_SOURCE_LISTENING,
        8U,
        4U,
        0U,
        true,
        false,
        NULL,
        300U,
        60U));
    assert(nuwax_head_touch_agent_intent_observe_frame(
               &intent, 360U, false, false) ==
           NUWAX_HEAD_TOUCH_AGENT_FRAME_BEGIN_QUIESCE);
    assert(nuwax_head_touch_agent_intent_bind_capture_transaction(
        &intent, "audio_started", true));
    assert(intent.terminal_required);
    assert(!intent.terminal_released);
    assert(nuwax_head_touch_agent_intent_mark_capture_released(
        &intent, 8U, 4U, "audio_started"));
    assert(!nuwax_head_touch_agent_intent_advance_release_barrier(&intent));
    assert(nuwax_head_touch_agent_intent_mark_terminal_released(
        &intent, 8U, "audio_started"));
    assert(nuwax_head_touch_agent_intent_advance_release_barrier(&intent));

    nuwax_head_touch_agent_intent_clear(&intent);
    assert(nuwax_head_touch_agent_intent_arm(
        &intent,
        NUWAX_HEAD_TOUCH_AGENT_SOURCE_PLAYBACK,
        8U,
        4U,
        11U,
        true,
        false,
        NULL,
        UINT32_MAX - 20U,
        60U));
    assert(nuwax_head_touch_agent_intent_observe_frame(
               &intent, 20U, false, false) ==
           NUWAX_HEAD_TOUCH_AGENT_FRAME_WAIT);
    assert(nuwax_head_touch_agent_intent_observe_frame(
               &intent, 40U, false, false) ==
           NUWAX_HEAD_TOUCH_AGENT_FRAME_BEGIN_QUIESCE);
    assert(nuwax_head_touch_agent_intent_bind_capture_transaction(
        &intent, "barge_2", true));
    assert(nuwax_head_touch_agent_intent_mark_capture_released(
        &intent, 8U, 4U, "barge_2"));
    assert(!nuwax_head_touch_agent_intent_advance_release_barrier(&intent));
    assert(nuwax_head_touch_agent_intent_mark_terminal_released(
        &intent, 8U, "barge_2"));
    assert(nuwax_head_touch_agent_intent_mark_speaker_released(
        &intent, 8U, 11U));
    assert(nuwax_head_touch_agent_intent_advance_release_barrier(&intent));
    assert(intent.state == NUWAX_HEAD_TOUCH_AGENT_STATE_READY_DRAIN);
    assert(nuwax_head_touch_agent_intent_advance_drain_barrier(&intent));
    assert(nuwax_head_touch_agent_intent_mark_sent(&intent, "touch_2"));
    assert(intent.state == NUWAX_HEAD_TOUCH_AGENT_STATE_SENT);
    assert(strcmp(intent.request_id, "touch_2") == 0);
    assert(nuwax_head_touch_agent_intent_mark_cancel_ack(&intent, true));

    nuwax_head_touch_agent_intent_clear(&intent);
    assert(!nuwax_head_touch_agent_intent_arm(
        &intent,
        NUWAX_HEAD_TOUCH_AGENT_SOURCE_PLAYBACK,
        8U,
        4U,
        0U,
        false,
        false,
        NULL,
        1U,
        60U));
    assert(!nuwax_head_touch_agent_intent_arm(
        &intent,
        NUWAX_HEAD_TOUCH_AGENT_SOURCE_LISTENING,
        8U,
        4U,
        0U,
        true,
        true,
        NULL,
        1U,
        60U));

    nuwax_head_touch_agent_intent_clear(&intent);
    assert(nuwax_head_touch_agent_intent_arm_kind(
        &intent,
        NUWAX_AGENT_INTERACTION_SHAKE,
        NUWAX_HEAD_TOUCH_AGENT_SOURCE_SLEEPING,
        9U,
        5U,
        0U,
        false,
        false,
        NULL,
        200U,
        60U));
    assert(intent.interaction == NUWAX_AGENT_INTERACTION_SHAKE);
    assert(intent.source == NUWAX_HEAD_TOUCH_AGENT_SOURCE_SLEEPING);
    assert(!intent.capture_required);
    assert(!intent.speaker_required);
    assert(!intent.terminal_required);
    assert(nuwax_head_touch_agent_intent_begin_quiesce(&intent));
    assert(nuwax_head_touch_agent_intent_advance_release_barrier(&intent));
    assert(nuwax_head_touch_agent_intent_advance_drain_barrier(&intent));
    assert(nuwax_head_touch_agent_intent_mark_sent(&intent, "shake_3"));
    assert(strcmp(intent.request_id, "shake_3") == 0);
    assert(!nuwax_head_touch_agent_intent_mark_cancel_ack(&intent, true));

    nuwax_head_touch_agent_intent_clear(&intent);
    assert(nuwax_head_touch_agent_intent_arm_kind(
        &intent,
        NUWAX_AGENT_INTERACTION_HEAD_TOUCH,
        NUWAX_HEAD_TOUCH_AGENT_SOURCE_CLOUD_WAIT,
        10U,
        6U,
        0U,
        false,
        false,
        NULL,
        300U,
        60U));
    assert(!intent.capture_required && !intent.speaker_required &&
           !intent.terminal_required);
    assert(nuwax_head_touch_agent_intent_begin_quiesce(&intent));
    assert(nuwax_head_touch_agent_intent_bind_cancel_ack(
        &intent, 10U, "cancel_cloud_1", "turn_cloud_1", 400U));
    assert(!nuwax_head_touch_agent_intent_advance_release_barrier(&intent));
    assert(!nuwax_head_touch_agent_intent_apply_cancel_ack(
        &intent, 9U, "cancel_cloud_1", "turn_cloud_1", true));
    assert(!nuwax_head_touch_agent_intent_apply_cancel_ack(
        &intent, 10U, "cancel_stale", "turn_stale", true));
    assert(!nuwax_head_touch_agent_intent_cancel_ack_expired(&intent, 399U));
    assert(nuwax_head_touch_agent_intent_apply_cancel_ack(
        &intent, 10U, "", "turn_cloud_1", true));
    assert(nuwax_head_touch_agent_intent_advance_release_barrier(&intent));

    nuwax_head_touch_agent_intent_clear(&intent);
    assert(nuwax_head_touch_agent_intent_arm_kind(
        &intent,
        NUWAX_AGENT_INTERACTION_HEAD_TOUCH,
        NUWAX_HEAD_TOUCH_AGENT_SOURCE_CLOUD_WAIT,
        10U,
        6U,
        0U,
        false,
        false,
        NULL,
        350U,
        60U));
    assert(nuwax_head_touch_agent_intent_begin_quiesce(&intent));
    assert(nuwax_head_touch_agent_intent_bind_cancel_ack(
        &intent, 10U, "cancel_cloud_2", "", 25U));
    assert(nuwax_head_touch_agent_intent_apply_cancel_ack(
        &intent, 10U, "cancel_cloud_2", "", false));
    assert(!nuwax_head_touch_agent_intent_advance_release_barrier(&intent));

    nuwax_head_touch_agent_intent_clear(&intent);
    assert(nuwax_head_touch_agent_intent_arm_kind(
        &intent,
        NUWAX_AGENT_INTERACTION_HEAD_TOUCH,
        NUWAX_HEAD_TOUCH_AGENT_SOURCE_CLOUD_WAIT,
        10U,
        6U,
        0U,
        false,
        false,
        NULL,
        350U,
        60U));
    assert(nuwax_head_touch_agent_intent_begin_quiesce(&intent));
    assert(nuwax_head_touch_agent_intent_bind_cancel_ack(
        &intent, 10U, "cancel_cloud_3", "", 25U));
    assert(!nuwax_head_touch_agent_intent_cancel_ack_expired(
        &intent, UINT32_MAX));
    assert(nuwax_head_touch_agent_intent_cancel_ack_expired(&intent, 25U));

    nuwax_head_touch_agent_intent_clear(&intent);
    assert(nuwax_head_touch_agent_intent_arm_kind(
        &intent,
        NUWAX_AGENT_INTERACTION_SHAKE,
        NUWAX_HEAD_TOUCH_AGENT_SOURCE_POST_END_ASR,
        10U,
        6U,
        0U,
        false,
        false,
        "audio_post_end",
        320U,
        60U));
    assert(intent.source == NUWAX_HEAD_TOUCH_AGENT_SOURCE_POST_END_ASR);
    assert(nuwax_head_touch_agent_intent_audio_matches(
        &intent, "audio_post_end"));
    assert(!intent.capture_required && !intent.terminal_required);

    nuwax_head_touch_agent_intent_clear(&intent);
    assert(!nuwax_head_touch_agent_intent_arm_kind(
        &intent,
        NUWAX_AGENT_INTERACTION_SHAKE,
        NUWAX_HEAD_TOUCH_AGENT_SOURCE_POST_END_ASR,
        10U,
        6U,
        0U,
        false,
        false,
        NULL,
        320U,
        60U));

    nuwax_head_touch_agent_intent_clear(&intent);
    assert(!nuwax_head_touch_agent_intent_arm_kind(
        &intent,
        (nuwax_agent_interaction_t)(NUWAX_AGENT_INTERACTION_SHAKE + 1),
        NUWAX_HEAD_TOUCH_AGENT_SOURCE_SLEEPING,
        9U,
        5U,
        0U,
        false,
        false,
        NULL,
        200U,
        60U));

    return 0;
}
