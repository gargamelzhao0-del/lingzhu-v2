/* Host-only check. This file is intentionally excluded from firmware CMake sources. */

#include <stdio.h>

#include "conversation/conversation_audio_health.h"

#define CHECK(condition, message)                                      \
    do {                                                               \
        if (!(condition)) {                                            \
            printf("[FAIL] %s:%d: %s\n", __func__, __LINE__, message); \
            return 1;                                                  \
        }                                                              \
    } while (0)

static nuwax_conversation_audio_health_frame_t healthy_playback_frame(void) {
    return (nuwax_conversation_audio_health_frame_t){
        .enabled = true,
        .initialized = true,
        .playback_active = true,
        .reference_generation_matches = true,
        .reference_contiguous = true,
        .reference_active = true,
        .reference_locked = true,
        .aec_frame_processed = true,
        .frame_age_us = 10000U,
    };
}

static int check_fail_closed_default(void) {
    nuwax_conversation_audio_health_t health = {0};
    nuwax_conversation_audio_health_frame_t frame = healthy_playback_frame();
    nuwax_conversation_audio_health_result_t result = {
        .aec_health = NUWAX_BARGE_HEALTH_GREEN,
        .reference_health = NUWAX_BARGE_HEALTH_GREEN,
        .reference_locked = true,
    };
    CHECK(!nuwax_conversation_audio_health_process(&health, &frame, &result),
          "uninitialized classifier accepted input");
    CHECK(result.aec_health == NUWAX_BARGE_HEALTH_RED &&
              result.reference_health == NUWAX_BARGE_HEALTH_RED &&
              !result.reference_locked,
          "invalid processing did not leave a fail-closed result");
    return 0;
}

static int check_clean_lock_and_silent_reference(void) {
    nuwax_conversation_audio_health_t health = {0};
    nuwax_conversation_audio_health_result_t result = {0};
    CHECK(nuwax_conversation_audio_health_init(&health, NULL),
          "default init failed");
    nuwax_conversation_audio_health_frame_t frame = healthy_playback_frame();
    for (uint32_t i = 1U; i <= 3U; i++) {
        CHECK(nuwax_conversation_audio_health_process(&health, &frame, &result),
              "clean frame failed");
        const nuwax_barge_health_t expected =
            i < 3U ? NUWAX_BARGE_HEALTH_YELLOW : NUWAX_BARGE_HEALTH_GREEN;
        CHECK(result.aec_health == expected &&
                  result.reference_health == expected,
              "clean lock hysteresis mismatch");
        CHECK(result.reference_locked == (i == 3U),
              "reference lock exposed too early or too late");
    }
    CHECK(result.metrics_runtime_state == NUWAX_AEC_RUNTIME_HEALTHY &&
              result.metrics_reason_bits == NUWAX_AEC_REASON_NONE &&
              !result.fail_closed,
          "healthy runtime classification mismatch");

    nuwax_conversation_audio_health_reset(&health);
    frame = healthy_playback_frame();
    frame.reference_active = false;
    frame.reference_locked = false;
    frame.aec_frame_processed = false;
    for (uint32_t i = 1U; i <= 3U; i++) {
        CHECK(nuwax_conversation_audio_health_process(&health, &frame, &result),
              "silent reference frame failed");
    }
    CHECK(result.reference_health == NUWAX_BARGE_HEALTH_GREEN &&
              result.reference_locked &&
              result.aec_health == NUWAX_BARGE_HEALTH_YELLOW &&
              result.verified_silent_passthrough &&
              !result.fail_closed &&
              (result.metrics_reason_bits &
               NUWAX_AEC_REASON_REFERENCE_INACTIVE) != 0U &&
              (result.metrics_reason_bits &
               NUWAX_AEC_REASON_RAW_FALLBACK) != 0U &&
              result.metrics_runtime_state == NUWAX_AEC_RUNTIME_DEGRADED,
          "continuous silent reference was treated as missing");

    frame.output_underrun_delta = 1U;
    CHECK(nuwax_conversation_audio_health_process(&health, &frame, &result),
          "silent-reference underrun frame failed");
    CHECK(!result.verified_silent_passthrough,
          "an output underrun was marked as verified silent pass-through");
    return 0;
}

static int check_transient_and_persistent_reference_fault(void) {
    nuwax_conversation_audio_health_t health = {0};
    nuwax_conversation_audio_health_result_t result = {0};
    CHECK(nuwax_conversation_audio_health_init(&health, NULL), "init failed");
    nuwax_conversation_audio_health_frame_t frame = healthy_playback_frame();
    for (uint32_t i = 0U; i < 3U; i++) {
        CHECK(nuwax_conversation_audio_health_process(&health, &frame, &result),
              "arming frame failed");
    }

    frame.reference_contiguous = false;
    CHECK(nuwax_conversation_audio_health_process(&health, &frame, &result),
          "transient gap failed");
    CHECK(result.reference_health == NUWAX_BARGE_HEALTH_YELLOW &&
              result.aec_health == NUWAX_BARGE_HEALTH_YELLOW &&
              !result.reference_locked && !result.fail_closed &&
              result.reference_anomaly_frames == 1U &&
              (result.metrics_reason_bits & NUWAX_AEC_REASON_REFERENCE_GAP) != 0U,
          "single reference gap was not YELLOW");
    CHECK(nuwax_conversation_audio_health_process(&health, &frame, &result),
          "second gap failed");
    CHECK(nuwax_conversation_audio_health_process(&health, &frame, &result),
          "third gap failed");
    CHECK(result.reference_health == NUWAX_BARGE_HEALTH_RED &&
              result.aec_health == NUWAX_BARGE_HEALTH_RED &&
              result.fail_closed &&
              result.metrics_runtime_state == NUWAX_AEC_RUNTIME_DEGRADED,
          "persistent reference gap did not fail closed");

    frame = healthy_playback_frame();
    for (uint32_t i = 1U; i <= 3U; i++) {
        CHECK(nuwax_conversation_audio_health_process(&health, &frame, &result),
              "recovery frame failed");
        CHECK(result.reference_health ==
                  (i < 3U ? NUWAX_BARGE_HEALTH_YELLOW
                          : NUWAX_BARGE_HEALTH_GREEN),
              "fail-closed recovery did not require stable frames");
    }
    CHECK(result.reference_locked && !result.fail_closed,
          "reference did not recover after a fresh stable run");
    return 0;
}

static int check_missing_and_bypass(void) {
    nuwax_conversation_audio_health_t health = {0};
    nuwax_conversation_audio_health_result_t result = {0};
    CHECK(nuwax_conversation_audio_health_init(&health, NULL), "init failed");
    nuwax_conversation_audio_health_frame_t frame = healthy_playback_frame();
    frame.reference_active = false;
    frame.reference_locked = false;
    frame.ref_missing = true;
    frame.aec_frame_processed = false;
    for (uint32_t i = 1U; i <= 3U; i++) {
        CHECK(nuwax_conversation_audio_health_process(&health, &frame, &result),
              "missing reference frame failed");
    }
    CHECK(result.reference_health == NUWAX_BARGE_HEALTH_RED &&
              result.aec_health == NUWAX_BARGE_HEALTH_RED &&
              !result.reference_locked && result.fail_closed &&
              result.metrics_runtime_state == NUWAX_AEC_RUNTIME_BYPASSED &&
              (result.metrics_reason_bits & NUWAX_AEC_REASON_REFERENCE_MISSING) != 0U &&
              (result.metrics_reason_bits & NUWAX_AEC_REASON_RAW_FALLBACK) != 0U,
          "missing high-energy path could become healthy or hide bypass");
    return 0;
}

static int check_hard_failures_and_quality_reasons(void) {
    nuwax_conversation_audio_health_t health = {0};
    nuwax_conversation_audio_health_result_t result = {0};
    CHECK(nuwax_conversation_audio_health_init(&health, NULL), "init failed");
    nuwax_conversation_audio_health_frame_t frame = healthy_playback_frame();
    frame.processing_error = true;
    CHECK(nuwax_conversation_audio_health_process(&health, &frame, &result),
          "processing error frame failed");
    CHECK(result.aec_health == NUWAX_BARGE_HEALTH_RED && result.fail_closed &&
              result.metrics_runtime_state == NUWAX_AEC_RUNTIME_FAILED &&
              (result.metrics_reason_bits & NUWAX_AEC_REASON_PROCESSING_ERROR) != 0U,
          "processing error was not an immediate hard failure");

    nuwax_conversation_audio_health_reset(&health);
    frame = healthy_playback_frame();
    frame.enabled = false;
    frame.initialized = false;
    CHECK(nuwax_conversation_audio_health_process(&health, &frame, &result),
          "disabled frame failed");
    CHECK(result.aec_health == NUWAX_BARGE_HEALTH_RED &&
              result.metrics_runtime_state == NUWAX_AEC_RUNTIME_DISABLED &&
              (result.metrics_reason_bits & NUWAX_AEC_REASON_CONFIG_DISABLED) != 0U &&
              (result.metrics_reason_bits & NUWAX_AEC_REASON_NOT_PREPARED) != 0U,
          "disabled AEC was not fail closed");

    nuwax_conversation_audio_health_reset(&health);
    frame = healthy_playback_frame();
    frame.microphone_clipped = true;
    frame.reference_clipped = true;
    frame.output_underrun_delta = 1U;
    frame.frame_age_us = 60001U;
    CHECK(nuwax_conversation_audio_health_process(&health, &frame, &result),
          "quality fault frame failed");
    const nuwax_aec_runtime_reason_bits_t expected =
        NUWAX_AEC_REASON_MIC_CLIPPING |
        NUWAX_AEC_REASON_REFERENCE_CLIPPING |
        NUWAX_AEC_REASON_OUTPUT_UNDERRUN |
        NUWAX_AEC_REASON_DSP_DEADLINE_MISS;
    CHECK(result.aec_health == NUWAX_BARGE_HEALTH_YELLOW &&
              result.reference_health == NUWAX_BARGE_HEALTH_YELLOW &&
              (result.metrics_reason_bits & expected) == expected,
          "transient clipping/underrun/deadline reasons were incomplete");
    return 0;
}

static int check_idle_and_playback_transition(void) {
    nuwax_conversation_audio_health_t health = {0};
    nuwax_conversation_audio_health_result_t result = {0};
    CHECK(nuwax_conversation_audio_health_init(&health, NULL), "init failed");
    nuwax_conversation_audio_health_frame_t frame = healthy_playback_frame();
    frame.playback_active = false;
    frame.reference_generation_matches = false;
    frame.reference_contiguous = false;
    frame.reference_active = false;
    frame.reference_locked = false;
    frame.aec_frame_processed = false;
    CHECK(nuwax_conversation_audio_health_process(&health, &frame, &result),
          "idle frame failed");
    CHECK(result.aec_health == NUWAX_BARGE_HEALTH_GREEN &&
              result.reference_health == NUWAX_BARGE_HEALTH_GREEN &&
              !result.reference_locked && !result.fail_closed &&
              result.metrics_runtime_state == NUWAX_AEC_RUNTIME_HEALTHY,
          "idle reference was incorrectly required");

    frame = healthy_playback_frame();
    CHECK(nuwax_conversation_audio_health_process(&health, &frame, &result),
          "playback transition failed");
    CHECK(result.aec_health == NUWAX_BARGE_HEALTH_YELLOW &&
              result.reference_health == NUWAX_BARGE_HEALTH_YELLOW &&
              result.aec_stable_frames == 1U &&
              result.reference_stable_frames == 1U,
          "playback transition reused stale idle health");
    return 0;
}

static int check_config_and_generation_binding(void) {
    nuwax_conversation_audio_health_t health = {0};
    nuwax_conversation_audio_health_result_t result = {0};
    nuwax_conversation_audio_health_config_t config = {0};
    CHECK(!nuwax_conversation_audio_health_init(&health, &config),
          "invalid zero config was accepted");

    config = (nuwax_conversation_audio_health_config_t){
        .stable_frames_for_green = 2U,
        .anomaly_frames_for_red = 2U,
        .maximum_frame_age_us = 40000U,
    };
    CHECK(nuwax_conversation_audio_health_init(&health, &config),
          "valid custom config was rejected");
    nuwax_conversation_audio_health_frame_t frame = healthy_playback_frame();
    frame.reference_generation_matches = false;
    CHECK(nuwax_conversation_audio_health_process(&health, &frame, &result),
          "first generation mismatch failed");
    CHECK(result.reference_health == NUWAX_BARGE_HEALTH_YELLOW &&
              (result.metrics_reason_bits &
               NUWAX_AEC_REASON_REFERENCE_GENERATION_MISMATCH) != 0U,
          "transient generation mismatch was not YELLOW");
    CHECK(nuwax_conversation_audio_health_process(&health, &frame, &result),
          "second generation mismatch failed");
    CHECK(result.reference_health == NUWAX_BARGE_HEALTH_RED &&
              result.aec_health == NUWAX_BARGE_HEALTH_RED &&
              result.fail_closed,
          "persistent generation mismatch did not use custom RED threshold");

    nuwax_conversation_audio_health_reset(&health);
    frame = healthy_playback_frame();
    frame.initialized = false;
    CHECK(nuwax_conversation_audio_health_process(&health, &frame, &result),
          "unprepared frame failed");
    CHECK(result.aec_health == NUWAX_BARGE_HEALTH_RED &&
              result.metrics_runtime_state == NUWAX_AEC_RUNTIME_INITIALIZING &&
              (result.metrics_reason_bits & NUWAX_AEC_REASON_NOT_PREPARED) != 0U,
          "unprepared AEC did not fail closed as INITIALIZING");
    return 0;
}

int main(void) {
    int failures = 0;
    failures += check_fail_closed_default();
    failures += check_clean_lock_and_silent_reference();
    failures += check_transient_and_persistent_reference_fault();
    failures += check_missing_and_bypass();
    failures += check_hard_failures_and_quality_reasons();
    failures += check_idle_and_playback_transition();
    failures += check_config_and_generation_binding();
    printf("conversation audio health host check: %s (%d failures)\n",
           failures == 0 ? "PASS" : "FAIL",
           failures);
    return failures == 0 ? 0 : 1;
}
