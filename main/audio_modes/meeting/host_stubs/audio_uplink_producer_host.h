#pragma once

#include <assert.h>
#include "audio_uplink.h"
#include "conversation/audio_uplink_terminal.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "producer_host";
static uint8_t s_audio_upload_batch[NUWAX_AUDIO_UPLINK_PACKET_BYTES];

static void audio_uplink_refresh_adaptive_batch(nuwax_audio_uplink_t *ctx) {
    (void)ctx;
}
static void report_audio_uplink_fault(const char *code, esp_err_t err) {
    (void)code; (void)err;
    assert(false);
}
bool nuwax_ws_heartbeat_healthy_if_epoch(const nuwax_ws_client_t *ws, uint32_t epoch) {
    return nuwax_ws_is_connected(ws) && nuwax_ws_connection_epoch(ws) == epoch;
}
static esp_err_t audio_uplink_arm_terminal_recovery(nuwax_audio_uplink_t *ctx,
    const uint8_t *packet, size_t len, bool end_committed) {
    (void)ctx; (void)packet; (void)len; (void)end_committed;
    assert(false);
    return ESP_FAIL;
}
static esp_err_t audio_uplink_enqueue_batch(nuwax_audio_uplink_t *ctx,
    const uint8_t *payload, size_t len, uint32_t start_seq, uint32_t frames,
    bool final, int64_t started_us, uint32_t wait_ms) {
    (void)ctx; (void)started_us; (void)wait_ms;
    assert(payload && len > 0U && frames > 0U);
    const size_t frame_bytes = NUWAX_AUDIO_FRAME_HEADER_BYTES + 640U;
    assert(len == frames * frame_bytes);
    for (uint32_t i = 0; i < frames; ++i) {
        const uint8_t *p = payload + i * frame_bytes;
        uint32_t seq = (uint32_t)p[10] | ((uint32_t)p[11] << 8) |
                       ((uint32_t)p[12] << 16) | ((uint32_t)p[13] << 24);
        assert(seq == start_seq + i);
        assert(((p[4] & NUWAX_AUDIO_FLAG_START) != 0U) == (seq == 0U));
        assert(((p[4] & NUWAX_AUDIO_FLAG_END) != 0U) == (final && i + 1U == frames));
    }
    return ESP_OK;
}

#include "audio_uplink_producer_host.inc"

static void host_check_reject_nonzero_stream_start(void) {
    nuwax_audio_uplink_t ctx = {0};
    int16_t samples[NUWAX_AUDIO_UPLINK_PCM_FRAME_SAMPLES] = {0};
    assert(host_production_enqueue_frame(&ctx, NULL, samples,
        NUWAX_AUDIO_UPLINK_PCM_FRAME_SAMPLES, 2391U, NUWAX_AUDIO_FLAG_START,
        false) == ESP_ERR_INVALID_ARG);
    assert(!ctx.frame_seen && ctx.next_sequence == 0U);
}
