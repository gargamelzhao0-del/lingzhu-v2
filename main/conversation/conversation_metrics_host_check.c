/* Host-only check. This file is intentionally excluded from firmware CMake sources. */

#include <stdio.h>
#include <string.h>

#include "conversation/conversation_metrics.h"

static int expect_true(const char *name, bool condition) {
    printf("[%s] %s\n", condition ? "PASS" : "FAIL", name);
    return condition ? 0 : 1;
}

int main(void) {
    int failures = 0;
    nuwax_conversation_metrics_t metrics;
    nuwax_conversation_metrics_snapshot_t snapshot;

    failures += expect_true("null init rejected",
                            !nuwax_conversation_metrics_init(NULL, 1, 100));
    failures += expect_true("init",
                            nuwax_conversation_metrics_init(&metrics, 41, 100));

    static const uint64_t samples_us[] = {
        1000, 2000, 3000, 4000, 5000,
        10000, 20000, 30000, 100000, 300000,
    };
    for (size_t i = 0; i < sizeof(samples_us) / sizeof(samples_us[0]); i++) {
        failures += expect_true(
            "record duration",
            nuwax_conversation_metrics_record_duration(
                &metrics, NUWAX_CONVERSATION_LATENCY_DSP_PROCESS, samples_us[i]));
    }

    nuwax_conversation_timepoint_t timepoint = {0};
    failures += expect_true(
        "invalid timepoint rejected",
        !nuwax_conversation_metrics_record_since(
            &metrics, NUWAX_CONVERSATION_LATENCY_UPLINK_QUEUE, &timepoint, 1000));
    failures += expect_true(
        "timepoint mark", nuwax_conversation_timepoint_mark(&timepoint, 1000));
    failures += expect_true(
        "timepoint mark regression rejected",
        !nuwax_conversation_timepoint_mark(&timepoint, 999));
    failures += expect_true(
        "elapsed timepoint recorded",
        nuwax_conversation_metrics_record_since(
            &metrics, NUWAX_CONVERSATION_LATENCY_UPLINK_QUEUE, &timepoint, 2500));
    failures += expect_true(
        "regressing timepoint rejected",
        !nuwax_conversation_metrics_record_since(
            &metrics, NUWAX_CONVERSATION_LATENCY_UPLINK_QUEUE, &timepoint, 999));
    nuwax_conversation_timepoint_clear(&timepoint);
    failures += expect_true("timepoint clear", !timepoint.valid && timepoint.at_us == 0);

    failures += expect_true(
        "queue observation one",
        nuwax_conversation_metrics_observe_queue(
            &metrics, NUWAX_CONVERSATION_QUEUE_UPLINK, 2, 8));
    failures += expect_true(
        "queue high water",
        nuwax_conversation_metrics_observe_queue(
            &metrics, NUWAX_CONVERSATION_QUEUE_UPLINK, 7, 8));
    failures += expect_true(
        "queue over capacity",
        nuwax_conversation_metrics_observe_queue(
            &metrics, NUWAX_CONVERSATION_QUEUE_UPLINK, 9, 8));

    failures += expect_true(
        "event counter",
        nuwax_conversation_metrics_count_event(
            &metrics, NUWAX_CONVERSATION_EVENT_SEQUENCE_GAP, 3));

    failures += expect_true(
        "aec initializing",
        nuwax_conversation_metrics_set_aec_runtime(
            &metrics, NUWAX_AEC_RUNTIME_INITIALIZING, NUWAX_AEC_REASON_NOT_PREPARED, 200));
    failures += expect_true(
        "aec healthy",
        nuwax_conversation_metrics_set_aec_runtime(
            &metrics, NUWAX_AEC_RUNTIME_HEALTHY, NUWAX_AEC_REASON_NONE, 500));
    failures += expect_true(
        "aec degraded",
        nuwax_conversation_metrics_set_aec_runtime(
            &metrics,
            NUWAX_AEC_RUNTIME_DEGRADED,
            NUWAX_AEC_REASON_REFERENCE_GAP | NUWAX_AEC_REASON_DELAY_UNLOCKED,
            900));
    failures += expect_true(
        "aec regressing timestamp rejected",
        !nuwax_conversation_metrics_set_aec_runtime(
            &metrics, NUWAX_AEC_RUNTIME_HEALTHY, NUWAX_AEC_REASON_NONE, 899));

    failures += expect_true(
        "snapshot",
        nuwax_conversation_metrics_snapshot(&metrics, 1200, &snapshot));
    const nuwax_conversation_latency_summary_t *dsp =
        &snapshot.latency[NUWAX_CONVERSATION_LATENCY_DSP_PROCESS];
    failures += expect_true(
        "duration statistics",
        dsp->count == 10 && dsp->min_us == 1000 && dsp->max_us == 300000 &&
            dsp->p50_us == 5000 && dsp->p95_us == 300000 && dsp->p99_us == 300000);
    failures += expect_true(
        "timepoint statistics",
        snapshot.latency[NUWAX_CONVERSATION_LATENCY_UPLINK_QUEUE].count == 1 &&
            snapshot.latency[NUWAX_CONVERSATION_LATENCY_UPLINK_QUEUE].min_us == 1500);
    failures += expect_true(
        "rejected timing count", snapshot.rejected_timing_samples == 3);
    failures += expect_true(
        "queue summary",
        snapshot.queues[NUWAX_CONVERSATION_QUEUE_UPLINK].observations == 3 &&
            snapshot.queues[NUWAX_CONVERSATION_QUEUE_UPLINK].last_depth == 9 &&
            snapshot.queues[NUWAX_CONVERSATION_QUEUE_UPLINK].high_watermark == 9 &&
            snapshot.queues[NUWAX_CONVERSATION_QUEUE_UPLINK].last_capacity == 8 &&
            snapshot.queues[NUWAX_CONVERSATION_QUEUE_UPLINK].over_capacity_observations == 1);
    failures += expect_true(
        "event summary",
        snapshot.events[NUWAX_CONVERSATION_EVENT_SEQUENCE_GAP] == 3);
    failures += expect_true(
        "aec current and worst state",
        snapshot.aec.current_state == NUWAX_AEC_RUNTIME_DEGRADED &&
            snapshot.aec.worst_state == NUWAX_AEC_RUNTIME_DEGRADED &&
            snapshot.aec.transitions == 3);
    failures += expect_true(
        "aec reason accumulation",
        snapshot.aec.current_reasons ==
                (NUWAX_AEC_REASON_REFERENCE_GAP | NUWAX_AEC_REASON_DELAY_UNLOCKED) &&
            (snapshot.aec.observed_reasons & NUWAX_AEC_REASON_NOT_PREPARED) != 0 &&
            (snapshot.aec.observed_reasons & NUWAX_AEC_REASON_REFERENCE_GAP) != 0);
    failures += expect_true(
        "aec state durations",
        snapshot.aec.duration_us[NUWAX_AEC_RUNTIME_UNKNOWN] == 100 &&
            snapshot.aec.duration_us[NUWAX_AEC_RUNTIME_INITIALIZING] == 300 &&
            snapshot.aec.duration_us[NUWAX_AEC_RUNTIME_HEALTHY] == 400 &&
            snapshot.aec.duration_us[NUWAX_AEC_RUNTIME_DEGRADED] == 300);
    failures += expect_true(
        "round metadata",
        snapshot.round_id == 41 && snapshot.round_started_us == 100 &&
            snapshot.snapshot_us == 1200 && snapshot.round_elapsed_us == 1100);
    failures += expect_true(
        "names",
        strcmp(nuwax_conversation_latency_name(NUWAX_CONVERSATION_LATENCY_DSP_PROCESS),
               "dsp_process") == 0 &&
            strcmp(nuwax_conversation_queue_name(NUWAX_CONVERSATION_QUEUE_SPEAKER),
                   "speaker") == 0 &&
            strcmp(nuwax_conversation_event_name(NUWAX_CONVERSATION_EVENT_AEC_FALLBACK),
                   "aec_fallback") == 0 &&
            strcmp(nuwax_aec_runtime_state_name(NUWAX_AEC_RUNTIME_HEALTHY),
                   "healthy") == 0);

    failures += expect_true(
        "snapshot timestamp regression rejected",
        !nuwax_conversation_metrics_snapshot(&metrics, 50, &snapshot));
    failures += expect_true(
        "round reset", nuwax_conversation_metrics_reset(&metrics, 42, 5000));
    failures += expect_true(
        "snapshot after reset",
        nuwax_conversation_metrics_snapshot(&metrics, 5100, &snapshot));
    failures += expect_true(
        "reset cleared accumulators",
        snapshot.round_id == 42 &&
            snapshot.latency[NUWAX_CONVERSATION_LATENCY_DSP_PROCESS].count == 0 &&
            snapshot.events[NUWAX_CONVERSATION_EVENT_SEQUENCE_GAP] == 0 &&
            snapshot.rejected_timing_samples == 0 &&
            snapshot.aec.current_state == NUWAX_AEC_RUNTIME_UNKNOWN &&
            snapshot.aec.duration_us[NUWAX_AEC_RUNTIME_UNKNOWN] == 100);

    printf("conversation metrics host check: %s (%d failures)\n",
           failures == 0 ? "PASS" : "FAIL",
           failures);
    return failures == 0 ? 0 : 1;
}
