#include "speaker_playback.h"

#include <stddef.h>
#include <stdatomic.h>
#include <string.h>

#include "audio_frame.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "p3_software_ref.h"
#include "speaker_backpressure_policy.h"
#include "speaker_generation_policy.h"
#include "speaker_jitter_policy.h"
#include "speaker_output.h"
#include "speaker_stream_policy.h"

static const char *TAG = "NuwaxPartner6";

#ifndef CONFIG_NUWAX_SPEAKER_DEFAULT_VOLUME_PERCENT
#define CONFIG_NUWAX_SPEAKER_DEFAULT_VOLUME_PERCENT 80
#endif

#define NUWAX_SPEAKER_CANCEL_DROP_GRACE_MS 1500
#define NUWAX_SPEAKER_CANCEL_DROP_ACTIVE_MS 30000
#define NUWAX_SPEAKER_FADE_CANCEL_DEFAULT_MS 320
#define NUWAX_SPEAKER_FADE_CANCEL_STEP_MS 40
#ifndef CONFIG_NUWAX_SPEAKER_BUFFER_BYTES
#define CONFIG_NUWAX_SPEAKER_BUFFER_BYTES (384U * 1024U)
#endif
#define NUWAX_SPEAKER_BUFFER_BYTES \
    ((size_t)CONFIG_NUWAX_SPEAKER_BUFFER_BYTES)
#ifndef CONFIG_NUWAX_SPEAKER_BACKPRESSURE_POLL_MS
#define CONFIG_NUWAX_SPEAKER_BACKPRESSURE_POLL_MS 20U
#endif
#define NUWAX_SPEAKER_BACKPRESSURE_POLL_MS \
    ((uint32_t)CONFIG_NUWAX_SPEAKER_BACKPRESSURE_POLL_MS)
#define NUWAX_SPEAKER_BACKPRESSURE_LOG_MS 1000U
#define NUWAX_SPEAKER_RX_ADMISSION_BUDGET_MS 20U
#ifndef CONFIG_NUWAX_SPEAKER_START_GATE_TIMEOUT_MS
#define CONFIG_NUWAX_SPEAKER_START_GATE_TIMEOUT_MS 2000U
#endif
#define NUWAX_SPEAKER_START_GATE_TIMEOUT_MS \
    ((uint32_t)CONFIG_NUWAX_SPEAKER_START_GATE_TIMEOUT_MS)
#define NUWAX_SPEAKER_JITTER_STARTUP_MIN_MS 480U
#define NUWAX_SPEAKER_JITTER_STARTUP_BASE_MS 800U
#define NUWAX_SPEAKER_JITTER_STARTUP_MAX_MS 1600U
#define NUWAX_SPEAKER_JITTER_RESUME_MIN_MS 800U
#define NUWAX_SPEAKER_JITTER_RESUME_MAX_MS 1200U
#define NUWAX_SPEAKER_JITTER_RESUME_GAP_CUSHION_MS 240U
#define NUWAX_SPEAKER_JITTER_LEARNING_IDLE_RESET_MS 120000U
/* The shared TX DMA ring carries about 90 ms. Let it bridge short scheduler
 * gaps before declaring a user-visible rebuffer. */
#define NUWAX_SPEAKER_JITTER_EMPTY_GRACE_MS 80U
#define NUWAX_SPEAKER_JITTER_POLL_MS 20U
#define NUWAX_SPEAKER_REBUFFER_FADE_IN_MS 5U
#define NUWAX_SPEAKER_NATURAL_DRAIN_TIMEOUT_MS 300U
#ifndef CONFIG_NUWAX_SPEAKER_STREAM_STALL_WARN_MS
#define CONFIG_NUWAX_SPEAKER_STREAM_STALL_WARN_MS 12000U
#endif
#ifndef CONFIG_NUWAX_SPEAKER_STREAM_STALL_TIMEOUT_MS
#define CONFIG_NUWAX_SPEAKER_STREAM_STALL_TIMEOUT_MS 60000U
#endif
#define NUWAX_SPEAKER_STREAM_STALL_WARN_MS \
    ((uint32_t)CONFIG_NUWAX_SPEAKER_STREAM_STALL_WARN_MS)
#define NUWAX_SPEAKER_STREAM_STALL_TIMEOUT_MS \
    ((uint32_t)CONFIG_NUWAX_SPEAKER_STREAM_STALL_TIMEOUT_MS)
_Static_assert(NUWAX_SPEAKER_STREAM_STALL_WARN_MS <
                   NUWAX_SPEAKER_STREAM_STALL_TIMEOUT_MS,
               "speaker stall warning must precede hard timeout");
#define NUWAX_SPEAKER_BACKPRESSURE_TIMEOUT_MS \
    NUWAX_SPEAKER_RX_ADMISSION_BUDGET_MS

_Static_assert(NUWAX_SPEAKER_BACKPRESSURE_TIMEOUT_MS <= 40U,
               "speaker admission must not stall WebSocket RX");
#define NUWAX_SPEAKER_PROGRESS_CALLBACK_INTERVAL_MS 500U
#define NUWAX_SPEAKER_INGRESS_CALLBACK_INTERVAL_MS 250U
#define NUWAX_SPEAKER_TASK_STACK_WARN_BYTES 2048U
#define NUWAX_SPEAKER_ALLOWED_AUDIO_FLAGS \
    (NUWAX_AUDIO_FLAG_START | NUWAX_AUDIO_FLAG_END | \
     NUWAX_AUDIO_FLAG_VAD_SPEECH)
#define NUWAX_SPEAKER_CLAIM_WAIT_MARGIN_MS 250U
#define NUWAX_SPEAKER_CLAIM_WAIT_TIMEOUT_MS \
    ((uint32_t)CONFIG_NUWAX_SPEAKER_WRITE_TIMEOUT_MS + \
     NUWAX_SPEAKER_CLAIM_WAIT_MARGIN_MS)
#define NUWAX_SPEAKER_RUNTIME_RESET_MAX_STOP_RETRIES 2U
#define NUWAX_SPEAKER_RUNTIME_RESET_TIMEOUT_MS \
    (NUWAX_SPEAKER_CLAIM_WAIT_TIMEOUT_MS * 4U)
#define NUWAX_SPEAKER_STATE_LOCK_TIMEOUT_MS 5000U
#define NUWAX_SPEAKER_PROBE_MIN_ATTENUATION_DB 12U
#define NUWAX_SPEAKER_PROBE_MAX_ATTENUATION_DB 18U
#define NUWAX_SPEAKER_PROBE_MIN_DURATION_MS 20U
#define NUWAX_SPEAKER_PROBE_MAX_DURATION_MS 200U
#define NUWAX_SETTINGS_NVS_NAMESPACE "nuwax"
#define NUWAX_SETTINGS_NVS_KEY_VOLUME "spk_vol"

#if CONFIG_NUWAX_SPEAKER_ENABLE
static TickType_t speaker_positive_delay_ticks(uint32_t delay_ms) {
    const TickType_t ticks = pdMS_TO_TICKS(delay_ms);
    return ticks == 0 ? 1 : ticks;
}

typedef struct {
    uint32_t connection_epoch;
    uint32_t generation;
    uint32_t sequence;
    uint16_t sample_rate;
    uint8_t channels;
    uint8_t flags;
    size_t sample_count;
    int64_t start_handler_entry_us;
    int64_t enqueue_us;
    int16_t samples[CONFIG_NUWAX_VOICE_MAX_SAMPLES_PER_FRAME];
} speaker_playback_packet_t;

typedef struct {
    uint32_t connection_epoch;
    uint32_t generation;
} speaker_generation_ref_t;

typedef struct {
    nuwax_speaker_output_t output;
    RingbufHandle_t packets;
    SemaphoreHandle_t mutex;
    nuwax_speaker_playback_callbacks_t callbacks;
    bool drop_until_end;
    bool downlink_admission_allowed;
    bool playback_active;
    bool playback_session_active;
    bool keep_initialized;
    uint32_t drop_deadline_ms;
    uint32_t dropped_frames;
    uint32_t downlink_admission_epoch;
    uint32_t submitted_generation;
    uint32_t submitted_connection_epoch;
    uint32_t playback_generation;
    uint32_t playback_connection_epoch;
    uint32_t io_claim_generation;
    uint32_t completion_claim_generation;
    uint32_t maintenance_claim_generation;
    uint32_t maintenance_claim_sequence;
    uint32_t cancel_pending_generation;
    uint32_t cancel_stopped_generation;
    uint32_t cancel_stopped_connection_epoch;
    uint32_t cancel_request_generation;
    uint32_t cancel_request_connection_epoch;
    int64_t cancel_request_timestamp_us;
    uint32_t cancel_recovery_notified_generation;
    uint32_t cancel_recovery_notified_connection_epoch;
    uint32_t output_stop_owner_generation;
    uint32_t completed_stop_generation;
    uint32_t completed_stop_connection_epoch;
    bool completed_stop_cancel_requested;
    int64_t completed_stop_timestamp_us;
    esp_err_t completed_stop_result;
    bool stop_fault_active;
    uint32_t failed_stop_generation;
    uint32_t failed_stop_connection_epoch;
    int64_t failed_stop_timestamp_us;
    esp_err_t failed_stop_result;
    bool runtime_reset_in_progress;
    _Atomic uint32_t packet_count;
    _Atomic uint32_t queued_samples;
    TaskHandle_t playback_task;
    uint32_t queued_frames;
    uint32_t played_frames;
    uint32_t queue_dropped_frames;
    uint32_t write_failures;
    uint64_t played_bytes;
    uint32_t gap_silence_frames;
    size_t last_frame_samples;
    uint32_t playback_last_progress_ms;
    bool jitter_rebuffering;
    uint32_t jitter_rebuffer_started_ms;
    uint32_t jitter_rebuffer_events;
    uint32_t jitter_rebuffer_wait_ms;
    uint32_t jitter_rebuffer_max_wait_ms;
    uint32_t jitter_empty_started_ms;
    uint32_t jitter_rebuffer_entry_pcm_frames;
    uint32_t jitter_tail_wait_events;
    uint32_t jitter_tail_wait_ms;
    uint32_t jitter_low_watermark_ms;
    uint32_t startup_buffer_wait_ms;
    uint32_t jitter_startup_target_ms;
    uint32_t jitter_resume_target_ms;
    uint8_t jitter_recent_instability_history;
    uint8_t jitter_stable_playbacks;
    uint32_t jitter_learning_connection_epoch;
    uint32_t jitter_last_playback_completed_ms;
    bool jitter_learning_reset_pending;
    bool jitter_resume_fade_pending;
    uint32_t ingress_last_pcm_rx_ms;
    uint32_t ingress_max_gap_ms;
    int64_t playback_begin_rx_us;
    int64_t first_pcm_rx_us;
    int64_t start_gate_authorized_us;
    int64_t startup_watermark_ready_us;
    int64_t first_i2s_write_us;
    uint32_t progress_callback_ms;
    uint32_t ingress_progress_callback_ms;
    uint32_t stream_stall_faults;
    uint32_t stream_protocol_faults;
    nuwax_speaker_stream_policy_t stream;
    uint32_t metadata_expected_frames;
    uint32_t metadata_expected_bytes;
    bool stream_protocol_faulted;
    uint32_t protocol_fault_pending_generation;
    uint32_t protocol_fault_pending_connection_epoch;
    uint32_t protocol_fault_notified_generation;
    uint32_t protocol_fault_notified_connection_epoch;
    uint32_t stale_playback_frames_dropped;
    uint32_t stale_playback_sessions_dropped;
    nuwax_speaker_playback_owner_t active_owner;
    char active_turn_id[48];
    char active_notify_id[48];
    char active_playback_id[48];
    char stale_turn_id[48];
    char stale_playback_id[48];
    uint8_t volume_percent;
    uint32_t volume_generation;
    /* The logical volume may temporarily differ from the hardware volume
     * during probe duck/fade cancellation. Keep a retry bit so a failed
     * setter cannot leave a later generation at the old attenuation. */
    bool output_volume_sync_pending;
    bool fade_cancel_active;
    /* The owner queue only needs the SPEAKING edge once per generation. */
    uint32_t speaking_state_generation;
    bool start_gate_pending;
    bool start_gate_authorized;
    uint32_t start_gate_generation;
    uint32_t start_gate_connection_epoch;
    uint32_t start_gate_deadline_ms;
    bool end_seen;
    uint32_t end_seen_generation;
    uint32_t end_seen_sequence;
    uint32_t fade_cancel_generation;
    uint32_t fade_cancel_total_samples;
    uint32_t fade_cancel_processed_samples;
    uint32_t fade_cancel_deadline_ms;
    char fade_cancel_source[40];
    bool probe_duck_active;
    uint32_t probe_duck_generation;
    uint32_t probe_duck_deadline_ms;
    uint8_t probe_duck_output_percent;
} speaker_ctx_t;

static speaker_ctx_t s_speaker_ctx = {
    .keep_initialized = true,
    .volume_percent = CONFIG_NUWAX_SPEAKER_DEFAULT_VOLUME_PERCENT,
    .volume_generation = 1U,
    .jitter_startup_target_ms = NUWAX_SPEAKER_JITTER_STARTUP_BASE_MS,
    .jitter_resume_target_ms = NUWAX_SPEAKER_JITTER_RESUME_MIN_MS,
};
static speaker_playback_packet_t *s_speaker_task_packet;
static speaker_playback_packet_t *s_speaker_task_rebuffer_ref;

static void speaker_release_task_buffers(void) {
    if (s_speaker_task_packet) {
        heap_caps_free(s_speaker_task_packet);
        s_speaker_task_packet = NULL;
    }
    if (s_speaker_task_rebuffer_ref) {
        heap_caps_free(s_speaker_task_rebuffer_ref);
        s_speaker_task_rebuffer_ref = NULL;
    }
}

static bool speaker_cancel_now_if_current(uint32_t expected_generation,
                                          const char *source);
static bool speaker_cancel_now_if_current_epoch(uint32_t expected_connection_epoch,
                                                uint32_t expected_generation,
                                                const char *source);

static uint32_t monotonic_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool elapsed_ms(uint32_t now_ms, uint32_t deadline_ms) {
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static void speaker_lock(void) {
    if (s_speaker_ctx.mutex &&
        xSemaphoreTake(
            s_speaker_ctx.mutex,
            speaker_positive_delay_ticks(
                NUWAX_SPEAKER_STATE_LOCK_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG,
                 "speaker state lock timed out after %u ms; controlled restart preserves output and task ownership",
                 (unsigned)NUWAX_SPEAKER_STATE_LOCK_TIMEOUT_MS);
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
        vTaskDelay(portMAX_DELAY);
    }
}

static void speaker_unlock(void) {
    if (s_speaker_ctx.mutex) {
        xSemaphoreGive(s_speaker_ctx.mutex);
    }
}

static size_t speaker_packet_storage_size(size_t sample_count) {
    return offsetof(speaker_playback_packet_t, samples) +
           sample_count * sizeof(int16_t);
}

static uint32_t speaker_packet_ring_count(void) {
    return atomic_load_explicit(&s_speaker_ctx.packet_count,
                                memory_order_acquire);
}

static uint32_t speaker_packet_ring_samples(void) {
    return atomic_load_explicit(&s_speaker_ctx.queued_samples,
                                memory_order_acquire);
}

static void speaker_atomic_saturating_sub(_Atomic uint32_t *value,
                                          uint32_t decrement) {
    uint32_t current = atomic_load_explicit(value, memory_order_relaxed);
    while (current > 0U) {
        const uint32_t next = current > decrement ? current - decrement : 0U;
        if (atomic_compare_exchange_weak_explicit(value,
                                                  &current,
                                                  next,
                                                  memory_order_release,
                                                  memory_order_relaxed)) {
            break;
        }
    }
}

static void speaker_packet_ring_note_removed(uint32_t sample_count) {
    uint32_t count = atomic_load_explicit(&s_speaker_ctx.packet_count,
                                          memory_order_relaxed);
    while (count > 0U &&
           !atomic_compare_exchange_weak_explicit(
               &s_speaker_ctx.packet_count,
               &count,
               count - 1U,
               memory_order_release,
               memory_order_relaxed)) {
    }
    speaker_atomic_saturating_sub(&s_speaker_ctx.queued_samples, sample_count);
}

/* The caller holds speaker_lock(), which prevents new records from being
 * published while cancellation/reset drains all records still owned by the
 * ring. A record already claimed by the consumer is generation-checked before
 * playback and is not included in the returned waiting count. */
static uint32_t speaker_packet_ring_clear_locked(void) {
    if (!s_speaker_ctx.packets) {
        return 0U;
    }

    const uint32_t waiting = speaker_packet_ring_count();
    if (vRingbufferReset(s_speaker_ctx.packets) == ESP_OK) {
        atomic_store_explicit(&s_speaker_ctx.packet_count,
                              0U,
                              memory_order_release);
        atomic_store_explicit(&s_speaker_ctx.queued_samples,
                              0U,
                              memory_order_release);
        return waiting;
    }

    uint32_t cleared = 0U;
    size_t item_size = 0U;
    void *item = NULL;
    while ((item = xRingbufferReceive(s_speaker_ctx.packets,
                                      &item_size,
                                      0)) != NULL) {
        const size_t header_size = offsetof(speaker_playback_packet_t, samples);
        const uint32_t stored_samples =
            item_size >= header_size && ((item_size - header_size) & 1U) == 0U
                ? (uint32_t)((item_size - header_size) / sizeof(int16_t))
                : 0U;
        speaker_packet_ring_note_removed(stored_samples);
        vRingbufferReturnItem(s_speaker_ctx.packets, item);
        cleared++;
    }
    return cleared;
}

static bool speaker_packet_ring_receive(speaker_playback_packet_t *packet,
                                        TickType_t wait_ticks) {
    if (!packet || !s_speaker_ctx.packets) {
        return false;
    }

    size_t item_size = 0U;
    void *item = xRingbufferReceive(s_speaker_ctx.packets,
                                    &item_size,
                                    wait_ticks);
    if (!item) {
        return false;
    }

    memset(packet, 0, sizeof(*packet));
    bool valid = item_size >= offsetof(speaker_playback_packet_t, samples) &&
                 item_size <= sizeof(*packet);
    if (valid) {
        memcpy(packet, item, item_size);
        valid = packet->sample_count <=
                    CONFIG_NUWAX_VOICE_MAX_SAMPLES_PER_FRAME &&
                item_size == speaker_packet_storage_size(packet->sample_count);
    }
    const size_t header_size = offsetof(speaker_playback_packet_t, samples);
    const uint32_t stored_samples =
        item_size >= header_size && ((item_size - header_size) & 1U) == 0U
            ? (uint32_t)((item_size - header_size) / sizeof(int16_t))
            : 0U;
    speaker_packet_ring_note_removed(stored_samples);
    vRingbufferReturnItem(s_speaker_ctx.packets, item);

    if (!valid) {
        ESP_LOGE(TAG,
                 "speaker ring record invalid bytes=%u max=%u",
                 (unsigned)item_size,
                 (unsigned)sizeof(*packet));
    }
    return valid;
}

static bool speaker_packet_ring_receive_notified(
    speaker_playback_packet_t *packet,
    TickType_t wait_ticks) {
    if (speaker_packet_ring_receive(packet, 0)) {
        /* Each queued record notifies this single consumer.  When the ring is
         * already readable, consume any matching stale count so the next
         * empty-ring wait can block instead of spinning once.  A producer can
         * race this clear without losing work because every loop checks the
         * ring before waiting on the notification. */
        (void)ulTaskNotifyTake(pdTRUE, 0);
        return true;
    }
    (void)ulTaskNotifyTake(pdTRUE, wait_ticks);
    return speaker_packet_ring_receive(packet, 0);
}

static uint32_t speaker_next_generation(uint32_t generation) {
    generation++;
    return generation == 0 ? 1 : generation;
}

static bool speaker_generation_source_matches_locked(uint32_t connection_epoch,
                                                     uint32_t generation) {
    return generation != 0U &&
           s_speaker_ctx.submitted_generation == generation &&
           s_speaker_ctx.submitted_connection_epoch == connection_epoch;
}

static bool speaker_start_gate_matches_locked(uint32_t connection_epoch,
                                              uint32_t generation) {
    return generation != 0U &&
           s_speaker_ctx.start_gate_generation == generation &&
           s_speaker_ctx.start_gate_connection_epoch == connection_epoch &&
           s_speaker_ctx.submitted_generation == generation;
}

static bool speaker_downlink_admission_matches_locked(uint32_t epoch) {
    return epoch != 0U && s_speaker_ctx.downlink_admission_allowed &&
           s_speaker_ctx.downlink_admission_epoch == epoch;
}

static bool speaker_start_gate_authorized_locked(uint32_t connection_epoch,
                                                 uint32_t generation) {
    return speaker_start_gate_matches_locked(connection_epoch, generation) &&
           !s_speaker_ctx.start_gate_pending &&
           s_speaker_ctx.start_gate_authorized;
}

static void speaker_clear_start_gate_locked(void) {
    s_speaker_ctx.start_gate_pending = false;
    s_speaker_ctx.start_gate_authorized = false;
    s_speaker_ctx.start_gate_generation = 0U;
    s_speaker_ctx.start_gate_connection_epoch = 0U;
    s_speaker_ctx.start_gate_deadline_ms = 0U;
}

static bool speaker_end_seen_locked(uint32_t generation) {
    return generation != 0U && s_speaker_ctx.end_seen &&
           s_speaker_ctx.end_seen_generation == generation;
}

static void speaker_clear_end_seen_locked(void) {
    s_speaker_ctx.end_seen = false;
    s_speaker_ctx.end_seen_generation = 0U;
    s_speaker_ctx.end_seen_sequence = 0U;
}

static void speaker_reset_stream_runtime_locked(bool clear_metadata) {
    nuwax_speaker_stream_reset(&s_speaker_ctx.stream);
    s_speaker_ctx.stream_protocol_faulted = false;
    s_speaker_ctx.protocol_fault_pending_generation = 0U;
    s_speaker_ctx.protocol_fault_pending_connection_epoch = 0U;
    s_speaker_ctx.protocol_fault_notified_generation = 0U;
    s_speaker_ctx.protocol_fault_notified_connection_epoch = 0U;
    s_speaker_ctx.playback_last_progress_ms = 0U;
    s_speaker_ctx.progress_callback_ms = 0U;
    s_speaker_ctx.ingress_progress_callback_ms = 0U;
    s_speaker_ctx.played_bytes = 0U;
    s_speaker_ctx.jitter_rebuffering = false;
    s_speaker_ctx.jitter_rebuffer_started_ms = 0U;
    s_speaker_ctx.jitter_empty_started_ms = 0U;
    s_speaker_ctx.jitter_rebuffer_entry_pcm_frames = 0U;
    s_speaker_ctx.jitter_low_watermark_ms = UINT32_MAX;
    s_speaker_ctx.jitter_resume_target_ms =
        NUWAX_SPEAKER_JITTER_RESUME_MIN_MS;
    s_speaker_ctx.jitter_resume_fade_pending = false;
    if (clear_metadata) {
        s_speaker_ctx.metadata_expected_frames = 0U;
        s_speaker_ctx.metadata_expected_bytes = 0U;
    }
}

static void speaker_reset_jitter_learning_locked(uint32_t connection_epoch) {
    s_speaker_ctx.jitter_startup_target_ms =
        NUWAX_SPEAKER_JITTER_STARTUP_BASE_MS;
    s_speaker_ctx.jitter_recent_instability_history = 0U;
    s_speaker_ctx.jitter_stable_playbacks = 0U;
    s_speaker_ctx.jitter_learning_connection_epoch = connection_epoch;
    s_speaker_ctx.jitter_last_playback_completed_ms = 0U;
    s_speaker_ctx.jitter_learning_reset_pending = false;
}

static void speaker_apply_rebuffer_fade_in(
    speaker_playback_packet_t *packet) {
    if (!packet || packet->sample_count == 0U ||
        packet->sample_rate == 0U) {
        return;
    }
    size_t fade_samples =
        ((size_t)packet->sample_rate * NUWAX_SPEAKER_REBUFFER_FADE_IN_MS) /
        1000U;
    if (fade_samples == 0U) {
        fade_samples = 1U;
    } else if (fade_samples > packet->sample_count) {
        fade_samples = packet->sample_count;
    }
    for (size_t i = 0U; i < fade_samples; ++i) {
        packet->samples[i] = (int16_t)(
            ((int32_t)packet->samples[i] * (int32_t)(i + 1U)) /
            (int32_t)fade_samples);
    }
}

static const char *speaker_stream_result_name(
    nuwax_speaker_stream_result_t result) {
    switch (result) {
        case NUWAX_SPEAKER_STREAM_OK:
            return "ok";
        case NUWAX_SPEAKER_STREAM_INACTIVE:
            return "inactive";
        case NUWAX_SPEAKER_STREAM_WRONG_GENERATION:
            return "wrong_generation";
        case NUWAX_SPEAKER_STREAM_INVALID_FLAGS:
            return "invalid_flags";
        case NUWAX_SPEAKER_STREAM_START_REQUIRED:
            return "start_required";
        case NUWAX_SPEAKER_STREAM_INVALID_START:
            return "invalid_start";
        case NUWAX_SPEAKER_STREAM_POST_END:
            return "post_end";
        case NUWAX_SPEAKER_STREAM_BAD_SEQUENCE:
            return "bad_sequence";
        case NUWAX_SPEAKER_STREAM_BAD_FRAME_SAMPLES:
            return "bad_frame_samples";
        case NUWAX_SPEAKER_STREAM_TOTAL_EXCEEDED:
            return "total_exceeded";
        case NUWAX_SPEAKER_STREAM_TOTAL_MISMATCH:
            return "total_mismatch";
        default:
            return "unknown";
    }
}


static bool speaker_should_publish_speaking_locked(uint32_t connection_epoch,
                                                   uint32_t generation) {
    if (generation == 0U ||
        !speaker_generation_source_matches_locked(connection_epoch,
                                                  generation) ||
        !nuwax_speaker_generation_is_current(
            s_speaker_ctx.submitted_generation,
            s_speaker_ctx.playback_generation,
            s_speaker_ctx.drop_until_end,
            generation) ||
        s_speaker_ctx.speaking_state_generation == generation) {
        return false;
    }
    s_speaker_ctx.speaking_state_generation = generation;
    return true;
}

static bool speaker_stop_completion_matches_locked(uint32_t connection_epoch,
                                                   uint32_t generation) {
    return generation != 0U &&
           s_speaker_ctx.completed_stop_generation == generation &&
           (connection_epoch == 0U ||
            s_speaker_ctx.completed_stop_connection_epoch ==
                connection_epoch) &&
           s_speaker_ctx.completed_stop_timestamp_us > 0;
}

static bool speaker_stop_failure_matches_locked(uint32_t connection_epoch,
                                                uint32_t generation) {
    return s_speaker_ctx.stop_fault_active && generation != 0U &&
           s_speaker_ctx.failed_stop_generation == generation &&
           (connection_epoch == 0U ||
            s_speaker_ctx.failed_stop_connection_epoch ==
                connection_epoch) &&
           s_speaker_ctx.failed_stop_timestamp_us > 0;
}

static void speaker_publish_stop_failure_locked(uint32_t connection_epoch,
                                                uint32_t generation,
                                                int64_t timestamp_us,
                                                esp_err_t result) {
    s_speaker_ctx.stop_fault_active = true;
    s_speaker_ctx.failed_stop_generation = generation;
    s_speaker_ctx.failed_stop_connection_epoch = connection_epoch;
    s_speaker_ctx.failed_stop_timestamp_us = timestamp_us;
    s_speaker_ctx.failed_stop_result = result;
}

static void speaker_clear_stop_failure_locked(uint32_t connection_epoch,
                                              uint32_t generation) {
    if (s_speaker_ctx.failed_stop_generation != generation ||
        (connection_epoch != 0U &&
         s_speaker_ctx.failed_stop_connection_epoch != connection_epoch)) {
        return;
    }
    s_speaker_ctx.stop_fault_active = false;
    s_speaker_ctx.failed_stop_generation = 0U;
    s_speaker_ctx.failed_stop_connection_epoch = 0U;
    s_speaker_ctx.failed_stop_timestamp_us = 0;
    s_speaker_ctx.failed_stop_result = ESP_ERR_INVALID_STATE;
}

static void speaker_notify_playback_fault(uint32_t connection_epoch,
                                          uint32_t generation,
                                          esp_err_t result) {
    if (generation == 0U || result == ESP_OK ||
        !s_speaker_ctx.callbacks.on_playback_fault) {
        return;
    }
    s_speaker_ctx.callbacks.on_playback_fault(connection_epoch,
                                              generation,
                                              result);
}

static void speaker_schedule_protocol_fault(uint32_t connection_epoch,
                                            uint32_t generation,
                                            esp_err_t result,
                                            const char *reason,
                                            bool stream_stall) {
    bool notify = false;
    uint32_t expected_sequence = 0U;
    uint32_t received_frames = 0U;
    uint64_t received_bytes = 0U;
    speaker_lock();
    if (speaker_generation_source_matches_locked(connection_epoch,
                                                 generation) &&
        s_speaker_ctx.stream.active &&
        s_speaker_ctx.stream.generation == generation) {
        s_speaker_ctx.stream_protocol_faulted = true;
        s_speaker_ctx.protocol_fault_pending_generation = generation;
        s_speaker_ctx.protocol_fault_pending_connection_epoch =
            connection_epoch;
        expected_sequence = s_speaker_ctx.stream.next_sequence;
        received_frames = s_speaker_ctx.stream.pcm_frames;
        received_bytes = s_speaker_ctx.stream.pcm_bytes;
        if (s_speaker_ctx.protocol_fault_notified_generation != generation ||
            s_speaker_ctx.protocol_fault_notified_connection_epoch !=
                connection_epoch) {
            s_speaker_ctx.protocol_fault_notified_generation = generation;
            s_speaker_ctx.protocol_fault_notified_connection_epoch =
                connection_epoch;
            s_speaker_ctx.stream_protocol_faults++;
            if (stream_stall) {
                s_speaker_ctx.stream_stall_faults++;
            }
            notify = true;
        }
    }
    speaker_unlock();
    if (!notify) {
        return;
    }
    ESP_LOGE(TAG,
             "speaker downlink stream fault generation=%lu result=%s reason=%s expected_seq=%lu received_frames=%lu received_bytes=%llu",
             (unsigned long)generation,
             esp_err_to_name(result),
             reason ? reason : "-",
             (unsigned long)expected_sequence,
             (unsigned long)received_frames,
             (unsigned long long)received_bytes);
    speaker_notify_playback_fault(connection_epoch, generation, result);
}

static bool speaker_process_pending_protocol_fault(void) {
    speaker_generation_ref_t fault = {0};
    speaker_lock();
    fault.generation = s_speaker_ctx.protocol_fault_pending_generation;
    fault.connection_epoch =
        s_speaker_ctx.protocol_fault_pending_connection_epoch;
    s_speaker_ctx.protocol_fault_pending_generation = 0U;
    s_speaker_ctx.protocol_fault_pending_connection_epoch = 0U;
    speaker_unlock();
    if (fault.generation == 0U) {
        return false;
    }
    if (!speaker_cancel_now_if_current_epoch(
            fault.connection_epoch,
            fault.generation,
            "downlink_protocol_fault")) {
        ESP_LOGE(TAG,
                 "speaker downlink protocol-fault stop incomplete generation=%lu",
                  (unsigned long)fault.generation);
    }
    return true;
}

static speaker_generation_ref_t speaker_stalled_generation(uint32_t now_ms) {
    speaker_generation_ref_t stalled = {0};
    speaker_lock();
    if (s_speaker_ctx.playback_active &&
        !s_speaker_ctx.stream_protocol_faulted &&
        s_speaker_ctx.playback_last_progress_ms != 0U &&
        s_speaker_ctx.stream.active &&
        /* `end_accepted` is an ingress fact: it becomes true as soon as the
         * END packet enters the queue. Keep the watchdog alive until the
         * playback task has actually dequeued that END and completed the
         * physical stop, otherwise a queued END can mask a stalled speaker. */
        !s_speaker_ctx.end_seen &&
        nuwax_speaker_generation_is_current(
            s_speaker_ctx.submitted_generation,
            s_speaker_ctx.playback_generation,
            s_speaker_ctx.drop_until_end,
            s_speaker_ctx.stream.generation) &&
        elapsed_ms(now_ms,
                   s_speaker_ctx.playback_last_progress_ms +
                       NUWAX_SPEAKER_STREAM_STALL_TIMEOUT_MS)) {
        stalled.generation = s_speaker_ctx.stream.generation;
        stalled.connection_epoch =
            s_speaker_ctx.submitted_connection_epoch;
    }
    speaker_unlock();
    return stalled;
}

static void speaker_notify_cancel_recovery_once(uint32_t connection_epoch,
                                                uint32_t generation,
                                                const char *reason) {
    bool notify = false;
    speaker_lock();
    if (generation != 0U &&
        (s_speaker_ctx.cancel_recovery_notified_generation != generation ||
         s_speaker_ctx.cancel_recovery_notified_connection_epoch !=
             connection_epoch)) {
        s_speaker_ctx.cancel_recovery_notified_generation = generation;
        s_speaker_ctx.cancel_recovery_notified_connection_epoch =
            connection_epoch;
        notify = true;
    }
    speaker_unlock();
    if (notify && s_speaker_ctx.callbacks.return_to_listening_after_cancel) {
        s_speaker_ctx.callbacks.return_to_listening_after_cancel(
            connection_epoch,
            generation,
            reason);
    }
}

static bool speaker_claim_packet_io(uint32_t connection_epoch,
                                    uint32_t generation) {
    const uint32_t claim_deadline_ms =
        monotonic_ms() + NUWAX_SPEAKER_CLAIM_WAIT_TIMEOUT_MS;
    while (true) {
        bool claimed = false;
        bool wait_for_stop = false;
        uint32_t stop_owner_generation = 0U;
        speaker_lock();
        const bool current =
            speaker_generation_source_matches_locked(connection_epoch,
                                                     generation) &&
            nuwax_speaker_generation_is_current(
            s_speaker_ctx.submitted_generation,
            s_speaker_ctx.playback_generation,
            s_speaker_ctx.drop_until_end,
            generation) &&
                             !s_speaker_ctx.stream_protocol_faulted;
        const bool start_authorized =
            speaker_start_gate_authorized_locked(connection_epoch,
                                                 generation);
        if (start_authorized && nuwax_speaker_write_claim_allowed(
                s_speaker_ctx.submitted_generation,
                s_speaker_ctx.playback_generation,
                s_speaker_ctx.drop_until_end,
                s_speaker_ctx.io_claim_generation,
                s_speaker_ctx.output_stop_owner_generation,
                generation)) {
            s_speaker_ctx.io_claim_generation = generation;
            claimed = true;
        } else {
            stop_owner_generation =
                s_speaker_ctx.output_stop_owner_generation;
            wait_for_stop = current &&
                            start_authorized &&
                            stop_owner_generation != 0U;
        }
        speaker_unlock();
        if (claimed || !wait_for_stop) {
            return claimed;
        }
        if (elapsed_ms(monotonic_ms(), claim_deadline_ms)) {
            ESP_LOGE(TAG,
                     "speaker packet I/O claim timed out generation=%lu stop_owner=%lu",
                     (unsigned long)generation,
                     (unsigned long)stop_owner_generation);
            return false;
        }
        vTaskDelay(speaker_positive_delay_ticks(1));
    }
}

static void speaker_release_packet_io(uint32_t generation) {
    speaker_lock();
    if (s_speaker_ctx.io_claim_generation == generation) {
        s_speaker_ctx.io_claim_generation = 0;
    }
    speaker_unlock();
}

static uint32_t speaker_claim_idle_output_action(void) {
    uint32_t claim_generation = 0;
    speaker_lock();
    const bool queue_empty = speaker_packet_ring_count() == 0U;
    if (!s_speaker_ctx.playback_active &&
        !s_speaker_ctx.playback_session_active &&
        !s_speaker_ctx.start_gate_pending &&
        !s_speaker_ctx.start_gate_authorized &&
        queue_empty &&
        s_speaker_ctx.io_claim_generation == 0 &&
        s_speaker_ctx.completion_claim_generation == 0 &&
        s_speaker_ctx.maintenance_claim_generation == 0 &&
        s_speaker_ctx.cancel_pending_generation == 0 &&
        s_speaker_ctx.output_stop_owner_generation == 0 &&
        !s_speaker_ctx.runtime_reset_in_progress) {
        s_speaker_ctx.maintenance_claim_sequence =
            speaker_next_generation(s_speaker_ctx.maintenance_claim_sequence);
        claim_generation = s_speaker_ctx.maintenance_claim_sequence;
        s_speaker_ctx.maintenance_claim_generation = claim_generation;
    }
    speaker_unlock();
    return claim_generation;
}

static void speaker_release_idle_output_action(uint32_t claim_generation) {
    if (claim_generation == 0) {
        return;
    }
    speaker_lock();
    if (s_speaker_ctx.maintenance_claim_generation == claim_generation) {
        s_speaker_ctx.maintenance_claim_generation = 0;
    }
    speaker_unlock();
}

static bool speaker_packet_is_submitted(uint32_t connection_epoch,
                                        uint32_t generation) {
    bool current = false;
    speaker_lock();
    current = speaker_generation_source_matches_locked(connection_epoch,
                                                        generation) &&
              !s_speaker_ctx.drop_until_end &&
              !s_speaker_ctx.stream_protocol_faulted &&
              !speaker_end_seen_locked(generation);
    speaker_unlock();
    return current;
}

static bool speaker_packet_is_active(uint32_t connection_epoch,
                                     uint32_t generation) {
    bool current = false;
    speaker_lock();
    current = s_speaker_ctx.playback_active &&
              !s_speaker_ctx.stream_protocol_faulted &&
              s_speaker_ctx.playback_connection_epoch == connection_epoch &&
              speaker_start_gate_authorized_locked(connection_epoch,
                                                   generation) &&
              nuwax_speaker_generation_is_current(
                  s_speaker_ctx.submitted_generation,
                  s_speaker_ctx.playback_generation,
                  s_speaker_ctx.drop_until_end,
                  generation);
    speaker_unlock();
    return current;
}

static bool speaker_output_absent_locked(void) {
    return !s_speaker_ctx.output.initialized &&
           !s_speaker_ctx.output.tx_handle &&
           !s_speaker_ctx.output.lock &&
           !s_speaker_ctx.output.stereo_buffer &&
           s_speaker_ctx.output.stereo_capacity == 0U &&
           !s_speaker_ctx.output.vocat_shared_tx;
}

static bool speaker_output_ready_locked(void) {
    return s_speaker_ctx.output.initialized &&
           s_speaker_ctx.output.tx_handle &&
           s_speaker_ctx.output.lock &&
           (!s_speaker_ctx.output.software_ref_required ||
            s_speaker_ctx.output.software_ref_ready);
}

static esp_err_t speaker_sync_output_volume_locked(void) {
    s_speaker_ctx.output_volume_sync_pending = true;
    if (speaker_output_absent_locked()) {
        /* A future lazy init will apply the logical volume. */
        return ESP_OK;
    }
    if (!speaker_output_ready_locked()) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = nuwax_speaker_output_set_volume_percent(
        &s_speaker_ctx.output, s_speaker_ctx.volume_percent);
    if (err == ESP_OK) {
        s_speaker_ctx.output_volume_sync_pending = false;
    }
    return err;
}

static esp_err_t speaker_clear_probe_duck_locked(bool restore_volume) {
    if (!s_speaker_ctx.probe_duck_active) {
        if (!restore_volume) {
            s_speaker_ctx.output_volume_sync_pending = true;
        } else if (s_speaker_ctx.output_volume_sync_pending) {
            return speaker_sync_output_volume_locked();
        }
        return ESP_OK;
    }
    if (restore_volume) {
        const esp_err_t err = speaker_sync_output_volume_locked();
        if (err != ESP_OK) {
            /* Keep the active marker until the physical volume is restored;
             * the speaker task will retry instead of silently losing duck
             * state and carrying the attenuated volume into the next turn. */
            return err;
        }
    } else {
        s_speaker_ctx.output_volume_sync_pending = true;
    }
    s_speaker_ctx.probe_duck_active = false;
    s_speaker_ctx.probe_duck_generation = 0U;
    s_speaker_ctx.probe_duck_deadline_ms = 0U;
    s_speaker_ctx.probe_duck_output_percent = 0U;
    return ESP_OK;
}

static bool speaker_restore_probe_duck_if_due(uint32_t now_ms) {
    bool restored = false;
    esp_err_t restore_err = ESP_OK;
    uint32_t generation = 0U;
    speaker_lock();
    if (s_speaker_ctx.probe_duck_active &&
        (elapsed_ms(now_ms, s_speaker_ctx.probe_duck_deadline_ms) ||
         !nuwax_speaker_generation_is_current(
             s_speaker_ctx.submitted_generation,
             s_speaker_ctx.playback_generation,
             s_speaker_ctx.drop_until_end,
             s_speaker_ctx.probe_duck_generation))) {
        generation = s_speaker_ctx.probe_duck_generation;
        /* Even when the generation is stale, the output may remain alive for
         * the next turn. Always restore the logical volume before clearing the
         * marker; otherwise a failed/aborted probe can leak its duck level. */
        restore_err = speaker_clear_probe_duck_locked(true);
        restored = restore_err == ESP_OK;
        if (!restored) {
            s_speaker_ctx.probe_duck_deadline_ms = now_ms + 20U;
        }
    }
    speaker_unlock();
    if (restored) {
        ESP_LOGI(TAG,
                 "speaker probe duck ended generation=%lu",
                 (unsigned long)generation);
    } else if (restore_err != ESP_OK) {
        ESP_LOGW(TAG,
                 "speaker probe duck restore deferred generation=%lu err=%s",
                 (unsigned long)generation,
                 esp_err_to_name(restore_err));
    }
    return restored;
}

static bool speaker_activate_start_packet(const speaker_playback_packet_t *packet,
                                          uint32_t startup_wait_ms) {
    if (!packet || (packet->flags & NUWAX_AUDIO_FLAG_START) == 0) {
        return false;
    }
    speaker_lock();
    if (!speaker_generation_source_matches_locked(packet->connection_epoch,
                                                  packet->generation) ||
        s_speaker_ctx.drop_until_end ||
        s_speaker_ctx.stream_protocol_faulted ||
        !s_speaker_ctx.stream.active ||
        !s_speaker_ctx.stream.started ||
        s_speaker_ctx.stream.generation != packet->generation ||
        !speaker_start_gate_authorized_locked(packet->connection_epoch,
                                              packet->generation)) {
        speaker_unlock();
        return false;
    }
    /* A new START must never inherit a prior probe/fade attenuation. Mark the
     * logical volume for synchronization; the actual setter runs immediately
     * before the first I2S write in ensure_speaker_output_ready(). */
    (void)speaker_clear_probe_duck_locked(false);
    s_speaker_ctx.output_volume_sync_pending = true;
    s_speaker_ctx.playback_generation = packet->generation;
    s_speaker_ctx.playback_connection_epoch = packet->connection_epoch;
    s_speaker_ctx.fade_cancel_active = false;
    s_speaker_ctx.fade_cancel_generation = 0;
    s_speaker_ctx.fade_cancel_total_samples = 0;
    s_speaker_ctx.fade_cancel_processed_samples = 0;
    s_speaker_ctx.fade_cancel_deadline_ms = 0;
    s_speaker_ctx.fade_cancel_source[0] = '\0';
    s_speaker_ctx.playback_active = true;
    s_speaker_ctx.queued_frames = 0;
    s_speaker_ctx.played_frames = 0;
    s_speaker_ctx.queue_dropped_frames = 0;
    s_speaker_ctx.write_failures = 0;
    s_speaker_ctx.gap_silence_frames = 0;
    s_speaker_ctx.jitter_rebuffering = false;
    s_speaker_ctx.jitter_rebuffer_started_ms = 0U;
    s_speaker_ctx.jitter_rebuffer_events = 0U;
    s_speaker_ctx.jitter_rebuffer_wait_ms = 0U;
    s_speaker_ctx.jitter_rebuffer_max_wait_ms = 0U;
    s_speaker_ctx.jitter_empty_started_ms = 0U;
    s_speaker_ctx.jitter_rebuffer_entry_pcm_frames = 0U;
    s_speaker_ctx.jitter_tail_wait_events = 0U;
    s_speaker_ctx.jitter_tail_wait_ms = 0U;
    s_speaker_ctx.jitter_low_watermark_ms = UINT32_MAX;
    s_speaker_ctx.jitter_resume_target_ms =
        NUWAX_SPEAKER_JITTER_RESUME_MIN_MS;
    s_speaker_ctx.startup_buffer_wait_ms = startup_wait_ms;
    s_speaker_ctx.last_frame_samples =
        s_speaker_ctx.stream.frame_samples;
    speaker_unlock();

    return true;
}

static uint8_t clamp_speaker_volume_percent(int volume_percent) {
    if (volume_percent < 0) {
        return 0;
    }
    if (volume_percent > 100) {
        return 100;
    }
    return (uint8_t)volume_percent;
}

static uint8_t load_speaker_volume_percent(void) {
    uint8_t volume = clamp_speaker_volume_percent(CONFIG_NUWAX_SPEAKER_DEFAULT_VOLUME_PERCENT);
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NUWAX_SETTINGS_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return volume;
    }
    uint8_t stored = volume;
    err = nvs_get_u8(handle, NUWAX_SETTINGS_NVS_KEY_VOLUME, &stored);
    nvs_close(handle);
    if (err == ESP_OK && stored <= 100) {
        volume = stored;
    }
    return volume;
}

static void save_speaker_volume_percent(uint8_t volume_percent) {
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NUWAX_SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "speaker volume NVS open failed: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_u8(handle, NUWAX_SETTINGS_NVS_KEY_VOLUME, volume_percent);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "speaker volume NVS save failed: %s", esp_err_to_name(err));
    }
}

static bool speaker_get_drop_state(uint32_t now_ms) {
    bool dropping = false;
    speaker_lock();
    if (s_speaker_ctx.drop_until_end &&
        s_speaker_ctx.drop_deadline_ms != 0 &&
        elapsed_ms(now_ms, s_speaker_ctx.drop_deadline_ms)) {
        ESP_LOGW(TAG,
                 "speaker cancel drop window expired turn=%s playback=%s dropped=%lu stale_total=%lu; accepting new audio",
                 s_speaker_ctx.stale_turn_id[0] ? s_speaker_ctx.stale_turn_id : "-",
                 s_speaker_ctx.stale_playback_id[0] ? s_speaker_ctx.stale_playback_id : "-",
                 (unsigned long)s_speaker_ctx.dropped_frames,
                 (unsigned long)s_speaker_ctx.stale_playback_frames_dropped);
        s_speaker_ctx.drop_until_end = false;
        s_speaker_ctx.dropped_frames = 0;
        s_speaker_ctx.drop_deadline_ms = 0;
    }
    dropping = s_speaker_ctx.drop_until_end;
    speaker_unlock();
    return dropping;
}

static uint32_t speaker_submit_start_generation(uint32_t sequence,
                                                uint8_t flags,
                                                uint32_t connection_epoch,
                                                uint32_t admission_epoch) {
    if ((flags & NUWAX_AUDIO_FLAG_START) == 0) {
        return 0;
    }

    bool cleared = false;
    char stale_turn_id[sizeof(s_speaker_ctx.stale_turn_id)] = {0};
    char stale_playback_id[sizeof(s_speaker_ctx.stale_playback_id)] = {0};
    uint32_t generation = 0;
    uint32_t cleared_frames = 0;
    const uint32_t admission_deadline_ms =
        monotonic_ms() + NUWAX_SPEAKER_CLAIM_WAIT_TIMEOUT_MS;
    while (generation == 0) {
        speaker_lock();
        if (admission_epoch != 0U &&
            !speaker_downlink_admission_matches_locked(admission_epoch)) {
            speaker_unlock();
            ESP_LOGI(TAG,
                     "speaker START rejected stale downlink admission epoch=%lu seq=%u",
                     (unsigned long)admission_epoch,
                     (unsigned)sequence);
            return 0U;
        }
        if (s_speaker_ctx.start_gate_pending ||
            s_speaker_ctx.start_gate_authorized ||
            s_speaker_ctx.playback_active) {
            const uint32_t pending_generation =
                s_speaker_ctx.start_gate_generation != 0U
                    ? s_speaker_ctx.start_gate_generation
                    : s_speaker_ctx.playback_generation;
            speaker_unlock();
            ESP_LOGW(TAG,
                     "speaker overlapping START rejected seq=%u generation=%lu",
                     (unsigned)sequence,
                     (unsigned long)pending_generation);
            return 0U;
        }
        if (s_speaker_ctx.stop_fault_active) {
            const uint32_t failed_generation =
                s_speaker_ctx.failed_stop_generation;
            const esp_err_t failed_result =
                s_speaker_ctx.failed_stop_result;
            speaker_unlock();
            ESP_LOGE(TAG,
                     "speaker START rejected by unresolved stop fault seq=%u failed_generation=%lu result=%s",
                     (unsigned)sequence,
                     (unsigned long)failed_generation,
                     esp_err_to_name(failed_result));
            return 0U;
        }
        if (nuwax_speaker_start_submission_allowed(
                s_speaker_ctx.io_claim_generation,
                s_speaker_ctx.completion_claim_generation,
                s_speaker_ctx.maintenance_claim_generation,
                s_speaker_ctx.cancel_pending_generation,
                s_speaker_ctx.output_stop_owner_generation,
                s_speaker_ctx.runtime_reset_in_progress)) {
            if (s_speaker_ctx.drop_until_end) {
                strlcpy(stale_turn_id,
                        s_speaker_ctx.stale_turn_id,
                        sizeof(stale_turn_id));
                strlcpy(stale_playback_id,
                        s_speaker_ctx.stale_playback_id,
                        sizeof(stale_playback_id));
                s_speaker_ctx.drop_until_end = false;
                s_speaker_ctx.dropped_frames = 0;
                s_speaker_ctx.drop_deadline_ms = 0;
                cleared = true;
            }
            s_speaker_ctx.submitted_generation =
                speaker_next_generation(s_speaker_ctx.submitted_generation);
            s_speaker_ctx.submitted_connection_epoch = connection_epoch;
            const uint32_t learning_now_ms = monotonic_ms();
            if (s_speaker_ctx.jitter_learning_reset_pending ||
                s_speaker_ctx.jitter_learning_connection_epoch !=
                    connection_epoch ||
                (s_speaker_ctx.jitter_last_playback_completed_ms != 0U &&
                 elapsed_ms(
                     learning_now_ms,
                     s_speaker_ctx.jitter_last_playback_completed_ms +
                         NUWAX_SPEAKER_JITTER_LEARNING_IDLE_RESET_MS))) {
                speaker_reset_jitter_learning_locked(connection_epoch);
            }
            speaker_clear_end_seen_locked();
            s_speaker_ctx.cancel_stopped_generation = 0U;
            s_speaker_ctx.cancel_stopped_connection_epoch = 0U;
            s_speaker_ctx.cancel_request_generation = 0U;
            s_speaker_ctx.cancel_request_connection_epoch = 0U;
            s_speaker_ctx.cancel_request_timestamp_us = 0;
            generation = s_speaker_ctx.submitted_generation;
            s_speaker_ctx.playback_connection_epoch = 0U;
            s_speaker_ctx.speaking_state_generation = 0U;
            s_speaker_ctx.cancel_recovery_notified_generation = 0U;
            s_speaker_ctx.cancel_recovery_notified_connection_epoch = 0U;
            speaker_reset_stream_runtime_locked(false);
            /* START carries no turn/playback ID. Do not bind the handoff
             * cache here: metadata from a previous playback could otherwise
             * poison a new generation before the owner authorizes its IDs.
             * Totals are atomically bound in authorize_generation(). */
            nuwax_speaker_stream_begin_generation(
                &s_speaker_ctx.stream,
                generation,
                0U,
                0U);
            s_speaker_ctx.start_gate_pending = true;
            s_speaker_ctx.start_gate_authorized = false;
            s_speaker_ctx.start_gate_generation = generation;
            s_speaker_ctx.start_gate_connection_epoch = connection_epoch;
            s_speaker_ctx.start_gate_deadline_ms =
                monotonic_ms() + NUWAX_SPEAKER_START_GATE_TIMEOUT_MS;
            s_speaker_ctx.ingress_last_pcm_rx_ms = 0U;
            s_speaker_ctx.ingress_max_gap_ms = 0U;
            if (!s_speaker_ctx.playback_session_active) {
                s_speaker_ctx.playback_begin_rx_us = 0;
            }
            s_speaker_ctx.first_pcm_rx_us = 0;
            s_speaker_ctx.start_gate_authorized_us = 0;
            s_speaker_ctx.startup_watermark_ready_us = 0;
            s_speaker_ctx.first_i2s_write_us = 0;
            if (s_speaker_ctx.packets) {
                cleared_frames = speaker_packet_ring_clear_locked();
            }
        }
        speaker_unlock();
        if (generation == 0) {
            if (elapsed_ms(monotonic_ms(), admission_deadline_ms)) {
                ESP_LOGE(TAG,
                         "speaker START admission timed out seq=%u timeout_ms=%u",
                         (unsigned)sequence,
                         (unsigned)NUWAX_SPEAKER_CLAIM_WAIT_TIMEOUT_MS);
                return 0U;
            }
            vTaskDelay(speaker_positive_delay_ticks(1));
        }
    }

    if (cleared_frames > 0) {
        ESP_LOGI(TAG,
                 "speaker playback queue cleared frames=%lu reason=new_start",
                 (unsigned long)cleared_frames);
    }

    if (cleared) {
        ESP_LOGI(TAG,
                 "speaker accepted new START after cancel seq=%u stale_turn=%s stale_playback=%s",
                 (unsigned)sequence,
                 stale_turn_id[0] ? stale_turn_id : "-",
                 stale_playback_id[0] ? stale_playback_id : "-");
    }
    return generation;
}

static bool speaker_note_cancelled_frame(uint32_t connection_epoch,
                                         uint32_t sequence,
                                         uint8_t flags) {
    uint32_t dropped = 0;
    uint32_t generation = 0U;
    uint32_t stale_total = 0;
    bool drain_complete = false;
    bool first_dropped_frame = false;
    char turn_id[sizeof(s_speaker_ctx.stale_turn_id)] = {0};
    char playback_id[sizeof(s_speaker_ctx.stale_playback_id)] = {0};
    speaker_lock();
    if (!s_speaker_ctx.drop_until_end ||
        s_speaker_ctx.cancel_stopped_connection_epoch != connection_epoch) {
        speaker_unlock();
        return false;
    }
    generation = s_speaker_ctx.cancel_stopped_generation;
    s_speaker_ctx.dropped_frames++;
    s_speaker_ctx.stale_playback_frames_dropped++;
    dropped = s_speaker_ctx.dropped_frames;
    first_dropped_frame = dropped == 1;
    stale_total = s_speaker_ctx.stale_playback_frames_dropped;
    strlcpy(turn_id, s_speaker_ctx.stale_turn_id, sizeof(turn_id));
    strlcpy(playback_id, s_speaker_ctx.stale_playback_id, sizeof(playback_id));
    if ((flags & NUWAX_AUDIO_FLAG_END) != 0) {
        s_speaker_ctx.drop_until_end = false;
        s_speaker_ctx.dropped_frames = 0;
        s_speaker_ctx.drop_deadline_ms = 0;
        drain_complete = true;
    }
    speaker_unlock();

    if (first_dropped_frame || drain_complete || (dropped % 50U) == 0U) {
        ESP_LOGI(TAG,
                 "speaker drop cancelled audio turn=%s playback=%s seq=%u flags=0x%02x dropped=%lu stale_total=%lu",
                 turn_id[0] ? turn_id : "-",
                 playback_id[0] ? playback_id : "-",
                 (unsigned)sequence,
                 flags,
                 (unsigned long)dropped,
                 (unsigned long)stale_total);
    }
    if (drain_complete) {
        ESP_LOGI(TAG,
                 "speaker cancelled audio drain complete turn=%s playback=%s dropped=%lu stale_total=%lu",
                 turn_id[0] ? turn_id : "-",
                 playback_id[0] ? playback_id : "-",
                 (unsigned long)dropped,
                 (unsigned long)stale_total);
        if (generation != 0U && s_speaker_ctx.callbacks.on_stream_end) {
            s_speaker_ctx.callbacks.on_stream_end(connection_epoch, generation);
        }
    }
    return true;
}

static bool speaker_fade_cancel_due(uint32_t now_ms,
                                    char *source,
                                    size_t source_size,
                                    uint32_t *generation_out) {
    bool due = false;
    speaker_lock();
    if (s_speaker_ctx.fade_cancel_active &&
        s_speaker_ctx.fade_cancel_deadline_ms != 0 &&
        elapsed_ms(now_ms, s_speaker_ctx.fade_cancel_deadline_ms)) {
        due = true;
        if (source && source_size > 0) {
            strlcpy(source, s_speaker_ctx.fade_cancel_source, source_size);
        }
        if (generation_out) {
            *generation_out = s_speaker_ctx.fade_cancel_generation;
        }
    }
    speaker_unlock();
    return due;
}

static bool speaker_apply_fade_cancel(speaker_playback_packet_t *packet,
                                      char *source,
                                      size_t source_size,
                                      bool *fade_active) {
    if (!packet) {
        return false;
    }
    if (fade_active) {
        *fade_active = false;
    }

    bool cancel_after_packet = false;
    speaker_lock();
    if (s_speaker_ctx.fade_cancel_active &&
        packet->generation == s_speaker_ctx.fade_cancel_generation) {
        if (fade_active) {
            *fade_active = true;
        }
        const uint32_t total = s_speaker_ctx.fade_cancel_total_samples;
        const uint32_t processed = s_speaker_ctx.fade_cancel_processed_samples;
        for (size_t i = 0; i < packet->sample_count; i++) {
            const uint64_t position = (uint64_t)processed + i + 1U;
            const uint32_t gain_q15 =
                position >= total
                    ? 0
                    : (uint32_t)(((uint64_t)(total - position) * 32768ULL) / total);
            packet->samples[i] =
                (int16_t)(((int32_t)packet->samples[i] * (int32_t)gain_q15) >> 15);
        }
        const uint64_t next_processed = (uint64_t)processed + packet->sample_count;
        s_speaker_ctx.fade_cancel_processed_samples =
            next_processed >= total ? total : (uint32_t)next_processed;
        cancel_after_packet =
            s_speaker_ctx.fade_cancel_processed_samples >= total ||
            (packet->flags & NUWAX_AUDIO_FLAG_END) != 0;
        if (cancel_after_packet && source && source_size > 0) {
            strlcpy(source, s_speaker_ctx.fade_cancel_source, source_size);
        }
    }
    speaker_unlock();
    return cancel_after_packet;
}

static esp_err_t ensure_speaker_output_ready(void) {
    bool initialized_now = false;
    uint8_t logical_volume = 0U;
    speaker_lock();
    esp_err_t err = ESP_OK;
    if (!speaker_output_ready_locked()) {
        err = nuwax_speaker_output_init(&s_speaker_ctx.output);
        if (err == ESP_OK) {
            initialized_now = true;
            s_speaker_ctx.output_volume_sync_pending = true;
        }
    }
    if (err == ESP_OK && !s_speaker_ctx.probe_duck_active &&
        (initialized_now || s_speaker_ctx.output_volume_sync_pending)) {
        err = speaker_sync_output_volume_locked();
    }
    logical_volume = s_speaker_ctx.volume_percent;
    speaker_unlock();
    if (initialized_now && err == ESP_OK) {
        ESP_LOGI(TAG,
                 "speaker lazy init ok volume=%u%%",
                 (unsigned)logical_volume);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "speaker output preparation failed: %s", esp_err_to_name(err));
    }
    return err;
}

static void speaker_log_task_stack_watermark(const char *stage) {
    const TaskHandle_t task = s_speaker_ctx.playback_task;
    if (!task) {
        return;
    }
    const size_t free_bytes =
        (size_t)uxTaskGetStackHighWaterMark(task) * sizeof(StackType_t);
    if (free_bytes < NUWAX_SPEAKER_TASK_STACK_WARN_BYTES) {
        ESP_LOGW(TAG,
                 "speaker task resources stage=%s stack_min_free=%u",
                 stage ? stage : "-", (unsigned)free_bytes);
    } else {
        ESP_LOGI(TAG,
                 "speaker task resources stage=%s stack_min_free=%u",
                 stage ? stage : "-", (unsigned)free_bytes);
    }
}

static esp_err_t speaker_mute_idle_output(const char *reason) {
    if (!s_speaker_ctx.keep_initialized) {
        speaker_lock();
        const esp_err_t deinit_err =
            nuwax_speaker_output_deinit(&s_speaker_ctx.output);
        speaker_unlock();
        if (deinit_err != ESP_OK) {
            ESP_LOGW(TAG,
                     "speaker output deinit failed reason=%s err=%s",
                     reason ? reason : "-",
                     esp_err_to_name(deinit_err));
        }
        return deinit_err;
    }
    esp_err_t stop_err = nuwax_speaker_output_stop(&s_speaker_ctx.output);
    if (stop_err == ESP_OK) {
        ESP_LOGI(TAG, "speaker output kept initialized and muted reason=%s", reason ? reason : "-");
    } else if (stop_err == ESP_ERR_INVALID_STATE) {
        /* INVALID_STATE is only benign when no output handle exists.  Treating
         * it as successful while an initialized handle is present can advance
         * the conversation after an inaudible response. */
        speaker_lock();
        const bool output_present = !speaker_output_absent_locked();
        speaker_unlock();
        if (output_present) {
            ESP_LOGE(TAG,
                     "speaker output stop returned invalid state with live handle reason=%s",
                     reason ? reason : "-");
            return stop_err;
        }
        ESP_LOGI(TAG, "speaker output already idle reason=%s", reason ? reason : "-");
        stop_err = ESP_OK;
    } else {
        ESP_LOGW(TAG,
                 "speaker idle mute failed reason=%s err=%s; keeping handle for next playback",
                 reason ? reason : "-",
                 esp_err_to_name(stop_err));
    }
    return stop_err;
}

static esp_err_t speaker_drain_idle_output(const char *reason) {
    esp_err_t stop_err = nuwax_speaker_output_drain_and_stop(
        &s_speaker_ctx.output,
        NUWAX_SPEAKER_NATURAL_DRAIN_TIMEOUT_MS);
    if (stop_err != ESP_OK) {
        return stop_err;
    }

    speaker_lock();
    const bool keep_initialized = s_speaker_ctx.keep_initialized;
    const esp_err_t deinit_err = keep_initialized
                                     ? ESP_OK
                                     : nuwax_speaker_output_deinit(
                                           &s_speaker_ctx.output);
    speaker_unlock();
    if (!keep_initialized && deinit_err != ESP_OK) {
        ESP_LOGW(TAG,
                 "speaker output deinit after natural drain failed reason=%s err=%s",
                 reason ? reason : "-",
                 esp_err_to_name(deinit_err));
    }
    return deinit_err;
}

static void speaker_finish_playback_if_current(uint32_t connection_epoch,
                                               uint32_t generation,
                                               uint32_t sequence,
                                               uint32_t queued,
                                               uint32_t played,
                                               uint32_t failures,
                                               uint32_t queue_drops,
                                               const char *reason) {
    uint32_t queue_waiting = 0U;
    uint32_t submitted_generation_value = 0;
    uint32_t current_generation_value = 0;
    uint32_t gap_silence_frames = 0;
    uint32_t startup_buffer_wait_ms = 0U;
    uint32_t rebuffer_events = 0U;
    uint32_t rebuffer_wait_ms = 0U;
    uint32_t rebuffer_max_wait_ms = 0U;
    uint32_t tail_wait_events = 0U;
    uint32_t tail_wait_ms = 0U;
    uint32_t max_ingress_gap_ms = 0U;
    uint32_t low_watermark_ms = UINT32_MAX;
    uint32_t startup_target_ms = 0U;
    uint32_t next_startup_target_ms = 0U;
    uint32_t resume_target_ms = NUWAX_SPEAKER_JITTER_RESUME_MIN_MS;
    uint32_t post_end_frames_dropped = 0U;
    bool completion_claimed = false;
    char turn_id[sizeof(s_speaker_ctx.active_turn_id)] = {0};
    char playback_id[sizeof(s_speaker_ctx.active_playback_id)] = {0};
    speaker_lock();
    const bool generation_current =
        speaker_generation_source_matches_locked(connection_epoch,
                                                 generation) &&
        s_speaker_ctx.playback_connection_epoch == connection_epoch &&
        !s_speaker_ctx.drop_until_end &&
        !s_speaker_ctx.stream_protocol_faulted;
    if (generation_current) {
        s_speaker_ctx.end_seen = true;
        s_speaker_ctx.end_seen_generation = generation;
        s_speaker_ctx.end_seen_sequence = sequence;
        if (s_speaker_ctx.packets) {
            post_end_frames_dropped = speaker_packet_ring_clear_locked();
            if (post_end_frames_dropped > 0U) {
                s_speaker_ctx.stale_playback_frames_dropped +=
                    post_end_frames_dropped;
                s_speaker_ctx.queue_dropped_frames +=
                    post_end_frames_dropped;
            }
        }
    }
    queue_waiting = speaker_packet_ring_count();
    submitted_generation_value = s_speaker_ctx.submitted_generation;
    current_generation_value = s_speaker_ctx.playback_generation;
    gap_silence_frames = s_speaker_ctx.gap_silence_frames;
    startup_buffer_wait_ms = s_speaker_ctx.startup_buffer_wait_ms;
    rebuffer_events = s_speaker_ctx.jitter_rebuffer_events;
    rebuffer_wait_ms = s_speaker_ctx.jitter_rebuffer_wait_ms;
    rebuffer_max_wait_ms = s_speaker_ctx.jitter_rebuffer_max_wait_ms;
    low_watermark_ms = s_speaker_ctx.jitter_low_watermark_ms;
    tail_wait_events = s_speaker_ctx.jitter_tail_wait_events;
    tail_wait_ms = s_speaker_ctx.jitter_tail_wait_ms;
    max_ingress_gap_ms = s_speaker_ctx.ingress_max_gap_ms;
    startup_target_ms = s_speaker_ctx.jitter_startup_target_ms;
    resume_target_ms = s_speaker_ctx.jitter_resume_target_ms;
    queue_drops = s_speaker_ctx.queue_dropped_frames;
    completion_claimed = generation_current && queue_waiting == 0 &&
                         nuwax_speaker_finish_claim_allowed(
                             s_speaker_ctx.submitted_generation,
                             s_speaker_ctx.playback_generation,
                             s_speaker_ctx.drop_until_end,
                             s_speaker_ctx.playback_active,
                             s_speaker_ctx.completion_claim_generation,
                             generation);
    strlcpy(turn_id, s_speaker_ctx.active_turn_id, sizeof(turn_id));
    strlcpy(playback_id, s_speaker_ctx.active_playback_id, sizeof(playback_id));
    if (completion_claimed) {
        s_speaker_ctx.completion_claim_generation = generation;
    }
    speaker_unlock();

    if (post_end_frames_dropped > 0U) {
        ESP_LOGW(TAG,
                 "speaker dropped frames received after END generation=%lu end_seq=%u frames=%lu",
                 (unsigned long)generation,
                 (unsigned)sequence,
                 (unsigned long)post_end_frames_dropped);
    }
    if (generation_current && s_speaker_ctx.callbacks.on_stream_end) {
        s_speaker_ctx.callbacks.on_stream_end(connection_epoch, generation);
    }

    esp_err_t stop_err = ESP_ERR_INVALID_STATE;
    int64_t stop_completed_us = 0;
    bool completion_committed = false;
    bool cancel_requested = false;
    if (completion_claimed) {
        stop_err =
            speaker_drain_idle_output(reason ? reason : "playback_done");
        stop_completed_us = esp_timer_get_time();
        speaker_lock();
        const bool completion_still_current =
            nuwax_speaker_completion_owner_can_release(
                s_speaker_ctx.completion_claim_generation,
                generation) &&
            generation == s_speaker_ctx.submitted_generation &&
            generation == s_speaker_ctx.playback_generation &&
            connection_epoch ==
                s_speaker_ctx.submitted_connection_epoch &&
            connection_epoch ==
                s_speaker_ctx.playback_connection_epoch &&
            !s_speaker_ctx.drop_until_end &&
            !s_speaker_ctx.stream_protocol_faulted;
        if (completion_still_current) {
            if (stop_err == ESP_OK) {
                s_speaker_ctx.completed_stop_generation = generation;
                s_speaker_ctx.completed_stop_connection_epoch =
                    connection_epoch;
                s_speaker_ctx.completed_stop_timestamp_us =
                    stop_completed_us;
                s_speaker_ctx.completed_stop_result = ESP_OK;
                speaker_clear_stop_failure_locked(connection_epoch,
                                                  generation);
            } else {
                speaker_publish_stop_failure_locked(connection_epoch,
                                                    generation,
                                                    stop_completed_us,
                                                    stop_err);
            }
            cancel_requested =
                s_speaker_ctx.cancel_request_generation == generation &&
                s_speaker_ctx.cancel_request_connection_epoch ==
                    connection_epoch;
            s_speaker_ctx.completed_stop_cancel_requested =
                cancel_requested;
            if (s_speaker_ctx.fade_cancel_generation == generation) {
                s_speaker_ctx.fade_cancel_active = false;
                s_speaker_ctx.fade_cancel_generation = 0U;
                s_speaker_ctx.fade_cancel_total_samples = 0U;
                s_speaker_ctx.fade_cancel_processed_samples = 0U;
                s_speaker_ctx.fade_cancel_deadline_ms = 0U;
                s_speaker_ctx.fade_cancel_source[0] = '\0';
            }
            s_speaker_ctx.playback_active = false;
            s_speaker_ctx.playback_session_active = false;
            s_speaker_ctx.playback_connection_epoch = 0U;
            if (s_speaker_ctx.speaking_state_generation == generation) {
                s_speaker_ctx.speaking_state_generation = 0U;
            }
            speaker_clear_start_gate_locked();
            /* playback_begin metadata belongs to exactly one generation. The
             * accepted stream keeps its own totals for post-completion stats,
             * but a later START must begin with unknown totals until its own
             * metadata arrives. */
            s_speaker_ctx.metadata_expected_frames = 0U;
            s_speaker_ctx.metadata_expected_bytes = 0U;
            s_speaker_ctx.active_owner = NUWAX_SPEAKER_PLAYBACK_OWNER_NONE;
            s_speaker_ctx.active_turn_id[0] = '\0';
            s_speaker_ctx.active_notify_id[0] = '\0';
            s_speaker_ctx.active_playback_id[0] = '\0';
            s_speaker_ctx.completion_claim_generation = 0;
            s_speaker_ctx.jitter_startup_target_ms =
                nuwax_speaker_jitter_adapt_startup_ms(
                    s_speaker_ctx.jitter_startup_target_ms,
                    s_speaker_ctx.jitter_rebuffer_events > 0U,
                    s_speaker_ctx.jitter_rebuffer_max_wait_ms,
                    NUWAX_SPEAKER_JITTER_STARTUP_MIN_MS,
                    NUWAX_SPEAKER_JITTER_STARTUP_BASE_MS,
                    NUWAX_SPEAKER_JITTER_STARTUP_MAX_MS,
                    &s_speaker_ctx.jitter_recent_instability_history,
                    &s_speaker_ctx.jitter_stable_playbacks);
            next_startup_target_ms =
                s_speaker_ctx.jitter_startup_target_ms;
            s_speaker_ctx.jitter_last_playback_completed_ms = monotonic_ms();
            completion_committed = true;
        } else if (s_speaker_ctx.completion_claim_generation == generation) {
            /* A protocol fault can race with the physical stop after END was
             * dequeued. Do not publish playback_done for the now-faulted
             * generation; release the claim so its pending cancel can own the
             * terminal transition. */
            s_speaker_ctx.completion_claim_generation = 0U;
        }
        speaker_unlock();
    }

    ESP_LOGI(TAG,
             "speaker playback complete turn=%s playback=%s seq=%u generation=%lu submitted=%lu active=%lu claimed=%d committed=%d stop_result=%s queued=%lu played=%lu startup_target_ms=%lu next_startup_target_ms=%lu resume_target_ms=%lu startup_wait_ms=%lu low_watermark_ms=%lu rebuffer_events=%lu rebuffer_wait_ms=%lu rebuffer_max_wait_ms=%lu tail_wait_events=%lu tail_wait_ms=%lu max_ingress_gap_ms=%lu gap_silence=%lu write_failures=%lu queue_drops=%lu queue_waiting=%lu reason=%s",
             turn_id[0] ? turn_id : "-",
             playback_id[0] ? playback_id : "-",
             (unsigned)sequence,
             (unsigned long)generation,
             (unsigned long)submitted_generation_value,
             (unsigned long)current_generation_value,
             completion_claimed ? 1 : 0,
             completion_committed ? 1 : 0,
             esp_err_to_name(stop_err),
             (unsigned long)queued,
             (unsigned long)played,
             (unsigned long)startup_target_ms,
             (unsigned long)next_startup_target_ms,
             (unsigned long)resume_target_ms,
             (unsigned long)startup_buffer_wait_ms,
             (unsigned long)(low_watermark_ms == UINT32_MAX
                                 ? 0U
                                 : low_watermark_ms),
             (unsigned long)rebuffer_events,
             (unsigned long)rebuffer_wait_ms,
             (unsigned long)rebuffer_max_wait_ms,
             (unsigned long)tail_wait_events,
             (unsigned long)tail_wait_ms,
             (unsigned long)max_ingress_gap_ms,
             (unsigned long)gap_silence_frames,
             (unsigned long)failures,
             (unsigned long)queue_drops,
             (unsigned long)queue_waiting,
             reason ? reason : "-");
    if (!completion_committed) {
        ESP_LOGI(TAG,
                 "speaker playback complete ignored for follow-up claimed=%d queue_waiting=%lu",
                 completion_claimed ? 1 : 0,
                 (unsigned long)queue_waiting);
        return;
    }
    if (cancel_requested) {
        if (stop_err == ESP_OK) {
            speaker_notify_cancel_recovery_once(
                connection_epoch,
                generation,
                reason ? reason : "natural_completion_after_cancel");
        } else {
            speaker_notify_playback_fault(connection_epoch,
                                          generation,
                                          stop_err);
        }
        ESP_LOGI(TAG,
                 "speaker natural completion suppressing follow-up after cancel request generation=%lu stop_result=%s",
                 (unsigned long)generation,
                 esp_err_to_name(stop_err));
        return;
    }
    if (stop_err != ESP_OK) {
        speaker_notify_playback_fault(connection_epoch, generation, stop_err);
        ESP_LOGE(TAG,
                 "speaker playback completion held in stop fault generation=%lu result=%s",
                 (unsigned long)generation,
                 esp_err_to_name(stop_err));
        return;
    }
    if (s_speaker_ctx.callbacks.on_playback_done) {
        s_speaker_ctx.callbacks.on_playback_done(connection_epoch,
                                                 generation,
                                                 played,
                                                 failures);
    }
    bool scheduled = false;
    if (s_speaker_ctx.callbacks.schedule_auto_followup_after_playback) {
        scheduled = s_speaker_ctx.callbacks.schedule_auto_followup_after_playback(
            connection_epoch,
            generation);
    }
    if (!scheduled && s_speaker_ctx.callbacks.set_state) {
        s_speaker_ctx.callbacks.set_state(connection_epoch,
                                          generation,
                                          NUWAX_APP_STATE_IDLE);
    }
}

static esp_err_t speaker_enqueue_packet(uint32_t generation,
                                        uint32_t connection_epoch,
                                        uint32_t admission_epoch,
                                        uint32_t sequence,
                                        uint8_t flags,
                                        uint16_t sample_rate,
                                        uint8_t channels,
                                        const int16_t *samples,
                                        size_t sample_count,
                                        int64_t start_handler_entry_us,
                                        uint32_t admission_timeout_ms) {
    const bool control_end_marker = sample_count == 0 && (flags & NUWAX_AUDIO_FLAG_END) != 0;
    if (!s_speaker_ctx.packets || (!samples && !control_end_marker)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sample_count > CONFIG_NUWAX_VOICE_MAX_SAMPLES_PER_FRAME) {
        return ESP_ERR_INVALID_SIZE;
    }
    const size_t packet_bytes = speaker_packet_storage_size(sample_count);

    speaker_playback_packet_t packet = {
        .connection_epoch = connection_epoch,
        .generation = generation,
        .sequence = sequence,
        .sample_rate = sample_rate,
        .channels = channels,
        .flags = flags,
        .sample_count = sample_count,
        .start_handler_entry_us = start_handler_entry_us,
    };
    if (sample_count > 0) {
        memcpy(packet.samples, samples, sample_count * sizeof(int16_t));
    }

    const uint32_t wait_started_ms = monotonic_ms();
    const uint32_t bounded_admission_timeout_ms =
        admission_timeout_ms < NUWAX_SPEAKER_BACKPRESSURE_TIMEOUT_MS
            ? admission_timeout_ms
            : NUWAX_SPEAKER_BACKPRESSURE_TIMEOUT_MS;
    const uint32_t wait_deadline_ms =
        wait_started_ms + bounded_admission_timeout_ms;
    uint32_t last_wait_log_ms = 0U;
    bool wait_logged = false;
    while (true) {
        bool generation_current = false;
        bool admission_current = false;
        bool queued = false;
        bool stream_faulted = false;
        bool commit_failed = false;
        bool publish_ingress_progress = false;
        uint32_t current_generation = 0U;
        uint32_t queue_waiting = 0U;
        nuwax_speaker_stream_result_t stream_result =
            NUWAX_SPEAKER_STREAM_INACTIVE;

        speaker_lock();
        generation_current =
            speaker_generation_source_matches_locked(connection_epoch,
                                                     generation) &&
            !s_speaker_ctx.drop_until_end;
        current_generation = s_speaker_ctx.submitted_generation;
        admission_current = admission_epoch == 0U ||
                            speaker_downlink_admission_matches_locked(
                                admission_epoch);
        stream_faulted = s_speaker_ctx.stream_protocol_faulted;
        if (generation_current && admission_current && !stream_faulted) {
            stream_result = nuwax_speaker_stream_validate(
                &s_speaker_ctx.stream,
                generation,
                sequence,
                flags,
                NUWAX_SPEAKER_ALLOWED_AUDIO_FLAGS,
                NUWAX_AUDIO_FLAG_START,
                NUWAX_AUDIO_FLAG_END,
                sample_count);
            if (stream_result == NUWAX_SPEAKER_STREAM_OK) {
                void *ring_item = NULL;
                if (xRingbufferSendAcquire(s_speaker_ctx.packets,
                                           &ring_item,
                                           packet_bytes,
                                           0) == pdTRUE) {
                    packet.enqueue_us = start_handler_entry_us > 0
                                            ? esp_timer_get_time()
                                            : 0;
                    memcpy(ring_item, &packet, packet_bytes);
                    atomic_fetch_add_explicit(&s_speaker_ctx.packet_count,
                                              1U,
                                              memory_order_release);
                    atomic_fetch_add_explicit(&s_speaker_ctx.queued_samples,
                                              (uint32_t)sample_count,
                                              memory_order_release);
                    queued = xRingbufferSendComplete(s_speaker_ctx.packets,
                                                     ring_item) == pdTRUE;
                    if (queued) {
                        /* Publish sequence ownership only after the compact PCM
                         * record is visible to the playback consumer. */
                        nuwax_speaker_stream_commit(&s_speaker_ctx.stream,
                                                    sequence,
                                                    flags,
                                                    NUWAX_AUDIO_FLAG_END,
                                                    sample_count);
                        if (sample_count > 0U) {
                            const uint32_t ingress_now_ms = monotonic_ms();
                            if (s_speaker_ctx.ingress_last_pcm_rx_ms != 0U) {
                                const uint32_t gap_ms =
                                    ingress_now_ms -
                                    s_speaker_ctx.ingress_last_pcm_rx_ms;
                                if (gap_ms >
                                    s_speaker_ctx.ingress_max_gap_ms) {
                                    s_speaker_ctx.ingress_max_gap_ms = gap_ms;
                                }
                            }
                            s_speaker_ctx.ingress_last_pcm_rx_ms = ingress_now_ms;
                            if (s_speaker_ctx.callbacks.on_ingress_progress &&
                                (s_speaker_ctx.ingress_progress_callback_ms ==
                                     0U ||
                                 elapsed_ms(
                                     ingress_now_ms,
                                     s_speaker_ctx.ingress_progress_callback_ms +
                                         NUWAX_SPEAKER_INGRESS_CALLBACK_INTERVAL_MS))) {
                                s_speaker_ctx.ingress_progress_callback_ms =
                                    ingress_now_ms;
                                publish_ingress_progress = true;
                            }
                            if (s_speaker_ctx.first_pcm_rx_us == 0) {
                                s_speaker_ctx.first_pcm_rx_us =
                                    packet.start_handler_entry_us > 0
                                        ? packet.start_handler_entry_us
                                        : esp_timer_get_time();
                            }
                        }
                    } else {
                        speaker_packet_ring_note_removed((uint32_t)sample_count);
                        commit_failed = true;
                    }
                }
                queue_waiting = speaker_packet_ring_count();
            }
        }
        speaker_unlock();

        if (!generation_current || !admission_current) {
            ESP_LOGI(TAG,
                     "speaker rejected stale/admission packet seq=%u generation=%lu current=%lu admission_epoch=%lu",
                     (unsigned)sequence,
                     (unsigned long)generation,
                     (unsigned long)current_generation,
                     (unsigned long)admission_epoch);
            return ESP_ERR_INVALID_STATE;
        }
        if (stream_faulted) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (stream_result != NUWAX_SPEAKER_STREAM_OK) {
            ESP_LOGE(TAG,
                     "speaker rejected downlink protocol seq=%u generation=%lu flags=0x%02x samples=%u reason=%s",
                     (unsigned)sequence,
                     (unsigned long)generation,
                     flags,
                     (unsigned)sample_count,
                     speaker_stream_result_name(stream_result));
            speaker_schedule_protocol_fault(connection_epoch,
                                            generation,
                                            ESP_ERR_INVALID_RESPONSE,
                                            speaker_stream_result_name(
                                                stream_result),
                                            false);
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (commit_failed) {
            ESP_LOGE(TAG,
                     "speaker ingress ring commit failed seq=%u generation=%lu",
                     (unsigned)sequence,
                     (unsigned long)generation);
            speaker_schedule_protocol_fault(connection_epoch,
                                            generation,
                                            ESP_FAIL,
                                            "ingress_commit_failed",
                                            false);
            return ESP_FAIL;
        }
        if (queued) {
            TaskHandle_t playback_task = NULL;
            speaker_lock();
            playback_task = s_speaker_ctx.playback_task;
            speaker_unlock();
            if (playback_task) {
                xTaskNotifyGive(playback_task);
            }
            if (publish_ingress_progress &&
                s_speaker_ctx.callbacks.on_ingress_progress) {
                s_speaker_ctx.callbacks.on_ingress_progress(connection_epoch,
                                                             generation);
            }
            const uint32_t waited_ms = monotonic_ms() - wait_started_ms;
            if (wait_logged) {
                ESP_LOGI(TAG,
                         "speaker ingress backpressure recovered seq=%u waited_ms=%lu queue_waiting=%lu",
                         (unsigned)sequence,
                         (unsigned long)waited_ms,
                         (unsigned long)queue_waiting);
            }
            return ESP_OK;
        }

        const uint32_t now_ms = monotonic_ms();
        const uint32_t waited_ms = now_ms - wait_started_ms;
        if (nuwax_speaker_backpressure_deadline_reached(
                now_ms, wait_deadline_ms)) {
            ESP_LOGE(TAG,
                     "speaker ingress backpressure timed out seq=%u generation=%lu waited_ms=%lu queue_waiting=%lu; aborting generation while retaining WebSocket",
                     (unsigned)sequence,
                     (unsigned long)generation,
                     (unsigned long)waited_ms,
                     (unsigned long)queue_waiting);
            speaker_schedule_protocol_fault(connection_epoch,
                                            generation,
                                            ESP_ERR_TIMEOUT,
                                            "ingress_backpressure_timeout",
                                            false);
            return ESP_ERR_TIMEOUT;
        }
        if (!wait_logged ||
            waited_ms - last_wait_log_ms >=
                NUWAX_SPEAKER_BACKPRESSURE_LOG_MS) {
            ESP_LOGW(TAG,
                     "speaker ingress backpressure seq=%u generation=%lu waited_ms=%lu queue_waiting=%lu max_item_free=%u; preserving PCM",
                     (unsigned)sequence,
                     (unsigned long)generation,
                     (unsigned long)waited_ms,
                     (unsigned long)queue_waiting,
                     (unsigned)xRingbufferGetCurFreeSize(
                         s_speaker_ctx.packets));
            wait_logged = true;
            last_wait_log_ms = waited_ms;
        }
        const uint32_t poll_wait_ms =
            nuwax_speaker_backpressure_poll_wait_ms(
                now_ms,
                wait_deadline_ms,
                NUWAX_SPEAKER_BACKPRESSURE_POLL_MS);
        vTaskDelay(speaker_positive_delay_ticks(poll_wait_ms));
    }
}

esp_err_t nuwax_speaker_playback_init(const nuwax_speaker_playback_callbacks_t *callbacks) {
    if (callbacks) {
        s_speaker_ctx.callbacks = *callbacks;
    } else {
        memset(&s_speaker_ctx.callbacks, 0, sizeof(s_speaker_ctx.callbacks));
    }
    s_speaker_ctx.volume_percent = load_speaker_volume_percent();
    s_speaker_ctx.volume_generation = 1U;
    s_speaker_ctx.output_volume_sync_pending = true;
    s_speaker_ctx.keep_initialized = true;
    s_speaker_ctx.jitter_startup_target_ms =
        NUWAX_SPEAKER_JITTER_STARTUP_BASE_MS;
    s_speaker_ctx.jitter_resume_target_ms =
        NUWAX_SPEAKER_JITTER_RESUME_MIN_MS;
    s_speaker_ctx.jitter_recent_instability_history = 0U;
    s_speaker_ctx.jitter_stable_playbacks = 0U;
    s_speaker_ctx.jitter_resume_fade_pending = false;
    s_speaker_ctx.downlink_admission_allowed = false;
    s_speaker_ctx.downlink_admission_epoch = 0U;
    s_speaker_ctx.completed_stop_result = ESP_ERR_INVALID_STATE;
    s_speaker_ctx.completed_stop_connection_epoch = 0U;
    s_speaker_ctx.failed_stop_result = ESP_ERR_INVALID_STATE;
    s_speaker_ctx.failed_stop_connection_epoch = 0U;
    s_speaker_ctx.start_gate_pending = false;
    s_speaker_ctx.start_gate_authorized = false;
    s_speaker_ctx.start_gate_generation = 0U;
    s_speaker_ctx.start_gate_connection_epoch = 0U;
    s_speaker_ctx.start_gate_deadline_ms = 0U;
    s_speaker_ctx.speaking_state_generation = 0U;
    s_speaker_ctx.cancel_recovery_notified_generation = 0U;
    s_speaker_ctx.cancel_recovery_notified_connection_epoch = 0U;
    speaker_clear_end_seen_locked();
    speaker_reset_stream_runtime_locked(true);
    s_speaker_ctx.stream_stall_faults = 0U;
    s_speaker_ctx.stream_protocol_faults = 0U;
    atomic_store_explicit(&s_speaker_ctx.packet_count,
                          0U,
                          memory_order_release);
    atomic_store_explicit(&s_speaker_ctx.queued_samples,
                          0U,
                          memory_order_release);
    s_speaker_ctx.playback_task = NULL;
    s_speaker_task_packet = heap_caps_calloc(
        1U, sizeof(*s_speaker_task_packet),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_speaker_task_rebuffer_ref = heap_caps_calloc(
        1U, sizeof(*s_speaker_task_rebuffer_ref),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_speaker_task_packet || !s_speaker_task_rebuffer_ref) {
        ESP_LOGE(TAG,
                 "speaker PSRAM task packet allocation failed packet=%p rebuffer=%p free_psram=%u largest_psram=%u",
                 (void *)s_speaker_task_packet,
                 (void *)s_speaker_task_rebuffer_ref,
                 (unsigned)heap_caps_get_free_size(
                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(
                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        speaker_release_task_buffers();
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG,
             "speaker PSRAM task packet storage ready packet_bytes=%u rebuffer_bytes=%u",
             (unsigned)sizeof(*s_speaker_task_packet),
             (unsigned)sizeof(*s_speaker_task_rebuffer_ref));
    s_speaker_ctx.packets = xRingbufferCreateWithCaps(
        NUWAX_SPEAKER_BUFFER_BYTES,
        RINGBUF_TYPE_NOSPLIT,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_speaker_ctx.packets) {
        ESP_LOGE(TAG,
                 "speaker PSRAM ring allocation failed bytes=%u max_record=%u free_psram=%u largest_psram=%u",
                 (unsigned)NUWAX_SPEAKER_BUFFER_BYTES,
                 (unsigned)speaker_packet_storage_size(
                     CONFIG_NUWAX_VOICE_MAX_SAMPLES_PER_FRAME),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        speaker_release_task_buffers();
        return ESP_ERR_NO_MEM;
    }
    const size_t max_record_bytes = speaker_packet_storage_size(
        CONFIG_NUWAX_VOICE_MAX_SAMPLES_PER_FRAME);
    if (xRingbufferGetMaxItemSize(s_speaker_ctx.packets) <
        max_record_bytes) {
        ESP_LOGE(TAG,
                 "speaker PSRAM ring cannot hold max record ring_bytes=%u max_item=%u required=%u",
                 (unsigned)NUWAX_SPEAKER_BUFFER_BYTES,
                 (unsigned)xRingbufferGetMaxItemSize(
                     s_speaker_ctx.packets),
                 (unsigned)max_record_bytes);
        vRingbufferDeleteWithCaps(s_speaker_ctx.packets);
        s_speaker_ctx.packets = NULL;
        speaker_release_task_buffers();
        return ESP_ERR_INVALID_SIZE;
    }
    s_speaker_ctx.mutex = xSemaphoreCreateMutex();
    if (!s_speaker_ctx.mutex) {
        ESP_LOGE(TAG, "failed to create speaker mutex");
        vRingbufferDeleteWithCaps(s_speaker_ctx.packets);
        s_speaker_ctx.packets = NULL;
        speaker_release_task_buffers();
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG,
             "speaker PSRAM compact ring ready bytes=%u max_record=%u poll_ms=%u",
             (unsigned)NUWAX_SPEAKER_BUFFER_BYTES,
             (unsigned)max_record_bytes,
             (unsigned)NUWAX_SPEAKER_BACKPRESSURE_POLL_MS);
    return ESP_OK;
}

void nuwax_speaker_playback_set_downlink_admission(uint32_t epoch,
                                                   bool allowed) {
    speaker_lock();
    s_speaker_ctx.downlink_admission_epoch = epoch;
    s_speaker_ctx.downlink_admission_allowed = allowed && epoch != 0U;
    speaker_unlock();
}

void nuwax_speaker_playback_reset_jitter_learning(void) {
    speaker_lock();
    s_speaker_ctx.jitter_learning_reset_pending = true;
    if (!s_speaker_ctx.playback_active &&
        !s_speaker_ctx.start_gate_pending &&
        !s_speaker_ctx.start_gate_authorized &&
        speaker_packet_ring_count() == 0U) {
        speaker_reset_jitter_learning_locked(0U);
    }
    speaker_unlock();
}

bool nuwax_speaker_playback_has_pending(void) {
    bool active = false;
    uint32_t queue_waiting = 0U;
    speaker_lock();
    active = s_speaker_ctx.playback_active ||
             s_speaker_ctx.playback_session_active ||
             s_speaker_ctx.start_gate_pending ||
             s_speaker_ctx.start_gate_authorized ||
             s_speaker_ctx.fade_cancel_active ||
             s_speaker_ctx.stop_fault_active ||
             s_speaker_ctx.completion_claim_generation != 0U ||
              s_speaker_ctx.cancel_pending_generation != 0U ||
              s_speaker_ctx.output_stop_owner_generation != 0U;
    queue_waiting = speaker_packet_ring_count();
    speaker_unlock();
    return active || queue_waiting > 0U;
}

bool nuwax_speaker_playback_session_active(void) {
    bool active = false;
    speaker_lock();
    active = s_speaker_ctx.playback_session_active;
    speaker_unlock();
    return active;
}

bool nuwax_speaker_playback_output_ready(void) {
    bool ready;
    speaker_lock();
    ready = speaker_output_ready_locked();
    speaker_unlock();
    return ready;
}

void nuwax_speaker_playback_get_stats(nuwax_speaker_playback_stats_t *stats) {
    if (!stats) {
        return;
    }
    memset(stats, 0, sizeof(*stats));
    speaker_lock();
    stats->playback_active = s_speaker_ctx.playback_active;
    stats->playback_session_active = s_speaker_ctx.playback_session_active;
    stats->fade_cancel_active = s_speaker_ctx.fade_cancel_active;
    stats->submitted_generation = s_speaker_ctx.submitted_generation;
    stats->submitted_connection_epoch =
        s_speaker_ctx.submitted_connection_epoch;
    stats->playback_generation = s_speaker_ctx.playback_generation;
    stats->playback_connection_epoch =
        s_speaker_ctx.playback_connection_epoch;
    stats->fade_cancel_generation =
        s_speaker_ctx.fade_cancel_generation;
    stats->volume_generation = s_speaker_ctx.volume_generation;
    stats->volume_percent = s_speaker_ctx.volume_percent;
    stats->probe_duck_active = s_speaker_ctx.probe_duck_active;
    stats->probe_duck_generation = s_speaker_ctx.probe_duck_generation;
    stats->start_gate_pending = s_speaker_ctx.start_gate_pending;
    stats->start_gate_authorized = s_speaker_ctx.start_gate_authorized;
    stats->start_gate_generation = s_speaker_ctx.start_gate_generation;
    stats->start_gate_connection_epoch =
        s_speaker_ctx.start_gate_connection_epoch;
    stats->start_gate_deadline_ms = s_speaker_ctx.start_gate_deadline_ms;
    stats->queued_frames = s_speaker_ctx.queued_frames;
    stats->played_frames = s_speaker_ctx.played_frames;
    stats->queue_dropped_frames = s_speaker_ctx.queue_dropped_frames;
    stats->write_failures = s_speaker_ctx.write_failures;
    stats->played_bytes = s_speaker_ctx.played_bytes;
    stats->playback_last_progress_ms =
        s_speaker_ctx.playback_last_progress_ms;
    stats->jitter_rebuffering = s_speaker_ctx.jitter_rebuffering;
    stats->startup_target_ms = s_speaker_ctx.jitter_startup_target_ms;
    stats->startup_buffer_wait_ms = s_speaker_ctx.startup_buffer_wait_ms;
    stats->low_watermark_ms =
        s_speaker_ctx.jitter_low_watermark_ms == UINT32_MAX
            ? 0U
            : s_speaker_ctx.jitter_low_watermark_ms;
    stats->resume_target_ms = s_speaker_ctx.jitter_resume_target_ms;
    stats->rebuffer_events = s_speaker_ctx.jitter_rebuffer_events;
    stats->rebuffer_wait_ms = s_speaker_ctx.jitter_rebuffer_wait_ms;
    stats->rebuffer_max_wait_ms = s_speaker_ctx.jitter_rebuffer_max_wait_ms;
    stats->tail_wait_events = s_speaker_ctx.jitter_tail_wait_events;
    stats->tail_wait_ms = s_speaker_ctx.jitter_tail_wait_ms;
    stats->max_ingress_gap_ms = s_speaker_ctx.ingress_max_gap_ms;
    stats->stream_stall_faults = s_speaker_ctx.stream_stall_faults;
    stats->stream_protocol_faults =
        s_speaker_ctx.stream_protocol_faults;
    stats->rx_stream_active = s_speaker_ctx.stream.active;
    stats->rx_end_accepted = s_speaker_ctx.stream.end_accepted;
    stats->rx_next_sequence = s_speaker_ctx.stream.next_sequence;
    stats->rx_pcm_frames = s_speaker_ctx.stream.pcm_frames;
    stats->rx_pcm_bytes = s_speaker_ctx.stream.pcm_bytes;
    stats->rx_expected_frames = s_speaker_ctx.stream.expected_frames;
    stats->rx_expected_bytes = s_speaker_ctx.stream.expected_bytes;
    stats->aec_reference_required =
        s_speaker_ctx.output.software_ref_required;
    stats->aec_reference_ready =
        nuwax_speaker_output_software_ref_ready(&s_speaker_ctx.output);
    stats->queue_waiting = speaker_packet_ring_count();
    stats->completed_stop_generation =
        s_speaker_ctx.completed_stop_generation;
    stats->completed_stop_connection_epoch =
        s_speaker_ctx.completed_stop_connection_epoch;
    stats->completed_stop_timestamp_us =
        s_speaker_ctx.completed_stop_timestamp_us;
    stats->completed_stop_result = s_speaker_ctx.completed_stop_result;
    stats->stop_fault_active = s_speaker_ctx.stop_fault_active;
    stats->failed_stop_generation = s_speaker_ctx.failed_stop_generation;
    stats->failed_stop_connection_epoch =
        s_speaker_ctx.failed_stop_connection_epoch;
    stats->failed_stop_timestamp_us =
        s_speaker_ctx.failed_stop_timestamp_us;
    stats->failed_stop_result = s_speaker_ctx.failed_stop_result;
    stats->stop_in_progress =
        s_speaker_ctx.fade_cancel_active ||
        s_speaker_ctx.stop_fault_active ||
        s_speaker_ctx.completion_claim_generation != 0U ||
        s_speaker_ctx.cancel_pending_generation != 0U ||
        s_speaker_ctx.output_stop_owner_generation != 0U ||
        s_speaker_ctx.protocol_fault_pending_generation != 0U;
    stats->pending = s_speaker_ctx.playback_active ||
                     s_speaker_ctx.playback_session_active ||
                     s_speaker_ctx.start_gate_pending ||
                     s_speaker_ctx.start_gate_authorized ||
                     stats->queue_waiting > 0 ||
                     stats->stop_in_progress;
    stats->active_owner = s_speaker_ctx.active_owner;
    strlcpy(stats->active_turn_id, s_speaker_ctx.active_turn_id, sizeof(stats->active_turn_id));
    strlcpy(stats->active_notify_id, s_speaker_ctx.active_notify_id, sizeof(stats->active_notify_id));
    strlcpy(stats->active_playback_id, s_speaker_ctx.active_playback_id, sizeof(stats->active_playback_id));
    speaker_unlock();
}

typedef enum {
    SPEAKER_JOINED_STOP_COMPLETED = 0,
    SPEAKER_JOINED_STOP_FAILED,
    SPEAKER_JOINED_STOP_STILL_ACTIVE,
} speaker_joined_stop_result_t;

static speaker_joined_stop_result_t speaker_wait_for_joined_stop(
    uint32_t stopped_connection_epoch,
    uint32_t stopped_generation,
    const char *source) {
    const uint32_t deadline_ms =
        monotonic_ms() + NUWAX_SPEAKER_CLAIM_WAIT_TIMEOUT_MS;
    while (true) {
        bool completed = false;
        bool failed = false;
        bool transaction_active = false;
        speaker_lock();
        completed =
            speaker_stop_completion_matches_locked(stopped_connection_epoch,
                                                   stopped_generation);
        failed = speaker_stop_failure_matches_locked(stopped_connection_epoch,
                                                     stopped_generation);
        transaction_active =
            s_speaker_ctx.completion_claim_generation ==
                stopped_generation ||
            (s_speaker_ctx.cancel_stopped_generation ==
                 stopped_generation &&
             (s_speaker_ctx.cancel_pending_generation != 0U ||
              s_speaker_ctx.output_stop_owner_generation != 0U));
        speaker_unlock();
        if (completed) {
            bool cancel_terminal = false;
            speaker_lock();
            cancel_terminal =
                speaker_stop_completion_matches_locked(
                    stopped_connection_epoch,
                    stopped_generation) &&
                s_speaker_ctx.completed_stop_cancel_requested;
            speaker_unlock();
            if (cancel_terminal) {
                speaker_notify_cancel_recovery_once(
                    stopped_connection_epoch,
                    stopped_generation,
                    source ? source : "joined_stop_complete");
            }
            return SPEAKER_JOINED_STOP_COMPLETED;
        }
        if (failed) {
            return SPEAKER_JOINED_STOP_FAILED;
        }
        if (!transaction_active) {
            speaker_lock();
            if (!speaker_stop_completion_matches_locked(
                    stopped_connection_epoch,
                    stopped_generation) &&
                !speaker_stop_failure_matches_locked(
                    stopped_connection_epoch,
                    stopped_generation)) {
                speaker_publish_stop_failure_locked(
                    stopped_connection_epoch,
                    stopped_generation,
                    esp_timer_get_time(),
                    ESP_ERR_INVALID_STATE);
            }
            speaker_unlock();
            ESP_LOGE(TAG,
                     "speaker joined stop lost active owner without terminal result source=%s stopped_generation=%lu",
                     source ? source : "-",
                     (unsigned long)stopped_generation);
            return SPEAKER_JOINED_STOP_FAILED;
        }
        if (elapsed_ms(monotonic_ms(), deadline_ms)) {
            ESP_LOGW(TAG,
                     "speaker joined stop still in progress source=%s stopped_generation=%lu timeout_ms=%lu",
                     source ? source : "-",
                     (unsigned long)stopped_generation,
                     (unsigned long)NUWAX_SPEAKER_CLAIM_WAIT_TIMEOUT_MS);
            return SPEAKER_JOINED_STOP_STILL_ACTIVE;
        }
        vTaskDelay(speaker_positive_delay_ticks(1));
    }
}

static bool speaker_cancel_now_if_current(uint32_t expected_generation,
                                          const char *source) {
    return speaker_cancel_now_if_current_epoch(0U,
                                               expected_generation,
                                               source);
}

static bool speaker_cancel_now_if_current_epoch(
    uint32_t expected_connection_epoch,
    uint32_t expected_generation,
    const char *source) {
    const uint32_t started_ms = monotonic_ms();
    uint32_t queued_before = 0U;
    bool had_active_playback = false;
    uint32_t drop_window_ms = NUWAX_SPEAKER_CANCEL_DROP_GRACE_MS;
    char stale_turn_id[sizeof(s_speaker_ctx.stale_turn_id)] = {0};
    char stale_playback_id[sizeof(s_speaker_ctx.stale_playback_id)] = {0};
    uint32_t stale_sessions = 0;
    uint32_t cancel_generation = 0;
    uint32_t stopped_playback_generation = 0;
    uint32_t stopped_connection_epoch = 0U;
    uint32_t joined_stop_generation = 0;
    uint32_t joined_stop_connection_epoch = 0U;
    uint32_t cleared_frames = 0;
    bool retry_failed_cancel = false;
    speaker_lock();
    queued_before = speaker_packet_ring_count();
    had_active_playback = s_speaker_ctx.playback_active ||
                          s_speaker_ctx.playback_session_active ||
                          s_speaker_ctx.start_gate_pending ||
                          s_speaker_ctx.start_gate_authorized ||
                          s_speaker_ctx.fade_cancel_active ||
                          s_speaker_ctx.stop_fault_active ||
                          s_speaker_ctx.completion_claim_generation != 0U ||
                           s_speaker_ctx.cancel_pending_generation != 0U ||
                           s_speaker_ctx.output_stop_owner_generation != 0U ||
                           queued_before > 0U;
    drop_window_ms = had_active_playback
                         ? NUWAX_SPEAKER_CANCEL_DROP_ACTIVE_MS
                         : NUWAX_SPEAKER_CANCEL_DROP_GRACE_MS;
    if (expected_generation == 0U && s_speaker_ctx.stop_fault_active &&
        s_speaker_ctx.failed_stop_generation != 0U &&
        s_speaker_ctx.failed_stop_generation ==
            s_speaker_ctx.cancel_stopped_generation &&
        s_speaker_ctx.submitted_generation ==
            s_speaker_ctx.playback_generation &&
        s_speaker_ctx.drop_until_end) {
        retry_failed_cancel = true;
        stopped_playback_generation =
            s_speaker_ctx.failed_stop_generation;
        stopped_connection_epoch =
            s_speaker_ctx.failed_stop_connection_epoch;
        cancel_generation = s_speaker_ctx.submitted_generation;
        s_speaker_ctx.cancel_pending_generation = cancel_generation;
    }
    if (expected_generation == 0U && !retry_failed_cancel) {
        joined_stop_generation =
            nuwax_speaker_unconditional_cancel_join_target(
                s_speaker_ctx.completion_claim_generation,
                s_speaker_ctx.cancel_pending_generation,
                s_speaker_ctx.output_stop_owner_generation,
                s_speaker_ctx.cancel_stopped_generation);
        if (joined_stop_generation == 0U &&
            speaker_stop_completion_matches_locked(
                s_speaker_ctx.submitted_connection_epoch,
                s_speaker_ctx.submitted_generation)) {
            joined_stop_generation = s_speaker_ctx.submitted_generation;
        }
        if (joined_stop_generation == 0U &&
            s_speaker_ctx.cancel_stopped_generation != 0U &&
            speaker_stop_completion_matches_locked(
                s_speaker_ctx.cancel_stopped_connection_epoch,
                s_speaker_ctx.cancel_stopped_generation)) {
            joined_stop_generation =
                s_speaker_ctx.cancel_stopped_generation;
        }
        if (joined_stop_generation != 0U) {
            if (s_speaker_ctx.completed_stop_generation ==
                    joined_stop_generation) {
                joined_stop_connection_epoch =
                    s_speaker_ctx.completed_stop_connection_epoch;
            } else if (s_speaker_ctx.completion_claim_generation ==
                           joined_stop_generation) {
                joined_stop_connection_epoch =
                    s_speaker_ctx.playback_connection_epoch;
            } else {
                joined_stop_connection_epoch =
                    s_speaker_ctx.cancel_stopped_connection_epoch;
            }
            /* A cancel that joins the END/completion owner must be visible to
             * that owner before it commits its terminal callback. Otherwise
             * normal PLAYBACK_DONE and cancel recovery can both escape for the
             * same generation. */
            if (s_speaker_ctx.completion_claim_generation ==
                    joined_stop_generation &&
                (s_speaker_ctx.cancel_request_generation !=
                     joined_stop_generation ||
                 s_speaker_ctx.cancel_request_connection_epoch !=
                     joined_stop_connection_epoch)) {
                s_speaker_ctx.cancel_request_generation =
                    joined_stop_generation;
                s_speaker_ctx.cancel_request_connection_epoch =
                    joined_stop_connection_epoch;
                s_speaker_ctx.cancel_request_timestamp_us =
                    esp_timer_get_time();
            }
        }
    }
    if (joined_stop_generation != 0U) {
        speaker_unlock();
        ESP_LOGI(TAG,
                 "speaker unconditional cancel joined existing stop source=%s stopped_generation=%lu",
                 source ? source : "-",
                 (unsigned long)joined_stop_generation);
        const speaker_joined_stop_result_t join_result =
            speaker_wait_for_joined_stop(joined_stop_connection_epoch,
                                         joined_stop_generation,
                                         source);
        if (join_result == SPEAKER_JOINED_STOP_COMPLETED) {
            return true;
        }
        if (join_result == SPEAKER_JOINED_STOP_STILL_ACTIVE) {
            ESP_LOGW(TAG,
                     "speaker unconditional cancel joined stop still active source=%s stopped_generation=%lu",
                     source ? source : "-",
                     (unsigned long)joined_stop_generation);
            return false;
        }
        ESP_LOGW(TAG,
                 "speaker unconditional cancel retrying joined failed stop source=%s stopped_generation=%lu",
                 source ? source : "-",
                 (unsigned long)joined_stop_generation);
        return speaker_cancel_now_if_current(0U, source);
    }
    if (retry_failed_cancel) {
        speaker_unlock();
        ESP_LOGW(TAG,
                 "speaker unconditional cancel retrying failed stop source=%s cancel_generation=%lu stopped_generation=%lu",
                 source ? source : "-",
                 (unsigned long)cancel_generation,
                 (unsigned long)stopped_playback_generation);
    } else {
      if (expected_generation != 0 &&
        !nuwax_speaker_cancel_expected_matches(
            s_speaker_ctx.submitted_generation,
            s_speaker_ctx.drop_until_end,
            expected_generation)) {
        const uint32_t submitted_generation =
            s_speaker_ctx.submitted_generation;
        const uint32_t playback_generation =
            s_speaker_ctx.playback_generation;
        speaker_unlock();
        ESP_LOGI(TAG,
                 "speaker generation-bound cancel skipped source=%s expected=%lu submitted=%lu active=%lu",
                 source ? source : "-",
                 (unsigned long)expected_generation,
                 (unsigned long)submitted_generation,
                 (unsigned long)playback_generation);
        return false;
      }
      if (expected_connection_epoch != 0U &&
          s_speaker_ctx.submitted_connection_epoch !=
              expected_connection_epoch) {
        const uint32_t submitted_connection_epoch =
            s_speaker_ctx.submitted_connection_epoch;
        speaker_unlock();
        ESP_LOGI(TAG,
                 "speaker epoch-bound cancel skipped source=%s expected_epoch=%lu active_epoch=%lu generation=%lu",
                 source ? source : "-",
                 (unsigned long)expected_connection_epoch,
                 (unsigned long)submitted_connection_epoch,
                 (unsigned long)expected_generation);
        return false;
      }
    if (s_speaker_ctx.active_turn_id[0] != '\0' || s_speaker_ctx.active_playback_id[0] != '\0') {
        strlcpy(s_speaker_ctx.stale_turn_id, s_speaker_ctx.active_turn_id, sizeof(s_speaker_ctx.stale_turn_id));
        strlcpy(s_speaker_ctx.stale_playback_id,
                s_speaker_ctx.active_playback_id,
                sizeof(s_speaker_ctx.stale_playback_id));
        s_speaker_ctx.stale_playback_sessions_dropped++;
    }
    strlcpy(stale_turn_id, s_speaker_ctx.stale_turn_id, sizeof(stale_turn_id));
    strlcpy(stale_playback_id, s_speaker_ctx.stale_playback_id, sizeof(stale_playback_id));
    stale_sessions = s_speaker_ctx.stale_playback_sessions_dropped;
    s_speaker_ctx.drop_until_end = true;
    s_speaker_ctx.dropped_frames = 0;
    s_speaker_ctx.drop_deadline_ms = monotonic_ms() + drop_window_ms;
    s_speaker_ctx.playback_active = false;
    s_speaker_ctx.playback_session_active = false;
    s_speaker_ctx.speaking_state_generation = 0U;
    if (s_speaker_ctx.start_gate_generation == stopped_playback_generation ||
        s_speaker_ctx.start_gate_generation == s_speaker_ctx.submitted_generation) {
        speaker_clear_start_gate_locked();
    }
    s_speaker_ctx.fade_cancel_active = false;
    s_speaker_ctx.fade_cancel_generation = 0;
    s_speaker_ctx.fade_cancel_total_samples = 0;
    s_speaker_ctx.fade_cancel_processed_samples = 0;
    s_speaker_ctx.fade_cancel_deadline_ms = 0;
    s_speaker_ctx.fade_cancel_source[0] = '\0';
    (void)speaker_clear_probe_duck_locked(false);
    stopped_playback_generation = s_speaker_ctx.submitted_generation;
    stopped_connection_epoch =
        s_speaker_ctx.submitted_connection_epoch;
    s_speaker_ctx.cancel_stopped_generation =
        stopped_playback_generation;
    s_speaker_ctx.cancel_stopped_connection_epoch =
        stopped_connection_epoch;
    if (s_speaker_ctx.cancel_request_generation !=
        stopped_playback_generation) {
        s_speaker_ctx.cancel_request_generation =
            stopped_playback_generation;
        s_speaker_ctx.cancel_request_connection_epoch =
            stopped_connection_epoch;
        s_speaker_ctx.cancel_request_timestamp_us = esp_timer_get_time();
    }
    s_speaker_ctx.submitted_generation =
        speaker_next_generation(s_speaker_ctx.submitted_generation);
    s_speaker_ctx.submitted_connection_epoch = 0U;
    s_speaker_ctx.playback_generation = s_speaker_ctx.submitted_generation;
    s_speaker_ctx.playback_connection_epoch = 0U;
    speaker_clear_end_seen_locked();
    speaker_reset_stream_runtime_locked(true);
    cancel_generation = s_speaker_ctx.submitted_generation;
    s_speaker_ctx.cancel_pending_generation = cancel_generation;
    if (s_speaker_ctx.packets) {
        cleared_frames = speaker_packet_ring_clear_locked();
    }
      speaker_unlock();
    }

    if (!retry_failed_cancel && cleared_frames > 0) {
        ESP_LOGI(TAG,
                 "speaker playback queue cleared frames=%lu reason=cancel",
                 (unsigned long)cleared_frames);
    }

    uint32_t last_wait_log_ms = started_ms;
    const uint32_t claim_wait_deadline_ms =
        started_ms + NUWAX_SPEAKER_CLAIM_WAIT_TIMEOUT_MS;
    bool claim_wait_timed_out = false;
    while (true) {
        uint32_t io_claim_generation = 0;
        uint32_t completion_claim_generation = 0;
        uint32_t maintenance_claim_generation = 0;
        speaker_lock();
        io_claim_generation = s_speaker_ctx.io_claim_generation;
        completion_claim_generation = s_speaker_ctx.completion_claim_generation;
        maintenance_claim_generation = s_speaker_ctx.maintenance_claim_generation;
        speaker_unlock();
        if (io_claim_generation == 0 &&
            completion_claim_generation == 0 &&
            maintenance_claim_generation == 0) {
            break;
        }
        const uint32_t now_ms = monotonic_ms();
        if (elapsed_ms(now_ms, claim_wait_deadline_ms)) {
            claim_wait_timed_out = true;
            break;
        }
        if ((uint32_t)(now_ms - last_wait_log_ms) >= 1000U) {
            ESP_LOGW(TAG,
                     "speaker cancel waiting for action claim cancel_generation=%lu io=%lu completion=%lu maintenance=%lu waited_ms=%lu",
                     (unsigned long)cancel_generation,
                     (unsigned long)io_claim_generation,
                     (unsigned long)completion_claim_generation,
                     (unsigned long)maintenance_claim_generation,
                     (unsigned long)(now_ms - started_ms));
            last_wait_log_ms = now_ms;
        }
        vTaskDelay(speaker_positive_delay_ticks(1));
    }

    if (claim_wait_timed_out) {
        ESP_LOGW(TAG,
                 "speaker cancel action claim wait timed out; recovering output-stop ownership cancel_generation=%lu stopped_generation=%lu timeout_ms=%lu",
                 (unsigned long)cancel_generation,
                 (unsigned long)stopped_playback_generation,
                 (unsigned long)NUWAX_SPEAKER_CLAIM_WAIT_TIMEOUT_MS);
    }
    const bool recover_action_claim_timeout = claim_wait_timed_out;
    claim_wait_timed_out = false;

    bool stop_claimed = false;
    bool stop_recovery_claimed = false;
    bool stop_superseded = false;
    bool stop_completed_elsewhere = false;
    esp_err_t stop_err = ESP_OK;
    while (!stop_claimed && !stop_superseded) {
        speaker_lock();
        if (speaker_stop_completion_matches_locked(
                stopped_connection_epoch,
                stopped_playback_generation)) {
            stop_completed_elsewhere = true;
            stop_err = s_speaker_ctx.completed_stop_result;
        } else {
            if (recover_action_claim_timeout &&
                nuwax_speaker_stop_recovery_claim_allowed(
                    s_speaker_ctx.submitted_generation,
                    s_speaker_ctx.playback_generation,
                    s_speaker_ctx.drop_until_end,
                    s_speaker_ctx.output_stop_owner_generation,
                    cancel_generation)) {
                s_speaker_ctx.output_stop_owner_generation =
                    cancel_generation;
                stop_claimed = true;
                stop_recovery_claimed = true;
            } else {
                const nuwax_speaker_stop_claim_result_t decision =
                    nuwax_speaker_stop_claim_decide(
                        s_speaker_ctx.submitted_generation,
                        s_speaker_ctx.playback_generation,
                        s_speaker_ctx.drop_until_end,
                        s_speaker_ctx.output_stop_owner_generation,
                        cancel_generation);
                if (decision == NUWAX_SPEAKER_STOP_CLAIMED) {
                    s_speaker_ctx.output_stop_owner_generation =
                        cancel_generation;
                    stop_claimed = true;
                } else if (decision ==
                           NUWAX_SPEAKER_STOP_SUPERSEDED) {
                    stop_superseded = true;
                }
            }
        }
        speaker_unlock();
        if (stop_completed_elsewhere) {
            break;
        }
        if (!stop_claimed && !stop_superseded &&
            elapsed_ms(monotonic_ms(), claim_wait_deadline_ms)) {
            claim_wait_timed_out = true;
            break;
        }
        if (!stop_claimed && !stop_superseded) {
            vTaskDelay(speaker_positive_delay_ticks(1));
        }
    }

    if (claim_wait_timed_out) {
        uint32_t current_owner = 0U;
        speaker_lock();
        current_owner = s_speaker_ctx.output_stop_owner_generation;
        speaker_unlock();
        ESP_LOGE(TAG,
                 "speaker cancel output-stop owner wait timed out; transaction remains in progress cancel_generation=%lu stopped_generation=%lu owner=%lu",
                 (unsigned long)cancel_generation,
                 (unsigned long)stopped_playback_generation,
                 (unsigned long)current_owner);
        speaker_notify_playback_fault(stopped_connection_epoch,
                                      stopped_playback_generation,
                                      ESP_ERR_TIMEOUT);
        return false;
    }
    if (stop_claimed) {
        if (stop_recovery_claimed) {
            ESP_LOGW(TAG,
                     "speaker cancel recovered output-stop ownership cancel_generation=%lu stopped_generation=%lu",
                     (unsigned long)cancel_generation,
                     (unsigned long)stopped_playback_generation);
        }
        stop_err = nuwax_speaker_output_stop(&s_speaker_ctx.output);
        if (stop_err == ESP_ERR_INVALID_STATE) {
            /* Only a completely absent output is already quiet.  An
             * inconsistent or live handle must remain a hard stop fault so a
             * caller cannot advance the generation while audio may continue. */
            speaker_lock();
            const bool output_absent = speaker_output_absent_locked();
            speaker_unlock();
            if (output_absent) {
                stop_err = ESP_OK;
            } else {
                ESP_LOGE(TAG,
                         "speaker cancel stop invalid state with output resources still present");
            }
        }
        const int64_t stop_completed_us = esp_timer_get_time();
        speaker_lock();
        if (stop_err == ESP_OK) {
            s_speaker_ctx.completed_stop_generation =
                stopped_playback_generation;
            s_speaker_ctx.completed_stop_connection_epoch =
                stopped_connection_epoch;
            s_speaker_ctx.completed_stop_timestamp_us =
                stop_completed_us;
            s_speaker_ctx.completed_stop_result = ESP_OK;
            s_speaker_ctx.completed_stop_cancel_requested = true;
            speaker_clear_stop_failure_locked(
                stopped_connection_epoch,
                stopped_playback_generation);
        } else {
            speaker_publish_stop_failure_locked(
                stopped_connection_epoch,
                stopped_playback_generation,
                stop_completed_us,
                stop_err);
        }
        if (nuwax_speaker_stop_owner_can_release(
                s_speaker_ctx.output_stop_owner_generation,
                cancel_generation)) {
            s_speaker_ctx.output_stop_owner_generation = 0;
        } else {
            ESP_LOGE(TAG,
                     "speaker output stop owner mismatch owner=%lu cancel_generation=%lu",
                     (unsigned long)s_speaker_ctx.output_stop_owner_generation,
                     (unsigned long)cancel_generation);
        }
        speaker_unlock();
    }
    speaker_lock();
    if (s_speaker_ctx.cancel_pending_generation == cancel_generation) {
        s_speaker_ctx.cancel_pending_generation = 0;
    }
    speaker_unlock();
    const uint32_t stop_ms = monotonic_ms() - started_ms;
    if (stop_completed_elsewhere) {
        ESP_LOGI(TAG,
                 "speaker local stop reused generation-bound completion source=%s generation=%lu result=%s stop_ms=%u",
                 source ? source : "-",
                 (unsigned long)stopped_playback_generation,
                 esp_err_to_name(stop_err),
                 (unsigned)stop_ms);
    } else if (stop_superseded) {
        ESP_LOGI(TAG,
                 "speaker local stop skipped superseded source=%s cancel_generation=%lu stop_ms=%u",
                 source ? source : "-",
                 (unsigned long)cancel_generation,
                 (unsigned)stop_ms);
    } else if (stop_err == ESP_OK) {
        ESP_LOGI(TAG,
                 "speaker local stop ok source=%s turn=%s playback=%s generation=%lu stop_ms=%u queued_before=%u had_active=%d window=%ums stale_sessions=%lu",
                 source ? source : "-",
                 stale_turn_id[0] ? stale_turn_id : "-",
                 stale_playback_id[0] ? stale_playback_id : "-",
                 (unsigned long)cancel_generation,
                 (unsigned)stop_ms,
                 (unsigned)queued_before,
                 had_active_playback ? 1 : 0,
                 (unsigned)drop_window_ms,
                 (unsigned long)stale_sessions);
    } else {
        ESP_LOGW(TAG,
                 "speaker local stop failed source=%s turn=%s playback=%s generation=%lu err=%s stop_ms=%u queued_before=%u had_active=%d window=%ums stale_sessions=%lu",
                 source ? source : "-",
                 stale_turn_id[0] ? stale_turn_id : "-",
                 stale_playback_id[0] ? stale_playback_id : "-",
                 (unsigned long)cancel_generation,
                 esp_err_to_name(stop_err),
                 (unsigned)stop_ms,
                 (unsigned)queued_before,
                 had_active_playback ? 1 : 0,
                 (unsigned)drop_window_ms,
                 (unsigned long)stale_sessions);
    }
    if (!stop_superseded && stop_err == ESP_OK) {
        /* Physical silence is the cancel terminal state.  Publish it even
         * when the stop was completed by another action owner; no later PCM
         * frame is required to drive conversation recovery. */
        speaker_notify_cancel_recovery_once(
            stopped_connection_epoch,
            stopped_playback_generation,
            source ? source : "cancel_complete");
    } else if (!stop_superseded && stop_err != ESP_OK) {
        speaker_notify_playback_fault(stopped_connection_epoch,
                                      stopped_playback_generation,
                                      stop_err);
    }
    return !stop_superseded && stop_err == ESP_OK;
}

void nuwax_speaker_playback_cancel_now(const char *source) {
    (void)speaker_cancel_now_if_current(0, source);
}

static bool speaker_request_cancel_generation_ex_internal(
    uint32_t expected_connection_epoch,
    uint32_t expected_generation,
    bool preserve_natural_drain,
    const char *source,
    uint32_t fade_ms,
    nuwax_speaker_cancel_request_result_t *result) {
    nuwax_speaker_cancel_request_result_t request = {
        .status = NUWAX_SPEAKER_CANCEL_REQUEST_REJECTED,
        .playback_generation = expected_generation,
        .completed_stop_result = ESP_ERR_INVALID_STATE,
        .failed_stop_result = ESP_ERR_INVALID_STATE,
    };
    if (expected_generation == 0) {
        if (result) {
            *result = request;
        }
        return false;
    }
    if (fade_ms == UINT32_MAX) {
        fade_ms = NUWAX_SPEAKER_FADE_CANCEL_DEFAULT_MS;
    }

    uint64_t total_samples =
        ((uint64_t)CONFIG_NUWAX_VOICE_OUTPUT_SAMPLE_RATE * fade_ms + 999ULL) / 1000ULL;
    if (total_samples == 0) {
        total_samples = 1;
    } else if (total_samples > UINT32_MAX) {
        total_samples = UINT32_MAX;
    }

    const uint32_t now_ms = monotonic_ms();
    uint32_t processed_samples = 0U;
    uint32_t existing_total_samples = 0U;
    const int64_t now_us = esp_timer_get_time();
    speaker_lock();
    if (speaker_stop_completion_matches_locked(0U, expected_generation)) {
        request.status = NUWAX_SPEAKER_CANCEL_REQUEST_COMPLETED;
        request.connection_epoch =
            s_speaker_ctx.completed_stop_connection_epoch;
        request.request_timestamp_us =
            s_speaker_ctx.cancel_request_generation == expected_generation &&
                    s_speaker_ctx.cancel_request_timestamp_us > 0
                ? s_speaker_ctx.cancel_request_timestamp_us
                : now_us;
        request.completed_stop_timestamp_us =
            s_speaker_ctx.completed_stop_timestamp_us;
        request.completed_stop_result = s_speaker_ctx.completed_stop_result;
    } else if (speaker_stop_failure_matches_locked(0U,
                                                   expected_generation)) {
        request.status = NUWAX_SPEAKER_CANCEL_REQUEST_FAILED;
        request.connection_epoch =
            s_speaker_ctx.failed_stop_connection_epoch;
        request.request_timestamp_us =
            s_speaker_ctx.cancel_request_generation == expected_generation &&
                    s_speaker_ctx.cancel_request_timestamp_us > 0
                ? s_speaker_ctx.cancel_request_timestamp_us
                : now_us;
        request.failed_stop_timestamp_us =
            s_speaker_ctx.failed_stop_timestamp_us;
        request.failed_stop_result = s_speaker_ctx.failed_stop_result;
    } else if (preserve_natural_drain &&
               nuwax_speaker_natural_drain_preservable(
                   s_speaker_ctx.submitted_generation,
                   s_speaker_ctx.submitted_connection_epoch,
                   s_speaker_ctx.playback_generation,
                   s_speaker_ctx.playback_connection_epoch,
                   s_speaker_ctx.drop_until_end,
                   s_speaker_ctx.stream.active,
                   s_speaker_ctx.stream.generation,
                   s_speaker_ctx.stream.end_accepted,
                   s_speaker_ctx.stream_protocol_faulted,
                   s_speaker_ctx.stop_fault_active,
                   s_speaker_ctx.cancel_request_generation ==
                           expected_generation ||
                       s_speaker_ctx.fade_cancel_generation ==
                           expected_generation ||
                       s_speaker_ctx.cancel_pending_generation != 0U ||
                       s_speaker_ctx.output_stop_owner_generation != 0U,
                   expected_connection_epoch,
                   expected_generation)) {
        request.status = NUWAX_SPEAKER_CANCEL_REQUEST_NATURAL_DRAINING;
        request.connection_epoch =
            s_speaker_ctx.submitted_connection_epoch;
    } else if (s_speaker_ctx.completion_claim_generation ==
               expected_generation) {
        if (s_speaker_ctx.cancel_request_generation != expected_generation) {
            s_speaker_ctx.cancel_request_generation = expected_generation;
            s_speaker_ctx.cancel_request_connection_epoch =
                s_speaker_ctx.submitted_connection_epoch;
            s_speaker_ctx.cancel_request_timestamp_us = now_us;
        }
        request.connection_epoch =
            s_speaker_ctx.cancel_request_connection_epoch;
        request.status = NUWAX_SPEAKER_CANCEL_REQUEST_STOPPING;
        request.request_timestamp_us =
            s_speaker_ctx.cancel_request_timestamp_us;
        request.connection_epoch =
            s_speaker_ctx.cancel_request_connection_epoch;
    } else if (s_speaker_ctx.cancel_request_generation ==
                   expected_generation &&
               (s_speaker_ctx.fade_cancel_active ||
                s_speaker_ctx.cancel_pending_generation != 0U ||
                s_speaker_ctx.output_stop_owner_generation != 0U)) {
        request.status = s_speaker_ctx.fade_cancel_active
                             ? NUWAX_SPEAKER_CANCEL_REQUEST_ARMED
                             : NUWAX_SPEAKER_CANCEL_REQUEST_STOPPING;
        request.request_timestamp_us =
            s_speaker_ctx.cancel_request_timestamp_us;
        request.connection_epoch =
            s_speaker_ctx.cancel_request_connection_epoch;
        processed_samples = s_speaker_ctx.fade_cancel_processed_samples;
        existing_total_samples = s_speaker_ctx.fade_cancel_total_samples;
    } else {
        const bool pending = nuwax_speaker_cancel_expected_matches(
                                 s_speaker_ctx.submitted_generation,
                                 s_speaker_ctx.drop_until_end,
                                 expected_generation) &&
                             (s_speaker_ctx.playback_active ||
                             s_speaker_ctx.playback_session_active ||
                             s_speaker_ctx.start_gate_pending ||
                             s_speaker_ctx.start_gate_authorized ||
                              speaker_packet_ring_count() > 0U);
        if (pending) {
            s_speaker_ctx.cancel_request_generation = expected_generation;
            s_speaker_ctx.cancel_request_connection_epoch =
                s_speaker_ctx.submitted_connection_epoch;
            s_speaker_ctx.cancel_request_timestamp_us = now_us;
            request.status = NUWAX_SPEAKER_CANCEL_REQUEST_ARMED;
            request.connection_epoch =
                s_speaker_ctx.cancel_request_connection_epoch;
            request.request_timestamp_us = now_us;
            s_speaker_ctx.fade_cancel_active = true;
            s_speaker_ctx.fade_cancel_generation = expected_generation;
            s_speaker_ctx.fade_cancel_total_samples =
                (uint32_t)total_samples;
            s_speaker_ctx.fade_cancel_processed_samples = 0;
            s_speaker_ctx.fade_cancel_deadline_ms = now_ms + fade_ms;
            strlcpy(s_speaker_ctx.fade_cancel_source,
                    source ? source : "generation_cancel",
                    sizeof(s_speaker_ctx.fade_cancel_source));
        }
    }
    speaker_unlock();

    if (result) {
        *result = request;
    }
    if (request.status == NUWAX_SPEAKER_CANCEL_REQUEST_REJECTED) {
        ESP_LOGI(TAG,
                 "speaker generation-bound async cancel skipped source=%s expected=%lu",
                 source ? source : "-",
                 (unsigned long)expected_generation);
        return false;
    }
    if (request.status ==
        NUWAX_SPEAKER_CANCEL_REQUEST_NATURAL_DRAINING) {
        ESP_LOGI(TAG,
                 "speaker scoped stop preserved natural drain source=%s epoch=%lu generation=%lu",
                 source ? source : "-",
                 (unsigned long)request.connection_epoch,
                 (unsigned long)expected_generation);
        return true;
    }
    if (request.status == NUWAX_SPEAKER_CANCEL_REQUEST_COMPLETED) {
        ESP_LOGI(TAG,
                 "speaker generation-bound cancel already completed source=%s generation=%lu request_us=%lld completed_us=%lld result=%s",
                 source ? source : "-",
                 (unsigned long)expected_generation,
                 (long long)request.request_timestamp_us,
                 (long long)request.completed_stop_timestamp_us,
                 esp_err_to_name(request.completed_stop_result));
        return true;
    }
    if (request.status == NUWAX_SPEAKER_CANCEL_REQUEST_FAILED) {
        ESP_LOGE(TAG,
                 "speaker generation-bound cancel stop failed source=%s generation=%lu request_us=%lld failed_us=%lld result=%s",
                 source ? source : "-",
                 (unsigned long)expected_generation,
                 (long long)request.request_timestamp_us,
                 (long long)request.failed_stop_timestamp_us,
                 esp_err_to_name(request.failed_stop_result));
        /* A terminal hardware-stop failure must not be reported as a
         * successful cancellation.  Callers use this return value to decide
         * whether it is safe to advance the conversation generation; doing
         * so after a failed stop could let stale playback continue. */
        return false;
    }
    if (request.status == NUWAX_SPEAKER_CANCEL_REQUEST_STOPPING) {
        ESP_LOGI(TAG,
                 "speaker generation-bound cancel stop already in progress source=%s generation=%lu request_us=%lld",
                 source ? source : "-",
                 (unsigned long)expected_generation,
                 (long long)request.request_timestamp_us);
        return true;
    }
    if (processed_samples > 0U || existing_total_samples > 0U) {
        ESP_LOGI(TAG,
                 "speaker generation-bound async cancel already armed source=%s generation=%lu request_us=%lld samples=%lu/%lu",
                 source ? source : "-",
                 (unsigned long)expected_generation,
                 (long long)request.request_timestamp_us,
                 (unsigned long)processed_samples,
                 (unsigned long)existing_total_samples);
        return true;
    }
    ESP_LOGI(TAG,
             "speaker generation-bound async cancel armed source=%s generation=%lu request_us=%lld fade_ms=%lu samples=%lu",
             source ? source : "-",
             (unsigned long)expected_generation,
             (long long)request.request_timestamp_us,
             (unsigned long)fade_ms,
             (unsigned long)total_samples);
    return true;
}

bool nuwax_speaker_playback_request_cancel_generation_ex(
    uint32_t expected_generation,
    const char *source,
    uint32_t fade_ms,
    nuwax_speaker_cancel_request_result_t *result) {
    return speaker_request_cancel_generation_ex_internal(0U,
                                                         expected_generation,
                                                         false,
                                                         source,
                                                         fade_ms,
                                                         result);
}

bool nuwax_speaker_playback_request_scoped_stop_generation_ex(
    uint32_t expected_connection_epoch,
    uint32_t expected_generation,
    bool preserve_natural_drain,
    const char *source,
    uint32_t fade_ms,
    nuwax_speaker_cancel_request_result_t *result) {
    return speaker_request_cancel_generation_ex_internal(
        expected_connection_epoch,
        expected_generation,
        preserve_natural_drain,
        source,
        fade_ms,
        result);
}

bool nuwax_speaker_playback_request_cancel_generation(uint32_t expected_generation,
                                                      const char *source,
                                                      uint32_t fade_ms) {
    return nuwax_speaker_playback_request_cancel_generation_ex(
        expected_generation, source, fade_ms, NULL);
}

bool nuwax_speaker_playback_request_probe_duck_generation(
    uint32_t expected_generation,
    uint8_t attenuation_db,
    uint16_t duration_ms) {
    if (expected_generation == 0U ||
        attenuation_db < NUWAX_SPEAKER_PROBE_MIN_ATTENUATION_DB ||
        attenuation_db > NUWAX_SPEAKER_PROBE_MAX_ATTENUATION_DB ||
        duration_ms < NUWAX_SPEAKER_PROBE_MIN_DURATION_MS ||
        duration_ms > NUWAX_SPEAKER_PROBE_MAX_DURATION_MS) {
        return false;
    }

    bool applied = false;
    bool already_active = false;
    uint8_t logical_volume = 0U;
    uint8_t duck_volume = 0U;
    esp_err_t duck_err = ESP_ERR_INVALID_STATE;
    speaker_lock();
    const bool output_ready = speaker_output_ready_locked();
    if (nuwax_speaker_probe_duck_allowed(
            s_speaker_ctx.submitted_generation,
            s_speaker_ctx.playback_generation,
            s_speaker_ctx.drop_until_end,
            output_ready,
            expected_generation)) {
        already_active = s_speaker_ctx.probe_duck_active &&
                         s_speaker_ctx.probe_duck_generation ==
                             expected_generation;
        logical_volume = s_speaker_ctx.volume_percent;
        duck_volume = already_active
                          ? s_speaker_ctx.probe_duck_output_percent
                          : 0U;
        if (!already_active) {
            duck_err = speaker_clear_probe_duck_locked(true);
            if (duck_err == ESP_OK) {
                duck_volume = nuwax_speaker_probe_output_percent(
                    logical_volume, attenuation_db);
                duck_err = nuwax_speaker_output_set_volume_percent(
                    &s_speaker_ctx.output, duck_volume);
            }
            if (duck_err == ESP_OK) {
                s_speaker_ctx.probe_duck_active = true;
                s_speaker_ctx.probe_duck_generation = expected_generation;
                s_speaker_ctx.probe_duck_deadline_ms =
                    monotonic_ms() + duration_ms;
                s_speaker_ctx.probe_duck_output_percent = duck_volume;
                s_speaker_ctx.output_volume_sync_pending = true;
            }
        }
        applied = already_active || duck_err == ESP_OK;
    }
    speaker_unlock();

    if (applied) {
        ESP_LOGI(TAG,
                 "speaker probe duck %s generation=%lu attenuation_db=%u duration_ms=%u volume=%u->%u",
                 already_active ? "already-active" : "applied",
                 (unsigned long)expected_generation,
                 (unsigned)attenuation_db,
                 (unsigned)duration_ms,
                 (unsigned)logical_volume,
                 (unsigned)duck_volume);
    } else {
        ESP_LOGW(TAG,
                 "speaker probe duck rejected generation=%lu attenuation_db=%u duration_ms=%u err=%s",
                 (unsigned long)expected_generation,
                 (unsigned)attenuation_db,
                 (unsigned)duration_ms,
                 esp_err_to_name(duck_err));
    }
    return applied;
}

void nuwax_speaker_playback_request_fade_cancel(const char *source, uint32_t fade_ms) {
    if (fade_ms == UINT32_MAX) {
        fade_ms = NUWAX_SPEAKER_FADE_CANCEL_DEFAULT_MS;
    }
    uint32_t expected_generation = 0;
    bool had_pending_playback = false;
    bool unactivated_start_gate = false;
    speaker_lock();
    expected_generation = s_speaker_ctx.submitted_generation;
    unactivated_start_gate =
        (s_speaker_ctx.start_gate_pending ||
         s_speaker_ctx.start_gate_authorized) &&
        !s_speaker_ctx.playback_active;
    had_pending_playback = s_speaker_ctx.playback_active ||
                           s_speaker_ctx.playback_session_active ||
                           s_speaker_ctx.start_gate_pending ||
                           s_speaker_ctx.start_gate_authorized ||
                           speaker_packet_ring_count() > 0U;
    speaker_unlock();
    if (!had_pending_playback || expected_generation == 0) {
        ESP_LOGI(TAG,
                 "speaker async PCM fade cancel ignored without pending playback source=%s",
                 source ? source : "-");
        return;
    }
    if (unactivated_start_gate &&
        nuwax_speaker_playback_reject_start_generation(expected_generation,
                                                       source)) {
        return;
    }
    (void)nuwax_speaker_playback_request_cancel_generation(
        expected_generation, source, fade_ms);
}

void nuwax_speaker_playback_fade_cancel(const char *source, uint32_t fade_ms) {
    const uint32_t started_ms = monotonic_ms();
    uint8_t restore_volume = 0;
    uint32_t expected_generation = 0;
    bool had_active_playback = false;
    bool unactivated_start_gate = false;
    speaker_lock();
    restore_volume = s_speaker_ctx.volume_percent;
    expected_generation = s_speaker_ctx.submitted_generation;
    unactivated_start_gate =
        (s_speaker_ctx.start_gate_pending ||
         s_speaker_ctx.start_gate_authorized) &&
        !s_speaker_ctx.playback_active;
    had_active_playback = s_speaker_ctx.playback_active ||
                          s_speaker_ctx.playback_session_active ||
                          s_speaker_ctx.start_gate_pending ||
                          s_speaker_ctx.start_gate_authorized ||
                          speaker_packet_ring_count() > 0U;
    speaker_unlock();
    if (!had_active_playback) {
        ESP_LOGI(TAG,
                 "speaker fade cancel ignored without pending playback source=%s",
                 source ? source : "-");
        return;
    }
    if (unactivated_start_gate &&
        nuwax_speaker_playback_reject_start_generation(expected_generation,
                                                       source)) {
        return;
    }
    if (fade_ms == 0) {
        (void)speaker_cancel_now_if_current(expected_generation, source);
        return;
    }

    if (fade_ms == UINT32_MAX) {
        fade_ms = NUWAX_SPEAKER_FADE_CANCEL_DEFAULT_MS;
    }
    uint32_t steps = fade_ms / NUWAX_SPEAKER_FADE_CANCEL_STEP_MS;
    if (steps < 2) {
        steps = 2;
    } else if (steps > 12) {
        steps = 12;
    }
    const uint32_t step_ms = fade_ms / steps;

    for (uint32_t step = 1; step <= steps; step++) {
        const uint8_t next = (uint8_t)(((uint32_t)restore_volume * (steps - step)) / steps);
        esp_err_t fade_volume_err = ESP_OK;
        speaker_lock();
        const bool generation_current =
            nuwax_speaker_generation_is_current(
                s_speaker_ctx.submitted_generation,
                s_speaker_ctx.playback_generation,
                s_speaker_ctx.drop_until_end,
                expected_generation);
        const bool output_ready = generation_current &&
                                  speaker_output_ready_locked();
        if (output_ready && generation_current) {
            fade_volume_err = nuwax_speaker_output_set_volume_percent(
                &s_speaker_ctx.output, next);
        }
        speaker_unlock();
        if (fade_volume_err != ESP_OK) {
            ESP_LOGW(TAG,
                     "speaker fade volume update failed generation=%lu step=%lu err=%s",
                     (unsigned long)expected_generation,
                     (unsigned long)step,
                     esp_err_to_name(fade_volume_err));
            break;
        }
        if (!generation_current || !output_ready ||
            !nuwax_speaker_playback_has_pending()) {
            break;
        }
        vTaskDelay(speaker_positive_delay_ticks(step_ms == 0 ? 1 : step_ms));
    }

    (void)speaker_cancel_now_if_current(expected_generation, source);

    uint8_t restored_volume = restore_volume;
    esp_err_t restore_err = ESP_OK;
    speaker_lock();
    restored_volume = s_speaker_ctx.volume_percent;
    restore_err = speaker_sync_output_volume_locked();
    speaker_unlock();
    if (restore_err != ESP_OK) {
        ESP_LOGW(TAG,
                 "speaker fade volume restore deferred generation=%lu err=%s",
                 (unsigned long)expected_generation,
                 esp_err_to_name(restore_err));
    }

    ESP_LOGI(TAG,
             "speaker fade cancel source=%s fade_ms=%lu restored_volume=%u elapsed_ms=%lu",
             source ? source : "-",
             (unsigned long)fade_ms,
             (unsigned)restored_volume,
             (unsigned long)(monotonic_ms() - started_ms));
}

void nuwax_speaker_playback_prepare_for_tts(const char *reason) {
    const uint32_t action_claim = speaker_claim_idle_output_action();
    if (action_claim == 0) {
        ESP_LOGI(TAG,
                 "speaker preinit skipped reason=%s output_action_busy=1",
                 reason ? reason : "-");
        return;
    }
    esp_err_t err = ensure_speaker_output_ready();
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "speaker preinit failed reason=%s err=%s",
                 reason ? reason : "-",
                 esp_err_to_name(err));
        speaker_release_idle_output_action(action_claim);
        return;
    }
    err = nuwax_speaker_output_stop(&s_speaker_ctx.output);
    speaker_release_idle_output_action(action_claim);
    if (err == ESP_ERR_INVALID_STATE) {
        speaker_lock();
        const bool output_absent = speaker_output_absent_locked();
        speaker_unlock();
        if (output_absent) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "speaker preinit ready reason=%s muted=1", reason ? reason : "-");
    } else {
        ESP_LOGW(TAG,
                 "speaker preinit mute failed reason=%s err=%s",
                 reason ? reason : "-",
                 esp_err_to_name(err));
    }
}

void nuwax_speaker_playback_release_idle_output(const char *reason) {
    const uint32_t action_claim = speaker_claim_idle_output_action();
    if (action_claim == 0) {
        return;
    }
    speaker_lock();
    const bool should_release = !speaker_output_absent_locked();
    const bool keep_initialized = s_speaker_ctx.keep_initialized;
    speaker_unlock();
    if (should_release && keep_initialized) {
        const esp_err_t mute_err =
            speaker_mute_idle_output(reason ? reason : "release_idle");
        if (mute_err == ESP_OK) {
            ESP_LOGI(TAG,
                     "speaker idle output kept initialized reason=%s",
                     reason ? reason : "-");
        } else {
            ESP_LOGW(TAG,
                     "speaker idle output mute failed reason=%s err=%s",
                     reason ? reason : "-",
                     esp_err_to_name(mute_err));
        }
        speaker_release_idle_output_action(action_claim);
        return;
    }
    if (should_release) {
        speaker_lock();
        const esp_err_t deinit_err =
            nuwax_speaker_output_deinit(&s_speaker_ctx.output);
        speaker_unlock();
        if (deinit_err == ESP_OK) {
            ESP_LOGI(TAG,
                     "speaker idle output released reason=%s",
                     reason ? reason : "-");
        } else {
            ESP_LOGW(TAG,
                     "speaker idle output release failed reason=%s err=%s",
                     reason ? reason : "-",
                     esp_err_to_name(deinit_err));
        }
    }
    speaker_release_idle_output_action(action_claim);
}

esp_err_t nuwax_speaker_playback_reset_runtime(void) {
    bool reset_claimed = false;
    const uint32_t claim_deadline_ms =
        monotonic_ms() + NUWAX_SPEAKER_RUNTIME_RESET_TIMEOUT_MS;
    while (!reset_claimed) {
        speaker_lock();
        if (!s_speaker_ctx.runtime_reset_in_progress) {
            s_speaker_ctx.runtime_reset_in_progress = true;
            reset_claimed = true;
        }
        speaker_unlock();
        if (!reset_claimed) {
            if (elapsed_ms(monotonic_ms(), claim_deadline_ms)) {
                ESP_LOGE(TAG,
                         "speaker runtime reset timed out waiting for reset owner timeout_ms=%lu",
                         (unsigned long)NUWAX_SPEAKER_RUNTIME_RESET_TIMEOUT_MS);
                return ESP_ERR_TIMEOUT;
            }
            vTaskDelay(speaker_positive_delay_ticks(1));
        }
    }

    nuwax_speaker_playback_cancel_now("runtime_reset");
    const uint32_t reset_deadline_ms =
        monotonic_ms() + NUWAX_SPEAKER_RUNTIME_RESET_TIMEOUT_MS;
    uint32_t stop_retries = 0U;
    while (true) {
        bool reset_complete = false;
        bool retry_failed_stop = false;
        uint32_t failed_generation = 0U;
        esp_err_t failed_result = ESP_ERR_INVALID_STATE;
        speaker_lock();
        if (s_speaker_ctx.io_claim_generation == 0 &&
            s_speaker_ctx.completion_claim_generation == 0 &&
            s_speaker_ctx.maintenance_claim_generation == 0 &&
            s_speaker_ctx.cancel_pending_generation == 0 &&
            s_speaker_ctx.output_stop_owner_generation == 0 &&
            !s_speaker_ctx.stop_fault_active) {
            if (s_speaker_ctx.packets) {
                (void)speaker_packet_ring_clear_locked();
            }
            s_speaker_ctx.submitted_generation =
                speaker_next_generation(s_speaker_ctx.submitted_generation);
            s_speaker_ctx.submitted_connection_epoch = 0U;
            s_speaker_ctx.playback_generation = s_speaker_ctx.submitted_generation;
            s_speaker_ctx.playback_connection_epoch = 0U;
            speaker_clear_end_seen_locked();
            speaker_reset_stream_runtime_locked(true);
            speaker_reset_jitter_learning_locked(0U);
            s_speaker_ctx.drop_until_end = false;
            speaker_clear_start_gate_locked();
            s_speaker_ctx.playback_active = false;
            s_speaker_ctx.playback_session_active = false;
            s_speaker_ctx.drop_deadline_ms = 0;
            s_speaker_ctx.dropped_frames = 0;
            s_speaker_ctx.queued_frames = 0;
            s_speaker_ctx.played_frames = 0;
            s_speaker_ctx.queue_dropped_frames = 0;
            s_speaker_ctx.write_failures = 0;
            s_speaker_ctx.gap_silence_frames = 0;
            s_speaker_ctx.last_frame_samples = 0;
            s_speaker_ctx.jitter_rebuffering = false;
            s_speaker_ctx.jitter_rebuffer_started_ms = 0U;
            s_speaker_ctx.jitter_rebuffer_events = 0U;
            s_speaker_ctx.jitter_rebuffer_wait_ms = 0U;
            s_speaker_ctx.jitter_rebuffer_max_wait_ms = 0U;
            s_speaker_ctx.jitter_empty_started_ms = 0U;
            s_speaker_ctx.jitter_low_watermark_ms = UINT32_MAX;
            s_speaker_ctx.startup_buffer_wait_ms = 0U;
            s_speaker_ctx.fade_cancel_active = false;
            s_speaker_ctx.fade_cancel_generation = 0;
            s_speaker_ctx.fade_cancel_total_samples = 0;
            s_speaker_ctx.fade_cancel_processed_samples = 0;
            s_speaker_ctx.fade_cancel_deadline_ms = 0;
            s_speaker_ctx.fade_cancel_source[0] = '\0';
            s_speaker_ctx.cancel_request_generation = 0;
            s_speaker_ctx.cancel_request_connection_epoch = 0U;
            s_speaker_ctx.cancel_request_timestamp_us = 0;
            s_speaker_ctx.cancel_stopped_generation = 0;
            s_speaker_ctx.cancel_stopped_connection_epoch = 0U;
            s_speaker_ctx.cancel_recovery_notified_generation = 0;
            s_speaker_ctx.cancel_recovery_notified_connection_epoch = 0U;
            s_speaker_ctx.stop_fault_active = false;
            s_speaker_ctx.failed_stop_generation = 0;
            s_speaker_ctx.failed_stop_connection_epoch = 0U;
            s_speaker_ctx.failed_stop_timestamp_us = 0;
            s_speaker_ctx.failed_stop_result = ESP_ERR_INVALID_STATE;
            (void)speaker_clear_probe_duck_locked(false);
            s_speaker_ctx.active_owner = NUWAX_SPEAKER_PLAYBACK_OWNER_NONE;
            s_speaker_ctx.active_turn_id[0] = '\0';
            s_speaker_ctx.active_notify_id[0] = '\0';
            s_speaker_ctx.active_playback_id[0] = '\0';
            s_speaker_ctx.stale_turn_id[0] = '\0';
            s_speaker_ctx.stale_playback_id[0] = '\0';
            s_speaker_ctx.runtime_reset_in_progress = false;
            reset_complete = true;
        } else if (s_speaker_ctx.stop_fault_active &&
                   s_speaker_ctx.io_claim_generation == 0 &&
                   s_speaker_ctx.completion_claim_generation == 0 &&
                   s_speaker_ctx.maintenance_claim_generation == 0 &&
                   s_speaker_ctx.cancel_pending_generation == 0 &&
                   s_speaker_ctx.output_stop_owner_generation == 0 &&
                   stop_retries <
                       NUWAX_SPEAKER_RUNTIME_RESET_MAX_STOP_RETRIES) {
            retry_failed_stop = true;
            failed_generation = s_speaker_ctx.failed_stop_generation;
            failed_result = s_speaker_ctx.failed_stop_result;
        }
        speaker_unlock();
        if (reset_complete) {
            return ESP_OK;
        }
        if (retry_failed_stop) {
            stop_retries++;
            ESP_LOGW(TAG,
                     "speaker runtime reset retrying failed physical stop attempt=%lu/%u generation=%lu result=%s",
                     (unsigned long)stop_retries,
                     (unsigned)NUWAX_SPEAKER_RUNTIME_RESET_MAX_STOP_RETRIES,
                     (unsigned long)failed_generation,
                     esp_err_to_name(failed_result));
            nuwax_speaker_playback_cancel_now("runtime_reset_stop_retry");
            continue;
        }
        if (elapsed_ms(monotonic_ms(), reset_deadline_ms)) {
            speaker_lock();
            failed_generation = s_speaker_ctx.failed_stop_generation;
            failed_result = s_speaker_ctx.stop_fault_active
                                ? s_speaker_ctx.failed_stop_result
                                : ESP_ERR_TIMEOUT;
            s_speaker_ctx.runtime_reset_in_progress = false;
            speaker_unlock();
            ESP_LOGE(TAG,
                     "speaker runtime reset aborted with unresolved stop generation=%lu result=%s retries=%lu timeout_ms=%lu",
                     (unsigned long)failed_generation,
                     esp_err_to_name(failed_result),
                     (unsigned long)stop_retries,
                     (unsigned long)NUWAX_SPEAKER_RUNTIME_RESET_TIMEOUT_MS);
            return failed_result == ESP_OK ? ESP_ERR_TIMEOUT : failed_result;
        }
        vTaskDelay(speaker_positive_delay_ticks(1));
    }
    return ESP_OK;
}

void nuwax_speaker_playback_note_playback_begin(const char *turn_id,
                                                const char *playback_id,
                                                uint32_t frames,
                                                uint32_t bytes) {
    const char *turn = turn_id ? turn_id : "";
    const char *playback = playback_id ? playback_id : "";
    uint32_t generation = 0U;
    uint32_t connection_epoch = 0U;
    bool totals_valid = true;
    const int64_t playback_begin_rx_us = esp_timer_get_time();
    speaker_lock();
    const bool same_playback =
        s_speaker_ctx.active_owner ==
            NUWAX_SPEAKER_PLAYBACK_OWNER_CONVERSATION &&
        turn[0] != '\0' &&
        s_speaker_ctx.active_turn_id[0] != '\0' &&
        strcmp(turn, s_speaker_ctx.active_turn_id) == 0 &&
        playback[0] != '\0' &&
        s_speaker_ctx.active_playback_id[0] != '\0' &&
        strcmp(playback, s_speaker_ctx.active_playback_id) == 0;
    if (s_speaker_ctx.fade_cancel_active &&
        playback[0] != '\0' &&
        s_speaker_ctx.active_playback_id[0] != '\0' &&
        strcmp(playback, s_speaker_ctx.active_playback_id) != 0) {
        s_speaker_ctx.fade_cancel_active = false;
        s_speaker_ctx.fade_cancel_deadline_ms = 0;
        s_speaker_ctx.fade_cancel_source[0] = '\0';
    }
    if (!same_playback) {
        s_speaker_ctx.metadata_expected_frames = frames;
        s_speaker_ctx.metadata_expected_bytes = bytes;
        s_speaker_ctx.playback_begin_rx_us = playback_begin_rx_us;
    } else {
        if (frames != 0U &&
            s_speaker_ctx.metadata_expected_frames != 0U &&
            frames != s_speaker_ctx.metadata_expected_frames) {
            totals_valid = false;
        } else if (frames != 0U) {
            s_speaker_ctx.metadata_expected_frames = frames;
        }
        if (bytes != 0U &&
            s_speaker_ctx.metadata_expected_bytes != 0U &&
            bytes != s_speaker_ctx.metadata_expected_bytes) {
            totals_valid = false;
        } else if (bytes != 0U) {
            s_speaker_ctx.metadata_expected_bytes = bytes;
        }
    }
    s_speaker_ctx.active_owner =
        NUWAX_SPEAKER_PLAYBACK_OWNER_CONVERSATION;
    strlcpy(s_speaker_ctx.active_turn_id, turn, sizeof(s_speaker_ctx.active_turn_id));
    s_speaker_ctx.active_notify_id[0] = '\0';
    strlcpy(s_speaker_ctx.active_playback_id, playback, sizeof(s_speaker_ctx.active_playback_id));
    s_speaker_ctx.playback_session_active = true;
    const bool live_stream_generation =
        s_speaker_ctx.stream.active &&
        !s_speaker_ctx.stream.end_accepted &&
        (s_speaker_ctx.start_gate_pending ||
         s_speaker_ctx.start_gate_authorized ||
         s_speaker_ctx.playback_active) &&
        s_speaker_ctx.stream.generation ==
            s_speaker_ctx.submitted_generation;
    if (live_stream_generation) {
        generation = s_speaker_ctx.stream.generation;
        connection_epoch = s_speaker_ctx.submitted_connection_epoch;
        totals_valid = totals_valid && nuwax_speaker_stream_bind_expected(
                                           &s_speaker_ctx.stream,
                                           generation,
                                           s_speaker_ctx.metadata_expected_frames,
                                           s_speaker_ctx.metadata_expected_bytes);
    }
    speaker_unlock();

    ESP_LOGI(TAG,
             "speaker playback context begin turn=%s playback=%s frames=%lu bytes=%lu generation=%lu totals_valid=%d",
             turn[0] ? turn : "-",
             playback[0] ? playback : "-",
             (unsigned long)frames,
             (unsigned long)bytes,
             (unsigned long)generation,
             totals_valid ? 1 : 0);
    if (!totals_valid && generation != 0U) {
        speaker_schedule_protocol_fault(connection_epoch,
                                        generation,
                                        ESP_ERR_INVALID_RESPONSE,
                                        "playback_metadata_totals",
                                        false);
    }
}

bool nuwax_speaker_playback_abort_metadata_only_session(
    const char *turn_id,
    const char *playback_id,
    const char *reason) {
    const char *turn = turn_id ? turn_id : "";
    const char *playback = playback_id ? playback_id : "";
    bool cleared = false;
    int64_t playback_begin_rx_us = 0;
    uint32_t stale_generation = 0U;

    speaker_lock();
    const bool ids_match =
        s_speaker_ctx.active_owner ==
            NUWAX_SPEAKER_PLAYBACK_OWNER_CONVERSATION &&
        turn[0] != '\0' && playback[0] != '\0' &&
        strcmp(turn, s_speaker_ctx.active_turn_id) == 0 &&
        strcmp(playback, s_speaker_ctx.active_playback_id) == 0;
    const bool metadata_only =
        s_speaker_ctx.playback_session_active &&
        !s_speaker_ctx.playback_active &&
        !s_speaker_ctx.start_gate_pending &&
        !s_speaker_ctx.start_gate_authorized &&
        s_speaker_ctx.completion_claim_generation == 0U &&
        s_speaker_ctx.cancel_pending_generation == 0U &&
        s_speaker_ctx.output_stop_owner_generation == 0U &&
        speaker_packet_ring_count() == 0U;
    if (ids_match && metadata_only) {
        playback_begin_rx_us = s_speaker_ctx.playback_begin_rx_us;
        stale_generation = s_speaker_ctx.submitted_generation;
        s_speaker_ctx.playback_session_active = false;
        s_speaker_ctx.active_owner = NUWAX_SPEAKER_PLAYBACK_OWNER_NONE;
        s_speaker_ctx.active_turn_id[0] = '\0';
        s_speaker_ctx.active_notify_id[0] = '\0';
        s_speaker_ctx.active_playback_id[0] = '\0';
        s_speaker_ctx.playback_begin_rx_us = 0;
        s_speaker_ctx.first_pcm_rx_us = 0;
        s_speaker_ctx.start_gate_authorized_us = 0;
        s_speaker_ctx.startup_watermark_ready_us = 0;
        s_speaker_ctx.first_i2s_write_us = 0;
        s_speaker_ctx.metadata_expected_frames = 0U;
        s_speaker_ctx.metadata_expected_bytes = 0U;
        speaker_clear_end_seen_locked();
        speaker_reset_stream_runtime_locked(true);
        cleared = true;
    }
    speaker_unlock();

    if (cleared) {
        ESP_LOGW(TAG,
                 "speaker metadata-only playback aborted turn=%s playback=%s stale_generation=%lu age_ms=%lld reason=%s",
                 turn,
                 playback,
                 (unsigned long)stale_generation,
                 playback_begin_rx_us > 0
                     ? (long long)((esp_timer_get_time() -
                                    playback_begin_rx_us) /
                                   1000LL)
                     : -1LL,
                 reason ? reason : "-");
    }
    return cleared;
}

void nuwax_speaker_playback_note_playback_end(const char *turn_id,
                                              const char *playback_id,
                                              const char *reason) {
    bool cleared = false;
    bool matched = true;
    speaker_lock();
    if (s_speaker_ctx.active_owner !=
        NUWAX_SPEAKER_PLAYBACK_OWNER_CONVERSATION) {
        matched = false;
    }
    if (turn_id && turn_id[0] && s_speaker_ctx.active_turn_id[0] &&
        strcmp(turn_id, s_speaker_ctx.active_turn_id) != 0) {
        matched = false;
    }
    if (playback_id && playback_id[0] && s_speaker_ctx.active_playback_id[0] &&
        strcmp(playback_id, s_speaker_ctx.active_playback_id) != 0) {
        matched = false;
    }
    if (matched) {
        s_speaker_ctx.playback_session_active = false;
        /* Once START is accepted, the stream owns a private copy of the
         * validated totals. If START is still queued, retain the handoff
         * cache until the speaker task consumes it. */
        if (s_speaker_ctx.stream.active ||
            speaker_packet_ring_count() == 0U) {
            s_speaker_ctx.metadata_expected_frames = 0U;
            s_speaker_ctx.metadata_expected_bytes = 0U;
        }
        if (!s_speaker_ctx.playback_active &&
            !s_speaker_ctx.start_gate_pending &&
            !s_speaker_ctx.start_gate_authorized &&
            speaker_packet_ring_count() == 0U) {
            s_speaker_ctx.active_owner = NUWAX_SPEAKER_PLAYBACK_OWNER_NONE;
            s_speaker_ctx.active_turn_id[0] = '\0';
            s_speaker_ctx.active_notify_id[0] = '\0';
            s_speaker_ctx.active_playback_id[0] = '\0';
            s_speaker_ctx.playback_begin_rx_us = 0;
            speaker_clear_end_seen_locked();
            speaker_reset_stream_runtime_locked(true);
        }
        cleared = true;
    }
    speaker_unlock();
    ESP_LOGI(TAG,
             "speaker playback context end turn=%s playback=%s reason=%s session_matched=%d session_cleared=%d",
             turn_id && turn_id[0] ? turn_id : "-",
             playback_id && playback_id[0] ? playback_id : "-",
             reason && reason[0] ? reason : "-",
             matched ? 1 : 0,
             cleared ? 1 : 0);
}

void nuwax_speaker_playback_note_notification_begin(const char *notify_id,
                                                    const char *playback_id,
                                                    uint32_t frames,
                                                    uint32_t bytes) {
    const char *notify = notify_id ? notify_id : "";
    const char *playback = playback_id ? playback_id : "";
    uint32_t generation = 0U;
    uint32_t connection_epoch = 0U;
    bool totals_valid = true;
    const int64_t playback_begin_rx_us = esp_timer_get_time();
    speaker_lock();
    const bool same_playback =
        s_speaker_ctx.active_owner ==
            NUWAX_SPEAKER_PLAYBACK_OWNER_NOTIFICATION &&
        notify[0] != '\0' &&
        s_speaker_ctx.active_notify_id[0] != '\0' &&
        strcmp(notify, s_speaker_ctx.active_notify_id) == 0 &&
        playback[0] != '\0' &&
        s_speaker_ctx.active_playback_id[0] != '\0' &&
        strcmp(playback, s_speaker_ctx.active_playback_id) == 0;
    if (s_speaker_ctx.fade_cancel_active && playback[0] != '\0' &&
        s_speaker_ctx.active_playback_id[0] != '\0' &&
        strcmp(playback, s_speaker_ctx.active_playback_id) != 0) {
        s_speaker_ctx.fade_cancel_active = false;
        s_speaker_ctx.fade_cancel_deadline_ms = 0U;
        s_speaker_ctx.fade_cancel_source[0] = '\0';
    }
    if (!same_playback) {
        s_speaker_ctx.metadata_expected_frames = frames;
        s_speaker_ctx.metadata_expected_bytes = bytes;
        s_speaker_ctx.playback_begin_rx_us = playback_begin_rx_us;
    } else {
        if (frames != 0U &&
            s_speaker_ctx.metadata_expected_frames != 0U &&
            frames != s_speaker_ctx.metadata_expected_frames) {
            totals_valid = false;
        } else if (frames != 0U) {
            s_speaker_ctx.metadata_expected_frames = frames;
        }
        if (bytes != 0U && s_speaker_ctx.metadata_expected_bytes != 0U &&
            bytes != s_speaker_ctx.metadata_expected_bytes) {
            totals_valid = false;
        } else if (bytes != 0U) {
            s_speaker_ctx.metadata_expected_bytes = bytes;
        }
    }
    s_speaker_ctx.active_owner =
        NUWAX_SPEAKER_PLAYBACK_OWNER_NOTIFICATION;
    s_speaker_ctx.active_turn_id[0] = '\0';
    strlcpy(s_speaker_ctx.active_notify_id,
            notify,
            sizeof(s_speaker_ctx.active_notify_id));
    strlcpy(s_speaker_ctx.active_playback_id,
            playback,
            sizeof(s_speaker_ctx.active_playback_id));
    s_speaker_ctx.playback_session_active = true;
    const bool live_stream_generation =
        s_speaker_ctx.stream.active && !s_speaker_ctx.stream.end_accepted &&
        (s_speaker_ctx.start_gate_pending ||
         s_speaker_ctx.start_gate_authorized ||
         s_speaker_ctx.playback_active) &&
        s_speaker_ctx.stream.generation ==
            s_speaker_ctx.submitted_generation;
    if (live_stream_generation) {
        generation = s_speaker_ctx.stream.generation;
        connection_epoch = s_speaker_ctx.submitted_connection_epoch;
        totals_valid = totals_valid && nuwax_speaker_stream_bind_expected(
                                           &s_speaker_ctx.stream,
                                           generation,
                                           s_speaker_ctx.metadata_expected_frames,
                                           s_speaker_ctx.metadata_expected_bytes);
    }
    speaker_unlock();

    ESP_LOGI(TAG,
             "speaker notification context begin notify=%s playback=%s frames=%lu bytes=%lu generation=%lu totals_valid=%d",
             notify[0] ? notify : "-",
             playback[0] ? playback : "-",
             (unsigned long)frames,
             (unsigned long)bytes,
             (unsigned long)generation,
             totals_valid ? 1 : 0);
    if (!totals_valid && generation != 0U) {
        speaker_schedule_protocol_fault(connection_epoch,
                                        generation,
                                        ESP_ERR_INVALID_RESPONSE,
                                        "notification_metadata_totals",
                                        false);
    }
}

bool nuwax_speaker_playback_abort_notification_metadata_only_session(
    const char *notify_id,
    const char *playback_id,
    const char *reason) {
    const char *notify = notify_id ? notify_id : "";
    const char *playback = playback_id ? playback_id : "";
    bool cleared = false;
    speaker_lock();
    const bool ids_match =
        s_speaker_ctx.active_owner ==
            NUWAX_SPEAKER_PLAYBACK_OWNER_NOTIFICATION &&
        notify[0] != '\0' && playback[0] != '\0' &&
        strcmp(notify, s_speaker_ctx.active_notify_id) == 0 &&
        strcmp(playback, s_speaker_ctx.active_playback_id) == 0;
    const bool metadata_only =
        s_speaker_ctx.playback_session_active &&
        !s_speaker_ctx.playback_active &&
        !s_speaker_ctx.start_gate_pending &&
        !s_speaker_ctx.start_gate_authorized &&
        s_speaker_ctx.completion_claim_generation == 0U &&
        s_speaker_ctx.cancel_pending_generation == 0U &&
        s_speaker_ctx.output_stop_owner_generation == 0U &&
        speaker_packet_ring_count() == 0U;
    if (ids_match && metadata_only) {
        s_speaker_ctx.playback_session_active = false;
        s_speaker_ctx.active_owner = NUWAX_SPEAKER_PLAYBACK_OWNER_NONE;
        s_speaker_ctx.active_turn_id[0] = '\0';
        s_speaker_ctx.active_notify_id[0] = '\0';
        s_speaker_ctx.active_playback_id[0] = '\0';
        s_speaker_ctx.playback_begin_rx_us = 0;
        s_speaker_ctx.first_pcm_rx_us = 0;
        s_speaker_ctx.start_gate_authorized_us = 0;
        s_speaker_ctx.startup_watermark_ready_us = 0;
        s_speaker_ctx.first_i2s_write_us = 0;
        s_speaker_ctx.metadata_expected_frames = 0U;
        s_speaker_ctx.metadata_expected_bytes = 0U;
        speaker_clear_end_seen_locked();
        speaker_reset_stream_runtime_locked(true);
        cleared = true;
    }
    speaker_unlock();
    if (cleared) {
        ESP_LOGW(TAG,
                 "speaker metadata-only notification aborted notify=%s playback=%s reason=%s",
                 notify,
                 playback,
                 reason ? reason : "-");
    }
    return cleared;
}

void nuwax_speaker_playback_note_notification_end(const char *notify_id,
                                                  const char *playback_id,
                                                  const char *reason) {
    bool cleared = false;
    bool matched = true;
    speaker_lock();
    if (s_speaker_ctx.active_owner !=
        NUWAX_SPEAKER_PLAYBACK_OWNER_NOTIFICATION) {
        matched = false;
    }
    if (notify_id && notify_id[0] && s_speaker_ctx.active_notify_id[0] &&
        strcmp(notify_id, s_speaker_ctx.active_notify_id) != 0) {
        matched = false;
    }
    if (playback_id && playback_id[0] &&
        s_speaker_ctx.active_playback_id[0] &&
        strcmp(playback_id, s_speaker_ctx.active_playback_id) != 0) {
        matched = false;
    }
    if (matched) {
        s_speaker_ctx.playback_session_active = false;
        if (s_speaker_ctx.stream.active ||
            speaker_packet_ring_count() == 0U) {
            s_speaker_ctx.metadata_expected_frames = 0U;
            s_speaker_ctx.metadata_expected_bytes = 0U;
        }
        if (!s_speaker_ctx.playback_active &&
            !s_speaker_ctx.start_gate_pending &&
            !s_speaker_ctx.start_gate_authorized &&
            speaker_packet_ring_count() == 0U) {
            s_speaker_ctx.active_owner = NUWAX_SPEAKER_PLAYBACK_OWNER_NONE;
            s_speaker_ctx.active_turn_id[0] = '\0';
            s_speaker_ctx.active_notify_id[0] = '\0';
            s_speaker_ctx.active_playback_id[0] = '\0';
            s_speaker_ctx.playback_begin_rx_us = 0;
            speaker_clear_end_seen_locked();
            speaker_reset_stream_runtime_locked(true);
        }
        cleared = true;
    }
    speaker_unlock();
    ESP_LOGI(TAG,
             "speaker notification context end notify=%s playback=%s reason=%s session_matched=%d session_cleared=%d",
             notify_id && notify_id[0] ? notify_id : "-",
             playback_id && playback_id[0] ? playback_id : "-",
             reason && reason[0] ? reason : "-",
             matched ? 1 : 0,
             cleared ? 1 : 0);
}

esp_err_t nuwax_speaker_playback_authorize_generation(
    uint32_t generation,
    const char *turn_id,
    const char *playback_id) {
    if (generation == 0U || !turn_id || turn_id[0] == '\0' ||
        !playback_id || playback_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = ESP_ERR_INVALID_STATE;
    bool totals_invalid = false;
    uint32_t connection_epoch = 0U;
    speaker_lock();
    const bool ids_match =
        s_speaker_ctx.active_owner ==
            NUWAX_SPEAKER_PLAYBACK_OWNER_CONVERSATION &&
        s_speaker_ctx.active_turn_id[0] != '\0' &&
        s_speaker_ctx.active_playback_id[0] != '\0' &&
        strcmp(turn_id, s_speaker_ctx.active_turn_id) == 0 &&
        strcmp(playback_id, s_speaker_ctx.active_playback_id) == 0;
    if (speaker_start_gate_matches_locked(
            s_speaker_ctx.start_gate_connection_epoch,
            generation) &&
        !s_speaker_ctx.drop_until_end && ids_match) {
        connection_epoch = s_speaker_ctx.start_gate_connection_epoch;
        const bool totals_valid = nuwax_speaker_stream_bind_expected(
            &s_speaker_ctx.stream,
            generation,
            s_speaker_ctx.metadata_expected_frames,
            s_speaker_ctx.metadata_expected_bytes);
        if (!totals_valid) {
            totals_invalid = true;
            result = ESP_ERR_INVALID_RESPONSE;
        } else if (s_speaker_ctx.start_gate_pending) {
            s_speaker_ctx.start_gate_pending = false;
            s_speaker_ctx.start_gate_authorized = true;
            s_speaker_ctx.start_gate_deadline_ms = 0U;
            s_speaker_ctx.start_gate_authorized_us = esp_timer_get_time();
            result = ESP_OK;
        } else if (s_speaker_ctx.start_gate_authorized) {
            /* Metadata may be observed by the owner before and after the
             * deferred START event. Treat the same generation and IDs as an
             * idempotent authorization, never as a new playback. */
            result = ESP_OK;
        }
    }
    speaker_unlock();

    if (totals_invalid) {
        speaker_schedule_protocol_fault(connection_epoch,
                                        generation,
                                        ESP_ERR_INVALID_RESPONSE,
                                        "playback_metadata_totals",
                                        false);
    }

    if (result == ESP_OK) {
        ESP_LOGI(TAG,
                 "speaker START metadata authorized generation=%lu turn=%s playback=%s",
                 (unsigned long)generation,
                 turn_id,
                 playback_id);
    } else {
        ESP_LOGW(TAG,
                 "speaker START metadata authorization rejected generation=%lu turn=%s playback=%s",
                 (unsigned long)generation,
                 turn_id,
                 playback_id);
    }
    return result;
}

esp_err_t nuwax_speaker_playback_authorize_notification_generation(
    uint32_t generation,
    const char *notify_id,
    const char *playback_id) {
    if (generation == 0U || !notify_id || notify_id[0] == '\0' ||
        !playback_id || playback_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = ESP_ERR_INVALID_STATE;
    bool totals_invalid = false;
    uint32_t connection_epoch = 0U;
    speaker_lock();
    const bool ids_match =
        s_speaker_ctx.active_owner ==
            NUWAX_SPEAKER_PLAYBACK_OWNER_NOTIFICATION &&
        s_speaker_ctx.active_notify_id[0] != '\0' &&
        s_speaker_ctx.active_playback_id[0] != '\0' &&
        strcmp(notify_id, s_speaker_ctx.active_notify_id) == 0 &&
        strcmp(playback_id, s_speaker_ctx.active_playback_id) == 0;
    if (speaker_start_gate_matches_locked(
            s_speaker_ctx.start_gate_connection_epoch,
            generation) &&
        !s_speaker_ctx.drop_until_end && ids_match) {
        connection_epoch = s_speaker_ctx.start_gate_connection_epoch;
        const bool totals_valid = nuwax_speaker_stream_bind_expected(
            &s_speaker_ctx.stream,
            generation,
            s_speaker_ctx.metadata_expected_frames,
            s_speaker_ctx.metadata_expected_bytes);
        if (!totals_valid) {
            totals_invalid = true;
            result = ESP_ERR_INVALID_RESPONSE;
        } else if (s_speaker_ctx.start_gate_pending) {
            s_speaker_ctx.start_gate_pending = false;
            s_speaker_ctx.start_gate_authorized = true;
            s_speaker_ctx.start_gate_deadline_ms = 0U;
            s_speaker_ctx.start_gate_authorized_us = esp_timer_get_time();
            result = ESP_OK;
        } else if (s_speaker_ctx.start_gate_authorized) {
            result = ESP_OK;
        }
    }
    speaker_unlock();

    if (totals_invalid) {
        speaker_schedule_protocol_fault(connection_epoch,
                                        generation,
                                        ESP_ERR_INVALID_RESPONSE,
                                        "notification_metadata_totals",
                                        false);
    }
    if (result == ESP_OK) {
        ESP_LOGI(TAG,
                 "speaker notification START authorized generation=%lu notify=%s playback=%s",
                 (unsigned long)generation,
                 notify_id,
                 playback_id);
    } else {
        ESP_LOGW(TAG,
                 "speaker notification START authorization rejected generation=%lu notify=%s playback=%s",
                 (unsigned long)generation,
                 notify_id,
                 playback_id);
    }
    return result;
}

bool nuwax_speaker_playback_reject_start_generation(uint32_t expected_generation,
                                                    const char *source) {
    bool rejected = false;
    uint32_t generation = 0U;
    uint32_t cleared_frames = 0U;
    speaker_lock();
    generation = s_speaker_ctx.start_gate_generation;
    /* An authorized START can still be waiting in the speaker task between
     * metadata validation and playback activation. It is safe to invalidate
     * that generation while playback_active is false; once I2S activation has
     * happened, the regular generation-bound cancel path must stop it. */
    const bool gate_can_be_rejected =
        s_speaker_ctx.start_gate_pending ||
        (s_speaker_ctx.start_gate_authorized &&
         !s_speaker_ctx.playback_active);
    if (gate_can_be_rejected &&
        (expected_generation == 0U || expected_generation == generation) &&
        generation != 0U &&
        generation == s_speaker_ctx.submitted_generation) {
        const uint32_t rejected_connection_epoch =
            s_speaker_ctx.start_gate_connection_epoch;
        const int64_t rejected_timestamp_us = esp_timer_get_time();
        speaker_clear_start_gate_locked();
        s_speaker_ctx.cancel_stopped_generation = generation;
        s_speaker_ctx.cancel_stopped_connection_epoch =
            rejected_connection_epoch;
        /* This generation never owned the physical I2S output. Publish a
         * terminal successful stop just like a physical stop so callers that
         * are waiting on an epoch/generation-bound cancellation can converge
         * immediately.  Leaving only cancel_stopped_generation set makes the
         * lifecycle owner wait for a completion that can never be emitted. */
        s_speaker_ctx.completed_stop_generation = generation;
        s_speaker_ctx.completed_stop_connection_epoch =
            rejected_connection_epoch;
        s_speaker_ctx.completed_stop_timestamp_us = rejected_timestamp_us;
        s_speaker_ctx.completed_stop_result = ESP_OK;
        s_speaker_ctx.completed_stop_cancel_requested = true;
        if (s_speaker_ctx.cancel_request_generation == generation) {
            s_speaker_ctx.cancel_pending_generation = 0U;
            s_speaker_ctx.cancel_request_generation = 0U;
            s_speaker_ctx.cancel_request_connection_epoch = 0U;
            s_speaker_ctx.cancel_request_timestamp_us = 0;
        }
        if (s_speaker_ctx.fade_cancel_generation == generation) {
            s_speaker_ctx.fade_cancel_active = false;
            s_speaker_ctx.fade_cancel_generation = 0U;
            s_speaker_ctx.fade_cancel_total_samples = 0U;
            s_speaker_ctx.fade_cancel_processed_samples = 0U;
            s_speaker_ctx.fade_cancel_deadline_ms = 0U;
            s_speaker_ctx.fade_cancel_source[0] = '\0';
        }
        s_speaker_ctx.drop_until_end = true;
        s_speaker_ctx.drop_deadline_ms =
            monotonic_ms() + NUWAX_SPEAKER_CANCEL_DROP_ACTIVE_MS;
        s_speaker_ctx.dropped_frames = 0U;
        s_speaker_ctx.playback_active = false;
        s_speaker_ctx.playback_session_active = false;
        s_speaker_ctx.speaking_state_generation = 0U;
        speaker_clear_end_seen_locked();
        speaker_reset_stream_runtime_locked(true);
        if (s_speaker_ctx.packets) {
            cleared_frames = speaker_packet_ring_clear_locked();
        }
        rejected = true;
    }
    speaker_unlock();
    if (rejected) {
        ESP_LOGI(TAG,
                 "speaker START metadata gate rejected generation=%lu source=%s cleared_frames=%lu physical_stop=not_needed",
                 (unsigned long)generation,
                 source ? source : "-",
                 (unsigned long)cleared_frames);
    }
    return rejected;
}

esp_err_t nuwax_speaker_playback_deinit_output(void) {
    const uint32_t action_claim = speaker_claim_idle_output_action();
    if (action_claim == 0) {
        ESP_LOGW(TAG, "speaker output deinit rejected output_action_busy=1");
        return ESP_ERR_INVALID_STATE;
    }
    speaker_lock();
    const esp_err_t err = speaker_output_absent_locked()
                              ? ESP_OK
                              : nuwax_speaker_output_deinit(
                                    &s_speaker_ctx.output);
    const bool released = speaker_output_absent_locked();
    speaker_unlock();
    speaker_release_idle_output_action(action_claim);
    if (err != ESP_OK || !released) {
        const esp_err_t result = err != ESP_OK ? err : ESP_ERR_INVALID_STATE;
        ESP_LOGE(TAG,
                 "speaker physical output release failed err=%s released=%d",
                 esp_err_to_name(result),
                 released ? 1 : 0);
        return result;
    }
    ESP_LOGI(TAG,
             "speaker physical output released; playback runtime retained");
    return ESP_OK;
}

void nuwax_speaker_playback_apply_volume_percent(int volume_percent, bool persist) {
    uint8_t next = clamp_speaker_volume_percent(volume_percent);
    esp_err_t volume_err = ESP_OK;
    speaker_lock();
    (void)speaker_clear_probe_duck_locked(false);
    if (s_speaker_ctx.volume_percent != next) {
        s_speaker_ctx.volume_generation =
            speaker_next_generation(s_speaker_ctx.volume_generation);
    }
    s_speaker_ctx.volume_percent = next;
    volume_err = speaker_sync_output_volume_locked();
    speaker_unlock();
    if (volume_err != ESP_OK) {
        ESP_LOGW(TAG,
                 "speaker volume hardware update deferred target=%u err=%s",
                 (unsigned)next,
                 esp_err_to_name(volume_err));
    }
    if (s_speaker_ctx.callbacks.on_volume_changed) {
        s_speaker_ctx.callbacks.on_volume_changed(next);
    }
    if (persist) {
        save_speaker_volume_percent(next);
    }
    ESP_LOGI(TAG, "speaker volume set to %u%% persist=%d", (unsigned)next, persist ? 1 : 0);
}

void nuwax_speaker_playback_preview_volume_percent(int volume_percent) {
    uint8_t next = clamp_speaker_volume_percent(volume_percent);
    esp_err_t volume_err = ESP_OK;
    speaker_lock();
    (void)speaker_clear_probe_duck_locked(false);
    if (s_speaker_ctx.volume_percent != next) {
        s_speaker_ctx.volume_generation =
            speaker_next_generation(s_speaker_ctx.volume_generation);
        s_speaker_ctx.volume_percent = next;
    }
    volume_err = speaker_sync_output_volume_locked();
    speaker_unlock();
    if (volume_err != ESP_OK) {
        ESP_LOGW(TAG,
                 "speaker preview volume hardware update deferred target=%u err=%s",
                 (unsigned)next,
                 esp_err_to_name(volume_err));
    }
}

void nuwax_speaker_playback_apply_current_volume(void) {
    nuwax_speaker_playback_apply_volume_percent(
        nuwax_speaker_playback_get_volume_percent(),
        false);
}

uint8_t nuwax_speaker_playback_get_volume_percent(void) {
    uint8_t volume = 0U;
    speaker_lock();
    volume = s_speaker_ctx.volume_percent;
    speaker_unlock();
    return volume;
}

// v15-v2: 设备声纹 mask 模块需要的 TTS ID getter/setter
// 默认 zh_female_daimengchuanmei_moon_bigtts (ID=1)
esp_err_t nuwax_speaker_playback_handle_audio_frame(uint32_t sequence,
                                                    uint8_t flags,
                                                    uint16_t sample_rate,
                                                    uint8_t channels,
                                                    const int16_t *samples,
                                                    size_t sample_count) {
    return nuwax_speaker_playback_handle_audio_frame_admitted(
        0U,
        0U,
        sequence,
        flags,
        sample_rate,
        channels,
        samples,
        sample_count);
}

esp_err_t nuwax_speaker_playback_handle_audio_frame_admitted(
    uint32_t connection_epoch,
    uint32_t admission_epoch,
    uint32_t sequence,
    uint8_t flags,
    uint16_t sample_rate,
    uint8_t channels,
    const int16_t *samples,
    size_t sample_count) {
    return nuwax_speaker_playback_handle_audio_frame_admitted_with_timeout(
        connection_epoch,
        admission_epoch,
        sequence,
        flags,
        sample_rate,
        channels,
        samples,
        sample_count,
        NUWAX_SPEAKER_RX_ADMISSION_BUDGET_MS);
}

esp_err_t nuwax_speaker_playback_handle_audio_frame_admitted_with_timeout(
    uint32_t connection_epoch,
    uint32_t admission_epoch,
    uint32_t sequence,
    uint8_t flags,
    uint16_t sample_rate,
    uint8_t channels,
    const int16_t *samples,
    size_t sample_count,
    uint32_t admission_timeout_ms) {
    const int64_t start_handler_entry_us =
        (flags & NUWAX_AUDIO_FLAG_START) != 0 ? esp_timer_get_time() : 0;
    const bool start = (flags & NUWAX_AUDIO_FLAG_START) != 0U;
    const bool end = (flags & NUWAX_AUDIO_FLAG_END) != 0U;
    const bool control_end_marker = sample_count == 0U && end && !start;
    if (admission_epoch != 0U && connection_epoch == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Downlink packetization is independent of the fixed 256-sample AFE
     * processing chunk. The gateway currently sends 20 ms PCM frames while
     * capture/AEC runs at 16 ms, and the software-reference FIFO bridges the
     * two clocks without padding or dropping samples. */
    const size_t max_samples = CONFIG_NUWAX_VOICE_MAX_SAMPLES_PER_FRAME;
    const bool flags_valid =
        (flags & (uint8_t)~NUWAX_SPEAKER_ALLOWED_AUDIO_FLAGS) == 0U;
    const bool format_valid =
        flags_valid &&
        sample_rate == CONFIG_NUWAX_VOICE_OUTPUT_SAMPLE_RATE &&
        channels == 1U && sample_count <= max_samples &&
        ((sample_count > 0U && samples != NULL) || control_end_marker) &&
        (!start || (sequence == 0U && sample_count > 0U));
    if (!format_valid) {
        speaker_generation_ref_t current = {0};
        speaker_lock();
        if (s_speaker_ctx.stream.active) {
            current.generation = s_speaker_ctx.stream.generation;
            current.connection_epoch =
                s_speaker_ctx.submitted_connection_epoch;
        }
        speaker_unlock();
        ESP_LOGE(TAG,
                 "speaker rejected format seq=%u flags=0x%02x rate=%u channels=%u samples=%u expected=known_flags/seq0_START/1..%u/%u/1",
                 (unsigned)sequence,
                 flags,
                 (unsigned)sample_rate,
                 (unsigned)channels,
                 (unsigned)sample_count,
                 (unsigned)max_samples,
                 (unsigned)CONFIG_NUWAX_VOICE_OUTPUT_SAMPLE_RATE);
        if (!start && current.generation != 0U &&
            current.connection_epoch == connection_epoch) {
            speaker_schedule_protocol_fault(current.connection_epoch,
                                            current.generation,
                                            ESP_ERR_INVALID_RESPONSE,
                                            "invalid_audio_format",
                                            false);
        }
        /* The existing WebSocket owner closes immediately for an invalid
         * START only when INVALID_STATE is returned. Preserve that fail-fast
         * contract for STARTs that cannot safely create a generation. */
        return start ? ESP_ERR_INVALID_STATE : ESP_ERR_INVALID_RESPONSE;
    }
    const uint32_t now_ms = monotonic_ms();
    uint32_t frame_generation = 0;
    const uint32_t submitted_start_generation =
        speaker_submit_start_generation(sequence,
                                        flags,
                                        connection_epoch,
                                        admission_epoch);
    if ((flags & NUWAX_AUDIO_FLAG_START) != 0 &&
        submitted_start_generation == 0U) {
        return ESP_ERR_INVALID_STATE;
    }
    if (speaker_get_drop_state(now_ms)) {
        return speaker_note_cancelled_frame(connection_epoch,
                                            sequence,
                                            flags)
                   ? ESP_OK
                   : ESP_ERR_INVALID_STATE;
    }

    if ((flags & NUWAX_AUDIO_FLAG_START) == 0) {
        bool playback_context_present = false;
        bool stream_active = false;
        speaker_lock();
        playback_context_present = s_speaker_ctx.start_gate_pending ||
                                   s_speaker_ctx.start_gate_authorized ||
                                   s_speaker_ctx.playback_active ||
                                   s_speaker_ctx.playback_session_active;
        stream_active = s_speaker_ctx.stream.active;
        speaker_unlock();
        /* A stream remains authoritative until its generation is explicitly
         * reset. In particular, after the physical END has completed,
         * `playback_context_present` may already be false while
         * `stream_active` is still true. Let the stream policy see a late PCM
         * frame so POST_END/sequence faults invalidate the transaction;
         * only a frame for which no stream generation ever existed is stale
         * residue from a canceled prior cloud turn. */
        if (nuwax_speaker_stream_frame_is_orphan(start, stream_active)) {
            ESP_LOGW(TAG,
                     "speaker dropped orphan audio before START seq=%u context=%d stream=%d",
                     (unsigned)sequence,
                     playback_context_present ? 1 : 0,
                     stream_active ? 1 : 0);
            return ESP_ERR_NOT_FOUND;
        }
    }

    if ((flags & NUWAX_AUDIO_FLAG_START) != 0) {
        frame_generation = submitted_start_generation;
    } else {
        speaker_lock();
        if (s_speaker_ctx.submitted_connection_epoch == connection_epoch) {
            frame_generation = s_speaker_ctx.submitted_generation;
        }
        speaker_unlock();
        if (frame_generation == 0U) {
            return ESP_ERR_INVALID_STATE;
        }
    }
    return speaker_enqueue_packet(frame_generation,
                                  connection_epoch,
                                  admission_epoch,
                                  sequence,
                                  flags,
                                  sample_rate,
                                  channels,
                                  samples,
                                  sample_count,
                                  start_handler_entry_us,
                                  admission_timeout_ms);
}

static bool speaker_wait_for_start_authorization(
    const speaker_playback_packet_t *packet) {
    if (!packet || packet->generation == 0U) {
        return false;
    }

    while (true) {
        bool generation_current = false;
        bool authorized = false;
        bool timed_out = false;
        bool protocol_faulted = false;
        uint32_t cleared_frames = 0U;
        speaker_lock();
        protocol_faulted = s_speaker_ctx.stream_protocol_faulted &&
                           s_speaker_ctx.stream.generation ==
                               packet->generation;
        generation_current =
            speaker_generation_source_matches_locked(
                packet->connection_epoch,
                packet->generation) &&
            !s_speaker_ctx.drop_until_end && !protocol_faulted;
        authorized = generation_current &&
                     speaker_start_gate_authorized_locked(
                         packet->connection_epoch,
                         packet->generation);
        if (generation_current && !authorized &&
            s_speaker_ctx.start_gate_pending &&
            speaker_start_gate_matches_locked(packet->connection_epoch,
                                              packet->generation) &&
            s_speaker_ctx.start_gate_deadline_ms != 0U &&
            elapsed_ms(monotonic_ms(), s_speaker_ctx.start_gate_deadline_ms)) {
            /* Invalidate the queued generation without touching the physical
             * output.  The owner callback will close the uncertain cloud
             * session; subsequent frames are drained as stale. */
            s_speaker_ctx.cancel_stopped_generation = packet->generation;
            s_speaker_ctx.cancel_stopped_connection_epoch =
                packet->connection_epoch;
            speaker_clear_start_gate_locked();
            s_speaker_ctx.drop_until_end = true;
            s_speaker_ctx.drop_deadline_ms =
                monotonic_ms() + NUWAX_SPEAKER_CANCEL_DROP_ACTIVE_MS;
            s_speaker_ctx.dropped_frames = 0U;
            s_speaker_ctx.playback_active = false;
            s_speaker_ctx.playback_session_active = false;
            s_speaker_ctx.speaking_state_generation = 0U;
            speaker_clear_end_seen_locked();
            speaker_reset_stream_runtime_locked(true);
            if (s_speaker_ctx.packets) {
                cleared_frames = speaker_packet_ring_clear_locked();
            }
            timed_out = true;
        }
        speaker_unlock();

        if (authorized) {
            return true;
        }
        if (!generation_current || timed_out) {
            if (timed_out && s_speaker_ctx.callbacks.on_start_gate_timeout) {
                s_speaker_ctx.callbacks.on_start_gate_timeout(
                    packet->connection_epoch,
                    packet->generation);
            }
            if (timed_out) {
                ESP_LOGE(TAG,
                         "speaker START metadata authorization timed out generation=%lu timeout_ms=%u cleared_frames=%lu",
                         (unsigned long)packet->generation,
                         (unsigned)NUWAX_SPEAKER_START_GATE_TIMEOUT_MS,
                         (unsigned long)cleared_frames);
            }
            return false;
        }
        vTaskDelay(speaker_positive_delay_ticks(1));
    }
}

static bool speaker_wait_for_jitter_watermark(
    const speaker_playback_packet_t *packet,
    bool playback_started,
    uint32_t held_samples,
    uint32_t *waited_ms_out) {
    if (!packet || packet->generation == 0U) {
        return false;
    }

    const uint32_t wait_started_ms = monotonic_ms();
    uint32_t last_ingress_progress_ms = wait_started_ms;
    uint32_t last_ingress_frames = 0U;
    bool recoverable_stall_warned = false;
    while (true) {
        const uint32_t now_ms = monotonic_ms();
        bool generation_current = false;
        bool end_accepted = false;
        bool ready = false;
        uint32_t queued_records = 0U;
        uint32_t queued_samples = 0U;
        uint32_t ingress_frames = 0U;
        uint32_t expected_sequence = 0U;
        uint32_t startup_target_ms =
            NUWAX_SPEAKER_JITTER_STARTUP_BASE_MS;
        uint32_t resume_target_ms = NUWAX_SPEAKER_JITTER_RESUME_MIN_MS;
        speaker_lock();
        generation_current =
            speaker_generation_source_matches_locked(packet->connection_epoch,
                                                     packet->generation) &&
            !s_speaker_ctx.drop_until_end &&
            !s_speaker_ctx.stream_protocol_faulted &&
            s_speaker_ctx.stream.active &&
            s_speaker_ctx.stream.generation == packet->generation &&
            (!playback_started ||
             (s_speaker_ctx.playback_active &&
              s_speaker_ctx.playback_connection_epoch ==
                  packet->connection_epoch &&
              s_speaker_ctx.playback_generation == packet->generation));
        end_accepted = generation_current &&
                       s_speaker_ctx.stream.end_accepted;
        ingress_frames = s_speaker_ctx.stream.pcm_frames;
        expected_sequence = s_speaker_ctx.stream.next_sequence;
        startup_target_ms = s_speaker_ctx.jitter_startup_target_ms;
        resume_target_ms = s_speaker_ctx.jitter_resume_target_ms;
        queued_records = speaker_packet_ring_count() +
                         (held_samples > 0U ? 1U : 0U);
        queued_samples = speaker_packet_ring_samples() + held_samples;
        ready = generation_current && nuwax_speaker_jitter_ready_ms(
                                         playback_started,
                                         end_accepted,
                                         queued_records,
                                         queued_samples,
                                         packet->sample_rate,
                                         startup_target_ms,
                                         resume_target_ms);
        if (ready && playback_started &&
            s_speaker_ctx.jitter_rebuffering) {
            const uint32_t waited_ms =
                now_ms - s_speaker_ctx.jitter_rebuffer_started_ms;
            const bool midstream =
                nuwax_speaker_jitter_rebuffer_is_midstream(
                    s_speaker_ctx.jitter_rebuffer_entry_pcm_frames,
                    ingress_frames);
            if (midstream) {
                s_speaker_ctx.jitter_rebuffer_events++;
                s_speaker_ctx.jitter_resume_target_ms =
                    nuwax_speaker_jitter_resume_target_with_gap(
                        s_speaker_ctx.jitter_rebuffer_events,
                        NUWAX_SPEAKER_JITTER_RESUME_MIN_MS,
                        NUWAX_SPEAKER_JITTER_RESUME_MAX_MS,
                        s_speaker_ctx.ingress_max_gap_ms,
                        NUWAX_SPEAKER_JITTER_RESUME_GAP_CUSHION_MS);
                s_speaker_ctx.jitter_rebuffer_wait_ms += waited_ms;
                if (waited_ms >
                    s_speaker_ctx.jitter_rebuffer_max_wait_ms) {
                    s_speaker_ctx.jitter_rebuffer_max_wait_ms = waited_ms;
                }
                s_speaker_ctx.jitter_resume_fade_pending = true;
            } else {
                s_speaker_ctx.jitter_tail_wait_events++;
                s_speaker_ctx.jitter_tail_wait_ms += waited_ms;
            }
            s_speaker_ctx.jitter_rebuffering = false;
            s_speaker_ctx.jitter_rebuffer_started_ms = 0U;
            s_speaker_ctx.jitter_rebuffer_entry_pcm_frames = 0U;
            s_speaker_ctx.jitter_empty_started_ms = 0U;
        }
        if (ready && !playback_started) {
            s_speaker_ctx.startup_watermark_ready_us = esp_timer_get_time();
        }
        speaker_unlock();

        if (ingress_frames != last_ingress_frames) {
            last_ingress_frames = ingress_frames;
            last_ingress_progress_ms = now_ms;
        }

        if (!generation_current) {
            return false;
        }
        if (ready) {
            const uint32_t waited_ms = now_ms - wait_started_ms;
            if (waited_ms_out) {
                *waited_ms_out = waited_ms;
            }
            ESP_LOGI(TAG,
                     "speaker jitter watermark ready generation=%lu phase=%s queued_records=%lu queued_samples=%lu queued_ms=%lu target_ms=%u end_accepted=%d waited_ms=%lu",
                     (unsigned long)packet->generation,
                     playback_started ? "resume" : "startup",
                     (unsigned long)queued_records,
                     (unsigned long)queued_samples,
                     (unsigned long)nuwax_speaker_jitter_audio_ms(
                         queued_samples, packet->sample_rate),
                     (unsigned)(playback_started
                                    ? resume_target_ms
                                    : startup_target_ms),
                     end_accepted ? 1 : 0,
                     (unsigned long)waited_ms);
            return true;
        }
        if (speaker_process_pending_protocol_fault()) {
            return false;
        }
        (void)speaker_restore_probe_duck_if_due(now_ms);
        char fade_source[sizeof(s_speaker_ctx.fade_cancel_source)] = {0};
        uint32_t fade_generation = 0U;
        if (speaker_fade_cancel_due(now_ms,
                                    fade_source,
                                    sizeof(fade_source),
                                    &fade_generation)) {
            (void)speaker_cancel_now_if_current_epoch(
                packet->connection_epoch,
                fade_generation,
                fade_source[0] ? fade_source : "fade_deadline");
            return false;
        }
        const uint32_t stalled_ms = now_ms - last_ingress_progress_ms;
        const nuwax_speaker_stall_action_t stall_action =
            nuwax_speaker_stall_action(
                stalled_ms,
                NUWAX_SPEAKER_STREAM_STALL_WARN_MS,
                NUWAX_SPEAKER_STREAM_STALL_TIMEOUT_MS,
                recoverable_stall_warned);
        if (stall_action == NUWAX_SPEAKER_STALL_WARN) {
            recoverable_stall_warned = true;
            ESP_LOGW(TAG,
                     "speaker downlink PCM paused; keeping generation recoverable generation=%lu stalled_ms=%lu hard_timeout_ms=%u expected_seq=%lu",
                     (unsigned long)packet->generation,
                     (unsigned long)stalled_ms,
                     (unsigned)NUWAX_SPEAKER_STREAM_STALL_TIMEOUT_MS,
                     (unsigned long)expected_sequence);
        } else if (stall_action == NUWAX_SPEAKER_STALL_ABORT) {
            speaker_schedule_protocol_fault(packet->connection_epoch,
                                            packet->generation,
                                            ESP_ERR_TIMEOUT,
                                            "pcm_progress_hard_stall",
                                            true);
            return false;
        }
        (void)ulTaskNotifyTake(
            pdTRUE,
            speaker_positive_delay_ticks(NUWAX_SPEAKER_JITTER_POLL_MS));
    }
}

void nuwax_speaker_playback_task(void *arg) {
    (void)arg;
    speaker_lock();
    s_speaker_ctx.playback_task = xTaskGetCurrentTaskHandle();
    speaker_unlock();
    /* These records are owned by the singleton playback task. Keep their
     * large PCM payloads out of the task stack; the ring buffer already owns
     * the producer-side copy in PSRAM. The macros preserve the existing field
     * access syntax while making the backing storage heap-owned PSRAM. */
    if (!s_speaker_task_packet || !s_speaker_task_rebuffer_ref) {
        ESP_LOGE(TAG, "speaker task packet storage is unavailable; suspending");
        vTaskSuspend(NULL);
        return;
    }
    speaker_playback_packet_t *packet_storage = s_speaker_task_packet;
    speaker_playback_packet_t *rebuffer_ref_storage =
        s_speaker_task_rebuffer_ref;
#define packet (*packet_storage)
#define rebuffer_ref (*rebuffer_ref_storage)
    speaker_log_task_stack_watermark("task_start");
    while (true) {
        if (speaker_process_pending_protocol_fault()) {
            continue;
        }
        (void)speaker_restore_probe_duck_if_due(monotonic_ms());
        memset(&rebuffer_ref, 0, sizeof(rebuffer_ref));
        bool rebuffering = false;
        uint32_t probe_duck_deadline_ms = 0U;
        speaker_lock();
        rebuffering = s_speaker_ctx.jitter_rebuffering;
        if (rebuffering) {
            rebuffer_ref.connection_epoch =
                s_speaker_ctx.playback_connection_epoch;
            rebuffer_ref.generation = s_speaker_ctx.playback_generation;
            rebuffer_ref.sample_rate = CONFIG_NUWAX_VOICE_SAMPLE_RATE;
        }
        if (s_speaker_ctx.probe_duck_active) {
            probe_duck_deadline_ms =
                s_speaker_ctx.probe_duck_deadline_ms;
        }
        speaker_unlock();
        if (rebuffering &&
            !speaker_wait_for_jitter_watermark(&rebuffer_ref,
                                               true,
                                               0U,
                                               NULL)) {
            continue;
        }
        uint32_t receive_wait_ms = NUWAX_SPEAKER_JITTER_POLL_MS;
        if (probe_duck_deadline_ms != 0U) {
            const uint32_t now_ms = monotonic_ms();
            const uint32_t probe_wait_ms =
                elapsed_ms(now_ms, probe_duck_deadline_ms)
                    ? 1U
                    : probe_duck_deadline_ms - now_ms;
            if (receive_wait_ms > probe_wait_ms) {
                receive_wait_ms = probe_wait_ms;
            }
        }
        TickType_t receive_wait_ticks = pdMS_TO_TICKS(receive_wait_ms);
        if (receive_wait_ticks == 0) {
            receive_wait_ticks = 1;
        }
        if (!speaker_packet_ring_receive_notified(&packet,
                                                  receive_wait_ticks)) {
            if (speaker_process_pending_protocol_fault()) {
                continue;
            }
            (void)speaker_restore_probe_duck_if_due(monotonic_ms());
            char fade_source[sizeof(s_speaker_ctx.fade_cancel_source)] = {0};
            uint32_t fade_generation = 0;
            if (speaker_fade_cancel_due(monotonic_ms(),
                                        fade_source,
                                        sizeof(fade_source),
                                        &fade_generation)) {
                (void)speaker_cancel_now_if_current(
                    fade_generation,
                    fade_source[0] ? fade_source : "fade_deadline");
                continue;
            }
            const speaker_generation_ref_t stalled =
                speaker_stalled_generation(monotonic_ms());
            if (stalled.generation != 0U) {
                speaker_schedule_protocol_fault(stalled.connection_epoch,
                                                stalled.generation,
                                                ESP_ERR_TIMEOUT,
                                                "pcm_progress_hard_stall",
                                                true);
                continue;
            }
            bool playback_active = false;
            bool entered_rebuffer = false;
            uint32_t playback_generation = 0U;
            uint32_t queue_waiting = 0U;
            uint32_t empty_wait_ms = 0U;
            uint32_t entry_pcm_frames = 0U;
            uint32_t ingress_gap_ms = 0U;
            uint32_t confirmed_rebuffers = 0U;
            uint32_t resume_target_ms =
                NUWAX_SPEAKER_JITTER_RESUME_MIN_MS;
            const uint32_t empty_now_ms = monotonic_ms();
            speaker_lock();
            playback_active = s_speaker_ctx.playback_active;
            playback_generation = s_speaker_ctx.playback_generation;
            queue_waiting = speaker_packet_ring_count();
            if (playback_active && !s_speaker_ctx.jitter_rebuffering &&
                queue_waiting == 0U) {
                if (s_speaker_ctx.jitter_empty_started_ms == 0U) {
                    s_speaker_ctx.jitter_empty_started_ms = empty_now_ms;
                }
                empty_wait_ms =
                    empty_now_ms - s_speaker_ctx.jitter_empty_started_ms;
            } else {
                s_speaker_ctx.jitter_empty_started_ms = 0U;
            }
            if (nuwax_speaker_jitter_should_enter_rebuffer(
                    playback_active,
                    s_speaker_ctx.jitter_rebuffering,
                    queue_waiting,
                    empty_wait_ms,
                    NUWAX_SPEAKER_JITTER_EMPTY_GRACE_MS)) {
                s_speaker_ctx.jitter_rebuffering = true;
                s_speaker_ctx.jitter_rebuffer_started_ms = empty_now_ms;
                s_speaker_ctx.jitter_empty_started_ms = 0U;
                s_speaker_ctx.jitter_rebuffer_entry_pcm_frames =
                    s_speaker_ctx.stream.pcm_frames;
                s_speaker_ctx.jitter_resume_target_ms =
                    nuwax_speaker_jitter_resume_target_with_gap(
                        s_speaker_ctx.jitter_rebuffer_events,
                        NUWAX_SPEAKER_JITTER_RESUME_MIN_MS,
                        NUWAX_SPEAKER_JITTER_RESUME_MAX_MS,
                        s_speaker_ctx.ingress_max_gap_ms,
                        NUWAX_SPEAKER_JITTER_RESUME_GAP_CUSHION_MS);
                resume_target_ms = s_speaker_ctx.jitter_resume_target_ms;
                entry_pcm_frames =
                    s_speaker_ctx.jitter_rebuffer_entry_pcm_frames;
                ingress_gap_ms = s_speaker_ctx.ingress_max_gap_ms;
                confirmed_rebuffers = s_speaker_ctx.jitter_rebuffer_events;
                entered_rebuffer = true;
            }
            speaker_unlock();
            if (entered_rebuffer) {
                ESP_LOGW(TAG,
                         "speaker jitter empty wait entered generation=%lu resume_target_ms=%u queue_waiting=%lu pcm_frames=%lu empty_grace_ms=%lu ingress_max_gap_ms=%lu confirmed_rebuffers=%lu",
                         (unsigned long)playback_generation,
                         (unsigned)resume_target_ms,
                         (unsigned long)queue_waiting,
                         (unsigned long)entry_pcm_frames,
                         (unsigned long)empty_wait_ms,
                         (unsigned long)ingress_gap_ms,
                         (unsigned long)confirmed_rebuffers);
            }
            continue;
        }

        speaker_lock();
        s_speaker_ctx.jitter_empty_started_ms = 0U;
        speaker_unlock();

        if (!speaker_packet_is_submitted(packet.connection_epoch,
                                         packet.generation)) {
            ESP_LOGI(TAG,
                     "speaker dropped stale dequeued packet seq=%u generation=%lu",
                     (unsigned)packet.sequence,
                     (unsigned long)packet.generation);
            continue;
        }

        if ((packet.flags & NUWAX_AUDIO_FLAG_START) != 0) {
            if (s_speaker_ctx.callbacks.on_start_pending) {
                s_speaker_ctx.callbacks.on_start_pending(
                    packet.connection_epoch,
                    packet.generation);
            }
            if (!speaker_wait_for_start_authorization(&packet)) {
                ESP_LOGW(TAG,
                         "speaker dropped START before physical playback seq=%u generation=%lu",
                         (unsigned)packet.sequence,
                         (unsigned long)packet.generation);
                continue;
            }
            uint32_t startup_wait_ms = 0U;
            if (!speaker_wait_for_jitter_watermark(&packet,
                                                   false,
                                                   (uint32_t)packet.sample_count,
                                                   &startup_wait_ms) ||
                !speaker_activate_start_packet(&packet,
                                               startup_wait_ms)) {
                ESP_LOGI(TAG,
                         "speaker dropped superseded START seq=%u generation=%lu",
                         (unsigned)packet.sequence,
                         (unsigned long)packet.generation);
                continue;
            }
        } else if (!speaker_packet_is_active(packet.connection_epoch,
                                             packet.generation)) {
            ESP_LOGI(TAG,
                     "speaker dropped packet before generation activation seq=%u generation=%lu",
                     (unsigned)packet.sequence,
                     (unsigned long)packet.generation);
            continue;
        }

        if (packet.sample_count > 0) {
            speaker_lock();
            if (s_speaker_ctx.playback_active &&
                speaker_generation_source_matches_locked(
                    packet.connection_epoch,
                    packet.generation) &&
                s_speaker_ctx.playback_connection_epoch ==
                    packet.connection_epoch &&
                nuwax_speaker_generation_is_current(
                    s_speaker_ctx.submitted_generation,
                    s_speaker_ctx.playback_generation,
                    s_speaker_ctx.drop_until_end,
                    packet.generation)) {
                s_speaker_ctx.queued_frames++;
            }
            speaker_unlock();
        }

        const uint32_t now_ms = monotonic_ms();
        char fade_source[sizeof(s_speaker_ctx.fade_cancel_source)] = {0};
        uint32_t fade_generation = 0;
        if (speaker_fade_cancel_due(now_ms,
                                    fade_source,
                                    sizeof(fade_source),
                                    &fade_generation)) {
            const bool cancelled = speaker_cancel_now_if_current_epoch(
                packet.connection_epoch,
                fade_generation,
                fade_source[0] ? fade_source : "fade_deadline");
            if (cancelled) {
                speaker_note_cancelled_frame(packet.connection_epoch,
                                             packet.sequence,
                                             packet.flags);
            }
            continue;
        }
        if (speaker_get_drop_state(now_ms)) {
            speaker_note_cancelled_frame(packet.connection_epoch,
                                         packet.sequence,
                                         packet.flags);
            continue;
        }

        if (packet.sample_count == 0) {
            bool fade_active = false;
            if (speaker_apply_fade_cancel(&packet,
                                          fade_source,
                                          sizeof(fade_source),
                                          &fade_active)) {
                (void)speaker_cancel_now_if_current_epoch(
                    packet.connection_epoch,
                    packet.generation,
                    fade_source[0] ? fade_source : "fade_end_marker");
                continue;
            }
            if ((packet.flags & NUWAX_AUDIO_FLAG_END) != 0) {
                uint32_t played = 0;
                uint32_t failures = 0;
                uint32_t queued = 0;
                uint32_t queue_drops = 0;
                speaker_lock();
                played = s_speaker_ctx.played_frames;
                failures = s_speaker_ctx.write_failures;
                queued = s_speaker_ctx.queued_frames;
                queue_drops = s_speaker_ctx.queue_dropped_frames;
                speaker_unlock();
                 speaker_finish_playback_if_current(packet.connection_epoch,
                                                   packet.generation,
                                                   packet.sequence,
                                                   queued,
                                                   played,
                                                   failures,
                                                   queue_drops,
                                                   "playback_end_marker");
                speaker_log_task_stack_watermark("playback_end");
            } else {
                ESP_LOGW(TAG,
                         "speaker ignored empty playback packet seq=%u flags=0x%02x generation=%lu",
                         (unsigned)packet.sequence,
                         packet.flags,
                         (unsigned long)packet.generation);
            }
            continue;
        }

        bool fade_active = false;
        const bool fade_cancel_after_write =
            speaker_apply_fade_cancel(&packet,
                                      fade_source,
                                      sizeof(fade_source),
                                      &fade_active);
        bool apply_rebuffer_fade = false;
        speaker_lock();
        if (!fade_active && s_speaker_ctx.jitter_resume_fade_pending &&
            s_speaker_ctx.playback_generation == packet.generation &&
            s_speaker_ctx.playback_connection_epoch ==
                packet.connection_epoch) {
            s_speaker_ctx.jitter_resume_fade_pending = false;
            apply_rebuffer_fade = true;
        }
        speaker_unlock();
        if (apply_rebuffer_fade) {
            speaker_apply_rebuffer_fade_in(&packet);
        }
        const bool measure_start_latency =
            (packet.flags & NUWAX_AUDIO_FLAG_START) != 0 &&
            packet.start_handler_entry_us > 0 &&
            packet.enqueue_us > 0;
        int64_t i2s_write_begin_us = 0;
        esp_err_t err = ESP_OK;
            const bool io_claimed = speaker_claim_packet_io(
                packet.connection_epoch,
                packet.generation);
        if (io_claimed) {
            if ((packet.flags & NUWAX_AUDIO_FLAG_START) != 0) {
                if (s_speaker_ctx.callbacks.on_tts_start) {
                    s_speaker_ctx.callbacks.on_tts_start(
                        packet.connection_epoch,
                        packet.generation);
                }
                nuwax_p3_software_ref_reset();
            }
            err = ensure_speaker_output_ready();
            if ((packet.flags & NUWAX_AUDIO_FLAG_START) != 0U) {
                speaker_log_task_stack_watermark(
                    err == ESP_OK ? "first_pcm_ready" : "first_pcm_init_failed");
            }
            if (err == ESP_OK) {
                i2s_write_begin_us = esp_timer_get_time();
                err = nuwax_speaker_output_write_pcm16(
                    &s_speaker_ctx.output,
                    packet.samples,
                    packet.sample_count,
                    CONFIG_NUWAX_SPEAKER_WRITE_TIMEOUT_MS);
            }
            speaker_release_packet_io(packet.generation);
        } else {
            err = ESP_ERR_INVALID_STATE;
        }
        if (io_claimed && err == ESP_OK && !fade_active &&
            (packet.flags & NUWAX_AUDIO_FLAG_START) != 0U &&
            s_speaker_ctx.callbacks.on_first_pcm_written) {
            s_speaker_ctx.callbacks.on_first_pcm_written(
                packet.connection_epoch,
                packet.generation,
                esp_timer_get_time());
        }
        const int64_t i2s_write_done_us = measure_start_latency ? esp_timer_get_time() : 0;
        int64_t playback_begin_rx_us = 0;
        int64_t first_pcm_rx_us = 0;
        int64_t start_gate_authorized_us = 0;
        int64_t startup_watermark_ready_us = 0;
        uint32_t startup_target_ms = 0U;
        bool publish_speaking = false;
        if (io_claimed && err == ESP_OK && !fade_active) {
            speaker_lock();
            if (measure_start_latency) {
                s_speaker_ctx.first_i2s_write_us = i2s_write_begin_us;
                playback_begin_rx_us = s_speaker_ctx.playback_begin_rx_us;
                first_pcm_rx_us = s_speaker_ctx.first_pcm_rx_us;
                start_gate_authorized_us =
                    s_speaker_ctx.start_gate_authorized_us;
                startup_watermark_ready_us =
                    s_speaker_ctx.startup_watermark_ready_us;
                startup_target_ms = s_speaker_ctx.jitter_startup_target_ms;
            }
            publish_speaking = speaker_should_publish_speaking_locked(
                packet.connection_epoch, packet.generation);
            speaker_unlock();
        }
        if (publish_speaking) {
            if (s_speaker_ctx.callbacks.set_state_fast) {
                s_speaker_ctx.callbacks.set_state_fast(
                    packet.connection_epoch,
                    packet.generation,
                    NUWAX_APP_STATE_SPEAKING);
            } else if (s_speaker_ctx.callbacks.set_state) {
                s_speaker_ctx.callbacks.set_state(
                    packet.connection_epoch,
                    packet.generation,
                    NUWAX_APP_STATE_SPEAKING);
            }
        }
        if (measure_start_latency) {
            ESP_LOGI(TAG,
                     "speaker start latency generation=%lu seq=%u handler_to_enqueue_us=%lld enqueue_to_i2s_begin_us=%lld i2s_write_us=%lld handler_to_i2s_done_us=%lld result=%s",
                     (unsigned long)packet.generation,
                     (unsigned)packet.sequence,
                     (long long)(packet.enqueue_us - packet.start_handler_entry_us),
                     i2s_write_begin_us > 0
                         ? (long long)(i2s_write_begin_us - packet.enqueue_us)
                         : -1LL,
                     i2s_write_begin_us > 0
                         ? (long long)(i2s_write_done_us - i2s_write_begin_us)
                         : -1LL,
                     (long long)(i2s_write_done_us - packet.start_handler_entry_us),
                      esp_err_to_name(err));
            ESP_LOGI(
                TAG,
                "speaker latency breakdown generation=%lu playback_begin_to_first_pcm_ms=%lld first_pcm_to_gate_ms=%lld first_pcm_to_watermark_ms=%lld watermark_to_i2s_ms=%lld first_pcm_to_i2s_ms=%lld playback_begin_to_i2s_ms=%lld startup_target_ms=%lu",
                (unsigned long)packet.generation,
                playback_begin_rx_us > 0 && first_pcm_rx_us >= playback_begin_rx_us
                    ? (long long)((first_pcm_rx_us - playback_begin_rx_us) / 1000LL)
                    : -1LL,
                first_pcm_rx_us > 0 &&
                        start_gate_authorized_us >= first_pcm_rx_us
                    ? (long long)((start_gate_authorized_us - first_pcm_rx_us) /
                                  1000LL)
                    : -1LL,
                first_pcm_rx_us > 0 &&
                        startup_watermark_ready_us >= first_pcm_rx_us
                    ? (long long)((startup_watermark_ready_us - first_pcm_rx_us) /
                                  1000LL)
                    : -1LL,
                startup_watermark_ready_us > 0 &&
                        i2s_write_begin_us >= startup_watermark_ready_us
                    ? (long long)((i2s_write_begin_us -
                                   startup_watermark_ready_us) /
                                  1000LL)
                    : -1LL,
                first_pcm_rx_us > 0 && i2s_write_begin_us >= first_pcm_rx_us
                    ? (long long)((i2s_write_begin_us - first_pcm_rx_us) /
                                  1000LL)
                    : -1LL,
                playback_begin_rx_us > 0 &&
                        i2s_write_begin_us >= playback_begin_rx_us
                    ? (long long)((i2s_write_begin_us - playback_begin_rx_us) /
                                  1000LL)
                    : -1LL,
                (unsigned long)startup_target_ms);
        }
        bool publish_progress = false;
        uint32_t progress_ms = 0U;
        uint64_t played_bytes = 0U;
        speaker_lock();
        const bool write_result_current =
            !s_speaker_ctx.stream_protocol_faulted &&
            speaker_generation_source_matches_locked(packet.connection_epoch,
                                                     packet.generation) &&
            s_speaker_ctx.playback_connection_epoch ==
                packet.connection_epoch &&
            nuwax_speaker_generation_is_current(
                s_speaker_ctx.submitted_generation,
                s_speaker_ctx.playback_generation,
                s_speaker_ctx.drop_until_end,
                packet.generation);
        if (write_result_current && err == ESP_OK) {
            s_speaker_ctx.played_frames++;
            s_speaker_ctx.played_bytes +=
                (uint64_t)packet.sample_count * sizeof(int16_t);
            progress_ms = monotonic_ms();
            s_speaker_ctx.playback_last_progress_ms = progress_ms;
            played_bytes = s_speaker_ctx.played_bytes;
            if (s_speaker_ctx.callbacks.on_playback_progress &&
                (s_speaker_ctx.progress_callback_ms == 0U ||
                 elapsed_ms(progress_ms,
                            s_speaker_ctx.progress_callback_ms +
                                NUWAX_SPEAKER_PROGRESS_CALLBACK_INTERVAL_MS))) {
                s_speaker_ctx.progress_callback_ms = progress_ms;
                publish_progress = true;
            }
        } else if (write_result_current) {
            s_speaker_ctx.write_failures++;
        }
        uint32_t played = s_speaker_ctx.played_frames;
        uint32_t failures = s_speaker_ctx.write_failures;
        uint32_t queued = s_speaker_ctx.queued_frames;
        uint32_t queue_drops = s_speaker_ctx.queue_dropped_frames;
        uint32_t queue_waiting = speaker_packet_ring_count();
        uint32_t queue_audio_ms = nuwax_speaker_jitter_audio_ms(
            speaker_packet_ring_samples(), packet.sample_rate);
        if (write_result_current && err == ESP_OK &&
            !s_speaker_ctx.stream.end_accepted &&
            queue_audio_ms < s_speaker_ctx.jitter_low_watermark_ms) {
            s_speaker_ctx.jitter_low_watermark_ms = queue_audio_ms;
        }
        speaker_unlock();

        if (publish_progress &&
            s_speaker_ctx.callbacks.on_playback_progress) {
            s_speaker_ctx.callbacks.on_playback_progress(
                                                         packet.connection_epoch,
                                                         packet.generation,
                                                         played,
                                                         played_bytes,
                                                         progress_ms);
        }

        if (err != ESP_OK && write_result_current) {
            ESP_LOGE(TAG,
                      "speaker async write failed seq=%u err=%s failures=%lu queue_waiting=%u",
                      (unsigned)packet.sequence,
                      esp_err_to_name(err),
                      (unsigned long)failures,
                      (unsigned)queue_waiting);
            speaker_notify_playback_fault(packet.connection_epoch,
                                          packet.generation,
                                          err);
            const bool stopped = speaker_cancel_now_if_current_epoch(
                packet.connection_epoch,
                packet.generation,
                "playback_output_failure");
            if (!stopped) {
                ESP_LOGE(TAG,
                         "speaker output failure stop incomplete generation=%lu err=%s",
                         (unsigned long)packet.generation,
                         esp_err_to_name(err));
            }
            continue;
        } else if (err != ESP_OK) {
            ESP_LOGI(TAG,
                     "speaker stale write result ignored seq=%u generation=%lu err=%s",
                     (unsigned)packet.sequence,
                     (unsigned long)packet.generation,
                     esp_err_to_name(err));
        }
        if (fade_cancel_after_write && io_claimed) {
            (void)speaker_cancel_now_if_current_epoch(
                packet.connection_epoch,
                packet.generation,
                fade_source[0] ? fade_source : "fade_complete");
            continue;
        }
        if ((packet.flags & NUWAX_AUDIO_FLAG_END) != 0) {
            speaker_finish_playback_if_current(packet.connection_epoch,
                                               packet.generation,
                                               packet.sequence,
                                               queued,
                                               played,
                                               failures,
                                               queue_drops,
                                               "playback_done");
        }
    }
#undef rebuffer_ref
#undef packet
}
#else
esp_err_t nuwax_speaker_playback_init(const nuwax_speaker_playback_callbacks_t *callbacks) {
    (void)callbacks;
    return ESP_OK;
}

void nuwax_speaker_playback_set_downlink_admission(uint32_t epoch,
                                                   bool allowed) {
    (void)epoch;
    (void)allowed;
}

esp_err_t nuwax_speaker_playback_handle_audio_frame_admitted(
    uint32_t connection_epoch,
    uint32_t admission_epoch,
    uint32_t sequence,
    uint8_t flags,
    uint16_t sample_rate,
    uint8_t channels,
    const int16_t *samples,
    size_t sample_count) {
    return nuwax_speaker_playback_handle_audio_frame_admitted_with_timeout(
        connection_epoch,
        admission_epoch,
        sequence,
        flags,
        sample_rate,
        channels,
        samples,
        sample_count,
        NUWAX_SPEAKER_RX_ADMISSION_BUDGET_MS);
}

esp_err_t nuwax_speaker_playback_handle_audio_frame_admitted_with_timeout(
    uint32_t connection_epoch,
    uint32_t admission_epoch,
    uint32_t sequence,
    uint8_t flags,
    uint16_t sample_rate,
    uint8_t channels,
    const int16_t *samples,
    size_t sample_count,
    uint32_t admission_timeout_ms) {
    (void)connection_epoch;
    (void)admission_epoch;
    (void)admission_timeout_ms;
    return nuwax_speaker_playback_handle_audio_frame(sequence,
                                                     flags,
                                                     sample_rate,
                                                     channels,
                                                     samples,
                                                     sample_count);
}

void nuwax_speaker_playback_task(void *arg) {
    (void)arg;
}

bool nuwax_speaker_playback_has_pending(void) {
    return false;
}

bool nuwax_speaker_playback_request_cancel_generation_ex(
    uint32_t expected_generation,
    const char *source,
    uint32_t fade_ms,
    nuwax_speaker_cancel_request_result_t *result) {
    (void)source;
    (void)fade_ms;
    if (result) {
        memset(result, 0, sizeof(*result));
        result->status = NUWAX_SPEAKER_CANCEL_REQUEST_REJECTED;
        result->playback_generation = expected_generation;
        result->completed_stop_result = ESP_ERR_INVALID_STATE;
        result->failed_stop_result = ESP_ERR_INVALID_STATE;
    }
    return false;
}

bool nuwax_speaker_playback_request_scoped_stop_generation_ex(
    uint32_t expected_connection_epoch,
    uint32_t expected_generation,
    bool preserve_natural_drain,
    const char *source,
    uint32_t fade_ms,
    nuwax_speaker_cancel_request_result_t *result) {
    (void)expected_connection_epoch;
    (void)preserve_natural_drain;
    return nuwax_speaker_playback_request_cancel_generation_ex(
        expected_generation, source, fade_ms, result);
}

bool nuwax_speaker_playback_session_active(void) {
    return false;
}

bool nuwax_speaker_playback_output_ready(void) {
    return false;
}

void nuwax_speaker_playback_get_stats(nuwax_speaker_playback_stats_t *stats) {
    if (stats) {
        memset(stats, 0, sizeof(*stats));
    }
}

void nuwax_speaker_playback_cancel_now(const char *source) {
    (void)source;
}

bool nuwax_speaker_playback_request_cancel_generation(uint32_t expected_generation,
                                                      const char *source,
                                                      uint32_t fade_ms) {
    (void)expected_generation;
    (void)source;
    (void)fade_ms;
    return false;
}

bool nuwax_speaker_playback_request_probe_duck_generation(
    uint32_t expected_generation,
    uint8_t attenuation_db,
    uint16_t duration_ms) {
    (void)expected_generation;
    (void)attenuation_db;
    (void)duration_ms;
    return false;
}

void nuwax_speaker_playback_request_fade_cancel(const char *source, uint32_t fade_ms) {
    (void)source;
    (void)fade_ms;
}

void nuwax_speaker_playback_fade_cancel(const char *source, uint32_t fade_ms) {
    (void)source;
    (void)fade_ms;
}

void nuwax_speaker_playback_prepare_for_tts(const char *reason) {
    (void)reason;
}

void nuwax_speaker_playback_note_notification_begin(const char *notify_id,
                                                    const char *playback_id,
                                                    uint32_t frames,
                                                    uint32_t bytes) {
    (void)notify_id;
    (void)playback_id;
    (void)frames;
    (void)bytes;
}

bool nuwax_speaker_playback_abort_notification_metadata_only_session(
    const char *notify_id,
    const char *playback_id,
    const char *reason) {
    (void)notify_id;
    (void)playback_id;
    (void)reason;
    return false;
}

void nuwax_speaker_playback_note_notification_end(const char *notify_id,
                                                  const char *playback_id,
                                                  const char *reason) {
    (void)notify_id;
    (void)playback_id;
    (void)reason;
}

void nuwax_speaker_playback_release_idle_output(const char *reason) {
    (void)reason;
}

esp_err_t nuwax_speaker_playback_reset_runtime(void) {
    return ESP_OK;
}

void nuwax_speaker_playback_reset_jitter_learning(void) {
}

esp_err_t nuwax_speaker_playback_deinit_output(void) {
    return ESP_OK;
}

void nuwax_speaker_playback_note_playback_begin(const char *turn_id,
                                                const char *playback_id,
                                                uint32_t frames,
                                                uint32_t bytes) {
    (void)turn_id;
    (void)playback_id;
    (void)frames;
    (void)bytes;
}

bool nuwax_speaker_playback_abort_metadata_only_session(
    const char *turn_id,
    const char *playback_id,
    const char *reason) {
    (void)turn_id;
    (void)playback_id;
    (void)reason;
    return false;
}

void nuwax_speaker_playback_note_playback_end(const char *turn_id,
                                              const char *playback_id,
                                              const char *reason) {
    (void)turn_id;
    (void)playback_id;
    (void)reason;
}

void nuwax_speaker_playback_apply_volume_percent(int volume_percent, bool persist) {
    (void)volume_percent;
    (void)persist;
}

void nuwax_speaker_playback_preview_volume_percent(int volume_percent) {
    (void)volume_percent;
}

void nuwax_speaker_playback_apply_current_volume(void) {
}

uint8_t nuwax_speaker_playback_get_volume_percent(void) {
    return 0;
}

esp_err_t nuwax_speaker_playback_handle_audio_frame(uint32_t sequence,
                                                    uint8_t flags,
                                                    uint16_t sample_rate,
                                                    uint8_t channels,
                                                    const int16_t *samples,
                                                    size_t sample_count) {
    (void)sequence;
    (void)flags;
    (void)sample_rate;
    (void)channels;
    (void)samples;
    (void)sample_count;
    return ESP_OK;
}

esp_err_t nuwax_speaker_playback_authorize_generation(
    uint32_t generation,
    const char *turn_id,
    const char *playback_id) {
    (void)generation;
    (void)turn_id;
    (void)playback_id;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t nuwax_speaker_playback_authorize_notification_generation(
    uint32_t generation,
    const char *notify_id,
    const char *playback_id) {
    (void)generation;
    (void)notify_id;
    (void)playback_id;
    return ESP_ERR_NOT_SUPPORTED;
}

bool nuwax_speaker_playback_reject_start_generation(uint32_t expected_generation,
                                                    const char *source) {
    (void)expected_generation;
    (void)source;
    return false;
}
#endif
