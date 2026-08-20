/* Host-only check. This file is intentionally excluded from firmware CMake sources. */

#include <stdbool.h>
#include <stdio.h>

#include "conversation/live_aec_output_provenance.h"

#define CHECK(condition, message)                                      \
    do {                                                               \
        if (!(condition)) {                                            \
            printf("[FAIL] %s:%d: %s\n", __func__, __LINE__, message); \
            return 1;                                                  \
        }                                                              \
    } while (0)

static int check_mixed_output_accounting(void) {
    nuwax_live_aec_provenance_queue_t queue = {0};
    nuwax_live_aec_output_stats_t stats = {0};
    CHECK(nuwax_live_aec_provenance_append_pair(
              &queue,
              NUWAX_LIVE_AEC_OUTPUT_AEC,
              192U,
              NUWAX_LIVE_AEC_OUTPUT_REF_MISSING,
              128U),
          "mixed provenance append failed");
    CHECK(nuwax_live_aec_provenance_consume(&queue, 320U, &stats) == 320U,
          "mixed provenance consume was short");
    CHECK(stats.aec_samples == 192U &&
              stats.bypass_samples == 128U &&
              stats.ref_missing_samples == 128U &&
              stats.underrun_samples == 0U,
          "mixed provenance counts are wrong");
    CHECK(!nuwax_live_aec_output_stats_fully_aec(&stats, 320U) &&
              nuwax_live_aec_output_stats_has_fallback(&stats),
          "mixed output was not classified as fallback");
    return 0;
}

static int check_frame_adapter_320_to_512(void) {
    nuwax_live_aec_provenance_queue_t queue = {0};
    size_t pending_samples = 0U;
    bool output_started = false;
    bool found_fifo_only_aec_frame = false;
    bool found_mixed_underrun_frame = false;

    for (size_t frame = 0U; frame < 16U; frame++) {
        bool produced_chunk = false;
        pending_samples += 320U;
        while (pending_samples >= 512U) {
            CHECK(nuwax_live_aec_provenance_append(
                      &queue, NUWAX_LIVE_AEC_OUTPUT_AEC, 512U),
                  "adapter provenance append failed");
            pending_samples -= 512U;
            produced_chunk = true;
        }

        nuwax_live_aec_output_stats_t stats = {0};
        if (!output_started && queue.queued_samples < 320U) {
            nuwax_live_aec_output_stats_account(
                &stats, NUWAX_LIVE_AEC_OUTPUT_UNDERRUN, 320U);
        } else {
            output_started = true;
            const size_t consumed =
                nuwax_live_aec_provenance_consume(&queue, 320U, &stats);
            if (consumed < 320U) {
                nuwax_live_aec_output_stats_account(
                    &stats,
                    NUWAX_LIVE_AEC_OUTPUT_UNDERRUN,
                    320U - consumed);
            }
        }

        if (!produced_chunk &&
            nuwax_live_aec_output_stats_fully_aec(&stats, 320U)) {
            found_fifo_only_aec_frame = true;
        }
        if (stats.aec_samples > 0U && stats.underrun_samples > 0U) {
            found_mixed_underrun_frame = true;
        }
    }

    CHECK(found_fifo_only_aec_frame,
          "320/512 adapter never exposed an all-AEC FIFO frame without new chunk output");
    CHECK(found_mixed_underrun_frame,
          "320/512 adapter did not preserve mixed AEC/underrun provenance");
    return 0;
}

static int check_pair_append_is_transactional(void) {
    nuwax_live_aec_provenance_queue_t queue = {0};
    for (size_t i = 0U;
         i < NUWAX_LIVE_AEC_PROVENANCE_SEGMENT_CAPACITY;
         i++) {
        const nuwax_live_aec_output_kind_t kind =
            (i & 1U) == 0U
                ? NUWAX_LIVE_AEC_OUTPUT_AEC
                : NUWAX_LIVE_AEC_OUTPUT_BYPASS;
        CHECK(nuwax_live_aec_provenance_append(&queue, kind, 1U),
              "failed to fill provenance queue");
    }
    const size_t queued_before = queue.queued_samples;
    const size_t segments_before = queue.segment_count;
    CHECK(!nuwax_live_aec_provenance_append_pair(
              &queue,
              NUWAX_LIVE_AEC_OUTPUT_REF_MISSING,
              1U,
              NUWAX_LIVE_AEC_OUTPUT_UNDERRUN,
              1U),
          "full provenance queue accepted an overflowing pair");
    CHECK(queue.queued_samples == queued_before &&
              queue.segment_count == segments_before,
          "failed pair append partially mutated provenance queue");
    return 0;
}

int main(void) {
    int failures = 0;
    failures += check_mixed_output_accounting();
    failures += check_frame_adapter_320_to_512();
    failures += check_pair_append_is_transactional();
    printf("live AEC output provenance host check: %s (%d failures)\n",
           failures == 0 ? "PASS" : "FAIL",
           failures);
    return failures == 0 ? 0 : 1;
}
