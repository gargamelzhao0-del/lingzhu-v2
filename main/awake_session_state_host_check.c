/* Host-only logic check. This file is intentionally excluded from firmware CMake sources. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "conversation/awake_session_state.h"

#define HOST_AWAKE_IDLE_TIMEOUT_MS 60000U

static int expect_state(const char *name,
                        const nuwax_awake_session_state_t *state,
                        uint32_t now_ms,
                        bool expected_active,
                        bool expected_waiting,
                        bool expected_expired,
                        uint32_t expected_remaining_ms,
                        uint32_t expected_generation) {
    const bool active = nuwax_awake_session_state_is_active(state);
    const bool waiting = nuwax_awake_session_state_is_waiting(state);
    const bool expired = nuwax_awake_session_state_is_expired(state, now_ms);
    const uint32_t remaining_ms =
        nuwax_awake_session_state_remaining_ms(state, now_ms);
    const bool passed = active == expected_active &&
                        waiting == expected_waiting &&
                        expired == expected_expired &&
                        remaining_ms == expected_remaining_ms &&
                        nuwax_awake_session_state_generation(state) ==
                            expected_generation;
    printf("[%s] %s active=%d waiting=%d expired=%d remaining_ms=%u generation=%u\n",
           passed ? "PASS" : "FAIL",
           name,
           active ? 1 : 0,
           waiting ? 1 : 0,
           expired ? 1 : 0,
           remaining_ms,
           nuwax_awake_session_state_generation(state));
    return passed ? 0 : 1;
}

static int expect_idle_sleep_admission(
    const char *name,
    const nuwax_awake_session_state_t *state,
    uint32_t now_ms,
    bool listening,
    bool turn_active,
    bool playback_active,
    bool speech_candidate_active,
    bool expected) {
    const bool admitted = nuwax_awake_session_state_can_enter_idle_sleep(
        state, now_ms, listening, turn_active, playback_active,
        speech_candidate_active);
    const bool passed = admitted == expected;
    printf("[%s] %s admitted=%d\n",
           passed ? "PASS" : "FAIL",
           name,
           admitted ? 1 : 0);
    return passed ? 0 : 1;
}

int main(void) {
    int failures = 0;
    nuwax_awake_session_state_t state = {0};

    failures += expect_state("initially_inactive", &state, 1000, false, false,
                             false, 0, 0);

    nuwax_awake_session_state_begin(&state, HOST_AWAKE_IDLE_TIMEOUT_MS);
    failures += expect_state("begins_paused_with_full_budget", &state, 1000,
                             true, false, false, 60000, 1);
    nuwax_awake_session_state_resume_waiting(&state, 1000);
    failures += expect_state("armed_for_one_minute", &state, 1000, true, true,
                             false, 60000, 1);
    failures += expect_state("not_expired_one_ms_early", &state, 60999, true,
                             true, false, 1, 1);
    failures += expect_idle_sleep_admission(
        "idle_sleep_rejected_before_full_minute",
        &state, 60999, true, false, false, false, false);
    failures += expect_idle_sleep_admission(
        "idle_sleep_requires_listening",
        &state, 61000, false, false, false, false, false);
    failures += expect_idle_sleep_admission(
        "idle_sleep_requires_turn_idle",
        &state, 61000, true, true, false, false, false);
    failures += expect_idle_sleep_admission(
        "idle_sleep_requires_playback_idle",
        &state, 61000, true, false, true, false, false);
    failures += expect_idle_sleep_admission(
        "idle_sleep_requires_no_pending_speech_candidate",
        &state, 61000, true, false, false, true, false);
    failures += expect_idle_sleep_admission(
        "idle_sleep_admitted_after_full_quiet_minute",
        &state, 61000, true, false, false, false, true);
    if (nuwax_awake_session_state_can_commit_deferred_idle_sleep(
            &state, 2, 61000, true, false, false, false) ||
        !nuwax_awake_session_state_can_commit_deferred_idle_sleep(
            &state, 1, 61000, true, false, false, false)) {
        printf("[FAIL] deferred idle sleep generation fence\n");
        failures++;
    } else {
        printf("[PASS] deferred idle sleep generation fence\n");
    }

    nuwax_awake_session_state_pause(&state, 31000);
    failures += expect_state("processing_pauses_remaining_budget", &state,
                             121000, true, false, false, 30000, 1);
    nuwax_awake_session_state_resume_waiting(&state, 200000);
    failures += expect_state("resume_uses_preserved_budget", &state, 229999,
                             true, true, false, 1, 1);
    failures += expect_state("expires_only_while_waiting", &state, 230000, true,
                             true, true, 0, 1);

    nuwax_awake_session_state_reset_waiting(&state, 300000);
    failures += expect_state("playback_completion_resets_full_budget", &state,
                             300000, true, true, false, 60000, 1);

    nuwax_awake_session_state_pause(&state, 359000);
    failures += expect_state("late_user_speech_preserves_old_remainder", &state,
                             359000, true, false, false, 1000, 1);
    nuwax_awake_session_state_reset_paused(&state);
    failures += expect_state("confirmed_user_speech_starts_new_full_round", &state,
                             359000, true, false, false, 60000, 1);

    nuwax_awake_session_state_reset_paused(&state);
    failures += expect_state("playback_handoff_keeps_reset_budget_paused", &state,
                             350000, true, false, false, 60000, 1);
    nuwax_awake_session_state_resume_waiting(&state, 350000);

    nuwax_awake_session_state_reset_waiting(&state, UINT32_MAX - 1000U);
    failures += expect_state("wraparound_before_deadline", &state, 50000, true,
                             true, false, 8999, 1);
    failures += expect_state("wraparound_at_deadline", &state, 58999, true,
                             true, true, 0, 1);

    nuwax_awake_session_state_clear(&state);
    failures += expect_state("clear_deactivates", &state, 200000, false, false,
                             false, 0, 1);

    nuwax_awake_session_state_begin(&state, 0);
    failures += expect_state("zero_timeout_stays_inactive", &state, 200000,
                             false, false, false, 0, 1);

    nuwax_awake_session_state_begin(&state, HOST_AWAKE_IDLE_TIMEOUT_MS);
    failures += expect_state("new_wake_advances_generation", &state, 200000,
                             true, false, false, 60000, 2);
    if (!nuwax_awake_session_state_generation_matches(&state, 2) ||
        nuwax_awake_session_state_generation_matches(&state, 1)) {
        printf("[FAIL] generation matching\n");
        failures++;
    } else {
        printf("[PASS] generation matching\n");
    }

    printf("awake session host check: %s (%d failures)\n",
           failures == 0 ? "PASS" : "FAIL",
           failures);
    return failures == 0 ? 0 : 1;
}
