#include "conversation/live_aec.h"

#include <math.h>
#include <string.h>

#include "conversation/mic_level.h"
#include "conversation/realtime_log.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "p3_software_ref.h"
#include "sdkconfig.h"

#ifndef CONFIG_NUWAX_LIVE_AEC_ENABLE
#define CONFIG_NUWAX_LIVE_AEC_ENABLE 0
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_REF_DELAY_SAMPLES
#if CONFIG_NUWAX_BOARD_ESP_VOCAT_V1_0
#define CONFIG_NUWAX_LIVE_AEC_REF_DELAY_SAMPLES 360
#else
#define CONFIG_NUWAX_LIVE_AEC_REF_DELAY_SAMPLES 240
#endif
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_MIN_REF_AVG_ABS
#define CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_MIN_REF_AVG_ABS 120
#endif
#ifndef CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_MIN_REF_PEAK
#define CONFIG_NUWAX_AUDIO_CLEANER_SOFTREF_MIN_REF_PEAK 350
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_MIN_REF_AVG_ABS
#define CONFIG_NUWAX_LIVE_AEC_MIN_REF_AVG_ABS 20
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_MIN_REF_PEAK
#define CONFIG_NUWAX_LIVE_AEC_MIN_REF_PEAK 60
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_OUTPUT_GAIN_Q15
#define CONFIG_NUWAX_LIVE_AEC_OUTPUT_GAIN_Q15 32768
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_REF_MISSING_ATTENUATE_Q15
#define CONFIG_NUWAX_LIVE_AEC_REF_MISSING_ATTENUATE_Q15 4096
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_NEAREND_BLEND_ENABLE
#define CONFIG_NUWAX_LIVE_AEC_NEAREND_BLEND_ENABLE 1
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_NEAREND_BLEND_AVG_ABS
#define CONFIG_NUWAX_LIVE_AEC_NEAREND_BLEND_AVG_ABS 300
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_NEAREND_BLEND_PEAK
#define CONFIG_NUWAX_LIVE_AEC_NEAREND_BLEND_PEAK 1000
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_NEAREND_RAW_MIX_Q15
#define CONFIG_NUWAX_LIVE_AEC_NEAREND_RAW_MIX_Q15 1024
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_STRONG_NEAREND_AVG_ABS
#define CONFIG_NUWAX_LIVE_AEC_STRONG_NEAREND_AVG_ABS 550
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_STRONG_NEAREND_PEAK
#define CONFIG_NUWAX_LIVE_AEC_STRONG_NEAREND_PEAK 1800
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_STRONG_NEAREND_RAW_MIX_Q15
#define CONFIG_NUWAX_LIVE_AEC_STRONG_NEAREND_RAW_MIX_Q15 2048
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_HINT_STRONG_RAW_MIX_Q15
#define CONFIG_NUWAX_LIVE_AEC_HINT_STRONG_RAW_MIX_Q15 0
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_REF_MISSING_HINT_ATTENUATE_Q15
#define CONFIG_NUWAX_LIVE_AEC_REF_MISSING_HINT_ATTENUATE_Q15 24576
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_REF_INACTIVE_ATTENUATE_Q15
#define CONFIG_NUWAX_LIVE_AEC_REF_INACTIVE_ATTENUATE_Q15 32768
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_REF_INACTIVE_HINT_ATTENUATE_Q15
#define CONFIG_NUWAX_LIVE_AEC_REF_INACTIVE_HINT_ATTENUATE_Q15 24576
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_NEAREND_BLEND_AEC_AVG_ABS
#define CONFIG_NUWAX_LIVE_AEC_NEAREND_BLEND_AEC_AVG_ABS 300
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_NEAREND_BLEND_AEC_PEAK
#define CONFIG_NUWAX_LIVE_AEC_NEAREND_BLEND_AEC_PEAK 1000
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_NEAREND_BLEND_MIN_OUT_RATIO_Q15
#define CONFIG_NUWAX_LIVE_AEC_NEAREND_BLEND_MIN_OUT_RATIO_Q15 16384
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_NEAREND_BLEND_FULL_CORR_Q15
#define CONFIG_NUWAX_LIVE_AEC_NEAREND_BLEND_FULL_CORR_Q15 9830
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_NEAREND_BLEND_GUARD_CORR_Q15
#define CONFIG_NUWAX_LIVE_AEC_NEAREND_BLEND_GUARD_CORR_Q15 19661
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_STRONG_NEAREND_BLEND_GUARD_CORR_Q15
#define CONFIG_NUWAX_LIVE_AEC_STRONG_NEAREND_BLEND_GUARD_CORR_Q15 22938
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_NEAREND_BLEND_DUAL_GUARD_ENABLE
#define CONFIG_NUWAX_LIVE_AEC_NEAREND_BLEND_DUAL_GUARD_ENABLE 1
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_NEAREND_BLEND_DUAL_BALANCE_Q15
#define CONFIG_NUWAX_LIVE_AEC_NEAREND_BLEND_DUAL_BALANCE_Q15 22000
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_NEAREND_BLEND_DUAL_DIFF_AVG_ABS
#define CONFIG_NUWAX_LIVE_AEC_NEAREND_BLEND_DUAL_DIFF_AVG_ABS 450
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_NEAREND_BLEND_DUAL_DIFF_PEAK
#define CONFIG_NUWAX_LIVE_AEC_NEAREND_BLEND_DUAL_DIFF_PEAK 1600
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_RESIDUAL_CANCEL_ENABLE
#define CONFIG_NUWAX_LIVE_AEC_RESIDUAL_CANCEL_ENABLE 0
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_RESIDUAL_MIN_CORR_Q15
#define CONFIG_NUWAX_LIVE_AEC_RESIDUAL_MIN_CORR_Q15 8192
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_RESIDUAL_NEAREND_MIN_CORR_Q15
#define CONFIG_NUWAX_LIVE_AEC_RESIDUAL_NEAREND_MIN_CORR_Q15 18022
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_RESIDUAL_MAX_GAIN_Q15
#define CONFIG_NUWAX_LIVE_AEC_RESIDUAL_MAX_GAIN_Q15 196608
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_POST_BLEND_RESIDUAL_ENABLE
#define CONFIG_NUWAX_LIVE_AEC_POST_BLEND_RESIDUAL_ENABLE 0
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_POST_BLEND_RESIDUAL_MIN_CORR_Q15
#define CONFIG_NUWAX_LIVE_AEC_POST_BLEND_RESIDUAL_MIN_CORR_Q15 12000
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_POST_BLEND_RESIDUAL_MAX_GAIN_Q15
#define CONFIG_NUWAX_LIVE_AEC_POST_BLEND_RESIDUAL_MAX_GAIN_Q15 65536
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_FILTER_LENGTH
#define CONFIG_NUWAX_LIVE_AEC_FILTER_LENGTH 4
#endif
#ifndef CONFIG_NUWAX_P3_SOFTWARE_REF_AEC_AB_ENABLE
#define CONFIG_NUWAX_P3_SOFTWARE_REF_AEC_AB_ENABLE 0
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_BACKEND_AFE
#define CONFIG_NUWAX_LIVE_AEC_BACKEND_AFE 1
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_FORCE_VC
#define CONFIG_NUWAX_LIVE_AEC_FORCE_VC 1
#endif

#ifndef CONFIG_NUWAX_LIVE_AEC_ADAPTIVE_REF_ENABLE
#define CONFIG_NUWAX_LIVE_AEC_ADAPTIVE_REF_ENABLE 0
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_ADAPTIVE_REF_SPAN_SAMPLES
#define CONFIG_NUWAX_LIVE_AEC_ADAPTIVE_REF_SPAN_SAMPLES 640
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_ADAPTIVE_REF_STEP_SAMPLES
#define CONFIG_NUWAX_LIVE_AEC_ADAPTIVE_REF_STEP_SAMPLES 320
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_ADAPTIVE_REF_MIN_CORR_Q15
#define CONFIG_NUWAX_LIVE_AEC_ADAPTIVE_REF_MIN_CORR_Q15 1500
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_ADAPTIVE_REF_MIN_IMPROVE_Q15
#define CONFIG_NUWAX_LIVE_AEC_ADAPTIVE_REF_MIN_IMPROVE_Q15 2048
#endif
#ifndef NUWAX_LIVE_AEC_ADAPTIVE_REF_SCAN_INTERVAL_CHUNKS
#define NUWAX_LIVE_AEC_ADAPTIVE_REF_SCAN_INTERVAL_CHUNKS 8U
#endif

#ifndef CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_ENABLE
#define CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_ENABLE 0
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_MIN_SCORE_Q15
#define CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_MIN_SCORE_Q15 14000
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_WEAK_GAIN_Q15
#define CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_WEAK_GAIN_Q15 3072
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_MID_GAIN_Q15
#define CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_MID_GAIN_Q15 6144
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_STRONG_GAIN_Q15
#define CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_STRONG_GAIN_Q15 8192
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_HINT_GAIN_Q15
#define CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_HINT_GAIN_Q15 8192
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_HIGH_CORR_GUARD_Q15
#define CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_HIGH_CORR_GUARD_Q15 19661
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_MIN_DIFF_AVG_ABS
#define CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_MIN_DIFF_AVG_ABS 180
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_MIN_DIFF_PEAK
#define CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_MIN_DIFF_PEAK 600
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_SPATIAL_BALANCE_Q15
#define CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_SPATIAL_BALANCE_Q15 22000
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_SPATIAL_DIFF_AVG_ABS
#define CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_SPATIAL_DIFF_AVG_ABS 320
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_SPATIAL_DIFF_PEAK
#define CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_SPATIAL_DIFF_PEAK 1100
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_VERY_STRONG_DIFF_AVG_ABS
#define CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_VERY_STRONG_DIFF_AVG_ABS 1200
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_VERY_STRONG_DIFF_PEAK
#define CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_VERY_STRONG_DIFF_PEAK 4200
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_VERY_STRONG_BALANCE_Q15
#define CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_VERY_STRONG_BALANCE_Q15 26000
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_POST_RESIDUAL_CORR_Q15
#define CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_POST_RESIDUAL_CORR_Q15 12000
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_POST_RESIDUAL_MAX_GAIN_Q15
#define CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_POST_RESIDUAL_MAX_GAIN_Q15 32768
#endif

#ifndef CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_ENABLE
#define CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_ENABLE 0
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_PROVISIONAL_MIX_Q15
#define CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_PROVISIONAL_MIX_Q15 24576
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_CONFIRMED_MIX_Q15
#define CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_CONFIRMED_MIX_Q15 24576
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FAST_MIX_Q15
#define CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FAST_MIX_Q15 24576
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_MAX_REF_GAIN_Q15
#define CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_MAX_REF_GAIN_Q15 65536
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_MIN_REF_CORR_Q15
#define CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_MIN_REF_CORR_Q15 8192
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_ENABLE
#define CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_ENABLE 0
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_TAPS
#define CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_TAPS 256
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_SEED_TAP
#define CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_SEED_TAP 120
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_STEP_Q15
#define CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_STEP_Q15 2048
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_MIN_TRAINING_SAMPLES
#define CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_MIN_TRAINING_SAMPLES 8192
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_WARMUP_SAMPLES
#define CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_WARMUP_SAMPLES 32000
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_MAX_RESIDUAL_RAW_Q15
#define CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_MAX_RESIDUAL_RAW_Q15 6144
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_MAX_RESIDUAL_SCALAR_Q15
#define CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_MAX_RESIDUAL_SCALAR_Q15 8192
#endif
#define NUWAX_LIVE_AEC_HUMAN_RESCUE_ATTACK_STEP_Q15 8192U
#define NUWAX_LIVE_AEC_HUMAN_RESCUE_RELEASE_STEP_Q15 2048U
#define NUWAX_LIVE_AEC_HUMAN_RESCUE_GAIN_SMOOTH_SHIFT 2U
#define NUWAX_LIVE_AEC_HUMAN_RESCUE_GAIN_MAX_STEP_Q15 4096
#define NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_NORM_EPSILON 0.001f
#define NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_MAX_COEFF 4.0f
#define NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_MAX_ESTIMATE 4.0f
#define NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_DIVERGENCE_RATIO_Q15 40960U
#define NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_MIN_PREDICTION_CORR_Q15 8192U
#define NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_VALIDATION_CHUNKS 32U
#define NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_READY_MAX_RAW_Q15 40960U
#define NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_READY_HINT_RAW_Q15 16384U
#define NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_READY_HEALTH_CHUNKS 8U
#define NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_NEAREND_HOLD_CHUNKS 64U
#define NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_PENDING_MIX_Q15 8192U
#define NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_COEFF_EDGE_TAPS 16U
#define NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_MIN_PEAK_TAP 8U
#define NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_MAX_EDGE_ENERGY_Q15 16384U
#define NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_TRAIN_MIN_REF_AVG_ABS 100U
#define NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_TRAIN_MIN_REF_PEAK 300U

#ifndef CONFIG_NUWAX_LIVE_AEC_NEAREND_FLOOR_ENABLE
#define CONFIG_NUWAX_LIVE_AEC_NEAREND_FLOOR_ENABLE 0
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_NEAREND_FLOOR_TARGET_Q15
#define CONFIG_NUWAX_LIVE_AEC_NEAREND_FLOOR_TARGET_Q15 12288
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_NEAREND_FLOOR_MAX_GAIN_Q15
#define CONFIG_NUWAX_LIVE_AEC_NEAREND_FLOOR_MAX_GAIN_Q15 8192
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_NEAREND_FLOOR_MIN_MIC_AVG_ABS
#define CONFIG_NUWAX_LIVE_AEC_NEAREND_FLOOR_MIN_MIC_AVG_ABS 300
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_NEAREND_FLOOR_MIN_MIC_PEAK
#define CONFIG_NUWAX_LIVE_AEC_NEAREND_FLOOR_MIN_MIC_PEAK 900
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_NEAREND_FLOOR_STRICT_BALANCE_Q15
#define CONFIG_NUWAX_LIVE_AEC_NEAREND_FLOOR_STRICT_BALANCE_Q15 20000
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_NEAREND_FLOOR_MIN_DIFF_AVG_ABS
#define CONFIG_NUWAX_LIVE_AEC_NEAREND_FLOOR_MIN_DIFF_AVG_ABS 180
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_NEAREND_FLOOR_MIN_DIFF_PEAK
#define CONFIG_NUWAX_LIVE_AEC_NEAREND_FLOOR_MIN_DIFF_PEAK 700
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_NEAREND_FLOOR_REF_GUARD_CORR_Q15
#define CONFIG_NUWAX_LIVE_AEC_NEAREND_FLOOR_REF_GUARD_CORR_Q15 19661
#endif

#if CONFIG_NUWAX_LIVE_AEC_ENABLE && (CONFIG_NUWAX_P3_SOFTWARE_REF_ENABLE || CONFIG_NUWAX_VOCAT_SLOT2_AEC_REF_ENABLE)
#if CONFIG_NUWAX_LIVE_AEC_BACKEND_AFE
#include "esp_afe_aec.h"
#define NUWAX_LIVE_AEC_BACKEND_AFE 1
#else
#include "esp_aec.h"
#define NUWAX_LIVE_AEC_BACKEND_AFE 0
#endif
#if NUWAX_LIVE_AEC_BACKEND_AFE
#if defined(CONFIG_NUWAX_LIVE_AEC_FORCE_VC) && CONFIG_NUWAX_LIVE_AEC_FORCE_VC
#define NUWAX_LIVE_AEC_TYPE_NAME "VC"
#define NUWAX_LIVE_AFE_TYPE AFE_TYPE_VC
#elif CONFIG_NUWAX_P3_SOFTREF_AB_AEC_TYPE_VC
#define NUWAX_LIVE_AEC_TYPE_NAME "VC"
#define NUWAX_LIVE_AFE_TYPE AFE_TYPE_VC
#else
#define NUWAX_LIVE_AEC_TYPE_NAME "SR"
#define NUWAX_LIVE_AFE_TYPE AFE_TYPE_SR
#endif
#if CONFIG_NUWAX_LIVE_AEC_MODE_HIGH_PERF
#define NUWAX_LIVE_AEC_MODE_NAME "HIGH_PERF"
#define NUWAX_LIVE_AFE_MODE AFE_MODE_HIGH_PERF
#else
#define NUWAX_LIVE_AEC_MODE_NAME "LOW_COST"
#define NUWAX_LIVE_AFE_MODE AFE_MODE_LOW_COST
#endif
#else
#if defined(CONFIG_NUWAX_LIVE_AEC_FORCE_VC) && CONFIG_NUWAX_LIVE_AEC_FORCE_VC
#define NUWAX_LIVE_AEC_TYPE_NAME "VC"
#if CONFIG_NUWAX_LIVE_AEC_MODE_HIGH_PERF
#define NUWAX_LIVE_AEC_MODE AEC_MODE_VOIP_HIGH_PERF
#define NUWAX_LIVE_AEC_MODE_NAME "VOIP_HIGH_PERF"
#else
#define NUWAX_LIVE_AEC_MODE AEC_MODE_VOIP_LOW_COST
#define NUWAX_LIVE_AEC_MODE_NAME "VOIP_LOW_COST"
#endif
#elif CONFIG_NUWAX_P3_SOFTREF_AB_AEC_TYPE_VC
#define NUWAX_LIVE_AEC_TYPE_NAME "VC"
#if CONFIG_NUWAX_LIVE_AEC_MODE_HIGH_PERF
#define NUWAX_LIVE_AEC_MODE AEC_MODE_VOIP_HIGH_PERF
#define NUWAX_LIVE_AEC_MODE_NAME "VOIP_HIGH_PERF"
#else
#define NUWAX_LIVE_AEC_MODE AEC_MODE_VOIP_LOW_COST
#define NUWAX_LIVE_AEC_MODE_NAME "VOIP_LOW_COST"
#endif
#else
#define NUWAX_LIVE_AEC_TYPE_NAME "SR"
#if CONFIG_NUWAX_LIVE_AEC_MODE_HIGH_PERF
#define NUWAX_LIVE_AEC_MODE AEC_MODE_SR_HIGH_PERF
#define NUWAX_LIVE_AEC_MODE_NAME "SR_HIGH_PERF"
#else
#define NUWAX_LIVE_AEC_MODE AEC_MODE_SR_LOW_COST
#define NUWAX_LIVE_AEC_MODE_NAME "SR_LOW_COST"
#endif
#endif
#endif
#endif

static const char *TAG = "nuwax_live_aec";

static void *alloc_aligned_audio(size_t bytes) {
    void *ptr = heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) {
        ptr = heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_8BIT);
    }
    return ptr;
}

static void *alloc_hot_audio(size_t bytes) {
    void *ptr = heap_caps_aligned_alloc(
        16, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return ptr ? ptr : alloc_aligned_audio(bytes);
}

static void *alloc_internal_dsp(size_t bytes) {
    return heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static bool ref_measured_level_active(nuwax_mic_level_t level) {
    return level.avg_abs >= (uint32_t)CONFIG_NUWAX_LIVE_AEC_MIN_REF_AVG_ABS ||
           level.peak >= (uint32_t)CONFIG_NUWAX_LIVE_AEC_MIN_REF_PEAK;
}

static bool ref_level_active(const int16_t *samples, size_t sample_count) {
    return ref_measured_level_active(
        nuwax_mic_level_measure(samples, sample_count));
}

static int16_t clamp_i16(int32_t value) {
    if (value > INT16_MAX) {
        return INT16_MAX;
    }
    if (value < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)value;
}

static void apply_output_gain(nuwax_live_aec_t *aec, int16_t *samples, size_t sample_count) {
    if (!aec || !samples || sample_count == 0 || aec->output_gain_q15 == 32768U) {
        return;
    }
    const int32_t gain_q15 = (int32_t)aec->output_gain_q15;
    for (size_t i = 0; i < sample_count; i++) {
        const int32_t scaled = (int32_t)(((int64_t)samples[i] * gain_q15) >> 15);
        const int16_t out = clamp_i16(scaled);
        if ((int32_t)out != scaled) {
            aec->clipped_samples++;
        }
        samples[i] = out;
    }
}

static bool nearend_blend_active(nuwax_mic_level_t mic_level) {
#if CONFIG_NUWAX_LIVE_AEC_NEAREND_BLEND_ENABLE
    return mic_level.avg_abs >= (uint32_t)CONFIG_NUWAX_LIVE_AEC_NEAREND_BLEND_AVG_ABS ||
           mic_level.peak >= (uint32_t)CONFIG_NUWAX_LIVE_AEC_NEAREND_BLEND_PEAK;
#else
    (void)mic_level;
    return false;
#endif
}

static bool strong_nearend_active(nuwax_mic_level_t mic_level) {
#if CONFIG_NUWAX_LIVE_AEC_NEAREND_BLEND_ENABLE
    return mic_level.avg_abs >= (uint32_t)CONFIG_NUWAX_LIVE_AEC_STRONG_NEAREND_AVG_ABS ||
           mic_level.peak >= (uint32_t)CONFIG_NUWAX_LIVE_AEC_STRONG_NEAREND_PEAK;
#else
    (void)mic_level;
    return false;
#endif
}

static bool nearend_blend_aec_output_evidence(nuwax_mic_level_t out_level) {
#if CONFIG_NUWAX_LIVE_AEC_NEAREND_BLEND_ENABLE
    return out_level.avg_abs >= (uint32_t)CONFIG_NUWAX_LIVE_AEC_NEAREND_BLEND_AEC_AVG_ABS ||
           out_level.peak >= (uint32_t)CONFIG_NUWAX_LIVE_AEC_NEAREND_BLEND_AEC_PEAK;
#else
    (void)out_level;
    return false;
#endif
}

typedef struct {
    uint32_t corr_q15;
    int32_t gain_q15;
} residual_metrics_t;

typedef struct {
    nuwax_mic_level_t level;
    uint32_t energy_ratio_q15;
    int32_t mic2_to_mic1_gain_q15;
    int sign;
} dual_restore_metrics_t;


#if CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_ENABLE
static uint32_t clamp_u32(uint32_t value, uint32_t lo, uint32_t hi) {
    if (value < lo) {
        return lo;
    }
    if (value > hi) {
        return hi;
    }
    return value;
}
#endif

static int32_t clamp_i32(int32_t value, int32_t lo, int32_t hi) {
    if (value < lo) {
        return lo;
    }
    if (value > hi) {
        return hi;
    }
    return value;
}

static uint32_t q15_ratio_u64(uint64_t num, uint64_t den) {
    if (den == 0) {
        return 0;
    }
    uint64_t q15 = (num * 32768ULL) / den;
    if (q15 > 32768ULL) {
        q15 = 32768ULL;
    }
    return (uint32_t)q15;
}

static uint32_t dual_level_balance_q15(nuwax_mic_level_t mic1_level, nuwax_mic_level_t mic2_level) {
    const uint32_t max_avg =
        mic1_level.avg_abs > mic2_level.avg_abs ? mic1_level.avg_abs : mic2_level.avg_abs;
    const uint32_t min_avg =
        mic1_level.avg_abs > mic2_level.avg_abs ? mic2_level.avg_abs : mic1_level.avg_abs;
    if (max_avg == 0) {
        return 0;
    }
    uint64_t balance = ((uint64_t)min_avg * 32768ULL) / (uint64_t)max_avg;
    if (balance > 32768ULL) {
        balance = 32768ULL;
    }
    return (uint32_t)balance;
}

static dual_restore_metrics_t compute_dual_restore_metrics(const int16_t *mic1,
                                                           const int16_t *mic2,
                                                           size_t sample_count) {
    dual_restore_metrics_t metrics = {
        .mic2_to_mic1_gain_q15 = 32768,
        .sign = 1,
    };
    if (!mic1 || !mic2 || sample_count == 0) {
        return metrics;
    }
    int64_t dot12 = 0;
    uint64_t sum1 = 0;
    uint64_t sum2 = 0;
    for (size_t i = 0; i < sample_count; i++) {
        const int32_t s1 = mic1[i];
        const int32_t s2 = mic2[i];
        dot12 += (int64_t)s1 * (int64_t)s2;
        sum1 += (uint64_t)((int64_t)s1 * (int64_t)s1);
        sum2 += (uint64_t)((int64_t)s2 * (int64_t)s2);
    }
    if (sum2 > 0) {
        int64_t gain = (dot12 * 32768LL) / (int64_t)sum2;
        if (gain > 131072LL) {
            gain = 131072LL;
        } else if (gain < -131072LL) {
            gain = -131072LL;
        }
        metrics.mic2_to_mic1_gain_q15 = (int32_t)gain;
    }

    uint64_t sum_near = 0;
    uint64_t sum_abs = 0;
    uint32_t peak = 0;
    int64_t dot_near_mic1 = 0;
    for (size_t i = 0; i < sample_count; i++) {
        const int32_t s1 = mic1[i];
        const int32_t s2 = mic2[i];
        int32_t near = s1 - (int32_t)(((int64_t)metrics.mic2_to_mic1_gain_q15 * (int64_t)s2) >> 15);
        near = clamp_i32(near, INT16_MIN, INT16_MAX);
        const uint32_t abs_near = (uint32_t)(near < 0 ? -near : near);
        sum_abs += abs_near;
        sum_near += (uint64_t)((int64_t)near * (int64_t)near);
        dot_near_mic1 += (int64_t)near * (int64_t)s1;
        if (abs_near > peak) {
            peak = abs_near;
        }
    }
    metrics.sign = dot_near_mic1 < 0 ? -1 : 1;
    metrics.level.avg_abs = (uint32_t)(sum_abs / sample_count);
    metrics.level.peak = peak;
    const uint64_t den = sum1 > sum2 ? sum1 : sum2;
    metrics.energy_ratio_q15 = q15_ratio_u64(sum_near, den);
    return metrics;
}

static bool nearend_blend_dual_evidence(const nuwax_live_aec_t *aec, size_t sample_count) {
#if CONFIG_NUWAX_LIVE_AEC_NEAREND_BLEND_DUAL_GUARD_ENABLE
    if (!aec || !aec->pending_dual_mic || !aec->pending_mic || !aec->pending_mic2 ||
        sample_count == 0) {
        return false;
    }
    const nuwax_mic_level_t mic1_level = nuwax_mic_level_measure(aec->pending_mic, sample_count);
    const nuwax_mic_level_t mic2_level = nuwax_mic_level_measure(aec->pending_mic2, sample_count);
    const uint32_t balance_q15 = dual_level_balance_q15(mic1_level, mic2_level);
    if (balance_q15 == 0 ||
        balance_q15 > (uint32_t)CONFIG_NUWAX_LIVE_AEC_NEAREND_BLEND_DUAL_BALANCE_Q15) {
        return false;
    }
    const dual_restore_metrics_t dual =
        compute_dual_restore_metrics(aec->pending_mic, aec->pending_mic2, sample_count);
    return dual.level.avg_abs >= (uint32_t)CONFIG_NUWAX_LIVE_AEC_NEAREND_BLEND_DUAL_DIFF_AVG_ABS ||
           dual.level.peak >= (uint32_t)CONFIG_NUWAX_LIVE_AEC_NEAREND_BLEND_DUAL_DIFF_PEAK;
#else
    (void)aec;
    (void)sample_count;
    return true;
#endif
}

#if CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_ENABLE
static uint32_t dual_restore_score_q15(nuwax_mic_level_t mic_level,
                                       nuwax_mic_level_t mic2_level,
                                       nuwax_mic_level_t out_level,
                                       dual_restore_metrics_t dual,
                                       residual_metrics_t mic_ref_metrics,
                                       bool hinted_nearend,
                                       bool hinted_strong_nearend) {
    uint32_t score = 0;
    if (mic_level.avg_abs >= 300U && mic_level.peak >= 1000U) {
        score += 4000U;
    }
    if (mic_level.avg_abs >= 650U && mic_level.peak >= 1800U) {
        score += 4000U;
    }
    if (mic2_level.avg_abs >= 70U && mic2_level.peak >= 220U) {
        score += 3000U;
    }
    if (dual.level.avg_abs >= (uint32_t)CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_MIN_DIFF_AVG_ABS ||
        dual.level.peak >= (uint32_t)CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_MIN_DIFF_PEAK) {
        score += 6000U;
    }
    if (dual.level.avg_abs >= 260U || dual.level.peak >= 1200U) {
        score += 6000U;
    }
    if (dual.energy_ratio_q15 >= 2048U) {
        score += 3000U;
    }
    if (dual.energy_ratio_q15 >= 6144U) {
        score += 3000U;
    }
    if ((uint64_t)out_level.avg_abs * 32768ULL < (uint64_t)mic_level.avg_abs * 18000ULL &&
        mic_level.avg_abs >= 350U) {
        score += 5000U;
    }
    if (hinted_nearend) {
        score += 8000U;
    }
    if (hinted_strong_nearend) {
        score += 10000U;
    }
    if (mic_ref_metrics.corr_q15 >= (uint32_t)CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_HIGH_CORR_GUARD_Q15 &&
        !hinted_strong_nearend &&
        dual.level.avg_abs < 260U) {
        score = score > 9000U ? score - 9000U : 0U;
    }
    return clamp_u32(score, 0U, 32768U);
}
#endif

static bool apply_dual_mic_nearend_restore(nuwax_live_aec_t *aec,
                                           int16_t *out,
                                           size_t sample_count,
                                           residual_metrics_t mic_ref_metrics,
                                           bool ref_active) {
#if CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_ENABLE
    if (!aec || !out || !aec->pending_mic || !aec->pending_mic2 || !aec->pending_dual_mic ||
        sample_count == 0) {
        return false;
    }
    const nuwax_mic_level_t mic_level = nuwax_mic_level_measure(aec->pending_mic, sample_count);
    const nuwax_mic_level_t mic2_level = nuwax_mic_level_measure(aec->pending_mic2, sample_count);
    const nuwax_mic_level_t out_level = nuwax_mic_level_measure(out, sample_count);
    const dual_restore_metrics_t dual =
        compute_dual_restore_metrics(aec->pending_mic, aec->pending_mic2, sample_count);
    if (dual.level.avg_abs > aec->dual_restore_diff_avg_max) {
        aec->dual_restore_diff_avg_max = dual.level.avg_abs;
    }
    if (dual.level.peak > aec->dual_restore_diff_peak_max) {
        aec->dual_restore_diff_peak_max = dual.level.peak;
    }
    const uint32_t balance_q15 = dual_level_balance_q15(mic_level, mic2_level);
    if (balance_q15 > 0 &&
        (aec->dual_restore_balance_min_q15 == 0 || balance_q15 < aec->dual_restore_balance_min_q15)) {
        aec->dual_restore_balance_min_q15 = balance_q15;
    }

    const bool hinted_nearend = aec->nearend_preserve_hint;
    const bool hinted_strong_nearend = aec->strong_nearend_preserve_hint;
    const uint32_t score = dual_restore_score_q15(mic_level,
                                                  mic2_level,
                                                  out_level,
                                                  dual,
                                                  mic_ref_metrics,
                                                  hinted_nearend,
                                                  hinted_strong_nearend);
    if (score > aec->dual_restore_score_max_q15) {
        aec->dual_restore_score_max_q15 = score;
    }
    const bool enough_diff =
        dual.level.avg_abs >= (uint32_t)CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_MIN_DIFF_AVG_ABS &&
        dual.level.peak >= (uint32_t)CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_MIN_DIFF_PEAK;
    if (!enough_diff) {
        aec->dual_restore_guard_chunks++;
        return false;
    }
    const bool spatial_nearend =
        balance_q15 > 0 &&
        balance_q15 <= (uint32_t)CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_SPATIAL_BALANCE_Q15 &&
        dual.level.avg_abs >= (uint32_t)CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_SPATIAL_DIFF_AVG_ABS &&
        dual.level.peak >= (uint32_t)CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_SPATIAL_DIFF_PEAK;
    const bool very_strong_nearend =
        balance_q15 > 0 &&
        balance_q15 <= (uint32_t)CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_VERY_STRONG_BALANCE_Q15 &&
        (dual.level.avg_abs >= (uint32_t)CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_VERY_STRONG_DIFF_AVG_ABS ||
         dual.level.peak >= (uint32_t)CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_VERY_STRONG_DIFF_PEAK);
    if (!ref_active) {
        aec->dual_restore_no_ref_guard_chunks++;
        return false;
    }
    /* A weak preserve hint is intentionally excluded here. Far-end playback can
     * produce short weak-hint runs from room asymmetry; restoring the dual-mic
     * difference for those frames reintroduces the device voice after AEC. */
    if (!hinted_strong_nearend && !spatial_nearend && !very_strong_nearend) {
        aec->dual_restore_spatial_guard_chunks++;
        return false;
    }
    const bool high_ref_corr =
        mic_ref_metrics.corr_q15 >= (uint32_t)CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_HIGH_CORR_GUARD_Q15;
    if (high_ref_corr && !very_strong_nearend) {
        const bool strong_spatial_evidence =
            dual.level.avg_abs >= (uint32_t)CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_SPATIAL_DIFF_AVG_ABS &&
            dual.level.peak >= (uint32_t)CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_SPATIAL_DIFF_PEAK;
        if (!hinted_strong_nearend || !strong_spatial_evidence) {
            aec->dual_restore_corr_guard_chunks++;
            return false;
        }
    }
    if (score < (uint32_t)CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_MIN_SCORE_Q15) {
        aec->dual_restore_guard_chunks++;
        return false;
    }

    uint32_t gain_q15 = (uint32_t)CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_WEAK_GAIN_Q15;
    if (score >= 28000U) {
        gain_q15 = (uint32_t)CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_STRONG_GAIN_Q15;
    } else if (score >= 21000U) {
        gain_q15 = (uint32_t)CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_MID_GAIN_Q15;
    }
    if (hinted_strong_nearend && gain_q15 < (uint32_t)CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_HINT_GAIN_Q15) {
        gain_q15 = (uint32_t)CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_HINT_GAIN_Q15;
    }
    if (high_ref_corr && !very_strong_nearend &&
        gain_q15 > (uint32_t)CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_WEAK_GAIN_Q15) {
        gain_q15 = (uint32_t)CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_WEAK_GAIN_Q15;
    }
    if (gain_q15 > 32768U) {
        gain_q15 = 32768U;
    }

    for (size_t i = 0; i < sample_count; i++) {
        const int32_t s1 = aec->pending_mic[i];
        const int32_t s2 = aec->pending_mic2[i];
        int32_t near = s1 - (int32_t)(((int64_t)dual.mic2_to_mic1_gain_q15 * (int64_t)s2) >> 15);
        near = clamp_i32(near, INT16_MIN, INT16_MAX);
        if (dual.sign < 0) {
            near = -near;
        }
        const int32_t add = (int32_t)(((int64_t)near * (int64_t)gain_q15) >> 15);
        const int32_t mixed = (int32_t)out[i] + add;
        const int16_t clamped = clamp_i16(mixed);
        if ((int32_t)clamped != mixed) {
            aec->clipped_samples++;
        }
        out[i] = clamped;
    }
    aec->dual_restore_chunks++;
    aec->dual_restore_gain_sum_q15 += gain_q15;
    if (hinted_nearend || hinted_strong_nearend) {
        aec->dual_restore_hint_chunks++;
    }
    return true;
#else
    (void)aec;
    (void)out;
    (void)sample_count;
    (void)mic_ref_metrics;
    (void)ref_active;
    return false;
#endif
}

static bool apply_nearend_floor_restore(nuwax_live_aec_t *aec,
                                        int16_t *out,
                                        size_t sample_count,
                                        residual_metrics_t mic_ref_metrics) {
#if CONFIG_NUWAX_LIVE_AEC_NEAREND_FLOOR_ENABLE
    if (!aec || !out || !aec->pending_mic || !aec->pending_mic2 || !aec->pending_dual_mic ||
        sample_count == 0) {
        return false;
    }
    const nuwax_mic_level_t mic_level = nuwax_mic_level_measure(aec->pending_mic, sample_count);
    if (mic_level.avg_abs < (uint32_t)CONFIG_NUWAX_LIVE_AEC_NEAREND_FLOOR_MIN_MIC_AVG_ABS ||
        mic_level.peak < (uint32_t)CONFIG_NUWAX_LIVE_AEC_NEAREND_FLOOR_MIN_MIC_PEAK) {
        aec->nearend_floor_guard_chunks++;
        return false;
    }

    const nuwax_mic_level_t mic2_level = nuwax_mic_level_measure(aec->pending_mic2, sample_count);
    const nuwax_mic_level_t out_level = nuwax_mic_level_measure(out, sample_count);
    const dual_restore_metrics_t dual =
        compute_dual_restore_metrics(aec->pending_mic, aec->pending_mic2, sample_count);
    if (dual.level.avg_abs > aec->nearend_floor_diff_avg_max) {
        aec->nearend_floor_diff_avg_max = dual.level.avg_abs;
    }
    const uint32_t balance_q15 = dual_level_balance_q15(mic_level, mic2_level);
    if (balance_q15 > 0 &&
        (aec->nearend_floor_balance_min_q15 == 0 || balance_q15 < aec->nearend_floor_balance_min_q15)) {
        aec->nearend_floor_balance_min_q15 = balance_q15;
    }

    const bool strict_spatial =
        balance_q15 > 0 &&
        balance_q15 <= (uint32_t)CONFIG_NUWAX_LIVE_AEC_NEAREND_FLOOR_STRICT_BALANCE_Q15 &&
        dual.level.avg_abs >= (uint32_t)CONFIG_NUWAX_LIVE_AEC_NEAREND_FLOOR_MIN_DIFF_AVG_ABS &&
        dual.level.peak >= (uint32_t)CONFIG_NUWAX_LIVE_AEC_NEAREND_FLOOR_MIN_DIFF_PEAK;
    const bool very_strong_nearend =
        balance_q15 > 0 &&
        balance_q15 <= (uint32_t)CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_VERY_STRONG_BALANCE_Q15 &&
        (dual.level.avg_abs >= (uint32_t)CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_VERY_STRONG_DIFF_AVG_ABS ||
         dual.level.peak >= (uint32_t)CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_VERY_STRONG_DIFF_PEAK);
    if (!aec->nearend_preserve_hint && !strict_spatial && !very_strong_nearend) {
        aec->nearend_floor_guard_chunks++;
        return false;
    }
    if (mic_ref_metrics.corr_q15 >= (uint32_t)CONFIG_NUWAX_LIVE_AEC_NEAREND_FLOOR_REF_GUARD_CORR_Q15 &&
        !very_strong_nearend) {
        aec->nearend_floor_ref_guard_chunks++;
        return false;
    }

    const uint32_t target_q15 = (uint32_t)CONFIG_NUWAX_LIVE_AEC_NEAREND_FLOOR_TARGET_Q15;
    const uint32_t target_avg = (uint32_t)(((uint64_t)mic_level.avg_abs * target_q15) >> 15);
    const uint32_t target_peak = (uint32_t)(((uint64_t)mic_level.peak * target_q15) >> 15);
    if ((out_level.avg_abs >= target_avg || target_avg == 0) &&
        (out_level.peak >= target_peak || target_peak == 0)) {
        aec->nearend_floor_guard_chunks++;
        return false;
    }
    if (dual.level.avg_abs == 0 && dual.level.peak == 0) {
        aec->nearend_floor_guard_chunks++;
        return false;
    }

    uint32_t gain_q15 = 0;
    if (target_avg > out_level.avg_abs && dual.level.avg_abs > 0) {
        gain_q15 = (uint32_t)((((uint64_t)(target_avg - out_level.avg_abs)) * 32768ULL) /
                              (uint64_t)dual.level.avg_abs);
    }
    if (target_peak > out_level.peak && dual.level.peak > 0) {
        const uint32_t peak_gain_q15 =
            (uint32_t)((((uint64_t)(target_peak - out_level.peak)) * 32768ULL) /
                       (uint64_t)dual.level.peak);
        if (peak_gain_q15 > gain_q15) {
            gain_q15 = peak_gain_q15;
        }
    }
    const uint32_t max_gain_q15 =
        (aec->strong_nearend_preserve_hint || very_strong_nearend)
            ? (uint32_t)CONFIG_NUWAX_LIVE_AEC_NEAREND_FLOOR_MAX_GAIN_Q15
            : ((uint32_t)CONFIG_NUWAX_LIVE_AEC_NEAREND_FLOOR_MAX_GAIN_Q15 / 2U);
    if (gain_q15 > max_gain_q15) {
        gain_q15 = max_gain_q15;
    }
    if (gain_q15 == 0) {
        aec->nearend_floor_guard_chunks++;
        return false;
    }

    for (size_t i = 0; i < sample_count; i++) {
        const int32_t s1 = aec->pending_mic[i];
        const int32_t s2 = aec->pending_mic2[i];
        int32_t near = s1 - (int32_t)(((int64_t)dual.mic2_to_mic1_gain_q15 * (int64_t)s2) >> 15);
        near = clamp_i32(near, INT16_MIN, INT16_MAX);
        if (dual.sign < 0) {
            near = -near;
        }
        const int32_t add = (int32_t)(((int64_t)near * (int64_t)gain_q15) >> 15);
        const int32_t mixed = (int32_t)out[i] + add;
        const int16_t clamped = clamp_i16(mixed);
        if ((int32_t)clamped != mixed) {
            aec->clipped_samples++;
        }
        out[i] = clamped;
    }
    aec->nearend_floor_restore_chunks++;
    aec->nearend_floor_gain_sum_q15 += gain_q15;
    return true;
#else
    (void)aec;
    (void)out;
    (void)sample_count;
    (void)mic_ref_metrics;
    return false;
#endif
}

static void apply_nearend_blend(nuwax_live_aec_t *aec,
                                const int16_t *mic,
                                int16_t *out,
                                size_t sample_count,
                                nuwax_mic_level_t mic_level,
                                nuwax_mic_level_t out_level,
                                residual_metrics_t mic_ref_metrics) {
    if (!aec || !mic || !out || sample_count == 0) {
        return;
    }
#if CONFIG_NUWAX_LIVE_AEC_NEAREND_RAW_MIX_Q15 == 0 && \
    CONFIG_NUWAX_LIVE_AEC_STRONG_NEAREND_RAW_MIX_Q15 == 0 && \
    CONFIG_NUWAX_LIVE_AEC_HINT_STRONG_RAW_MIX_Q15 == 0
    if (aec->nearend_preserve_hint) {
        aec->nearend_hint_chunks++;
    }
    if (aec->strong_nearend_preserve_hint) {
        aec->strong_nearend_hint_chunks++;
    }
    (void)mic_level;
    (void)out_level;
    (void)mic_ref_metrics;
    return;
#endif
    const bool hinted_nearend = aec->nearend_preserve_hint;
    const bool hinted_strong_nearend = aec->strong_nearend_preserve_hint;
    const bool level_nearend = nearend_blend_active(mic_level);
    const bool strong_nearend = hinted_strong_nearend || strong_nearend_active(mic_level);
    const bool dual_evidence = nearend_blend_dual_evidence(aec, sample_count);
    if (!level_nearend && !hinted_nearend) {
        return;
    }
    if (!hinted_nearend && !dual_evidence) {
        aec->nearend_blend_dual_guard_chunks++;
        return;
    }
    if (hinted_nearend) {
        aec->nearend_hint_chunks++;
    }
    if (hinted_strong_nearend) {
        aec->strong_nearend_hint_chunks++;
    }
    if (!nearend_blend_aec_output_evidence(out_level) && !strong_nearend) {
        aec->nearend_blend_output_guard_chunks++;
        return;
    } else if (!nearend_blend_aec_output_evidence(out_level)) {
        aec->nearend_hint_output_guard_bypass_chunks++;
    }
    const uint32_t min_out_ratio_q15 = (uint32_t)CONFIG_NUWAX_LIVE_AEC_NEAREND_BLEND_MIN_OUT_RATIO_Q15;
    if (!strong_nearend &&
        min_out_ratio_q15 > 0 &&
        ((uint64_t)out_level.avg_abs * 32768ULL < (uint64_t)mic_level.avg_abs * min_out_ratio_q15) &&
        ((uint64_t)out_level.peak * 32768ULL < (uint64_t)mic_level.peak * min_out_ratio_q15)) {
        aec->nearend_blend_output_guard_chunks++;
        return;
    }
    uint32_t raw_mix = hinted_strong_nearend
                           ? (uint32_t)CONFIG_NUWAX_LIVE_AEC_HINT_STRONG_RAW_MIX_Q15
                           : (strong_nearend ? (uint32_t)CONFIG_NUWAX_LIVE_AEC_STRONG_NEAREND_RAW_MIX_Q15
                                             : (uint32_t)CONFIG_NUWAX_LIVE_AEC_NEAREND_RAW_MIX_Q15);
    if (hinted_strong_nearend && !dual_evidence &&
        raw_mix > (uint32_t)CONFIG_NUWAX_LIVE_AEC_STRONG_NEAREND_RAW_MIX_Q15) {
        raw_mix = (uint32_t)CONFIG_NUWAX_LIVE_AEC_STRONG_NEAREND_RAW_MIX_Q15;
    }
    if (raw_mix > 32768U) {
        raw_mix = 32768U;
    }
    if (raw_mix == 0) {
        return;
    }
    const uint32_t guard_corr = strong_nearend
                                    ? (uint32_t)CONFIG_NUWAX_LIVE_AEC_STRONG_NEAREND_BLEND_GUARD_CORR_Q15
                                    : (uint32_t)CONFIG_NUWAX_LIVE_AEC_NEAREND_BLEND_GUARD_CORR_Q15;
    const uint32_t full_corr = (uint32_t)CONFIG_NUWAX_LIVE_AEC_NEAREND_BLEND_FULL_CORR_Q15;
    if (mic_ref_metrics.corr_q15 >= guard_corr) {
        aec->nearend_blend_ref_guard_chunks++;
        return;
    }
    if (guard_corr > full_corr && mic_ref_metrics.corr_q15 > full_corr) {
        raw_mix = (uint32_t)(((uint64_t)raw_mix * (uint64_t)(guard_corr - mic_ref_metrics.corr_q15)) /
                             (uint64_t)(guard_corr - full_corr));
        if (raw_mix == 0) {
            aec->nearend_blend_ref_guard_chunks++;
            return;
        }
    }
    const int32_t aec_mix = (int32_t)(32768U - raw_mix);
    for (size_t i = 0; i < sample_count; i++) {
        const int32_t mixed = (int32_t)((((int64_t)out[i] * aec_mix) +
                                        ((int64_t)mic[i] * (int32_t)raw_mix)) >> 15);
        out[i] = clamp_i16(mixed);
    }
    aec->nearend_blend_chunks++;
    if (strong_nearend) {
        aec->strong_nearend_blend_chunks++;
    }
}

static residual_metrics_t compute_residual_metrics(const int16_t *samples,
                                                    const int16_t *ref,
                                                    size_t sample_count,
                                                    int32_t max_gain_q15) {
    residual_metrics_t metrics = {0};
    uint64_t out_sumsq = 0;
    uint64_t ref_sumsq = 0;
    int64_t dot = 0;
    for (size_t i = 0; i < sample_count; i++) {
        const int32_t out = samples[i];
        const int32_t r = ref[i];
        out_sumsq += (uint64_t)((int64_t)out * (int64_t)out);
        ref_sumsq += (uint64_t)((int64_t)r * (int64_t)r);
        dot += (int64_t)out * (int64_t)r;
    }
    if (out_sumsq == 0 || ref_sumsq == 0) {
        return metrics;
    }

    const double denom = sqrt((double)out_sumsq * (double)ref_sumsq);
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
    const double max_gain = (double)max_gain_q15 / 32768.0;
    if (gain > max_gain) {
        gain = max_gain;
    } else if (gain < -max_gain) {
        gain = -max_gain;
    }
    metrics.gain_q15 = (int32_t)(gain * 32768.0);
    return metrics;
}

static uint32_t ratio_q15_unclamped(uint64_t num, uint64_t den) {
    if (den == 0) {
        return num == 0 ? 0 : UINT32_MAX;
    }
    const double ratio = ((double)num * 32768.0) / (double)den;
    return ratio >= (double)UINT32_MAX ? UINT32_MAX : (uint32_t)(ratio + 0.5);
}

static uint32_t saturating_add_u32(uint32_t value, uint32_t increment) {
    return increment > UINT32_MAX - value ? UINT32_MAX : value + increment;
}

static uint32_t correlation_q15_from_energy(int64_t dot,
                                            uint64_t first_energy,
                                            uint64_t second_energy) {
    if (first_energy == 0 || second_energy == 0) {
        return 0;
    }
    const double denom = sqrt((double)first_energy * (double)second_energy);
    if (denom <= 0.0) {
        return 0;
    }
    double corr = fabs((double)dot) / denom;
    if (corr > 1.0) {
        corr = 1.0;
    }
    uint32_t corr_q15 = (uint32_t)(corr * 32768.0 + 0.5);
    return corr_q15 > 32767U ? 32767U : corr_q15;
}

static void reset_human_rescue_fir_training_health(nuwax_live_aec_t *aec) {
    if (!aec) {
        return;
    }
    aec->human_rescue_fir_session_training_samples = 0;
    aec->human_rescue_fir_bad_health_windows = 0;
    aec->human_rescue_fir_training_raw_energy = 0;
    aec->human_rescue_fir_training_residual_energy = 0;
    aec->human_rescue_fir_training_scalar_energy = 0;
    aec->human_rescue_fir_training_prediction_energy = 0;
    aec->human_rescue_fir_training_prediction_mic_dot = 0;
    aec->human_rescue_fir_residual_raw_ema_q15 = 0;
    aec->human_rescue_fir_residual_scalar_ema_q15 = 0;
    aec->human_rescue_fir_prediction_corr_q15 = 0;
}

static void reset_human_rescue_fir_validation(nuwax_live_aec_t *aec) {
    if (!aec) {
        return;
    }
    aec->human_rescue_fir_validating = false;
    aec->human_rescue_fir_validation_remaining = 0;
    aec->human_rescue_fir_validation_scalar_gain_q15 = 0;
    aec->human_rescue_fir_validation_raw_energy = 0;
    aec->human_rescue_fir_validation_residual_energy = 0;
    aec->human_rescue_fir_validation_scalar_energy = 0;
    aec->human_rescue_fir_validation_prediction_energy = 0;
    aec->human_rescue_fir_validation_prediction_mic_dot = 0;
}

static void reset_human_rescue_fir_ready_health_window(nuwax_live_aec_t *aec) {
    if (!aec) {
        return;
    }
    aec->human_rescue_fir_ready_health_chunks = 0;
    aec->human_rescue_fir_ready_health_hint_seen = false;
    aec->human_rescue_fir_ready_health_raw_energy = 0;
    aec->human_rescue_fir_ready_health_residual_energy = 0;
    aec->human_rescue_fir_ready_health_scalar_energy = 0;
}

static void update_human_rescue_fir_coefficient_metrics(nuwax_live_aec_t *aec) {
    if (!aec || !aec->human_rescue_fir_coeffs) {
        return;
    }
    const size_t taps = (size_t)CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_TAPS;
    const size_t edge_taps = taps / 2U < NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_COEFF_EDGE_TAPS
                                 ? taps / 2U
                                 : NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_COEFF_EDGE_TAPS;
    float coeff_peak = 0.0f;
    size_t coeff_peak_tap = 0;
    double coeff_energy = 0.0;
    double coeff_edge_energy = 0.0;
    for (size_t tap = 0; tap < taps; tap++) {
        const float magnitude = fabsf(aec->human_rescue_fir_coeffs[tap]);
        if (magnitude > coeff_peak) {
            coeff_peak = magnitude;
            coeff_peak_tap = tap;
        }
        const double tap_energy = (double)aec->human_rescue_fir_coeffs[tap] *
                                  (double)aec->human_rescue_fir_coeffs[tap];
        coeff_energy += tap_energy;
        if (tap < edge_taps || tap + edge_taps >= taps) {
            coeff_edge_energy += tap_energy;
        }
    }
    const double coeff_peak_q15 = (double)coeff_peak * 32768.0;
    aec->human_rescue_fir_coeff_peak_q15 =
        coeff_peak_q15 >= (double)UINT32_MAX ? UINT32_MAX : (uint32_t)(coeff_peak_q15 + 0.5);
    aec->human_rescue_fir_coeff_peak_tap = (uint32_t)coeff_peak_tap;
    aec->human_rescue_fir_edge_energy_q15 =
        coeff_energy > 0.0
            ? (uint32_t)fmin(32768.0, (coeff_edge_energy * 32768.0) / coeff_energy + 0.5)
            : 0;
}

static bool human_rescue_fir_coefficient_shape_valid(const nuwax_live_aec_t *aec) {
    if (!aec || aec->human_rescue_fir_coeff_peak_q15 == 0) {
        return false;
    }
    const uint32_t taps = (uint32_t)CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_TAPS;
    return aec->human_rescue_fir_coeff_peak_tap >=
               NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_MIN_PEAK_TAP &&
           aec->human_rescue_fir_coeff_peak_tap +
                   NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_MIN_PEAK_TAP <
               taps &&
           aec->human_rescue_fir_edge_energy_q15 <=
               NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_MAX_EDGE_ENERGY_Q15;
}

static void reset_human_rescue_fir_session(nuwax_live_aec_t *aec) {
    if (!aec) {
        return;
    }
    const size_t taps = (size_t)CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_TAPS;
    if (aec->human_rescue_fir_coeffs) {
        memset(aec->human_rescue_fir_coeffs, 0, taps * sizeof(aec->human_rescue_fir_coeffs[0]));
    }
    if (aec->human_rescue_fir_ref_history) {
        memset(aec->human_rescue_fir_ref_history,
               0,
               taps * 2U * sizeof(aec->human_rescue_fir_ref_history[0]));
    }
    aec->human_rescue_fir_initialized = false;
    aec->human_rescue_fir_ready = false;
    reset_human_rescue_fir_validation(aec);
    aec->human_rescue_fir_frozen = false;
    aec->human_rescue_fir_history_pos = 0;
    aec->human_rescue_fir_ref_energy = 0.0f;
    reset_human_rescue_fir_training_health(aec);
    aec->human_rescue_fir_ready_bad_windows = 0;
    aec->human_rescue_fir_nearend_hold_chunks = 0;
    reset_human_rescue_fir_ready_health_window(aec);
    aec->human_rescue_source_count = 0;
    aec->human_rescue_source_valid = false;
    aec->human_rescue_source_fir = false;
    aec->human_rescue_source_kind = NUWAX_LIVE_AEC_RESCUE_SOURCE_RAW;
}

void nuwax_live_aec_reset_reference_epoch(nuwax_live_aec_t *aec) {
#if CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_ENABLE
    if (!aec) {
        return;
    }
    reset_human_rescue_fir_session(aec);
    aec->human_rescue_fir_warmup_samples = 0;
    aec->human_rescue_fir_reference_epoch_resets++;
#else
    (void)aec;
#endif
}

bool nuwax_live_aec_reanchor_reference(nuwax_live_aec_t *aec) {
#if CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_ENABLE
    if (!aec) {
        return false;
    }
    const bool preserve_ready_model = aec->human_rescue_fir_ready;
    if (!preserve_ready_model) {
        reset_human_rescue_fir_session(aec);
        aec->human_rescue_fir_warmup_samples = 0;
        aec->human_rescue_fir_reference_reanchors++;
        aec->human_rescue_fir_reference_retrain_reanchors++;
        return false;
    }
    const size_t taps = (size_t)CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_TAPS;
    if (aec->human_rescue_fir_ref_history) {
        memset(aec->human_rescue_fir_ref_history,
               0,
               taps * 2U * sizeof(aec->human_rescue_fir_ref_history[0]));
    }
    aec->human_rescue_fir_history_pos = 0;
    aec->human_rescue_fir_ref_energy = 0.0f;
    reset_human_rescue_fir_validation(aec);
    aec->human_rescue_fir_ready_bad_windows = 0;
    reset_human_rescue_fir_ready_health_window(aec);
    aec->human_rescue_source_count = 0;
    aec->human_rescue_source_valid = false;
    aec->human_rescue_source_fir = false;
    aec->human_rescue_source_kind = NUWAX_LIVE_AEC_RESCUE_SOURCE_RAW;
    aec->human_rescue_fir_reference_reanchors++;
    return true;
#else
    (void)aec;
    return false;
#endif
}

static void initialize_human_rescue_fir(nuwax_live_aec_t *aec) {
#if CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_ENABLE
    if (!aec || !aec->human_rescue_fir_available || aec->human_rescue_fir_initialized) {
        return;
    }
    const size_t taps = (size_t)CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_TAPS;
    memset(aec->human_rescue_fir_coeffs, 0, taps * sizeof(aec->human_rescue_fir_coeffs[0]));
    memset(aec->human_rescue_fir_ref_history,
           0,
           taps * 2U * sizeof(aec->human_rescue_fir_ref_history[0]));
    if (aec->human_rescue_ref_gain_valid) {
        const size_t seed_tap =
            (size_t)CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_SEED_TAP < taps
                ? (size_t)CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_SEED_TAP
                : 0U;
        aec->human_rescue_fir_coeffs[seed_tap] =
            (float)aec->human_rescue_ref_gain_q15 / 32768.0f;
    }
    aec->human_rescue_fir_history_pos = 0;
    aec->human_rescue_fir_ref_energy = 0.0f;
    aec->human_rescue_fir_initialized = true;
#else
    (void)aec;
#endif
}

static void build_scalar_human_rescue_source(nuwax_live_aec_t *aec,
                                             const int16_t *ref,
                                             size_t sample_count) {
    if (!aec || !aec->human_rescue_source_chunk || !aec->pending_mic || sample_count == 0) {
        return;
    }
    const int32_t ref_gain_q15 =
        (ref && aec->human_rescue_ref_gain_valid) ? aec->human_rescue_ref_gain_q15 : 0;
    for (size_t i = 0; i < sample_count; i++) {
        const int32_t cancel = ref
                                   ? (int32_t)(((int64_t)ref_gain_q15 * (int64_t)ref[i]) >> 15)
                                   : 0;
        aec->human_rescue_source_chunk[i] =
            clamp_i16((int32_t)aec->pending_mic[i] - cancel);
    }
    aec->human_rescue_source_count = sample_count;
    aec->human_rescue_source_valid = true;
    aec->human_rescue_source_fir = false;
    aec->human_rescue_source_kind = ref_gain_q15 != 0
                                        ? NUWAX_LIVE_AEC_RESCUE_SOURCE_SCALAR
                                        : NUWAX_LIVE_AEC_RESCUE_SOURCE_RAW;
}

static void advance_human_rescue_fir_reference(nuwax_live_aec_t *aec,
                                               const int16_t *ref,
                                               size_t sample_count) {
#if CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_ENABLE
    if (!aec || !ref || !aec->human_rescue_fir_initialized ||
        !aec->human_rescue_fir_ref_history || sample_count == 0) {
        return;
    }
    const size_t taps = (size_t)CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_TAPS;
    size_t pos = aec->human_rescue_fir_history_pos;
    float energy = aec->human_rescue_fir_ref_energy;
    for (size_t i = 0; i < sample_count; i++) {
        const float x = (float)ref[i] / 32768.0f;
        const float old = aec->human_rescue_fir_ref_history[pos];
        energy += (x * x) - (old * old);
        if (energy < 0.0f) {
            energy = 0.0f;
        }
        aec->human_rescue_fir_ref_history[pos] = x;
        aec->human_rescue_fir_ref_history[pos + taps] = x;
        pos++;
        if (pos == taps) {
            pos = 0;
        }
    }
    aec->human_rescue_fir_history_pos = (uint32_t)pos;
    aec->human_rescue_fir_ref_energy = energy;
#else
    (void)aec;
    (void)ref;
    (void)sample_count;
#endif
}

static void prepare_human_rescue_source(nuwax_live_aec_t *aec,
                                        const int16_t *scalar_ref,
                                        const int16_t *fir_ref,
                                        size_t sample_count,
                                        residual_metrics_t aligned_ref_metrics) {
    if (!aec || !scalar_ref || !fir_ref || !aec->pending_mic || sample_count == 0) {
        return;
    }
    build_scalar_human_rescue_source(aec, scalar_ref, sample_count);
#if CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_ENABLE
    if (!aec->human_rescue_fir_available || !aec->human_rescue_source_chunk) {
        return;
    }
    initialize_human_rescue_fir(aec);
    if (!aec->human_rescue_fir_initialized) {
        return;
    }

    const nuwax_mic_level_t training_ref_level = nuwax_mic_level_measure(fir_ref, sample_count);
    const residual_metrics_t fir_ref_metrics =
        compute_residual_metrics(aec->pending_mic,
                                 fir_ref,
                                 sample_count,
                                 CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_MAX_REF_GAIN_Q15);
    aec->human_rescue_fir_last_ref_corr_q15 = fir_ref_metrics.corr_q15;
    aec->human_rescue_fir_last_gate_corr_q15 = aligned_ref_metrics.corr_q15;
    const bool enough_training_excitation =
        training_ref_level.avg_abs >= NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_TRAIN_MIN_REF_AVG_ABS ||
        training_ref_level.peak >= NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_TRAIN_MIN_REF_PEAK;
    if (enough_training_excitation) {
        aec->human_rescue_fir_warmup_samples =
            saturating_add_u32(aec->human_rescue_fir_warmup_samples, (uint32_t)sample_count);
        if (aec->human_rescue_fir_warmup_samples >
            (uint32_t)CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_WARMUP_SAMPLES) {
            aec->human_rescue_fir_warmup_samples =
                (uint32_t)CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_WARMUP_SAMPLES;
        }
    }
    const bool reference_warmup_complete =
        aec->human_rescue_fir_warmup_samples >=
        (uint32_t)CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_WARMUP_SAMPLES;
    const bool hard_human_protection = aec->provisional_human_protect ||
                                       aec->fast_human_protect ||
                                       aec->confirmed_human_protect ||
                                       aec->nearend_preserve_hint;
    const bool rescue_release_hangover = !hard_human_protection &&
                                         aec->human_rescue_mix_q15 > 0;
    const bool freeze_coefficients = hard_human_protection || rescue_release_hangover;
    /* The main AEC reference is intentionally about 30 ms ahead of this FIR
     * reference, so its zero-lag correlation is not a valid training gate. */
    const bool strict_far_only = reference_warmup_complete &&
                                 !freeze_coefficients && enough_training_excitation;
    const bool validation_block = aec->human_rescue_fir_validating && strict_far_only;
    const bool ready_health_block = aec->human_rescue_fir_ready && enough_training_excitation;
    const bool allow_adaptation = strict_far_only &&
                                  !aec->human_rescue_fir_validating &&
                                  !aec->human_rescue_fir_ready;
    if (freeze_coefficients) {
        if (!aec->human_rescue_fir_frozen &&
            aec->human_rescue_fir_session_training_samples > 0) {
            aec->human_rescue_fir_freeze_events++;
        }
        aec->human_rescue_fir_frozen = true;
        aec->human_rescue_fir_frozen_chunks++;
    } else if (enough_training_excitation) {
        aec->human_rescue_fir_frozen = false;
    }
    if (aec->human_rescue_fir_ready && !enough_training_excitation &&
        aec->human_rescue_fir_ready_health_chunks > 0) {
        aec->human_rescue_fir_health_boundary_resets++;
        aec->human_rescue_fir_ready_bad_windows = 0;
        reset_human_rescue_fir_ready_health_window(aec);
    }

    const size_t taps = (size_t)CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_TAPS;
    const float mu = (float)CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_STEP_Q15 / 32768.0f;
    size_t pos = aec->human_rescue_fir_history_pos;
    float ref_energy = aec->human_rescue_fir_ref_energy;
    uint64_t raw_energy = 0;
    uint64_t fir_residual_energy = 0;
    uint64_t scalar_residual_energy = 0;
    uint64_t validation_scalar_residual_energy = 0;
    uint64_t prediction_energy = 0;
    int64_t prediction_mic_dot = 0;
    uint32_t adapted_samples = 0;
    bool diverged = false;
    const int32_t scalar_gain_q15 =
        aec->human_rescue_ref_gain_valid ? aec->human_rescue_ref_gain_q15 : 0;

    for (size_t i = 0; i < sample_count; i++) {
        const float x = (float)fir_ref[i] / 32768.0f;
        const float old = aec->human_rescue_fir_ref_history[pos];
        ref_energy += (x * x) - (old * old);
        if (ref_energy < 0.0f) {
            ref_energy = 0.0f;
        }
        aec->human_rescue_fir_ref_history[pos] = x;
        aec->human_rescue_fir_ref_history[pos + taps] = x;
        pos++;
        if (pos == taps) {
            pos = 0;
        }

        float estimated_echo = 0.0f;
        const size_t newest_history_index = pos + taps - 1U;
        for (size_t tap = 0; tap < taps; tap++) {
            estimated_echo += aec->human_rescue_fir_coeffs[tap] *
                              aec->human_rescue_fir_ref_history[newest_history_index - tap];
        }
        const float desired = (float)aec->pending_mic[i] / 32768.0f;
        const float error = desired - estimated_echo;
        if (!isfinite(error) || !isfinite(estimated_echo) ||
            fabsf(estimated_echo) > NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_MAX_ESTIMATE) {
            diverged = true;
            break;
        }
        const float scaled_error = error * 32768.0f;
        const int32_t residual = (int32_t)(scaled_error >= 0.0f
                                               ? scaled_error + 0.5f
                                               : scaled_error - 0.5f);
        const int16_t residual_i16 = clamp_i16(residual);
        aec->human_rescue_source_chunk[i] = residual_i16;

        const int32_t raw = aec->pending_mic[i];
        const float scaled_estimate = estimated_echo * 32768.0f;
        const int32_t prediction = (int32_t)(scaled_estimate >= 0.0f
                                                 ? scaled_estimate + 0.5f
                                                 : scaled_estimate - 0.5f);
        const int32_t scalar_cancel =
            (int32_t)(((int64_t)scalar_gain_q15 * (int64_t)scalar_ref[i]) >> 15);
        const int32_t scalar_residual = raw - scalar_cancel;
        raw_energy += (uint64_t)((int64_t)raw * (int64_t)raw);
        fir_residual_energy +=
            (uint64_t)((int64_t)residual * (int64_t)residual);
        scalar_residual_energy +=
            (uint64_t)((int64_t)scalar_residual * (int64_t)scalar_residual);
        if (validation_block) {
            const int32_t validation_scalar_cancel =
                (int32_t)(((int64_t)aec->human_rescue_fir_validation_scalar_gain_q15 *
                           (int64_t)scalar_ref[i]) >>
                          15);
            const int32_t validation_scalar_residual = raw - validation_scalar_cancel;
            validation_scalar_residual_energy +=
                (uint64_t)((int64_t)validation_scalar_residual *
                           (int64_t)validation_scalar_residual);
        }
        prediction_energy +=
            (uint64_t)((int64_t)prediction * (int64_t)prediction);
        prediction_mic_dot += (int64_t)prediction * (int64_t)raw;

        if (allow_adaptation && ref_energy > NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_NORM_EPSILON) {
            const float step = (mu * error) /
                               (ref_energy + NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_NORM_EPSILON);
            for (size_t tap = 0; tap < taps; tap++) {
                const float coefficient =
                    aec->human_rescue_fir_coeffs[tap] +
                    step * aec->human_rescue_fir_ref_history[newest_history_index - tap];
                if (!isfinite(coefficient) ||
                    fabsf(coefficient) > NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_MAX_COEFF) {
                    diverged = true;
                    break;
                }
                aec->human_rescue_fir_coeffs[tap] = coefficient;
            }
            if (diverged) {
                break;
            }
            adapted_samples++;
        }
    }

    aec->human_rescue_fir_history_pos = (uint32_t)pos;
    aec->human_rescue_fir_ref_energy = ref_energy;
    if (!diverged && raw_energy > 0) {
        aec->human_rescue_fir_last_residual_raw_q15 =
            ratio_q15_unclamped(fir_residual_energy, raw_energy);
        aec->human_rescue_fir_last_residual_scalar_q15 =
            ratio_q15_unclamped(fir_residual_energy, scalar_residual_energy);
    }

    if (diverged) {
        aec->human_rescue_fir_divergences++;
        reset_human_rescue_fir_session(aec);
        build_scalar_human_rescue_source(aec, scalar_ref, sample_count);
        return;
    }

    if (adapted_samples > 0) {
        aec->human_rescue_fir_session_training_samples =
            saturating_add_u32(aec->human_rescue_fir_session_training_samples, adapted_samples);
        aec->human_rescue_fir_training_samples =
            saturating_add_u32(aec->human_rescue_fir_training_samples, adapted_samples);
        aec->human_rescue_fir_training_chunks++;
        aec->human_rescue_fir_training_raw_energy += raw_energy;
        aec->human_rescue_fir_training_residual_energy += fir_residual_energy;
        aec->human_rescue_fir_training_scalar_energy += scalar_residual_energy;
        aec->human_rescue_fir_training_prediction_energy += prediction_energy;
        aec->human_rescue_fir_training_prediction_mic_dot += prediction_mic_dot;
        aec->human_rescue_fir_residual_raw_ema_q15 =
            ratio_q15_unclamped(aec->human_rescue_fir_training_residual_energy,
                                aec->human_rescue_fir_training_raw_energy);
        aec->human_rescue_fir_residual_scalar_ema_q15 =
            ratio_q15_unclamped(aec->human_rescue_fir_training_residual_energy,
                                aec->human_rescue_fir_training_scalar_energy);
        aec->human_rescue_fir_prediction_corr_q15 =
            correlation_q15_from_energy(aec->human_rescue_fir_training_prediction_mic_dot,
                                        aec->human_rescue_fir_training_prediction_energy,
                                        aec->human_rescue_fir_training_raw_energy);
        if (aec->human_rescue_fir_session_training_samples >=
                (uint32_t)CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_MIN_TRAINING_SAMPLES &&
            aec->human_rescue_fir_residual_raw_ema_q15 >
                NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_DIVERGENCE_RATIO_Q15) {
            aec->human_rescue_fir_bad_health_windows++;
        } else {
            aec->human_rescue_fir_bad_health_windows = 0;
        }
        if (aec->human_rescue_fir_bad_health_windows >= 2U) {
            aec->human_rescue_fir_divergences++;
            reset_human_rescue_fir_session(aec);
            build_scalar_human_rescue_source(aec, scalar_ref, sample_count);
            return;
        }
    }

    update_human_rescue_fir_coefficient_metrics(aec);

    if (validation_block) {
        aec->human_rescue_fir_validation_raw_energy += raw_energy;
        aec->human_rescue_fir_validation_residual_energy += fir_residual_energy;
        aec->human_rescue_fir_validation_scalar_energy += validation_scalar_residual_energy;
        aec->human_rescue_fir_validation_prediction_energy += prediction_energy;
        aec->human_rescue_fir_validation_prediction_mic_dot += prediction_mic_dot;
        if (aec->human_rescue_fir_validation_remaining > 0) {
            aec->human_rescue_fir_validation_remaining--;
        }
        if (aec->human_rescue_fir_validation_remaining == 0) {
            const uint32_t validation_raw_q15 =
                ratio_q15_unclamped(aec->human_rescue_fir_validation_residual_energy,
                                    aec->human_rescue_fir_validation_raw_energy);
            const uint32_t validation_scalar_q15 =
                ratio_q15_unclamped(aec->human_rescue_fir_validation_residual_energy,
                                    aec->human_rescue_fir_validation_scalar_energy);
            const uint32_t validation_corr_q15 =
                correlation_q15_from_energy(
                    aec->human_rescue_fir_validation_prediction_mic_dot,
                    aec->human_rescue_fir_validation_prediction_energy,
                    aec->human_rescue_fir_validation_raw_energy);
            const bool coefficient_shape_valid =
                human_rescue_fir_coefficient_shape_valid(aec);
            const bool validation_passed =
                validation_raw_q15 <=
                    (uint32_t)CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_MAX_RESIDUAL_RAW_Q15 &&
                validation_scalar_q15 <=
                    (uint32_t)CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_MAX_RESIDUAL_SCALAR_Q15 &&
                validation_corr_q15 >=
                    NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_MIN_PREDICTION_CORR_Q15 &&
                coefficient_shape_valid;
            aec->human_rescue_fir_last_validation_raw_q15 = validation_raw_q15;
            aec->human_rescue_fir_last_validation_scalar_q15 = validation_scalar_q15;
            aec->human_rescue_fir_last_validation_prediction_corr_q15 = validation_corr_q15;
            aec->human_rescue_fir_last_validation_passed = validation_passed;
            ESP_LOGI(TAG,
                     "human rescue FIR validation passed=%d raw_q15=%lu scalar_q15=%lu prediction_corr_q15=%lu shape=%d coeff_peak=%lu/%lu edge_energy_q15=%lu gate_corr_q15=%lu fir_ref_corr_q15=%lu",
                     validation_passed ? 1 : 0,
                     (unsigned long)validation_raw_q15,
                     (unsigned long)validation_scalar_q15,
                     (unsigned long)validation_corr_q15,
                     coefficient_shape_valid ? 1 : 0,
                     (unsigned long)aec->human_rescue_fir_coeff_peak_q15,
                     (unsigned long)aec->human_rescue_fir_coeff_peak_tap,
                     (unsigned long)aec->human_rescue_fir_edge_energy_q15,
                     (unsigned long)aligned_ref_metrics.corr_q15,
                     (unsigned long)fir_ref_metrics.corr_q15);
            aec->human_rescue_fir_residual_raw_ema_q15 = validation_raw_q15;
            aec->human_rescue_fir_residual_scalar_ema_q15 = validation_scalar_q15;
            aec->human_rescue_fir_prediction_corr_q15 = validation_corr_q15;
            reset_human_rescue_fir_validation(aec);
            if (validation_passed) {
                aec->human_rescue_fir_ready = true;
                aec->human_rescue_fir_ready_events++;
                aec->human_rescue_fir_validation_passes++;
                aec->human_rescue_fir_ready_bad_windows = 0;
                reset_human_rescue_fir_ready_health_window(aec);
            } else {
                aec->human_rescue_fir_validation_failures++;
                ESP_LOGW(TAG,
                         "human rescue FIR validation rejected action=%s raw_q15=%lu scalar_q15=%lu prediction_corr_q15=%lu shape=%d coeff_peak=%lu/%lu edge_energy_q15=%lu",
                         coefficient_shape_valid ? "continue_training" : "clear_retrain",
                         (unsigned long)validation_raw_q15,
                         (unsigned long)validation_scalar_q15,
                         (unsigned long)validation_corr_q15,
                         coefficient_shape_valid ? 1 : 0,
                         (unsigned long)aec->human_rescue_fir_coeff_peak_q15,
                         (unsigned long)aec->human_rescue_fir_coeff_peak_tap,
                         (unsigned long)aec->human_rescue_fir_edge_energy_q15);
                if (coefficient_shape_valid) {
                    reset_human_rescue_fir_training_health(aec);
                } else {
                    reset_human_rescue_fir_session(aec);
                }
                build_scalar_human_rescue_source(aec, scalar_ref, sample_count);
                return;
            }
        }
    }

    if (ready_health_block) {
        aec->human_rescue_fir_ready_health_hint_seen =
            aec->human_rescue_fir_ready_health_hint_seen || hard_human_protection ||
            aec->strong_nearend_preserve_hint;
        aec->human_rescue_fir_ready_health_raw_energy += raw_energy;
        aec->human_rescue_fir_ready_health_residual_energy += fir_residual_energy;
        aec->human_rescue_fir_ready_health_scalar_energy += scalar_residual_energy;
        aec->human_rescue_fir_ready_health_chunks++;
        if (aec->human_rescue_fir_ready_health_chunks >=
            NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_READY_HEALTH_CHUNKS) {
            const uint32_t ready_raw_q15 =
                ratio_q15_unclamped(aec->human_rescue_fir_ready_health_residual_energy,
                                    aec->human_rescue_fir_ready_health_raw_energy);
            const uint32_t ready_scalar_q15 =
                ratio_q15_unclamped(aec->human_rescue_fir_ready_health_residual_energy,
                                    aec->human_rescue_fir_ready_health_scalar_energy);
            const bool hinted_nearend = aec->human_rescue_fir_ready_health_hint_seen;
            const bool ready_health_bad =
                ready_raw_q15 > NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_READY_MAX_RAW_Q15 ||
                (hinted_nearend &&
                 ready_raw_q15 > NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_READY_HINT_RAW_Q15);
            aec->human_rescue_fir_last_ready_health_raw_q15 = ready_raw_q15;
            aec->human_rescue_fir_last_ready_health_scalar_q15 = ready_scalar_q15;
            aec->human_rescue_fir_ready_health_windows++;
            if (ready_health_bad) {
                aec->human_rescue_fir_ready_health_failures++;
                aec->human_rescue_fir_nearend_events++;
                aec->human_rescue_fir_ready_bad_windows = 0;
                aec->human_rescue_fir_nearend_hold_chunks =
                    NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_NEAREND_HOLD_CHUNKS;
                uint32_t protect_mix_q15 =
                    (uint32_t)CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_CONFIRMED_MIX_Q15;
                if (protect_mix_q15 > 32768U) {
                    protect_mix_q15 = 32768U;
                }
                if (aec->human_rescue_mix_q15 < protect_mix_q15) {
                    aec->human_rescue_mix_q15 = protect_mix_q15;
                }
                ESP_LOGW(TAG,
                         "human rescue FIR unexplained energy action=nearend_hold raw_q15=%lu scalar_q15=%lu hinted=%d hold_chunks=%lu coeff_peak=%lu/%lu edge_energy_q15=%lu gate_corr_q15=%lu fir_ref_corr_q15=%lu",
                         (unsigned long)ready_raw_q15,
                         (unsigned long)ready_scalar_q15,
                         hinted_nearend ? 1 : 0,
                         (unsigned long)aec->human_rescue_fir_nearend_hold_chunks,
                         (unsigned long)aec->human_rescue_fir_coeff_peak_q15,
                         (unsigned long)aec->human_rescue_fir_coeff_peak_tap,
                         (unsigned long)aec->human_rescue_fir_edge_energy_q15,
                         (unsigned long)aligned_ref_metrics.corr_q15,
                         (unsigned long)fir_ref_metrics.corr_q15);
            } else {
                aec->human_rescue_fir_ready_bad_windows = 0;
            }
            reset_human_rescue_fir_ready_health_window(aec);
        }
    }

    if (strict_far_only &&
        !aec->human_rescue_fir_ready && !aec->human_rescue_fir_validating &&
        aec->human_rescue_fir_session_training_samples >=
            (uint32_t)CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_MIN_TRAINING_SAMPLES) {
        reset_human_rescue_fir_validation(aec);
        aec->human_rescue_fir_validating = true;
        aec->human_rescue_fir_validation_remaining =
            NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_VALIDATION_CHUNKS;
        aec->human_rescue_fir_validation_scalar_gain_q15 =
            aec->human_rescue_ref_gain_q15;
        aec->human_rescue_fir_validation_starts++;
    }

    if (aec->human_rescue_fir_ready) {
        aec->human_rescue_source_count = sample_count;
        aec->human_rescue_source_valid = true;
        aec->human_rescue_source_fir = true;
        aec->human_rescue_source_kind = NUWAX_LIVE_AEC_RESCUE_SOURCE_FIR;
    } else {
        build_scalar_human_rescue_source(aec, scalar_ref, sample_count);
    }
#endif
}

static void update_human_rescue_mix(nuwax_live_aec_t *aec) {
    if (!aec) {
        return;
    }
#if CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_ENABLE
    const bool possible_nearend =
        aec->nearend_preserve_hint || aec->provisional_human_protect ||
        aec->fast_human_protect || aec->confirmed_human_protect;
    const bool trusted_fir_hold =
        aec->human_rescue_fir_ready && aec->human_rescue_fir_nearend_hold_chunks > 0;
    uint32_t target_q15 = 0;
    if (trusted_fir_hold) {
        target_q15 = (uint32_t)CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_CONFIRMED_MIX_Q15;
    } else if (aec->human_rescue_fir_ready && possible_nearend) {
        target_q15 = NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_PENDING_MIX_Q15;
    } else if (aec->confirmed_human_protect) {
        target_q15 = (uint32_t)CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_CONFIRMED_MIX_Q15;
    } else if (aec->fast_human_protect) {
        target_q15 = (uint32_t)CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FAST_MIX_Q15;
    } else if (aec->provisional_human_protect) {
        target_q15 = (uint32_t)CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_PROVISIONAL_MIX_Q15;
    }
    if (target_q15 > 32768U) {
        target_q15 = 32768U;
    }

    if (aec->human_rescue_mix_q15 < target_q15) {
        const uint32_t remaining = target_q15 - aec->human_rescue_mix_q15;
        aec->human_rescue_mix_q15 +=
            remaining < NUWAX_LIVE_AEC_HUMAN_RESCUE_ATTACK_STEP_Q15
                ? remaining
                : NUWAX_LIVE_AEC_HUMAN_RESCUE_ATTACK_STEP_Q15;
    } else if (aec->human_rescue_mix_q15 > target_q15) {
        const uint32_t remaining = aec->human_rescue_mix_q15 - target_q15;
        aec->human_rescue_mix_q15 -=
            remaining < NUWAX_LIVE_AEC_HUMAN_RESCUE_RELEASE_STEP_Q15
                ? remaining
                : NUWAX_LIVE_AEC_HUMAN_RESCUE_RELEASE_STEP_Q15;
    }
    if (aec->human_rescue_fir_nearend_hold_chunks > 0) {
        aec->human_rescue_fir_nearend_hold_chunks--;
    }
#else
    aec->human_rescue_mix_q15 = 0;
#endif
}

static void update_human_rescue_ref_gain(nuwax_live_aec_t *aec,
                                         residual_metrics_t projection) {
#if CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_ENABLE
    if (!aec || aec->nearend_preserve_hint || aec->strong_nearend_preserve_hint ||
        aec->provisional_human_protect || aec->fast_human_protect ||
        aec->confirmed_human_protect ||
        aec->human_rescue_mix_q15 > 0) {
        return;
    }
    if (projection.corr_q15 < (uint32_t)CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_MIN_REF_CORR_Q15 ||
        projection.gain_q15 == 0) {
        aec->human_rescue_ref_gain_rejects++;
        return;
    }
    if (!aec->human_rescue_ref_gain_valid) {
        aec->human_rescue_ref_gain_q15 = projection.gain_q15;
        aec->human_rescue_ref_gain_valid = true;
        aec->human_rescue_ref_gain_updates++;
        return;
    }

    int32_t delta = projection.gain_q15 - aec->human_rescue_ref_gain_q15;
    int32_t step = delta / (1 << NUWAX_LIVE_AEC_HUMAN_RESCUE_GAIN_SMOOTH_SHIFT);
    if (step > NUWAX_LIVE_AEC_HUMAN_RESCUE_GAIN_MAX_STEP_Q15) {
        step = NUWAX_LIVE_AEC_HUMAN_RESCUE_GAIN_MAX_STEP_Q15;
    } else if (step < -NUWAX_LIVE_AEC_HUMAN_RESCUE_GAIN_MAX_STEP_Q15) {
        step = -NUWAX_LIVE_AEC_HUMAN_RESCUE_GAIN_MAX_STEP_Q15;
    } else if (step == 0 && delta != 0) {
        step = delta > 0 ? 1 : -1;
    }
    aec->human_rescue_ref_gain_q15 += step;
    aec->human_rescue_ref_gain_updates++;
#else
    (void)aec;
    (void)projection;
#endif
}

static bool apply_human_rescue(nuwax_live_aec_t *aec,
                               int16_t *out,
                               const int16_t *ref,
                               size_t sample_count,
                               residual_metrics_t projection) {
#if CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_ENABLE
    if (!aec || !out || !ref || !aec->pending_mic || sample_count == 0 ||
        aec->human_rescue_mix_q15 == 0) {
        return false;
    }

    if (projection.corr_q15 > aec->human_rescue_ref_corr_max_q15) {
        aec->human_rescue_ref_corr_max_q15 = projection.corr_q15;
    }
    const int32_t ref_gain_q15 =
        aec->human_rescue_ref_gain_valid ? aec->human_rescue_ref_gain_q15 : 0;
    aec->last_human_rescue_ref_gain_q15 = ref_gain_q15;
    if (ref_gain_q15 == 0) {
        aec->human_rescue_no_ref_gain_chunks++;
    }

    const uint32_t rescue_mix_q15 = aec->human_rescue_mix_q15;
    const int32_t aec_mix_q15 = (int32_t)(32768U - rescue_mix_q15);
    const bool prepared_source = aec->human_rescue_source_valid &&
                                 aec->human_rescue_source_chunk &&
                                 aec->human_rescue_source_count >= sample_count;
    for (size_t i = 0; i < sample_count; i++) {
        int16_t rescue = 0;
        if (prepared_source) {
            rescue = aec->human_rescue_source_chunk[i];
        } else {
            const int32_t cancel =
                (int32_t)(((int64_t)ref_gain_q15 * (int64_t)ref[i]) >> 15);
            const int32_t projected = (int32_t)aec->pending_mic[i] - cancel;
            rescue = clamp_i16(projected);
            if ((int32_t)rescue != projected) {
                aec->clipped_samples++;
            }
        }
        const int32_t mixed =
            (int32_t)((((int64_t)out[i] * aec_mix_q15) +
                       ((int64_t)rescue * (int32_t)rescue_mix_q15)) >> 15);
        out[i] = clamp_i16(mixed);
    }

    aec->human_rescue_chunks++;
    aec->human_rescue_mix_sum_q15 += rescue_mix_q15;
    if (rescue_mix_q15 > aec->human_rescue_mix_max_q15) {
        aec->human_rescue_mix_max_q15 = rescue_mix_q15;
    }
    if (prepared_source && aec->human_rescue_source_fir) {
        aec->human_rescue_fir_rescue_chunks++;
    } else {
        aec->human_rescue_fir_fallback_chunks++;
    }
    if (aec->confirmed_human_protect) {
        aec->human_rescue_confirmed_chunks++;
    } else if (aec->fast_human_protect) {
        aec->human_rescue_fast_chunks++;
    } else if (aec->provisional_human_protect) {
        aec->human_rescue_provisional_chunks++;
    } else {
        aec->human_rescue_release_chunks++;
    }
    return true;
#else
    (void)aec;
    (void)out;
    (void)ref;
    (void)sample_count;
    (void)projection;
    return false;
#endif
}

static size_t read_adaptive_software_ref(nuwax_live_aec_t *aec, int16_t *out, size_t sample_count) {
    if (!aec || !out || sample_count == 0) {
        return 0;
    }
    const size_t base_copied =
        nuwax_p3_software_ref_read_delayed(out, sample_count, aec->delay_samples);
#if CONFIG_NUWAX_LIVE_AEC_ADAPTIVE_REF_ENABLE
    const uint32_t chunk_index =
        aec->external_ref_chunks > 0
            ? aec->external_ref_chunks
            : (aec->processed_chunks + aec->bypass_chunks +
               aec->ref_missing_attenuated_chunks + aec->ref_inactive_preserved_chunks +
               aec->ref_inactive_attenuated_chunks + 1U);
    if (NUWAX_LIVE_AEC_ADAPTIVE_REF_SCAN_INTERVAL_CHUNKS > 1U &&
        chunk_index > 1U &&
        ((chunk_index - 1U) % NUWAX_LIVE_AEC_ADAPTIVE_REF_SCAN_INTERVAL_CHUNKS) != 0U) {
        return base_copied;
    }
    if (!aec->pending_mic || !aec->probe_ref_chunk ||
        CONFIG_NUWAX_LIVE_AEC_ADAPTIVE_REF_STEP_SAMPLES <= 0) {
        return base_copied;
    }
    const residual_metrics_t base_metrics =
        compute_residual_metrics(aec->pending_mic, out, sample_count, 32768);
    aec->adaptive_ref_scans++;
    aec->adaptive_ref_last_base_corr_q15 = base_metrics.corr_q15;
    uint32_t best_corr = base_metrics.corr_q15;
    int32_t best_offset = 0;
    size_t best_copied = base_copied;
    bool best_is_probe = false;

    const int32_t span = (int32_t)CONFIG_NUWAX_LIVE_AEC_ADAPTIVE_REF_SPAN_SAMPLES;
    const int32_t step = (int32_t)CONFIG_NUWAX_LIVE_AEC_ADAPTIVE_REF_STEP_SAMPLES;
    const uint32_t nominal_delay = aec->adaptive_ref_nominal_delay_samples > 0
                                       ? aec->adaptive_ref_nominal_delay_samples
                                       : aec->delay_samples;
    const uint32_t min_delay = nominal_delay > (uint32_t)span
                                   ? nominal_delay - (uint32_t)span
                                   : 0;
    const uint32_t max_delay = nominal_delay <= UINT32_MAX - (uint32_t)span
                                   ? nominal_delay + (uint32_t)span
                                   : UINT32_MAX;
    for (int32_t offset = -span; offset <= span; offset += step) {
        if (offset == 0) {
            continue;
        }
        const int64_t candidate_delay = (int64_t)aec->delay_samples + (int64_t)offset;
        if (candidate_delay < (int64_t)min_delay || candidate_delay > (int64_t)max_delay) {
            continue;
        }
        aec->adaptive_ref_probe_attempts++;
        const size_t copied = nuwax_p3_software_ref_read_delayed(aec->probe_ref_chunk,
                                                                 sample_count,
                                                                 (uint32_t)candidate_delay);
        if (copied < (sample_count / 2U)) {
            aec->adaptive_ref_probe_copied_low++;
            continue;
        }
        if (!ref_level_active(aec->probe_ref_chunk, sample_count)) {
            aec->adaptive_ref_probe_ref_inactive++;
            continue;
        }
        aec->adaptive_ref_active_candidates++;
        const residual_metrics_t metrics =
            compute_residual_metrics(aec->pending_mic, aec->probe_ref_chunk, sample_count, 32768);
        if (metrics.corr_q15 > best_corr) {
            best_corr = metrics.corr_q15;
            best_offset = offset;
            best_copied = copied;
            best_is_probe = true;
            memcpy(aec->best_ref_chunk,
                   aec->probe_ref_chunk,
                   sample_count * sizeof(aec->best_ref_chunk[0]));
        }
    }
    aec->adaptive_ref_last_best_corr_q15 = best_corr;

    if (best_corr > aec->adaptive_ref_corr_max_q15) {
        aec->adaptive_ref_corr_max_q15 = best_corr;
    }
    const uint32_t min_improve = (uint32_t)CONFIG_NUWAX_LIVE_AEC_ADAPTIVE_REF_MIN_IMPROVE_Q15;
    const bool clearly_better =
        best_corr > base_metrics.corr_q15 &&
        (best_corr - base_metrics.corr_q15) >= min_improve;
    if (best_is_probe &&
        clearly_better &&
        best_corr >= (uint32_t)CONFIG_NUWAX_LIVE_AEC_ADAPTIVE_REF_MIN_CORR_Q15) {
        memcpy(out, aec->best_ref_chunk, sample_count * sizeof(int16_t));
        aec->delay_samples = (uint32_t)((int64_t)aec->delay_samples + (int64_t)best_offset);
        aec->adaptive_ref_chunks++;
        aec->adaptive_ref_last_offset_samples = best_offset;
        aec->adaptive_ref_offset_abs_sum += (uint32_t)(best_offset < 0 ? -best_offset : best_offset);
        return best_copied;
    }
    aec->adaptive_ref_probe_no_improve++;
#endif
    return base_copied;
}

static void apply_residual_ref_cancel(nuwax_live_aec_t *aec,
                                      int16_t *samples,
                                      const int16_t *ref,
                                      size_t sample_count,
                                      bool nearend_active) {
    if (!aec || !samples || !ref || sample_count == 0) {
        return;
    }
#if CONFIG_NUWAX_LIVE_AEC_RESIDUAL_CANCEL_ENABLE
    const residual_metrics_t metrics = compute_residual_metrics(samples,
                                                                ref,
                                                                sample_count,
                                                                CONFIG_NUWAX_LIVE_AEC_RESIDUAL_MAX_GAIN_Q15);
    aec->last_residual_corr_q15 = metrics.corr_q15;
    aec->last_residual_gain_q15 = metrics.gain_q15;
    if (metrics.corr_q15 < (uint32_t)CONFIG_NUWAX_LIVE_AEC_RESIDUAL_MIN_CORR_Q15 ||
        metrics.gain_q15 == 0) {
        return;
    }
    if (nearend_active &&
        metrics.corr_q15 < (uint32_t)CONFIG_NUWAX_LIVE_AEC_RESIDUAL_NEAREND_MIN_CORR_Q15) {
        aec->residual_nearend_skip_chunks++;
        return;
    }
    for (size_t i = 0; i < sample_count; i++) {
        const int32_t cancel = (int32_t)(((int64_t)metrics.gain_q15 * (int64_t)ref[i]) >> 15);
        const int32_t out = (int32_t)samples[i] - cancel;
        const int16_t clamped = clamp_i16(out);
        if ((int32_t)clamped != out) {
            aec->clipped_samples++;
        }
        samples[i] = clamped;
    }
    aec->residual_cancel_chunks++;
#else
    (void)nearend_active;
#endif
}

static void apply_post_blend_residual_cancel(nuwax_live_aec_t *aec,
                                             int16_t *samples,
                                             const int16_t *ref,
                                             size_t sample_count) {
    if (!aec || !samples || !ref || sample_count == 0) {
        return;
    }
#if CONFIG_NUWAX_LIVE_AEC_POST_BLEND_RESIDUAL_ENABLE
    const residual_metrics_t metrics = compute_residual_metrics(samples,
                                                                ref,
                                                                sample_count,
                                                                CONFIG_NUWAX_LIVE_AEC_POST_BLEND_RESIDUAL_MAX_GAIN_Q15);
    aec->last_post_blend_residual_corr_q15 = metrics.corr_q15;
    aec->last_post_blend_residual_gain_q15 = metrics.gain_q15;
    if (metrics.corr_q15 < (uint32_t)CONFIG_NUWAX_LIVE_AEC_POST_BLEND_RESIDUAL_MIN_CORR_Q15 ||
        metrics.gain_q15 == 0) {
        return;
    }
    for (size_t i = 0; i < sample_count; i++) {
        const int32_t cancel = (int32_t)(((int64_t)metrics.gain_q15 * (int64_t)ref[i]) >> 15);
        const int32_t out = (int32_t)samples[i] - cancel;
        const int16_t clamped = clamp_i16(out);
        if ((int32_t)clamped != out) {
            aec->clipped_samples++;
        }
        samples[i] = clamped;
    }
    aec->post_blend_residual_cancel_chunks++;
#endif
}

static void apply_dual_restore_residual_guard(nuwax_live_aec_t *aec,
                                              int16_t *samples,
                                              const int16_t *ref,
                                              size_t sample_count) {
    if (!aec || !samples || !ref || sample_count == 0) {
        return;
    }
#if CONFIG_NUWAX_LIVE_AEC_POST_BLEND_RESIDUAL_ENABLE
    const residual_metrics_t metrics =
        compute_residual_metrics(samples,
                                 ref,
                                 sample_count,
                                 CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_POST_RESIDUAL_MAX_GAIN_Q15);
    aec->last_post_blend_residual_corr_q15 = metrics.corr_q15;
    aec->last_post_blend_residual_gain_q15 = metrics.gain_q15;
    if (metrics.corr_q15 < (uint32_t)CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_POST_RESIDUAL_CORR_Q15 ||
        metrics.gain_q15 == 0) {
        return;
    }
    for (size_t i = 0; i < sample_count; i++) {
        const int32_t cancel = (int32_t)(((int64_t)metrics.gain_q15 * (int64_t)ref[i]) >> 15);
        const int32_t out = (int32_t)samples[i] - cancel;
        const int16_t clamped = clamp_i16(out);
        if ((int32_t)clamped != out) {
            aec->clipped_samples++;
        }
        samples[i] = clamped;
    }
    aec->dual_restore_post_residual_chunks++;
#endif
}

static bool rescue_kind_valid(nuwax_live_aec_rescue_source_kind_t kind) {
    return kind >= NUWAX_LIVE_AEC_RESCUE_SOURCE_RAW &&
           kind <= NUWAX_LIVE_AEC_RESCUE_SOURCE_FIR;
}

static void rescue_provenance_clear(
    nuwax_live_aec_rescue_provenance_t *provenance) {
    if (provenance) {
        memset(provenance, 0, sizeof(*provenance));
    }
}

static bool rescue_provenance_append(
    nuwax_live_aec_rescue_provenance_t *provenance,
    nuwax_live_aec_rescue_source_kind_t kind,
    uint32_t mix_q15,
    size_t sample_count) {
    if (!provenance || !rescue_kind_valid(kind)) {
        return false;
    }
    if (sample_count == 0U) {
        return true;
    }
    if (sample_count > SIZE_MAX - provenance->sample_count) {
        return false;
    }
    if (provenance->segment_count > 0U) {
        nuwax_live_aec_rescue_segment_t *last =
            &provenance->segments[provenance->segment_count - 1U];
        if (last->kind == kind && last->mix_q15 == mix_q15) {
            if (sample_count > SIZE_MAX - last->sample_count) {
                return false;
            }
            last->sample_count += sample_count;
            provenance->sample_count += sample_count;
            return true;
        }
    }
    if (provenance->segment_count >=
        NUWAX_LIVE_AEC_RESCUE_SEGMENT_CAPACITY) {
        return false;
    }
    provenance->segments[provenance->segment_count++] =
        (nuwax_live_aec_rescue_segment_t){
            .kind = kind,
            .sample_count = sample_count,
            .mix_q15 = mix_q15 > 32768U ? 32768U : mix_q15,
        };
    provenance->sample_count += sample_count;
    return true;
}

static size_t rescue_provenance_consume(
    nuwax_live_aec_rescue_provenance_t *queued,
    size_t sample_count,
    nuwax_live_aec_rescue_provenance_t *consumed) {
    if (!queued || !consumed || sample_count == 0U) {
        return 0U;
    }
    rescue_provenance_clear(consumed);
    size_t total = 0U;
    while (total < sample_count && queued->segment_count > 0U) {
        nuwax_live_aec_rescue_segment_t *first = &queued->segments[0];
        const size_t remaining = sample_count - total;
        const size_t take = first->sample_count < remaining
                                ? first->sample_count
                                : remaining;
        if (!rescue_provenance_append(consumed,
                                      first->kind,
                                      first->mix_q15,
                                      take)) {
            return total;
        }
        first->sample_count -= take;
        queued->sample_count -= take;
        total += take;
        if (first->sample_count == 0U) {
            if (queued->segment_count > 1U) {
                memmove(&queued->segments[0],
                        &queued->segments[1],
                        (queued->segment_count - 1U) *
                            sizeof(queued->segments[0]));
            }
            queued->segment_count--;
        }
    }
    return total;
}

static void fifo_clear(nuwax_live_aec_t *aec) {
    if (!aec) {
        return;
    }
    aec->output_read = 0;
    aec->output_count = 0;
    aec->output_started = false;
    aec->human_rescue_read = 0U;
    aec->human_rescue_count = 0U;
    aec->human_rescue_output_count = 0U;
    aec->last_output_emitted = false;
    nuwax_live_aec_provenance_clear(&aec->output_provenance);
    rescue_provenance_clear(&aec->human_rescue_queued);
    rescue_provenance_clear(&aec->last_human_rescue_output);
}

static bool fifo_append_pair(nuwax_live_aec_t *aec,
                             const int16_t *samples,
                             size_t sample_count,
                             nuwax_live_aec_output_kind_t first_kind,
                             size_t first_sample_count,
                             nuwax_live_aec_output_kind_t second_kind) {
    if (!aec || !aec->output_fifo || !aec->human_rescue_fifo || !samples ||
        sample_count == 0 ||
        first_sample_count > sample_count ||
        sample_count > aec->output_capacity - aec->output_count ||
        sample_count > aec->output_capacity - aec->human_rescue_count) {
        return false;
    }
    const int16_t *rescue_samples =
        aec->human_rescue_source_valid &&
                aec->human_rescue_source_chunk &&
                aec->human_rescue_source_count >= sample_count
            ? aec->human_rescue_source_chunk
            : aec->pending_mic;
    const nuwax_live_aec_rescue_source_kind_t rescue_kind =
        rescue_samples == aec->human_rescue_source_chunk
            ? aec->human_rescue_source_kind
            : NUWAX_LIVE_AEC_RESCUE_SOURCE_RAW;
    const uint32_t rescue_mix_q15 =
        rescue_kind == NUWAX_LIVE_AEC_RESCUE_SOURCE_RAW
            ? 0U
            : (aec->human_rescue_mix_q15 > 32768U
                   ? 32768U
                   : aec->human_rescue_mix_q15);
    if (!rescue_samples) {
        return false;
    }
    const size_t second_sample_count = sample_count - first_sample_count;
    const nuwax_live_aec_provenance_queue_t output_before =
        aec->output_provenance;
    const nuwax_live_aec_rescue_provenance_t rescue_before =
        aec->human_rescue_queued;
    if (!nuwax_live_aec_provenance_append_pair(&aec->output_provenance,
                                                first_kind,
                                                first_sample_count,
                                                second_kind,
                                                second_sample_count) ||
        !rescue_provenance_append(&aec->human_rescue_queued,
                                  rescue_kind,
                                  rescue_mix_q15,
                                  sample_count)) {
        aec->output_provenance = output_before;
        aec->human_rescue_queued = rescue_before;
        return false;
    }
    size_t write = (aec->output_read + aec->output_count) % aec->output_capacity;
    size_t rescue_write =
        (aec->human_rescue_read + aec->human_rescue_count) %
        aec->output_capacity;
    for (size_t i = 0; i < sample_count; i++) {
        aec->output_fifo[write] = samples[i];
        aec->human_rescue_fifo[rescue_write] = rescue_samples[i];
        write = (write + 1U) % aec->output_capacity;
        rescue_write = (rescue_write + 1U) % aec->output_capacity;
    }
    aec->output_count += sample_count;
    aec->human_rescue_count += sample_count;
    return true;
}

static bool fifo_append(nuwax_live_aec_t *aec,
                        const int16_t *samples,
                        size_t sample_count,
                        nuwax_live_aec_output_kind_t kind) {
    return fifo_append_pair(aec,
                            samples,
                            sample_count,
                            kind,
                            sample_count,
                            kind);
}

static size_t fifo_read(nuwax_live_aec_t *aec, int16_t *out, size_t sample_count) {
    if (!aec || !out || !aec->output_fifo || !aec->human_rescue_fifo ||
        !aec->human_rescue_output_frame || sample_count == 0) {
        return 0;
    }
    if (aec->output_provenance.queued_samples != aec->output_count ||
        aec->human_rescue_queued.sample_count != aec->human_rescue_count ||
        aec->human_rescue_count != aec->output_count) {
        ESP_LOGE(TAG,
                 "AEC parallel FIFO mismatch audio=%u provenance=%u rescue=%u rescue_provenance=%u; flushing adapter",
                 (unsigned)aec->output_count,
                 (unsigned)aec->output_provenance.queued_samples,
                 (unsigned)aec->human_rescue_count,
                 (unsigned)aec->human_rescue_queued.sample_count);
        fifo_clear(aec);
        return 0;
    }
    size_t copied = 0;
    while (copied < sample_count && aec->output_count > 0) {
        out[copied++] = aec->output_fifo[aec->output_read];
        aec->human_rescue_output_frame[copied - 1U] =
            aec->human_rescue_fifo[aec->human_rescue_read];
        aec->output_read = (aec->output_read + 1U) % aec->output_capacity;
        aec->human_rescue_read =
            (aec->human_rescue_read + 1U) % aec->output_capacity;
        aec->output_count--;
        aec->human_rescue_count--;
    }
    if (aec->output_count == 0) {
        aec->output_read = 0;
        aec->human_rescue_read = 0U;
    }
    const size_t provenance_copied =
        nuwax_live_aec_provenance_consume(&aec->output_provenance,
                                          copied,
                                          &aec->last_output);
    if (provenance_copied != copied) {
        ESP_LOGE(TAG,
                 "AEC output provenance short read audio=%u provenance=%u; flushing adapter",
                 (unsigned)copied,
                 (unsigned)provenance_copied);
        nuwax_live_aec_output_stats_reset(&aec->last_output);
        fifo_clear(aec);
        return 0;
    }
    const size_t rescue_provenance_copied =
        rescue_provenance_consume(&aec->human_rescue_queued,
                                  copied,
                                  &aec->last_human_rescue_output);
    if (rescue_provenance_copied != copied) {
        ESP_LOGE(TAG,
                 "AEC rescue provenance short read audio=%u rescue=%u; flushing adapter",
                 (unsigned)copied,
                 (unsigned)rescue_provenance_copied);
        nuwax_live_aec_output_stats_reset(&aec->last_output);
        fifo_clear(aec);
        return 0;
    }
    aec->human_rescue_output_count = copied;
    return copied;
}

void nuwax_live_aec_init(nuwax_live_aec_t *aec) {
    if (!aec) {
        return;
    }
    memset(aec, 0, sizeof(*aec));
    aec->enabled = CONFIG_NUWAX_LIVE_AEC_ENABLE;
    aec->internal_software_ref_enabled = true;
    aec->delay_samples = (uint32_t)CONFIG_NUWAX_LIVE_AEC_REF_DELAY_SAMPLES;
    aec->output_gain_q15 = (uint32_t)CONFIG_NUWAX_LIVE_AEC_OUTPUT_GAIN_Q15;
}

bool nuwax_live_aec_is_enabled(const nuwax_live_aec_t *aec) {
    return aec && aec->enabled;
}

void nuwax_live_aec_set_nearend_hint(nuwax_live_aec_t *aec, bool nearend_hint, bool strong_hint) {
    if (!aec) {
        return;
    }
    aec->nearend_preserve_hint = nearend_hint || strong_hint;
    aec->strong_nearend_preserve_hint = strong_hint;
}

void nuwax_live_aec_set_human_protection(nuwax_live_aec_t *aec,
                                         bool provisional_human,
                                         bool fast_human,
                                         bool confirmed_human) {
    if (!aec) {
        return;
    }
    aec->provisional_human_protect = provisional_human && !fast_human && !confirmed_human;
    aec->fast_human_protect = fast_human && !confirmed_human;
    aec->confirmed_human_protect = confirmed_human;
}

void nuwax_live_aec_set_internal_software_ref_enabled(nuwax_live_aec_t *aec, bool enabled) {
    if (!aec) {
        return;
    }
    aec->internal_software_ref_enabled = enabled;
}

bool nuwax_live_aec_copy_human_rescue_source(const nuwax_live_aec_t *aec,
                                             int16_t *out,
                                             size_t sample_count,
                                             nuwax_live_aec_rescue_source_kind_t *source_kind) {
    if (source_kind) {
        *source_kind = NUWAX_LIVE_AEC_RESCUE_SOURCE_RAW;
    }
    nuwax_live_aec_rescue_provenance_t provenance = {0};
    if (!nuwax_live_aec_copy_human_rescue_frame(aec,
                                                out,
                                                sample_count,
                                                &provenance)) {
        return false;
    }
    if (source_kind) {
        *source_kind = provenance.segment_count == 1U
                           ? provenance.segments[0].kind
                           : NUWAX_LIVE_AEC_RESCUE_SOURCE_RAW;
    }
    return true;
}

bool nuwax_live_aec_copy_human_rescue_frame(
    const nuwax_live_aec_t *aec,
    int16_t *out,
    size_t sample_count,
    nuwax_live_aec_rescue_provenance_t *provenance) {
    if (provenance) {
        rescue_provenance_clear(provenance);
    }
    if (!aec || !out || sample_count == 0U || !aec->last_output_emitted ||
        !aec->human_rescue_output_frame ||
        aec->human_rescue_output_count != sample_count ||
        aec->last_human_rescue_output.sample_count != sample_count) {
        return false;
    }
    memcpy(out,
           aec->human_rescue_output_frame,
           sample_count * sizeof(out[0]));
    if (provenance) {
        *provenance = aec->last_human_rescue_output;
    }
    return true;
}

bool nuwax_live_aec_rescue_provenance_kind_at(
    const nuwax_live_aec_rescue_provenance_t *provenance,
    size_t sample_index,
    nuwax_live_aec_rescue_source_kind_t *kind) {
    uint32_t ignored_mix_q15 = 0U;
    return nuwax_live_aec_rescue_provenance_sample_at(
        provenance, sample_index, kind, &ignored_mix_q15);
}

bool nuwax_live_aec_rescue_provenance_sample_at(
    const nuwax_live_aec_rescue_provenance_t *provenance,
    size_t sample_index,
    nuwax_live_aec_rescue_source_kind_t *kind,
    uint32_t *mix_q15) {
    if (kind) {
        *kind = NUWAX_LIVE_AEC_RESCUE_SOURCE_RAW;
    }
    if (mix_q15) {
        *mix_q15 = 0U;
    }
    if (!provenance || !kind || !mix_q15 ||
        sample_index >= provenance->sample_count) {
        return false;
    }
    size_t offset = 0U;
    for (size_t i = 0U; i < provenance->segment_count; i++) {
        const size_t count = provenance->segments[i].sample_count;
        if (sample_index - offset < count) {
            *kind = provenance->segments[i].kind;
            *mix_q15 = provenance->segments[i].mix_q15;
            return true;
        }
        offset += count;
    }
    return false;
}

bool nuwax_live_aec_output_emitted(const nuwax_live_aec_t *aec) {
    return aec && aec->last_output_emitted;
}

size_t nuwax_live_aec_flush_pending(nuwax_live_aec_t *aec) {
    if (!aec) {
        return 0U;
    }
    const size_t dropped = aec->output_count + aec->pending_count;
    aec->pending_count = 0U;
    aec->pending_external_ref = false;
    aec->pending_dual_mic = false;
    fifo_clear(aec);
    return dropped;
}

static esp_err_t ensure_live_aec(nuwax_live_aec_t *aec) {
#if CONFIG_NUWAX_LIVE_AEC_ENABLE && (CONFIG_NUWAX_P3_SOFTWARE_REF_ENABLE || CONFIG_NUWAX_VOCAT_SLOT2_AEC_REF_ENABLE)
    if (!aec) {
        return ESP_ERR_INVALID_ARG;
    }
    if (aec->initialized) {
        return ESP_OK;
    }
#if NUWAX_LIVE_AEC_BACKEND_AFE
    aec->aec = afe_aec_create("MR",
                              CONFIG_NUWAX_LIVE_AEC_FILTER_LENGTH,
                              NUWAX_LIVE_AFE_TYPE,
                              NUWAX_LIVE_AFE_MODE);
#else
    aec->aec = aec_create(AEC_SAMPLE_RATE,
                          CONFIG_NUWAX_LIVE_AEC_FILTER_LENGTH,
                          1,
                          NUWAX_LIVE_AEC_MODE);
#endif
    if (!aec->aec) {
        ESP_LOGE(TAG, "aec_create failed backend=%s sample_rate=%d channels=1 filter=%d type=%s mode=%s",
#if NUWAX_LIVE_AEC_BACKEND_AFE
                 "afe_aec",
#else
                 "esp_aec",
#endif
                 CONFIG_NUWAX_VOICE_SAMPLE_RATE,
                 CONFIG_NUWAX_LIVE_AEC_FILTER_LENGTH,
                 NUWAX_LIVE_AEC_TYPE_NAME,
                 NUWAX_LIVE_AEC_MODE_NAME);
        aec->enabled = false;
        return ESP_FAIL;
    }

#if NUWAX_LIVE_AEC_BACKEND_AFE
    aec->chunk_samples = afe_aec_get_chunksize((afe_aec_handle_t *)aec->aec);
#else
    aec->chunk_samples = aec_get_chunksize((aec_handle_t *)aec->aec);
#endif
    if (aec->chunk_samples <= 0 || aec->chunk_samples > 2048) {
        ESP_LOGE(TAG, "invalid AEC chunk_samples=%d", aec->chunk_samples);
        aec->enabled = false;
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t chunk = (size_t)aec->chunk_samples;
    aec->pending_mic = (int16_t *)alloc_hot_audio(chunk * sizeof(int16_t));
    aec->pending_mic2 = (int16_t *)alloc_hot_audio(chunk * sizeof(int16_t));
    aec->pending_ref = (int16_t *)alloc_hot_audio(chunk * sizeof(int16_t));
    aec->pending_fir_ref = (int16_t *)alloc_hot_audio(chunk * sizeof(int16_t));
    aec->mr_chunk = (int16_t *)alloc_hot_audio(chunk * 2U * sizeof(int16_t));
    aec->ref_chunk = (int16_t *)alloc_hot_audio(chunk * sizeof(int16_t));
    aec->probe_ref_chunk = (int16_t *)alloc_aligned_audio(chunk * sizeof(int16_t));
    aec->best_ref_chunk = (int16_t *)alloc_aligned_audio(chunk * sizeof(int16_t));
    aec->out_chunk = (int16_t *)alloc_hot_audio(chunk * sizeof(int16_t));
    aec->output_capacity = chunk * 4U;
    aec->output_fifo = (int16_t *)alloc_aligned_audio(aec->output_capacity * sizeof(int16_t));
    aec->human_rescue_fifo =
        (int16_t *)alloc_aligned_audio(aec->output_capacity * sizeof(int16_t));
    aec->human_rescue_output_frame =
        (int16_t *)alloc_aligned_audio(aec->output_capacity * sizeof(int16_t));
    aec->human_rescue_source_chunk =
        (int16_t *)alloc_aligned_audio(chunk * sizeof(aec->human_rescue_source_chunk[0]));
    if (!aec->pending_mic || !aec->pending_mic2 || !aec->pending_ref ||
        !aec->pending_fir_ref || !aec->mr_chunk ||
        !aec->probe_ref_chunk || !aec->best_ref_chunk ||
        !aec->ref_chunk || !aec->out_chunk || !aec->output_fifo ||
        !aec->human_rescue_fifo || !aec->human_rescue_output_frame ||
        !aec->human_rescue_source_chunk) {
        ESP_LOGE(TAG, "AEC buffer allocation failed chunk=%u", (unsigned)chunk);
        aec->enabled = false;
        return ESP_ERR_NO_MEM;
    }
#if CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_ENABLE
    const size_t fir_taps = (size_t)CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_TAPS;
    aec->human_rescue_fir_coeffs =
        (float *)alloc_internal_dsp(fir_taps * sizeof(aec->human_rescue_fir_coeffs[0]));
    aec->human_rescue_fir_ref_history =
        (float *)alloc_internal_dsp(fir_taps * 2U * sizeof(aec->human_rescue_fir_ref_history[0]));
    aec->human_rescue_fir_available = aec->human_rescue_fir_coeffs &&
                                       aec->human_rescue_fir_ref_history;
    if (!aec->human_rescue_fir_available) {
        heap_caps_free(aec->human_rescue_fir_coeffs);
        heap_caps_free(aec->human_rescue_fir_ref_history);
        aec->human_rescue_fir_coeffs = NULL;
        aec->human_rescue_fir_ref_history = NULL;
        ESP_LOGW(TAG, "human rescue FIR allocation failed; scalar rescue fallback remains active");
    } else {
        reset_human_rescue_fir_session(aec);
    }
#endif
    memset(aec->output_fifo, 0, aec->output_capacity * sizeof(int16_t));
    memset(aec->human_rescue_fifo,
           0,
           aec->output_capacity * sizeof(int16_t));
    memset(aec->human_rescue_output_frame,
           0,
           aec->output_capacity * sizeof(int16_t));
    aec->initialized = true;
    ESP_LOGI(TAG, "live AEC ready api=%s chunk=%d delay_samples=%lu filter=%d force_vc=%d type=%s mode=%s min_ref_avg=%d min_ref_peak=%d output_gain_q15=%lu ref_missing_attenuate_q15=%d ref_missing_hint_attenuate_q15=%d ref_inactive_attenuate_q15=%d ref_inactive_hint_attenuate_q15=%d adaptive_ref=%d span=%d step=%d min_corr=%d nearend_raw_mix_q15=%d hint_strong_raw_mix_q15=%d nearend_blend_aec_avg=%d nearend_blend_aec_peak=%d nearend_blend_min_out_ratio_q15=%d nearend_blend_full_corr_q15=%d nearend_blend_guard_corr_q15=%d strong_nearend_avg=%d strong_nearend_peak=%d strong_nearend_raw_mix_q15=%d strong_nearend_blend_guard_corr_q15=%d residual_enable=%d residual_min_corr_q15=%d residual_nearend_min_corr_q15=%d residual_max_gain_q15=%d post_blend_residual_enable=%d post_blend_residual_min_corr_q15=%d post_blend_residual_max_gain_q15=%d dual_restore=%d dual_min_score=%d dual_gain=%d/%d/%d/%d dual_high_corr_guard=%d dual_min_diff=%d/%d dual_spatial=%d/%d/%d dual_very_strong=%d/%d/%d dual_post_residual=%d/%d nearend_floor=%d target=%d max_gain=%d min_mic=%d/%d strict=%d/%d/%d ref_guard=%d human_rescue=%d rescue_mix=%d/%d/%d rescue_max_ref_gain=%d rescue_min_corr=%d fifo=%u",
#if NUWAX_LIVE_AEC_BACKEND_AFE
             "esp_afe_aec",
#else
             "esp_aec",
#endif
             aec->chunk_samples,
             (unsigned long)aec->delay_samples,
             CONFIG_NUWAX_LIVE_AEC_FILTER_LENGTH,
             CONFIG_NUWAX_LIVE_AEC_FORCE_VC ? 1 : 0,
             NUWAX_LIVE_AEC_TYPE_NAME,
             NUWAX_LIVE_AEC_MODE_NAME,
              CONFIG_NUWAX_LIVE_AEC_MIN_REF_AVG_ABS,
              CONFIG_NUWAX_LIVE_AEC_MIN_REF_PEAK,
              (unsigned long)aec->output_gain_q15,
             CONFIG_NUWAX_LIVE_AEC_REF_MISSING_ATTENUATE_Q15,
             CONFIG_NUWAX_LIVE_AEC_REF_MISSING_HINT_ATTENUATE_Q15,
             CONFIG_NUWAX_LIVE_AEC_REF_INACTIVE_ATTENUATE_Q15,
             CONFIG_NUWAX_LIVE_AEC_REF_INACTIVE_HINT_ATTENUATE_Q15,
             CONFIG_NUWAX_LIVE_AEC_ADAPTIVE_REF_ENABLE ? 1 : 0,
             CONFIG_NUWAX_LIVE_AEC_ADAPTIVE_REF_SPAN_SAMPLES,
             CONFIG_NUWAX_LIVE_AEC_ADAPTIVE_REF_STEP_SAMPLES,
             CONFIG_NUWAX_LIVE_AEC_ADAPTIVE_REF_MIN_CORR_Q15,
             CONFIG_NUWAX_LIVE_AEC_NEAREND_RAW_MIX_Q15,
             CONFIG_NUWAX_LIVE_AEC_HINT_STRONG_RAW_MIX_Q15,
             CONFIG_NUWAX_LIVE_AEC_NEAREND_BLEND_AEC_AVG_ABS,
             CONFIG_NUWAX_LIVE_AEC_NEAREND_BLEND_AEC_PEAK,
             CONFIG_NUWAX_LIVE_AEC_NEAREND_BLEND_MIN_OUT_RATIO_Q15,
             CONFIG_NUWAX_LIVE_AEC_NEAREND_BLEND_FULL_CORR_Q15,
             CONFIG_NUWAX_LIVE_AEC_NEAREND_BLEND_GUARD_CORR_Q15,
             CONFIG_NUWAX_LIVE_AEC_STRONG_NEAREND_AVG_ABS,
             CONFIG_NUWAX_LIVE_AEC_STRONG_NEAREND_PEAK,
             CONFIG_NUWAX_LIVE_AEC_STRONG_NEAREND_RAW_MIX_Q15,
             CONFIG_NUWAX_LIVE_AEC_STRONG_NEAREND_BLEND_GUARD_CORR_Q15,
             CONFIG_NUWAX_LIVE_AEC_RESIDUAL_CANCEL_ENABLE ? 1 : 0,
             CONFIG_NUWAX_LIVE_AEC_RESIDUAL_MIN_CORR_Q15,
             CONFIG_NUWAX_LIVE_AEC_RESIDUAL_NEAREND_MIN_CORR_Q15,
             CONFIG_NUWAX_LIVE_AEC_RESIDUAL_MAX_GAIN_Q15,
             CONFIG_NUWAX_LIVE_AEC_POST_BLEND_RESIDUAL_ENABLE ? 1 : 0,
             CONFIG_NUWAX_LIVE_AEC_POST_BLEND_RESIDUAL_MIN_CORR_Q15,
             CONFIG_NUWAX_LIVE_AEC_POST_BLEND_RESIDUAL_MAX_GAIN_Q15,
             CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_ENABLE ? 1 : 0,
             CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_MIN_SCORE_Q15,
             CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_WEAK_GAIN_Q15,
             CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_MID_GAIN_Q15,
             CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_STRONG_GAIN_Q15,
             CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_HINT_GAIN_Q15,
             CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_HIGH_CORR_GUARD_Q15,
             CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_MIN_DIFF_AVG_ABS,
             CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_MIN_DIFF_PEAK,
             CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_SPATIAL_BALANCE_Q15,
             CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_SPATIAL_DIFF_AVG_ABS,
             CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_SPATIAL_DIFF_PEAK,
             CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_VERY_STRONG_BALANCE_Q15,
             CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_VERY_STRONG_DIFF_AVG_ABS,
             CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_VERY_STRONG_DIFF_PEAK,
             CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_POST_RESIDUAL_CORR_Q15,
             CONFIG_NUWAX_LIVE_AEC_DUAL_RESTORE_POST_RESIDUAL_MAX_GAIN_Q15,
             CONFIG_NUWAX_LIVE_AEC_NEAREND_FLOOR_ENABLE ? 1 : 0,
             CONFIG_NUWAX_LIVE_AEC_NEAREND_FLOOR_TARGET_Q15,
             CONFIG_NUWAX_LIVE_AEC_NEAREND_FLOOR_MAX_GAIN_Q15,
             CONFIG_NUWAX_LIVE_AEC_NEAREND_FLOOR_MIN_MIC_AVG_ABS,
             CONFIG_NUWAX_LIVE_AEC_NEAREND_FLOOR_MIN_MIC_PEAK,
             CONFIG_NUWAX_LIVE_AEC_NEAREND_FLOOR_STRICT_BALANCE_Q15,
             CONFIG_NUWAX_LIVE_AEC_NEAREND_FLOOR_MIN_DIFF_AVG_ABS,
             CONFIG_NUWAX_LIVE_AEC_NEAREND_FLOOR_MIN_DIFF_PEAK,
             CONFIG_NUWAX_LIVE_AEC_NEAREND_FLOOR_REF_GUARD_CORR_Q15,
             CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_ENABLE ? 1 : 0,
             CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_PROVISIONAL_MIX_Q15,
             CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FAST_MIX_Q15,
             CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_CONFIRMED_MIX_Q15,
             CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_MAX_REF_GAIN_Q15,
             CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_MIN_REF_CORR_Q15,
             (unsigned)aec->output_capacity);
    ESP_LOGI(TAG,
             "human rescue FIR enabled=%d available=%d taps=%d seed_tap=%d step_q15=%d warmup=%d min_training=%d train_ref_min=%u/%u max_residual_raw_q15=%d max_residual_scalar_q15=%d min_prediction_corr_q15=%u",
             CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_ENABLE ? 1 : 0,
             aec->human_rescue_fir_available ? 1 : 0,
             CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_TAPS,
             CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_SEED_TAP,
             CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_STEP_Q15,
             CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_WARMUP_SAMPLES,
             CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_MIN_TRAINING_SAMPLES,
             (unsigned)NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_TRAIN_MIN_REF_AVG_ABS,
             (unsigned)NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_TRAIN_MIN_REF_PEAK,
             CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_MAX_RESIDUAL_RAW_Q15,
             CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_MAX_RESIDUAL_SCALAR_Q15,
             (unsigned)NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_MIN_PREDICTION_CORR_Q15);
    return ESP_OK;
#else
    if (aec) {
        aec->enabled = false;
    }
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static esp_err_t validate_frame_adapter_contract(const nuwax_live_aec_t *aec,
                                                 size_t frame_samples) {
    if (!aec || frame_samples == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!aec->initialized || aec->chunk_samples <= 0 ||
        aec->output_capacity < (size_t)aec->chunk_samples) {
        ESP_LOGE(TAG,
                 "AEC frame adapter is not ready initialized=%d chunk_samples=%d fifo=%u",
                 aec->initialized ? 1 : 0,
                 aec->chunk_samples,
                 (unsigned)aec->output_capacity);
        return ESP_ERR_INVALID_STATE;
    }
    const size_t chunk_samples = (size_t)aec->chunk_samples;
    const size_t max_safe_frame_samples =
        (aec->output_capacity - chunk_samples + 1U) / 2U;
    if (frame_samples > max_safe_frame_samples) {
        ESP_LOGE(TAG,
                 "AEC frame exceeds FIFO contract frame_samples=%u chunk_samples=%u fifo=%u max_frame=%u",
                 (unsigned)frame_samples,
                 (unsigned)chunk_samples,
                 (unsigned)aec->output_capacity,
                 (unsigned)max_safe_frame_samples);
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static void snapshot_human_rescue_fir_session(nuwax_live_aec_t *aec) {
    if (!aec || !aec->human_rescue_fir_initialized) {
        return;
    }
    aec->human_rescue_fir_last_session_ready = aec->human_rescue_fir_ready;
    aec->human_rescue_fir_last_session_training_samples =
        aec->human_rescue_fir_session_training_samples;
    aec->human_rescue_fir_last_session_raw_q15 =
        aec->human_rescue_fir_residual_raw_ema_q15;
    aec->human_rescue_fir_last_session_scalar_q15 =
        aec->human_rescue_fir_residual_scalar_ema_q15;
    aec->human_rescue_fir_last_session_prediction_corr_q15 =
        aec->human_rescue_fir_prediction_corr_q15;
    aec->human_rescue_fir_last_session_coeff_peak_q15 =
        aec->human_rescue_fir_coeff_peak_q15;
    aec->human_rescue_fir_last_session_coeff_peak_tap =
        aec->human_rescue_fir_coeff_peak_tap;
    aec->human_rescue_fir_last_session_edge_energy_q15 =
        aec->human_rescue_fir_edge_energy_q15;
}

static void reset_human_rescue_for_new_session(nuwax_live_aec_t *aec) {
    if (!aec) {
        return;
    }

    snapshot_human_rescue_fir_session(aec);
    aec->human_rescue_mix_q15 = 0;
    aec->human_rescue_ref_gain_valid = false;
    aec->human_rescue_ref_gain_q15 = 0;
    aec->human_rescue_source_count = 0;
    aec->human_rescue_source_valid = false;
    aec->human_rescue_source_fir = false;
    aec->human_rescue_source_kind = NUWAX_LIVE_AEC_RESCUE_SOURCE_RAW;
    if (aec->human_rescue_source_chunk && aec->chunk_samples > 0) {
        memset(aec->human_rescue_source_chunk,
               0,
               (size_t)aec->chunk_samples * sizeof(aec->human_rescue_source_chunk[0]));
    }

#if CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_ENABLE
    if (aec->human_rescue_fir_available && aec->human_rescue_fir_initialized &&
        aec->human_rescue_fir_ready) {
        const size_t taps = (size_t)CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_TAPS;
        if (aec->human_rescue_fir_ref_history) {
            memset(aec->human_rescue_fir_ref_history,
                   0,
                   taps * 2U * sizeof(aec->human_rescue_fir_ref_history[0]));
        }
        aec->human_rescue_fir_history_pos = 0;
        aec->human_rescue_fir_ref_energy = 0.0f;
        aec->human_rescue_fir_warmup_samples = 0;
        aec->human_rescue_fir_frozen = false;
        aec->human_rescue_fir_nearend_hold_chunks = 0;
        aec->human_rescue_fir_ready_bad_windows = 0;
        reset_human_rescue_fir_training_health(aec);
        reset_human_rescue_fir_validation(aec);
        reset_human_rescue_fir_ready_health_window(aec);
    } else if (aec->human_rescue_fir_available) {
        reset_human_rescue_fir_session(aec);
        aec->human_rescue_fir_warmup_samples = 0;
    }
#endif
}

static void reset_live_aec_session_state(nuwax_live_aec_t *aec) {
    if (!aec) {
        return;
    }

    aec->pending_count = 0;
    aec->pending_external_ref = false;
    aec->pending_dual_mic = false;
    aec->nearend_preserve_hint = false;
    aec->strong_nearend_preserve_hint = false;
    aec->provisional_human_protect = false;
    aec->fast_human_protect = false;
    aec->confirmed_human_protect = false;
    aec->last_residual_corr_q15 = 0;
    aec->last_residual_gain_q15 = 0;
    aec->last_post_blend_residual_corr_q15 = 0;
    aec->last_post_blend_residual_gain_q15 = 0;
    aec->last_mic_ref_corr_q15 = 0;
    aec->last_mic_ref_gain_q15 = 0;
    aec->last_mic_avg_abs = 0;
    aec->last_mic_peak = 0;
    aec->last_ref_avg_abs = 0;
    aec->last_ref_peak = 0;
    aec->last_out_avg_abs = 0;
    aec->last_out_peak = 0;
    aec->adaptive_ref_nominal_delay_samples = aec->delay_samples;
    fifo_clear(aec);
    nuwax_live_aec_output_stats_reset(&aec->last_output);

    if (aec->chunk_samples > 0) {
        const size_t chunk_bytes = (size_t)aec->chunk_samples * sizeof(int16_t);
        if (aec->pending_mic) {
            memset(aec->pending_mic, 0, chunk_bytes);
        }
        if (aec->pending_mic2) {
            memset(aec->pending_mic2, 0, chunk_bytes);
        }
        if (aec->pending_ref) {
            memset(aec->pending_ref, 0, chunk_bytes);
        }
        if (aec->pending_fir_ref) {
            memset(aec->pending_fir_ref, 0, chunk_bytes);
        }
    }
    if (aec->output_fifo && aec->output_capacity > 0) {
        memset(aec->output_fifo, 0, aec->output_capacity * sizeof(aec->output_fifo[0]));
    }
    if (aec->human_rescue_fifo && aec->output_capacity > 0) {
        memset(aec->human_rescue_fifo,
               0,
               aec->output_capacity * sizeof(aec->human_rescue_fifo[0]));
    }
    if (aec->human_rescue_output_frame && aec->output_capacity > 0) {
        memset(aec->human_rescue_output_frame,
               0,
               aec->output_capacity *
                   sizeof(aec->human_rescue_output_frame[0]));
    }
    reset_human_rescue_for_new_session(aec);
}

esp_err_t nuwax_live_aec_prepare(nuwax_live_aec_t *aec, size_t frame_samples) {
    if (!aec || frame_samples == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!aec->enabled) {
        return ESP_OK;
    }
    esp_err_t err = ensure_live_aec(aec);
    if (err != ESP_OK) {
        return err;
    }
    err = validate_frame_adapter_contract(aec, frame_samples);
    if (err != ESP_OK) {
        return err;
    }
    const size_t chunk_samples = (size_t)aec->chunk_samples;
    if (chunk_samples != frame_samples) {
        ESP_LOGI(TAG,
                 "AEC frame adapter enabled frame_samples=%u chunk_samples=%u fifo=%u",
                 (unsigned)frame_samples,
                 (unsigned)chunk_samples,
                 (unsigned)aec->output_capacity);
    }
    aec->adaptive_ref_nominal_delay_samples = aec->delay_samples;
    return ESP_OK;
}

esp_err_t nuwax_live_aec_begin_session(nuwax_live_aec_t *aec, size_t frame_samples) {
    if (!aec || frame_samples == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!aec->enabled) {
        reset_live_aec_session_state(aec);
        return ESP_OK;
    }

    esp_err_t err = ensure_live_aec(aec);
    if (err != ESP_OK) {
        return err;
    }
    err = validate_frame_adapter_contract(aec, frame_samples);
    if (err != ESP_OK) {
        return err;
    }

    const bool fir_model_preserved = aec->human_rescue_fir_available &&
                                     aec->human_rescue_fir_initialized &&
                                     aec->human_rescue_fir_ready;
    reset_live_aec_session_state(aec);
    ESP_LOGI(TAG,
             "live AEC session begin frame_samples=%u chunk_samples=%d fifo=%u fir_model=%s",
             (unsigned)frame_samples,
             aec->chunk_samples,
             (unsigned)aec->output_capacity,
             fir_model_preserved ? "preserved" : "retrain");
    return ESP_OK;
}

static void append_bypass_chunk(nuwax_live_aec_t *aec,
                                nuwax_live_aec_output_kind_t kind) {
    if (!fifo_append(aec,
                     aec->pending_mic,
                     (size_t)aec->chunk_samples,
                     kind)) {
        fifo_clear(aec);
        (void)fifo_append(aec,
                          aec->pending_mic,
                          (size_t)aec->chunk_samples,
                          kind);
    }
    aec->bypass_chunks++;
}

static void append_ref_missing_chunk(nuwax_live_aec_t *aec) {
    if (!aec) {
        return;
    }
    const size_t chunk = (size_t)aec->chunk_samples;
    int32_t atten_q15 = (int32_t)CONFIG_NUWAX_LIVE_AEC_REF_MISSING_ATTENUATE_Q15;
    if (aec->strong_nearend_preserve_hint &&
        CONFIG_NUWAX_LIVE_AEC_REF_MISSING_HINT_ATTENUATE_Q15 > atten_q15) {
        atten_q15 = (int32_t)CONFIG_NUWAX_LIVE_AEC_REF_MISSING_HINT_ATTENUATE_Q15;
        aec->ref_missing_hint_preserved_chunks++;
    }
    if (atten_q15 >= 32768) {
        append_bypass_chunk(aec, NUWAX_LIVE_AEC_OUTPUT_REF_MISSING);
        return;
    }
    if (!aec->out_chunk || !aec->pending_mic || chunk == 0) {
        return;
    }
    if (atten_q15 <= 0) {
        memset(aec->out_chunk, 0, chunk * sizeof(aec->out_chunk[0]));
    } else {
        for (size_t i = 0; i < chunk; i++) {
            aec->out_chunk[i] = clamp_i16((int32_t)(((int64_t)aec->pending_mic[i] * atten_q15) >> 15));
        }
    }
    const residual_metrics_t zero_metrics = {0};
    (void)apply_dual_mic_nearend_restore(aec, aec->out_chunk, chunk, zero_metrics, false);
    if (!fifo_append(aec,
                     aec->out_chunk,
                     chunk,
                     NUWAX_LIVE_AEC_OUTPUT_REF_MISSING)) {
        fifo_clear(aec);
        (void)fifo_append(aec,
                          aec->out_chunk,
                          chunk,
                          NUWAX_LIVE_AEC_OUTPUT_REF_MISSING);
    }
    const nuwax_mic_level_t out_level = nuwax_mic_level_measure(aec->out_chunk, chunk);
    aec->last_out_avg_abs = out_level.avg_abs;
    aec->last_out_peak = out_level.peak;
    aec->ref_missing_attenuated_chunks++;
}

static void append_ref_inactive_chunk(nuwax_live_aec_t *aec) {
    if (!aec) {
        return;
    }
    const size_t chunk = (size_t)aec->chunk_samples;
    const nuwax_mic_level_t mic_level =
        (aec->pending_mic && chunk > 0) ? nuwax_mic_level_measure(aec->pending_mic, chunk) : (nuwax_mic_level_t){0};
    const bool preserve_nearend =
        aec->nearend_preserve_hint ||
        aec->strong_nearend_preserve_hint;
    const bool level_nearend = nearend_blend_active(mic_level);
    int32_t atten_q15 = (int32_t)CONFIG_NUWAX_LIVE_AEC_REF_INACTIVE_ATTENUATE_Q15;
    if (preserve_nearend) {
        atten_q15 = 32768;
    } else if (level_nearend &&
        CONFIG_NUWAX_LIVE_AEC_REF_INACTIVE_HINT_ATTENUATE_Q15 > atten_q15) {
        atten_q15 = (int32_t)CONFIG_NUWAX_LIVE_AEC_REF_INACTIVE_HINT_ATTENUATE_Q15;
    }
    if (atten_q15 >= 32768) {
        append_bypass_chunk(aec, NUWAX_LIVE_AEC_OUTPUT_BYPASS);
        aec->ref_inactive_preserved_chunks++;
        return;
    }
    if (!aec->out_chunk || !aec->pending_mic || chunk == 0) {
        return;
    }
    if (atten_q15 <= 0) {
        memset(aec->out_chunk, 0, chunk * sizeof(aec->out_chunk[0]));
    } else {
        for (size_t i = 0; i < chunk; i++) {
            aec->out_chunk[i] = clamp_i16((int32_t)(((int64_t)aec->pending_mic[i] * atten_q15) >> 15));
        }
    }
    const residual_metrics_t zero_metrics = {0};
    (void)apply_dual_mic_nearend_restore(aec, aec->out_chunk, chunk, zero_metrics, false);
    if (!fifo_append(aec,
                     aec->out_chunk,
                     chunk,
                     NUWAX_LIVE_AEC_OUTPUT_BYPASS)) {
        fifo_clear(aec);
        (void)fifo_append(aec,
                          aec->out_chunk,
                          chunk,
                          NUWAX_LIVE_AEC_OUTPUT_BYPASS);
    }
    const nuwax_mic_level_t out_level = nuwax_mic_level_measure(aec->out_chunk, chunk);
    aec->last_out_avg_abs = out_level.avg_abs;
    aec->last_out_peak = out_level.peak;
    aec->ref_inactive_attenuated_chunks++;
}

static void maybe_log_chunk_levels(const nuwax_live_aec_t *aec) {
    if (!aec) {
        return;
    }
    const uint32_t chunks = aec->processed_chunks + aec->bypass_chunks + aec->ref_missing_attenuated_chunks;
    if (chunks == 1U || (chunks % 500U) == 0U) {
        NUWAX_RT_DIAG_LOGI(TAG,
                 "live AEC realtime chunks=%lu processed=%lu bypass=%lu ref=%lu inactive=%lu missing=%lu nearend=%lu hint=%lu corr_q15=%lu mic=%lu/%lu ref_level=%lu/%lu out=%lu/%lu fifo=%u underrun=%lu",
                 (unsigned long)chunks,
                 (unsigned long)aec->processed_chunks,
                 (unsigned long)aec->bypass_chunks,
                 (unsigned long)aec->external_ref_chunks,
                 (unsigned long)aec->external_ref_inactive_chunks,
                 (unsigned long)aec->ref_missing_chunks,
                 (unsigned long)aec->nearend_blend_chunks,
                 (unsigned long)aec->nearend_hint_chunks,
                 (unsigned long)aec->last_mic_ref_corr_q15,
                 (unsigned long)aec->last_mic_avg_abs,
                 (unsigned long)aec->last_mic_peak,
                 (unsigned long)aec->last_ref_avg_abs,
                 (unsigned long)aec->last_ref_peak,
                 (unsigned long)aec->last_out_avg_abs,
                 (unsigned long)aec->last_out_peak,
                 (unsigned)aec->output_count,
                 (unsigned long)aec->output_underrun_frames);
    }
}

static void process_pending_chunk(nuwax_live_aec_t *aec, bool use_external_ref) {
#if CONFIG_NUWAX_LIVE_AEC_ENABLE && (CONFIG_NUWAX_P3_SOFTWARE_REF_ENABLE || CONFIG_NUWAX_VOCAT_SLOT2_AEC_REF_ENABLE)
    const size_t chunk = (size_t)aec->chunk_samples;
    aec->human_rescue_source_count = 0;
    aec->human_rescue_source_valid = false;
    aec->human_rescue_source_fir = false;
    aec->human_rescue_source_kind = NUWAX_LIVE_AEC_RESCUE_SOURCE_RAW;
    const nuwax_mic_level_t mic_level = nuwax_mic_level_measure(aec->pending_mic, chunk);
    update_human_rescue_mix(aec);
    aec->last_mic_avg_abs = mic_level.avg_abs;
    aec->last_mic_peak = mic_level.peak;
    memset(aec->ref_chunk, 0, chunk * sizeof(int16_t));
    size_t copied = 0;
    bool external_ref_selected = false;
    bool ref_level_valid = false;
    nuwax_mic_level_t ref_level = {0};
    if (use_external_ref && aec->pending_ref) {
        memcpy(aec->ref_chunk, aec->pending_ref, chunk * sizeof(int16_t));
        copied = chunk;
        external_ref_selected = true;
        aec->external_ref_chunks++;
        ref_level = nuwax_mic_level_measure(aec->ref_chunk, chunk);
        ref_level_valid = true;
        if (!ref_measured_level_active(ref_level)) {
            aec->external_ref_inactive_chunks++;
            if (aec->internal_software_ref_enabled) {
                memset(aec->ref_chunk, 0, chunk * sizeof(int16_t));
                copied = read_adaptive_software_ref(aec, aec->ref_chunk, chunk);
                external_ref_selected = false;
                ref_level_valid = false;
            }
        }
    } else if (aec->internal_software_ref_enabled) {
        copied = read_adaptive_software_ref(aec, aec->ref_chunk, chunk);
    } else {
        aec->internal_ref_disabled_chunks++;
    }
    if (!ref_level_valid) {
        ref_level = nuwax_mic_level_measure(aec->ref_chunk, chunk);
    }
    const int16_t *fir_ref_chunk =
        external_ref_selected && aec->pending_fir_ref ? aec->pending_fir_ref : aec->ref_chunk;
    aec->last_ref_avg_abs = ref_level.avg_abs;
    aec->last_ref_peak = ref_level.peak;
    if (copied < (chunk / 2U)) {
        aec->ref_missing_chunks++;
        build_scalar_human_rescue_source(aec, aec->ref_chunk, chunk);
        advance_human_rescue_fir_reference(aec, fir_ref_chunk, chunk);
        append_ref_missing_chunk(aec);
        maybe_log_chunk_levels(aec);
        return;
    }
    if (!ref_measured_level_active(ref_level)) {
        build_scalar_human_rescue_source(aec, aec->ref_chunk, chunk);
        advance_human_rescue_fir_reference(aec, fir_ref_chunk, chunk);
        append_ref_inactive_chunk(aec);
        maybe_log_chunk_levels(aec);
        return;
    }

    size_t aec_output_samples = chunk;
#if NUWAX_LIVE_AEC_BACKEND_AFE
    for (size_t i = 0; i < chunk; i++) {
        aec->mr_chunk[i * 2U] = aec->pending_mic[i];
        aec->mr_chunk[i * 2U + 1U] = aec->ref_chunk[i];
    }
    size_t out_bytes = afe_aec_process((afe_aec_handle_t *)aec->aec,
                                       aec->mr_chunk,
                                       aec->out_chunk);
    size_t out_samples = out_bytes / sizeof(int16_t);
    if (out_samples > chunk) {
        out_samples = chunk;
    }
    aec_output_samples = out_samples;
    if (out_samples < chunk) {
        memcpy(&aec->out_chunk[out_samples],
               &aec->pending_mic[out_samples],
               (chunk - out_samples) * sizeof(int16_t));
        aec->output_underrun_frames++;
        if (aec->output_underrun_frames == 1 ||
            (aec->output_underrun_frames % 100U) == 0) {
            ESP_LOGW(TAG,
                     "AFE short output samples=%u expected=%u; preserved raw mic tail count=%lu",
                     (unsigned)out_samples,
                     (unsigned)chunk,
                     (unsigned long)aec->output_underrun_frames);
        }
        out_samples = chunk;
    }
#else
    aec_process((aec_handle_t *)aec->aec,
                aec->pending_mic,
                aec->ref_chunk,
                aec->out_chunk);
    const size_t out_samples = chunk;
#endif
    const bool nearend_active = aec->nearend_preserve_hint;
    const residual_metrics_t mic_ref_metrics =
        compute_residual_metrics(aec->pending_mic,
                                 aec->ref_chunk,
                                 out_samples,
                                 CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_MAX_REF_GAIN_Q15);
    aec->last_mic_ref_corr_q15 = mic_ref_metrics.corr_q15;
    aec->last_mic_ref_gain_q15 = mic_ref_metrics.gain_q15;
    update_human_rescue_ref_gain(aec, mic_ref_metrics);
    const int64_t fir_dsp_started_us = esp_timer_get_time();
    prepare_human_rescue_source(aec,
                                aec->ref_chunk,
                                fir_ref_chunk,
                                out_samples,
                                mic_ref_metrics);
    const int64_t fir_dsp_elapsed_us = esp_timer_get_time() - fir_dsp_started_us;
    if (aec->human_rescue_fir_available && fir_dsp_elapsed_us >= 0) {
        const uint32_t elapsed_us = fir_dsp_elapsed_us > UINT32_MAX
                                        ? UINT32_MAX
                                        : (uint32_t)fir_dsp_elapsed_us;
        aec->human_rescue_fir_dsp_chunks++;
        aec->human_rescue_fir_dsp_us_sum += elapsed_us;
        if (elapsed_us > aec->human_rescue_fir_dsp_us_max) {
            aec->human_rescue_fir_dsp_us_max = elapsed_us;
        }
    }
    apply_residual_ref_cancel(aec, aec->out_chunk, aec->ref_chunk, out_samples, nearend_active);
    const nuwax_mic_level_t pre_blend_out_level = nuwax_mic_level_measure(aec->out_chunk, out_samples);
    apply_nearend_blend(aec,
                        aec->pending_mic,
                        aec->out_chunk,
                        out_samples,
                        mic_level,
                        pre_blend_out_level,
                        mic_ref_metrics);
    const bool human_rescued =
        apply_human_rescue(aec,
                           aec->out_chunk,
                           aec->ref_chunk,
                           out_samples,
                           mic_ref_metrics);
    if (human_rescued) {
        apply_post_blend_residual_cancel(aec, aec->out_chunk, aec->ref_chunk, out_samples);
    } else {
        const bool dual_restored =
            apply_dual_mic_nearend_restore(aec, aec->out_chunk, out_samples, mic_ref_metrics, true);
        if (dual_restored) {
            aec->dual_restore_post_skip_chunks++;
            apply_dual_restore_residual_guard(aec, aec->out_chunk, aec->ref_chunk, out_samples);
        } else {
            const bool floor_restored =
                apply_nearend_floor_restore(aec, aec->out_chunk, out_samples, mic_ref_metrics);
            if (floor_restored) {
                apply_dual_restore_residual_guard(aec, aec->out_chunk, aec->ref_chunk, out_samples);
            } else {
                apply_post_blend_residual_cancel(aec, aec->out_chunk, aec->ref_chunk, out_samples);
            }
        }
    }
    apply_output_gain(aec, aec->out_chunk, out_samples);
    const nuwax_mic_level_t out_level = nuwax_mic_level_measure(aec->out_chunk, out_samples);
    aec->last_out_avg_abs = out_level.avg_abs;
    aec->last_out_peak = out_level.peak;
    if (!fifo_append_pair(aec,
                          aec->out_chunk,
                          out_samples,
                          NUWAX_LIVE_AEC_OUTPUT_AEC,
                          aec_output_samples,
                          NUWAX_LIVE_AEC_OUTPUT_UNDERRUN)) {
        fifo_clear(aec);
        (void)fifo_append_pair(aec,
                               aec->out_chunk,
                               out_samples,
                               NUWAX_LIVE_AEC_OUTPUT_AEC,
                               aec_output_samples,
                               NUWAX_LIVE_AEC_OUTPUT_UNDERRUN);
    }
    aec->processed_chunks++;
    maybe_log_chunk_levels(aec);
#else
    append_bypass_chunk(aec, NUWAX_LIVE_AEC_OUTPUT_BYPASS);
#endif
}

static esp_err_t live_aec_process_internal(nuwax_live_aec_t *aec,
                                            int16_t *samples,
                                            const int16_t *mic2_samples,
                                            const int16_t *ref_samples,
                                            const int16_t *fir_ref_samples,
                                            size_t sample_count,
                                           bool playback_active,
                                           bool use_external_ref,
                                           bool use_dual_mic) {
    if (aec) {
        nuwax_live_aec_output_stats_reset(&aec->last_output);
        rescue_provenance_clear(&aec->last_human_rescue_output);
        aec->human_rescue_output_count = 0U;
        aec->last_output_emitted = false;
    }
    if (!aec || !samples || sample_count == 0) {
        return ESP_OK;
    }
    if (!aec->enabled) {
        nuwax_live_aec_output_stats_account(&aec->last_output,
                                            NUWAX_LIVE_AEC_OUTPUT_BYPASS,
                                            sample_count);
        aec->last_output_emitted = true;
        return ESP_OK;
    }
    if (!playback_active) {
        if (aec->human_rescue_fir_initialized) {
            aec->human_rescue_fir_last_session_ready = aec->human_rescue_fir_ready;
            aec->human_rescue_fir_last_session_training_samples =
                aec->human_rescue_fir_session_training_samples;
            aec->human_rescue_fir_last_session_raw_q15 =
                aec->human_rescue_fir_residual_raw_ema_q15;
            aec->human_rescue_fir_last_session_scalar_q15 =
                aec->human_rescue_fir_residual_scalar_ema_q15;
            aec->human_rescue_fir_last_session_prediction_corr_q15 =
                aec->human_rescue_fir_prediction_corr_q15;
            aec->human_rescue_fir_last_session_coeff_peak_q15 =
                aec->human_rescue_fir_coeff_peak_q15;
            aec->human_rescue_fir_last_session_coeff_peak_tap =
                aec->human_rescue_fir_coeff_peak_tap;
            aec->human_rescue_fir_last_session_edge_energy_q15 =
                aec->human_rescue_fir_edge_energy_q15;
        }
        aec->pending_count = 0;
        aec->pending_external_ref = false;
        aec->pending_dual_mic = false;
        aec->provisional_human_protect = false;
        aec->fast_human_protect = false;
        aec->confirmed_human_protect = false;
        aec->human_rescue_mix_q15 = 0;
        aec->human_rescue_ref_gain_valid = false;
        aec->human_rescue_ref_gain_q15 = 0;
        if (aec->human_rescue_fir_initialized ||
            aec->human_rescue_fir_session_training_samples > 0 ||
            aec->human_rescue_source_valid) {
            reset_human_rescue_fir_session(aec);
        }
        fifo_clear(aec);
        nuwax_live_aec_output_stats_account(&aec->last_output,
                                            NUWAX_LIVE_AEC_OUTPUT_BYPASS,
                                            sample_count);
        aec->last_output_emitted = true;
        return ESP_OK;
    }
    if (use_external_ref && !ref_samples) {
        use_external_ref = false;
    }
    if (use_dual_mic && !mic2_samples) {
        use_dual_mic = false;
    }
    esp_err_t err = ensure_live_aec(aec);
    if (err != ESP_OK) {
        nuwax_live_aec_output_stats_account(&aec->last_output,
                                            NUWAX_LIVE_AEC_OUTPUT_BYPASS,
                                            sample_count);
        return err;
    }
    const bool external_ref_mode_changed = aec->pending_external_ref != use_external_ref;
    const bool dual_mic_mode_changed = aec->pending_dual_mic != use_dual_mic;
    if (aec->pending_count > 0 &&
        (external_ref_mode_changed || dual_mic_mode_changed)) {
        aec->pending_count = 0;
        if (dual_mic_mode_changed) {
            fifo_clear(aec);
        }
    }
    aec->pending_external_ref = use_external_ref;
    aec->pending_dual_mic = use_dual_mic;

    size_t input_offset = 0U;
    const size_t chunk_samples = (size_t)aec->chunk_samples;
    while (input_offset < sample_count) {
        const size_t room = chunk_samples - aec->pending_count;
        const size_t remaining = sample_count - input_offset;
        const size_t copy_samples = remaining < room ? remaining : room;
        const size_t copy_bytes = copy_samples * sizeof(samples[0]);

        memcpy(&aec->pending_mic[aec->pending_count],
               &samples[input_offset],
               copy_bytes);
        if (aec->pending_mic2) {
            if (use_dual_mic) {
                memcpy(&aec->pending_mic2[aec->pending_count],
                       &mic2_samples[input_offset],
                       copy_bytes);
            } else {
                memset(&aec->pending_mic2[aec->pending_count], 0, copy_bytes);
            }
        }
        if (use_external_ref && aec->pending_ref) {
            memcpy(&aec->pending_ref[aec->pending_count],
                   &ref_samples[input_offset],
                   copy_bytes);
            memcpy(&aec->pending_fir_ref[aec->pending_count],
                   fir_ref_samples ? &fir_ref_samples[input_offset]
                                   : &ref_samples[input_offset],
                   copy_bytes);
        }

        aec->pending_count += copy_samples;
        input_offset += copy_samples;
        if (aec->pending_count == chunk_samples) {
            process_pending_chunk(aec, use_external_ref);
            aec->pending_count = 0;
        }
    }

    const size_t release_threshold = sample_count;
    if (!aec->output_started) {
        if (aec->output_count < release_threshold) {
            aec->output_underrun_frames++;
            nuwax_live_aec_output_stats_account(&aec->last_output,
                                                NUWAX_LIVE_AEC_OUTPUT_UNDERRUN,
                                                sample_count);
            return ESP_OK;
        }
        aec->output_started = true;
    }

    if (aec->output_count < sample_count ||
        aec->human_rescue_count < sample_count) {
        aec->output_underrun_frames++;
        nuwax_live_aec_output_stats_account(&aec->last_output,
                                            NUWAX_LIVE_AEC_OUTPUT_UNDERRUN,
                                            sample_count);
        return ESP_OK;
    }

    const size_t copied = fifo_read(aec, samples, sample_count);
    if (copied < sample_count) {
        /* The untouched tail still contains this frame's raw mic samples. */
        aec->output_underrun_frames++;
        nuwax_live_aec_output_stats_account(&aec->last_output,
                                            NUWAX_LIVE_AEC_OUTPUT_UNDERRUN,
                                            sample_count - copied);
        return ESP_OK;
    }
    aec->last_output_emitted = true;
    return ESP_OK;
}

esp_err_t nuwax_live_aec_process(nuwax_live_aec_t *aec,
                                 int16_t *samples,
                                 size_t sample_count,
                                 bool playback_active) {
    return live_aec_process_internal(aec,
                                     samples,
                                     NULL,
                                     NULL,
                                     NULL,
                                     sample_count,
                                     playback_active,
                                     false,
                                     false);
}

esp_err_t nuwax_live_aec_process_with_dual_mic(nuwax_live_aec_t *aec,
                                               int16_t *samples,
                                               const int16_t *mic2_samples,
                                               size_t sample_count,
                                               bool playback_active) {
    return live_aec_process_internal(aec,
                                     samples,
                                     mic2_samples,
                                     NULL,
                                     NULL,
                                     sample_count,
                                     playback_active,
                                     false,
                                     true);
}

esp_err_t nuwax_live_aec_process_with_ref(nuwax_live_aec_t *aec,
                                          int16_t *samples,
                                          const int16_t *ref_samples,
                                          size_t sample_count,
                                          bool playback_active) {
    return live_aec_process_internal(aec,
                                     samples,
                                     NULL,
                                     ref_samples,
                                     ref_samples,
                                     sample_count,
                                     playback_active,
                                     true,
                                     false);
}

esp_err_t nuwax_live_aec_process_with_ref_and_dual_mic(nuwax_live_aec_t *aec,
                                                       int16_t *samples,
                                                       const int16_t *mic2_samples,
                                                       const int16_t *ref_samples,
                                                       size_t sample_count,
                                                       bool playback_active) {
    return live_aec_process_internal(aec,
                                     samples,
                                     mic2_samples,
                                     ref_samples,
                                     ref_samples,
                                     sample_count,
                                     playback_active,
                                     true,
                                     true);
}

esp_err_t nuwax_live_aec_process_with_refs_and_dual_mic(nuwax_live_aec_t *aec,
                                                        int16_t *samples,
                                                        const int16_t *mic2_samples,
                                                        const int16_t *aec_ref_samples,
                                                        const int16_t *fir_ref_samples,
                                                        size_t sample_count,
                                                        bool playback_active) {
    return live_aec_process_internal(aec,
                                     samples,
                                     mic2_samples,
                                     aec_ref_samples,
                                     fir_ref_samples,
                                     sample_count,
                                     playback_active,
                                     true,
                                     true);
}

void nuwax_live_aec_log_summary(const nuwax_live_aec_t *aec) {
    if (!aec || !aec->enabled) {
        return;
    }
    const uint32_t rescue_mix_avg_q15 =
        aec->human_rescue_chunks > 0
            ? aec->human_rescue_mix_sum_q15 / aec->human_rescue_chunks
            : 0;
    const uint32_t dual_restore_gain_avg_q15 =
        aec->dual_restore_chunks > 0
            ? aec->dual_restore_gain_sum_q15 / aec->dual_restore_chunks
            : 0;
    const uint32_t fir_dsp_avg_us =
        aec->human_rescue_fir_dsp_chunks > 0
            ? (uint32_t)(aec->human_rescue_fir_dsp_us_sum /
                         aec->human_rescue_fir_dsp_chunks)
            : 0;
    ESP_LOGI(TAG,
             "live AEC summary processed=%lu bypass=%lu ext_ref=%lu ext_inactive=%lu ref_missing=%lu inactive_preserved=%lu adaptive_chunks=%lu adaptive_scans=%lu adaptive_last_offset=%ld adaptive_corr=%lu/%lu rescue_chunks=%lu provisional=%lu fast=%lu confirmed=%lu release=%lu rescue_mix_avg=%lu rescue_mix_max=%lu rescue_ref_gain_valid=%d rescue_ref_gain=%ld rescue_ref_updates=%lu rescue_ref_rejects=%lu rescue_ref_latches=%lu rescue_no_ref_gain=%lu post_residual=%lu post_corr=%lu post_gain=%ld dual_restore=%lu dual_hint=%lu dual_gain_avg=%lu dual_score_max=%lu clipped=%lu underrun=%lu",
             (unsigned long)aec->processed_chunks,
             (unsigned long)aec->bypass_chunks,
             (unsigned long)aec->external_ref_chunks,
             (unsigned long)aec->external_ref_inactive_chunks,
             (unsigned long)aec->ref_missing_chunks,
             (unsigned long)aec->ref_inactive_preserved_chunks,
             (unsigned long)aec->adaptive_ref_chunks,
             (unsigned long)aec->adaptive_ref_scans,
             (long)aec->adaptive_ref_last_offset_samples,
             (unsigned long)aec->adaptive_ref_last_base_corr_q15,
             (unsigned long)aec->adaptive_ref_last_best_corr_q15,
             (unsigned long)aec->human_rescue_chunks,
             (unsigned long)aec->human_rescue_provisional_chunks,
             (unsigned long)aec->human_rescue_fast_chunks,
             (unsigned long)aec->human_rescue_confirmed_chunks,
             (unsigned long)aec->human_rescue_release_chunks,
             (unsigned long)rescue_mix_avg_q15,
             (unsigned long)aec->human_rescue_mix_max_q15,
             aec->human_rescue_ref_gain_valid ? 1 : 0,
             (long)aec->last_human_rescue_ref_gain_q15,
             (unsigned long)aec->human_rescue_ref_gain_updates,
             (unsigned long)aec->human_rescue_ref_gain_rejects,
             (unsigned long)aec->human_rescue_ref_gain_latches,
             (unsigned long)aec->human_rescue_no_ref_gain_chunks,
             (unsigned long)aec->post_blend_residual_cancel_chunks,
             (unsigned long)aec->last_post_blend_residual_corr_q15,
             (long)aec->last_post_blend_residual_gain_q15,
             (unsigned long)aec->dual_restore_chunks,
             (unsigned long)aec->dual_restore_hint_chunks,
             (unsigned long)dual_restore_gain_avg_q15,
             (unsigned long)aec->dual_restore_score_max_q15,
             (unsigned long)aec->clipped_samples,
             (unsigned long)aec->output_underrun_frames);
    ESP_LOGI(TAG,
             "human rescue FIR summary available=%d ready=%d validating=%d validation_remaining=%lu warmup=%lu/%d session_train=%lu train_samples=%lu train_chunks=%lu freeze_events=%lu frozen_chunks=%lu ready_events=%lu ready_lost=%lu nearend_events=%lu nearend_hold=%lu health_resets=%lu validation=%lu/%lu/%lu epoch_resets=%lu reanchors=%lu/%lu fir_rescue=%lu fallback=%lu divergences=%lu bad_health=%lu ready_bad=%lu ready_health=%lu/%lu/%lu/%lu/%lu gate_corr_q15=%lu fir_ref_corr_q15=%lu residual_raw_q15=%lu residual_scalar_q15=%lu train_raw_q15=%lu train_scalar_q15=%lu prediction_corr_q15=%lu coeff_peak=%lu/%lu edge_energy_q15=%lu last_validation=%d/%lu/%lu/%lu last_session=%d/%lu/%lu/%lu/%lu/%lu/%lu/%lu dsp_us=%lu/%lu dsp_chunks=%lu",
             aec->human_rescue_fir_available ? 1 : 0,
             aec->human_rescue_fir_ready ? 1 : 0,
             aec->human_rescue_fir_validating ? 1 : 0,
             (unsigned long)aec->human_rescue_fir_validation_remaining,
             (unsigned long)aec->human_rescue_fir_warmup_samples,
             CONFIG_NUWAX_LIVE_AEC_HUMAN_RESCUE_FIR_WARMUP_SAMPLES,
             (unsigned long)aec->human_rescue_fir_session_training_samples,
             (unsigned long)aec->human_rescue_fir_training_samples,
             (unsigned long)aec->human_rescue_fir_training_chunks,
             (unsigned long)aec->human_rescue_fir_freeze_events,
             (unsigned long)aec->human_rescue_fir_frozen_chunks,
             (unsigned long)aec->human_rescue_fir_ready_events,
             (unsigned long)aec->human_rescue_fir_ready_lost_events,
             (unsigned long)aec->human_rescue_fir_nearend_events,
             (unsigned long)aec->human_rescue_fir_nearend_hold_chunks,
             (unsigned long)aec->human_rescue_fir_health_boundary_resets,
             (unsigned long)aec->human_rescue_fir_validation_starts,
             (unsigned long)aec->human_rescue_fir_validation_passes,
             (unsigned long)aec->human_rescue_fir_validation_failures,
             (unsigned long)aec->human_rescue_fir_reference_epoch_resets,
             (unsigned long)aec->human_rescue_fir_reference_reanchors,
             (unsigned long)aec->human_rescue_fir_reference_retrain_reanchors,
             (unsigned long)aec->human_rescue_fir_rescue_chunks,
             (unsigned long)aec->human_rescue_fir_fallback_chunks,
             (unsigned long)aec->human_rescue_fir_divergences,
             (unsigned long)aec->human_rescue_fir_bad_health_windows,
             (unsigned long)aec->human_rescue_fir_ready_bad_windows,
             (unsigned long)aec->human_rescue_fir_ready_health_windows,
             (unsigned long)aec->human_rescue_fir_ready_health_failures,
             (unsigned long)aec->human_rescue_fir_ready_health_chunks,
             (unsigned long)aec->human_rescue_fir_last_ready_health_raw_q15,
             (unsigned long)aec->human_rescue_fir_last_ready_health_scalar_q15,
             (unsigned long)aec->human_rescue_fir_last_gate_corr_q15,
             (unsigned long)aec->human_rescue_fir_last_ref_corr_q15,
             (unsigned long)aec->human_rescue_fir_last_residual_raw_q15,
             (unsigned long)aec->human_rescue_fir_last_residual_scalar_q15,
             (unsigned long)aec->human_rescue_fir_residual_raw_ema_q15,
             (unsigned long)aec->human_rescue_fir_residual_scalar_ema_q15,
             (unsigned long)aec->human_rescue_fir_prediction_corr_q15,
             (unsigned long)aec->human_rescue_fir_coeff_peak_q15,
             (unsigned long)aec->human_rescue_fir_coeff_peak_tap,
             (unsigned long)aec->human_rescue_fir_edge_energy_q15,
             aec->human_rescue_fir_last_validation_passed ? 1 : 0,
             (unsigned long)aec->human_rescue_fir_last_validation_raw_q15,
             (unsigned long)aec->human_rescue_fir_last_validation_scalar_q15,
             (unsigned long)aec->human_rescue_fir_last_validation_prediction_corr_q15,
             aec->human_rescue_fir_last_session_ready ? 1 : 0,
             (unsigned long)aec->human_rescue_fir_last_session_training_samples,
             (unsigned long)aec->human_rescue_fir_last_session_raw_q15,
             (unsigned long)aec->human_rescue_fir_last_session_scalar_q15,
             (unsigned long)aec->human_rescue_fir_last_session_prediction_corr_q15,
             (unsigned long)aec->human_rescue_fir_last_session_coeff_peak_q15,
             (unsigned long)aec->human_rescue_fir_last_session_coeff_peak_tap,
             (unsigned long)aec->human_rescue_fir_last_session_edge_energy_q15,
             (unsigned long)fir_dsp_avg_us,
             (unsigned long)aec->human_rescue_fir_dsp_us_max,
             (unsigned long)aec->human_rescue_fir_dsp_chunks);
}

void nuwax_live_aec_deinit(nuwax_live_aec_t *aec) {
    if (!aec) {
        return;
    }
#if CONFIG_NUWAX_LIVE_AEC_ENABLE && (CONFIG_NUWAX_P3_SOFTWARE_REF_ENABLE || CONFIG_NUWAX_VOCAT_SLOT2_AEC_REF_ENABLE)
    if (aec->aec) {
#if NUWAX_LIVE_AEC_BACKEND_AFE
        afe_aec_destroy((afe_aec_handle_t *)aec->aec);
#else
        aec_destroy((aec_handle_t *)aec->aec);
#endif
    }
#endif
    heap_caps_free(aec->pending_mic);
    heap_caps_free(aec->pending_mic2);
    heap_caps_free(aec->pending_ref);
    heap_caps_free(aec->pending_fir_ref);
    heap_caps_free(aec->ref_chunk);
    heap_caps_free(aec->probe_ref_chunk);
    heap_caps_free(aec->best_ref_chunk);
    heap_caps_free(aec->mr_chunk);
    heap_caps_free(aec->out_chunk);
    heap_caps_free(aec->human_rescue_source_chunk);
    heap_caps_free(aec->human_rescue_fir_coeffs);
    heap_caps_free(aec->human_rescue_fir_ref_history);
    heap_caps_free(aec->output_fifo);
    heap_caps_free(aec->human_rescue_fifo);
    heap_caps_free(aec->human_rescue_output_frame);
    memset(aec, 0, sizeof(*aec));
}
