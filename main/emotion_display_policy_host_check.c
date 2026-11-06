#include <stdbool.h>
#include <stdio.h>

#include "nuwax_ui_v28/emotion_display_policy.h"

static int s_failures;

static void expect_true(const char *name, bool value)
{
    if (!value) {
        fprintf(stderr, "[FAIL] %s\n", name);
        s_failures++;
    } else {
        printf("[PASS] %s\n", name);
    }
}

int main(void)
{
    nuwax_emotion_display_policy_t policy;
    uint32_t generation = 0U;
    nuwax_emotion_display_policy_init(&policy);

    expect_true("pending sleep visual starts",
                nuwax_emotion_display_policy_start_pending_sleep_visual(
                    &policy) && policy.pending_sleep_visual_active);
    expect_true("thinking lifecycle preserves pending sleep visual",
                !nuwax_emotion_display_policy_enter_lifecycle(
                    &policy, NUWAX_DIALOG_THINKING, 1U) &&
                    policy.pending_sleep_visual_active);
    expect_true("waiting question replaces pending sleep visual",
                nuwax_emotion_display_policy_start_waiting_question(
                    &policy) && policy.waiting_question_active &&
                    !policy.pending_sleep_visual_active);
    expect_true("pending sleep visual replaces waiting question",
                nuwax_emotion_display_policy_start_pending_sleep_visual(
                    &policy) && policy.pending_sleep_visual_active &&
                    !policy.waiting_question_active);
    (void)nuwax_emotion_display_policy_apply_cloud(
        &policy, NUWAX_EMOTION_HAPPY, 10U, &generation);
    expect_true("accepted cloud emotion clears pending sleep visual",
                !policy.pending_sleep_visual_active);

    nuwax_emotion_display_policy_init(&policy);

    expect_true("waiting question starts",
                nuwax_emotion_display_policy_start_waiting_question(
                    &policy) && policy.waiting_question_active);
    (void)nuwax_emotion_display_policy_apply_cloud(
        &policy, NUWAX_EMOTION_HAPPY, 50U, &generation);
    expect_true("accepted cloud emotion replaces waiting question",
                !policy.waiting_question_active);

    (void)nuwax_emotion_display_policy_apply_cloud(
        &policy, NUWAX_EMOTION_ANGRY, 100U, &generation);
    expect_true("accepted cloud emotion returns generation",
                generation != 0U &&
                    generation == policy.semantic_generation);
    expect_true("ordinary cloud emotion is active",
                policy.semantic_valid &&
                    policy.semantic_emotion == NUWAX_EMOTION_ANGRY);
    expect_true("ordinary cloud emotion becomes happy at three seconds",
                nuwax_emotion_display_policy_service(&policy, 3100U) &&
                    policy.semantic_emotion == NUWAX_EMOTION_HAPPY);
    (void)nuwax_emotion_display_policy_apply_cloud(
        &policy, NUWAX_EMOTION_ANGRY, 100U, &generation);
    (void)nuwax_emotion_display_policy_start_touch(&policy, 200U);
    expect_true("touch drops ordinary cloud emotion",
                policy.touch_active && !policy.semantic_valid);
    expect_true("touch remains for two seconds",
                !nuwax_emotion_display_policy_service(&policy, 2199U));
    expect_true("touch expires at two seconds",
                nuwax_emotion_display_policy_service(&policy, 2200U) &&
                    !policy.touch_active);

    (void)nuwax_emotion_display_policy_apply_cloud(
        &policy, NUWAX_EMOTION_THINKING, 3000U, &generation);
    const uint32_t thinking_generation = generation;
    expect_true("new cloud emotion overrides thinking",
                nuwax_emotion_display_policy_apply_cloud(
                    &policy, NUWAX_EMOTION_EXCITED, 3050U,
                    &generation) &&
                    generation != thinking_generation &&
                    policy.semantic_emotion == NUWAX_EMOTION_EXCITED &&
                    policy.semantic_mode == NUWAX_EMOTION_DISPLAY_ORDINARY);
    (void)nuwax_emotion_display_policy_apply_cloud(
        &policy, NUWAX_EMOTION_THINKING, 3060U, &generation);
    (void)nuwax_emotion_display_policy_start_touch(&policy, 3100U);
    expect_true("touch preserves thinking owner",
                policy.semantic_valid &&
                    policy.semantic_mode == NUWAX_EMOTION_DISPLAY_THINKING);
    (void)nuwax_emotion_display_policy_service(&policy, 5100U);
    expect_true("thinking resumes after touch",
                !policy.touch_active && policy.semantic_valid &&
                    policy.semantic_emotion == NUWAX_EMOTION_THINKING);
    expect_true("thinking has no automatic timeout",
                !nuwax_emotion_display_policy_service(&policy, 60000U) &&
                    policy.semantic_emotion == NUWAX_EMOTION_THINKING);

    (void)nuwax_emotion_display_policy_pin_interaction(
        &policy, NUWAX_EMOTION_TOUCH);
    expect_true("committed touch replaces old thinking turn",
                policy.touch_active && policy.interaction_pinned &&
                    !policy.semantic_valid &&
                    !nuwax_emotion_display_policy_service(&policy, 120000U));
    expect_true("committed touch ends only on lifecycle release",
                nuwax_emotion_display_policy_cancel_touch(&policy, 120001U) &&
                    !policy.touch_active && !policy.interaction_pinned);

    /* Reproduce the shake round: keep the reply expression while audio plays,
     * then release on listening without needing another animation timeout. */
    (void)nuwax_emotion_display_policy_start_interaction(
        &policy, NUWAX_EMOTION_DIZZINESS, 3000U, 34266U);
    (void)nuwax_emotion_display_policy_pin_interaction(
        &policy, NUWAX_EMOTION_DIZZINESS);
    (void)nuwax_emotion_display_policy_enter_lifecycle(
        &policy, NUWAX_DIALOG_SPEAKING, 36026U);
    expect_true("shake reply stays visible through the original three-second deadline",
                !nuwax_emotion_display_policy_service(&policy, 39000U) &&
                    policy.touch_active && policy.interaction_pinned &&
                    policy.interaction_emotion == NUWAX_EMOTION_DIZZINESS);
    expect_true("shake immediately releases on playback completion listening",
                nuwax_emotion_display_policy_enter_lifecycle(
                    &policy, NUWAX_DIALOG_LISTENING, 40056U) &&
                    !policy.touch_active && !policy.interaction_pinned);

    (void)nuwax_emotion_display_policy_apply_cloud(
        &policy, NUWAX_EMOTION_THOUGHT_COMPLETE, 6000U, &generation);
    expect_true("ordinary cloud emotion cannot shorten thought-complete hold",
                !nuwax_emotion_display_policy_apply_cloud(
                    &policy, NUWAX_EMOTION_ANGRY, 6100U,
                    &generation) &&
                    policy.semantic_emotion ==
                        NUWAX_EMOTION_THOUGHT_COMPLETE);
    expect_true("thought complete remains before 1.5 seconds",
                !nuwax_emotion_display_policy_service(&policy, 7499U));
    expect_true("thought complete becomes happy",
                nuwax_emotion_display_policy_service(&policy, 7500U) &&
                    policy.semantic_valid &&
                    policy.semantic_emotion == NUWAX_EMOTION_HAPPY);

    (void)nuwax_emotion_display_policy_apply_cloud(
        &policy, NUWAX_EMOTION_THOUGHT_COMPLETE, 7600U, &generation);
    (void)nuwax_emotion_display_policy_start_touch(&policy, 7700U);
    (void)nuwax_emotion_display_policy_start_touch(&policy, 7800U);
    (void)nuwax_emotion_display_policy_service(&policy, 9700U);
    expect_true("repeated touch extends insertion",
                policy.touch_active);
    (void)nuwax_emotion_display_policy_service(&policy, 9800U);
    expect_true("touch pauses thought-complete display budget",
                policy.semantic_emotion == NUWAX_EMOTION_THOUGHT_COMPLETE &&
                    policy.semantic_deadline_active &&
                    policy.semantic_deadline_ms == 11200U);

    (void)nuwax_emotion_display_policy_enter_lifecycle(
        &policy, NUWAX_DIALOG_LISTENING, 9900U);
    (void)nuwax_emotion_display_policy_apply_cloud(
        &policy, NUWAX_EMOTION_EXCITED, 8000U, &generation);
    expect_true("first PCM starts playback tracking",
                nuwax_emotion_display_policy_note_first_pcm(
                    &policy, 7U, 11U));
    expect_true("first PCM binds playback generation",
                policy.playback_pcm_started &&
                    policy.playback_epoch == 7U &&
                    policy.playback_generation == 11U);
    expect_true("stale playback cannot stop visual generation",
                !nuwax_emotion_display_policy_stop_playback(
                    &policy, 7U, 10U));
    expect_true("matching playback stops visual generation",
                nuwax_emotion_display_policy_stop_playback(
                    &policy, 7U, 11U) &&
                    !policy.playback_pcm_started);
    (void)nuwax_emotion_display_policy_start_pending_sleep_visual(&policy);
    expect_true("first PCM clears pending sleep visual",
                nuwax_emotion_display_policy_note_first_pcm(
                    &policy, 7U, 12U) &&
                    !policy.pending_sleep_visual_active);

    nuwax_emotion_display_policy_init(&policy);
    (void)nuwax_emotion_display_policy_note_first_pcm(&policy, 8U, 12U);
    expect_true("default playback generation is tracked",
                policy.playback_pcm_started &&
                    policy.playback_epoch == 8U &&
                    policy.playback_generation == 12U);
    (void)nuwax_emotion_display_policy_start_touch(&policy, 8100U);
    expect_true("touch preserves playback generation tracking",
                policy.playback_pcm_started &&
                    policy.playback_epoch == 8U &&
                    policy.playback_generation == 12U);
    (void)nuwax_emotion_display_policy_enter_lifecycle(
        &policy, NUWAX_DIALOG_LISTENING, 8200U);
    expect_true("listening invalidates cloud playback but preserves local touch",
                !policy.semantic_valid && !policy.playback_pcm_started &&
                    policy.touch_active);
    expect_true("no-speech does not shorten the two-second touch feedback",
                !nuwax_emotion_display_policy_service(&policy, 10099U) &&
                    policy.touch_active &&
                    nuwax_emotion_display_policy_service(&policy, 10100U) &&
                    !policy.touch_active);
    (void)nuwax_emotion_display_policy_service(&policy, 20000U);
    expect_true("stale timer cannot replace listening with happy",
                !nuwax_emotion_display_policy_service(&policy, 20001U) &&
                    !policy.semantic_valid);

    (void)nuwax_emotion_display_policy_start_touch(&policy, 9000U);
    expect_true("strong lifecycle cancels touch",
                nuwax_emotion_display_policy_enter_lifecycle(
                    &policy, NUWAX_DIALOG_SLEEP, 9001U) &&
                    !policy.touch_active);

    const nuwax_dialog_state_t pending_sleep_clear_states[] = {
        NUWAX_DIALOG_LISTENING,
        NUWAX_DIALOG_SPEAKING,
        NUWAX_DIALOG_SLEEP,
        NUWAX_DIALOG_ERROR,
    };
    for (size_t i = 0U;
         i < sizeof(pending_sleep_clear_states) /
                 sizeof(pending_sleep_clear_states[0]);
         ++i) {
        nuwax_emotion_display_policy_init(&policy);
        (void)nuwax_emotion_display_policy_start_pending_sleep_visual(
            &policy);
        expect_true("lifecycle clears pending sleep visual",
                    nuwax_emotion_display_policy_enter_lifecycle(
                        &policy, pending_sleep_clear_states[i], 9200U) &&
                        !policy.pending_sleep_visual_active);
    }

    nuwax_emotion_display_policy_init(&policy);
    (void)nuwax_emotion_display_policy_start_pending_sleep_visual(&policy);
    (void)nuwax_emotion_display_policy_start_touch(&policy, 9300U);
    expect_true("touch replaces pending sleep visual",
                policy.touch_active &&
                    !policy.pending_sleep_visual_active);

    nuwax_emotion_display_policy_init(&policy);
    (void)nuwax_emotion_display_policy_apply_cloud(
        &policy, NUWAX_EMOTION_HAPPY, UINT32_MAX - 1000U, &generation);
    expect_true("deadline comparison survives clock wrap",
                 nuwax_emotion_display_policy_service(&policy, 1999U));

    nuwax_emotion_display_policy_init(&policy);
    (void)nuwax_emotion_display_policy_apply_cloud(
        &policy, NUWAX_EMOTION_HAPPY, UINT32_MAX - 2999U,
        &generation);
    expect_true("zero-valued wrapped semantic deadline remains active",
                policy.semantic_deadline_active &&
                    policy.semantic_deadline_ms == 0U &&
                    nuwax_emotion_display_policy_service(&policy, 0U));

    nuwax_emotion_display_policy_init(&policy);
    (void)nuwax_emotion_display_policy_start_touch(
        &policy, UINT32_MAX - 1999U);
    expect_true("zero-valued wrapped touch deadline remains active",
                policy.touch_deadline_active &&
                    policy.touch_deadline_ms == 0U &&
                    nuwax_emotion_display_policy_service(&policy, 0U));

    nuwax_emotion_display_policy_init(&policy);
    (void)nuwax_emotion_display_policy_apply_cloud(
        &policy, NUWAX_EMOTION_THOUGHT_COMPLETE, 10000U,
        &generation);
    (void)nuwax_emotion_display_policy_start_touch(&policy, 10500U);
    expect_true("cancel touch restores paused thought-complete deadline",
                nuwax_emotion_display_policy_cancel_touch(
                    &policy, 11000U) &&
                    policy.semantic_deadline_active &&
                    policy.semantic_deadline_ms == 12000U &&
                    policy.semantic_paused_remaining_ms == 0U);

    nuwax_emotion_display_policy_init(&policy);
    (void)nuwax_emotion_display_policy_apply_cloud(
        &policy, NUWAX_EMOTION_THINKING, 12000U, &generation);
    (void)nuwax_emotion_display_policy_start_interaction(
        &policy, NUWAX_EMOTION_DIZZINESS,
        NUWAX_EMOTION_SHAKE_OVERRIDE_DURATION_MS, 12100U);
    expect_true("shake covers and preserves thinking",
                policy.touch_active &&
                    policy.interaction_emotion == NUWAX_EMOTION_DIZZINESS &&
                    policy.semantic_valid &&
                    policy.semantic_emotion == NUWAX_EMOTION_THINKING);
    expect_true("thinking resumes after shake",
                 nuwax_emotion_display_policy_service(&policy, 15100U) &&
                     !policy.touch_active && policy.semantic_valid &&
                     policy.semantic_emotion == NUWAX_EMOTION_THINKING);

    nuwax_emotion_display_policy_init(&policy);
    expect_true("sleep thinking lease is accepted",
                nuwax_emotion_display_policy_apply_cloud_leased(
                    &policy, NUWAX_EMOTION_THINKING, 3000U, true,
                    20000U, &generation) &&
                    policy.semantic_valid &&
                    policy.semantic_finite_lease &&
                    policy.semantic_deadline_ms == 23000U);
    const uint32_t initial_sleep_thinking_generation = generation;
    expect_true("matching sleep lease refresh preserves generation",
                nuwax_emotion_display_policy_refresh_matching_lease(
                    &policy, NUWAX_EMOTION_THINKING, 3000U, 22000U,
                    &generation) &&
                    generation == initial_sleep_thinking_generation &&
                    policy.semantic_generation ==
                        initial_sleep_thinking_generation &&
                    policy.semantic_deadline_ms == 25000U);
    expect_true("different sleep emotion does not use refresh path",
                !nuwax_emotion_display_policy_refresh_matching_lease(
                    &policy, NUWAX_EMOTION_HAPPY, 3000U, 22000U,
                    &generation) &&
                    policy.semantic_emotion == NUWAX_EMOTION_THINKING &&
                    policy.semantic_deadline_ms == 25000U);
    expect_true("sleep thinking lease remains active before deadline",
                !nuwax_emotion_display_policy_service(&policy, 24999U) &&
                    policy.semantic_valid);
    expect_true("sleep thinking lease expires instead of becoming happy",
                nuwax_emotion_display_policy_service(&policy, 25000U) &&
                    !policy.semantic_valid &&
                    policy.semantic_emotion == NUWAX_EMOTION_NEUTRAL);

    nuwax_emotion_display_policy_init(&policy);
    (void)nuwax_emotion_display_policy_apply_cloud_leased(
        &policy, NUWAX_EMOTION_ANGRY, 3000U, true, 30000U, &generation);
    const uint32_t first_sleep_generation = generation;
    expect_true("latest sleep lease supersedes the previous lease",
                nuwax_emotion_display_policy_apply_cloud_leased(
                    &policy, NUWAX_EMOTION_PLAYFUL, 5000U, true,
                    30100U, &generation) &&
                    generation != first_sleep_generation &&
                    policy.semantic_emotion == NUWAX_EMOTION_PLAYFUL &&
                    policy.semantic_deadline_ms == 35100U);

    nuwax_emotion_display_policy_init(&policy);
    (void)nuwax_emotion_display_policy_apply_cloud_leased(
        &policy, NUWAX_EMOTION_QUESTION, 500U, true,
        UINT32_MAX - 249U, &generation);
    expect_true("sleep lease deadline is wrap safe",
                policy.semantic_deadline_ms == 250U &&
                    !nuwax_emotion_display_policy_service(&policy, 249U) &&
                    nuwax_emotion_display_policy_service(&policy, 250U) &&
                    !policy.semantic_valid);

    printf("emotion display policy host check: %s (%d failures)\n",
           s_failures ? "FAIL" : "PASS", s_failures);
    return s_failures ? 1 : 0;
}
