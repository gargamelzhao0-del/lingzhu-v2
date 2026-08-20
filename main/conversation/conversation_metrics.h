#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The implementation is opaque so host checks do not need ESP-IDF headers.
 * Keep the object in static storage; it must not be copied after init. */
#define NUWAX_CONVERSATION_METRICS_STORAGE_BYTES 6144U

typedef union {
    uint64_t alignment;
    uint8_t bytes[NUWAX_CONVERSATION_METRICS_STORAGE_BYTES];
} nuwax_conversation_metrics_t;

typedef struct {
    uint64_t at_us;
    bool valid;
} nuwax_conversation_timepoint_t;

typedef enum {
    NUWAX_CONVERSATION_LATENCY_DSP_PROCESS = 0,
    NUWAX_CONVERSATION_LATENCY_DSP_AEC,
    NUWAX_CONVERSATION_LATENCY_DSP_HUMAN_DETECTOR,
    NUWAX_CONVERSATION_LATENCY_DSP_CLEANER,
    NUWAX_CONVERSATION_LATENCY_DSP_PREROLL,
    NUWAX_CONVERSATION_LATENCY_DSP_ENCODE,
    NUWAX_CONVERSATION_LATENCY_DSP_ENQUEUE,
    NUWAX_CONVERSATION_LATENCY_CAPTURE_TO_UPLINK_ENQUEUE,
    NUWAX_CONVERSATION_LATENCY_UPLINK_QUEUE,
    NUWAX_CONVERSATION_LATENCY_UPLINK_SEND,
    NUWAX_CONVERSATION_LATENCY_CAPTURE_TO_SOCKET_SEND,
    NUWAX_CONVERSATION_LATENCY_DOWNLINK_PARSE,
    NUWAX_CONVERSATION_LATENCY_SPEAKER_QUEUE,
    NUWAX_CONVERSATION_LATENCY_DOWNLINK_TO_I2S,
    NUWAX_CONVERSATION_LATENCY_BARGE_ONSET_TO_CANDIDATE,
    NUWAX_CONVERSATION_LATENCY_BARGE_CANDIDATE_TO_CONFIRM,
    NUWAX_CONVERSATION_LATENCY_BARGE_CONFIRM_TO_LOCAL_STOP,
    NUWAX_CONVERSATION_LATENCY_BARGE_ONSET_TO_LOCAL_STOP,
    NUWAX_CONVERSATION_LATENCY_COUNT,
} nuwax_conversation_latency_id_t;

typedef enum {
    NUWAX_CONVERSATION_QUEUE_CAPTURE = 0,
    NUWAX_CONVERSATION_QUEUE_UPLINK,
    NUWAX_CONVERSATION_QUEUE_DOWNLINK,
    NUWAX_CONVERSATION_QUEUE_SPEAKER,
    NUWAX_CONVERSATION_QUEUE_COUNT,
} nuwax_conversation_queue_id_t;

typedef enum {
    NUWAX_CONVERSATION_EVENT_CAPTURE_QUEUE_FULL = 0,
    NUWAX_CONVERSATION_EVENT_STALE_CAPTURE_FRAME,
    NUWAX_CONVERSATION_EVENT_UPLINK_QUEUE_FULL,
    NUWAX_CONVERSATION_EVENT_STALE_UPLINK_FRAME,
    NUWAX_CONVERSATION_EVENT_STALE_DOWNLINK_FRAME,
    NUWAX_CONVERSATION_EVENT_SEQUENCE_GAP,
    NUWAX_CONVERSATION_EVENT_SEQUENCE_DUPLICATE,
    NUWAX_CONVERSATION_EVENT_SEQUENCE_OUT_OF_ORDER,
    NUWAX_CONVERSATION_EVENT_SPEAKER_UNDERRUN,
    NUWAX_CONVERSATION_EVENT_AEC_FALLBACK,
    NUWAX_CONVERSATION_EVENT_COUNT,
} nuwax_conversation_event_id_t;

typedef enum {
    NUWAX_AEC_RUNTIME_UNKNOWN = 0,
    NUWAX_AEC_RUNTIME_DISABLED,
    NUWAX_AEC_RUNTIME_INITIALIZING,
    NUWAX_AEC_RUNTIME_HEALTHY,
    NUWAX_AEC_RUNTIME_DEGRADED,
    NUWAX_AEC_RUNTIME_BYPASSED,
    NUWAX_AEC_RUNTIME_FAILED,
    NUWAX_AEC_RUNTIME_STATE_COUNT,
} nuwax_aec_runtime_state_t;

typedef uint32_t nuwax_aec_runtime_reason_bits_t;

enum {
    NUWAX_AEC_REASON_NONE = 0U,
    NUWAX_AEC_REASON_CONFIG_DISABLED = 1U << 0,
    NUWAX_AEC_REASON_NOT_PREPARED = 1U << 1,
    NUWAX_AEC_REASON_ALLOCATION_FAILED = 1U << 2,
    NUWAX_AEC_REASON_LOW_INTERNAL_MEMORY = 1U << 3,
    NUWAX_AEC_REASON_REFERENCE_MISSING = 1U << 4,
    NUWAX_AEC_REASON_REFERENCE_INACTIVE = 1U << 5,
    NUWAX_AEC_REASON_REFERENCE_GAP = 1U << 6,
    NUWAX_AEC_REASON_REFERENCE_GENERATION_MISMATCH = 1U << 7,
    NUWAX_AEC_REASON_DELAY_UNLOCKED = 1U << 8,
    NUWAX_AEC_REASON_LOW_CORRELATION = 1U << 9,
    NUWAX_AEC_REASON_MIC_CLIPPING = 1U << 10,
    NUWAX_AEC_REASON_REFERENCE_CLIPPING = 1U << 11,
    NUWAX_AEC_REASON_OUTPUT_UNDERRUN = 1U << 12,
    NUWAX_AEC_REASON_DSP_DEADLINE_MISS = 1U << 13,
    NUWAX_AEC_REASON_PROCESSING_ERROR = 1U << 14,
    NUWAX_AEC_REASON_RAW_FALLBACK = 1U << 15,
    NUWAX_AEC_REASON_UNSUPPORTED_ROUTE = 1U << 16,
};

typedef struct {
    uint64_t count;
    uint64_t min_us;
    uint64_t max_us;
    /* Percentiles are conservative histogram bucket upper bounds. */
    uint64_t p50_us;
    uint64_t p95_us;
    uint64_t p99_us;
} nuwax_conversation_latency_summary_t;

typedef struct {
    uint32_t observations;
    uint32_t last_depth;
    uint32_t high_watermark;
    uint32_t last_capacity;
    uint32_t over_capacity_observations;
} nuwax_conversation_queue_summary_t;

typedef struct {
    nuwax_aec_runtime_state_t current_state;
    nuwax_aec_runtime_state_t worst_state;
    nuwax_aec_runtime_reason_bits_t current_reasons;
    nuwax_aec_runtime_reason_bits_t observed_reasons;
    uint32_t transitions;
    uint32_t entries[NUWAX_AEC_RUNTIME_STATE_COUNT];
    uint64_t duration_us[NUWAX_AEC_RUNTIME_STATE_COUNT];
} nuwax_aec_runtime_summary_t;

typedef struct {
    uint64_t round_id;
    uint64_t round_started_us;
    uint64_t snapshot_us;
    uint64_t round_elapsed_us;
    uint32_t rejected_timing_samples;
    uint32_t histogram_saturation_events;
    nuwax_conversation_latency_summary_t latency[NUWAX_CONVERSATION_LATENCY_COUNT];
    nuwax_conversation_queue_summary_t queues[NUWAX_CONVERSATION_QUEUE_COUNT];
    uint64_t events[NUWAX_CONVERSATION_EVENT_COUNT];
    nuwax_aec_runtime_summary_t aec;
} nuwax_conversation_metrics_snapshot_t;

/* All mutation APIs are task-context safe on ESP-IDF and use no heap. They
 * must not be called from an ISR. Timestamp arguments must use one monotonic
 * microsecond clock (esp_timer_get_time on the device) for the whole round. */
bool nuwax_conversation_metrics_init(nuwax_conversation_metrics_t *metrics,
                                     uint64_t round_id,
                                     uint64_t started_us);
bool nuwax_conversation_metrics_reset(nuwax_conversation_metrics_t *metrics,
                                      uint64_t round_id,
                                      uint64_t started_us);

void nuwax_conversation_timepoint_clear(nuwax_conversation_timepoint_t *timepoint);
bool nuwax_conversation_timepoint_mark(nuwax_conversation_timepoint_t *timepoint,
                                       uint64_t now_us);

bool nuwax_conversation_metrics_record_duration(
    nuwax_conversation_metrics_t *metrics,
    nuwax_conversation_latency_id_t latency_id,
    uint64_t duration_us);
bool nuwax_conversation_metrics_record_since(
    nuwax_conversation_metrics_t *metrics,
    nuwax_conversation_latency_id_t latency_id,
    const nuwax_conversation_timepoint_t *started,
    uint64_t completed_us);

bool nuwax_conversation_metrics_observe_queue(
    nuwax_conversation_metrics_t *metrics,
    nuwax_conversation_queue_id_t queue_id,
    uint32_t depth,
    uint32_t capacity);
bool nuwax_conversation_metrics_count_event(
    nuwax_conversation_metrics_t *metrics,
    nuwax_conversation_event_id_t event_id,
    uint32_t increment);

bool nuwax_conversation_metrics_set_aec_runtime(
    nuwax_conversation_metrics_t *metrics,
    nuwax_aec_runtime_state_t state,
    nuwax_aec_runtime_reason_bits_t reasons,
    uint64_t now_us);

bool nuwax_conversation_metrics_snapshot(
    nuwax_conversation_metrics_t *metrics,
    uint64_t now_us,
    nuwax_conversation_metrics_snapshot_t *snapshot);

const char *nuwax_conversation_latency_name(nuwax_conversation_latency_id_t latency_id);
const char *nuwax_conversation_queue_name(nuwax_conversation_queue_id_t queue_id);
const char *nuwax_conversation_event_name(nuwax_conversation_event_id_t event_id);
const char *nuwax_aec_runtime_state_name(nuwax_aec_runtime_state_t state);

#ifdef __cplusplus
}
#endif
