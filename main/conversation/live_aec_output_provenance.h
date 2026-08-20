#pragma once

#include <stdbool.h>
#include <stddef.h>

#define NUWAX_LIVE_AEC_PROVENANCE_SEGMENT_CAPACITY 12U

typedef enum {
    NUWAX_LIVE_AEC_OUTPUT_AEC = 0,
    NUWAX_LIVE_AEC_OUTPUT_BYPASS,
    NUWAX_LIVE_AEC_OUTPUT_REF_MISSING,
    NUWAX_LIVE_AEC_OUTPUT_UNDERRUN,
} nuwax_live_aec_output_kind_t;

typedef struct {
    nuwax_live_aec_output_kind_t kind;
    size_t sample_count;
} nuwax_live_aec_provenance_segment_t;

typedef struct {
    nuwax_live_aec_provenance_segment_t
        segments[NUWAX_LIVE_AEC_PROVENANCE_SEGMENT_CAPACITY];
    size_t read_index;
    size_t segment_count;
    size_t queued_samples;
} nuwax_live_aec_provenance_queue_t;

typedef struct {
    size_t aec_samples;
    size_t bypass_samples;
    size_t ref_missing_samples;
    size_t underrun_samples;
} nuwax_live_aec_output_stats_t;

void nuwax_live_aec_provenance_clear(
    nuwax_live_aec_provenance_queue_t *queue);
bool nuwax_live_aec_provenance_append(
    nuwax_live_aec_provenance_queue_t *queue,
    nuwax_live_aec_output_kind_t kind,
    size_t sample_count);
bool nuwax_live_aec_provenance_append_pair(
    nuwax_live_aec_provenance_queue_t *queue,
    nuwax_live_aec_output_kind_t first_kind,
    size_t first_sample_count,
    nuwax_live_aec_output_kind_t second_kind,
    size_t second_sample_count);
size_t nuwax_live_aec_provenance_consume(
    nuwax_live_aec_provenance_queue_t *queue,
    size_t sample_count,
    nuwax_live_aec_output_stats_t *stats);
void nuwax_live_aec_output_stats_reset(
    nuwax_live_aec_output_stats_t *stats);
void nuwax_live_aec_output_stats_account(
    nuwax_live_aec_output_stats_t *stats,
    nuwax_live_aec_output_kind_t kind,
    size_t sample_count);
bool nuwax_live_aec_output_stats_fully_aec(
    const nuwax_live_aec_output_stats_t *stats,
    size_t expected_samples);
bool nuwax_live_aec_output_stats_has_fallback(
    const nuwax_live_aec_output_stats_t *stats);
