#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool reference_available;
    bool reference_active;
    bool echo_likely;
    bool nearfield_safe;
    uint32_t mic1_avg_abs;
    uint32_t mic1_peak;
    uint32_t mic2_avg_abs;
    uint32_t mic2_peak;
    uint32_t diff_avg_abs;
    uint32_t diff_peak;
    uint32_t diff_energy_q15;
    uint32_t balance_q15;
    uint32_t reference_avg_abs;
    uint32_t reference_peak;
    uint32_t reference_correlation_q15;
} nuwax_wake_interrupt_filter_result_t;

/* Analyze one recognizer frame. Both reference buffers must belong to the
 * current software-reference generation; reference_available=false is a
 * fail-closed result even when the dual-microphone evidence looks human. */
void nuwax_wake_interrupt_filter_analyze(
    const int16_t *mic1,
    const int16_t *mic2,
    const int16_t *reference_primary,
    const int16_t *reference_secondary,
    size_t sample_count,
    bool reference_available,
    nuwax_wake_interrupt_filter_result_t *result);
