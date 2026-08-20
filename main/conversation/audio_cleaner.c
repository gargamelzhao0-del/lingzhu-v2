#include "conversation/audio_cleaner.h"

#include <math.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "p3_software_ref.h"
#include "sdkconfig.h"

#ifndef CONFIG_NUWAX_AUDIO_CLEANER_ENABLE
#define CONFIG_NUWAX_AUDIO_CLEANER_ENABLE 1
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_HIGHPASS_ENABLE
#define CONFIG_NUWAX_AUDIO_CLEANER_HIGHPASS_ENABLE 1
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_HIGHPASS_COEFF_Q15
#define CONFIG_NUWAX_AUDIO_CLEANER_HIGHPASS_COEFF_Q15 32604
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_NOTCH_100HZ_ENABLE
#define CONFIG_NUWAX_AUDIO_CLEANER_NOTCH_100HZ_ENABLE 1
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_QUIET_AVG_ABS
#define CONFIG_NUWAX_AUDIO_CLEANER_QUIET_AVG_ABS 90
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_QUIET_PEAK
#define CONFIG_NUWAX_AUDIO_CLEANER_QUIET_PEAK 420
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_QUIET_AVG_ABS
#define CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_QUIET_AVG_ABS 220
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_QUIET_PEAK
#define CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_QUIET_PEAK 900
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_QUIET_ZERO_ENABLE
#define CONFIG_NUWAX_AUDIO_CLEANER_QUIET_ZERO_ENABLE 0
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_QUIET_ATTENUATE_Q15
#define CONFIG_NUWAX_AUDIO_CLEANER_QUIET_ATTENUATE_Q15 32768
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_QUIET_ATTENUATE_Q15
#define CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_QUIET_ATTENUATE_Q15 32768
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_SPEECH_HANGOVER_FRAMES
#define CONFIG_NUWAX_AUDIO_CLEANER_SPEECH_HANGOVER_FRAMES 12
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_SPEECH_GUARD_AVG_ABS
#define CONFIG_NUWAX_AUDIO_CLEANER_SPEECH_GUARD_AVG_ABS 300
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_SPEECH_GUARD_PEAK
#define CONFIG_NUWAX_AUDIO_CLEANER_SPEECH_GUARD_PEAK 1200
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_SPEECH_GUARD_AVG_ABS
#define CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_SPEECH_GUARD_AVG_ABS 700
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_SPEECH_GUARD_PEAK
#define CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_SPEECH_GUARD_PEAK 2500
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_SPEECH_HANGOVER_FRAMES
#define CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_SPEECH_HANGOVER_FRAMES 4
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_LEVEL_SPEECH_PROTECT_ENABLE
#define CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_LEVEL_SPEECH_PROTECT_ENABLE 0
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_UNPROTECTED_GATE_MAX_AVG_ABS
#define CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_UNPROTECTED_GATE_MAX_AVG_ABS 12000
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_UNPROTECTED_GATE_MAX_PEAK
#define CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_UNPROTECTED_GATE_MAX_PEAK 30000
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_UNPROTECTED_ATTENUATE_Q15
#define CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_UNPROTECTED_ATTENUATE_Q15 32768
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_UNPROTECTED_ATTACK_STEP_Q15
#define CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_UNPROTECTED_ATTACK_STEP_Q15 16384
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_UNPROTECTED_RELEASE_STEP_Q15
#define CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_UNPROTECTED_RELEASE_STEP_Q15 32768
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_PROTECTED_GAIN_ENABLE
#define CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_PROTECTED_GAIN_ENABLE 0
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_PROTECTED_GAIN_MIN_AVG_ABS
#define CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_PROTECTED_GAIN_MIN_AVG_ABS 90
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_PROTECTED_GAIN_MIN_PEAK
#define CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_PROTECTED_GAIN_MIN_PEAK 300
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_PROTECTED_GAIN_MIN_BEFORE_AVG_ABS
#define CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_PROTECTED_GAIN_MIN_BEFORE_AVG_ABS 80
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_PROTECTED_GAIN_MIN_BEFORE_PEAK
#define CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_PROTECTED_GAIN_MIN_BEFORE_PEAK 250
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_PROTECTED_GAIN_TARGET_AVG_ABS
#define CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_PROTECTED_GAIN_TARGET_AVG_ABS 520
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_PROTECTED_GAIN_MAX_Q15
#define CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_PROTECTED_GAIN_MAX_Q15 57344
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_PROTECTED_GAIN_MAX_PEAK
#define CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_PROTECTED_GAIN_MAX_PEAK 12000
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_PROTECTED_GAIN_MAX_CORR_Q15
#define CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_PROTECTED_GAIN_MAX_CORR_Q15 14000
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_CONTINUITY_GAIN_MIN_AVG_ABS
#define CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_CONTINUITY_GAIN_MIN_AVG_ABS 35
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_CONTINUITY_GAIN_MIN_PEAK
#define CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_CONTINUITY_GAIN_MIN_PEAK 80
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_CONTINUITY_GAIN_MIN_BEFORE_AVG_ABS
#define CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_CONTINUITY_GAIN_MIN_BEFORE_AVG_ABS 8
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_CONTINUITY_GAIN_MIN_BEFORE_PEAK
#define CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_CONTINUITY_GAIN_MIN_BEFORE_PEAK 24
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_CONTINUITY_GAIN_TARGET_AVG_ABS
#define CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_CONTINUITY_GAIN_TARGET_AVG_ABS 1000
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_CONTINUITY_GAIN_MAX_Q15
#define CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_CONTINUITY_GAIN_MAX_Q15 57344
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_CONTINUITY_GAIN_MAX_CORR_Q15
#define CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_CONTINUITY_GAIN_MAX_CORR_Q15 12000
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_PROTECTED_GAIN_ATTACK_Q15
#define CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_PROTECTED_GAIN_ATTACK_Q15 8192
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_PROTECTED_GAIN_RELEASE_Q15
#define CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_PROTECTED_GAIN_RELEASE_Q15 2048
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_PROTECTED_GAIN_HOLD_FRAMES
#define CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_PROTECTED_GAIN_HOLD_FRAMES 16
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_PROTECTED_LIMIT_ENABLE
#define CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_PROTECTED_LIMIT_ENABLE 1
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_PROTECTED_LIMIT_PEAK
#define CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_PROTECTED_LIMIT_PEAK 9000
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_ECHO_VETO_ATTENUATE_Q15
#define CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_ECHO_VETO_ATTENUATE_Q15 32768
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_ECHO_VETO_ATTACK_Q15
#define CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_ECHO_VETO_ATTACK_Q15 24576
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_ECHO_VETO_RELEASE_Q15
#define CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_ECHO_VETO_RELEASE_Q15 4096
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_ECHO_VETO_MIN_STREAK_FRAMES
#define CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_ECHO_VETO_MIN_STREAK_FRAMES 3
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_IMPULSE_PEAK_MIN
#define CONFIG_NUWAX_AUDIO_CLEANER_IMPULSE_PEAK_MIN 10000
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_IMPULSE_AVG_MAX
#define CONFIG_NUWAX_AUDIO_CLEANER_IMPULSE_AVG_MAX 260
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_IMPULSE_PEAK_MIN
#define CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_IMPULSE_PEAK_MIN 5200
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_IMPULSE_AVG_MAX
#define CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_IMPULSE_AVG_MAX 360
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_IMPULSE_ATTENUATE_Q15
#define CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_IMPULSE_ATTENUATE_Q15 32768
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_ENABLE
#define CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_ENABLE 0
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_ENABLE
#define CONFIG_NUWAX_LIVE_AEC_ENABLE 0
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_DELAY_SAMPLES
#define CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_DELAY_SAMPLES 240
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_MIN_REF_AVG_ABS
#define CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_MIN_REF_AVG_ABS 120
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_MIN_REF_PEAK
#define CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_MIN_REF_PEAK 350
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_MIN_CORR_Q15
#define CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_MIN_CORR_Q15 4915
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_GUARD_BYPASS_CORR_Q15
#define CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_GUARD_BYPASS_CORR_Q15 16056
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_MAX_LINEAR_GAIN_Q15
#define CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_MAX_LINEAR_GAIN_Q15 24576
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_NEAREND_LINEAR_MIN_CORR_Q15
#define CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_NEAREND_LINEAR_MIN_CORR_Q15 8192
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_NEAREND_MAX_LINEAR_GAIN_Q15
#define CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_NEAREND_MAX_LINEAR_GAIN_Q15 8192
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_ECHO_GATE_MAX_MIC_AVG_ABS
#define CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_ECHO_GATE_MAX_MIC_AVG_ABS 900
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_ECHO_GATE_MAX_MIC_PEAK
#define CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_ECHO_GATE_MAX_MIC_PEAK 5000
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_HIGH_CORR_GATE_MAX_MIC_AVG_ABS
#define CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_HIGH_CORR_GATE_MAX_MIC_AVG_ABS 7000
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_HIGH_CORR_GATE_MAX_MIC_PEAK
#define CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_HIGH_CORR_GATE_MAX_MIC_PEAK 16000
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_NEAREND_GUARD_AVG_ABS
#define CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_NEAREND_GUARD_AVG_ABS 300
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_NEAREND_GUARD_PEAK
#define CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_NEAREND_GUARD_PEAK 1200
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_ATTENUATE_Q15
#define CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_ATTENUATE_Q15 16384
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_HIGH_CORR_ATTENUATE_Q15
#define CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_HIGH_CORR_ATTENUATE_Q15 4096
#endif
#ifndef CONFIG_NUWAX_VOICE_MAX_SAMPLES_PER_FRAME
#define CONFIG_NUWAX_VOICE_MAX_SAMPLES_PER_FRAME 960
#endif

#if CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_ENABLE && CONFIG_NUWAX_P3_SOFTWARE_REF_ENABLE
static const char *TAG = "nuwax_audio_cleaner";
typedef struct {
    uint32_t corr_q15;
    int32_t gain_q15;
} softref_echo_metrics_t;
#endif

static int16_t clamp_i16(int32_t sample) {
    if (sample > INT16_MAX) {
        return INT16_MAX;
    }
    if (sample < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)sample;
}

static uint32_t max_u32(uint32_t a, uint32_t b) {
    return a > b ? a : b;
}

static uint32_t min_u32(uint32_t a, uint32_t b) {
    return a < b ? a : b;
}

static void reset_playback_gain_state(nuwax_audio_cleaner_t *cleaner) {
    if (!cleaner) {
        return;
    }
    cleaner->playback_gain_q15 = 32768U;
    cleaner->playback_gain_hold_frames = 0;
}

static void reset_playback_echo_atten_state(nuwax_audio_cleaner_t *cleaner) {
    if (!cleaner) {
        return;
    }
    cleaner->playback_echo_atten_q15 = 32768U;
    cleaner->playback_echo_veto_streak_frames = 0;
}

static void reset_playback_unprotected_gate_state(nuwax_audio_cleaner_t *cleaner) {
    if (!cleaner) {
        return;
    }
    cleaner->playback_unprotected_gain_q15 = 32768U;
}

static uint32_t smooth_playback_unprotected_gain_q15(nuwax_audio_cleaner_t *cleaner,
                                                      uint32_t desired_q15) {
    if (!cleaner) {
        return desired_q15;
    }
    if (desired_q15 > 32768U) {
        desired_q15 = 32768U;
    }
    uint32_t current = cleaner->playback_unprotected_gain_q15;
    if (current > 32768U) {
        current = 32768U;
    }
    if (desired_q15 < current) {
        uint32_t step = (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_UNPROTECTED_ATTACK_STEP_Q15;
        if (step == 0 || step >= current - desired_q15) {
            current = desired_q15;
        } else {
            current -= step;
        }
    } else if (desired_q15 > current) {
        uint32_t step = (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_UNPROTECTED_RELEASE_STEP_Q15;
        if (step == 0 || step >= desired_q15 - current) {
            current = desired_q15;
        } else {
            current += step;
        }
    }
    cleaner->playback_unprotected_gain_q15 = current;
    return current;
}

static uint32_t smooth_playback_gain_q15(nuwax_audio_cleaner_t *cleaner, uint32_t desired_q15) {
    if (!cleaner) {
        return desired_q15;
    }
    if (cleaner->playback_gain_q15 == 0) {
        cleaner->playback_gain_q15 = 32768U;
    }
    uint32_t current = cleaner->playback_gain_q15;
    if (desired_q15 > current) {
        const uint32_t attack = (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_PROTECTED_GAIN_ATTACK_Q15;
        uint32_t step = (uint32_t)(((uint64_t)(desired_q15 - current) * attack) >> 15);
        if (step == 0) {
            step = 1;
        }
        current += step;
        if (current > desired_q15) {
            current = desired_q15;
        }
        cleaner->playback_gain_hold_frames =
            (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_PROTECTED_GAIN_HOLD_FRAMES;
    } else if (desired_q15 < current) {
        if (cleaner->playback_gain_hold_frames > 0) {
            cleaner->playback_gain_hold_frames--;
        } else {
            const uint32_t release = (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_PROTECTED_GAIN_RELEASE_Q15;
            uint32_t step = (uint32_t)(((uint64_t)(current - desired_q15) * release) >> 15);
            if (step == 0) {
                step = 1;
            }
            current -= step;
            if (current < desired_q15) {
                current = desired_q15;
            }
        }
    }
    if (current < 32768U) {
        current = 32768U;
    }
    cleaner->playback_gain_q15 = current;
    if (current > cleaner->stats.playback_gain_max_q15) {
        cleaner->stats.playback_gain_max_q15 = current;
    }
    return current;
}

static uint32_t smooth_playback_echo_atten_q15(nuwax_audio_cleaner_t *cleaner, uint32_t desired_q15) {
    if (!cleaner) {
        return desired_q15;
    }
    if (cleaner->playback_echo_atten_q15 == 0) {
        cleaner->playback_echo_atten_q15 = 32768U;
    }
    if (desired_q15 > 32768U) {
        desired_q15 = 32768U;
    }
    uint32_t current = cleaner->playback_echo_atten_q15;
    if (desired_q15 < current) {
        const uint32_t attack = (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_ECHO_VETO_ATTACK_Q15;
        uint32_t step = (uint32_t)(((uint64_t)(current - desired_q15) * attack) >> 15);
        if (step == 0) {
            step = 1;
        }
        current -= step;
        if (current < desired_q15) {
            current = desired_q15;
        }
    } else if (desired_q15 > current) {
        const uint32_t release = (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_ECHO_VETO_RELEASE_Q15;
        uint32_t step = (uint32_t)(((uint64_t)(desired_q15 - current) * release) >> 15);
        if (step == 0) {
            step = 1;
        }
        current += step;
        if (current > desired_q15) {
            current = desired_q15;
        }
    }
    cleaner->playback_echo_atten_q15 = current;
    return current;
}

static bool speech_protect_level(nuwax_mic_level_t level) {
    return nuwax_mic_level_is_speech(level) ||
           (level.avg_abs >= (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_SPEECH_GUARD_AVG_ABS &&
            level.peak >= (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_SPEECH_GUARD_PEAK);
}

static bool playback_speech_protect_level(nuwax_mic_level_t level) {
#if CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_LEVEL_SPEECH_PROTECT_ENABLE
    return level.avg_abs >= (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_SPEECH_GUARD_AVG_ABS &&
           level.peak >= (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_SPEECH_GUARD_PEAK;
#else
    (void)level;
    return false;
#endif
}

static void update_noise_floor(nuwax_audio_cleaner_t *cleaner, uint32_t avg_abs) {
    if (!cleaner) {
        return;
    }
    if (avg_abs == 0) {
        return;
    }
    if (cleaner->noise_floor_avg_abs == 0) {
        cleaner->noise_floor_avg_abs = avg_abs;
    } else {
        cleaner->noise_floor_avg_abs =
            (cleaner->noise_floor_avg_abs * 15u + avg_abs) / 16u;
    }
    cleaner->stats.noise_floor_avg_abs = cleaner->noise_floor_avg_abs;
}

#if CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_ENABLE && CONFIG_NUWAX_P3_SOFTWARE_REF_ENABLE
static bool softref_enabled_for_cleaner(const nuwax_audio_cleaner_t *cleaner) {
#if CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_ENABLE && CONFIG_NUWAX_P3_SOFTWARE_REF_ENABLE
    return cleaner && cleaner->enabled && cleaner->softref_enabled &&
           cleaner->softref_buffer && cleaner->softref_capacity_samples > 0;
#else
    (void)cleaner;
    return false;
#endif
}

static softref_echo_metrics_t compute_softref_metrics(const int16_t *samples,
                                                      const int16_t *ref,
                                                      size_t sample_count) {
    uint64_t mic_sumsq = 0;
    uint64_t ref_sumsq = 0;
    int64_t dot = 0;
    for (size_t i = 0; i < sample_count; i++) {
        const int32_t m = samples[i];
        const int32_t r = ref[i];
        mic_sumsq += (uint64_t)((int64_t)m * (int64_t)m);
        ref_sumsq += (uint64_t)((int64_t)r * (int64_t)r);
        dot += (int64_t)m * (int64_t)r;
    }

    softref_echo_metrics_t metrics = {0};
    if (mic_sumsq == 0 || ref_sumsq == 0) {
        return metrics;
    }

    const double denom = sqrt((double)mic_sumsq * (double)ref_sumsq);
    if (denom > 0.0) {
        double corr = fabs((double)dot) / denom;
        if (corr > 1.0) {
            corr = 1.0;
        }
        metrics.corr_q15 = (uint32_t)(corr * 32768.0 + 0.5);
        if (metrics.corr_q15 > 32767U) {
            metrics.corr_q15 = 32767U;
        }
    }

    double gain = (double)dot / (double)ref_sumsq;
    const double max_gain = (double)CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_MAX_LINEAR_GAIN_Q15 / 32768.0;
    if (gain > max_gain) {
        gain = max_gain;
    } else if (gain < -max_gain) {
        gain = -max_gain;
    }
    metrics.gain_q15 = (int32_t)(gain * 32768.0);
    return metrics;
}

static int32_t clamp_abs_i32(int32_t value, int32_t max_abs) {
    if (max_abs < 0) {
        max_abs = -max_abs;
    }
    if (value > max_abs) {
        return max_abs;
    }
    if (value < -max_abs) {
        return -max_abs;
    }
    return value;
}

static void apply_softref_linear_cancel(nuwax_audio_cleaner_t *cleaner,
                                        int16_t *samples,
                                        const int16_t *ref,
                                        size_t sample_count,
                                        int32_t gain_q15) {
    for (size_t i = 0; i < sample_count; i++) {
        const int32_t cancel = (int32_t)(((int64_t)gain_q15 * (int64_t)ref[i]) >> 15);
        const int32_t out = (int32_t)samples[i] - cancel;
        const int16_t clamped = clamp_i16(out);
        if ((int32_t)clamped != out) {
            cleaner->stats.clipped_samples++;
        }
        samples[i] = clamped;
    }
}
#endif

static void apply_softref_attenuation(int16_t *samples, size_t sample_count, int32_t atten_q15) {
    for (size_t i = 0; i < sample_count; i++) {
        samples[i] = (int16_t)(((int32_t)samples[i] * atten_q15) >> 15);
    }
}

static bool apply_limited_gain(nuwax_audio_cleaner_t *cleaner,
                               int16_t *samples,
                               size_t sample_count,
                               int32_t gain_q15,
                               uint32_t peak_limit) {
    if (!samples || sample_count == 0 || gain_q15 <= 0) {
        return false;
    }
    if (gain_q15 == 32768) {
        return false;
    }
    bool changed = false;
    for (size_t i = 0; i < sample_count; i++) {
        int32_t out = (int32_t)(((int64_t)samples[i] * (int64_t)gain_q15) >> 15);
        if (peak_limit > 0 && out > (int32_t)peak_limit) {
            out = (int32_t)peak_limit;
        } else if (peak_limit > 0 && out < -(int32_t)peak_limit) {
            out = -(int32_t)peak_limit;
        }
        const int16_t clamped = clamp_i16(out);
        if ((int32_t)clamped != out && cleaner) {
            cleaner->stats.clipped_samples++;
        }
        if (clamped != samples[i]) {
            changed = true;
        }
        samples[i] = clamped;
    }
    return changed;
}

static bool apply_playback_gain_release(nuwax_audio_cleaner_t *cleaner,
                                        int16_t *samples,
                                        size_t sample_count,
                                        nuwax_mic_level_t *level) {
    if (!cleaner || !samples || !level || sample_count == 0) {
        return false;
    }
    const uint32_t gain_q15 = smooth_playback_gain_q15(cleaner, 32768U);
    if (gain_q15 <= 32768U) {
        return false;
    }
    const bool changed = apply_limited_gain(
        cleaner,
        samples,
        sample_count,
        (int32_t)gain_q15,
        (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_PROTECTED_GAIN_MAX_PEAK);
    if (changed) {
        *level = nuwax_mic_level_measure(samples, sample_count);
    }
    return changed;
}

static bool apply_playback_protected_gain(nuwax_audio_cleaner_t *cleaner,
                                          int16_t *samples,
                                          size_t sample_count,
                                          nuwax_mic_level_t *level,
                                          bool playback_active,
                                          bool speech_protected,
                                          bool speech_gain_hint,
                                          bool playback_echo_veto,
                                          nuwax_mic_level_t before_level,
                                          nuwax_audio_cleaner_result_t *result) {
#if CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_PROTECTED_GAIN_ENABLE
    if (!playback_active) {
        reset_playback_gain_state(cleaner);
        return false;
    }
    if (!samples || !level || sample_count == 0) {
        return false;
    }
    if (!speech_protected) {
        return apply_playback_gain_release(cleaner, samples, sample_count, level);
    }
    if (playback_echo_veto && !speech_gain_hint) {
        return apply_playback_gain_release(cleaner, samples, sample_count, level);
    }
    const bool continuity_gain = speech_gain_hint;
    const uint32_t min_before_avg =
        continuity_gain ? (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_CONTINUITY_GAIN_MIN_BEFORE_AVG_ABS
                        : (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_PROTECTED_GAIN_MIN_BEFORE_AVG_ABS;
    const uint32_t min_before_peak =
        continuity_gain ? (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_CONTINUITY_GAIN_MIN_BEFORE_PEAK
                        : (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_PROTECTED_GAIN_MIN_BEFORE_PEAK;
    const uint32_t min_avg =
        continuity_gain ? (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_CONTINUITY_GAIN_MIN_AVG_ABS
                        : (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_PROTECTED_GAIN_MIN_AVG_ABS;
    const uint32_t min_peak =
        continuity_gain ? (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_CONTINUITY_GAIN_MIN_PEAK
                        : (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_PROTECTED_GAIN_MIN_PEAK;
    const uint32_t max_corr =
        continuity_gain ? (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_CONTINUITY_GAIN_MAX_CORR_Q15
                        : (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_PROTECTED_GAIN_MAX_CORR_Q15;
    if (before_level.avg_abs < min_before_avg || before_level.peak < min_before_peak) {
        return apply_playback_gain_release(cleaner, samples, sample_count, level);
    }
    if (level->avg_abs < min_avg ||
        level->peak < min_peak ||
        level->peak > (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_PROTECTED_GAIN_MAX_PEAK) {
        return apply_playback_gain_release(cleaner, samples, sample_count, level);
    }
    if (result &&
        result->softref_active &&
        result->softref_corr_q15 >= max_corr) {
        return apply_playback_gain_release(cleaner, samples, sample_count, level);
    }

    const uint32_t target =
        continuity_gain ? (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_CONTINUITY_GAIN_TARGET_AVG_ABS
                        : (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_PROTECTED_GAIN_TARGET_AVG_ABS;
    if (target == 0 || level->avg_abs >= target) {
        return apply_playback_gain_release(cleaner, samples, sample_count, level);
    }
    uint32_t gain_q15 = (uint32_t)(((uint64_t)target * 32768ULL) / (uint64_t)level->avg_abs);
    const uint32_t max_gain_q15 =
        continuity_gain ? (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_CONTINUITY_GAIN_MAX_Q15
                        : (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_PROTECTED_GAIN_MAX_Q15;
    if (gain_q15 > max_gain_q15) {
        gain_q15 = max_gain_q15;
    }
    gain_q15 = smooth_playback_gain_q15(cleaner, gain_q15);
    if (gain_q15 <= 32768U) {
        return false;
    }
    if (!apply_limited_gain(cleaner,
                            samples,
                            sample_count,
                            (int32_t)gain_q15,
                            (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_PROTECTED_GAIN_MAX_PEAK)) {
        return false;
    }
    *level = nuwax_mic_level_measure(samples, sample_count);
    if (continuity_gain && result) {
        result->playback_continuity_gain = true;
    }
    return true;
#else
    (void)cleaner;
    (void)samples;
    (void)sample_count;
    (void)level;
    (void)playback_active;
    (void)speech_protected;
    (void)speech_gain_hint;
    (void)playback_echo_veto;
    (void)before_level;
    (void)result;
    return false;
#endif
}

static bool apply_playback_protected_limiter(int16_t *samples,
                                             size_t sample_count,
                                             nuwax_mic_level_t *level,
                                             bool playback_active,
                                             bool speech_protected) {
#if CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_PROTECTED_LIMIT_ENABLE
    (void)speech_protected;
    if (!playback_active || !samples || !level || sample_count == 0) {
        return false;
    }
    const uint32_t peak_limit = (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_PROTECTED_LIMIT_PEAK;
    if (peak_limit == 0 || level->peak <= peak_limit) {
        return false;
    }
    bool changed = false;
    for (size_t i = 0; i < sample_count; i++) {
        int32_t sample = samples[i];
        if (sample > (int32_t)peak_limit) {
            sample = (int32_t)peak_limit;
            changed = true;
        } else if (sample < -(int32_t)peak_limit) {
            sample = -(int32_t)peak_limit;
            changed = true;
        }
        samples[i] = (int16_t)sample;
    }
    if (changed) {
        *level = nuwax_mic_level_measure(samples, sample_count);
    }
    return changed;
#else
    (void)samples;
    (void)sample_count;
    (void)level;
    (void)playback_active;
    (void)speech_protected;
    return false;
#endif
}

static bool apply_playback_echo_veto_attenuation(nuwax_audio_cleaner_t *cleaner,
                                                 int16_t *samples,
                                                 size_t sample_count,
                                                 nuwax_mic_level_t *level,
                                                 bool playback_active,
                                                 bool speech_protect_hint,
                                                 bool speech_gain_hint,
                                                 bool speech_protected,
                                                 bool playback_echo_veto,
                                                 bool playback_echo_atten_guard,
                                                 bool *guarded) {
    if (guarded) {
        *guarded = false;
    }
    if (!playback_active) {
        reset_playback_echo_atten_state(cleaner);
        return false;
    }
    if (!playback_echo_veto || !samples || !level || sample_count == 0 ||
        (level->avg_abs == 0 && level->peak == 0)) {
        reset_playback_echo_atten_state(cleaner);
        return false;
    }
    if (speech_protect_hint || speech_gain_hint || speech_protected || playback_echo_atten_guard) {
        reset_playback_echo_atten_state(cleaner);
        if (guarded) {
            *guarded = true;
        }
        return false;
    }
    if (cleaner) {
        cleaner->playback_echo_veto_streak_frames++;
        if (cleaner->playback_echo_veto_streak_frames <
            (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_ECHO_VETO_MIN_STREAK_FRAMES) {
            if (guarded) {
                *guarded = true;
            }
            return false;
        }
    }
    uint32_t desired_q15 = (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_ECHO_VETO_ATTENUATE_Q15;
    if (desired_q15 >= 32768U) {
        reset_playback_echo_atten_state(cleaner);
        return false;
    }
    const uint32_t gain_q15 = smooth_playback_echo_atten_q15(cleaner, desired_q15);
    if (gain_q15 >= 32768U) {
        return false;
    }
    apply_softref_attenuation(samples, sample_count, (int32_t)gain_q15);
    *level = nuwax_mic_level_measure(samples, sample_count);
    return true;
}

static bool apply_playback_unprotected_attenuation(nuwax_audio_cleaner_t *cleaner,
                                                    int16_t *samples,
                                                    size_t sample_count,
                                                    nuwax_mic_level_t *level,
                                                    bool playback_active,
                                                    bool speech_protect_hint,
                                                    bool speech_gain_hint,
                                                    bool playback_human_strong_hint,
                                                    bool speech_protected,
                                                    nuwax_audio_cleaner_result_t *result) {
    if (!cleaner || !samples || !level || sample_count == 0) {
        return false;
    }
    if (!playback_active) {
        reset_playback_unprotected_gate_state(cleaner);
        if (result) {
            result->playback_unprotected_gain_q15 = 32768U;
        }
        return false;
    }

    const bool high_level_guard =
        level->avg_abs > (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_UNPROTECTED_GATE_MAX_AVG_ABS ||
        level->peak > (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_UNPROTECTED_GATE_MAX_PEAK;
    const bool human_guard =
        playback_human_strong_hint || speech_protect_hint || speech_gain_hint ||
        speech_protected || high_level_guard;
    const uint32_t desired_q15 =
        human_guard ? 32768U
                    : (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_UNPROTECTED_ATTENUATE_Q15;
    const uint32_t gain_q15 = smooth_playback_unprotected_gain_q15(cleaner, desired_q15);
    if (result) {
        result->playback_unprotected_gain_q15 = gain_q15;
        result->playback_unprotected_guarded = human_guard;
    }
    if (human_guard) {
        cleaner->stats.playback_unprotected_guarded_frames++;
    }
    if (gain_q15 < cleaner->stats.playback_unprotected_min_gain_q15) {
        cleaner->stats.playback_unprotected_min_gain_q15 = gain_q15;
    }
    if (gain_q15 >= 32768U) {
        return false;
    }

    apply_softref_attenuation(samples, sample_count, (int32_t)gain_q15);
    *level = nuwax_mic_level_measure(samples, sample_count);
    if (gain_q15 == 0) {
        cleaner->stats.playback_unprotected_zero_frames++;
    }
    return true;
}

static nuwax_mic_level_t process_softref_echo(nuwax_audio_cleaner_t *cleaner,
                                              int16_t *samples,
                                              size_t sample_count,
                                              bool playback_active,
                                              bool nearend_hint,
                                              bool speech_protected,
                                              const int16_t *external_ref,
                                              size_t external_ref_count,
                                              nuwax_mic_level_t level,
                                              nuwax_audio_cleaner_result_t *result) {
#if CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_ENABLE && CONFIG_NUWAX_P3_SOFTWARE_REF_ENABLE
    const bool have_external_ref =
        external_ref && external_ref_count >= sample_count && sample_count > 0;
    if (!playback_active || (!have_external_ref && !softref_enabled_for_cleaner(cleaner)) ||
        !samples || sample_count == 0) {
        return level;
    }
    if (!have_external_ref && sample_count > cleaner->softref_capacity_samples) {
        cleaner->stats.softref_ref_missing_frames++;
        return level;
    }

    const int16_t *ref_samples = external_ref;
    size_t copied = sample_count;
    if (!have_external_ref) {
        memset(cleaner->softref_buffer, 0, sample_count * sizeof(cleaner->softref_buffer[0]));
        copied = nuwax_p3_software_ref_read_delayed(cleaner->softref_buffer,
                                                    sample_count,
                                                    CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_DELAY_SAMPLES);
        ref_samples = cleaner->softref_buffer;
    }
    cleaner->stats.softref_frames++;
    if (result) {
        result->softref_copied_samples = (uint32_t)copied;
    }
    if (copied < sample_count) {
        cleaner->stats.softref_underrun_samples += (uint32_t)(sample_count - copied);
    }
    if (copied < (sample_count / 2U)) {
        cleaner->stats.softref_ref_missing_frames++;
        return level;
    }

    const nuwax_mic_level_t ref_level = nuwax_mic_level_measure(ref_samples, sample_count);
    cleaner->stats.softref_last_ref_avg_abs = ref_level.avg_abs;
    cleaner->stats.softref_last_ref_peak = ref_level.peak;
    if (result) {
        result->softref_ref_avg_abs = ref_level.avg_abs;
        result->softref_ref_peak = ref_level.peak;
    }
    const bool ref_active =
        ref_level.avg_abs >= (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_MIN_REF_AVG_ABS ||
        ref_level.peak >= (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_MIN_REF_PEAK;
    if (!ref_active) {
        return level;
    }
    cleaner->stats.softref_ref_active_frames++;
    if (result) {
        result->softref_active = true;
    }

    softref_echo_metrics_t metrics = compute_softref_metrics(samples, ref_samples, sample_count);
    cleaner->stats.softref_last_corr_q15 = metrics.corr_q15;
    if (result) {
        result->softref_corr_q15 = metrics.corr_q15;
    }
    const bool high_corr_echo =
        metrics.corr_q15 >= (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_GUARD_BYPASS_CORR_Q15;

    const bool pre_linear_nearend_guard =
        level.avg_abs >= (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_NEAREND_GUARD_AVG_ABS ||
        level.peak >= (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_NEAREND_GUARD_PEAK;
    const bool nearend_guard_requested =
        nearend_hint || (speech_protected && pre_linear_nearend_guard);
    const bool nearend_linear_echo =
        nearend_guard_requested &&
        metrics.corr_q15 >= (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_NEAREND_LINEAR_MIN_CORR_Q15 &&
        metrics.gain_q15 != 0;
    if (nearend_guard_requested && !nearend_linear_echo && !high_corr_echo) {
        cleaner->stats.softref_nearend_guard_frames++;
        if (result) {
            result->softref_nearend_guard = true;
        }
        return level;
    }

    if (metrics.corr_q15 >= (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_MIN_CORR_Q15 &&
        metrics.gain_q15 != 0) {
        int32_t linear_gain_q15 = metrics.gain_q15;
        if (nearend_guard_requested) {
            linear_gain_q15 =
                clamp_abs_i32(linear_gain_q15,
                              (int32_t)CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_NEAREND_MAX_LINEAR_GAIN_Q15);
        }
        apply_softref_linear_cancel(cleaner, samples, ref_samples, sample_count, linear_gain_q15);
        cleaner->stats.softref_linear_cancel_frames++;
        if (result) {
            result->softref_linear_cancel = true;
        }
        level = nuwax_mic_level_measure(samples, sample_count);
    }

    const bool nearend_guard =
        level.avg_abs >= (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_NEAREND_GUARD_AVG_ABS ||
        level.peak >= (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_NEAREND_GUARD_PEAK;
    if ((nearend_hint || (speech_protected && nearend_guard)) && !high_corr_echo) {
        cleaner->stats.softref_nearend_guard_frames++;
        if (result) {
            result->softref_nearend_guard = true;
        }
        return level;
    }

    const bool normal_echo_gate =
        !nearend_hint &&
        !speech_protected &&
        metrics.corr_q15 >= (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_MIN_CORR_Q15 &&
        level.avg_abs <= (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_ECHO_GATE_MAX_MIC_AVG_ABS &&
        level.peak <= (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_ECHO_GATE_MAX_MIC_PEAK;
    const bool high_corr_echo_gate =
        high_corr_echo &&
        !nearend_hint &&
        !speech_protected &&
        level.avg_abs <= (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_HIGH_CORR_GATE_MAX_MIC_AVG_ABS &&
        level.peak <= (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_HIGH_CORR_GATE_MAX_MIC_PEAK;
    if (normal_echo_gate || high_corr_echo_gate) {
        const int32_t atten_q15 = high_corr_echo_gate
                                      ? CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_HIGH_CORR_ATTENUATE_Q15
                                      : CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_ATTENUATE_Q15;
        apply_softref_attenuation(samples, sample_count, atten_q15);
        cleaner->stats.softref_echo_gate_frames++;
        if (result) {
            result->softref_echo_gate = true;
        }
        level = nuwax_mic_level_measure(samples, sample_count);
    }
    return level;
#else
    (void)cleaner;
    (void)samples;
    (void)sample_count;
    (void)playback_active;
    (void)nearend_hint;
    (void)speech_protected;
    (void)external_ref;
    (void)external_ref_count;
    (void)result;
    return level;
#endif
}

static void highpass_frame(nuwax_audio_cleaner_t *cleaner,
                           int16_t *samples,
                           size_t sample_count) {
#if CONFIG_NUWAX_AUDIO_CLEANER_HIGHPASS_ENABLE
    if (!cleaner || !samples) {
        return;
    }
    const int32_t coeff_q15 = CONFIG_NUWAX_AUDIO_CLEANER_HIGHPASS_COEFF_Q15;
    for (size_t i = 0; i < sample_count; i++) {
        const int32_t x = samples[i];
        /* Signed division truncates toward zero, so negative feedback cannot settle in a non-zero LSB fixed point. */
        const int32_t feedback =
            (int32_t)(((int64_t)cleaner->prev_highpass * coeff_q15) / 32768LL);
        int32_t y = x - (int32_t)cleaner->prev_input +
                    feedback;
        int16_t out = clamp_i16(y);
        if ((int32_t)out != y) {
            cleaner->stats.clipped_samples++;
            y = out;
        }
        cleaner->prev_input = (int16_t)x;
        cleaner->prev_highpass = y;
        samples[i] = out;
    }
#else
    (void)cleaner;
    (void)samples;
    (void)sample_count;
#endif
}

static void notch_100hz_frame(nuwax_audio_cleaner_t *cleaner,
                              int16_t *samples,
                              size_t sample_count) {
#if CONFIG_NUWAX_AUDIO_CLEANER_NOTCH_100HZ_ENABLE
    if (!cleaner || !samples) {
        return;
    }
    const float b0 = 0.9975522676f;
    const float b1 = -1.9935663820f;
    const float b2 = 0.9975522676f;
    const float a1 = -1.9935663820f;
    const float a2 = 0.9951045352f;
    float x1 = cleaner->notch_x1;
    float x2 = cleaner->notch_x2;
    float y1 = cleaner->notch_y1;
    float y2 = cleaner->notch_y2;
    for (size_t i = 0; i < sample_count; i++) {
        const float x0 = (float)samples[i];
        const float y0 = b0 * x0 + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        const int16_t out = clamp_i16((int32_t)lrintf(y0));
        if ((float)out != y0 && (y0 > 32767.0f || y0 < -32768.0f)) {
            cleaner->stats.clipped_samples++;
        }
        samples[i] = out;
        x2 = x1;
        x1 = x0;
        y2 = y1;
        y1 = isfinite(y0) ? y0 : 0.0f;
    }
    cleaner->notch_x1 = x1;
    cleaner->notch_x2 = x2;
    cleaner->notch_y1 = y1;
    cleaner->notch_y2 = y2;
#else
    (void)cleaner;
    (void)samples;
    (void)sample_count;
#endif
}

void nuwax_audio_cleaner_init(nuwax_audio_cleaner_t *cleaner) {
    if (!cleaner) {
        return;
    }
    memset(cleaner, 0, sizeof(*cleaner));
    cleaner->enabled = CONFIG_NUWAX_AUDIO_CLEANER_ENABLE;
    reset_playback_gain_state(cleaner);
    reset_playback_echo_atten_state(cleaner);
    reset_playback_unprotected_gate_state(cleaner);
    cleaner->stats.playback_unprotected_min_gain_q15 = 32768U;
#if CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_ENABLE && CONFIG_NUWAX_P3_SOFTWARE_REF_ENABLE
    cleaner->softref_enabled = cleaner->enabled;
    if (cleaner->softref_enabled) {
        cleaner->softref_capacity_samples = CONFIG_NUWAX_VOICE_MAX_SAMPLES_PER_FRAME;
        const size_t bytes = cleaner->softref_capacity_samples * sizeof(int16_t);
        cleaner->softref_buffer = (int16_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!cleaner->softref_buffer) {
            cleaner->softref_buffer = (int16_t *)heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
        }
        if (!cleaner->softref_buffer) {
            cleaner->softref_enabled = false;
            cleaner->softref_capacity_samples = 0;
            ESP_LOGW(TAG, "softref echo cleaner disabled: scratch allocation failed bytes=%u", (unsigned)bytes);
        }
    }
#endif
}

void nuwax_audio_cleaner_deinit(nuwax_audio_cleaner_t *cleaner) {
    if (!cleaner) {
        return;
    }
    heap_caps_free(cleaner->softref_buffer);
    cleaner->softref_buffer = NULL;
    cleaner->softref_capacity_samples = 0;
    cleaner->softref_enabled = false;
}

static void audio_cleaner_process_internal(nuwax_audio_cleaner_t *cleaner,
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
                                           nuwax_audio_cleaner_result_t *result) {
    nuwax_audio_cleaner_result_t local = {0};
    if (!result) {
        result = &local;
    }
    memset(result, 0, sizeof(*result));

    if (!cleaner || !samples || sample_count == 0) {
        return;
    }
    result->before = nuwax_mic_level_measure(samples, sample_count);
    cleaner->stats.processed_frames++;

    if (!cleaner->enabled) {
        result->after = result->before;
        return;
    }

    highpass_frame(cleaner, samples, sample_count);
    notch_100hz_frame(cleaner, samples, sample_count);
    nuwax_mic_level_t after_hpf = nuwax_mic_level_measure(samples, sample_count);
    const bool level_speech_now = playback_active
                                      ? playback_speech_protect_level(after_hpf)
                                      : speech_protect_level(after_hpf);
    const bool speech_now = speech_protect_hint || speech_gain_hint || level_speech_now;
    const bool speech_protected = speech_now || cleaner->speech_hangover_frames > 0;
    if (speech_now) {
        cleaner->speech_hangover_frames =
            (playback_active && !speech_protect_hint)
                ? (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_SPEECH_HANGOVER_FRAMES
                : (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_SPEECH_HANGOVER_FRAMES;
    } else if (cleaner->speech_hangover_frames > 0) {
        cleaner->speech_hangover_frames--;
    }
    if (speech_protected) {
        result->speech_protected = true;
        cleaner->stats.speech_protected_frames++;
    }

    nuwax_mic_level_t after_softref = process_softref_echo(cleaner,
                                                           samples,
                                                           sample_count,
                                                           playback_active,
                                                           speech_protect_hint,
                                                           speech_protected,
                                                           ref_samples,
                                                           ref_sample_count,
                                                           after_hpf,
                                                           result);
    if (apply_playback_protected_gain(cleaner,
                                      samples,
                                      sample_count,
                                      &after_softref,
                                      playback_active,
                                      speech_protected,
                                      speech_gain_hint,
                                      playback_echo_veto,
                                      result->before,
                                      result)) {
        result->playback_protected_gain = true;
        cleaner->stats.playback_protected_gain_frames++;
        if (result->playback_continuity_gain) {
            cleaner->stats.playback_continuity_gain_frames++;
        }
    }
    result->playback_gain_q15 = cleaner->playback_gain_q15;
    if (apply_playback_protected_limiter(samples,
                                         sample_count,
                                         &after_softref,
                                         playback_active,
                                         speech_protected)) {
        result->playback_protected_limited = true;
        cleaner->stats.playback_protected_limited_frames++;
    }
    bool playback_echo_veto_guarded = false;
    if (apply_playback_echo_veto_attenuation(cleaner,
                                             samples,
                                             sample_count,
                                             &after_softref,
                                             playback_active,
                                             speech_protect_hint,
                                             speech_gain_hint,
                                             speech_protected,
                                             playback_echo_veto,
                                             playback_echo_atten_guard,
                                             &playback_echo_veto_guarded)) {
        result->playback_echo_veto_attenuated = true;
        cleaner->stats.playback_echo_veto_attenuated_frames++;
    }
    if (playback_echo_veto_guarded) {
        result->playback_echo_veto_guarded = true;
        cleaner->stats.playback_echo_veto_guarded_frames++;
    }

    if (apply_playback_unprotected_attenuation(cleaner,
                                               samples,
                                               sample_count,
                                               &after_softref,
                                               playback_active,
                                               speech_protect_hint,
                                               speech_gain_hint,
                                               playback_human_strong_hint,
                                               speech_protected,
                                               result)) {
        result->playback_unprotected_attenuated = true;
        cleaner->stats.playback_unprotected_attenuated_frames++;
    }

    const uint32_t base_quiet_avg = (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_QUIET_AVG_ABS;
    const uint32_t base_quiet_peak = (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_QUIET_PEAK;
    uint32_t quiet_avg = base_quiet_avg;
    uint32_t quiet_peak = base_quiet_peak;
    if (playback_active) {
        quiet_avg = max_u32(quiet_avg, (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_QUIET_AVG_ABS);
        quiet_peak = max_u32(quiet_peak, (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_QUIET_PEAK);
    }

    if (cleaner->noise_floor_avg_abs > 0
#if CONFIG_NUWAX_LIVE_AEC_ENABLE
        && !playback_active
#endif
    ) {
        const uint32_t dynamic_avg = cleaner->noise_floor_avg_abs * 2u + 30u;
        quiet_avg = max_u32(quiet_avg, min_u32(dynamic_avg, quiet_avg * 2u));
    }

    const bool quiet_frame = after_softref.avg_abs <= quiet_avg && after_softref.peak <= quiet_peak;
    if (quiet_frame) {
        if (speech_protected || playback_active) {
            result->playback_quiet = playback_active;
            if (playback_active) {
                cleaner->stats.playback_quiet_frames++;
            }
            result->after = after_softref;
            return;
        }
#if CONFIG_NUWAX_AUDIO_CLEANER_QUIET_ZERO_ENABLE
        memset(samples, 0, sample_count * sizeof(samples[0]));
        result->zeroed_quiet = true;
        cleaner->stats.zeroed_quiet_frames++;
#else
        const int32_t atten_q15 =
            playback_active ? CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_QUIET_ATTENUATE_Q15 :
                              CONFIG_NUWAX_AUDIO_CLEANER_QUIET_ATTENUATE_Q15;
        if (atten_q15 >= 32768) {
            update_noise_floor(cleaner, after_hpf.avg_abs);
            result->after = after_softref;
            return;
        }
        apply_softref_attenuation(samples, sample_count, atten_q15);
        result->attenuated_quiet = true;
        cleaner->stats.attenuated_quiet_frames++;
#endif
        result->playback_quiet = playback_active;
        if (playback_active) {
            cleaner->stats.playback_quiet_frames++;
        }
        update_noise_floor(cleaner, after_hpf.avg_abs);
        result->after = nuwax_mic_level_measure(samples, sample_count);
        return;
    }

    const uint32_t impulse_peak_min =
        playback_active ? (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_IMPULSE_PEAK_MIN
                        : (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_IMPULSE_PEAK_MIN;
    const uint32_t impulse_avg_max =
        playback_active ? (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_IMPULSE_AVG_MAX
                        : (uint32_t)CONFIG_NUWAX_AUDIO_CLEANER_IMPULSE_AVG_MAX;
    const bool impulse_frame =
        !speech_protected &&
        after_softref.peak >= impulse_peak_min &&
        after_softref.avg_abs <= impulse_avg_max;
    if (impulse_frame) {
        const int32_t atten_q15 = playback_active
                                      ? (int32_t)CONFIG_NUWAX_AUDIO_CLEANER_PLAYBACK_IMPULSE_ATTENUATE_Q15
                                      : 32768;
        if (atten_q15 >= 32768) {
            result->after = after_softref;
            return;
        }
        apply_softref_attenuation(samples, sample_count, atten_q15);
        cleaner->stats.attenuated_impulse_frames++;
        result->attenuated_impulse = true;
        result->after = nuwax_mic_level_measure(samples, sample_count);
        return;
    }

#if CONFIG_NUWAX_VOICE_LOCAL_VAD_ENDPOINT_ENABLE
    if (after_softref.avg_abs <= (uint32_t)CONFIG_NUWAX_VOICE_LOCAL_VAD_SILENCE_AVG_ABS &&
        after_softref.peak <= (uint32_t)CONFIG_NUWAX_VOICE_LOCAL_VAD_SILENCE_PEAK) {
        update_noise_floor(cleaner, after_softref.avg_abs);
    }
#endif
    result->after = after_softref;
}

void nuwax_audio_cleaner_process(nuwax_audio_cleaner_t *cleaner,
                                 int16_t *samples,
                                 size_t sample_count,
                                 bool playback_active,
                                 bool speech_protect_hint,
                                 bool speech_gain_hint,
                                 bool playback_human_strong_hint,
                                 bool playback_echo_veto,
                                 bool playback_echo_atten_guard,
                                 nuwax_audio_cleaner_result_t *result) {
    audio_cleaner_process_internal(cleaner,
                                   samples,
                                   sample_count,
                                   playback_active,
                                   speech_protect_hint,
                                   speech_gain_hint,
                                   playback_human_strong_hint,
                                   playback_echo_veto,
                                   playback_echo_atten_guard,
                                   NULL,
                                   0,
                                   result);
}

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
                                          nuwax_audio_cleaner_result_t *result) {
    audio_cleaner_process_internal(cleaner,
                                   samples,
                                   sample_count,
                                   playback_active,
                                   speech_protect_hint,
                                   speech_gain_hint,
                                   playback_human_strong_hint,
                                   playback_echo_veto,
                                   playback_echo_atten_guard,
                                   ref_samples,
                                   ref_sample_count,
                                   result);
}

void nuwax_audio_cleaner_get_stats(const nuwax_audio_cleaner_t *cleaner,
                                   nuwax_audio_cleaner_stats_t *stats) {
    if (!stats) {
        return;
    }
    memset(stats, 0, sizeof(*stats));
    if (!cleaner) {
        return;
    }
    *stats = cleaner->stats;
}
