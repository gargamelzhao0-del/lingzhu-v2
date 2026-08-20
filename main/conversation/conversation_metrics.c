#include "conversation_metrics.h"

#include <limits.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#else
#include <stdatomic.h>
#endif

#define NUWAX_CONVERSATION_METRICS_MAGIC 0x434D4554U
#define NUWAX_CONVERSATION_HISTOGRAM_BUCKETS 64U

typedef struct {
    uint64_t count;
    uint64_t min_us;
    uint64_t max_us;
    uint32_t buckets[NUWAX_CONVERSATION_HISTOGRAM_BUCKETS];
} latency_accumulator_t;

typedef struct {
    uint32_t observations;
    uint32_t last_depth;
    uint32_t high_watermark;
    uint32_t last_capacity;
    uint32_t over_capacity_observations;
} queue_accumulator_t;

typedef struct {
    uint64_t round_id;
    uint64_t round_started_us;
    uint32_t rejected_timing_samples;
    uint32_t histogram_saturation_events;
    latency_accumulator_t latency[NUWAX_CONVERSATION_LATENCY_COUNT];
    queue_accumulator_t queues[NUWAX_CONVERSATION_QUEUE_COUNT];
    uint64_t events[NUWAX_CONVERSATION_EVENT_COUNT];
    nuwax_aec_runtime_state_t aec_current_state;
    nuwax_aec_runtime_state_t aec_worst_state;
    nuwax_aec_runtime_reason_bits_t aec_current_reasons;
    nuwax_aec_runtime_reason_bits_t aec_observed_reasons;
    uint64_t aec_state_since_us;
    uint32_t aec_transitions;
    uint32_t aec_entries[NUWAX_AEC_RUNTIME_STATE_COUNT];
    uint64_t aec_duration_us[NUWAX_AEC_RUNTIME_STATE_COUNT];
} metrics_data_t;

#ifdef ESP_PLATFORM
typedef struct {
    StaticSemaphore_t storage;
    SemaphoreHandle_t handle;
} metrics_lock_t;
#else
typedef struct {
    atomic_flag flag;
} metrics_lock_t;
#endif

typedef struct {
    metrics_lock_t lock;
    uint32_t magic;
    metrics_data_t data;
} metrics_impl_t;

_Static_assert(sizeof(metrics_impl_t) <= NUWAX_CONVERSATION_METRICS_STORAGE_BYTES,
               "conversation metrics opaque storage is too small");
_Static_assert(_Alignof(nuwax_conversation_metrics_t) >= _Alignof(metrics_impl_t),
               "conversation metrics opaque storage is under-aligned");

static const uint64_t s_histogram_upper_us[NUWAX_CONVERSATION_HISTOGRAM_BUCKETS] = {
    50ULL, 100ULL, 150ULL, 200ULL, 300ULL, 400ULL, 500ULL, 750ULL,
    1000ULL, 1500ULL, 2000ULL, 3000ULL, 4000ULL, 5000ULL, 7500ULL, 10000ULL,
    12500ULL, 15000ULL, 17500ULL, 20000ULL, 22500ULL, 25000ULL, 27500ULL,
    30000ULL, 35000ULL, 40000ULL, 45000ULL, 50000ULL, 60000ULL, 70000ULL,
    80000ULL, 90000ULL, 100000ULL, 120000ULL, 140000ULL, 160000ULL,
    180000ULL, 200000ULL, 220000ULL, 240000ULL, 260000ULL, 280000ULL,
    300000ULL, 350000ULL, 400000ULL, 450000ULL, 500000ULL, 600000ULL,
    700000ULL, 800000ULL, 900000ULL, 1000000ULL, 1200000ULL, 1500000ULL,
    2000000ULL, 3000000ULL, 5000000ULL, 7500000ULL, 10000000ULL,
    15000000ULL, 30000000ULL, 60000000ULL, UINT64_MAX,
};

static const char *const s_latency_names[NUWAX_CONVERSATION_LATENCY_COUNT] = {
    "dsp_process",
    "dsp_aec",
    "dsp_human_detector",
    "dsp_cleaner",
    "dsp_preroll",
    "dsp_encode",
    "dsp_enqueue",
    "capture_to_uplink_enqueue",
    "uplink_queue",
    "uplink_send",
    "capture_to_socket_send",
    "downlink_parse",
    "speaker_queue",
    "downlink_to_i2s",
    "barge_onset_to_candidate",
    "barge_candidate_to_confirm",
    "barge_confirm_to_local_stop",
    "barge_onset_to_local_stop",
};

static const char *const s_queue_names[NUWAX_CONVERSATION_QUEUE_COUNT] = {
    "capture", "uplink", "downlink", "speaker",
};

static const char *const s_event_names[NUWAX_CONVERSATION_EVENT_COUNT] = {
    "capture_queue_full",
    "stale_capture_frame",
    "uplink_queue_full",
    "stale_uplink_frame",
    "stale_downlink_frame",
    "sequence_gap",
    "sequence_duplicate",
    "sequence_out_of_order",
    "speaker_underrun",
    "aec_fallback",
};

static const char *const s_aec_state_names[NUWAX_AEC_RUNTIME_STATE_COUNT] = {
    "unknown", "disabled", "initializing", "healthy", "degraded", "bypassed", "failed",
};

static metrics_impl_t *metrics_impl(nuwax_conversation_metrics_t *metrics) {
    return (metrics_impl_t *)(void *)metrics;
}

static bool metrics_is_initialized(const metrics_impl_t *impl) {
    return impl && impl->magic == NUWAX_CONVERSATION_METRICS_MAGIC;
}

static bool lock_init(metrics_lock_t *lock) {
#ifdef ESP_PLATFORM
    lock->handle = xSemaphoreCreateMutexStatic(&lock->storage);
    return lock->handle != NULL;
#else
    atomic_flag_clear_explicit(&lock->flag, memory_order_release);
    return true;
#endif
}

static void lock_take(metrics_lock_t *lock) {
#ifdef ESP_PLATFORM
    (void)xSemaphoreTake(lock->handle, portMAX_DELAY);
#else
    while (atomic_flag_test_and_set_explicit(&lock->flag, memory_order_acquire)) {
    }
#endif
}

static void lock_give(metrics_lock_t *lock) {
#ifdef ESP_PLATFORM
    (void)xSemaphoreGive(lock->handle);
#else
    atomic_flag_clear_explicit(&lock->flag, memory_order_release);
#endif
}

static void increment_u32_saturating(uint32_t *value) {
    if (*value != UINT32_MAX) {
        (*value)++;
    }
}

static void add_u64_saturating(uint64_t *value, uint64_t increment) {
    if (UINT64_MAX - *value < increment) {
        *value = UINT64_MAX;
    } else {
        *value += increment;
    }
}

static uint32_t histogram_bucket(uint64_t duration_us) {
    uint32_t low = 0;
    uint32_t high = NUWAX_CONVERSATION_HISTOGRAM_BUCKETS - 1U;
    while (low < high) {
        const uint32_t middle = low + (high - low) / 2U;
        if (duration_us <= s_histogram_upper_us[middle]) {
            high = middle;
        } else {
            low = middle + 1U;
        }
    }
    return low;
}

static uint64_t percentile_upper_bound(const latency_accumulator_t *accumulator,
                                       uint32_t percentile) {
    uint64_t histogram_count = 0;
    for (uint32_t i = 0; i < NUWAX_CONVERSATION_HISTOGRAM_BUCKETS; i++) {
        histogram_count += accumulator->buckets[i];
    }
    if (histogram_count == 0) {
        return 0;
    }

    const uint32_t complement = 100U - percentile;
    const uint64_t target = histogram_count - (histogram_count * complement) / 100U;
    uint64_t cumulative = 0;
    for (uint32_t i = 0; i < NUWAX_CONVERSATION_HISTOGRAM_BUCKETS; i++) {
        cumulative += accumulator->buckets[i];
        if (cumulative >= target) {
            return s_histogram_upper_us[i] == UINT64_MAX
                       ? accumulator->max_us
                       : s_histogram_upper_us[i];
        }
    }
    return accumulator->max_us;
}

static uint32_t aec_state_severity(nuwax_aec_runtime_state_t state) {
    switch (state) {
        case NUWAX_AEC_RUNTIME_FAILED:
            return 6;
        case NUWAX_AEC_RUNTIME_BYPASSED:
            return 5;
        case NUWAX_AEC_RUNTIME_DEGRADED:
            return 4;
        case NUWAX_AEC_RUNTIME_DISABLED:
            return 3;
        case NUWAX_AEC_RUNTIME_INITIALIZING:
            return 0;
        case NUWAX_AEC_RUNTIME_HEALTHY:
            return 1;
        case NUWAX_AEC_RUNTIME_UNKNOWN:
        default:
            return 0;
    }
}

static void reset_data(metrics_data_t *data, uint64_t round_id, uint64_t started_us) {
    memset(data, 0, sizeof(*data));
    data->round_id = round_id;
    data->round_started_us = started_us;
    data->aec_current_state = NUWAX_AEC_RUNTIME_UNKNOWN;
    data->aec_worst_state = NUWAX_AEC_RUNTIME_UNKNOWN;
    data->aec_state_since_us = started_us;
    data->aec_entries[NUWAX_AEC_RUNTIME_UNKNOWN] = 1;
    for (uint32_t i = 0; i < NUWAX_CONVERSATION_LATENCY_COUNT; i++) {
        data->latency[i].min_us = UINT64_MAX;
    }
}

bool nuwax_conversation_metrics_init(nuwax_conversation_metrics_t *metrics,
                                     uint64_t round_id,
                                     uint64_t started_us) {
    if (!metrics) {
        return false;
    }
    memset(metrics, 0, sizeof(*metrics));
    metrics_impl_t *impl = metrics_impl(metrics);
    if (!lock_init(&impl->lock)) {
        return false;
    }
    reset_data(&impl->data, round_id, started_us);
    impl->magic = NUWAX_CONVERSATION_METRICS_MAGIC;
    return true;
}

bool nuwax_conversation_metrics_reset(nuwax_conversation_metrics_t *metrics,
                                      uint64_t round_id,
                                      uint64_t started_us) {
    metrics_impl_t *impl = metrics_impl(metrics);
    if (!metrics_is_initialized(impl)) {
        return false;
    }
    lock_take(&impl->lock);
    reset_data(&impl->data, round_id, started_us);
    lock_give(&impl->lock);
    return true;
}

void nuwax_conversation_timepoint_clear(nuwax_conversation_timepoint_t *timepoint) {
    if (timepoint) {
        *timepoint = (nuwax_conversation_timepoint_t){0};
    }
}

bool nuwax_conversation_timepoint_mark(nuwax_conversation_timepoint_t *timepoint,
                                       uint64_t now_us) {
    if (!timepoint || (timepoint->valid && now_us < timepoint->at_us)) {
        return false;
    }
    timepoint->at_us = now_us;
    timepoint->valid = true;
    return true;
}

bool nuwax_conversation_metrics_record_duration(
    nuwax_conversation_metrics_t *metrics,
    nuwax_conversation_latency_id_t latency_id,
    uint64_t duration_us) {
    metrics_impl_t *impl = metrics_impl(metrics);
    if (!metrics_is_initialized(impl) || latency_id < 0 ||
        latency_id >= NUWAX_CONVERSATION_LATENCY_COUNT) {
        return false;
    }

    lock_take(&impl->lock);
    latency_accumulator_t *accumulator = &impl->data.latency[latency_id];
    if (accumulator->count != UINT64_MAX) {
        accumulator->count++;
    }
    if (duration_us < accumulator->min_us) {
        accumulator->min_us = duration_us;
    }
    if (duration_us > accumulator->max_us) {
        accumulator->max_us = duration_us;
    }
    const uint32_t bucket = histogram_bucket(duration_us);
    if (accumulator->buckets[bucket] == UINT32_MAX) {
        increment_u32_saturating(&impl->data.histogram_saturation_events);
    } else {
        accumulator->buckets[bucket]++;
    }
    lock_give(&impl->lock);
    return true;
}

bool nuwax_conversation_metrics_record_since(
    nuwax_conversation_metrics_t *metrics,
    nuwax_conversation_latency_id_t latency_id,
    const nuwax_conversation_timepoint_t *started,
    uint64_t completed_us) {
    metrics_impl_t *impl = metrics_impl(metrics);
    if (!metrics_is_initialized(impl) || latency_id < 0 ||
        latency_id >= NUWAX_CONVERSATION_LATENCY_COUNT) {
        return false;
    }
    if (!started || !started->valid || completed_us < started->at_us) {
        lock_take(&impl->lock);
        increment_u32_saturating(&impl->data.rejected_timing_samples);
        lock_give(&impl->lock);
        return false;
    }
    return nuwax_conversation_metrics_record_duration(
        metrics, latency_id, completed_us - started->at_us);
}

bool nuwax_conversation_metrics_observe_queue(
    nuwax_conversation_metrics_t *metrics,
    nuwax_conversation_queue_id_t queue_id,
    uint32_t depth,
    uint32_t capacity) {
    metrics_impl_t *impl = metrics_impl(metrics);
    if (!metrics_is_initialized(impl) || queue_id < 0 ||
        queue_id >= NUWAX_CONVERSATION_QUEUE_COUNT) {
        return false;
    }
    lock_take(&impl->lock);
    queue_accumulator_t *queue = &impl->data.queues[queue_id];
    increment_u32_saturating(&queue->observations);
    queue->last_depth = depth;
    queue->last_capacity = capacity;
    if (depth > queue->high_watermark) {
        queue->high_watermark = depth;
    }
    if (capacity > 0 && depth > capacity) {
        increment_u32_saturating(&queue->over_capacity_observations);
    }
    lock_give(&impl->lock);
    return true;
}

bool nuwax_conversation_metrics_count_event(
    nuwax_conversation_metrics_t *metrics,
    nuwax_conversation_event_id_t event_id,
    uint32_t increment) {
    metrics_impl_t *impl = metrics_impl(metrics);
    if (!metrics_is_initialized(impl) || event_id < 0 ||
        event_id >= NUWAX_CONVERSATION_EVENT_COUNT) {
        return false;
    }
    lock_take(&impl->lock);
    add_u64_saturating(&impl->data.events[event_id], increment);
    lock_give(&impl->lock);
    return true;
}

bool nuwax_conversation_metrics_set_aec_runtime(
    nuwax_conversation_metrics_t *metrics,
    nuwax_aec_runtime_state_t state,
    nuwax_aec_runtime_reason_bits_t reasons,
    uint64_t now_us) {
    metrics_impl_t *impl = metrics_impl(metrics);
    if (!metrics_is_initialized(impl) || state < 0 ||
        state >= NUWAX_AEC_RUNTIME_STATE_COUNT) {
        return false;
    }

    lock_take(&impl->lock);
    metrics_data_t *data = &impl->data;
    if (now_us < data->aec_state_since_us) {
        increment_u32_saturating(&data->rejected_timing_samples);
        lock_give(&impl->lock);
        return false;
    }
    data->aec_observed_reasons |= reasons;
    if (state != data->aec_current_state) {
        add_u64_saturating(&data->aec_duration_us[data->aec_current_state],
                           now_us - data->aec_state_since_us);
        data->aec_current_state = state;
        data->aec_state_since_us = now_us;
        increment_u32_saturating(&data->aec_transitions);
        increment_u32_saturating(&data->aec_entries[state]);
        if (aec_state_severity(state) > aec_state_severity(data->aec_worst_state)) {
            data->aec_worst_state = state;
        }
    }
    data->aec_current_reasons = reasons;
    lock_give(&impl->lock);
    return true;
}

bool nuwax_conversation_metrics_snapshot(
    nuwax_conversation_metrics_t *metrics,
    uint64_t now_us,
    nuwax_conversation_metrics_snapshot_t *snapshot) {
    metrics_impl_t *impl = metrics_impl(metrics);
    if (!metrics_is_initialized(impl) || !snapshot) {
        return false;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    lock_take(&impl->lock);
    const metrics_data_t *data = &impl->data;
    if (now_us < data->round_started_us || now_us < data->aec_state_since_us) {
        increment_u32_saturating(&impl->data.rejected_timing_samples);
        lock_give(&impl->lock);
        return false;
    }

    snapshot->round_id = data->round_id;
    snapshot->round_started_us = data->round_started_us;
    snapshot->snapshot_us = now_us;
    snapshot->round_elapsed_us = now_us - data->round_started_us;
    snapshot->rejected_timing_samples = data->rejected_timing_samples;
    snapshot->histogram_saturation_events = data->histogram_saturation_events;

    for (uint32_t i = 0; i < NUWAX_CONVERSATION_LATENCY_COUNT; i++) {
        const latency_accumulator_t *source = &data->latency[i];
        nuwax_conversation_latency_summary_t *target = &snapshot->latency[i];
        target->count = source->count;
        if (source->count > 0) {
            target->min_us = source->min_us;
            target->max_us = source->max_us;
            target->p50_us = percentile_upper_bound(source, 50);
            target->p95_us = percentile_upper_bound(source, 95);
            target->p99_us = percentile_upper_bound(source, 99);
        }
    }
    for (uint32_t i = 0; i < NUWAX_CONVERSATION_QUEUE_COUNT; i++) {
        snapshot->queues[i].observations = data->queues[i].observations;
        snapshot->queues[i].last_depth = data->queues[i].last_depth;
        snapshot->queues[i].high_watermark = data->queues[i].high_watermark;
        snapshot->queues[i].last_capacity = data->queues[i].last_capacity;
        snapshot->queues[i].over_capacity_observations =
            data->queues[i].over_capacity_observations;
    }
    memcpy(snapshot->events, data->events, sizeof(snapshot->events));

    snapshot->aec.current_state = data->aec_current_state;
    snapshot->aec.worst_state = data->aec_worst_state;
    snapshot->aec.current_reasons = data->aec_current_reasons;
    snapshot->aec.observed_reasons = data->aec_observed_reasons;
    snapshot->aec.transitions = data->aec_transitions;
    memcpy(snapshot->aec.entries, data->aec_entries, sizeof(snapshot->aec.entries));
    memcpy(snapshot->aec.duration_us,
           data->aec_duration_us,
           sizeof(snapshot->aec.duration_us));
    add_u64_saturating(
        &snapshot->aec.duration_us[data->aec_current_state],
        now_us - data->aec_state_since_us);
    lock_give(&impl->lock);
    return true;
}

const char *nuwax_conversation_latency_name(nuwax_conversation_latency_id_t latency_id) {
    return latency_id >= 0 && latency_id < NUWAX_CONVERSATION_LATENCY_COUNT
               ? s_latency_names[latency_id]
               : "invalid";
}

const char *nuwax_conversation_queue_name(nuwax_conversation_queue_id_t queue_id) {
    return queue_id >= 0 && queue_id < NUWAX_CONVERSATION_QUEUE_COUNT
               ? s_queue_names[queue_id]
               : "invalid";
}

const char *nuwax_conversation_event_name(nuwax_conversation_event_id_t event_id) {
    return event_id >= 0 && event_id < NUWAX_CONVERSATION_EVENT_COUNT
               ? s_event_names[event_id]
               : "invalid";
}

const char *nuwax_aec_runtime_state_name(nuwax_aec_runtime_state_t state) {
    return state >= 0 && state < NUWAX_AEC_RUNTIME_STATE_COUNT
               ? s_aec_state_names[state]
               : "invalid";
}
