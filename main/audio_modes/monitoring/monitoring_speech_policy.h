#pragma once

#include <stdbool.h>
#include <stdint.h>

static inline bool nuwax_monitoring_vad_energy_qualified(
    uint32_t noise_floor_abs,
    uint32_t avg_abs,
    uint32_t peak,
    uint32_t min_avg_abs,
    uint32_t min_peak,
    uint32_t noise_margin_abs) {
    uint32_t weak_noise_margin = noise_margin_abs / 2U;
    if (weak_noise_margin < 10U) {
        weak_noise_margin = 10U;
    }
    uint32_t weak_avg_floor = min_avg_abs / 2U;
    if (weak_avg_floor < 20U) {
        weak_avg_floor = 20U;
    }
    const uint32_t adaptive_weak_avg = noise_floor_abs + weak_noise_margin;
    if (adaptive_weak_avg > weak_avg_floor) {
        weak_avg_floor = adaptive_weak_avg;
    }
    uint32_t weak_peak_floor = min_peak / 2U;
    if (weak_peak_floor < 100U) {
        weak_peak_floor = 100U;
    }
    return avg_abs >= weak_avg_floor && peak >= weak_peak_floor;
}

static inline bool nuwax_monitoring_stream_activity_confirmed(
    uint32_t consecutive_speech_frames,
    uint32_t resume_confirm_frames,
    uint32_t refresh_confirm_frames) {
    if (refresh_confirm_frames < resume_confirm_frames) {
        refresh_confirm_frames = resume_confirm_frames;
    }
    return consecutive_speech_frames >= refresh_confirm_frames;
}
