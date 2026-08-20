#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "barge_in_controller.h"
#include "conversation_metrics.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t stable_frames_for_green;
    uint8_t anomaly_frames_for_red;
    uint32_t maximum_frame_age_us;
} nuwax_conversation_audio_health_config_t;

typedef struct {
    bool enabled;
    bool initialized;
    bool playback_active;
    bool reference_generation_matches;
    bool reference_contiguous;
    bool reference_active;
    bool reference_locked;
    bool aec_frame_processed;
    bool ref_missing;
    bool processing_error;
    bool microphone_clipped;
    bool reference_clipped;
    uint32_t output_underrun_delta;
    uint32_t frame_age_us;
    bool deadline_missed;
} nuwax_conversation_audio_health_frame_t;

typedef struct {
    nuwax_barge_health_t aec_health;
    nuwax_barge_health_t reference_health;
    bool reference_locked;
    bool verified_silent_passthrough;
    bool fail_closed;
    uint16_t aec_stable_frames;
    uint16_t aec_anomaly_frames;
    uint16_t reference_stable_frames;
    uint16_t reference_anomaly_frames;
    nuwax_aec_runtime_state_t metrics_runtime_state;
    nuwax_aec_runtime_reason_bits_t metrics_reason_bits;
} nuwax_conversation_audio_health_result_t;

typedef struct {
    nuwax_conversation_audio_health_config_t config;
    uint16_t aec_stable_frames;
    uint16_t aec_anomaly_frames;
    uint16_t reference_stable_frames;
    uint16_t reference_anomaly_frames;
    bool initialized;
    bool previous_playback_active;
} nuwax_conversation_audio_health_t;

void nuwax_conversation_audio_health_default_config(
    nuwax_conversation_audio_health_config_t *config);

bool nuwax_conversation_audio_health_init(
    nuwax_conversation_audio_health_t *health,
    const nuwax_conversation_audio_health_config_t *config);

void nuwax_conversation_audio_health_reset(
    nuwax_conversation_audio_health_t *health);

/* Caller-owned, allocation-free policy. Use from one task or serialize calls. */
bool nuwax_conversation_audio_health_process(
    nuwax_conversation_audio_health_t *health,
    const nuwax_conversation_audio_health_frame_t *frame,
    nuwax_conversation_audio_health_result_t *result);

#ifdef __cplusplus
}
#endif
