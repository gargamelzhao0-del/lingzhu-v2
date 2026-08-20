#include "emotion_display_policy.h"

#include <string.h>

static bool deadline_elapsed(bool active,
                             uint32_t now_ms,
                             uint32_t deadline_ms)
{
    return active &&
           (int32_t)(now_ms - deadline_ms) >= 0;
}

static uint32_t next_generation(uint32_t generation)
{
    generation++;
    return generation == 0U ? 1U : generation;
}

static uint32_t remaining_until(bool active,
                                uint32_t now_ms,
                                uint32_t deadline_ms)
{
    if (!active || deadline_elapsed(true, now_ms, deadline_ms)) return 0U;
    return deadline_ms - now_ms;
}

static void set_semantic_deadline(nuwax_emotion_display_policy_t *policy,
                                  uint32_t now_ms,
                                  uint32_t duration_ms)
{
    policy->semantic_deadline_active = true;
    policy->semantic_deadline_ms = now_ms + duration_ms;
}

static void clear_semantic_deadline(nuwax_emotion_display_policy_t *policy)
{
    policy->semantic_deadline_active = false;
    policy->semantic_deadline_ms = 0U;
}

void nuwax_emotion_display_policy_init(
    nuwax_emotion_display_policy_t *policy)
{
    if (!policy) return;
    memset(policy, 0, sizeof(*policy));
    policy->semantic_emotion = NUWAX_EMOTION_NEUTRAL;
    policy->interaction_emotion = NUWAX_EMOTION_NEUTRAL;
}

bool nuwax_emotion_display_policy_apply_cloud(
    nuwax_emotion_display_policy_t *policy,
    nuwax_emotion_t emotion,
    uint32_t now_ms,
    uint32_t *out_generation)
{
    return nuwax_emotion_display_policy_apply_cloud_leased(
        policy, emotion, 0U, false, now_ms, out_generation);
}

bool nuwax_emotion_display_policy_apply_cloud_leased(
    nuwax_emotion_display_policy_t *policy,
    nuwax_emotion_t emotion,
    uint32_t duration_ms,
    bool finite_lease,
    uint32_t now_ms,
    uint32_t *out_generation)
{
    if (out_generation) *out_generation = 0U;
    if (!policy || emotion >= NUWAX_EMOTION_COUNT) return false;

    /* A newer cloud emotion may replace thinking directly. Keep only the
     * short thought_complete hold ordered so it cannot be truncated by a
     * second command while its completion animation is visible. */
    if (policy->semantic_valid && !finite_lease) {
        if (policy->semantic_mode == NUWAX_EMOTION_DISPLAY_THOUGHT_COMPLETE) {
            if (out_generation) {
                *out_generation = policy->semantic_generation;
            }
            return false;
        }
    }

    policy->semantic_generation =
        next_generation(policy->semantic_generation);
    policy->semantic_valid = true;
    policy->semantic_emotion = emotion;
    policy->semantic_finite_lease = finite_lease;
    policy->waiting_question_active = false;
    policy->pending_sleep_visual_active = false;
    clear_semantic_deadline(policy);
    policy->semantic_paused_remaining_ms = 0U;
    if (finite_lease) {
        policy->semantic_mode = emotion == NUWAX_EMOTION_THINKING
                                    ? NUWAX_EMOTION_DISPLAY_THINKING
                                    : emotion == NUWAX_EMOTION_THOUGHT_COMPLETE
                                          ? NUWAX_EMOTION_DISPLAY_THOUGHT_COMPLETE
                                          : NUWAX_EMOTION_DISPLAY_ORDINARY;
        set_semantic_deadline(policy, now_ms, duration_ms);
    } else if (emotion == NUWAX_EMOTION_THINKING) {
        policy->semantic_mode = NUWAX_EMOTION_DISPLAY_THINKING;
    } else if (emotion == NUWAX_EMOTION_THOUGHT_COMPLETE) {
        policy->semantic_mode = NUWAX_EMOTION_DISPLAY_THOUGHT_COMPLETE;
        if (policy->touch_active) {
            policy->semantic_paused_remaining_ms =
                NUWAX_EMOTION_THOUGHT_COMPLETE_DURATION_MS;
        } else {
            set_semantic_deadline(
                policy, now_ms,
                NUWAX_EMOTION_THOUGHT_COMPLETE_DURATION_MS);
        }
    } else {
        policy->semantic_mode = NUWAX_EMOTION_DISPLAY_ORDINARY;
        if (policy->touch_active) {
            policy->semantic_paused_remaining_ms =
                NUWAX_EMOTION_ORDINARY_DURATION_MS;
        } else {
            set_semantic_deadline(policy, now_ms,
                                  NUWAX_EMOTION_ORDINARY_DURATION_MS);
        }
    }
    if (out_generation) *out_generation = policy->semantic_generation;
    return true;
}

bool nuwax_emotion_display_policy_start_waiting_question(
    nuwax_emotion_display_policy_t *policy)
{
    if (!policy) return false;
    const bool changed = !policy->waiting_question_active ||
                         policy->pending_sleep_visual_active;
    policy->waiting_question_active = true;
    policy->pending_sleep_visual_active = false;
    return changed;
}

bool nuwax_emotion_display_policy_clear_waiting_question(
    nuwax_emotion_display_policy_t *policy)
{
    if (!policy || !policy->waiting_question_active) return false;
    policy->waiting_question_active = false;
    return true;
}

bool nuwax_emotion_display_policy_start_pending_sleep_visual(
    nuwax_emotion_display_policy_t *policy)
{
    if (!policy) return false;
    const bool changed = !policy->pending_sleep_visual_active ||
                         policy->waiting_question_active;
    policy->pending_sleep_visual_active = true;
    policy->waiting_question_active = false;
    return changed;
}

bool nuwax_emotion_display_policy_clear_pending_sleep_visual(
    nuwax_emotion_display_policy_t *policy)
{
    if (!policy || !policy->pending_sleep_visual_active) return false;
    policy->pending_sleep_visual_active = false;
    return true;
}

uint32_t nuwax_emotion_display_policy_start_interaction(
    nuwax_emotion_display_policy_t *policy,
    nuwax_emotion_t emotion,
    uint32_t duration_ms,
    uint32_t now_ms)
{
    if (!policy || emotion >= NUWAX_EMOTION_COUNT || duration_ms == 0U) {
        return 0U;
    }
    policy->touch_generation = next_generation(policy->touch_generation);
    policy->touch_active = true;
    policy->interaction_pinned = false;
    policy->interaction_emotion = emotion;
    policy->touch_deadline_active = true;
    policy->touch_deadline_ms = now_ms + duration_ms;
    policy->pending_sleep_visual_active = false;

    /* Interactions replace ordinary expressions but only cover the ordered
     * thinking sequence. thought_complete resumes with its remaining visible
     * budget after the interaction expires. */
    if (policy->semantic_valid &&
        policy->semantic_mode == NUWAX_EMOTION_DISPLAY_THOUGHT_COMPLETE) {
        if (policy->semantic_deadline_active) {
            const uint32_t remaining = remaining_until(
                true, now_ms, policy->semantic_deadline_ms);
            if (remaining == 0U) {
                policy->semantic_emotion = NUWAX_EMOTION_HAPPY;
                policy->semantic_mode = NUWAX_EMOTION_DISPLAY_NONE;
                clear_semantic_deadline(policy);
            } else {
                policy->semantic_paused_remaining_ms = remaining;
                clear_semantic_deadline(policy);
            }
        }
    } else if (policy->semantic_valid &&
               policy->semantic_mode != NUWAX_EMOTION_DISPLAY_THINKING) {
        policy->semantic_valid = false;
        policy->semantic_finite_lease = false;
        policy->semantic_mode = NUWAX_EMOTION_DISPLAY_NONE;
        clear_semantic_deadline(policy);
        policy->semantic_paused_remaining_ms = 0U;
        policy->semantic_generation =
            next_generation(policy->semantic_generation);
    }
    return policy->touch_generation;
}

uint32_t nuwax_emotion_display_policy_pin_interaction(
    nuwax_emotion_display_policy_t *policy,
    nuwax_emotion_t emotion)
{
    if (!policy || emotion >= NUWAX_EMOTION_COUNT) return 0U;

    if (!policy->touch_active || policy->interaction_emotion != emotion) {
        policy->touch_generation = next_generation(policy->touch_generation);
    }
    policy->touch_active = true;
    policy->interaction_pinned = true;
    policy->interaction_emotion = emotion;
    policy->touch_deadline_active = false;
    policy->touch_deadline_ms = 0U;

    /* A committed interaction is a new cloud turn. The cancelled turn's
     * semantic expression, including thinking/thought_complete, must not
     * reappear underneath it while the replacement reply is pending. */
    if (policy->semantic_valid) {
        policy->semantic_valid = false;
        policy->semantic_finite_lease = false;
        policy->semantic_mode = NUWAX_EMOTION_DISPLAY_NONE;
        clear_semantic_deadline(policy);
        policy->semantic_paused_remaining_ms = 0U;
        policy->semantic_generation =
            next_generation(policy->semantic_generation);
    }
    policy->waiting_question_active = false;
    policy->pending_sleep_visual_active = false;
    return policy->touch_generation;
}

uint32_t nuwax_emotion_display_policy_start_touch(
    nuwax_emotion_display_policy_t *policy,
    uint32_t now_ms)
{
    return nuwax_emotion_display_policy_start_interaction(
        policy, NUWAX_EMOTION_TOUCH,
        NUWAX_EMOTION_TOUCH_OVERRIDE_DURATION_MS, now_ms);
}

bool nuwax_emotion_display_policy_enter_lifecycle(
    nuwax_emotion_display_policy_t *policy,
    nuwax_dialog_state_t dialog,
    uint32_t now_ms)
{
    if (!policy) return false;
    const bool clears_semantic =
        dialog == NUWAX_DIALOG_LISTENING ||
        dialog == NUWAX_DIALOG_SLEEP ||
        dialog == NUWAX_DIALOG_ERROR ||
        dialog == NUWAX_DIALOG_RECOGNIZING;
    const bool clears_playback = dialog != NUWAX_DIALOG_SPEAKING;
    const bool clears_waiting = dialog != NUWAX_DIALOG_THINKING;
    const bool clears_pending_sleep = dialog != NUWAX_DIALOG_THINKING;
    const bool clears_touch =
        dialog == NUWAX_DIALOG_LISTENING ||
        dialog == NUWAX_DIALOG_SLEEP ||
        dialog == NUWAX_DIALOG_ERROR ||
        dialog == NUWAX_DIALOG_RECOGNIZING;
    bool changed = false;

    if (clears_semantic && policy->semantic_valid) {
        policy->semantic_valid = false;
        policy->semantic_finite_lease = false;
        policy->semantic_mode = NUWAX_EMOTION_DISPLAY_NONE;
        clear_semantic_deadline(policy);
        policy->semantic_paused_remaining_ms = 0U;
        policy->semantic_generation =
            next_generation(policy->semantic_generation);
        changed = true;
    }
    if (clears_playback && policy->playback_pcm_started) {
        policy->playback_pcm_started = false;
        policy->playback_epoch = 0U;
        policy->playback_generation = 0U;
        changed = true;
    }
    if (clears_waiting && policy->waiting_question_active) {
        policy->waiting_question_active = false;
        changed = true;
    }
    if (clears_pending_sleep && policy->pending_sleep_visual_active) {
        policy->pending_sleep_visual_active = false;
        changed = true;
    }
    if (clears_touch &&
        nuwax_emotion_display_policy_cancel_touch(policy, now_ms)) {
        changed = true;
    }
    return changed;
}

bool nuwax_emotion_display_policy_cancel_touch(
    nuwax_emotion_display_policy_t *policy,
    uint32_t now_ms)
{
    if (!policy || !policy->touch_active) return false;
    policy->touch_active = false;
    policy->interaction_pinned = false;
    policy->touch_deadline_active = false;
    policy->touch_deadline_ms = 0U;
    policy->interaction_emotion = NUWAX_EMOTION_NEUTRAL;
    if (policy->semantic_valid &&
        policy->semantic_paused_remaining_ms != 0U) {
        set_semantic_deadline(policy, now_ms,
                              policy->semantic_paused_remaining_ms);
        policy->semantic_paused_remaining_ms = 0U;
    }
    return true;
}

bool nuwax_emotion_display_policy_service(
    nuwax_emotion_display_policy_t *policy,
    uint32_t now_ms)
{
    if (!policy) return false;
    bool changed = false;
    if (policy->touch_active &&
        !policy->interaction_pinned &&
        deadline_elapsed(policy->touch_deadline_active,
                         now_ms, policy->touch_deadline_ms)) {
        policy->touch_active = false;
        policy->interaction_pinned = false;
        policy->touch_deadline_active = false;
        policy->touch_deadline_ms = 0U;
        policy->interaction_emotion = NUWAX_EMOTION_NEUTRAL;
        if (policy->semantic_valid &&
            policy->semantic_paused_remaining_ms != 0U) {
            set_semantic_deadline(
                policy, now_ms,
                policy->semantic_paused_remaining_ms);
            policy->semantic_paused_remaining_ms = 0U;
        }
        changed = true;
    }
    if (policy->semantic_valid && policy->semantic_deadline_active &&
        deadline_elapsed(policy->semantic_deadline_active,
                         now_ms, policy->semantic_deadline_ms)) {
        const bool finite_lease = policy->semantic_finite_lease;
        policy->semantic_valid = !finite_lease;
        policy->semantic_emotion = finite_lease
                                       ? NUWAX_EMOTION_NEUTRAL
                                       : NUWAX_EMOTION_HAPPY;
        policy->semantic_finite_lease = false;
        policy->semantic_mode = NUWAX_EMOTION_DISPLAY_NONE;
        clear_semantic_deadline(policy);
        policy->semantic_paused_remaining_ms = 0U;
        policy->semantic_generation =
            next_generation(policy->semantic_generation);
        changed = true;
    }
    return changed;
}

bool nuwax_emotion_display_policy_note_first_pcm(
    nuwax_emotion_display_policy_t *policy,
    uint32_t connection_epoch,
    uint32_t playback_generation)
{
    if (!policy || connection_epoch == 0U || playback_generation == 0U) {
        return false;
    }
    const bool changed = policy->waiting_question_active ||
                         policy->pending_sleep_visual_active ||
                         !policy->playback_pcm_started ||
                         policy->playback_epoch != connection_epoch ||
                         policy->playback_generation != playback_generation;
    policy->waiting_question_active = false;
    policy->pending_sleep_visual_active = false;
    policy->playback_pcm_started = true;
    policy->playback_epoch = connection_epoch;
    policy->playback_generation = playback_generation;
    return changed;
}

bool nuwax_emotion_display_policy_stop_playback(
    nuwax_emotion_display_policy_t *policy,
    uint32_t connection_epoch,
    uint32_t playback_generation)
{
    if (!policy || !policy->playback_pcm_started ||
        connection_epoch == 0U || playback_generation == 0U ||
        policy->playback_epoch != connection_epoch ||
        policy->playback_generation != playback_generation) {
        return false;
    }
    return nuwax_emotion_display_policy_clear_playback(policy);
}

bool nuwax_emotion_display_policy_clear_playback(
    nuwax_emotion_display_policy_t *policy)
{
    if (!policy || (!policy->playback_pcm_started &&
                    policy->playback_epoch == 0U &&
                    policy->playback_generation == 0U)) {
        return false;
    }
    policy->playback_pcm_started = false;
    policy->playback_epoch = 0U;
    policy->playback_generation = 0U;
    return true;
}

bool nuwax_emotion_display_policy_special_semantic_active(
    const nuwax_emotion_display_policy_t *policy)
{
    return policy && policy->semantic_valid &&
           (policy->semantic_mode == NUWAX_EMOTION_DISPLAY_THINKING ||
            policy->semantic_mode ==
                NUWAX_EMOTION_DISPLAY_THOUGHT_COMPLETE);
}
