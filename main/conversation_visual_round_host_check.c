#include <stdio.h>
#include <string.h>

#include "conversation/conversation_visual_round.h"

static int s_failures;

static void expect_true(const char *name, bool value) {
    if (!value) {
        fprintf(stderr, "[FAIL] %s\n", name);
        s_failures++;
    } else {
        printf("[PASS] %s\n", name);
    }
}

int main(void) {
    nuwax_conversation_visual_round_t round = {0};
    char emotion[NUWAX_VISUAL_ROUND_EMOTION_CAPACITY] = {0};

    expect_true("invalid owner rejected",
                !nuwax_conversation_visual_round_begin(
                    &round, NUWAX_VISUAL_ROUND_NORMAL, 0U, 1U, NULL));
    expect_true("normal round begins",
                nuwax_conversation_visual_round_begin(
                    &round, NUWAX_VISUAL_ROUND_NORMAL, 4U, 7U, NULL));
    expect_true("normal emotion held before PCM",
                nuwax_conversation_visual_round_hold_emotion(
                    &round, "excited") && round.emotion_pending);
    expect_true("stale PCM rejected",
                !nuwax_conversation_visual_round_note_first_pcm(
                    &round, 3U, 7U, 12U));
    expect_true("matching PCM binds generation",
                nuwax_conversation_visual_round_note_first_pcm(
                    &round, 4U, 7U, 12U));
    expect_true("held emotion released once",
                nuwax_conversation_visual_round_take_emotion(
                    &round, emotion, sizeof(emotion)) &&
                    strcmp(emotion, "excited") == 0 &&
                    !nuwax_conversation_visual_round_take_emotion(
                        &round, emotion, sizeof(emotion)));
    expect_true("new emotion is not re-held after PCM",
                !nuwax_conversation_visual_round_hold_emotion(
                    &round, "angry"));
    expect_true("speaker ownership is generation bound",
                nuwax_conversation_visual_round_speaker_matches(
                    &round, 4U, 12U) &&
                    !nuwax_conversation_visual_round_speaker_matches(
                        &round, 4U, 13U));

    expect_true("wake request owner begins",
                nuwax_conversation_visual_round_begin(
                    &round, NUWAX_VISUAL_ROUND_WAKE_GREETING,
                    5U, 8U, "wake_9"));
    expect_true("wake request matching is exact",
                nuwax_conversation_visual_round_request_matches(
                    &round, "wake_9") &&
                    !nuwax_conversation_visual_round_request_matches(
                        &round, "wake_8"));
    expect_true("wake round cannot retain cloud emotion",
                !nuwax_conversation_visual_round_hold_emotion(
                    &round, "happy"));

    expect_true("touch round is interaction",
                nuwax_conversation_visual_round_begin(
                    &round, NUWAX_VISUAL_ROUND_HEAD_TOUCH,
                    5U, 8U, "touch_10") &&
                    nuwax_conversation_visual_round_is_interaction(&round));
    expect_true("shake completion remains generation bound",
                nuwax_conversation_visual_round_begin(
                    &round, NUWAX_VISUAL_ROUND_SHAKE,
                    6U, 9U, "shake_11") &&
                    nuwax_conversation_visual_round_note_first_pcm(
                        &round, 6U, 9U, 21U) &&
                    nuwax_conversation_visual_round_is_interaction(&round) &&
                    nuwax_conversation_visual_round_speaker_matches(
                        &round, 6U, 21U) &&
                    !nuwax_conversation_visual_round_speaker_matches(
                        &round, 6U, 20U));
    nuwax_conversation_visual_round_clear(&round);
    expect_true("clear retires every owner",
                !nuwax_conversation_visual_round_is_active(&round) &&
                    !round.emotion_pending && round.speaker_generation == 0U);

    printf("conversation visual round host check: %s (%d failures)\n",
           s_failures ? "FAIL" : "PASS", s_failures);
    return s_failures ? 1 : 0;
}
