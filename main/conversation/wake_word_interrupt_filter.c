#include "wake_word_interrupt_filter.h"

#include <limits.h>
#include <string.h>

#define NUWAX_WAKE_INTERRUPT_MIN_MIC1_AVG_ABS 550U
#define NUWAX_WAKE_INTERRUPT_MIN_MIC1_PEAK 1550U
#define NUWAX_WAKE_INTERRUPT_MIN_MIC2_AVG_ABS 300U
#define NUWAX_WAKE_INTERRUPT_MIN_MIC2_PEAK 850U
#define NUWAX_WAKE_INTERRUPT_MIN_DIFF_AVG_ABS 150U
#define NUWAX_WAKE_INTERRUPT_MIN_DIFF_PEAK 600U
#define NUWAX_WAKE_INTERRUPT_MIN_DIFF_ENERGY_Q15 3500U
#define NUWAX_WAKE_INTERRUPT_MIN_BALANCE_Q15 6000U
#define NUWAX_WAKE_INTERRUPT_MAX_SAFE_REF_CORR_Q15 12000U
#define NUWAX_WAKE_INTERRUPT_ECHO_REF_CORR_Q15 18000U
#define NUWAX_WAKE_INTERRUPT_REF_ACTIVE_AVG_ABS 20U
#define NUWAX_WAKE_INTERRUPT_REF_ACTIVE_PEAK 60U

typedef struct {
    int32_t mean;
    uint32_t avg_abs;
    uint32_t peak;
} signal_stats_t;

static uint32_t abs_i32(int32_t value) {
    if (value >= 0) {
        return (uint32_t)value;
    }
    return value == INT32_MIN ? (uint32_t)INT32_MAX + 1U
                              : (uint32_t)(-value);
}

static signal_stats_t signal_stats(const int16_t *samples,
                                   size_t sample_count) {
    signal_stats_t stats = {0};
    if (!samples || sample_count == 0U) {
        return stats;
    }
    int64_t sum = 0;
    uint64_t sum_abs = 0U;
    for (size_t i = 0U; i < sample_count; ++i) {
        const int32_t sample = samples[i];
        const uint32_t magnitude = abs_i32(sample);
        sum += sample;
        sum_abs += magnitude;
        if (magnitude > stats.peak) {
            stats.peak = magnitude;
        }
    }
    stats.mean = (int32_t)(sum / (int64_t)sample_count);
    stats.avg_abs = (uint32_t)(sum_abs / sample_count);
    return stats;
}

static uint64_t integer_sqrt_u64(uint64_t value) {
    uint64_t result = 0U;
    uint64_t bit = 1ULL << 62U;
    while (bit > value) {
        bit >>= 2U;
    }
    while (bit != 0U) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1U) + bit;
        } else {
            result >>= 1U;
        }
        bit >>= 2U;
    }
    return result;
}

static uint32_t normalized_correlation_q15(const int16_t *first,
                                           int32_t first_mean,
                                           const int16_t *second,
                                           int32_t second_mean,
                                           size_t sample_count) {
    if (!first || !second || sample_count == 0U) {
        return 0U;
    }
    int64_t dot = 0;
    uint64_t first_energy = 0U;
    uint64_t second_energy = 0U;
    for (size_t i = 0U; i < sample_count; ++i) {
        const int32_t a = (int32_t)first[i] - first_mean;
        const int32_t b = (int32_t)second[i] - second_mean;
        dot += (int64_t)a * b;
        first_energy += (uint64_t)((int64_t)a * a);
        second_energy += (uint64_t)((int64_t)b * b);
    }
    const uint64_t denominator = integer_sqrt_u64(first_energy) *
                                 integer_sqrt_u64(second_energy);
    if (denominator == 0U) {
        return 0U;
    }
    const uint64_t magnitude = dot < 0 ? (uint64_t)(-dot) : (uint64_t)dot;
    uint64_t q15 = (magnitude << 15U) / denominator;
    if (q15 > 32768U) {
        q15 = 32768U;
    }
    return (uint32_t)q15;
}

void nuwax_wake_interrupt_filter_analyze(
    const int16_t *mic1,
    const int16_t *mic2,
    const int16_t *reference_primary,
    const int16_t *reference_secondary,
    size_t sample_count,
    bool reference_available,
    nuwax_wake_interrupt_filter_result_t *result) {
    if (!result) {
        return;
    }
    memset(result, 0, sizeof(*result));
    if (!mic1 || !mic2 || sample_count == 0U) {
        return;
    }

    const signal_stats_t mic1_stats = signal_stats(mic1, sample_count);
    const signal_stats_t mic2_stats = signal_stats(mic2, sample_count);
    const signal_stats_t ref1_stats =
        signal_stats(reference_primary, sample_count);
    const signal_stats_t ref2_stats =
        signal_stats(reference_secondary, sample_count);
    uint64_t diff_sum_abs = 0U;
    uint32_t diff_peak = 0U;
    for (size_t i = 0U; i < sample_count; ++i) {
        const uint32_t magnitude =
            abs_i32((int32_t)mic1[i] - (int32_t)mic2[i]);
        diff_sum_abs += magnitude;
        if (magnitude > diff_peak) {
            diff_peak = magnitude;
        }
    }

    result->reference_available = reference_available;
    result->mic1_avg_abs = mic1_stats.avg_abs;
    result->mic1_peak = mic1_stats.peak;
    result->mic2_avg_abs = mic2_stats.avg_abs;
    result->mic2_peak = mic2_stats.peak;
    result->diff_avg_abs = (uint32_t)(diff_sum_abs / sample_count);
    result->diff_peak = diff_peak;
    result->diff_energy_q15 = mic1_stats.avg_abs == 0U
                                  ? 0U
                                  : (uint32_t)(((uint64_t)result->diff_avg_abs << 15U) /
                                               mic1_stats.avg_abs);
    const uint32_t stronger_avg = mic1_stats.avg_abs > mic2_stats.avg_abs
                                      ? mic1_stats.avg_abs
                                      : mic2_stats.avg_abs;
    const uint32_t weaker_avg = mic1_stats.avg_abs > mic2_stats.avg_abs
                                    ? mic2_stats.avg_abs
                                    : mic1_stats.avg_abs;
    result->balance_q15 = stronger_avg == 0U
                              ? 0U
                              : (uint32_t)(((uint64_t)weaker_avg << 15U) /
                                           stronger_avg);
    result->reference_avg_abs = ref1_stats.avg_abs > ref2_stats.avg_abs
                                    ? ref1_stats.avg_abs
                                    : ref2_stats.avg_abs;
    result->reference_peak = ref1_stats.peak > ref2_stats.peak
                                 ? ref1_stats.peak
                                 : ref2_stats.peak;
    result->reference_active = reference_available &&
                               (result->reference_avg_abs >=
                                    NUWAX_WAKE_INTERRUPT_REF_ACTIVE_AVG_ABS ||
                                result->reference_peak >=
                                    NUWAX_WAKE_INTERRUPT_REF_ACTIVE_PEAK);

    if (reference_available && reference_primary && reference_secondary) {
        const uint32_t primary_corr = normalized_correlation_q15(
            mic1,
            mic1_stats.mean,
            reference_primary,
            ref1_stats.mean,
            sample_count);
        const uint32_t secondary_corr = normalized_correlation_q15(
            mic1,
            mic1_stats.mean,
            reference_secondary,
            ref2_stats.mean,
            sample_count);
        result->reference_correlation_q15 =
            primary_corr > secondary_corr ? primary_corr : secondary_corr;
    }
    result->echo_likely = result->reference_active &&
                          result->reference_correlation_q15 >=
                              NUWAX_WAKE_INTERRUPT_ECHO_REF_CORR_Q15;

    const bool reference_separated =
        !result->reference_active ||
        result->reference_correlation_q15 <=
            NUWAX_WAKE_INTERRUPT_MAX_SAFE_REF_CORR_Q15;
    result->nearfield_safe =
        reference_available && reference_separated && !result->echo_likely &&
        result->mic1_avg_abs >= NUWAX_WAKE_INTERRUPT_MIN_MIC1_AVG_ABS &&
        result->mic1_peak >= NUWAX_WAKE_INTERRUPT_MIN_MIC1_PEAK &&
        result->mic2_avg_abs >= NUWAX_WAKE_INTERRUPT_MIN_MIC2_AVG_ABS &&
        result->mic2_peak >= NUWAX_WAKE_INTERRUPT_MIN_MIC2_PEAK &&
        result->diff_avg_abs >= NUWAX_WAKE_INTERRUPT_MIN_DIFF_AVG_ABS &&
        result->diff_peak >= NUWAX_WAKE_INTERRUPT_MIN_DIFF_PEAK &&
        result->diff_energy_q15 >=
            NUWAX_WAKE_INTERRUPT_MIN_DIFF_ENERGY_Q15 &&
        result->balance_q15 >= NUWAX_WAKE_INTERRUPT_MIN_BALANCE_Q15;
}
