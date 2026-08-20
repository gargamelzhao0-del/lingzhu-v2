#include "p3_softref_aec_diag.h"

#include <math.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mic_input.h"
#include "p3_software_ref.h"

#ifndef CONFIG_NUWAX_P3_SOFTWARE_REF_ENABLE
#define CONFIG_NUWAX_P3_SOFTWARE_REF_ENABLE 0
#endif

#ifndef CONFIG_NUWAX_P3_SOFTWARE_REF_AEC_AB_ENABLE
#define CONFIG_NUWAX_P3_SOFTWARE_REF_AEC_AB_ENABLE 0
#endif

#ifndef CONFIG_NUWAX_P3_HARDREF_AEC_AB_ENABLE
#define CONFIG_NUWAX_P3_HARDREF_AEC_AB_ENABLE 0
#endif

#ifndef CONFIG_NUWAX_P3_SOFTWARE_REF_CAPACITY_MS
#define CONFIG_NUWAX_P3_SOFTWARE_REF_CAPACITY_MS 2000
#endif

#ifndef CONFIG_NUWAX_VOICE_SAMPLE_RATE
#define CONFIG_NUWAX_VOICE_SAMPLE_RATE 16000
#endif

#ifndef CONFIG_NUWAX_VOICE_OUTPUT_SAMPLE_RATE
#define CONFIG_NUWAX_VOICE_OUTPUT_SAMPLE_RATE 16000
#endif

#define NUWAX_P3_SOFTREF_AB_MIN_DURATION_MS 500U
#define NUWAX_P3_SOFTREF_AB_MAX_DURATION_MS 5000U
#define NUWAX_P3_SOFTREF_AB_DEFAULT_TIMEOUT_MS 250U

#ifndef CONFIG_NUWAX_P3_SOFTREF_AB_MIN_REF_RMS
#define CONFIG_NUWAX_P3_SOFTREF_AB_MIN_REF_RMS 20
#endif

#ifndef CONFIG_NUWAX_P3_SOFTREF_AB_MAX_READ_TIMEOUTS
#define CONFIG_NUWAX_P3_SOFTREF_AB_MAX_READ_TIMEOUTS 0
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

#ifndef CONFIG_NUWAX_P3_SOFTREF_AB_AEC_FILTER_LENGTH
#define CONFIG_NUWAX_P3_SOFTREF_AB_AEC_FILTER_LENGTH 4
#endif

#ifndef CONFIG_NUWAX_P3_SOFTREF_AB_MIN_INTERNAL_FREE
#define CONFIG_NUWAX_P3_SOFTREF_AB_MIN_INTERNAL_FREE 49152
#endif

#ifndef CONFIG_NUWAX_P3_SOFTREF_AB_MIN_INTERNAL_LARGEST
#define CONFIG_NUWAX_P3_SOFTREF_AB_MIN_INTERNAL_LARGEST 16384
#endif

#if CONFIG_NUWAX_P3_SOFTWARE_REF_AEC_AB_ENABLE || CONFIG_NUWAX_P3_HARDREF_AEC_AB_ENABLE
#include "esp_afe_aec.h"
#endif

#if CONFIG_NUWAX_P3_SOFTREF_AB_AEC_TYPE_VC
#define NUWAX_P3_SOFTREF_AB_AEC_TYPE AFE_TYPE_VC
#define NUWAX_P3_SOFTREF_AB_AEC_TYPE_NAME "VC"
#else
#define NUWAX_P3_SOFTREF_AB_AEC_TYPE AFE_TYPE_SR
#define NUWAX_P3_SOFTREF_AB_AEC_TYPE_NAME "SR"
#endif

#if CONFIG_NUWAX_P3_SOFTREF_AB_AEC_MODE_HIGH_PERF
#define NUWAX_P3_SOFTREF_AB_AEC_MODE AFE_MODE_HIGH_PERF
#define NUWAX_P3_SOFTREF_AB_AEC_MODE_NAME "HIGH_PERF"
#else
#define NUWAX_P3_SOFTREF_AB_AEC_MODE AFE_MODE_LOW_COST
#define NUWAX_P3_SOFTREF_AB_AEC_MODE_NAME "LOW_COST"
#endif

static const char *TAG = "nuwax_p3_aec_ab";

typedef enum {
    NUWAX_P3_AEC_REF_SOURCE_SOFTREF = 0,
    NUWAX_P3_AEC_REF_SOURCE_HARDREF,
} nuwax_p3_aec_ref_source_t;

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
} nuwax_p3_softref_ab_accum_t;

typedef struct {
    uint64_t sumsq_mic1;
    uint64_t sumsq_mic2;
    uint64_t sumsq_diff;
    int64_t dot_mic12;
    size_t samples;
} nuwax_p3_dual_mic_accum_t;

typedef struct {
    size_t internal_free;
    size_t internal_largest;
    size_t dma_free;
    size_t dma_largest;
} nuwax_p3_heap_snapshot_t;

static nuwax_p3_heap_snapshot_t take_heap_snapshot(void)
{
    nuwax_p3_heap_snapshot_t snap = {
        .internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        .internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        .dma_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
        .dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
    };
    return snap;
}

static void update_min_heap(size_t *min_free, size_t *min_largest, size_t free_now, size_t largest_now)
{
    if (min_free && (*min_free == 0 || free_now < *min_free)) {
        *min_free = free_now;
    }
    if (min_largest && (*min_largest == 0 || largest_now < *min_largest)) {
        *min_largest = largest_now;
    }
}

static uint32_t clamp_duration_ms(uint32_t duration_ms)
{
    if (duration_ms < NUWAX_P3_SOFTREF_AB_MIN_DURATION_MS) {
        return NUWAX_P3_SOFTREF_AB_MIN_DURATION_MS;
    }
    if (duration_ms > NUWAX_P3_SOFTREF_AB_MAX_DURATION_MS) {
        return NUWAX_P3_SOFTREF_AB_MAX_DURATION_MS;
    }
    return duration_ms;
}

static int32_t abs_pcm16(int16_t value)
{
    const int32_t v = value;
    return v < 0 ? -v : v;
}

static void accumulate_frame(nuwax_p3_softref_ab_accum_t *acc,
                             const int16_t *mic,
                             const int16_t *ref,
                             const int16_t *out,
                             size_t sample_count)
{
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

static void merge_accum(nuwax_p3_softref_ab_accum_t *dst, const nuwax_p3_softref_ab_accum_t *src)
{
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

static void accumulate_dual_mic(nuwax_p3_dual_mic_accum_t *acc,
                                const int16_t *mic1,
                                const int16_t *mic2,
                                size_t sample_count)
{
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

static void merge_dual_mic(nuwax_p3_dual_mic_accum_t *dst, const nuwax_p3_dual_mic_accum_t *src)
{
    if (!dst || !src) {
        return;
    }
    dst->sumsq_mic1 += src->sumsq_mic1;
    dst->sumsq_mic2 += src->sumsq_mic2;
    dst->sumsq_diff += src->sumsq_diff;
    dst->dot_mic12 += src->dot_mic12;
    dst->samples += src->samples;
}

static float rms_from_sumsq(uint64_t sumsq, size_t samples)
{
    if (samples == 0) {
        return 0.0f;
    }
    return (float)sqrt((double)sumsq / (double)samples);
}

static float corr_from_dot(int64_t dot, uint64_t x_sumsq, uint64_t y_sumsq)
{
    if (x_sumsq == 0 || y_sumsq == 0) {
        return 0.0f;
    }
    const double denom = sqrt((double)x_sumsq * (double)y_sumsq);
    if (denom <= 0.0) {
        return 0.0f;
    }
    return (float)((double)dot / denom);
}

static float db_ratio(float numerator_rms, float denominator_rms);

static void fill_dual_mic_metrics(float mic1_rms,
                                  const nuwax_p3_dual_mic_accum_t *acc,
                                  bool *ready,
                                  float *mic2_rms,
                                  float *corr,
                                  float *abs_corr,
                                  float *level_delta_db,
                                  float *diff_rms,
                                  float *diff_ratio)
{
    if (ready) {
        *ready = false;
    }
    if (!acc || acc->samples == 0) {
        return;
    }
    const float local_mic2_rms = rms_from_sumsq(acc->sumsq_mic2, acc->samples);
    const float local_corr = corr_from_dot(acc->dot_mic12, acc->sumsq_mic1, acc->sumsq_mic2);
    const float local_diff_rms = rms_from_sumsq(acc->sumsq_diff, acc->samples);
    const float max_mic_rms = mic1_rms > local_mic2_rms ? mic1_rms : local_mic2_rms;
    if (ready) {
        *ready = true;
    }
    if (mic2_rms) {
        *mic2_rms = local_mic2_rms;
    }
    if (corr) {
        *corr = local_corr;
    }
    if (abs_corr) {
        *abs_corr = fabsf(local_corr);
    }
    if (level_delta_db) {
        *level_delta_db = db_ratio(mic1_rms, local_mic2_rms);
    }
    if (diff_rms) {
        *diff_rms = local_diff_rms;
    }
    if (diff_ratio) {
        *diff_ratio = max_mic_rms > 1.0f ? local_diff_rms / max_mic_rms : 0.0f;
    }
}

static float erle_from_rms(float in_rms, float out_rms)
{
    if (in_rms <= 0.0f) {
        return 0.0f;
    }
    if (out_rms <= 0.0f) {
        return 60.0f;
    }
    return (float)(20.0 * log10((double)in_rms / (double)out_rms));
}

static float db_ratio(float numerator_rms, float denominator_rms)
{
    if (denominator_rms <= 0.0f) {
        return numerator_rms <= 0.0f ? 0.0f : 60.0f;
    }
    if (numerator_rms <= 0.0f) {
        return -60.0f;
    }
    return (float)(20.0 * log10((double)numerator_rms / (double)denominator_rms));
}

static float ratio_from_counts(size_t numerator, size_t denominator)
{
    if (denominator == 0) {
        return 1.0f;
    }
    return (float)((double)numerator / (double)denominator);
}

static void evaluate_result(esp_err_t err, nuwax_p3_softref_aec_ab_result_t *r)
{
    if (!r) {
        return;
    }
    r->corr_abs_before = fabsf(r->corr_before);
    r->corr_abs_after = fabsf(r->corr_after);
    r->corr_abs_delta = r->corr_abs_before - r->corr_abs_after;
    r->ref_underrun_ratio = ratio_from_counts(r->ref_underrun, r->samples);
    r->active_ref_ratio = ratio_from_counts(r->active_samples, r->samples);
    r->active_corr_abs_after = fabsf(r->active_corr_after);
    r->quiet_out_to_mic_db = db_ratio(r->quiet_out_rms, r->quiet_mic_rms);
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
                       r->read_timeouts <= (uint32_t)CONFIG_NUWAX_P3_SOFTREF_AB_MAX_READ_TIMEOUTS &&
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
    size_t min_internal_free = r->heap_process_min_internal_free;
    if (min_internal_free == 0 || (r->heap_internal_free > 0 && r->heap_internal_free < min_internal_free)) {
        min_internal_free = r->heap_internal_free;
    }
    size_t min_internal_largest = r->heap_process_min_internal_largest;
    if (min_internal_largest == 0 ||
        (r->heap_internal_largest > 0 && r->heap_internal_largest < min_internal_largest)) {
        min_internal_largest = r->heap_internal_largest;
    }
    r->resource_ok = min_internal_free >= (size_t)CONFIG_NUWAX_P3_SOFTREF_AB_MIN_INTERNAL_FREE &&
                     min_internal_largest >= (size_t)CONFIG_NUWAX_P3_SOFTREF_AB_MIN_INTERNAL_LARGEST;
    r->passed = r->data_plane_ok && r->quality_ok && r->resource_ok;
}

static void *alloc_aligned_prefer_psram(size_t bytes)
{
    void *ptr = heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) {
        ptr = heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return ptr;
}

static const char *log_prefix_for_source(nuwax_p3_aec_ref_source_t source)
{
    return source == NUWAX_P3_AEC_REF_SOURCE_HARDREF ? "P3-0H" : "P3-0S";
}

static void log_result(esp_err_t err,
                       nuwax_p3_aec_ref_source_t source,
                       const char *stage,
                       const nuwax_p3_softref_aec_ab_result_t *result)
{
    const nuwax_p3_softref_aec_ab_result_t zero = {0};
    const nuwax_p3_softref_aec_ab_result_t *r = result ? result : &zero;
    const char *prefix = log_prefix_for_source(source);
    ESP_LOGW(TAG,
             "%s-AEC-AB err=%s stage=%s diagnostic=1 ready=%d frames=%lu samples=%u chunk=%lu delay_samples=%lu "
             "aec_filter=%d aec_type=%s aec_mode=%s "
             "mic_rms=%.1f mic2_rms=%.1f ref_rms=%.1f out_rms=%.1f "
             "dual_mic_ready=%d dual_mic_corr=%.3f dual_mic_abs_corr=%.3f "
             "dual_mic_level_delta_db=%.2f dual_mic_diff_rms=%.1f dual_mic_diff_ratio=%.3f "
             "mic_peak=%ld ref_peak=%ld out_peak=%ld "
             "erle_db=%.2f corr_before=%.3f corr_after=%.3f ref_copied=%u ref_underrun=%u "
             "ref_underrun_ratio=%.3f corr_abs_before=%.3f corr_abs_after=%.3f corr_abs_delta=%.3f "
             "read_timeouts=%lu cpu_ms=%lld heap_internal=%u heap_internal_largest=%u heap_psram=%u "
             "min_ref_rms=%.1f min_erle_db=%.1f min_active_ref_ratio=%.3f "
             "max_corr_after_abs=%.3f max_underrun_ratio=%.3f "
             "min_heap_internal=%u min_heap_internal_largest=%u data_ok=%d quality_ok=%d resource_ok=%d passed=%d",
             prefix,
             esp_err_to_name(err),
             stage ? stage : "-",
             r->diagnostic_ready ? 1 : 0,
             (unsigned long)r->frames,
             (unsigned)r->samples,
             (unsigned long)r->chunk_samples,
             (unsigned long)r->delay_samples,
             CONFIG_NUWAX_P3_SOFTREF_AB_AEC_FILTER_LENGTH,
             NUWAX_P3_SOFTREF_AB_AEC_TYPE_NAME,
             NUWAX_P3_SOFTREF_AB_AEC_MODE_NAME,
             (double)r->mic_rms,
             (double)r->mic2_rms,
             (double)r->ref_rms,
             (double)r->out_rms,
             r->dual_mic_ready ? 1 : 0,
             (double)r->dual_mic_corr,
             (double)r->dual_mic_abs_corr,
             (double)r->dual_mic_level_delta_db,
             (double)r->dual_mic_diff_rms,
             (double)r->dual_mic_diff_ratio,
             (long)r->mic_peak,
             (long)r->ref_peak,
             (long)r->out_peak,
             (double)r->erle_db,
             (double)r->corr_before,
             (double)r->corr_after,
             (unsigned)r->ref_copied,
             (unsigned)r->ref_underrun,
             (double)r->ref_underrun_ratio,
             (double)r->corr_abs_before,
             (double)r->corr_abs_after,
             (double)r->corr_abs_delta,
             (unsigned long)r->read_timeouts,
             (long long)r->cpu_ms,
             (unsigned)r->heap_internal_free,
             (unsigned)r->heap_internal_largest,
             (unsigned)r->heap_psram_free,
             (double)((float)CONFIG_NUWAX_P3_SOFTREF_AB_MIN_REF_RMS),
             (double)NUWAX_CONFIG_DB_X100(CONFIG_NUWAX_P3_SOFTREF_AB_MIN_ERLE_DB),
             (double)NUWAX_CONFIG_RATIO_X1000(CONFIG_NUWAX_P3_SOFTREF_AB_MIN_ACTIVE_REF_RATIO),
             (double)NUWAX_CONFIG_RATIO_X1000(CONFIG_NUWAX_P3_SOFTREF_AB_MAX_CORR_AFTER_ABS),
             (double)NUWAX_CONFIG_RATIO_X1000(CONFIG_NUWAX_P3_SOFTREF_AB_MAX_UNDERRUN_RATIO),
             (unsigned)CONFIG_NUWAX_P3_SOFTREF_AB_MIN_INTERNAL_FREE,
             (unsigned)CONFIG_NUWAX_P3_SOFTREF_AB_MIN_INTERNAL_LARGEST,
             r->data_plane_ok ? 1 : 0,
             r->quality_ok ? 1 : 0,
             r->resource_ok ? 1 : 0,
             r->passed ? 1 : 0);
    ESP_LOGW(TAG,
             "%s-AEC-AB-ACTIVE active_frames=%lu active_samples=%u active_ref_ratio=%.3f "
             "active_mic_rms=%.1f active_mic2_rms=%.1f active_ref_rms=%.1f active_out_rms=%.1f "
             "active_dual_mic_voice_ok=%d active_dual_mic_corr=%.3f "
             "active_dual_mic_abs_corr=%.3f active_dual_mic_level_delta_db=%.2f "
             "active_dual_mic_diff_rms=%.1f active_dual_mic_diff_ratio=%.3f active_erle_db=%.2f "
             "active_corr_before=%.3f active_corr_after=%.3f active_corr_abs_after=%.3f active_quality_ok=%d "
             "quiet_frames=%lu quiet_samples=%u quiet_mic_rms=%.1f quiet_out_rms=%.1f quiet_out_to_mic_db=%.2f",
             prefix,
             (unsigned long)r->active_frames,
             (unsigned)r->active_samples,
             (double)r->active_ref_ratio,
             (double)r->active_mic_rms,
             (double)r->active_mic2_rms,
             (double)r->active_ref_rms,
             (double)r->active_out_rms,
             r->active_dual_mic_voice_ok ? 1 : 0,
             (double)r->active_dual_mic_corr,
             (double)r->active_dual_mic_abs_corr,
             (double)r->active_dual_mic_level_delta_db,
             (double)r->active_dual_mic_diff_rms,
             (double)r->active_dual_mic_diff_ratio,
             (double)r->active_erle_db,
             (double)r->active_corr_before,
             (double)r->active_corr_after,
             (double)r->active_corr_abs_after,
             r->active_quality_ok ? 1 : 0,
             (unsigned long)r->quiet_frames,
             (unsigned)r->quiet_samples,
             (double)r->quiet_mic_rms,
             (double)r->quiet_out_rms,
             (double)r->quiet_out_to_mic_db);
    ESP_LOGW(TAG,
             "%s-AEC-AB-HEAP start_internal=%u/%u softref_internal=%u/%u mic_internal=%u/%u "
             "aec_internal=%u/%u buffers_internal=%u/%u process_min_internal=%u/%u cleanup_internal=%u/%u "
             "start_dma=%u/%u buffers_dma=%u/%u process_min_dma=%u/%u psram_free=%u",
             prefix,
             (unsigned)r->heap_start_internal_free,
             (unsigned)r->heap_start_internal_largest,
             (unsigned)r->heap_after_softref_internal_free,
             (unsigned)r->heap_after_softref_internal_largest,
             (unsigned)r->heap_after_mic_internal_free,
             (unsigned)r->heap_after_mic_internal_largest,
             (unsigned)r->heap_after_aec_internal_free,
             (unsigned)r->heap_after_aec_internal_largest,
             (unsigned)r->heap_after_buffers_internal_free,
             (unsigned)r->heap_after_buffers_internal_largest,
             (unsigned)r->heap_process_min_internal_free,
             (unsigned)r->heap_process_min_internal_largest,
             (unsigned)r->heap_internal_free,
             (unsigned)r->heap_internal_largest,
             (unsigned)r->heap_start_dma_free,
             (unsigned)r->heap_start_dma_largest,
             (unsigned)r->heap_after_buffers_dma_free,
             (unsigned)r->heap_after_buffers_dma_largest,
             (unsigned)r->heap_process_min_dma_free,
             (unsigned)r->heap_process_min_dma_largest,
             (unsigned)r->heap_psram_free);
}

#if CONFIG_NUWAX_P3_SOFTWARE_REF_AEC_AB_ENABLE || CONFIG_NUWAX_P3_HARDREF_AEC_AB_ENABLE
static esp_err_t run_aec_ab(nuwax_p3_aec_ref_source_t source,
                            uint32_t duration_ms,
                            uint32_t delay_samples,
                            nuwax_p3_softref_aec_ab_result_t *out_result)
{
    nuwax_p3_softref_aec_ab_result_t result = {0};
    result.duration_ms = clamp_duration_ms(duration_ms);
    result.delay_samples = source == NUWAX_P3_AEC_REF_SOURCE_SOFTREF ? delay_samples : 0;
    if (out_result) {
        memset(out_result, 0, sizeof(*out_result));
    }

    esp_err_t err = ESP_OK;
    const char *stage = "start";
    nuwax_mic_input_t mic = {0};
    bool mic_ready = false;
    afe_aec_handle_t *aec = NULL;
    int16_t *mic_buf = NULL;
    int16_t *mic2_buf = NULL;
    int16_t *ref_buf = NULL;
    int16_t *mr_buf = NULL;
    int16_t *out_buf = NULL;

    const int64_t started_us = esp_timer_get_time();
    nuwax_p3_heap_snapshot_t heap = take_heap_snapshot();
    result.heap_start_internal_free = heap.internal_free;
    result.heap_start_internal_largest = heap.internal_largest;
    result.heap_start_dma_free = heap.dma_free;
    result.heap_start_dma_largest = heap.dma_largest;

    if (CONFIG_NUWAX_VOICE_SAMPLE_RATE != 16000 || CONFIG_NUWAX_VOICE_OUTPUT_SAMPLE_RATE != 16000) {
        err = ESP_ERR_INVALID_STATE;
        stage = "sample_rate";
        goto cleanup;
    }

    if (source == NUWAX_P3_AEC_REF_SOURCE_SOFTREF) {
        stage = "softref_init";
        err = nuwax_p3_software_ref_init(CONFIG_NUWAX_VOICE_OUTPUT_SAMPLE_RATE,
                                         CONFIG_NUWAX_P3_SOFTWARE_REF_CAPACITY_MS);
        if (err != ESP_OK) {
            goto cleanup;
        }
        nuwax_p3_software_ref_stats_t ref_stats = {0};
        nuwax_p3_software_ref_get_stats(&ref_stats);
        if (!ref_stats.enabled || !ref_stats.initialized) {
            err = ESP_ERR_INVALID_STATE;
            stage = "softref_stats";
            goto cleanup;
        }
        heap = take_heap_snapshot();
        result.heap_after_softref_internal_free = heap.internal_free;
        result.heap_after_softref_internal_largest = heap.internal_largest;
    } else {
        stage = "hardref_slot2";
        heap = take_heap_snapshot();
        result.heap_after_softref_internal_free = heap.internal_free;
        result.heap_after_softref_internal_largest = heap.internal_largest;
    }

    stage = "mic_init";
    err = nuwax_mic_input_init_with_owner(&mic, NUWAX_MIC_OWNER_CONVERSATION_CAPTURE);
    if (err != ESP_OK) {
        goto cleanup;
    }
    mic_ready = true;
    heap = take_heap_snapshot();
    result.heap_after_mic_internal_free = heap.internal_free;
    result.heap_after_mic_internal_largest = heap.internal_largest;

    stage = "aec_create";
    aec = afe_aec_create("MR",
                         CONFIG_NUWAX_P3_SOFTREF_AB_AEC_FILTER_LENGTH,
                         NUWAX_P3_SOFTREF_AB_AEC_TYPE,
                         NUWAX_P3_SOFTREF_AB_AEC_MODE);
    if (!aec) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    heap = take_heap_snapshot();
    result.heap_after_aec_internal_free = heap.internal_free;
    result.heap_after_aec_internal_largest = heap.internal_largest;
    stage = "aec_chunksize";
    const int chunk = afe_aec_get_chunksize(aec);
    if (chunk <= 0 || chunk > 2048) {
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }
    result.chunk_samples = (uint32_t)chunk;

    const size_t mono_bytes = (size_t)chunk * sizeof(int16_t);
    const size_t mr_bytes = (size_t)chunk * 2U * sizeof(int16_t);
    stage = "buffer_alloc";
    mic_buf = (int16_t *)alloc_aligned_prefer_psram(mono_bytes);
    mic2_buf = (int16_t *)alloc_aligned_prefer_psram(mono_bytes);
    ref_buf = (int16_t *)alloc_aligned_prefer_psram(mono_bytes);
    mr_buf = (int16_t *)alloc_aligned_prefer_psram(mr_bytes);
    out_buf = (int16_t *)alloc_aligned_prefer_psram(mono_bytes);
    if (!mic_buf || !mic2_buf || !ref_buf || !mr_buf || !out_buf) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    heap = take_heap_snapshot();
    result.heap_after_buffers_internal_free = heap.internal_free;
    result.heap_after_buffers_internal_largest = heap.internal_largest;
    result.heap_after_buffers_dma_free = heap.dma_free;
    result.heap_after_buffers_dma_largest = heap.dma_largest;
    result.heap_process_min_internal_free = heap.internal_free;
    result.heap_process_min_internal_largest = heap.internal_largest;
    result.heap_process_min_dma_free = heap.dma_free;
    result.heap_process_min_dma_largest = heap.dma_largest;

    const size_t target_samples = ((size_t)CONFIG_NUWAX_VOICE_SAMPLE_RATE * (size_t)result.duration_ms) / 1000U;
    size_t target_frames = (target_samples + (size_t)chunk - 1U) / (size_t)chunk;
    if (target_frames == 0) {
        target_frames = 1;
    }

    nuwax_p3_softref_ab_accum_t acc = {0};
    nuwax_p3_softref_ab_accum_t active_acc = {0};
    nuwax_p3_softref_ab_accum_t quiet_acc = {0};
    nuwax_p3_dual_mic_accum_t dual_acc = {0};
    nuwax_p3_dual_mic_accum_t active_dual_acc = {0};
    nuwax_p3_dual_mic_accum_t quiet_dual_acc = {0};
    for (size_t frame = 0; frame < target_frames; frame++) {
        size_t samples_read = 0;
        bool dual_read = false;
        memset(mic_buf, 0, mono_bytes);
        memset(mic2_buf, 0, mono_bytes);
        memset(ref_buf, 0, mono_bytes);
        memset(out_buf, 0, mono_bytes);
        if (source == NUWAX_P3_AEC_REF_SOURCE_SOFTREF) {
            stage = "mic_read";
            err = nuwax_mic_input_read_vocat_mic12_pcm16(&mic,
                                                         mic_buf,
                                                         mic2_buf,
                                                         (size_t)chunk,
                                                         NUWAX_P3_SOFTREF_AB_DEFAULT_TIMEOUT_MS,
                                                         &samples_read);
            dual_read = err == ESP_OK || err == ESP_ERR_TIMEOUT;
            if (err == ESP_ERR_NOT_SUPPORTED) {
                err = nuwax_mic_input_read_pcm16(&mic,
                                                 mic_buf,
                                                 (size_t)chunk,
                                                 NUWAX_P3_SOFTREF_AB_DEFAULT_TIMEOUT_MS,
                                                 &samples_read);
                dual_read = false;
            }
        } else {
            stage = "mic_mr_read";
            err = nuwax_mic_input_read_vocat_mr_pcm16(&mic,
                                                      mic_buf,
                                                      ref_buf,
                                                      (size_t)chunk,
                                                      NUWAX_P3_SOFTREF_AB_DEFAULT_TIMEOUT_MS,
                                                      &samples_read);
        }
        if (err == ESP_ERR_TIMEOUT) {
            result.read_timeouts++;
            err = ESP_OK;
        } else if (err != ESP_OK) {
            goto cleanup;
        }
        if (samples_read > (size_t)chunk) {
            samples_read = (size_t)chunk;
        }
        if (samples_read < (size_t)chunk) {
            memset(&mic_buf[samples_read], 0, ((size_t)chunk - samples_read) * sizeof(int16_t));
            memset(&mic2_buf[samples_read], 0, ((size_t)chunk - samples_read) * sizeof(int16_t));
            if (source == NUWAX_P3_AEC_REF_SOURCE_HARDREF) {
                memset(&ref_buf[samples_read], 0, ((size_t)chunk - samples_read) * sizeof(int16_t));
            }
        }

        if (source == NUWAX_P3_AEC_REF_SOURCE_SOFTREF) {
            const size_t copied = nuwax_p3_software_ref_read_delayed(ref_buf, (size_t)chunk, delay_samples);
            result.ref_copied += copied;
            result.ref_underrun += (size_t)chunk - copied;
        } else {
            result.ref_copied += samples_read;
            result.ref_underrun += (size_t)chunk - samples_read;
        }

        for (int i = 0; i < chunk; i++) {
            mr_buf[i * 2] = mic_buf[i];
            mr_buf[i * 2 + 1] = ref_buf[i];
        }

        stage = "aec_process";
        const size_t out_bytes = afe_aec_process(aec, mr_buf, out_buf);
        size_t out_samples = out_bytes / sizeof(int16_t);
        if (out_samples > (size_t)chunk) {
            out_samples = (size_t)chunk;
        }
        if (out_samples < (size_t)chunk) {
            memset(&out_buf[out_samples], 0, ((size_t)chunk - out_samples) * sizeof(int16_t));
        }

        stage = "process";
        nuwax_p3_softref_ab_accum_t frame_acc = {0};
        accumulate_frame(&frame_acc, mic_buf, ref_buf, out_buf, (size_t)chunk);
        merge_accum(&acc, &frame_acc);
        nuwax_p3_dual_mic_accum_t frame_dual_acc = {0};
        if (dual_read) {
            accumulate_dual_mic(&frame_dual_acc, mic_buf, mic2_buf, (size_t)chunk);
            merge_dual_mic(&dual_acc, &frame_dual_acc);
        }
        const float frame_ref_rms = rms_from_sumsq(frame_acc.sumsq_ref, frame_acc.samples);
        if (frame_ref_rms >= (float)CONFIG_NUWAX_P3_SOFTREF_AB_MIN_REF_RMS) {
            merge_accum(&active_acc, &frame_acc);
            if (dual_read) {
                merge_dual_mic(&active_dual_acc, &frame_dual_acc);
            }
            result.active_frames++;
        } else {
            merge_accum(&quiet_acc, &frame_acc);
            if (dual_read) {
                merge_dual_mic(&quiet_dual_acc, &frame_dual_acc);
            }
            result.quiet_frames++;
        }
        heap = take_heap_snapshot();
        update_min_heap(&result.heap_process_min_internal_free,
                        &result.heap_process_min_internal_largest,
                        heap.internal_free,
                        heap.internal_largest);
        update_min_heap(&result.heap_process_min_dma_free,
                        &result.heap_process_min_dma_largest,
                        heap.dma_free,
                        heap.dma_largest);
        result.frames++;
    }

    result.samples = acc.samples;
    result.mic_rms = rms_from_sumsq(acc.sumsq_mic, acc.samples);
    result.ref_rms = rms_from_sumsq(acc.sumsq_ref, acc.samples);
    result.out_rms = rms_from_sumsq(acc.sumsq_out, acc.samples);
    fill_dual_mic_metrics(result.mic_rms,
                          &dual_acc,
                          &result.dual_mic_ready,
                          &result.mic2_rms,
                          &result.dual_mic_corr,
                          &result.dual_mic_abs_corr,
                          &result.dual_mic_level_delta_db,
                          &result.dual_mic_diff_rms,
                          &result.dual_mic_diff_ratio);
    result.mic_peak = acc.mic_peak;
    result.ref_peak = acc.ref_peak;
    result.out_peak = acc.out_peak;
    result.erle_db = erle_from_rms(result.mic_rms, result.out_rms);
    result.corr_before = corr_from_dot(acc.dot_mic_ref, acc.sumsq_mic, acc.sumsq_ref);
    result.corr_after = corr_from_dot(acc.dot_out_ref, acc.sumsq_out, acc.sumsq_ref);
    result.active_samples = active_acc.samples;
    result.active_mic_rms = rms_from_sumsq(active_acc.sumsq_mic, active_acc.samples);
    result.active_ref_rms = rms_from_sumsq(active_acc.sumsq_ref, active_acc.samples);
    result.active_out_rms = rms_from_sumsq(active_acc.sumsq_out, active_acc.samples);
    bool active_dual_ready = false;
    fill_dual_mic_metrics(result.active_mic_rms,
                          &active_dual_acc,
                          &active_dual_ready,
                          &result.active_mic2_rms,
                          &result.active_dual_mic_corr,
                          &result.active_dual_mic_abs_corr,
                          &result.active_dual_mic_level_delta_db,
                          &result.active_dual_mic_diff_rms,
                          &result.active_dual_mic_diff_ratio);
    result.dual_mic_ready = result.dual_mic_ready || active_dual_ready;
    result.active_erle_db = erle_from_rms(result.active_mic_rms, result.active_out_rms);
    result.active_corr_before = corr_from_dot(active_acc.dot_mic_ref, active_acc.sumsq_mic, active_acc.sumsq_ref);
    result.active_corr_after = corr_from_dot(active_acc.dot_out_ref, active_acc.sumsq_out, active_acc.sumsq_ref);
    result.quiet_samples = quiet_acc.samples;
    result.quiet_mic_rms = rms_from_sumsq(quiet_acc.sumsq_mic, quiet_acc.samples);
    result.quiet_out_rms = rms_from_sumsq(quiet_acc.sumsq_out, quiet_acc.samples);
    (void)quiet_dual_acc;

cleanup:
    result.cpu_ms = (esp_timer_get_time() - started_us) / 1000;
    result.heap_internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    result.heap_internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    result.heap_psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    evaluate_result(err, &result);
    if (mic_ready) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(nuwax_mic_input_deinit(&mic));
    }
    if (aec) {
        afe_aec_destroy(aec);
    }
    heap_caps_free(mic_buf);
    heap_caps_free(mic2_buf);
    heap_caps_free(ref_buf);
    heap_caps_free(mr_buf);
    heap_caps_free(out_buf);
    if (out_result) {
        *out_result = result;
    }
    log_result(err, source, stage, &result);
    return err;
}
#endif

esp_err_t nuwax_p3_softref_aec_ab_run(uint32_t duration_ms,
                                      uint32_t delay_samples,
                                      nuwax_p3_softref_aec_ab_result_t *out_result)
{
    nuwax_p3_softref_aec_ab_result_t result = {0};
    result.duration_ms = clamp_duration_ms(duration_ms);
    result.delay_samples = delay_samples;
    if (out_result) {
        memset(out_result, 0, sizeof(*out_result));
    }

#if !CONFIG_NUWAX_P3_SOFTWARE_REF_AEC_AB_ENABLE
    log_result(ESP_ERR_NOT_SUPPORTED, NUWAX_P3_AEC_REF_SOURCE_SOFTREF, "disabled", &result);
    return ESP_ERR_NOT_SUPPORTED;
#elif !CONFIG_NUWAX_P3_SOFTWARE_REF_ENABLE
    log_result(ESP_ERR_NOT_SUPPORTED, NUWAX_P3_AEC_REF_SOURCE_SOFTREF, "softref_disabled", &result);
    return ESP_ERR_NOT_SUPPORTED;
#else
    return run_aec_ab(NUWAX_P3_AEC_REF_SOURCE_SOFTREF, duration_ms, delay_samples, out_result);
#endif
}

esp_err_t nuwax_p3_hardref_aec_ab_run(uint32_t duration_ms,
                                      nuwax_p3_softref_aec_ab_result_t *out_result)
{
    nuwax_p3_softref_aec_ab_result_t result = {0};
    result.duration_ms = clamp_duration_ms(duration_ms);
    result.delay_samples = 0;
    if (out_result) {
        memset(out_result, 0, sizeof(*out_result));
    }

#if !CONFIG_NUWAX_P3_HARDREF_AEC_AB_ENABLE
    log_result(ESP_ERR_NOT_SUPPORTED, NUWAX_P3_AEC_REF_SOURCE_HARDREF, "disabled", &result);
    return ESP_ERR_NOT_SUPPORTED;
#else
    return run_aec_ab(NUWAX_P3_AEC_REF_SOURCE_HARDREF, duration_ms, 0, out_result);
#endif
}
