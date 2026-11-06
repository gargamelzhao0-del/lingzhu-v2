/* Host-only low-latency batching policy check. Not part of firmware sources. */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "conversation/audio_uplink.h"

static int expect_target(const char *name,
                         const nuwax_audio_uplink_t *ctx,
                         uint32_t expected) {
    const uint32_t actual = nuwax_audio_uplink_batch_target_frames(ctx);
    if (actual == expected) {
        printf("[PASS] %s target=%lu\n", name, (unsigned long)actual);
        return 0;
    }
    printf("[FAIL] %s target=%lu expected=%lu\n",
           name,
           (unsigned long)actual,
           (unsigned long)expected);
    return 1;
}

int main(void) {
    nuwax_audio_uplink_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    int failures = 0;

    if (NUWAX_CONVERSATION_CAPTURE_SLOT_DEPTH != 12U ||
        NUWAX_CONVERSATION_CAPTURE_PRE_DSP_MAX_AGE_MS != 60U ||
        NUWAX_CONVERSATION_BARGE_END_TO_END_MAX_AGE_MS != 100U ||
        NUWAX_CONVERSATION_UPLINK_PACKET_SLOT_DEPTH != 64U ||
        NUWAX_CONVERSATION_UPLINK_STAGING_MAX_AGE_MS != 1000U ||
        NUWAX_CONVERSATION_CAPTURE_PRE_DSP_MAX_AGE_MS +
                NUWAX_CONVERSATION_PROFILE_MIN_DSP_RESERVE_MS >
            NUWAX_CONVERSATION_BARGE_END_TO_END_MAX_AGE_MS) {
        puts("[FAIL] normal-conversation latency contract drifted");
        failures++;
    } else {
        puts("[PASS] normal-conversation latency contract reserves DSP time");
    }

    if (NUWAX_AUDIO_UPLINK_REALTIME_QUEUE_DEPTH != 64U ||
#if CONFIG_NUWAX_VOICE_UPLINK_CODEC_OPUS
        NUWAX_AUDIO_UPLINK_RELIABLE_QUEUE_DEPTH != 32U ||
#else
        NUWAX_AUDIO_UPLINK_RELIABLE_QUEUE_DEPTH != 128U ||
#endif
        NUWAX_AUDIO_UPLINK_SEND_QUEUE_DEPTH !=
            NUWAX_AUDIO_UPLINK_RELIABLE_QUEUE_DEPTH ||
        NUWAX_AUDIO_UPLINK_REALTIME_QUEUE_DEPTH >=
            NUWAX_AUDIO_UPLINK_RELIABLE_QUEUE_DEPTH) {
        puts("[FAIL] realtime and reliable queue profiles are not isolated");
        failures++;
    } else {
        puts("[PASS] realtime and reliable queue profiles isolated");
    }

    if (!nuwax_audio_uplink_reserves_terminal_slot(
            NUWAX_AUDIO_UPLINK_PROFILE_REALTIME) ||
        nuwax_audio_uplink_reserves_terminal_slot(
            NUWAX_AUDIO_UPLINK_PROFILE_RELIABLE) ||
        nuwax_audio_uplink_pool_slot_count(
            NUWAX_AUDIO_UPLINK_PROFILE_REALTIME,
            NUWAX_AUDIO_UPLINK_REALTIME_QUEUE_DEPTH) !=
            NUWAX_AUDIO_UPLINK_REALTIME_QUEUE_DEPTH + 1U ||
        nuwax_audio_uplink_ready_queue_depth(
            NUWAX_AUDIO_UPLINK_PROFILE_REALTIME,
            NUWAX_AUDIO_UPLINK_REALTIME_QUEUE_DEPTH) !=
            NUWAX_AUDIO_UPLINK_REALTIME_QUEUE_DEPTH + 1U ||
        nuwax_audio_uplink_pool_slot_count(
            NUWAX_AUDIO_UPLINK_PROFILE_RELIABLE,
            NUWAX_AUDIO_UPLINK_RELIABLE_QUEUE_DEPTH) !=
            NUWAX_AUDIO_UPLINK_RELIABLE_QUEUE_DEPTH ||
        nuwax_audio_uplink_ready_queue_depth(
            NUWAX_AUDIO_UPLINK_PROFILE_RELIABLE,
            NUWAX_AUDIO_UPLINK_RELIABLE_QUEUE_DEPTH) !=
            NUWAX_AUDIO_UPLINK_RELIABLE_QUEUE_DEPTH) {
        puts("[FAIL] queue-full realtime END does not have an isolated packet/ready slot");
        failures++;
    } else {
        puts("[PASS] queue-full realtime END retains one isolated packet/ready slot");
    }

    if (!nuwax_audio_uplink_is_recoverable_backpressure(
            ESP_ERR_NOT_FINISHED) ||
        nuwax_audio_uplink_is_recoverable_backpressure(ESP_ERR_NO_MEM) ||
        nuwax_audio_uplink_is_recoverable_backpressure(ESP_ERR_TIMEOUT)) {
        puts("[FAIL] transient queue pressure is not isolated from fatal errors");
        failures++;
    } else {
        puts("[PASS] transient queue pressure isolated from fatal errors");
    }

    if (nuwax_audio_uplink_queue_pressure_result(
            NUWAX_AUDIO_UPLINK_PROFILE_REALTIME, false, 1U) !=
            ESP_ERR_NOT_FINISHED ||
        nuwax_audio_uplink_queue_pressure_result(
            NUWAX_AUDIO_UPLINK_PROFILE_REALTIME, true, 1U) !=
            ESP_ERR_TIMEOUT ||
        nuwax_audio_uplink_queue_pressure_result(
            NUWAX_AUDIO_UPLINK_PROFILE_RELIABLE, false, 1U) !=
            ESP_ERR_NO_MEM ||
        nuwax_audio_uplink_queue_pressure_result(
            NUWAX_AUDIO_UPLINK_PROFILE_REALTIME, false, 0U) !=
            ESP_ERR_NO_MEM) {
        puts("[FAIL] queue pressure policy does not preserve profile/END semantics");
        failures++;
    } else {
        puts("[PASS] realtime tail deferral is isolated from reliable/final traffic");
    }

    if (nuwax_audio_uplink_admission_wait_ms(
            NUWAX_AUDIO_UPLINK_PROFILE_REALTIME, false) != 0U ||
        nuwax_audio_uplink_admission_wait_ms(
            NUWAX_AUDIO_UPLINK_PROFILE_RELIABLE, false) != 80U ||
        nuwax_audio_uplink_admission_wait_ms(
            NUWAX_AUDIO_UPLINK_PROFILE_REALTIME, true) != 0U ||
        nuwax_audio_uplink_admission_wait_ms(
            NUWAX_AUDIO_UPLINK_PROFILE_RELIABLE, true) != 5000U) {
        puts("[FAIL] realtime admission can block the 20 ms capture/AEC path");
        failures++;
    } else {
        puts("[PASS] realtime data and END admission are non-blocking; reliable waits remain bounded");
    }

    if (nuwax_audio_uplink_send_budget_ms(
            NUWAX_AUDIO_UPLINK_PROFILE_RELIABLE, 1000U, 999999U) !=
            UINT32_MAX ||
        nuwax_audio_uplink_send_budget_ms(
            NUWAX_AUDIO_UPLINK_PROFILE_REALTIME, 0U, 1000U) != 1000U ||
        nuwax_audio_uplink_send_budget_ms(
            NUWAX_AUDIO_UPLINK_PROFILE_REALTIME, 1000U, 999000U) != 2U ||
        nuwax_audio_uplink_send_budget_ms(
            NUWAX_AUDIO_UPLINK_PROFILE_REALTIME, 1000U, 1000999U) != 1U ||
        nuwax_audio_uplink_send_budget_ms(
            NUWAX_AUDIO_UPLINK_PROFILE_REALTIME, 1000U, 1001000U) != 0U) {
        puts("[FAIL] realtime packet age budget is incorrect");
        failures++;
    } else {
        puts("[PASS] realtime packet age is bounded to 1000 ms");
    }

    if (nuwax_audio_uplink_recovery_send_budget_ms(
            NUWAX_AUDIO_UPLINK_PROFILE_REALTIME, 1000U, 1001000U) != 200U ||
        nuwax_audio_uplink_recovery_send_budget_ms(
            NUWAX_AUDIO_UPLINK_PROFILE_REALTIME, 1000U, 1200999U) != 1U ||
        nuwax_audio_uplink_recovery_send_budget_ms(
            NUWAX_AUDIO_UPLINK_PROFILE_REALTIME, 1000U, 1201000U) != 0U ||
        nuwax_audio_uplink_packet_recovery_send_budget_ms(
            NUWAX_AUDIO_UPLINK_PROFILE_REALTIME, true, 1000U, 1201000U) !=
            NUWAX_AUDIO_UPLINK_REALTIME_TERMINAL_SEND_TIMEOUT_MS) {
        puts("[FAIL] realtime bounded recovery or terminal budget is incorrect");
        failures++;
    } else {
        puts("[PASS] realtime recovery is bounded to 1200 ms; END remains transactional");
    }

    if (nuwax_audio_uplink_packet_send_budget_ms(
            NUWAX_AUDIO_UPLINK_PROFILE_REALTIME, true, 1000U, 999999U) !=
            NUWAX_AUDIO_UPLINK_REALTIME_TERMINAL_SEND_TIMEOUT_MS ||
        nuwax_audio_uplink_packet_send_budget_ms(
            NUWAX_AUDIO_UPLINK_PROFILE_REALTIME, false, 1000U, 221000U) !=
            780U ||
        !nuwax_audio_uplink_can_skip_unsent_data_packet(
            NUWAX_AUDIO_UPLINK_PROFILE_REALTIME, false, ESP_ERR_TIMEOUT,
            true, true) ||
        nuwax_audio_uplink_can_skip_unsent_data_packet(
            NUWAX_AUDIO_UPLINK_PROFILE_REALTIME, true, ESP_ERR_TIMEOUT,
            true, true) ||
        nuwax_audio_uplink_can_skip_unsent_data_packet(
            NUWAX_AUDIO_UPLINK_PROFILE_RELIABLE, false, ESP_ERR_TIMEOUT,
            true, true) ||
        nuwax_audio_uplink_can_skip_unsent_data_packet(
            NUWAX_AUDIO_UPLINK_PROFILE_REALTIME, false, ESP_ERR_TIMEOUT,
            false, true) ||
        nuwax_audio_uplink_can_skip_unsent_data_packet(
            NUWAX_AUDIO_UPLINK_PROFILE_REALTIME, false,
            ESP_ERR_INVALID_STATE,
            true, true)) {
        puts("[FAIL] realtime unsent data/terminal transaction policy is incorrect");
        failures++;
    } else {
        puts("[PASS] only safe unsent realtime data is skippable; END remains transactional");
    }

    if (!nuwax_audio_uplink_enqueue_frame_accepted(ESP_OK) ||
        !nuwax_audio_uplink_enqueue_frame_accepted(ESP_ERR_NOT_FINISHED) ||
        nuwax_audio_uplink_enqueue_frame_accepted(ESP_ERR_NO_MEM)) {
        puts("[FAIL] deferred frame acceptance contract is ambiguous");
        failures++;
    } else {
        puts("[PASS] deferred frame remains accepted by capture accounting");
    }

    if (nuwax_audio_uplink_resolve_capture_finish_result(
            ESP_ERR_NOT_FINISHED, ESP_OK) != ESP_ERR_NOT_FINISHED ||
        nuwax_audio_uplink_resolve_capture_finish_result(
            ESP_ERR_NOT_FINISHED, ESP_ERR_TIMEOUT) != ESP_ERR_TIMEOUT ||
        nuwax_audio_uplink_resolve_capture_finish_result(
            ESP_ERR_NO_MEM, ESP_OK) != ESP_ERR_NO_MEM ||
        nuwax_audio_uplink_resolve_capture_finish_result(
            ESP_OK, ESP_ERR_TIMEOUT) != ESP_ERR_TIMEOUT) {
        puts("[FAIL] capture/finish error resolution contract is incorrect");
        failures++;
    } else {
        puts("[PASS] successful drain recovers only transient backpressure");
    }

    failures += expect_target("default", &ctx,
                              NUWAX_AUDIO_UPLINK_STREAM_BATCH_FRAMES);

    esp_err_t err = nuwax_audio_uplink_set_low_latency(&ctx, true);
    if (err != ESP_OK) {
        printf("[FAIL] enable low latency err=%ld\n", (long)err);
        failures++;
    }
    failures += expect_target("low-latency first batch", &ctx,
                              NUWAX_AUDIO_UPLINK_LOW_LATENCY_FIRST_BATCH_FRAMES);

    nuwax_audio_uplink_record_batch_result(&ctx, false);
    failures += expect_target("failed first enqueue", &ctx,
                              NUWAX_AUDIO_UPLINK_LOW_LATENCY_FIRST_BATCH_FRAMES);

    nuwax_audio_uplink_record_batch_result(&ctx, true);
    failures += expect_target("successful first enqueue", &ctx,
                              NUWAX_AUDIO_UPLINK_LOW_LATENCY_STEADY_BATCH_FRAMES);
    if (nuwax_audio_uplink_adaptive_batch_frames(0U, 16U) != 1U ||
        nuwax_audio_uplink_adaptive_batch_frames(3U, 16U) != 1U ||
        nuwax_audio_uplink_adaptive_batch_frames(4U, 16U) != 2U ||
        nuwax_audio_uplink_adaptive_batch_frames(7U, 16U) != 2U ||
        nuwax_audio_uplink_adaptive_batch_frames(8U, 16U) != 4U ||
        nuwax_audio_uplink_adaptive_batch_frames(16U, 16U) != 4U) {
        puts("[FAIL] realtime batching does not adapt 1/2/4 under pressure");
        failures++;
    } else {
        puts("[PASS] realtime batching adapts 1/2/4 under pressure");
    }

    memset(&ctx, 0, sizeof(ctx));
    failures += expect_target("start/reset", &ctx,
                              NUWAX_AUDIO_UPLINK_STREAM_BATCH_FRAMES);

    err = nuwax_audio_uplink_set_low_latency(&ctx, true);
    ctx.frame_seen = true;
    if (err != ESP_OK ||
        nuwax_audio_uplink_set_low_latency(&ctx, false) != ESP_ERR_INVALID_STATE) {
        puts("[FAIL] mode change after first frame must be rejected");
        failures++;
    } else {
        puts("[PASS] mode change after first frame rejected");
    }

    printf("audio uplink low-latency host check: %s (%d failures)\n",
           failures == 0 ? "PASS" : "FAIL",
           failures);
    return failures == 0 ? 0 : 1;
}
