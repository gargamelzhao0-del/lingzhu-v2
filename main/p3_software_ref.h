#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool enabled;
    bool initialized;
    uint32_t sample_rate_hz;
    uint32_t capacity_ms;
    size_t capacity_samples;
    size_t available_samples;
    uint64_t write_samples;
    uint64_t write_calls;
    uint64_t overwritten_samples;
    uint64_t read_calls;
    uint64_t underrun_samples;
    uint32_t last_write_ms;
    float last_rms;
    int32_t last_peak;
} nuwax_p3_software_ref_stats_t;

typedef struct {
    bool initialized;
    bool reanchor_wait_for_write;
    uint32_t generation;
    uint64_t source_end_index;
    uint64_t reanchor_wait_write_index;
    uint64_t anchor_events;
    uint64_t generation_resets;
    uint64_t resync_events;
    uint64_t successful_reads;
    uint64_t underrun_reads;
    uint64_t blocked_reanchor_reads;
    int64_t last_phase_error;
    int64_t min_phase_error;
    int64_t max_phase_error;
} nuwax_p3_software_ref_reader_t;

esp_err_t nuwax_p3_software_ref_init(uint32_t sample_rate_hz, uint32_t capacity_ms);
void nuwax_p3_software_ref_reset(void);
void nuwax_p3_software_ref_write(const int16_t *samples, size_t sample_count);
void nuwax_p3_software_ref_write_interleaved_lr(const int16_t *samples, size_t frame_count);
size_t nuwax_p3_software_ref_read_delayed(int16_t *out, size_t sample_count, uint32_t delay_samples);
size_t nuwax_p3_software_ref_read_delayed_pair(int16_t *first_out,
                                               uint32_t first_delay_samples,
                                               int16_t *second_out,
                                               uint32_t second_delay_samples,
                                               size_t sample_count,
                                               size_t *second_copied);
void nuwax_p3_software_ref_reader_reset(nuwax_p3_software_ref_reader_t *reader);
void nuwax_p3_software_ref_reader_invalidate(nuwax_p3_software_ref_reader_t *reader);
size_t nuwax_p3_software_ref_read_delayed_pair_continuous(
    nuwax_p3_software_ref_reader_t *reader,
    int16_t *first_out,
    uint32_t first_delay_samples,
    int16_t *second_out,
    uint32_t second_delay_samples,
    size_t sample_count,
    size_t *second_copied);
void nuwax_p3_software_ref_get_stats(nuwax_p3_software_ref_stats_t *out_stats);

typedef struct {
    uint32_t generation;
    uint64_t end_index;
    uint64_t write_index_snapshot;
    uint64_t oldest_index_snapshot;
    uint32_t min_delay_samples;
    uint32_t max_delay_samples;
    size_t window_samples;
    size_t span_samples;
    uint32_t last_write_age_ms;
    bool recent;
} nuwax_p3_software_ref_history_span_t;
esp_err_t nuwax_p3_software_ref_read_history_span_at(
    uint32_t expected_generation,
    uint64_t end_index,
    uint32_t min_delay_samples,
    uint32_t max_delay_samples,
    size_t window_samples,
    int16_t *out_span,
    size_t out_capacity_samples,
    nuwax_p3_software_ref_history_span_t *out_meta);
