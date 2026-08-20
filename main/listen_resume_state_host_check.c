/* Host-only logic check. This file is intentionally excluded from firmware CMake sources. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "conversation/listen_resume_state.h"

static int expect_match(const char *name,
                        const nuwax_listen_resume_state_t *state,
                        uint32_t generation,
                        const char *audio_session_id,
                        bool expected) {
    const bool actual = nuwax_listen_resume_state_event_matches(
        state, generation, audio_session_id);
    const bool passed = actual == expected;
    printf("[%s] %s match=%d\n",
           passed ? "PASS" : "FAIL",
           name,
           actual ? 1 : 0);
    return passed ? 0 : 1;
}

int main(void) {
    int failures = 0;
    nuwax_listen_resume_state_t state = {0};

    nuwax_listen_resume_state_bind_round(&state, 7, "conversation_7_12");
    failures += expect_match("same_round", &state, 7, "conversation_7_12", true);
    failures += expect_match("old_generation", &state, 6, "conversation_7_12", false);
    failures += expect_match("old_audio_session", &state, 7, "conversation_7_11", false);
    failures += expect_match("bound_round_requires_id", &state, 7, "", false);
    if (!nuwax_listen_resume_state_can_bootstrap_cloud_turn(
            &state, 7, 11, true) ||
        nuwax_listen_resume_state_can_bootstrap_cloud_turn(
            &state, 6, 11, true) ||
        nuwax_listen_resume_state_can_bootstrap_cloud_turn(
            &state, 7, 11, false)) {
        printf("[FAIL] active capture cloud-turn bootstrap admission\n");
        failures++;
    } else {
        printf("[PASS] active capture cloud-turn bootstrap admission\n");
    }

    nuwax_listen_resume_state_arm_no_speech(&state, "awake_session", 12345);
    if (!nuwax_listen_resume_state_has_no_speech(&state) ||
        nuwax_listen_resume_state_deadline_ms(&state) != 12345) {
        printf("[FAIL] no-speech resume metadata\n");
        failures++;
    } else {
        printf("[PASS] no-speech resume metadata\n");
    }

    if (!nuwax_listen_resume_state_mark_post_end_terminal(&state, 11) ||
        !nuwax_listen_resume_state_has_post_end_terminal(&state) ||
        !nuwax_listen_resume_state_can_bootstrap_cloud_turn(
            &state, 7, 11, false) ||
        nuwax_listen_resume_state_can_bootstrap_cloud_turn(
            &state, 7, 12, false) ||
        nuwax_listen_resume_state_mark_post_end_terminal(&state, 12)) {
        printf("[FAIL] binary END terminal ownership metadata\n");
        failures++;
    } else {
        printf("[PASS] binary END terminal ownership metadata\n");
    }

    if (!nuwax_listen_resume_state_arm_post_end_asr(&state, 11, 12350) ||
        !nuwax_listen_resume_state_is_waiting_post_end_asr(&state) ||
        nuwax_listen_resume_state_ws_epoch(&state) != 11 ||
        !nuwax_listen_resume_state_can_bootstrap_cloud_turn(
            &state, 7, 11, false) ||
        nuwax_listen_resume_state_can_bootstrap_cloud_turn(
            &state, 7, 12, false) ||
        nuwax_listen_resume_state_post_end_asr_expired(&state, 12349) ||
        !nuwax_listen_resume_state_post_end_asr_expired(&state, 12350)) {
        printf("[FAIL] post-END ASR watchdog metadata\n");
        failures++;
    } else {
        printf("[PASS] post-END ASR watchdog metadata\n");
    }

    nuwax_listen_resume_state_set_idle_expiry_pending(&state, true);
    if (!nuwax_listen_resume_state_is_idle_expiry_pending(&state) ||
        !nuwax_listen_resume_state_holds_idle_expiry_boundary(&state)) {
        printf("[FAIL] idle-expiry boundary metadata\n");
        failures++;
    } else {
        printf("[PASS] idle-expiry boundary metadata\n");
    }
    nuwax_listen_resume_state_bind_round(&state, 7, "conversation_7_13");
    if (nuwax_listen_resume_state_is_idle_expiry_pending(&state) ||
        nuwax_listen_resume_state_is_waiting_post_end_asr(&state) ||
        nuwax_listen_resume_state_has_post_end_terminal(&state) ||
        nuwax_listen_resume_state_has_no_speech(&state) ||
        nuwax_listen_resume_state_ws_epoch(&state) != 0U) {
        printf("[FAIL] new round clears terminal metadata\n");
        failures++;
    } else {
        printf("[PASS] new round clears terminal metadata\n");
    }
    failures += expect_match("new_round", &state, 7,
                             "conversation_7_13", true);

    if (!nuwax_listen_resume_state_arm_post_end_asr(&state, 11, 12350)) {
        printf("[FAIL] rebound round can arm post-END watchdog\n");
        failures++;
    }

    nuwax_listen_resume_state_refresh_post_end_asr(&state, 12400);
    if (nuwax_listen_resume_state_post_end_asr_expired(&state, 12399) ||
        !nuwax_listen_resume_state_post_end_asr_expired(&state, 12400)) {
        printf("[FAIL] post-END ASR watchdog refresh\n");
        failures++;
    } else {
        printf("[PASS] post-END ASR watchdog refresh\n");
    }

    nuwax_listen_resume_state_disarm_post_end_asr(&state);
    if (nuwax_listen_resume_state_is_waiting_post_end_asr(&state) ||
        nuwax_listen_resume_state_has_post_end_terminal(&state) ||
        !nuwax_listen_resume_state_event_matches(
            &state, 7, "conversation_7_13")) {
        printf("[FAIL] post-END ASR disarm preserves round identity\n");
        failures++;
    } else {
        printf("[PASS] post-END ASR disarm preserves round identity\n");
    }

    nuwax_listen_resume_state_clear(&state);
    failures += expect_match("clear_invalidates_round", &state, 7,
                             "conversation_7_12", false);
    if (nuwax_listen_resume_state_arm_post_end_asr(&state, 11, 12500)) {
        printf("[FAIL] cleared round cannot arm post-END ASR watchdog\n");
        failures++;
    } else {
        printf("[PASS] cleared round cannot arm post-END ASR watchdog\n");
    }

    printf("listen resume host check: %s (%d failures)\n",
           failures == 0 ? "PASS" : "FAIL",
           failures);
    return failures == 0 ? 0 : 1;
}
