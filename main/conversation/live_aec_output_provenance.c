#include "conversation/live_aec_output_provenance.h"

#include <stdint.h>
#include <string.h>

static bool output_kind_valid(nuwax_live_aec_output_kind_t kind) {
    return kind >= NUWAX_LIVE_AEC_OUTPUT_AEC &&
           kind <= NUWAX_LIVE_AEC_OUTPUT_UNDERRUN;
}

void nuwax_live_aec_provenance_clear(
    nuwax_live_aec_provenance_queue_t *queue) {
    if (!queue) {
        return;
    }
    memset(queue, 0, sizeof(*queue));
}

bool nuwax_live_aec_provenance_append(
    nuwax_live_aec_provenance_queue_t *queue,
    nuwax_live_aec_output_kind_t kind,
    size_t sample_count) {
    if (!queue || !output_kind_valid(kind)) {
        return false;
    }
    if (sample_count == 0U) {
        return true;
    }
    if (sample_count > SIZE_MAX - queue->queued_samples) {
        return false;
    }

    if (queue->segment_count > 0U) {
        const size_t last_index =
            (queue->read_index + queue->segment_count - 1U) %
            NUWAX_LIVE_AEC_PROVENANCE_SEGMENT_CAPACITY;
        nuwax_live_aec_provenance_segment_t *last =
            &queue->segments[last_index];
        if (last->kind == kind) {
            if (sample_count > SIZE_MAX - last->sample_count) {
                return false;
            }
            last->sample_count += sample_count;
            queue->queued_samples += sample_count;
            return true;
        }
    }

    if (queue->segment_count >=
        NUWAX_LIVE_AEC_PROVENANCE_SEGMENT_CAPACITY) {
        return false;
    }
    const size_t write_index =
        (queue->read_index + queue->segment_count) %
        NUWAX_LIVE_AEC_PROVENANCE_SEGMENT_CAPACITY;
    queue->segments[write_index] =
        (nuwax_live_aec_provenance_segment_t){
            .kind = kind,
            .sample_count = sample_count,
        };
    queue->segment_count++;
    queue->queued_samples += sample_count;
    return true;
}

bool nuwax_live_aec_provenance_append_pair(
    nuwax_live_aec_provenance_queue_t *queue,
    nuwax_live_aec_output_kind_t first_kind,
    size_t first_sample_count,
    nuwax_live_aec_output_kind_t second_kind,
    size_t second_sample_count) {
    if (!queue) {
        return false;
    }
    const nuwax_live_aec_provenance_queue_t original = *queue;
    if (!nuwax_live_aec_provenance_append(
            queue, first_kind, first_sample_count) ||
        !nuwax_live_aec_provenance_append(
            queue, second_kind, second_sample_count)) {
        *queue = original;
        return false;
    }
    return true;
}

void nuwax_live_aec_output_stats_reset(
    nuwax_live_aec_output_stats_t *stats) {
    if (stats) {
        memset(stats, 0, sizeof(*stats));
    }
}

void nuwax_live_aec_output_stats_account(
    nuwax_live_aec_output_stats_t *stats,
    nuwax_live_aec_output_kind_t kind,
    size_t sample_count) {
    if (!stats || sample_count == 0U) {
        return;
    }
    switch (kind) {
        case NUWAX_LIVE_AEC_OUTPUT_AEC:
            stats->aec_samples += sample_count;
            break;
        case NUWAX_LIVE_AEC_OUTPUT_REF_MISSING:
            stats->ref_missing_samples += sample_count;
            stats->bypass_samples += sample_count;
            break;
        case NUWAX_LIVE_AEC_OUTPUT_BYPASS:
            stats->bypass_samples += sample_count;
            break;
        case NUWAX_LIVE_AEC_OUTPUT_UNDERRUN:
            stats->underrun_samples += sample_count;
            break;
        default:
            stats->underrun_samples += sample_count;
            break;
    }
}

size_t nuwax_live_aec_provenance_consume(
    nuwax_live_aec_provenance_queue_t *queue,
    size_t sample_count,
    nuwax_live_aec_output_stats_t *stats) {
    if (!queue || !stats || sample_count == 0U) {
        return 0U;
    }
    size_t consumed = 0U;
    while (consumed < sample_count && queue->segment_count > 0U) {
        nuwax_live_aec_provenance_segment_t *segment =
            &queue->segments[queue->read_index];
        const size_t remaining = sample_count - consumed;
        const size_t take =
            segment->sample_count < remaining
                ? segment->sample_count
                : remaining;
        nuwax_live_aec_output_stats_account(stats, segment->kind, take);
        segment->sample_count -= take;
        queue->queued_samples -= take;
        consumed += take;
        if (segment->sample_count == 0U) {
            queue->read_index =
                (queue->read_index + 1U) %
                NUWAX_LIVE_AEC_PROVENANCE_SEGMENT_CAPACITY;
            queue->segment_count--;
        }
    }
    if (queue->segment_count == 0U) {
        queue->read_index = 0U;
    }
    return consumed;
}

bool nuwax_live_aec_output_stats_fully_aec(
    const nuwax_live_aec_output_stats_t *stats,
    size_t expected_samples) {
    return stats && expected_samples > 0U &&
           stats->aec_samples == expected_samples &&
           stats->bypass_samples == 0U &&
           stats->underrun_samples == 0U;
}

bool nuwax_live_aec_output_stats_has_fallback(
    const nuwax_live_aec_output_stats_t *stats) {
    return stats &&
           (stats->bypass_samples > 0U ||
            stats->underrun_samples > 0U);
}
