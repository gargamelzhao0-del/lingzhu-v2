#include <assert.h>
#include <stdio.h>

#include "conversation/wake_word_interrupt_filter.h"

#define FRAME_SAMPLES 512U

int main(void) {
    int16_t mic1[FRAME_SAMPLES] = {0};
    int16_t mic2[FRAME_SAMPLES] = {0};
    int16_t ref1[FRAME_SAMPLES] = {0};
    int16_t ref2[FRAME_SAMPLES] = {0};
    nuwax_wake_interrupt_filter_result_t result = {0};

    for (size_t i = 0U; i < FRAME_SAMPLES; ++i) {
        ref1[i] = (int16_t)(((int)(i * 37U) % 29 - 14) * 220);
        ref2[i] = ref1[i];
        mic1[i] = (int16_t)(ref1[i] / 2);
        mic2[i] = (int16_t)(ref1[i] * 45 / 100);
    }
    nuwax_wake_interrupt_filter_analyze(
        mic1, mic2, ref1, ref2, FRAME_SAMPLES, true, &result);
    assert(result.reference_available);
    assert(result.reference_active);
    assert(result.echo_likely);
    assert(!result.nearfield_safe);

    for (size_t i = 0U; i < FRAME_SAMPLES; ++i) {
        mic1[i] = (int16_t)(((int)(i * 37U) % 23 - 11) * 330);
        mic2[i] = (int16_t)(((int)(i * 17U) % 19 - 9) * 180);
        ref1[i] = (int16_t)(((int)(i * 5U) % 7 - 3) * 100);
        ref2[i] = (int16_t)(((int)(i * 11U) % 13 - 6) * 80);
    }
    nuwax_wake_interrupt_filter_analyze(
        mic1, mic2, ref1, ref2, FRAME_SAMPLES, true, &result);
    assert(result.reference_available);
    assert(!result.echo_likely);
    assert(result.nearfield_safe);

    nuwax_wake_interrupt_filter_analyze(
        mic1, mic2, ref1, ref2, FRAME_SAMPLES, false, &result);
    assert(!result.reference_available);
    assert(!result.nearfield_safe);

    for (size_t i = 0U; i < FRAME_SAMPLES; ++i) {
        ref1[i] = 0;
        ref2[i] = 0;
    }
    nuwax_wake_interrupt_filter_analyze(
        mic1, mic2, ref1, ref2, FRAME_SAMPLES, true, &result);
    assert(result.reference_available);
    assert(!result.reference_active);
    assert(result.nearfield_safe);

    for (size_t i = 0U; i < FRAME_SAMPLES; ++i) {
        const int sign = (i & 1U) == 0U ? 1 : -1;
        const bool peak_sample = (i % 16U) == 0U;
        mic1[i] = (int16_t)(sign * (peak_sample ? 1600 : 560));
        mic2[i] = (int16_t)(sign * (peak_sample ? 900 : 300));
        ref1[i] = (int16_t)(((int)(i * 5U) % 7 - 3) * 100);
        ref2[i] = (int16_t)(((int)(i * 11U) % 13 - 6) * 80);
    }
    nuwax_wake_interrupt_filter_analyze(
        mic1, mic2, ref1, ref2, FRAME_SAMPLES, true, &result);
    assert(result.reference_active);
    assert(!result.echo_likely);
    assert(result.mic1_avg_abs >= 550U && result.mic1_avg_abs < 650U);
    assert(result.mic1_peak >= 1550U && result.mic1_peak < 1800U);
    assert(result.mic2_avg_abs >= 300U && result.mic2_avg_abs < 350U);
    assert(result.mic2_peak >= 850U && result.mic2_peak < 1000U);
    assert(result.nearfield_safe);

    for (size_t i = 0U; i < FRAME_SAMPLES; ++i) {
        mic1[i] = (int16_t)((int)(i % 5U) - 2);
        mic2[i] = mic1[i];
    }
    nuwax_wake_interrupt_filter_analyze(
        mic1, mic2, ref1, ref2, FRAME_SAMPLES, true, &result);
    assert(!result.nearfield_safe);

    puts("wake word interrupt filter host check: PASS");
    return 0;
}
