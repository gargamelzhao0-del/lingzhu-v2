#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "conversation/mic_level.h"

typedef struct {
    uint32_t processed_frames;
    uint32_t zeroed_quiet_frames;
    uint32_t attenuated_quiet_frames;
    uint32_t playback_quiet_frames;
    uint32_t attenuated_impulse_frames;
    uint32_t playback_unprotected_attenuated_frames;
    uint32_t playback_unprotected_guarded_frames;
    uint32_t playback_unprotected_zero_frames;
    uint32_t playback_unprotected_min_gain_q15;
    uint32_t playback_protected_gain_frames;
    uint32_t playback_continuity_gain_frames;
    uint32_t playback_protected_limited_frames;
    uint32_t playback_gain_max_q15;
    uint32_t playback_echo_veto_attenuated_frames;
    uint32_t playback_echo_veto_guarded_frames;
    uint32_t softref_frames;
    uint32_t softref_ref_active_frames;
    uint32_t softref_ref_missing_frames;
    uint32_t softref_linear_cancel_frames;
    uint32_t softref_echo_gate_frames;
    uint32_t softref_nearend_guard_frames;
    uint32_t softref_underrun_samples;
    uint32_t softref_last_ref_avg_abs;
    uint32_t softref_last_ref_peak;
    uint32_t softref_last_corr_q15;
    uint32_t clipped_samples;
    uint32_t noise_floor_avg_abs;
    uint32_t speech_protected_frames;
} nuwax_audio_cleaner_stats_t;

typedef struct {
    bool enabled;
    bool softref_enabled;
    int16_t prev_input;
    int32_t prev_highpass;
    float notch_x1;
    float notch_x2;
    float notch_y1;
    float notch_y2;
    uint32_t noise_floor_avg_abs;
    uint32_t speech_hangover_frames;
    uint32_t playback_gain_q15;
    uint32_t playback_gain_hold_frames;
    uint32_t playback_echo_atten_q15;
    uint32_t playback_echo_veto_streak_frames;
    uint32_t playback_unprotected_gain_q15;
    int16_t *softref_buffer;
    size_t softref_capacity_samples;
    nuwax_audio_cleaner_stats_t stats;
} nuwax_audio_cleaner_t;

typedef struct {
    nuwax_mic_level_t before;
    nuwax_mic_level_t after;
    uint32_t softref_ref_avg_abs;
    uint32_t softref_ref_peak;
    uint32_t softref_corr_q15;
    uint32_t softref_copied_samples;
    uint32_t playback_gain_q15;
    uint32_t playback_unprotected_gain_q15;
    bool zeroed_quiet;
    bool attenuated_quiet;
    bool playback_quiet;
    bool attenuated_impulse;
    bool playback_unprotected_attenuated;
    bool playback_unprotected_guarded;
    bool playback_protected_gain;
    bool playback_continuity_gain;
    bool playback_protected_limited;
    bool playback_echo_veto_attenuated;
    bool playback_echo_veto_guarded;
    bool speech_protected;
    bool softref_active;
    bool softref_linear_cancel;
    bool softref_echo_gate;
    bool softref_nearend_guard;
} nuwax_audio_cleaner_result_t;

void nuwax_audio_cleaner_init(nuwax_audio_cleaner_t *cleaner);
void nuwax_audio_cleaner_deinit(nuwax_audio_cleaner_t *cleaner);
void nuwax_audio_cleaner_process(nuwax_audio_cleaner_t *cleaner,
                                 int16_t *samples,
                                 size_t sample_count,
                                 bool playback_active,
                                 bool speech_protect_hint,
                                 bool speech_gain_hint,
                                 bool playback_human_strong_hint,
                                 bool playback_echo_veto,
                                 bool playback_echo_atten_guard,
                                 nuwax_audio_cleaner_result_t *result);
void nuwax_audio_cleaner_process_with_ref(nuwax_audio_cleaner_t *cleaner,
                                          int16_t *samples,
                                          size_t sample_count,
                                          bool playback_active,
                                          bool speech_protect_hint,
                                          bool speech_gain_hint,
                                          bool playback_human_strong_hint,
                                          bool playback_echo_veto,
                                          bool playback_echo_atten_guard,
                                          const int16_t *ref_samples,
                                          size_t ref_sample_count,
                                          nuwax_audio_cleaner_result_t *result);
void nuwax_audio_cleaner_get_stats(const nuwax_audio_cleaner_t *cleaner,
                                   nuwax_audio_cleaner_stats_t *stats);
