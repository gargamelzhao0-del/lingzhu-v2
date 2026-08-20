#pragma once

#include <stdbool.h>
#include <stdint.h>

#define NUWAX_WAKE_WEAK_THRESHOLD_Q1000 80U
#define NUWAX_WAKE_STRONG_THRESHOLD_Q1000 100U
#define NUWAX_WAKE_FAST_MIN_VOICE_SPAN_MS 160U
#define NUWAX_WAKE_FAST_MIN_SNR_RATIO_Q8 640U
#define NUWAX_WAKE_WEAK_MIN_VOICE_SPAN_MS 250U
#define NUWAX_WAKE_WEAK_MAX_VOICE_SPAN_MS 3500U
#define NUWAX_WAKE_WEAK_MIN_SNR_RATIO_Q8 512U
#define NUWAX_WAKE_WEAK_MAX_CLIP_PPM 10000U

#define NUWAX_WAKE_VOICE_NOISE_MULTIPLIER 2U
#define NUWAX_WAKE_VOICE_NOISE_MARGIN 60U
#define NUWAX_WAKE_VOICE_MIN_PEAK 400U
#define NUWAX_WAKE_VOICE_TAIL_HOLD_MS 900U

static inline uint32_t nuwax_wake_voice_threshold(uint32_t noise_avg_abs) {
    const uint32_t scaled =
        noise_avg_abs * NUWAX_WAKE_VOICE_NOISE_MULTIPLIER;
    const uint32_t margin = noise_avg_abs + NUWAX_WAKE_VOICE_NOISE_MARGIN;
    return scaled > margin ? scaled : margin;
}

static inline bool nuwax_wake_voice_frame_is_voice_like(
    uint32_t frame_avg_abs,
    uint32_t frame_peak,
    uint32_t noise_avg_abs) {
    return frame_avg_abs >= nuwax_wake_voice_threshold(noise_avg_abs) &&
           frame_peak >= NUWAX_WAKE_VOICE_MIN_PEAK;
}

static inline bool nuwax_wake_voice_tail_expired(
    uint32_t now_ms,
    uint32_t last_voice_ms) {
    return last_voice_ms != 0U &&
           (uint32_t)(now_ms - last_voice_ms) >
               NUWAX_WAKE_VOICE_TAIL_HOLD_MS;
}

static inline bool nuwax_wake_detection_accept(
    uint32_t probability_q1000,
    uint32_t noise_avg_abs,
    uint32_t signal_avg_abs,
    uint32_t voice_span_ms,
    uint32_t clipped_samples,
    uint32_t total_samples) {
    if (probability_q1000 >= NUWAX_WAKE_STRONG_THRESHOLD_Q1000) {
        return true;
    }
    if (probability_q1000 < NUWAX_WAKE_WEAK_THRESHOLD_Q1000 ||
        noise_avg_abs == 0U || total_samples == 0U ||
        voice_span_ms < NUWAX_WAKE_FAST_MIN_VOICE_SPAN_MS ||
        voice_span_ms > NUWAX_WAKE_WEAK_MAX_VOICE_SPAN_MS) {
        return false;
    }
    const uint64_t snr_ratio_q8 =
        ((uint64_t)signal_avg_abs << 8U) / noise_avg_abs;
    const uint64_t clip_ppm =
        ((uint64_t)clipped_samples * 1000000ULL) / total_samples;
    if (clip_ppm > NUWAX_WAKE_WEAK_MAX_CLIP_PPM) {
        return false;
    }
    if (voice_span_ms < NUWAX_WAKE_WEAK_MIN_VOICE_SPAN_MS) {
        return snr_ratio_q8 >= NUWAX_WAKE_FAST_MIN_SNR_RATIO_Q8;
    }
    return snr_ratio_q8 >= NUWAX_WAKE_WEAK_MIN_SNR_RATIO_Q8;
}
