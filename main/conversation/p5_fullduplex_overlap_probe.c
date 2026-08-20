#include "p5_fullduplex_overlap_probe.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_frame.h"
#include "conversation/audio_terminal_recovery.h"
#include "audio_uplink.h"
#include "conversation/speaker_playback.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mic_input.h"
#include "opus_encoder_wrapper.h"
#include "p3_software_ref.h"
#include "session_protocol.h"

#ifndef CONFIG_NUWAX_P5_FULLDUPLEX_OVERLAP_PROBE_ENABLE
#define CONFIG_NUWAX_P5_FULLDUPLEX_OVERLAP_PROBE_ENABLE 0
#endif

#ifndef CONFIG_NUWAX_P3_SOFTWARE_REF_AEC_AB_ENABLE
#define CONFIG_NUWAX_P3_SOFTWARE_REF_AEC_AB_ENABLE 0
#endif

#ifndef CONFIG_NUWAX_P3_SOFTWARE_REF_CAPACITY_MS
#define CONFIG_NUWAX_P3_SOFTWARE_REF_CAPACITY_MS 2000
#endif

#ifndef CONFIG_NUWAX_VOICE_OUTPUT_SAMPLE_RATE
#define CONFIG_NUWAX_VOICE_OUTPUT_SAMPLE_RATE 16000
#endif

#ifndef CONFIG_NUWAX_P3_SOFTREF_AB_MIN_REF_RMS
#define CONFIG_NUWAX_P3_SOFTREF_AB_MIN_REF_RMS 20
#endif

#ifndef CONFIG_NUWAX_P3_SOFTREF_AB_MAX_UNDERRUN_RATIO
#define CONFIG_NUWAX_P3_SOFTREF_AB_MAX_UNDERRUN_RATIO 10
#endif

#ifndef CONFIG_NUWAX_P3_SOFTREF_AB_MIN_ERLE_DB
#define CONFIG_NUWAX_P3_SOFTREF_AB_MIN_ERLE_DB 600
#endif

#ifndef CONFIG_NUWAX_P3_SOFTREF_AB_MIN_ACTIVE_REF_RATIO
#define CONFIG_NUWAX_P3_SOFTREF_AB_MIN_ACTIVE_REF_RATIO 600
#endif

#ifndef CONFIG_NUWAX_P3_SOFTREF_AB_MAX_CORR_AFTER_ABS
#define CONFIG_NUWAX_P3_SOFTREF_AB_MAX_CORR_AFTER_ABS 100
#endif

#define NUWAX_CONFIG_RATIO_X1000(value) ((float)(value) / 1000.0f)
#define NUWAX_CONFIG_DB_X100(value) ((float)(value) / 100.0f)
#define NUWAX_P5_CONTROL_SEND_TIMEOUT_US 1000000LL

#ifndef CONFIG_NUWAX_P3_SOFTREF_AB_MIN_INTERNAL_FREE
#define CONFIG_NUWAX_P3_SOFTREF_AB_MIN_INTERNAL_FREE 49152
#endif

#ifndef CONFIG_NUWAX_P3_SOFTREF_AB_MIN_INTERNAL_LARGEST
#define CONFIG_NUWAX_P3_SOFTREF_AB_MIN_INTERNAL_LARGEST 16384
#endif

#ifndef CONFIG_NUWAX_P3_SOFTREF_AB_AEC_FILTER_LENGTH
#define CONFIG_NUWAX_P3_SOFTREF_AB_AEC_FILTER_LENGTH 4
#endif

#ifndef CONFIG_NUWAX_P6_LIVE_CANCEL_WINDOW_MS
#define CONFIG_NUWAX_P6_LIVE_CANCEL_WINDOW_MS 200U
#endif

#if CONFIG_NUWAX_P3_SOFTWARE_REF_AEC_AB_ENABLE
#include "esp_afe_aec.h"
#endif

#if CONFIG_NUWAX_P3_SOFTREF_AB_AEC_TYPE_VC
#define NUWAX_P5_LIVE_AEC_TYPE AFE_TYPE_VC
#else
#define NUWAX_P5_LIVE_AEC_TYPE AFE_TYPE_SR
#endif

#if CONFIG_NUWAX_P3_SOFTREF_AB_AEC_MODE_HIGH_PERF
#define NUWAX_P5_LIVE_AEC_MODE AFE_MODE_HIGH_PERF
#else
#define NUWAX_P5_LIVE_AEC_MODE AFE_MODE_LOW_COST
#endif

#define NUWAX_P5_OVERLAP_DEFAULT_DURATION_MS 1200U
#define NUWAX_P5_OVERLAP_MIN_DURATION_MS 500U
#define NUWAX_P5_OVERLAP_MAX_DURATION_MS 5000U
#define NUWAX_P5_OVERLAP_MODE "p5_fullduplex_overlap_probe"

static const char *TAG = "NuwaxPartner6";

static uint32_t s_p5_probe_seq;

typedef struct {
    uint64_t sumsq_mic;
    uint64_t sumsq_ref;
    uint64_t sumsq_out;
    int64_t dot_mic_ref;
    int64_t dot_out_ref;
    int32_t mic_peak;
    int32_t ref_peak;
    int32_t out_peak;
    size_t samples;
} p5_live_aec_accum_t;

typedef struct {
    uint64_t sumsq_mic1;
    uint64_t sumsq_mic2;
    uint64_t sumsq_diff;
    int64_t dot_mic12;
    size_t samples;
} p5_dual_mic_accum_t;

typedef struct {
    bool enabled;
    bool ready;
    uint32_t delay_samples;
    uint32_t target_window_samples;
    uint32_t window_frames;
    uint32_t quiet_frames;
    size_t window_ref_copied;
    size_t window_ref_underrun;
    p5_live_aec_accum_t window_acc;
    p5_live_aec_accum_t active_acc;
    p5_live_aec_accum_t quiet_acc;
    p5_dual_mic_accum_t window_dual_acc;
    p5_dual_mic_accum_t active_dual_acc;
#if CONFIG_NUWAX_P3_SOFTWARE_REF_AEC_AB_ENABLE
    afe_aec_handle_t *aec;
#endif
    int chunk_samples;
    int16_t *ref_buf;
    int16_t *mr_buf;
    int16_t *out_buf;
} p5_live_cancel_detector_t;

static uint32_t clamp_duration_ms(uint32_t duration_ms) {
    if (duration_ms == 0) {
        duration_ms = NUWAX_P5_OVERLAP_DEFAULT_DURATION_MS;
    }
    if (duration_ms < NUWAX_P5_OVERLAP_MIN_DURATION_MS) {
        return NUWAX_P5_OVERLAP_MIN_DURATION_MS;
    }
    if (duration_ms > NUWAX_P5_OVERLAP_MAX_DURATION_MS) {
        return NUWAX_P5_OVERLAP_MAX_DURATION_MS;
    }
    return duration_ms;
}

static void set_result(nuwax_p5_fullduplex_overlap_probe_result_t *result, const char *text) {
    if (!result) {
        return;
    }
    strlcpy(result->result, text ? text : "-", sizeof(result->result));
}

static void make_audio_session_id(char *out, size_t out_size, const char *requested, const char *request_id) {
    if (!out || out_size == 0) {
        return;
    }
    if (requested && requested[0] != '\0') {
        strlcpy(out, requested, out_size);
        return;
    }
    const uint32_t seq = ++s_p5_probe_seq;
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (request_id && request_id[0] != '\0') {
        snprintf(out, out_size, "p5ovl_%lu_%s", (unsigned long)seq, request_id);
    } else {
        snprintf(out, out_size, "p5ovl_%lu_%lu", (unsigned long)seq, (unsigned long)now_ms);
    }
    out[out_size - 1] = '\0';
}

static float rms_from_sum(uint64_t sum_sq, uint64_t sample_count) {
    if (sample_count == 0) {
        return 0.0f;
    }
    return (float)sqrt((double)sum_sq / (double)sample_count);
}

static float db_ratio(float numerator, float denominator) {
    if (numerator <= 0.0f) {
        return -120.0f;
    }
    if (denominator <= 0.0f) {
        return 120.0f;
    }
    return 20.0f * log10f(numerator / denominator);
}

static uint32_t frame_count_for_duration(uint32_t duration_ms, uint32_t frame_ms) {
    if (frame_ms == 0) {
        return 1;
    }
    uint32_t frames = duration_ms / frame_ms;
    if ((duration_ms % frame_ms) != 0) {
        frames++;
    }
    return frames == 0 ? 1 : frames;
}

static uint32_t delta_u32(uint32_t after, uint32_t before) {
    return after >= before ? after - before : 0;
}

static uint32_t monotonic_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static int32_t abs_pcm16(int16_t value) {
    const int32_t v = value;
    return v < 0 ? -v : v;
}

static void dual_mic_accumulate(p5_dual_mic_accum_t *acc,
                                const int16_t *mic1,
                                const int16_t *mic2,
                                size_t sample_count) {
    if (!acc || !mic1 || !mic2) {
        return;
    }
    for (size_t i = 0; i < sample_count; i++) {
        const int32_t a = mic1[i];
        const int32_t b = mic2[i];
        const int32_t d = a - b;
        acc->sumsq_mic1 += (uint64_t)((int64_t)a * (int64_t)a);
        acc->sumsq_mic2 += (uint64_t)((int64_t)b * (int64_t)b);
        acc->sumsq_diff += (uint64_t)((int64_t)d * (int64_t)d);
        acc->dot_mic12 += (int64_t)a * (int64_t)b;
    }
    acc->samples += sample_count;
}

static void dual_mic_merge(p5_dual_mic_accum_t *dst, const p5_dual_mic_accum_t *src) {
    if (!dst || !src) {
        return;
    }
    dst->sumsq_mic1 += src->sumsq_mic1;
    dst->sumsq_mic2 += src->sumsq_mic2;
    dst->sumsq_diff += src->sumsq_diff;
    dst->dot_mic12 += src->dot_mic12;
    dst->samples += src->samples;
}

static float dual_mic_corr(const p5_dual_mic_accum_t *acc) {
    if (!acc || acc->samples == 0 || acc->sumsq_mic1 == 0 || acc->sumsq_mic2 == 0) {
        return 0.0f;
    }
    const double denom = sqrt((double)acc->sumsq_mic1 * (double)acc->sumsq_mic2);
    if (denom <= 0.0) {
        return 0.0f;
    }
    return (float)((double)acc->dot_mic12 / denom);
}

static void fill_dual_mic_result(nuwax_p5_fullduplex_overlap_probe_result_t *result,
                                 const p5_dual_mic_accum_t *acc) {
    if (!result || !acc || acc->samples == 0) {
        return;
    }
    result->dual_mic_ready = true;
    result->mic2_rms = rms_from_sum(acc->sumsq_mic2, acc->samples);
    result->dual_mic_corr = dual_mic_corr(acc);
    result->dual_mic_abs_corr = fabsf(result->dual_mic_corr);
    result->dual_mic_level_delta_db = db_ratio(result->mic_rms, result->mic2_rms);
    result->dual_mic_diff_rms = rms_from_sum(acc->sumsq_diff, acc->samples);
    const float max_mic_rms = result->mic_rms > result->mic2_rms ? result->mic_rms : result->mic2_rms;
    result->dual_mic_diff_ratio = max_mic_rms > 1.0f ? result->dual_mic_diff_rms / max_mic_rms : 0.0f;
    /*
     * Shadow-only discriminator for P6.5. A nearby user usually produces more
     * inter-mic difference than playback residue/common-mode room sound. Keep it
     * out of cancel authorization until real logs confirm the separation.
     */
    result->dual_mic_voice_ok =
        result->mic_rms >= 1800.0f &&
        result->mic2_rms >= 200.0f &&
        (fabsf(result->dual_mic_level_delta_db) >= 2.0f ||
         result->dual_mic_abs_corr <= 0.90f ||
         (result->dual_mic_diff_ratio >= 0.40f && result->dual_mic_abs_corr <= 0.94f));
}

static void live_accumulate(p5_live_aec_accum_t *acc,
                            const int16_t *mic,
                            const int16_t *ref,
                            const int16_t *out,
                            size_t sample_count) {
    if (!acc || !mic || !ref || !out) {
        return;
    }
    for (size_t i = 0; i < sample_count; i++) {
        const int32_t m = mic[i];
        const int32_t r = ref[i];
        const int32_t o = out[i];
        const int32_t am = abs_pcm16(mic[i]);
        const int32_t ar = abs_pcm16(ref[i]);
        const int32_t ao = abs_pcm16(out[i]);
        if (am > acc->mic_peak) {
            acc->mic_peak = am;
        }
        if (ar > acc->ref_peak) {
            acc->ref_peak = ar;
        }
        if (ao > acc->out_peak) {
            acc->out_peak = ao;
        }
        acc->sumsq_mic += (uint64_t)((int64_t)m * (int64_t)m);
        acc->sumsq_ref += (uint64_t)((int64_t)r * (int64_t)r);
        acc->sumsq_out += (uint64_t)((int64_t)o * (int64_t)o);
        acc->dot_mic_ref += (int64_t)m * (int64_t)r;
        acc->dot_out_ref += (int64_t)o * (int64_t)r;
    }
    acc->samples += sample_count;
}

static void live_merge(p5_live_aec_accum_t *dst, const p5_live_aec_accum_t *src) {
    if (!dst || !src) {
        return;
    }
    dst->sumsq_mic += src->sumsq_mic;
    dst->sumsq_ref += src->sumsq_ref;
    dst->sumsq_out += src->sumsq_out;
    dst->dot_mic_ref += src->dot_mic_ref;
    dst->dot_out_ref += src->dot_out_ref;
    if (src->mic_peak > dst->mic_peak) {
        dst->mic_peak = src->mic_peak;
    }
    if (src->ref_peak > dst->ref_peak) {
        dst->ref_peak = src->ref_peak;
    }
    if (src->out_peak > dst->out_peak) {
        dst->out_peak = src->out_peak;
    }
    dst->samples += src->samples;
}

static float rms_from_sumsq(uint64_t sumsq, size_t samples) {
    if (samples == 0) {
        return 0.0f;
    }
    return (float)sqrt((double)sumsq / (double)samples);
}

static float corr_from_dot(int64_t dot, uint64_t x_sumsq, uint64_t y_sumsq) {
    if (x_sumsq == 0 || y_sumsq == 0) {
        return 0.0f;
    }
    const double denom = sqrt((double)x_sumsq * (double)y_sumsq);
    if (denom <= 0.0) {
        return 0.0f;
    }
    return (float)((double)dot / denom);
}

static float erle_from_rms(float in_rms, float out_rms) {
    if (in_rms <= 0.0f) {
        return 0.0f;
    }
    if (out_rms <= 0.0f) {
        return 60.0f;
    }
    return (float)(20.0 * log10((double)in_rms / (double)out_rms));
}

static float ratio_from_counts(size_t numerator, size_t denominator) {
    if (denominator == 0) {
        return 0.0f;
    }
    if (numerator >= denominator) {
        return 1.0f;
    }
    return (float)((double)numerator / (double)denominator);
}

static void live_reset_window(p5_live_cancel_detector_t *detector) {
    if (!detector) {
        return;
    }
    detector->window_frames = 0;
    detector->quiet_frames = 0;
    detector->window_ref_copied = 0;
    detector->window_ref_underrun = 0;
    memset(&detector->window_acc, 0, sizeof(detector->window_acc));
    memset(&detector->active_acc, 0, sizeof(detector->active_acc));
    memset(&detector->quiet_acc, 0, sizeof(detector->quiet_acc));
    memset(&detector->window_dual_acc, 0, sizeof(detector->window_dual_acc));
    memset(&detector->active_dual_acc, 0, sizeof(detector->active_dual_acc));
}

static void live_evaluate_aec_result(esp_err_t err, nuwax_p3_softref_aec_ab_result_t *r) {
    if (!r) {
        return;
    }
    r->corr_abs_before = fabsf(r->corr_before);
    r->corr_abs_after = fabsf(r->corr_after);
    r->corr_abs_delta = r->corr_abs_before - r->corr_abs_after;
    r->ref_underrun_ratio = ratio_from_counts(r->ref_underrun, r->samples);
    r->active_ref_ratio = ratio_from_counts(r->active_samples, r->samples);
    r->active_corr_abs_after = fabsf(r->active_corr_after);
    r->active_dual_mic_voice_ok =
        r->dual_mic_ready &&
        r->active_mic_rms >= 1800.0f &&
        r->active_mic2_rms >= 200.0f &&
        (fabsf(r->active_dual_mic_level_delta_db) >= 2.0f ||
         r->active_dual_mic_abs_corr <= 0.90f ||
         (r->active_dual_mic_diff_ratio >= 0.40f && r->active_dual_mic_abs_corr <= 0.94f));
    r->diagnostic_ready = r->frames > 0 &&
                          r->samples > 0 &&
                          r->ref_copied >= (r->samples / 2U) &&
                          r->ref_rms >= (float)CONFIG_NUWAX_P3_SOFTREF_AB_MIN_REF_RMS;
    r->data_plane_ok = err == ESP_OK &&
                       r->diagnostic_ready &&
                       r->ref_underrun_ratio <=
                           NUWAX_CONFIG_RATIO_X1000(CONFIG_NUWAX_P3_SOFTREF_AB_MAX_UNDERRUN_RATIO);
    r->active_quality_ok = r->data_plane_ok &&
                           r->active_samples > 0 &&
                           r->active_ref_ratio >=
                               NUWAX_CONFIG_RATIO_X1000(CONFIG_NUWAX_P3_SOFTREF_AB_MIN_ACTIVE_REF_RATIO) &&
                           r->active_erle_db >=
                               NUWAX_CONFIG_DB_X100(CONFIG_NUWAX_P3_SOFTREF_AB_MIN_ERLE_DB) &&
                           r->active_corr_abs_after <=
                               NUWAX_CONFIG_RATIO_X1000(CONFIG_NUWAX_P3_SOFTREF_AB_MAX_CORR_AFTER_ABS);
    r->quality_ok = r->active_quality_ok;
    r->resource_ok = r->heap_internal_free >= (size_t)CONFIG_NUWAX_P3_SOFTREF_AB_MIN_INTERNAL_FREE &&
                     r->heap_internal_largest >= (size_t)CONFIG_NUWAX_P3_SOFTREF_AB_MIN_INTERNAL_LARGEST;
    r->passed = r->data_plane_ok && r->quality_ok && r->resource_ok;
}

static void live_fill_aec_result(const p5_live_cancel_detector_t *detector,
                                 nuwax_p3_softref_aec_ab_result_t *aec) {
    if (!detector || !aec) {
        return;
    }
    memset(aec, 0, sizeof(*aec));
    const p5_live_aec_accum_t *acc = &detector->window_acc;
    const p5_live_aec_accum_t *active = &detector->active_acc;
    const p5_live_aec_accum_t *quiet = &detector->quiet_acc;
    aec->duration_ms = (uint32_t)(((uint64_t)acc->samples * 1000ULL) / CONFIG_NUWAX_VOICE_SAMPLE_RATE);
    aec->delay_samples = detector->delay_samples;
    aec->frames = detector->window_frames;
    aec->chunk_samples = (uint32_t)detector->chunk_samples;
    aec->samples = acc->samples;
    aec->ref_copied = detector->window_ref_copied;
    aec->ref_underrun = detector->window_ref_underrun;
    aec->mic_rms = rms_from_sumsq(acc->sumsq_mic, acc->samples);
    aec->ref_rms = rms_from_sumsq(acc->sumsq_ref, acc->samples);
    aec->out_rms = rms_from_sumsq(acc->sumsq_out, acc->samples);
    if (detector->window_dual_acc.samples > 0) {
        aec->dual_mic_ready = true;
        aec->mic2_rms = rms_from_sum(detector->window_dual_acc.sumsq_mic2,
                                     detector->window_dual_acc.samples);
        aec->dual_mic_corr = dual_mic_corr(&detector->window_dual_acc);
        aec->dual_mic_abs_corr = fabsf(aec->dual_mic_corr);
        aec->dual_mic_level_delta_db = db_ratio(aec->mic_rms, aec->mic2_rms);
        aec->dual_mic_diff_rms = rms_from_sum(detector->window_dual_acc.sumsq_diff,
                                              detector->window_dual_acc.samples);
        const float max_mic_rms = aec->mic_rms > aec->mic2_rms ? aec->mic_rms : aec->mic2_rms;
        aec->dual_mic_diff_ratio = max_mic_rms > 1.0f ? aec->dual_mic_diff_rms / max_mic_rms : 0.0f;
    }
    aec->mic_peak = acc->mic_peak;
    aec->ref_peak = acc->ref_peak;
    aec->out_peak = acc->out_peak;
    aec->erle_db = erle_from_rms(aec->mic_rms, aec->out_rms);
    aec->corr_before = corr_from_dot(acc->dot_mic_ref, acc->sumsq_mic, acc->sumsq_ref);
    aec->corr_after = corr_from_dot(acc->dot_out_ref, acc->sumsq_out, acc->sumsq_ref);
    aec->active_frames = detector->window_frames - detector->quiet_frames;
    aec->active_samples = active->samples;
    aec->active_mic_rms = rms_from_sumsq(active->sumsq_mic, active->samples);
    aec->active_ref_rms = rms_from_sumsq(active->sumsq_ref, active->samples);
    aec->active_out_rms = rms_from_sumsq(active->sumsq_out, active->samples);
    if (detector->active_dual_acc.samples > 0) {
        aec->dual_mic_ready = true;
        aec->active_mic2_rms = rms_from_sum(detector->active_dual_acc.sumsq_mic2,
                                            detector->active_dual_acc.samples);
        aec->active_dual_mic_corr = dual_mic_corr(&detector->active_dual_acc);
        aec->active_dual_mic_abs_corr = fabsf(aec->active_dual_mic_corr);
        aec->active_dual_mic_level_delta_db = db_ratio(aec->active_mic_rms,
                                                       aec->active_mic2_rms);
        aec->active_dual_mic_diff_rms = rms_from_sum(detector->active_dual_acc.sumsq_diff,
                                                     detector->active_dual_acc.samples);
        const float active_max_mic_rms =
            aec->active_mic_rms > aec->active_mic2_rms ? aec->active_mic_rms : aec->active_mic2_rms;
        aec->active_dual_mic_diff_ratio =
            active_max_mic_rms > 1.0f ? aec->active_dual_mic_diff_rms / active_max_mic_rms : 0.0f;
    }
    aec->active_erle_db = erle_from_rms(aec->active_mic_rms, aec->active_out_rms);
    aec->active_corr_before = corr_from_dot(active->dot_mic_ref, active->sumsq_mic, active->sumsq_ref);
    aec->active_corr_after = corr_from_dot(active->dot_out_ref, active->sumsq_out, active->sumsq_ref);
    aec->quiet_frames = detector->quiet_frames;
    aec->quiet_samples = quiet->samples;
    aec->quiet_mic_rms = rms_from_sumsq(quiet->sumsq_mic, quiet->samples);
    aec->quiet_out_rms = rms_from_sumsq(quiet->sumsq_out, quiet->samples);
    aec->heap_internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    aec->heap_internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    aec->heap_psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    live_evaluate_aec_result(ESP_OK, aec);
}

static void *live_alloc_buffer(size_t bytes) {
    void *ptr = heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) {
        ptr = heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return ptr;
}

static esp_err_t live_detector_init(p5_live_cancel_detector_t *detector,
                                    bool enabled,
                                    uint32_t delay_samples) {
    if (!detector) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(detector, 0, sizeof(*detector));
    detector->enabled = enabled;
    detector->delay_samples = delay_samples;
    detector->target_window_samples =
        ((uint32_t)CONFIG_NUWAX_VOICE_SAMPLE_RATE * (uint32_t)CONFIG_NUWAX_P6_LIVE_CANCEL_WINDOW_MS) / 1000U;
    if (detector->target_window_samples == 0) {
        detector->target_window_samples = 1;
    }
    if (!enabled) {
        return ESP_OK;
    }
#if CONFIG_NUWAX_P3_SOFTWARE_REF_AEC_AB_ENABLE
    esp_err_t err = nuwax_p3_software_ref_init(CONFIG_NUWAX_VOICE_OUTPUT_SAMPLE_RATE,
                                               CONFIG_NUWAX_P3_SOFTWARE_REF_CAPACITY_MS);
    if (err != ESP_OK) {
        return err;
    }
    detector->aec = afe_aec_create("MR",
                                   CONFIG_NUWAX_P3_SOFTREF_AB_AEC_FILTER_LENGTH,
                                   NUWAX_P5_LIVE_AEC_TYPE,
                                   NUWAX_P5_LIVE_AEC_MODE);
    if (!detector->aec) {
        return ESP_ERR_NO_MEM;
    }
    detector->chunk_samples = afe_aec_get_chunksize(detector->aec);
    if (detector->chunk_samples <= 0 || detector->chunk_samples > 2048) {
        return ESP_ERR_INVALID_SIZE;
    }
    const size_t mono_bytes = (size_t)detector->chunk_samples * sizeof(int16_t);
    const size_t mr_bytes = (size_t)detector->chunk_samples * 2U * sizeof(int16_t);
    detector->ref_buf = (int16_t *)live_alloc_buffer(mono_bytes);
    detector->mr_buf = (int16_t *)live_alloc_buffer(mr_bytes);
    detector->out_buf = (int16_t *)live_alloc_buffer(mono_bytes);
    if (!detector->ref_buf || !detector->mr_buf || !detector->out_buf) {
        return ESP_ERR_NO_MEM;
    }
    detector->ready = true;
    ESP_LOGI(TAG,
             "P6 live cancel detector ready chunk=%d window_ms=%u delay_samples=%lu",
             detector->chunk_samples,
             (unsigned)CONFIG_NUWAX_P6_LIVE_CANCEL_WINDOW_MS,
             (unsigned long)delay_samples);
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static void live_detector_deinit(p5_live_cancel_detector_t *detector) {
    if (!detector) {
        return;
    }
#if CONFIG_NUWAX_P3_SOFTWARE_REF_AEC_AB_ENABLE
    if (detector->aec) {
        afe_aec_destroy(detector->aec);
    }
#endif
    heap_caps_free(detector->ref_buf);
    heap_caps_free(detector->mr_buf);
    heap_caps_free(detector->out_buf);
    memset(detector, 0, sizeof(*detector));
}

static bool live_cancel_voice_class_ok(const nuwax_p3_nearend_candidate_result_t *candidate) {
    if (!candidate) {
        return false;
    }
    /*
     * Live barge-in is allowed to stop TTS before a full post-window verdict,
     * so it must be stricter than the record/candidate diagnostic path. The
     * broad low-ref strong-residual shape can occur during playback start
     * transients; keep it observable, but do not let it fire live cancel.
     */
    return candidate->aec.active_dual_mic_voice_ok &&
           (candidate->p6_low_ref_normal_voice_ok ||
            candidate->p6_high_ref_voice_ok ||
            candidate->p6_mid_ref_strong_voice_ok ||
            candidate->p6_normal_voice_ok);
}

static bool live_detector_process(p5_live_cancel_detector_t *detector,
                                  const int16_t *samples,
                                  const int16_t *mic2_samples,
                                  size_t sample_count,
                                  uint32_t elapsed_ms,
                                  nuwax_p5_fullduplex_overlap_probe_result_t *result) {
    if (!detector || !detector->ready || !samples || !result || result->live_cancel_candidate) {
        return false;
    }
#if CONFIG_NUWAX_P3_SOFTWARE_REF_AEC_AB_ENABLE
    const size_t chunk = (size_t)detector->chunk_samples;
    size_t offset = 0;
    while (offset + chunk <= sample_count) {
        memset(detector->ref_buf, 0, chunk * sizeof(int16_t));
        memset(detector->out_buf, 0, chunk * sizeof(int16_t));
        const size_t copied = nuwax_p3_software_ref_read_delayed(detector->ref_buf,
                                                                 chunk,
                                                                 detector->delay_samples);
        detector->window_ref_copied += copied;
        detector->window_ref_underrun += chunk - copied;

        const int16_t *mic = &samples[offset];
        for (size_t i = 0; i < chunk; i++) {
            detector->mr_buf[i * 2U] = mic[i];
            detector->mr_buf[i * 2U + 1U] = detector->ref_buf[i];
        }
        size_t out_bytes = afe_aec_process(detector->aec, detector->mr_buf, detector->out_buf);
        size_t out_samples = out_bytes / sizeof(int16_t);
        if (out_samples > chunk) {
            out_samples = chunk;
        }
        if (out_samples < chunk) {
            memset(&detector->out_buf[out_samples], 0, (chunk - out_samples) * sizeof(int16_t));
        }

        p5_live_aec_accum_t frame_acc = {0};
        live_accumulate(&frame_acc, mic, detector->ref_buf, detector->out_buf, chunk);
        live_merge(&detector->window_acc, &frame_acc);
        p5_dual_mic_accum_t frame_dual_acc = {0};
        const bool dual_read = mic2_samples != NULL;
        if (dual_read) {
            dual_mic_accumulate(&frame_dual_acc, mic, &mic2_samples[offset], chunk);
            dual_mic_merge(&detector->window_dual_acc, &frame_dual_acc);
        }
        const float frame_ref_rms = rms_from_sumsq(frame_acc.sumsq_ref, frame_acc.samples);
        if (frame_ref_rms >= (float)CONFIG_NUWAX_P3_SOFTREF_AB_MIN_REF_RMS) {
            live_merge(&detector->active_acc, &frame_acc);
            if (dual_read) {
                dual_mic_merge(&detector->active_dual_acc, &frame_dual_acc);
            }
        } else {
            live_merge(&detector->quiet_acc, &frame_acc);
            detector->quiet_frames++;
        }
        detector->window_frames++;

        if (detector->window_acc.samples >= detector->target_window_samples) {
            nuwax_p3_softref_aec_ab_result_t aec = {0};
            nuwax_p3_nearend_candidate_result_t candidate = {0};
            live_fill_aec_result(detector, &aec);
            (void)nuwax_p3_nearend_candidate_evaluate(aec.duration_ms,
                                                       detector->delay_samples,
                                                       &aec,
                                                       &candidate);
            const bool live_voice_ok = live_cancel_voice_class_ok(&candidate);
            if (candidate.p6_cancel_candidate && live_voice_ok) {
                result->live_cancel_candidate = true;
                result->live_candidate_elapsed_ms = elapsed_ms;
                result->live_candidate_ts = monotonic_ms();
                result->live_candidate = candidate;
                ESP_LOGW(TAG,
                         "P6-LIVE-CANCEL-CANDIDATE elapsed_ms=%lu window_ms=%lu "
                         "active_ref_ratio=%.3f active_mic_rms=%.1f active_out_rms=%.1f "
                         "active_mic2_rms=%.1f active_dual_mic_voice_ok=%d "
                         "active_dual_mic_corr=%.3f active_dual_mic_diff_ratio=%.3f "
                         "active_erle_db=%.2f p6_low_ref_normal_voice_ok=%d "
                         "p6_high_ref_voice_ok=%d p6_mid_ref_strong_voice_ok=%d p6_normal_voice_ok=%d",
                         (unsigned long)elapsed_ms,
                         (unsigned long)aec.duration_ms,
                         (double)aec.active_ref_ratio,
                         (double)aec.active_mic_rms,
                         (double)aec.active_out_rms,
                         (double)aec.active_mic2_rms,
                         aec.active_dual_mic_voice_ok ? 1 : 0,
                         (double)aec.active_dual_mic_corr,
                         (double)aec.active_dual_mic_diff_ratio,
                         (double)aec.active_erle_db,
                         candidate.p6_low_ref_normal_voice_ok ? 1 : 0,
                         candidate.p6_high_ref_voice_ok ? 1 : 0,
                         candidate.p6_mid_ref_strong_voice_ok ? 1 : 0,
                         candidate.p6_normal_voice_ok ? 1 : 0);
                return true;
            }
            if (detector->window_acc.samples >= detector->target_window_samples * 2U) {
                if (candidate.p6_cancel_candidate && !live_voice_ok) {
                    ESP_LOGI(TAG,
                             "P6-LIVE-CANCEL-SUPPRESSED elapsed_ms=%lu window_ms=%lu "
                             "active_ref_ratio=%.3f active_mic_rms=%.1f active_out_rms=%.1f "
                             "active_mic2_rms=%.1f active_dual_mic_voice_ok=%d "
                             "active_dual_mic_corr=%.3f active_dual_mic_diff_ratio=%.3f "
                             "active_out_to_ref_db=%.2f active_erle_db=%.2f "
                             "p6_low_ref_nearend_ok=%d strong_nearend_ok=%d",
                             (unsigned long)elapsed_ms,
                             (unsigned long)aec.duration_ms,
                             (double)aec.active_ref_ratio,
                             (double)aec.active_mic_rms,
                             (double)aec.active_out_rms,
                             (double)aec.active_mic2_rms,
                             aec.active_dual_mic_voice_ok ? 1 : 0,
                             (double)aec.active_dual_mic_corr,
                             (double)aec.active_dual_mic_diff_ratio,
                             (double)candidate.active_out_to_ref_db,
                             (double)aec.active_erle_db,
                             candidate.p6_low_ref_nearend_ok ? 1 : 0,
                             candidate.strong_nearend_ok ? 1 : 0);
                }
                live_reset_window(detector);
            }
        }
        offset += chunk;
    }
#else
    (void)elapsed_ms;
#endif
    return false;
}

static void log_probe_result(esp_err_t err,
                             const nuwax_p5_fullduplex_overlap_probe_result_t *result,
                             uint32_t duration_ms) {
    const nuwax_p5_fullduplex_overlap_probe_result_t zero = {0};
    const nuwax_p5_fullduplex_overlap_probe_result_t *r = result ? result : &zero;
    ESP_LOGW(TAG,
             "P5-1-FULLDUPLEX-OVERLAP playback_id=%s audio_session_id=%s "
             "capture_frames=%lu upload_frames=%lu queued_frames=%lu queue_drops=%lu "
             "write_failures=%lu read_timeouts=%lu ref_rms=%.1f mic_rms=%.1f "
             "mic2_rms=%.1f dual_mic_ready=%d dual_mic_voice_ok=%d "
             "dual_mic_corr=%.3f dual_mic_abs_corr=%.3f dual_mic_level_delta_db=%.2f "
             "dual_mic_diff_rms=%.1f dual_mic_diff_ratio=%.3f "
             "heap_internal_min=%u dma_largest=%u overlap_ms=%lu "
             "live_cancel_candidate=%d live_candidate_elapsed_ms=%lu "
             "result=%s err=%s duration_ms=%lu",
             r->playback_id[0] ? r->playback_id : "-",
             r->audio_session_id[0] ? r->audio_session_id : "-",
             (unsigned long)r->capture_frames,
             (unsigned long)r->upload_frames,
             (unsigned long)r->queued_frames,
             (unsigned long)r->queue_drops,
             (unsigned long)r->write_failures,
             (unsigned long)r->read_timeouts,
             (double)r->ref_rms,
             (double)r->mic_rms,
             (double)r->mic2_rms,
             r->dual_mic_ready ? 1 : 0,
             r->dual_mic_voice_ok ? 1 : 0,
             (double)r->dual_mic_corr,
             (double)r->dual_mic_abs_corr,
             (double)r->dual_mic_level_delta_db,
             (double)r->dual_mic_diff_rms,
             (double)r->dual_mic_diff_ratio,
             (unsigned)r->heap_internal_min,
             (unsigned)r->dma_largest,
             (unsigned long)r->overlap_ms,
             r->live_cancel_candidate ? 1 : 0,
             (unsigned long)r->live_candidate_elapsed_ms,
             r->result[0] ? r->result : "-",
             esp_err_to_name(err),
             (unsigned long)duration_ms);
}

esp_err_t nuwax_p5_fullduplex_overlap_probe_run_ex_if_epoch(
    nuwax_ws_client_t *ws,
    uint32_t expected_ws_epoch,
    const char *request_id,
    const char *audio_session_id,
    uint32_t duration_ms,
    uint32_t delay_samples,
    bool enable_live_cancel_candidate,
    nuwax_p5_fullduplex_overlap_probe_result_t *out_result) {
    if (nuwax_audio_terminal_recovery_pending()) {
        /* The application owner is the only recovery service/consumer.  A
         * diagnostic command may observe the barrier but must not advance a
         * transaction owned by a conversation or exclusive audio mode. */
        return ESP_ERR_NOT_FINISHED;
    }
    nuwax_p5_fullduplex_overlap_probe_result_t result = {0};
    esp_err_t err = ESP_OK;
    bool listen_started = false;
    bool uplink_started = false;
    bool end_committed = false;
    bool mic_started = false;
    bool opus_ready = false;
    bool live_detector_ready = false;
    uint64_t mic_sum_sq = 0;
    uint64_t mic_samples = 0;
    const uint32_t safe_duration_ms = clamp_duration_ms(duration_ms);
    const uint32_t frame_ms = CONFIG_NUWAX_VOICE_FRAME_DURATION_MS;
    const uint32_t target_frames = frame_count_for_duration(safe_duration_ms, frame_ms);
    const size_t samples_per_frame = nuwax_audio_uplink_samples_per_frame();
    nuwax_speaker_playback_stats_t speaker_before = {0};
    nuwax_speaker_playback_stats_t speaker_after = {0};
    nuwax_audio_uplink_t *uplink = nuwax_audio_uplink_create();
    nuwax_mic_input_t mic = {0};
    p5_live_cancel_detector_t live_detector = {0};
    p5_dual_mic_accum_t dual_mic_acc = {0};
#if CONFIG_NUWAX_VOICE_UPLINK_CODEC_OPUS
    nuwax_opus_encoder_t opus_encoder = {0};
#endif
    int16_t *samples = NULL;
    int16_t *mic2_samples = NULL;

    result.ref_rms = -1.0f;
    result.heap_internal_min = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    result.dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    set_result(&result, "init");
    make_audio_session_id(result.audio_session_id,
                          sizeof(result.audio_session_id),
                          audio_session_id,
                          request_id);

#if !CONFIG_NUWAX_P5_FULLDUPLEX_OVERLAP_PROBE_ENABLE
    err = ESP_ERR_NOT_SUPPORTED;
    set_result(&result, "disabled");
    goto finish;
#else
    if (!ws || expected_ws_epoch == 0U ||
        nuwax_ws_connection_epoch(ws) != expected_ws_epoch ||
        !nuwax_ws_is_connected(ws)) {
        err = ESP_ERR_INVALID_STATE;
        set_result(&result, "ws_unavailable");
        goto finish;
    }
    if (samples_per_frame == 0 || samples_per_frame > CONFIG_NUWAX_VOICE_MAX_SAMPLES_PER_FRAME) {
        err = ESP_ERR_INVALID_SIZE;
        set_result(&result, "invalid_frame");
        goto finish;
    }

    nuwax_speaker_playback_get_stats(&speaker_before);
    strlcpy(result.playback_id,
            speaker_before.active_playback_id[0] ? speaker_before.active_playback_id : "-",
            sizeof(result.playback_id));
    if (!speaker_before.pending) {
        err = ESP_ERR_INVALID_STATE;
        set_result(&result, "no_playback");
        goto finish;
    }

    if (enable_live_cancel_candidate) {
        esp_err_t live_err = live_detector_init(&live_detector, true, delay_samples);
        if (live_err == ESP_OK) {
            live_detector_ready = true;
        } else {
            ESP_LOGW(TAG,
                     "P6 live cancel detector disabled err=%s request=%s",
                     esp_err_to_name(live_err),
                     request_id && request_id[0] ? request_id : "-");
            live_detector_deinit(&live_detector);
        }
    }

    samples = (int16_t *)heap_caps_malloc(samples_per_frame * sizeof(int16_t),
                                          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!samples) {
        err = ESP_ERR_NO_MEM;
        set_result(&result, "alloc_failed");
        goto finish;
    }
    mic2_samples = (int16_t *)heap_caps_malloc(samples_per_frame * sizeof(int16_t),
                                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    err = nuwax_mic_input_init_with_owner(&mic, NUWAX_MIC_OWNER_CONVERSATION_CAPTURE);
    if (err != ESP_OK) {
        set_result(&result, "mic_init_failed");
        goto finish;
    }
    mic_started = true;

#if CONFIG_NUWAX_VOICE_UPLINK_CODEC_OPUS
    err = nuwax_opus_encoder_init(&opus_encoder,
                                  CONFIG_NUWAX_VOICE_SAMPLE_RATE,
                                  CONFIG_NUWAX_VOICE_CHANNELS,
                                  CONFIG_NUWAX_VOICE_FRAME_DURATION_MS);
    if (err != ESP_OK) {
        set_result(&result, "opus_init_failed");
        goto finish;
    }
    opus_ready = true;
    err = nuwax_audio_uplink_ensure_encoded_frame_capacity(nuwax_opus_encoder_output_bytes(&opus_encoder));
    if (err != ESP_OK) {
        set_result(&result, "opus_buffer_failed");
        goto finish;
    }
#endif

    err = uplink
              ? nuwax_audio_uplink_start_if_epoch(
                    uplink, ws, expected_ws_epoch)
              : ESP_ERR_NO_MEM;
    if (err != ESP_OK) {
        set_result(&result, "uplink_start_failed");
        goto finish;
    }
    uplink_started = true;

    err = nuwax_session_send_listen_with_audio_session_until_if_epoch(
        ws,
        "start",
        NUWAX_P5_OVERLAP_MODE,
        result.audio_session_id,
        request_id,
        esp_timer_get_time() + NUWAX_P5_CONTROL_SEND_TIMEOUT_US,
        expected_ws_epoch);
    if (err != ESP_OK) {
        set_result(&result, "listen_start_failed");
        const esp_err_t discard_err =
            nuwax_audio_uplink_discard_uncommitted(uplink);
        if (discard_err != ESP_OK) {
            (void)nuwax_ws_request_close_if_epoch(ws, expected_ws_epoch);
            ESP_LOGE(TAG,
                     "P5 uncommitted uplink could not be discarded: %s",
                     esp_err_to_name(discard_err));
            /* Keep the heap-owned session alive for detached cleanup. Never
             * unwind a caller stack while the sender can still reference it. */
            (void)nuwax_audio_uplink_destroy_owned(&uplink);
        }
        uplink_started = false;
        (void)nuwax_ws_request_close_if_epoch(ws, expected_ws_epoch);
        goto finish;
    }
    listen_started = true;
    const esp_err_t recovery_bind_err =
        nuwax_audio_uplink_bind_terminal_recovery(
            uplink,
            NUWAX_P5_OVERLAP_MODE,
            result.audio_session_id,
            request_id,
            NUWAX_AUDIO_TERMINAL_RECOVERY_DEFAULT_ATTEMPT_TIMEOUT_MS);
    if (recovery_bind_err != ESP_OK) {
        ESP_LOGE(TAG,
                 "P5 terminal recovery binding failed session=%s err=%s",
                 result.audio_session_id,
                 esp_err_to_name(recovery_bind_err));
    }

    ESP_LOGW(TAG,
             "P5-1 full-duplex overlap probe start request=%s playback=%s audio_session_id=%s duration_ms=%lu target_frames=%lu",
             request_id && request_id[0] ? request_id : "-",
             result.playback_id[0] ? result.playback_id : "-",
             result.audio_session_id,
             (unsigned long)safe_duration_ms,
             (unsigned long)target_frames);

    for (uint32_t seq = 0;
         seq < target_frames && nuwax_ws_is_connected(ws) &&
         nuwax_ws_connection_epoch(ws) == expected_ws_epoch;
         seq++) {
        size_t samples_read = 0;
        bool dual_read = false;
        esp_err_t read_err = ESP_ERR_NOT_SUPPORTED;
        if (mic2_samples) {
            read_err = nuwax_mic_input_read_vocat_mic12_pcm16(&mic,
                                                              samples,
                                                              mic2_samples,
                                                              samples_per_frame,
                                                              CONFIG_NUWAX_MIC_READ_TIMEOUT_MS,
                                                              &samples_read);
            dual_read = read_err == ESP_OK || read_err == ESP_ERR_TIMEOUT;
        }
        if (read_err == ESP_ERR_NOT_SUPPORTED) {
            read_err = nuwax_mic_input_read_pcm16(&mic,
                                                  samples,
                                                  samples_per_frame,
                                                  CONFIG_NUWAX_MIC_READ_TIMEOUT_MS,
                                                  &samples_read);
            dual_read = false;
        }
        if (read_err == ESP_ERR_TIMEOUT && samples_read == 0) {
            result.read_timeouts++;
            continue;
        }
        if (read_err != ESP_OK && read_err != ESP_ERR_TIMEOUT) {
            err = read_err;
            set_result(&result, "read_failed");
            break;
        }
        if (samples_read < samples_per_frame) {
            memset(&samples[samples_read], 0, (samples_per_frame - samples_read) * sizeof(int16_t));
            if (mic2_samples) {
                memset(&mic2_samples[samples_read], 0, (samples_per_frame - samples_read) * sizeof(int16_t));
            }
            samples_read = samples_per_frame;
        }

        if (dual_read && mic2_samples) {
            dual_mic_accumulate(&dual_mic_acc, samples, mic2_samples, samples_read);
        }
        for (size_t i = 0; i < samples_read; i++) {
            const int32_t sample = samples[i];
            mic_sum_sq += (uint64_t)((int64_t)sample * (int64_t)sample);
        }
        mic_samples += samples_read;

        nuwax_speaker_playback_stats_t live = {0};
        nuwax_speaker_playback_get_stats(&live);
        if (live.pending) {
            result.overlap_ms += frame_ms;
        }

        const uint32_t elapsed_capture_ms = (seq + 1U) * frame_ms;
        const bool live_triggered = live_detector_ready &&
                                    live_detector_process(&live_detector,
                                                          samples,
                                                          dual_read ? mic2_samples : NULL,
                                                          samples_read,
                                                          elapsed_capture_ms,
                                                          &result);

        uint8_t flags = NUWAX_AUDIO_FLAG_VAD_SPEECH;
        if (result.capture_frames == 0) {
            flags |= NUWAX_AUDIO_FLAG_START;
        }
        const bool final_frame = live_triggered || (seq + 1) >= target_frames;
        if (final_frame) {
            flags |= NUWAX_AUDIO_FLAG_END;
        }

        err = nuwax_audio_uplink_enqueue_frame(uplink,
#if CONFIG_NUWAX_VOICE_UPLINK_CODEC_OPUS
                                               &opus_encoder,
#else
                                               NULL,
#endif
                                               samples,
                                               samples_read,
                                               result.capture_frames,
                                               flags,
                                               final_frame);
        if (err != ESP_OK) {
            set_result(&result, "enqueue_failed");
            break;
        }
        result.capture_frames++;

        if (live_triggered) {
            set_result(&result, "live_cancel_candidate");
            break;
        }

        size_t heap_now = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t dma_now = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (heap_now < result.heap_internal_min) {
            result.heap_internal_min = heap_now;
        }
        if (dma_now < result.dma_largest) {
            result.dma_largest = dma_now;
        }
    }

    if ((!nuwax_ws_is_connected(ws) ||
         nuwax_ws_connection_epoch(ws) != expected_ws_epoch) &&
        err == ESP_OK) {
        err = ESP_ERR_INVALID_STATE;
        set_result(&result, "ws_connection_changed");
    }
#endif

finish:
    if (uplink_started) {
        esp_err_t finish_err = nuwax_audio_uplink_finish(uplink);
        end_committed = nuwax_audio_uplink_end_committed(uplink);
        if (finish_err != ESP_OK &&
            !nuwax_audio_terminal_recovery_pending()) {
            (void)nuwax_ws_request_close_if_epoch(ws, expected_ws_epoch);
        }
        if (finish_err != ESP_OK && err == ESP_OK) {
            err = finish_err;
            set_result(&result, "finish_failed");
        }
    }
    if (listen_started && end_committed &&
        !nuwax_audio_terminal_recovery_pending() &&
        nuwax_ws_is_connected(ws) &&
        nuwax_ws_connection_epoch(ws) == expected_ws_epoch) {
        const esp_err_t stop_err =
            nuwax_session_send_listen_stop_retry_if_epoch(
                ws,
                NUWAX_P5_OVERLAP_MODE,
                result.audio_session_id,
                request_id,
                (uint32_t)(NUWAX_P5_CONTROL_SEND_TIMEOUT_US / 1000LL),
                expected_ws_epoch);
        if (stop_err != ESP_OK) {
            const esp_err_t recovery_arm_err =
                nuwax_audio_terminal_recovery_arm(
                    ws,
                    expected_ws_epoch,
                    NULL,
                    0U,
                    true,
                    NUWAX_P5_OVERLAP_MODE,
                    result.audio_session_id,
                    request_id,
                    NUWAX_AUDIO_TERMINAL_RECOVERY_DEFAULT_ATTEMPT_TIMEOUT_MS);
            if (recovery_arm_err != ESP_OK) {
                (void)nuwax_ws_request_close_if_epoch(ws, expected_ws_epoch);
            }
            ESP_LOGW(TAG,
                     "P5 overlap listen stop failed on bound connection: %s",
                     esp_err_to_name(stop_err));
            if (err == ESP_OK) {
                err = stop_err;
                set_result(&result, "listen_stop_failed");
            }
        }
    } else if (listen_started && !end_committed) {
        if (!nuwax_audio_terminal_recovery_pending()) {
            (void)nuwax_ws_request_close_if_epoch(ws, expected_ws_epoch);
        }
        ESP_LOGW(TAG,
                 "P5 listen stop suppressed because binary END was not committed session=%s epoch=%lu",
                 result.audio_session_id,
                 (unsigned long)expected_ws_epoch);
    }
    nuwax_audio_uplink_stats_t uplink_stats = {0};
    nuwax_audio_uplink_get_stats(uplink, &uplink_stats);
    result.queued_frames = uplink_stats.queued_frames;
    result.upload_frames = uplink_stats.sent_frames;
    result.queue_drops = uplink_stats.dropped_frames;

#if CONFIG_NUWAX_VOICE_UPLINK_CODEC_OPUS
    if (opus_ready) {
        nuwax_opus_encoder_deinit(&opus_encoder);
    }
#endif
    if (live_detector_ready) {
        live_detector_deinit(&live_detector);
    }
    if (mic_started) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(nuwax_mic_input_deinit(&mic));
    }
    if (samples) {
        heap_caps_free(samples);
    }
    if (mic2_samples) {
        heap_caps_free(mic2_samples);
    }
    (void)nuwax_audio_uplink_destroy_owned(&uplink);

    result.mic_rms = rms_from_sum(mic_sum_sq, mic_samples);
    fill_dual_mic_result(&result, &dual_mic_acc);
    nuwax_speaker_playback_get_stats(&speaker_after);
    result.write_failures = delta_u32(speaker_after.write_failures, speaker_before.write_failures);
    result.queue_drops += delta_u32(speaker_after.queue_dropped_frames, speaker_before.queue_dropped_frames);
    if (result.playback_id[0] == '\0' || strcmp(result.playback_id, "-") == 0) {
        strlcpy(result.playback_id,
                speaker_after.active_playback_id[0] ? speaker_after.active_playback_id : "-",
                sizeof(result.playback_id));
    }

    nuwax_p3_software_ref_stats_t ref_stats = {0};
    nuwax_p3_software_ref_get_stats(&ref_stats);
    if (ref_stats.enabled && ref_stats.initialized) {
        result.ref_rms = ref_stats.last_rms;
    }

    if (err == ESP_OK) {
        if (result.capture_frames > 0 &&
            result.upload_frames > 0 &&
            result.overlap_ms > 0 &&
            result.queue_drops == 0 &&
            result.write_failures == 0) {
            result.passed = true;
            set_result(&result, "pass");
        } else {
            set_result(&result, "warn");
        }
    }
    log_probe_result(err, &result, safe_duration_ms);
    if (out_result) {
        *out_result = result;
    }
    return err;
}

esp_err_t nuwax_p5_fullduplex_overlap_probe_run_ex(
    nuwax_ws_client_t *ws,
    const char *request_id,
    const char *audio_session_id,
    uint32_t duration_ms,
    uint32_t delay_samples,
    bool enable_live_cancel_candidate,
    nuwax_p5_fullduplex_overlap_probe_result_t *out_result) {
    if (!ws) {
        return ESP_ERR_INVALID_ARG;
    }
    return nuwax_p5_fullduplex_overlap_probe_run_ex_if_epoch(
        ws,
        nuwax_ws_connection_epoch(ws),
        request_id,
        audio_session_id,
        duration_ms,
        delay_samples,
        enable_live_cancel_candidate,
        out_result);
}

esp_err_t nuwax_p5_fullduplex_overlap_probe_run(nuwax_ws_client_t *ws,
                                                const char *request_id,
                                                const char *audio_session_id,
                                                uint32_t duration_ms,
                                                nuwax_p5_fullduplex_overlap_probe_result_t *out_result) {
    return nuwax_p5_fullduplex_overlap_probe_run_ex(ws,
                                                    request_id,
                                                    audio_session_id,
                                                    duration_ms,
                                                    0,
                                                    false,
                                                    out_result);
}
