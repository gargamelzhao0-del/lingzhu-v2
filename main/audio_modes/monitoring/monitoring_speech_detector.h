#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "monitoring_speech_policy.h"
#ifndef CONFIG_NUWAX_VOICE_SAMPLE_RATE
#define CONFIG_NUWAX_VOICE_SAMPLE_RATE 16000
#endif
#ifndef CONFIG_NUWAX_VOICE_FRAME_DURATION_MS
#define CONFIG_NUWAX_VOICE_FRAME_DURATION_MS 20
#endif
#ifndef CONFIG_NUWAX_VOICE_MAX_SAMPLES_PER_FRAME
#define CONFIG_NUWAX_VOICE_MAX_SAMPLES_PER_FRAME 960
#endif
#ifndef CONFIG_NUWAX_VOICE_UPLINK_CODEC_OPUS
#define CONFIG_NUWAX_VOICE_UPLINK_CODEC_OPUS 0
#endif
#ifndef CONFIG_NUWAX_VOICE_CHANNELS
#define CONFIG_NUWAX_VOICE_CHANNELS 1
#endif
#ifndef CONFIG_NUWAX_MIC_READ_TIMEOUT_MS
#define CONFIG_NUWAX_MIC_READ_TIMEOUT_MS 100
#endif
#ifndef CONFIG_NUWAX_MONITORING_SILENCE_STOP_MS
#define CONFIG_NUWAX_MONITORING_SILENCE_STOP_MS 120000
#endif
#ifndef CONFIG_NUWAX_MONITORING_PREROLL_MS
#define CONFIG_NUWAX_MONITORING_PREROLL_MS 500
#endif
#ifndef CONFIG_NUWAX_MONITORING_SPEECH_CONFIRM_FRAMES
#define CONFIG_NUWAX_MONITORING_SPEECH_CONFIRM_FRAMES 3
#endif
#ifndef CONFIG_NUWAX_MONITORING_RESUME_CONFIRM_FRAMES
#define CONFIG_NUWAX_MONITORING_RESUME_CONFIRM_FRAMES 10
#endif
#ifndef CONFIG_NUWAX_MONITORING_RESUME_GUARD_MS
#define CONFIG_NUWAX_MONITORING_RESUME_GUARD_MS 300
#endif
#ifndef CONFIG_NUWAX_MONITORING_RESUME_QUIET_FRAMES
#define CONFIG_NUWAX_MONITORING_RESUME_QUIET_FRAMES 10
#endif
#ifndef CONFIG_NUWAX_MONITORING_SPEECH_REFRESH_FRAMES
#define CONFIG_NUWAX_MONITORING_SPEECH_REFRESH_FRAMES 10
#endif
#ifndef CONFIG_NUWAX_MONITORING_SPEECH_MIN_AVG_ABS
#define CONFIG_NUWAX_MONITORING_SPEECH_MIN_AVG_ABS 208
#endif
#ifndef CONFIG_NUWAX_MONITORING_SPEECH_MIN_PEAK
#define CONFIG_NUWAX_MONITORING_SPEECH_MIN_PEAK 780
#endif
#ifndef CONFIG_NUWAX_MONITORING_SPEECH_NOISE_MARGIN_ABS
#define CONFIG_NUWAX_MONITORING_SPEECH_NOISE_MARGIN_ABS 104
#endif
#ifndef CONFIG_NUWAX_MONITORING_CAPTURE_ACK_TIMEOUT_MS
#define CONFIG_NUWAX_MONITORING_CAPTURE_ACK_TIMEOUT_MS 1500
#endif
#ifndef CONFIG_NUWAX_MONITORING_CAPTURE_ACK_MAX_RETRIES
#define CONFIG_NUWAX_MONITORING_CAPTURE_ACK_MAX_RETRIES 3
#endif
#ifndef CONFIG_NUWAX_MONITORING_ESP_VAD_ENABLE
#define CONFIG_NUWAX_MONITORING_ESP_VAD_ENABLE 1
#endif

#if CONFIG_NUWAX_MONITORING_ESP_VAD_ENABLE && defined(ESP_PLATFORM)
#define NUWAX_MONITORING_USE_ESP_VAD 1
#include "esp_vad.h"
#else
#define NUWAX_MONITORING_USE_ESP_VAD 0
#endif
typedef struct {
    uint32_t noise_floor_abs;
    uint32_t consecutive_speech_frames;
    uint32_t raw_vad_speech_frames;
    uint32_t qualified_vad_speech_frames;
    uint32_t rejected_vad_speech_frames;
    bool noise_floor_initialized;
    bool last_energy_speech;
    bool last_vad_speech;
    bool last_vad_qualified;
#if NUWAX_MONITORING_USE_ESP_VAD
    vad_handle_t vad;
#endif
} monitoring_speech_detector_t;
static inline void monitoring_speech_detector_init(
    monitoring_speech_detector_t *detector) {
    if (!detector) {
        return;
    }
    memset(detector, 0, sizeof(*detector));
#if NUWAX_MONITORING_USE_ESP_VAD
    detector->vad = vad_create(VAD_MODE_0);
    if (!detector->vad) {
        ESP_LOGW("monitoring_vad",
                 "ESP VAD unavailable; monitoring uses adaptive energy fallback");
    }
#endif
}

static inline void monitoring_speech_detector_deinit(
    monitoring_speech_detector_t *detector) {
    if (!detector) {
        return;
    }
#if NUWAX_MONITORING_USE_ESP_VAD
    if (detector->vad) {
        vad_destroy(detector->vad);
        detector->vad = NULL;
    }
#endif
}

static inline void monitoring_speech_detector_reset_decision(
    monitoring_speech_detector_t *detector) {
    if (!detector) {
        return;
    }
    detector->consecutive_speech_frames = 0U;
    detector->last_energy_speech = false;
    detector->last_vad_speech = false;
    detector->last_vad_qualified = false;
#if NUWAX_MONITORING_USE_ESP_VAD
    if (detector->vad) {
        vad_reset_trigger(detector->vad);
    }
#endif
}

static inline bool monitoring_detect_speech(monitoring_speech_detector_t *detector,
                                     const int16_t *samples,
                                     size_t sample_count,
                                     uint32_t *avg_abs_out,
                                     uint32_t *peak_out) {
    if (!detector || !samples || sample_count == 0U) {
        return false;
    }

    uint64_t sum_abs = 0U;
    uint32_t peak = 0U;
    for (size_t i = 0U; i < sample_count; ++i) {
        const uint32_t value = (uint32_t)(samples[i] < 0 ? -(int32_t)samples[i] : samples[i]);
        sum_abs += value;
        if (value > peak) {
            peak = value;
        }
    }
    const uint32_t avg_abs = (uint32_t)(sum_abs / sample_count);

    if (!detector->noise_floor_initialized) {
        const uint32_t seed_limit = CONFIG_NUWAX_MONITORING_SPEECH_MIN_AVG_ABS / 2U;
        detector->noise_floor_abs = avg_abs < seed_limit ? avg_abs : seed_limit;
        detector->noise_floor_initialized = true;
    } else if (avg_abs < detector->noise_floor_abs) {
        detector->noise_floor_abs =
            (detector->noise_floor_abs * 15U + avg_abs) / 16U;
    } else {
        detector->noise_floor_abs =
            (detector->noise_floor_abs * 63U + avg_abs) / 64U;
    }

    const uint32_t adaptive_threshold =
        detector->noise_floor_abs + CONFIG_NUWAX_MONITORING_SPEECH_NOISE_MARGIN_ABS;
    const uint32_t speech_threshold =
        adaptive_threshold > CONFIG_NUWAX_MONITORING_SPEECH_MIN_AVG_ABS
            ? adaptive_threshold
            : CONFIG_NUWAX_MONITORING_SPEECH_MIN_AVG_ABS;
    const bool speech_energy =
        avg_abs >= speech_threshold &&
        peak >= CONFIG_NUWAX_MONITORING_SPEECH_MIN_PEAK;
    const bool strong_peak =
        peak >= CONFIG_NUWAX_MONITORING_SPEECH_MIN_PEAK * 2U &&
        avg_abs >= CONFIG_NUWAX_MONITORING_SPEECH_MIN_AVG_ABS / 2U;
    const bool energy_speech = speech_energy || strong_peak;
    bool vad_speech = false;
#if NUWAX_MONITORING_USE_ESP_VAD
    if (detector->vad) {
        vad_speech =
            vad_process(detector->vad,
                        (int16_t *)samples,
                        CONFIG_NUWAX_VOICE_SAMPLE_RATE,
                        CONFIG_NUWAX_VOICE_FRAME_DURATION_MS) == VAD_SPEECH;
    }
#endif

    /* ESP VAD mode 0 intentionally hears weak voices, but on this microphone
     * it can also report several consecutive speech frames for a quiet room.
     * Require only a weak, adaptive amount of real PCM energy before accepting
     * VAD. The gate stays well below the standalone energy detector so distant
     * speech remains eligible. */
    const bool vad_qualified =
        vad_speech && nuwax_monitoring_vad_energy_qualified(
                          detector->noise_floor_abs,
                          avg_abs,
                          peak,
                          CONFIG_NUWAX_MONITORING_SPEECH_MIN_AVG_ABS,
                          CONFIG_NUWAX_MONITORING_SPEECH_MIN_PEAK,
                          CONFIG_NUWAX_MONITORING_SPEECH_NOISE_MARGIN_ABS);
    const bool speech = energy_speech || vad_qualified;

    detector->last_energy_speech = energy_speech;
    detector->last_vad_speech = vad_speech;
    detector->last_vad_qualified = vad_qualified;
    if (vad_speech) {
        if (detector->raw_vad_speech_frames < UINT32_MAX) {
            detector->raw_vad_speech_frames++;
        }
        uint32_t *counter = vad_qualified
                                ? &detector->qualified_vad_speech_frames
                                : &detector->rejected_vad_speech_frames;
        if (*counter < UINT32_MAX) {
            (*counter)++;
        }
    }
    if (speech) {
        if (detector->consecutive_speech_frames < UINT32_MAX) {
            detector->consecutive_speech_frames++;
        }
    } else {
        detector->consecutive_speech_frames = 0U;
    }

    if (avg_abs_out) {
        *avg_abs_out = avg_abs;
    }
    if (peak_out) {
        *peak_out = peak;
    }
    return speech;
}

