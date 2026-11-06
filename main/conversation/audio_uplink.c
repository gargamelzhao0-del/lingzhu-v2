#include "audio_uplink.h"
#include "ws_send_failure_policy.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "audio_frame.h"
#include "audio_runtime_memory_policy.h"
#include "conversation/audio_terminal_recovery.h"
#include "conversation/audio_uplink_terminal.h"
#include "conversation/realtime_log.h"
#include "device_error_report.h"
#include "diagnostics.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "session_protocol.h"

static const char *TAG = "nuwax_audio_uplink";

static void report_audio_uplink_fault(const char *code, esp_err_t result) {
    (void)nuwax_device_error_report_enqueue(
        "error", "audio_uplink", code, result, NULL, 0U, NULL);
}

static void report_audio_uplink_warning(const char *code, esp_err_t result) {
    (void)nuwax_device_warning_report_enqueue(
        "audio_uplink", code, result, NULL, 0U, NULL);
}

#define NUWAX_AUDIO_UPLINK_SEND_TASK_STACK_BYTES 10240
#define NUWAX_AUDIO_UPLINK_SEND_TASK_PRIORITY 9
#define NUWAX_AUDIO_UPLINK_SEND_TASK_CORE 1
#define NUWAX_AUDIO_UPLINK_QUEUE_FINAL_WAIT_MS 2000
#define NUWAX_AUDIO_UPLINK_REALTIME_FINISH_TIMEOUT_MS 12000U
#define NUWAX_AUDIO_UPLINK_RELIABLE_FINISH_TIMEOUT_MS 10000U
#define NUWAX_AUDIO_UPLINK_ABORT_WAIT_MS 1000
#define NUWAX_AUDIO_UPLINK_LOCAL_POWER_PREEMPT_WAIT_MS 1500U
#define NUWAX_AUDIO_UPLINK_REAPER_STACK_BYTES 4096
#define NUWAX_AUDIO_UPLINK_REAPER_PRIORITY 4
#define NUWAX_AUDIO_UPLINK_REAPER_CANCEL_GRACE_MS 250U
#define NUWAX_AUDIO_UPLINK_REAPER_NO_PROGRESS_MS 2000U
#define NUWAX_AUDIO_UPLINK_REAPER_CANCEL_LIMIT_MS 5000U
#define NUWAX_AUDIO_UPLINK_REAPER_DIAGNOSTIC_INTERVAL_MS 2000U
#define NUWAX_AUDIO_UPLINK_REAPER_FATAL_RESTART_MS 90000U
#define NUWAX_AUDIO_UPLINK_TASK_SUSPEND_SETTLE_MS 100
#define NUWAX_AUDIO_UPLINK_TASK_START_TIMEOUT_MS 100
#define NUWAX_AUDIO_UPLINK_IDLE_FALLBACK_WAIT_MS 500
#define NUWAX_AUDIO_UPLINK_SEND_SLOW_WARN_MS 180
#define NUWAX_AUDIO_UPLINK_SEND_SLOW_LOG_INTERVAL_MS 5000U
#define NUWAX_AUDIO_UPLINK_DROP_LOG_INTERVAL_MS 1000U
#define NUWAX_AUDIO_UPLINK_TERMINAL_RETRY_ATTEMPTS 3U
#define NUWAX_AUDIO_UPLINK_TERMINAL_RETRY_DELAY_MS 20U

_Static_assert(NUWAX_AUDIO_UPLINK_PCM_FRAME_SAMPLES <= CONFIG_NUWAX_VOICE_MAX_SAMPLES_PER_FRAME,
               "configured PCM frame exceeds voice frame capacity");
_Static_assert(CONFIG_NUWAX_VOICE_SAMPLE_RATE == 16000,
               "formal cloud uplink requires a 16 kHz sample rate");
_Static_assert(CONFIG_NUWAX_VOICE_CHANNELS == 1,
               "formal cloud uplink requires mono audio");
_Static_assert(NUWAX_AUDIO_UPLINK_LOCAL_POWER_PREEMPT_WAIT_MS >=
                   NUWAX_AUDIO_UPLINK_REALTIME_RECOVERY_MAX_AGE_MS,
               "local power preempt must cover an in-flight realtime packet");

static void audio_uplink_refresh_adaptive_batch(
    nuwax_audio_uplink_t *ctx) {
    if (!ctx || !ctx->low_latency_enabled || !ctx->ready_packets) {
        return;
    }
    ctx->adaptive_batch_frames = (uint8_t)
        nuwax_audio_uplink_adaptive_batch_frames(
            (uint32_t)uxQueueMessagesWaiting(ctx->ready_packets),
            ctx->queue_depth);
}
_Static_assert(CONFIG_NUWAX_VOICE_FRAME_DURATION_MS == 20,
               "formal cloud uplink requires 20 ms audio frames");

typedef struct {
    uint8_t slot;
    size_t len;
    uint32_t start_seq;
    uint32_t frames;
    bool final;
    int64_t enqueued_us;
} nuwax_audio_uplink_packet_t;

static esp_err_t audio_uplink_ensure_start_on_first_sent_packet(
    nuwax_audio_uplink_t *ctx,
    const nuwax_audio_uplink_packet_t *packet) {
    if (!ctx || !packet || !ctx->pool || packet->len < NUWAX_AUDIO_FRAME_HEADER_BYTES ||
        packet->slot >= ctx->pool_slot_count) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ctx->sent_batches > 0U) {
        return ESP_OK;
    }
    return nuwax_audio_uplink_ensure_batch_start(
        &ctx->pool[(size_t)packet->slot * NUWAX_AUDIO_UPLINK_PACKET_BYTES],
        packet->len);
}

static uint8_t s_audio_upload_batch[NUWAX_AUDIO_UPLINK_PACKET_BYTES];
static StaticTask_t s_audio_uplink_task_tcb;
static portMUX_TYPE s_audio_uplink_task_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_audio_uplink_task_in_use;
static nuwax_audio_uplink_t *s_audio_uplink_owner;
static StaticTask_t s_audio_uplink_reaper_task_tcb;
/* Allocation-free ownership transfer. Nodes belong to their heap contexts;
 * the lock protects only pointer publication, never task deletion or I/O. */
static portMUX_TYPE s_audio_uplink_cleanup_lock = portMUX_INITIALIZER_UNLOCKED;
static nuwax_audio_uplink_t *s_audio_uplink_cleanup_head;
static nuwax_audio_uplink_t *s_audio_uplink_cleanup_tail;
static TaskHandle_t s_audio_uplink_reaper_task;
static StackType_t *s_audio_uplink_reaper_stack;
static _Atomic int s_audio_uplink_reaper_init_state;

static bool audio_uplink_flag_load(const _Atomic bool *flag) {
    return atomic_load_explicit(flag, memory_order_acquire);
}

static void audio_uplink_flag_store(_Atomic bool *flag, bool value) {
    atomic_store_explicit(flag, value, memory_order_release);
}

static esp_err_t audio_uplink_result_load(const nuwax_audio_uplink_t *ctx) {
    return ctx ? (esp_err_t)atomic_load_explicit(&ctx->send_result,
                                                 memory_order_acquire)
               : ESP_ERR_INVALID_ARG;
}

static void audio_uplink_result_store_first_error(nuwax_audio_uplink_t *ctx,
                                                  esp_err_t result) {
    if (!ctx || result == ESP_OK) {
        return;
    }
    int expected = ESP_OK;
    (void)atomic_compare_exchange_strong_explicit(&ctx->send_result,
                                                  &expected,
                                                  (int)result,
                                                  memory_order_release,
                                                  memory_order_acquire);
}

static uint32_t audio_uplink_now_ms(void) {
    return (uint32_t)((uint64_t)esp_timer_get_time() / 1000ULL);
}

/* Queue metadata is normally external-memory backed.  A fallback into
 * internal RAM is safe only while the same reserve used by capture/DMA
 * admission is intact; otherwise TLS, I2S and wake-word allocations compete
 * with a nonessential audio queue and can fail later in less recoverable
 * places. */
static bool audio_uplink_internal_queue_fallback_allowed(const char *name) {
    const nuwax_audio_runtime_memory_t memory = {
        .internal_free = heap_caps_get_free_size(
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        .internal_largest = heap_caps_get_largest_free_block(
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        .dma_largest = heap_caps_get_largest_free_block(
            MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
    };
    const bool allowed =
        nuwax_audio_runtime_memory_classify(memory) ==
        NUWAX_AUDIO_RUNTIME_MEMORY_READY;
    if (!allowed) {
        ESP_LOGW(TAG,
                 "audio uplink queue internal fallback refused name=%s internal=%u largest=%u dma_largest=%u",
                 name ? name : "-",
                 (unsigned)memory.internal_free,
                 (unsigned)memory.internal_largest,
                 (unsigned)memory.dma_largest);
    }
    return allowed;
}

static void audio_uplink_log_realtime_drop(
    nuwax_audio_uplink_t *ctx,
    const nuwax_audio_uplink_packet_t *packet,
    uint64_t queue_wait_us,
    const char *reason) {
    if (!ctx || !packet) {
        return;
    }
    const uint32_t now_ms = audio_uplink_now_ms();
    if (ctx->realtime_drop_log_ms != 0U &&
        (uint32_t)(now_ms - ctx->realtime_drop_log_ms) <
            NUWAX_AUDIO_UPLINK_DROP_LOG_INTERVAL_MS) {
        ctx->realtime_drop_log_suppressed_batches++;
        ctx->realtime_drop_log_suppressed_frames += packet->frames;
        if (queue_wait_us > ctx->realtime_drop_log_max_queue_wait_us) {
            ctx->realtime_drop_log_max_queue_wait_us = queue_wait_us;
        }
        return;
    }

    ESP_LOGW(TAG,
             "realtime mic data skipped reason=%s start_seq=%lu frames=%lu queue_wait_us=%llu ready_depth=%u suppressed_batches=%lu suppressed_frames=%lu suppressed_max_queue_wait_us=%llu",
             reason ? reason : "bounded_latency",
             (unsigned long)packet->start_seq,
             (unsigned long)packet->frames,
             (unsigned long long)queue_wait_us,
             ctx->ready_packets
                 ? (unsigned)uxQueueMessagesWaiting(ctx->ready_packets)
                 : 0U,
             (unsigned long)ctx->realtime_drop_log_suppressed_batches,
             (unsigned long)ctx->realtime_drop_log_suppressed_frames,
             (unsigned long long)
                 ctx->realtime_drop_log_max_queue_wait_us);
    char result_detail[NUWAX_DEVICE_ERROR_RESULT_BYTES];
    snprintf(result_detail,
             sizeof(result_detail),
             "ESP_ERR_TIMEOUT;reason=%s;seq=%lu;frames=%lu;wait_us=%llu;depth=%u;sup=%lu/%lu",
             reason ? reason : "bounded_latency",
             (unsigned long)packet->start_seq,
             (unsigned long)packet->frames,
             (unsigned long long)queue_wait_us,
             ctx->ready_packets ? (unsigned)uxQueueMessagesWaiting(ctx->ready_packets) : 0U,
             (unsigned long)ctx->realtime_drop_log_suppressed_batches,
             (unsigned long)ctx->realtime_drop_log_suppressed_frames);
    (void)nuwax_device_warning_report_enqueue_detail(
        "audio_uplink",
        "realtime_packet_skipped",
        ESP_ERR_TIMEOUT,
        result_detail,
        NULL,
        0U,
        NULL);
    ctx->realtime_drop_log_ms = now_ms;
    ctx->realtime_drop_log_suppressed_batches = 0U;
    ctx->realtime_drop_log_suppressed_frames = 0U;
    ctx->realtime_drop_log_max_queue_wait_us = 0U;
}

static void audio_uplink_sender_stage_store(
    nuwax_audio_uplink_t *ctx,
    nuwax_audio_uplink_sender_stage_t stage,
    uint32_t sequence,
    size_t packet_bytes) {
    if (!ctx) {
        return;
    }
    atomic_store_explicit(&ctx->sender_packet_sequence, sequence,
                          memory_order_relaxed);
    atomic_store_explicit(&ctx->sender_packet_bytes,
                          packet_bytes > UINT32_MAX
                              ? UINT32_MAX
                              : (uint32_t)packet_bytes,
                          memory_order_relaxed);
    atomic_store_explicit(&ctx->sender_stage_since_ms,
                          audio_uplink_now_ms(), memory_order_relaxed);
    atomic_store_explicit(&ctx->sender_stage, (int)stage,
                          memory_order_release);
}

static bool audio_uplink_request_integrity_close(
    nuwax_audio_uplink_t *ctx,
    const char *reason) {
    if (!ctx || !ctx->ws || ctx->ws_connection_epoch == 0U) {
        return false;
    }
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(
            &ctx->integrity_close_requested,
            &expected,
            true,
            memory_order_acq_rel,
            memory_order_acquire)) {
        return true;
    }
    const bool admitted = nuwax_ws_close_if_epoch(
        ctx->ws,
        ctx->ws_connection_epoch,
        NUWAX_WS_CLOSE_CAUSE_TRANSPORT_INTEGRITY);
    if (!admitted &&
        nuwax_ws_connection_epoch(ctx->ws) == ctx->ws_connection_epoch) {
        atomic_store_explicit(&ctx->integrity_close_requested, false,
                              memory_order_release);
    }
    ESP_LOGW(TAG,
             "audio uplink transport-integrity close %s epoch=%lu reason=%s",
             admitted ? "admitted" : "not_needed_or_stale",
             (unsigned long)ctx->ws_connection_epoch,
             reason ? reason : "-");
    return admitted;
}

static const char *audio_uplink_sender_stage_name(int stage) {
    switch ((nuwax_audio_uplink_sender_stage_t)stage) {
        case NUWAX_AUDIO_UPLINK_SENDER_IDLE:
            return "idle";
        case NUWAX_AUDIO_UPLINK_SENDER_WAIT_PACKET:
            return "wait_packet";
        case NUWAX_AUDIO_UPLINK_SENDER_LISTEN_START:
            return "listen_start";
        case NUWAX_AUDIO_UPLINK_SENDER_PREPARE_PACKET:
            return "prepare_packet";
        case NUWAX_AUDIO_UPLINK_SENDER_SEND_PACKET:
            return "send_packet";
        case NUWAX_AUDIO_UPLINK_SENDER_AFTER_END:
            return "after_end";
        case NUWAX_AUDIO_UPLINK_SENDER_EXITED:
            return "exited";
        default:
            return "invalid";
    }
}

static void audio_uplink_log_stalled_sender(
    const nuwax_audio_uplink_t *ctx,
    const char *recovery_stage,
    uint32_t elapsed_ms) {
    if (!ctx) {
        return;
    }
    const int sender_stage = atomic_load_explicit(
        &ctx->sender_stage, memory_order_acquire);
    const uint32_t sender_stage_since_ms = (uint32_t)atomic_load_explicit(
        &ctx->sender_stage_since_ms, memory_order_relaxed);
    const uint32_t sender_sequence = (uint32_t)atomic_load_explicit(
        &ctx->sender_packet_sequence, memory_order_relaxed);
    const uint32_t sender_bytes = (uint32_t)atomic_load_explicit(
        &ctx->sender_packet_bytes, memory_order_relaxed);
    const nuwax_ws_tx_diagnostics_t ws_tx =
        nuwax_ws_tx_diagnostics(ctx->ws);
    const eTaskState task_state =
        ctx->task ? eTaskGetState(ctx->task) : eDeleted;
    const UBaseType_t stack_min_free =
        ctx->task ? uxTaskGetStackHighWaterMark(ctx->task) : 0U;
    ESP_LOGE(TAG,
             "uplink cleanup stalled recovery_stage=%s elapsed_ms=%lu epoch=%lu sender_stage=%s sender_stage_age_ms=%lu sequence=%lu packet_bytes=%lu task_state=%d stack_min_free=%u ws_tx_stage=%d ws_tx_epoch=%lu ws_tx_stage_age_ms=%lu queued=%lu sent=%lu ready_depth=%u",
             recovery_stage ? recovery_stage : "-",
             (unsigned long)elapsed_ms,
             (unsigned long)ctx->ws_connection_epoch,
             audio_uplink_sender_stage_name(sender_stage),
             (unsigned long)(audio_uplink_now_ms() -
                             sender_stage_since_ms),
             (unsigned long)sender_sequence,
             (unsigned long)sender_bytes,
             (int)task_state,
             (unsigned)stack_min_free,
             (int)ws_tx.stage,
             (unsigned long)ws_tx.epoch,
             (unsigned long)ws_tx.stage_age_ms,
             (unsigned long)ctx->queued_batches,
             (unsigned long)ctx->sent_batches,
             ctx->ready_packets
                 ? (unsigned)uxQueueMessagesWaiting(ctx->ready_packets)
                 : 0U);
    char result_detail[NUWAX_DEVICE_ERROR_RESULT_BYTES];
    snprintf(result_detail,
             sizeof(result_detail),
             "ESP_ERR_TIMEOUT;stage=%s;age=%lu;seq=%lu;bytes=%lu;queued=%lu;sent=%lu;depth=%u;stack=%u",
             recovery_stage ? recovery_stage : "-",
             (unsigned long)(audio_uplink_now_ms() - sender_stage_since_ms),
             (unsigned long)sender_sequence,
             (unsigned long)sender_bytes,
             (unsigned long)ctx->queued_batches,
             (unsigned long)ctx->sent_batches,
             ctx->ready_packets ? (unsigned)uxQueueMessagesWaiting(ctx->ready_packets) : 0U,
             (unsigned)stack_min_free);
    (void)nuwax_device_error_report_enqueue_detail(
        "error",
        "audio_uplink",
        "sender_stall",
        ESP_ERR_TIMEOUT,
        result_detail,
        NULL,
        0U,
        NULL);
}

static esp_err_t audio_uplink_arm_terminal_recovery(
    nuwax_audio_uplink_t *ctx,
    const uint8_t *packet,
    size_t packet_len,
    bool end_committed) {
    if (!ctx || !ctx->terminal_recovery_bound || !ctx->ws ||
        !nuwax_audio_uplink_listen_start_committed(ctx) ||
        ctx->ws_connection_epoch == 0U) {
        return ESP_ERR_INVALID_STATE;
    }
    return nuwax_audio_terminal_recovery_arm(
        ctx->ws,
        ctx->ws_connection_epoch,
        packet,
        packet_len,
        end_committed,
        ctx->terminal_recovery_mode,
        ctx->terminal_recovery_audio_session_id,
        ctx->terminal_recovery_request_id[0]
            ? ctx->terminal_recovery_request_id
            : NULL,
        ctx->terminal_recovery_attempt_timeout_ms);
}

static esp_err_t audio_uplink_arm_synthesized_terminal_recovery(
    nuwax_audio_uplink_t *ctx,
    uint32_t sequence) {
    if (!ctx) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t marker[NUWAX_AUDIO_FRAME_HEADER_BYTES] = {0};
    size_t marker_len = 0U;
    const esp_err_t build_err = nuwax_audio_uplink_build_terminal_marker(
        marker,
        sizeof(marker),
#if CONFIG_NUWAX_VOICE_UPLINK_CODEC_OPUS
        NUWAX_AUDIO_CODEC_OPUS,
#else
        NUWAX_AUDIO_CODEC_PCM16,
#endif
        sequence,
        ctx->sent_frames > 0U,
        CONFIG_NUWAX_VOICE_SAMPLE_RATE,
        CONFIG_NUWAX_VOICE_CHANNELS,
        CONFIG_NUWAX_VOICE_FRAME_DURATION_MS,
        &marker_len);
    if (build_err != ESP_OK) {
        return build_err;
    }
    return audio_uplink_arm_terminal_recovery(ctx,
                                              marker,
                                              marker_len,
                                              false);
}

static void audio_uplink_notify_sender(nuwax_audio_uplink_t *ctx) {
    if (ctx && ctx->task) {
        xTaskNotifyGive(ctx->task);
    }
}

static bool audio_uplink_is_reserved_terminal_slot(
    const nuwax_audio_uplink_t *ctx,
    uint8_t slot) {
    return ctx && ctx->terminal_slot_reserved &&
           slot == ctx->terminal_slot;
}

static void audio_uplink_release_packet_slot(nuwax_audio_uplink_t *ctx,
                                             uint8_t slot) {
    if (!ctx || !ctx->free_slots ||
        audio_uplink_is_reserved_terminal_slot(ctx, slot)) {
        return;
    }
    (void)xQueueSend(ctx->free_slots, &slot, 0);
}

static void audio_uplink_release_unqueued_slot(nuwax_audio_uplink_t *ctx,
                                               uint8_t slot) {
    if (audio_uplink_is_reserved_terminal_slot(ctx, slot)) {
        ctx->terminal_slot_claimed = false;
        return;
    }
    audio_uplink_release_packet_slot(ctx, slot);
}

#if CONFIG_NUWAX_VOICE_UPLINK_CODEC_OPUS
static uint8_t *s_audio_encoded_frame;
static size_t s_audio_encoded_frame_capacity;
#endif

size_t nuwax_audio_uplink_samples_per_frame(void) {
    return ((size_t)CONFIG_NUWAX_VOICE_SAMPLE_RATE * CONFIG_NUWAX_VOICE_FRAME_DURATION_MS) / 1000;
}

uint32_t nuwax_audio_uplink_batch_frames(void) {
    return NUWAX_AUDIO_UPLINK_STREAM_BATCH_FRAMES;
}

uint32_t nuwax_audio_uplink_queue_depth(void) {
    return NUWAX_AUDIO_UPLINK_RELIABLE_QUEUE_DEPTH;
}

uint32_t nuwax_audio_uplink_queue_depth_for_profile(
    nuwax_audio_uplink_profile_t profile) {
    return profile == NUWAX_AUDIO_UPLINK_PROFILE_REALTIME
               ? NUWAX_AUDIO_UPLINK_REALTIME_QUEUE_DEPTH
               : NUWAX_AUDIO_UPLINK_RELIABLE_QUEUE_DEPTH;
}

uint32_t nuwax_audio_uplink_context_queue_depth(
    const nuwax_audio_uplink_t *ctx) {
    return ctx ? ctx->queue_depth : 0U;
}

esp_err_t nuwax_audio_uplink_build_pcm16_frame(uint8_t *out,
                                               size_t out_capacity,
                                               const int16_t *samples,
                                               size_t sample_count,
                                               uint32_t sequence,
                                               uint8_t flags,
                                               size_t *frame_len) {
    const uint16_t frame_ms = CONFIG_NUWAX_VOICE_FRAME_DURATION_MS;

    return nuwax_audio_frame_build_pcm16(out,
                                         out_capacity,
                                         samples,
                                         sample_count,
                                         sequence,
                                         sequence * frame_ms,
                                         flags,
                                         CONFIG_NUWAX_VOICE_SAMPLE_RATE,
                                         CONFIG_NUWAX_VOICE_CHANNELS,
                                         frame_ms,
                                         frame_len);
}

#if CONFIG_NUWAX_VOICE_UPLINK_CODEC_OPUS
esp_err_t nuwax_audio_uplink_ensure_encoded_frame_capacity(size_t required_bytes) {
    if (required_bytes == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_audio_encoded_frame && s_audio_encoded_frame_capacity >= required_bytes) {
        return ESP_OK;
    }
    uint8_t *next = heap_caps_realloc(s_audio_encoded_frame,
                                      required_bytes,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!next) {
        next = heap_caps_realloc(s_audio_encoded_frame, required_bytes, MALLOC_CAP_8BIT);
    }
    if (!next) {
        ESP_LOGE(TAG,
                 "opus encoded frame buffer allocation failed required=%u free_psram=%u largest_psram=%u free_internal=%u largest_internal=%u",
                 (unsigned)required_bytes,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        report_audio_uplink_fault("encode_buffer_alloc", ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    s_audio_encoded_frame = next;
    s_audio_encoded_frame_capacity = required_bytes;
    ESP_LOGI(TAG, "opus encoded frame buffer ready bytes=%u", (unsigned)required_bytes);
    return ESP_OK;
}

static esp_err_t build_opus_audio_frame(nuwax_opus_encoder_t *encoder,
                                        uint8_t *out,
                                        size_t out_capacity,
                                        const int16_t *samples,
                                        size_t sample_count,
                                        uint32_t sequence,
                                        uint8_t flags,
                                        size_t *frame_len,
                                        size_t *encoded_len) {
    const uint16_t frame_ms = CONFIG_NUWAX_VOICE_FRAME_DURATION_MS;
    size_t opus_len = 0;
    if (!s_audio_encoded_frame || s_audio_encoded_frame_capacity == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = nuwax_opus_encoder_encode(encoder,
                                              samples,
                                              sample_count,
                                              s_audio_encoded_frame,
                                              s_audio_encoded_frame_capacity,
                                              &opus_len);
    if (err != ESP_OK) {
        return err;
    }

    err = nuwax_audio_frame_build(out,
                                  out_capacity,
                                  NUWAX_AUDIO_CODEC_OPUS,
                                  s_audio_encoded_frame,
                                  opus_len,
                                  sequence,
                                  sequence * frame_ms,
                                  flags,
                                  CONFIG_NUWAX_VOICE_SAMPLE_RATE,
                                  CONFIG_NUWAX_VOICE_CHANNELS,
                                  frame_ms,
                                  frame_len);
    if (err == ESP_OK && encoded_len) {
        *encoded_len = opus_len;
    }
    return err;
}
#endif

bool nuwax_audio_uplink_listen_start_committed(const nuwax_audio_uplink_t *ctx) {
    if (!ctx) {
        return false;
    }
    const int state = atomic_load_explicit(&ctx->listen_start_state,
                                           memory_order_acquire);
    return state == NUWAX_AUDIO_UPLINK_LISTEN_START_EXTERNAL ||
           state == NUWAX_AUDIO_UPLINK_LISTEN_START_COMMITTED;
}

static esp_err_t audio_uplink_commit_listen_start(nuwax_audio_uplink_t *ctx) {
    int state = atomic_load_explicit(&ctx->listen_start_state,
                                     memory_order_acquire);
    if (state == NUWAX_AUDIO_UPLINK_LISTEN_START_EXTERNAL ||
        state == NUWAX_AUDIO_UPLINK_LISTEN_START_COMMITTED) {
        return ESP_OK;
    }
    if (state != NUWAX_AUDIO_UPLINK_LISTEN_START_PENDING ||
        !atomic_compare_exchange_strong_explicit(
            &ctx->listen_start_state, &state,
            NUWAX_AUDIO_UPLINK_LISTEN_START_SENDING,
            memory_order_acq_rel, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    audio_uplink_sender_stage_store(
        ctx, NUWAX_AUDIO_UPLINK_SENDER_LISTEN_START, 0U, 0U);
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (!audio_uplink_flag_load(&ctx->send_failed) &&
        nuwax_ws_is_connected(ctx->ws) &&
        nuwax_ws_connection_epoch(ctx->ws) == ctx->ws_connection_epoch) {
        if (esp_timer_get_time() >= ctx->listen_start_deadline_us) {
            result = ESP_ERR_TIMEOUT;
        } else {
            result = nuwax_session_send_listen_with_audio_session_until_if_epoch(
                ctx->ws, "start", ctx->terminal_recovery_mode,
                ctx->terminal_recovery_audio_session_id[0]
                    ? ctx->terminal_recovery_audio_session_id : NULL,
                ctx->terminal_recovery_request_id[0]
                    ? ctx->terminal_recovery_request_id : NULL,
                ctx->listen_start_deadline_us, ctx->ws_connection_epoch);
        }
    }
    /* A cancellation can race the bounded send. Preserve its actual commit:
     * the owner must close a committed start even if no PCM was transmitted.
     * On failure the transport owns any partial-frame quarantine; no END or
     * stop may be invented for a start which was safely unsent. */
    atomic_store_explicit(
        &ctx->listen_start_state,
        result == ESP_OK ? NUWAX_AUDIO_UPLINK_LISTEN_START_COMMITTED
                         : NUWAX_AUDIO_UPLINK_LISTEN_START_FAILED,
        memory_order_release);
    if (result != ESP_OK) {
        ESP_LOGW(TAG,
                 "ordered listen start failed epoch=%lu err=%s; PCM and terminal withheld",
                 (unsigned long)ctx->ws_connection_epoch,
                 esp_err_to_name(result));
    }
    return result;
}

esp_err_t nuwax_audio_uplink_schedule_listen_start(
    nuwax_audio_uplink_t *ctx, uint32_t timeout_ms) {
    if (!ctx || !ctx->task || !ctx->terminal_recovery_bound ||
        timeout_ms == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ctx->frame_seen || ctx->batch_count != 0U || ctx->queued_batches != 0U ||
        ctx->end_enqueued || audio_uplink_flag_load(&ctx->capture_done) ||
        audio_uplink_flag_load(&ctx->send_failed) ||
        audio_uplink_flag_load(&ctx->finished) ||
        atomic_load_explicit(&ctx->listen_start_state, memory_order_acquire) !=
            NUWAX_AUDIO_UPLINK_LISTEN_START_EXTERNAL ||
        !nuwax_ws_is_connected(ctx->ws) ||
        nuwax_ws_connection_epoch(ctx->ws) != ctx->ws_connection_epoch) {
        return ESP_ERR_INVALID_STATE;
    }
    ctx->listen_start_deadline_us =
        esp_timer_get_time() + (int64_t)timeout_ms * 1000LL;
    atomic_store_explicit(&ctx->listen_start_state,
                          NUWAX_AUDIO_UPLINK_LISTEN_START_PENDING,
                          memory_order_release);
    audio_uplink_notify_sender(ctx);
    return ESP_OK;
}

static void audio_uplink_send_task(void *arg) {
    nuwax_audio_uplink_t *ctx = (nuwax_audio_uplink_t *)arg;
    nuwax_audio_uplink_packet_t packet = {0};
    atomic_store_explicit(&ctx->task_started, true, memory_order_release);
    audio_uplink_sender_stage_store(
        ctx, NUWAX_AUDIO_UPLINK_SENDER_WAIT_PACKET, 0U, 0U);
    while (ctx && ctx->ready_packets) {
        if (audio_uplink_flag_load(&ctx->send_failed)) {
            break;
        }
        const esp_err_t listen_start_err = audio_uplink_commit_listen_start(ctx);
        if (listen_start_err != ESP_OK) {
            audio_uplink_result_store_first_error(ctx, listen_start_err);
            audio_uplink_flag_store(&ctx->send_failed, true);
            break;
        }
        if (audio_uplink_flag_load(&ctx->send_failed)) {
            break;
        }
        if (xQueueReceive(ctx->ready_packets, &packet, 0) != pdTRUE) {
            if (audio_uplink_flag_load(&ctx->capture_done) &&
                uxQueueMessagesWaiting(ctx->ready_packets) == 0) {
                break;
            }
            (void)ulTaskNotifyTake(
                pdTRUE,
                pdMS_TO_TICKS(NUWAX_AUDIO_UPLINK_IDLE_FALLBACK_WAIT_MS));
            continue;
        }

        /* The producer can publish the prelude and first packet after the
         * loop-top EXTERNAL check. Recheck after acquiring the queued packet
         * so that publication race cannot place PCM ahead of listen/start. */
        const esp_err_t packet_start_err = audio_uplink_commit_listen_start(ctx);
        if (packet_start_err != ESP_OK ||
            audio_uplink_flag_load(&ctx->send_failed)) {
            audio_uplink_result_store_first_error(
                ctx, packet_start_err != ESP_OK
                         ? packet_start_err : ESP_ERR_INVALID_STATE);
            audio_uplink_flag_store(&ctx->send_failed, true);
            audio_uplink_release_packet_slot(ctx, packet.slot);
            break;
        }

        audio_uplink_sender_stage_store(
            ctx, NUWAX_AUDIO_UPLINK_SENDER_PREPARE_PACKET,
            packet.start_seq, packet.len);
        const bool first_batch = ctx->sent_batches == 0;
        int64_t send_started_us = esp_timer_get_time();
        const uint64_t queue_wait_us =
            packet.enqueued_us > 0 && send_started_us >= packet.enqueued_us
                ? (uint64_t)(send_started_us - packet.enqueued_us)
                : 0;
        if (queue_wait_us > ctx->max_queue_wait_us) {
            ctx->max_queue_wait_us = queue_wait_us;
        }
        const uint64_t enqueued_us =
            packet.enqueued_us > 0 ? (uint64_t)packet.enqueued_us : 0U;
        const uint64_t now_us =
            send_started_us > 0 ? (uint64_t)send_started_us : 0U;
        const uint32_t soft_send_budget_ms =
            nuwax_audio_uplink_packet_send_budget_ms(
                ctx->profile,
                packet.final,
                enqueued_us,
                now_us);
        const bool bounded_recovery_allowed =
            ctx->profile == NUWAX_AUDIO_UPLINK_PROFILE_REALTIME &&
            !packet.final && nuwax_ws_is_connected(ctx->ws) &&
            nuwax_ws_connection_epoch(ctx->ws) ==
                ctx->ws_connection_epoch &&
            nuwax_ws_heartbeat_healthy_if_epoch(
                ctx->ws, ctx->ws_connection_epoch);
        uint32_t send_budget_ms = bounded_recovery_allowed
                                      ? nuwax_audio_uplink_packet_recovery_send_budget_ms(
                                            ctx->profile,
                                            packet.final,
                                            enqueued_us,
                                            now_us)
                                      : soft_send_budget_ms;
        if (bounded_recovery_allowed && soft_send_budget_ms == 0U &&
            send_budget_ms > 0U) {
            ctx->recovery_grace_batches++;
            ctx->recovery_grace_frames += packet.frames;
            if (queue_wait_us > ctx->max_recovery_age_us) {
                ctx->max_recovery_age_us = queue_wait_us;
            }
        }
        if (ctx->profile == NUWAX_AUDIO_UPLINK_PROFILE_REALTIME &&
            send_budget_ms == 0U) {
            ctx->stale_batches++;
            ctx->stale_frames += packet.frames;
            ctx->dropped_batches++;
            ctx->dropped_frames += packet.frames;
            audio_uplink_log_realtime_drop(
                ctx, &packet, queue_wait_us, "expired_before_send");
            audio_uplink_release_packet_slot(ctx, packet.slot);
            continue;
        }
        if (nuwax_ws_connection_epoch(ctx->ws) !=
            ctx->ws_connection_epoch) {
            audio_uplink_result_store_first_error(ctx, ESP_ERR_INVALID_STATE);
            audio_uplink_flag_store(&ctx->send_failed, true);
            audio_uplink_release_packet_slot(ctx, packet.slot);
            ESP_LOGI(TAG,
                     "stale mic uplink worker stopped expected_epoch=%lu current_epoch=%lu",
                     (unsigned long)ctx->ws_connection_epoch,
                     (unsigned long)nuwax_ws_connection_epoch(ctx->ws));
            break;
        }
        const esp_err_t start_err =
            audio_uplink_ensure_start_on_first_sent_packet(ctx, &packet);
        if (start_err != ESP_OK) {
            esp_err_t recovery_err = ESP_ERR_INVALID_STATE;
            if (ctx->terminal_recovery_bound &&
                nuwax_ws_is_connected(ctx->ws) &&
                nuwax_ws_connection_epoch(ctx->ws) ==
                    ctx->ws_connection_epoch &&
                nuwax_ws_heartbeat_healthy_if_epoch(
                    ctx->ws, ctx->ws_connection_epoch)) {
                /* No binary packet has been committed yet. Close the cloud
                 * listen transaction with a fresh START|END marker instead
                 * of sacrificing a heartbeat-clean WebSocket. */
                recovery_err =
                    audio_uplink_arm_synthesized_terminal_recovery(
                        ctx, packet.start_seq);
            }
            audio_uplink_result_store_first_error(ctx, start_err);
            audio_uplink_flag_store(&ctx->send_failed, true);
            audio_uplink_release_packet_slot(ctx, packet.slot);
            if (recovery_err != ESP_OK &&
                !nuwax_audio_terminal_recovery_pending()) {
                (void)audio_uplink_request_integrity_close(
                    ctx, "start_ownership_not_preserved");
            }
            ESP_LOGE(TAG,
                     "mic packet could not preserve START ownership err=%s recovery=%s epoch=%lu",
                     esp_err_to_name(start_err),
                     recovery_err == ESP_OK ? "terminal_armed"
                                            : "policy_close_requested",
                     (unsigned long)ctx->ws_connection_epoch);
            report_audio_uplink_fault("start_ownership", start_err);
            break;
        }
        esp_err_t send_err = ESP_FAIL;
        unsigned send_attempt = 0U;
        const bool retryable_transactional_packet =
            ctx->profile == NUWAX_AUDIO_UPLINK_PROFILE_RELIABLE ||
            packet.final;
        for (;;) {
            send_attempt++;
            audio_uplink_sender_stage_store(
                ctx, NUWAX_AUDIO_UPLINK_SENDER_SEND_PACKET,
                packet.start_seq, packet.len);
            send_err =
                ctx->profile == NUWAX_AUDIO_UPLINK_PROFILE_REALTIME
                    ? nuwax_ws_send_binary_with_timeout_if_epoch(
                          ctx->ws,
                          &ctx->pool[(size_t)packet.slot *
                                     NUWAX_AUDIO_UPLINK_PACKET_BYTES],
                          packet.len,
                          send_budget_ms,
                          ctx->ws_connection_epoch)
                    : nuwax_ws_send_binary_if_epoch(
                          ctx->ws,
                          &ctx->pool[(size_t)packet.slot *
                                     NUWAX_AUDIO_UPLINK_PACKET_BYTES],
                          packet.len,
                          ctx->ws_connection_epoch);
            const bool safe_unsent_timeout =
                nuwax_ws_safe_unsent_retry_allowed(
                    send_err == ESP_ERR_TIMEOUT,
                    nuwax_ws_is_connected(ctx->ws),
                    nuwax_ws_connection_epoch(ctx->ws) ==
                        ctx->ws_connection_epoch,
                    nuwax_ws_heartbeat_healthy_if_epoch(
                        ctx->ws, ctx->ws_connection_epoch));
            const bool safe_unsent_terminal_timeout =
                retryable_transactional_packet && safe_unsent_timeout;
            if (!safe_unsent_terminal_timeout ||
                send_attempt >=
                    NUWAX_AUDIO_UPLINK_TERMINAL_RETRY_ATTEMPTS ||
                audio_uplink_flag_load(&ctx->finish_timed_out)) {
                break;
            }
            ESP_LOGW(TAG,
                     "mic %s packet safely unsent; retrying same buffer epoch=%lu attempt=%u/%u",
                     packet.final ? "terminal" : "reliable",
                     (unsigned long)ctx->ws_connection_epoch,
                     send_attempt + 1U,
                     (unsigned)NUWAX_AUDIO_UPLINK_TERMINAL_RETRY_ATTEMPTS);
            vTaskDelay(pdMS_TO_TICKS(
                NUWAX_AUDIO_UPLINK_TERMINAL_RETRY_DELAY_MS));
        }
        const int64_t send_finished_us = esp_timer_get_time();
        int64_t send_ms = (send_finished_us - send_started_us) / 1000;
        if (first_batch) {
            NUWAX_RT_DIAG_LOGI(TAG,
                     "mic first batch sent start_seq=%lu frames=%lu bytes=%u queue_wait_us=%lld send_us=%lld result=%s",
                     (unsigned long)packet.start_seq,
                     (unsigned long)packet.frames,
                     (unsigned)packet.len,
                     (long long)queue_wait_us,
                     (long long)(send_finished_us - send_started_us),
                     esp_err_to_name(send_err));
        }
        if (send_ms >= NUWAX_AUDIO_UPLINK_SEND_SLOW_WARN_MS) {
            const uint32_t slow_now_ms = audio_uplink_now_ms();
            const uint32_t ready_depth =
                ctx->ready_packets
                    ? (uint32_t)uxQueueMessagesWaiting(ctx->ready_packets)
                    : 0U;
            const uint32_t bounded_send_ms =
                send_ms > (int64_t)UINT32_MAX ? UINT32_MAX
                                                : (uint32_t)send_ms;
            const bool log_slow_batch =
                ctx->send_slow_log_ms == 0U ||
                (uint32_t)(slow_now_ms - ctx->send_slow_log_ms) >=
                    NUWAX_AUDIO_UPLINK_SEND_SLOW_LOG_INTERVAL_MS;
            if (!log_slow_batch) {
                ctx->send_slow_suppressed_batches++;
                if (bounded_send_ms > ctx->send_slow_max_ms) {
                    ctx->send_slow_max_ms = bounded_send_ms;
                }
                if (ready_depth > ctx->send_slow_max_depth) {
                    ctx->send_slow_max_depth = ready_depth;
                }
            } else {
                const uint32_t max_send_ms =
                    bounded_send_ms > ctx->send_slow_max_ms
                        ? bounded_send_ms
                        : ctx->send_slow_max_ms;
                const uint32_t max_ready_depth =
                    ready_depth > ctx->send_slow_max_depth
                        ? ready_depth
                        : ctx->send_slow_max_depth;
                ESP_LOGW(TAG,
                         "mic send queue batch slow start_seq=%lu frames=%lu send_ms=%lld bytes=%u ready_depth=%lu suppressed_batches=%lu interval_max_send_ms=%lu interval_max_depth=%lu",
                         (unsigned long)packet.start_seq,
                         (unsigned long)packet.frames,
                         (long long)send_ms,
                         (unsigned)packet.len,
                         (unsigned long)ready_depth,
                         (unsigned long)ctx->send_slow_suppressed_batches,
                         (unsigned long)max_send_ms,
                         (unsigned long)max_ready_depth);
                char result_detail[NUWAX_DEVICE_ERROR_RESULT_BYTES];
                snprintf(result_detail,
                         sizeof(result_detail),
                         "ESP_ERR_TIMEOUT;seq=%lu;frames=%lu;send_ms=%lld;bytes=%u;depth=%lu;sup=%lu;max_ms=%lu;max_depth=%lu",
                         (unsigned long)packet.start_seq,
                         (unsigned long)packet.frames,
                         (long long)send_ms,
                         (unsigned)packet.len,
                         (unsigned long)ready_depth,
                         (unsigned long)ctx->send_slow_suppressed_batches,
                         (unsigned long)max_send_ms,
                         (unsigned long)max_ready_depth);
                (void)nuwax_device_warning_report_enqueue_detail(
                    "audio_uplink",
                    "send_queue_slow",
                    ESP_ERR_TIMEOUT,
                    result_detail,
                    NULL,
                    0U,
                    NULL);
                ctx->send_slow_log_ms = slow_now_ms;
                ctx->send_slow_suppressed_batches = 0U;
                ctx->send_slow_max_ms = 0U;
                ctx->send_slow_max_depth = 0U;
            }
        }
        if (send_err != ESP_OK) {
            const bool ws_connected = nuwax_ws_is_connected(ctx->ws);
            const bool ws_epoch_matches =
                nuwax_ws_connection_epoch(ctx->ws) ==
                ctx->ws_connection_epoch;
            bool terminal_recovery_armed = false;
            const bool skip_unsent_data =
                nuwax_audio_uplink_can_skip_unsent_data_packet(
                    ctx->profile,
                    packet.final,
                    send_err,
                    ws_connected,
                    ws_epoch_matches);
            if (skip_unsent_data) {
                ctx->stale_batches++;
                ctx->stale_frames += packet.frames;
                ctx->dropped_batches++;
                ctx->dropped_frames += packet.frames;
                audio_uplink_log_realtime_drop(
                    ctx, &packet, queue_wait_us, "safe_unsent_timeout");
                audio_uplink_release_packet_slot(ctx, packet.slot);
                continue;
            }
            const bool transport_ended = !ws_connected || !ws_epoch_matches;
            const bool safe_unsent_failure =
                send_err == ESP_ERR_TIMEOUT && ws_connected &&
                ws_epoch_matches &&
                nuwax_ws_heartbeat_healthy_if_epoch(
                    ctx->ws, ctx->ws_connection_epoch);
            if (safe_unsent_failure && ctx->terminal_recovery_bound) {
                esp_err_t recovery_err = ESP_OK;
                if (packet.final) {
                    recovery_err = audio_uplink_arm_terminal_recovery(
                        ctx,
                        &ctx->pool[(size_t)packet.slot *
                                   NUWAX_AUDIO_UPLINK_PACKET_BYTES],
                        packet.len,
                        false);
                } else if (ctx->profile == NUWAX_AUDIO_UPLINK_PROFILE_RELIABLE) {
                    recovery_err =
                        audio_uplink_arm_synthesized_terminal_recovery(
                            ctx, packet.start_seq);
                }
                if (recovery_err == ESP_OK) {
                    terminal_recovery_armed = true;
                    ESP_LOGW(TAG,
                             "mic safely-unsent %s packet deferred to terminal recovery start_seq=%lu frames=%lu",
                             packet.final ? "terminal" : "reliable",
                             (unsigned long)packet.start_seq,
                             (unsigned long)packet.frames);
                } else {
                    ESP_LOGE(TAG,
                             "mic safely-unsent packet could not arm terminal recovery err=%s",
                             esp_err_to_name(recovery_err));
                    report_audio_uplink_fault("unsent_recovery", recovery_err);
                }
            }
            if (transport_ended) {
                ESP_LOGI(TAG,
                         "mic send queue stopped with closed WebSocket start_seq=%lu frames=%lu err=%s connected=%d epoch_matches=%d",
                         (unsigned long)packet.start_seq,
                         (unsigned long)packet.frames,
                         esp_err_to_name(send_err),
                         ws_connected ? 1 : 0,
                         ws_epoch_matches ? 1 : 0);
            } else {
                ESP_LOGW(TAG,
                         "mic send queue batch failed start_seq=%lu frames=%lu: %s",
                         (unsigned long)packet.start_seq,
                         (unsigned long)packet.frames,
                         esp_err_to_name(send_err));
            }
            audio_uplink_result_store_first_error(ctx, send_err);
            audio_uplink_flag_store(&ctx->send_failed, true);
            audio_uplink_release_packet_slot(ctx, packet.slot);
            if (!transport_ended && !terminal_recovery_armed && ctx->ws &&
                ctx->ws_connection_epoch != 0U) {
                (void)audio_uplink_request_integrity_close(
                    ctx, "binary_send_failed_without_terminal_recovery");
            }
            break;
        }

        ctx->sent_batches++;
        ctx->sent_frames += packet.frames;
        if (packet.final) {
            atomic_store_explicit(&ctx->end_committed,
                                  true,
                                  memory_order_release);
            if (nuwax_audio_terminal_recovery_pending() &&
                ctx->terminal_recovery_bound) {
                (void)audio_uplink_arm_terminal_recovery(ctx,
                                                          NULL,
                                                          0U,
                                                          true);
            }
        }
        if (packet.final && ctx->after_end) {
            audio_uplink_sender_stage_store(
                ctx, NUWAX_AUDIO_UPLINK_SENDER_AFTER_END,
                packet.start_seq, packet.len);
            esp_err_t after_end_err = ESP_FAIL;
            unsigned followup_attempt = 0U;
            for (;;) {
                followup_attempt++;
                after_end_err =
                    ctx->after_end((void *)ctx->after_end_user_ctx,
                                   ctx->ws_connection_epoch);
                const bool safe_unsent_followup_timeout =
                    nuwax_ws_safe_unsent_retry_allowed(
                        after_end_err == ESP_ERR_TIMEOUT,
                        nuwax_ws_is_connected(ctx->ws),
                        nuwax_ws_connection_epoch(ctx->ws) ==
                            ctx->ws_connection_epoch,
                        nuwax_ws_heartbeat_healthy_if_epoch(
                            ctx->ws, ctx->ws_connection_epoch));
                if (!safe_unsent_followup_timeout ||
                    followup_attempt >=
                        NUWAX_AUDIO_UPLINK_TERMINAL_RETRY_ATTEMPTS ||
                    audio_uplink_flag_load(&ctx->finish_timed_out)) {
                    break;
                }
                ESP_LOGW(TAG,
                         "mic terminal follow-up safely unsent; retrying epoch=%lu attempt=%u/%u",
                         (unsigned long)ctx->ws_connection_epoch,
                         followup_attempt + 1U,
                         (unsigned)NUWAX_AUDIO_UPLINK_TERMINAL_RETRY_ATTEMPTS);
                vTaskDelay(pdMS_TO_TICKS(
                    NUWAX_AUDIO_UPLINK_TERMINAL_RETRY_DELAY_MS));
            }
            if (after_end_err != ESP_OK) {
                ESP_LOGE(TAG,
                         "mic terminal follow-up failed after binary END: %s",
                         esp_err_to_name(after_end_err));
                report_audio_uplink_fault("terminal_followup", after_end_err);
                audio_uplink_result_store_first_error(ctx, after_end_err);
                audio_uplink_flag_store(&ctx->send_failed, true);
                if (nuwax_ws_is_connected(ctx->ws) &&
                    nuwax_ws_connection_epoch(ctx->ws) ==
                        ctx->ws_connection_epoch &&
                    nuwax_ws_heartbeat_healthy_if_epoch(
                        ctx->ws, ctx->ws_connection_epoch) &&
                    ctx->terminal_recovery_bound) {
                    (void)audio_uplink_arm_terminal_recovery(ctx,
                                                              NULL,
                                                              0U,
                                                              true);
                } else if (ctx->ws && ctx->ws_connection_epoch != 0U) {
                    (void)audio_uplink_request_integrity_close(
                        ctx, "listen_stop_failed_without_terminal_recovery");
                }
            } else if (ctx->terminal_recovery_bound) {
                (void)nuwax_audio_terminal_recovery_complete_if_matches(
                    ctx->ws,
                    ctx->ws_connection_epoch,
                    ctx->terminal_recovery_mode,
                    ctx->terminal_recovery_audio_session_id);
            }
        }
        audio_uplink_release_packet_slot(ctx, packet.slot);

        if (packet.final || audio_uplink_flag_load(&ctx->send_failed)) {
            break;
        }
        audio_uplink_sender_stage_store(
            ctx, NUWAX_AUDIO_UPLINK_SENDER_WAIT_PACKET, 0U, 0U);
    }
    audio_uplink_sender_stage_store(
        ctx, NUWAX_AUDIO_UPLINK_SENDER_EXITED, 0U, 0U);
    audio_uplink_flag_store(&ctx->finished, true);
    vTaskSuspend(NULL);
}

static bool audio_uplink_claim_task_tcb(nuwax_audio_uplink_t *ctx) {
    bool claimed = false;
    portENTER_CRITICAL(&s_audio_uplink_task_lock);
    if (!s_audio_uplink_task_in_use) {
        s_audio_uplink_task_in_use = true;
        s_audio_uplink_owner = ctx;
        claimed = true;
    }
    portEXIT_CRITICAL(&s_audio_uplink_task_lock);
    return claimed;
}

bool nuwax_audio_uplink_sender_busy(void) {
    bool busy = false;
    portENTER_CRITICAL(&s_audio_uplink_task_lock);
    busy = s_audio_uplink_task_in_use;
    portEXIT_CRITICAL(&s_audio_uplink_task_lock);
    return busy;
}

bool nuwax_audio_uplink_background_ready(void) {
    /* The owner is cleared before its queues can be freed. This is a bounded
     * snapshot, not permission to retain the context after the lock exits. */
    portENTER_CRITICAL(&s_audio_uplink_task_lock);
    const nuwax_audio_uplink_t *ctx = s_audio_uplink_owner;
    const bool ready = !s_audio_uplink_task_in_use ||
        (ctx && ctx->ready_packets &&
         atomic_load_explicit(&ctx->sender_stage, memory_order_acquire) ==
             NUWAX_AUDIO_UPLINK_SENDER_WAIT_PACKET &&
         uxQueueMessagesWaiting(ctx->ready_packets) == 0U);
    portEXIT_CRITICAL(&s_audio_uplink_task_lock);
    return ready;
}

static void audio_uplink_release_task_tcb(void) {
    portENTER_CRITICAL(&s_audio_uplink_task_lock);
    s_audio_uplink_task_in_use = false;
    s_audio_uplink_owner = NULL;
    portEXIT_CRITICAL(&s_audio_uplink_task_lock);
}

static void audio_uplink_queue_detached_cleanup(nuwax_audio_uplink_t *ctx) {
    ctx->cleanup_next = NULL;
    portENTER_CRITICAL(&s_audio_uplink_cleanup_lock);
    if (s_audio_uplink_cleanup_tail) {
        s_audio_uplink_cleanup_tail->cleanup_next = ctx;
    } else {
        s_audio_uplink_cleanup_head = ctx;
    }
    s_audio_uplink_cleanup_tail = ctx;
    portEXIT_CRITICAL(&s_audio_uplink_cleanup_lock);
    /* Publication transfers ownership: ctx may already have been freed. */
    xTaskNotifyGive(s_audio_uplink_reaper_task);
}

static nuwax_audio_uplink_t *audio_uplink_take_detached_cleanup(void) {
    portENTER_CRITICAL(&s_audio_uplink_cleanup_lock);
    nuwax_audio_uplink_t *ctx = s_audio_uplink_cleanup_head;
    if (ctx) {
        s_audio_uplink_cleanup_head = ctx->cleanup_next;
        if (!s_audio_uplink_cleanup_head) {
            s_audio_uplink_cleanup_tail = NULL;
        }
        ctx->cleanup_next = NULL;
    }
    portEXIT_CRITICAL(&s_audio_uplink_cleanup_lock);
    return ctx;
}

static bool audio_uplink_delete_task(nuwax_audio_uplink_t *ctx) {
    if (!ctx || !ctx->task) {
        return true;
    }

    const int64_t suspend_deadline_us =
        esp_timer_get_time() +
        (int64_t)NUWAX_AUDIO_UPLINK_TASK_SUSPEND_SETTLE_MS * 1000LL;
    vTaskSuspend(ctx->task);
    while (eTaskGetState(ctx->task) == eRunning &&
           esp_timer_get_time() < suspend_deadline_us) {
        vTaskDelay(1);
    }
    if (eTaskGetState(ctx->task) == eRunning) {
        /* The task/queue ownership is still live.  Keep the context intact
         * and let the detached reaper retry; never restart merely because a
         * short scheduler window was missed. */
        ESP_LOGW(TAG,
                 "audio uplink task did not suspend within %u ms; retaining task and queue ownership for deferred cleanup",
                 (unsigned)NUWAX_AUDIO_UPLINK_TASK_SUSPEND_SETTLE_MS);
        return false;
    }
    ESP_LOGI(TAG,
             "audio uplink task resources stack_min_free=%u stack_bytes=%u queued=%lu sent=%lu dropped=%lu max_ready=%lu",
             (unsigned)uxTaskGetStackHighWaterMark(ctx->task),
             (unsigned)NUWAX_AUDIO_UPLINK_SEND_TASK_STACK_BYTES,
             (unsigned long)ctx->queued_batches,
             (unsigned long)ctx->sent_batches,
             (unsigned long)ctx->dropped_batches,
             (unsigned long)ctx->max_ready_depth);
    vTaskDelete(ctx->task);
    ctx->task = NULL;
    heap_caps_free(ctx->task_stack);
    ctx->task_stack = NULL;
    audio_uplink_release_task_tcb();
    return true;
}

static void audio_uplink_detached_cleanup_task(void *arg) {
    (void)arg;
    for (;;) {
        nuwax_audio_uplink_t *ctx = audio_uplink_take_detached_cleanup();
        if (!ctx) {
            /* A publish between take and wait leaves a pending notification. */
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        const uint32_t cleanup_started_ms = audio_uplink_now_ms();
        uint32_t next_diagnostic_ms =
            cleanup_started_ms +
            NUWAX_AUDIO_UPLINK_REAPER_DIAGNOSTIC_INTERVAL_MS;
        bool integrity_close_requested = false;
        uint32_t last_sender_marker = (uint32_t)atomic_load_explicit(
            &ctx->sender_stage_since_ms, memory_order_acquire);
        uint32_t last_sender_progress_ms = cleanup_started_ms;
        ESP_LOGW(TAG,
                 "uplink detached cleanup stage=cancel_requested epoch=%lu",
                 (unsigned long)ctx->ws_connection_epoch);
        while (ctx->task && !audio_uplink_flag_load(&ctx->finished)) {
            audio_uplink_flag_store(&ctx->capture_done, true);
            audio_uplink_flag_store(&ctx->send_failed, true);
            audio_uplink_notify_sender(ctx);
            const uint32_t now_ms = audio_uplink_now_ms();
            const uint32_t cleanup_elapsed_ms =
                (uint32_t)(now_ms - cleanup_started_ms);
            const uint32_t sender_marker = (uint32_t)atomic_load_explicit(
                &ctx->sender_stage_since_ms, memory_order_acquire);
            if (sender_marker != last_sender_marker) {
                last_sender_marker = sender_marker;
                last_sender_progress_ms = now_ms;
            }
            /* Only a detached, explicitly cancelled sender reaches here.
             * Heartbeat traffic is not evidence of sender exit progress.
             * Stage changes grant a short grace, never an unbounded lease. */
            const bool cancel_stalled =
                (uint32_t)(now_ms - last_sender_progress_ms) >=
                    NUWAX_AUDIO_UPLINK_REAPER_NO_PROGRESS_MS ||
                cleanup_elapsed_ms >= NUWAX_AUDIO_UPLINK_REAPER_CANCEL_LIMIT_MS;
            const bool heartbeat_healthy =
                ctx->ws &&
                nuwax_ws_heartbeat_healthy_if_epoch(
                    ctx->ws, ctx->ws_connection_epoch);
            if (!integrity_close_requested &&
                (!heartbeat_healthy || cancel_stalled) &&
                cleanup_elapsed_ms >=
                    NUWAX_AUDIO_UPLINK_REAPER_CANCEL_GRACE_MS &&
                !audio_uplink_flag_load(&ctx->finished)) {
                integrity_close_requested =
                    audio_uplink_request_integrity_close(
                        ctx, "detached_sender_not_exited");
                ESP_LOGW(TAG,
                         "uplink detached cleanup stage=integrity_close_requested epoch=%lu admitted=%d",
                         (unsigned long)ctx->ws_connection_epoch,
                         integrity_close_requested ? 1 : 0);
            }
            if ((int32_t)(now_ms - next_diagnostic_ms) >= 0) {
                audio_uplink_log_stalled_sender(
                    ctx,
                    integrity_close_requested
                        ? "wait_transport_or_sender_exit"
                        : "wait_cancel_ack",
                    cleanup_elapsed_ms);
                next_diagnostic_ms =
                    now_ms +
                    NUWAX_AUDIO_UPLINK_REAPER_DIAGNOSTIC_INTERVAL_MS;
            }
            /* Stale epochs, absent transports or failed close admission must
             * not retain the singleton forever. Never free a live sender. */
            if (cleanup_elapsed_ms >=
                    NUWAX_AUDIO_UPLINK_REAPER_FATAL_RESTART_MS &&
                !audio_uplink_flag_load(&ctx->finished)) {
                audio_uplink_log_stalled_sender(
                    ctx, "fatal_fallback", cleanup_elapsed_ms);
                ESP_LOGE(TAG,
                         "uplink sender did not release singleton after cancellation and transport-integrity close attempts elapsed_ms=%lu epoch=%lu; controlled restart is the final ownership-safety fallback",
                         (unsigned long)cleanup_elapsed_ms,
                         (unsigned long)ctx->ws_connection_epoch);
                nuwax_diagnostics_log_heap_label(
                    TAG, "uplink_reaper_forced_restart");
                vTaskDelay(pdMS_TO_TICKS(100));
                /* Diagnostics/yield may have allowed the sender to finish. */
                if (audio_uplink_flag_load(&ctx->finished)) {
                    break;
                }
                esp_restart();
                vTaskDelay(portMAX_DELAY);
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        /* A finished sender can still require a scheduler turn before its
         * TCB is safely deleted.  Retry deletion while retaining ownership;
         * the existing long-horizon restart remains the final fallback. */
        while (ctx->task) {
            if (audio_uplink_delete_task(ctx)) {
                break;
            }
            const uint32_t now_ms = audio_uplink_now_ms();
            const uint32_t cleanup_elapsed_ms =
                (uint32_t)(now_ms - cleanup_started_ms);
            if (cleanup_elapsed_ms >=
                NUWAX_AUDIO_UPLINK_REAPER_FATAL_RESTART_MS) {
                ESP_LOGE(TAG,
                         "uplink task deletion remained unsafe after deferred cleanup elapsed_ms=%lu; controlled restart is the final ownership-safety fallback",
                         (unsigned long)cleanup_elapsed_ms);
                nuwax_diagnostics_log_heap_label(
                    TAG, "uplink_reaper_forced_restart");
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_restart();
                vTaskDelay(portMAX_DELAY);
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (ctx->task) {
            /* A restart above should normally be non-returning.  Keep the
             * context owned if the platform ever returns from esp_restart. */
            ESP_LOGE(TAG, "uplink task deletion incomplete; context retained");
            continue;
        }
        nuwax_audio_uplink_destroy(ctx);
        heap_caps_free(ctx);
    }
}

static esp_err_t audio_uplink_ensure_reaper_ready(void) {
    int state = atomic_load_explicit(&s_audio_uplink_reaper_init_state,
                                     memory_order_acquire);
    if (state == 2) {
        return ESP_OK;
    }

    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(
            &s_audio_uplink_reaper_init_state,
            &expected,
            1,
            memory_order_acq_rel,
            memory_order_acquire)) {
        while (atomic_load_explicit(&s_audio_uplink_reaper_init_state,
                                    memory_order_acquire) == 1) {
            vTaskDelay(1);
        }
        return atomic_load_explicit(&s_audio_uplink_reaper_init_state,
                                    memory_order_acquire) == 2
                   ? ESP_OK
                   : ESP_ERR_NO_MEM;
    }

    s_audio_uplink_reaper_stack = heap_caps_malloc(
        NUWAX_AUDIO_UPLINK_REAPER_STACK_BYTES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_audio_uplink_reaper_stack) {
        ESP_LOGE(TAG,
                 "audio uplink reaper PSRAM stack allocation failed bytes=%u",
                 (unsigned)NUWAX_AUDIO_UPLINK_REAPER_STACK_BYTES);
        atomic_store_explicit(&s_audio_uplink_reaper_init_state,
                              0,
                              memory_order_release);
        return ESP_ERR_NO_MEM;
    }
    s_audio_uplink_reaper_task = xTaskCreateStatic(
        audio_uplink_detached_cleanup_task,
        "uplink_reaper",
        NUWAX_AUDIO_UPLINK_REAPER_STACK_BYTES,
        NULL,
        NUWAX_AUDIO_UPLINK_REAPER_PRIORITY,
        s_audio_uplink_reaper_stack,
        &s_audio_uplink_reaper_task_tcb);
    if (!s_audio_uplink_reaper_task) {
        heap_caps_free(s_audio_uplink_reaper_stack);
        s_audio_uplink_reaper_stack = NULL;
        atomic_store_explicit(&s_audio_uplink_reaper_init_state,
                              0,
                              memory_order_release);
        return ESP_ERR_NO_MEM;
    }
    atomic_store_explicit(&s_audio_uplink_reaper_init_state,
                          2,
                          memory_order_release);
    ESP_LOGI(TAG,
             "audio uplink detached cleanup ready ownership=intrusive-v1 recovery=bounded-cancel-v1 stack=psram bytes=%u",
             (unsigned)NUWAX_AUDIO_UPLINK_REAPER_STACK_BYTES);
    return ESP_OK;
}

nuwax_audio_uplink_t *nuwax_audio_uplink_create(void) {
    nuwax_audio_uplink_t *ctx = heap_caps_calloc(
        1,
        sizeof(*ctx),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ctx) {
        ctx = heap_caps_calloc(1, sizeof(*ctx), MALLOC_CAP_8BIT);
    }
    return ctx;
}

esp_err_t nuwax_audio_uplink_destroy_owned(nuwax_audio_uplink_t **owner) {
    if (!owner || !*owner) {
        return ESP_OK;
    }
    nuwax_audio_uplink_t *ctx = *owner;
    if (!ctx->task) {
        *owner = NULL;
        nuwax_audio_uplink_destroy(ctx);
        heap_caps_free(ctx);
        return ESP_OK;
    }
    if (audio_uplink_flag_load(&ctx->finished)) {
        /* A finished task still owns its static TCB/stack until deletion
         * succeeds.  Do not free the context on a transient scheduler miss. */
        if (!audio_uplink_delete_task(ctx)) {
            const uint32_t epoch = ctx->ws_connection_epoch;
            *owner = NULL;
            audio_uplink_queue_detached_cleanup(ctx);
            ESP_LOGW(TAG,
                     "finished uplink detached for deferred task deletion epoch=%lu",
                     (unsigned long)epoch);
            return ESP_ERR_NOT_FINISHED;
        }
        *owner = NULL;
        nuwax_audio_uplink_destroy(ctx);
        heap_caps_free(ctx);
        return ESP_OK;
    }
    audio_uplink_flag_store(&ctx->capture_done, true);
    audio_uplink_flag_store(&ctx->send_failed, true);
    audio_uplink_notify_sender(ctx);
    const uint32_t epoch = ctx->ws_connection_epoch;
    *owner = NULL;
    audio_uplink_queue_detached_cleanup(ctx);
    ESP_LOGW(TAG,
             "uplink session detached for asynchronous cleanup with bounded cancellation grace epoch=%lu grace_ms=%u",
             (unsigned long)epoch,
             (unsigned)NUWAX_AUDIO_UPLINK_REAPER_CANCEL_GRACE_MS);
    return ESP_ERR_NOT_FINISHED;
}

static void audio_uplink_delete_queue(QueueHandle_t *queue, bool *created_with_caps) {
    if (!queue || !*queue) {
        return;
    }
    if (created_with_caps && *created_with_caps) {
        vQueueDeleteWithCaps(*queue);
    } else {
        vQueueDelete(*queue);
    }
    *queue = NULL;
    if (created_with_caps) {
        *created_with_caps = false;
    }
}

esp_err_t nuwax_audio_uplink_start(nuwax_audio_uplink_t *ctx, nuwax_ws_client_t *ws) {
    if (!ws) {
        return ESP_ERR_INVALID_ARG;
    }
    return nuwax_audio_uplink_start_if_epoch(
        ctx, ws, nuwax_ws_connection_epoch(ws));
}

esp_err_t nuwax_audio_uplink_start_if_epoch(nuwax_audio_uplink_t *ctx,
                                            nuwax_ws_client_t *ws,
                                            uint32_t expected_ws_epoch) {
    return nuwax_audio_uplink_start_with_profile_if_epoch(
        ctx,
        ws,
        NUWAX_AUDIO_UPLINK_PROFILE_RELIABLE,
        expected_ws_epoch);
}

esp_err_t nuwax_audio_uplink_start_with_profile(
    nuwax_audio_uplink_t *ctx,
    nuwax_ws_client_t *ws,
    nuwax_audio_uplink_profile_t profile) {
    if (!ws) {
        return ESP_ERR_INVALID_ARG;
    }
    return nuwax_audio_uplink_start_with_profile_if_epoch(
        ctx, ws, profile, nuwax_ws_connection_epoch(ws));
}

esp_err_t nuwax_audio_uplink_start_with_profile_if_epoch(
    nuwax_audio_uplink_t *ctx,
    nuwax_ws_client_t *ws,
    nuwax_audio_uplink_profile_t profile,
    uint32_t expected_ws_epoch) {
    if (!ctx || !ws) {
        return ESP_ERR_INVALID_ARG;
    }
    if (profile != NUWAX_AUDIO_UPLINK_PROFILE_RELIABLE &&
        profile != NUWAX_AUDIO_UPLINK_PROFILE_REALTIME) {
        return ESP_ERR_INVALID_ARG;
    }
    if (expected_ws_epoch == 0U ||
        nuwax_ws_connection_epoch(ws) != expected_ws_epoch ||
        !nuwax_ws_is_connected(ws)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ctx->task || ctx->task_stack || ctx->free_slots ||
        ctx->ready_packets || ctx->pool) {
        ESP_LOGE(TAG,
                 "refusing duplicate audio uplink start on a live context");
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t reaper_err = audio_uplink_ensure_reaper_ready();
    if (reaper_err != ESP_OK) {
        return reaper_err;
    }
    const uint32_t queue_depth =
        nuwax_audio_uplink_queue_depth_for_profile(profile);
    const uint32_t ready_queue_depth =
        nuwax_audio_uplink_ready_queue_depth(profile, queue_depth);
    const uint32_t pool_slot_count =
        nuwax_audio_uplink_pool_slot_count(profile, queue_depth);
    memset(ctx, 0, sizeof(*ctx));
    atomic_init(&ctx->task_started, false);
    atomic_init(&ctx->capture_done, false);
    atomic_init(&ctx->finished, false);
    atomic_init(&ctx->send_failed, false);
    atomic_init(&ctx->finish_timed_out, false);
    atomic_init(&ctx->end_committed, false);
    atomic_init(&ctx->listen_start_state,
                NUWAX_AUDIO_UPLINK_LISTEN_START_EXTERNAL);
    atomic_init(&ctx->send_result, ESP_OK);
    atomic_init(&ctx->sender_stage, NUWAX_AUDIO_UPLINK_SENDER_IDLE);
    atomic_init(&ctx->sender_stage_since_ms, audio_uplink_now_ms());
    atomic_init(&ctx->sender_packet_sequence, 0U);
    atomic_init(&ctx->sender_packet_bytes, 0U);
    atomic_init(&ctx->integrity_close_requested, false);
    ctx->ws = ws;
    ctx->ws_connection_epoch = expected_ws_epoch;
    ctx->queue_depth = (uint16_t)queue_depth;
    ctx->ready_queue_depth = (uint16_t)ready_queue_depth;
    ctx->pool_slot_count = (uint16_t)pool_slot_count;
    ctx->terminal_slot_reserved =
        nuwax_audio_uplink_reserves_terminal_slot(profile);
    ctx->terminal_slot = ctx->terminal_slot_reserved
                             ? (uint8_t)queue_depth
                             : 0U;
    ctx->terminal_slot_claimed = false;
    ctx->profile = profile;
    ctx->low_latency_enabled = false;
    ctx->low_latency_first_batch_queued = false;
    ctx->free_slots = xQueueCreateWithCaps(queue_depth,
                                           sizeof(uint8_t),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ctx->free_slots_with_caps = ctx->free_slots != NULL;
    if (!ctx->free_slots &&
        audio_uplink_internal_queue_fallback_allowed("free_slots")) {
        ctx->free_slots = xQueueCreate(queue_depth, sizeof(uint8_t));
    }
    ctx->ready_packets = xQueueCreateWithCaps(ready_queue_depth,
                                              sizeof(nuwax_audio_uplink_packet_t),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ctx->ready_packets_with_caps = ctx->ready_packets != NULL;
    if (!ctx->ready_packets &&
        audio_uplink_internal_queue_fallback_allowed("ready_packets")) {
        ctx->ready_packets = xQueueCreate(ready_queue_depth, sizeof(nuwax_audio_uplink_packet_t));
    }
    ctx->pool = heap_caps_malloc(pool_slot_count * NUWAX_AUDIO_UPLINK_PACKET_BYTES,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ctx->free_slots || !ctx->ready_packets || !ctx->pool) {
        ESP_LOGE(TAG,
                 "failed to allocate PSRAM audio send queue depth=%d packet_bytes=%u free_psram=%u largest_psram=%u",
                 (int)queue_depth,
                 (unsigned)NUWAX_AUDIO_UPLINK_PACKET_BYTES,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        audio_uplink_delete_queue(&ctx->free_slots, &ctx->free_slots_with_caps);
        audio_uplink_delete_queue(&ctx->ready_packets, &ctx->ready_packets_with_caps);
        heap_caps_free(ctx->pool);
        memset(ctx, 0, sizeof(*ctx));
        return ESP_ERR_NO_MEM;
    }
    for (uint32_t slot_index = 0; slot_index < queue_depth; slot_index++) {
        const uint8_t slot = (uint8_t)slot_index;
        (void)xQueueSend(ctx->free_slots, &slot, 0);
    }
    if (!audio_uplink_claim_task_tcb(ctx)) {
        ESP_LOGW(TAG,
                 "audio send task singleton is still owned by prior cleanup");
        audio_uplink_delete_queue(&ctx->free_slots, &ctx->free_slots_with_caps);
        audio_uplink_delete_queue(&ctx->ready_packets, &ctx->ready_packets_with_caps);
        heap_caps_free(ctx->pool);
        memset(ctx, 0, sizeof(*ctx));
        return ESP_ERR_NOT_FINISHED;
    }

    ctx->task_stack = heap_caps_malloc(NUWAX_AUDIO_UPLINK_SEND_TASK_STACK_BYTES,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ctx->task_stack) {
        ESP_LOGE(TAG,
                 "audio send task PSRAM stack allocation failed bytes=%u free_psram=%u largest_psram=%u",
                 (unsigned)NUWAX_AUDIO_UPLINK_SEND_TASK_STACK_BYTES,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        report_audio_uplink_fault("task_stack_alloc", ESP_ERR_NO_MEM);
        audio_uplink_release_task_tcb();
        audio_uplink_delete_queue(&ctx->free_slots, &ctx->free_slots_with_caps);
        audio_uplink_delete_queue(&ctx->ready_packets, &ctx->ready_packets_with_caps);
        heap_caps_free(ctx->pool);
        memset(ctx, 0, sizeof(*ctx));
        return ESP_ERR_NO_MEM;
    }

    ctx->task = xTaskCreateStaticPinnedToCore(
        audio_uplink_send_task,
        "nuwax_audio_tx",
        NUWAX_AUDIO_UPLINK_SEND_TASK_STACK_BYTES,
        ctx,
        NUWAX_AUDIO_UPLINK_SEND_TASK_PRIORITY,
        (StackType_t *)ctx->task_stack,
        &s_audio_uplink_task_tcb,
        NUWAX_AUDIO_UPLINK_SEND_TASK_CORE);
    if (!ctx->task) {
        ESP_LOGE(TAG, "audio send task static creation failed");
        report_audio_uplink_fault("task_create", ESP_ERR_NO_MEM);
        heap_caps_free(ctx->task_stack);
        ctx->task_stack = NULL;
        audio_uplink_release_task_tcb();
        audio_uplink_delete_queue(&ctx->free_slots, &ctx->free_slots_with_caps);
        audio_uplink_delete_queue(&ctx->ready_packets, &ctx->ready_packets_with_caps);
        heap_caps_free(ctx->pool);
        memset(ctx, 0, sizeof(*ctx));
        return ESP_ERR_NO_MEM;
    }
    const int64_t task_start_deadline_us =
        esp_timer_get_time() +
        (int64_t)NUWAX_AUDIO_UPLINK_TASK_START_TIMEOUT_MS * 1000LL;
    /* Realtime preparation can run in the active DSP consumer. The complete
     * context and queues precede task publication, so PCM can safely stage
     * before its first scheduler turn; waiting here would age capture frames. */
    while (profile != NUWAX_AUDIO_UPLINK_PROFILE_REALTIME &&
           !atomic_load_explicit(&ctx->task_started,
                                 memory_order_acquire) &&
           esp_timer_get_time() < task_start_deadline_us) {
        vTaskDelay(1);
    }
    if (profile != NUWAX_AUDIO_UPLINK_PROFILE_REALTIME &&
        !atomic_load_explicit(&ctx->task_started, memory_order_acquire)) {
        ESP_LOGE(TAG,
                 "audio send task did not start within %u ms; discarding uncommitted sender before listen start",
                 (unsigned)NUWAX_AUDIO_UPLINK_TASK_START_TIMEOUT_MS);
        report_audio_uplink_fault("task_start_timeout", ESP_ERR_TIMEOUT);
        const esp_err_t discard_err =
            nuwax_audio_uplink_discard_uncommitted(ctx);
        if (discard_err != ESP_OK) {
            ESP_LOGE(TAG,
                     "unstarted audio sender cleanup failed: %s",
                     esp_err_to_name(discard_err));
            report_audio_uplink_fault("task_cleanup", discard_err);
            return discard_err;
        }
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG,
             "audio send queue ready depth=%u ready_capacity=%u packet_bytes=%u pool_bytes=%u buffered_ms=%u terminal_reserved=%d task_priority=%u task_stack=psram static_tcb=1",
             (unsigned)queue_depth,
             (unsigned)ready_queue_depth,
             (unsigned)NUWAX_AUDIO_UPLINK_PACKET_BYTES,
             (unsigned)(pool_slot_count * NUWAX_AUDIO_UPLINK_PACKET_BYTES),
             (unsigned)(queue_depth *
                         (profile == NUWAX_AUDIO_UPLINK_PROFILE_REALTIME
                              ? 1U
                              : NUWAX_AUDIO_UPLINK_STREAM_BATCH_FRAMES) *
                         CONFIG_NUWAX_VOICE_FRAME_DURATION_MS),
             ctx->terminal_slot_reserved ? 1 : 0,
             (unsigned)NUWAX_AUDIO_UPLINK_SEND_TASK_PRIORITY);
    return ESP_OK;
}

esp_err_t nuwax_audio_uplink_bind_terminal_recovery(
    nuwax_audio_uplink_t *ctx,
    const char *mode,
    const char *audio_session_id,
    const char *request_id,
    uint32_t attempt_timeout_ms) {
    if (!ctx || !ctx->task || !mode || mode[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (ctx->frame_seen || ctx->queued_batches != 0U ||
        atomic_load_explicit(&ctx->listen_start_state, memory_order_acquire) !=
            NUWAX_AUDIO_UPLINK_LISTEN_START_EXTERNAL) {
        return ESP_ERR_INVALID_STATE;
    }
    const char *session_id = audio_session_id ? audio_session_id : "";
    if (strlen(mode) >= sizeof(ctx->terminal_recovery_mode) ||
        strlen(session_id) >= sizeof(ctx->terminal_recovery_audio_session_id) ||
        (request_id &&
         strlen(request_id) >= sizeof(ctx->terminal_recovery_request_id))) {
        return ESP_ERR_INVALID_SIZE;
    }
    strcpy(ctx->terminal_recovery_mode, mode);
    strcpy(ctx->terminal_recovery_audio_session_id, session_id);
    if (request_id) {
        strcpy(ctx->terminal_recovery_request_id, request_id);
    } else {
        ctx->terminal_recovery_request_id[0] = '\0';
    }
    ctx->terminal_recovery_attempt_timeout_ms =
        attempt_timeout_ms == 0U
            ? NUWAX_AUDIO_TERMINAL_RECOVERY_DEFAULT_ATTEMPT_TIMEOUT_MS
            : attempt_timeout_ms;
    ctx->terminal_recovery_bound = true;
    return ESP_OK;
}

static esp_err_t audio_uplink_enqueue_batch(nuwax_audio_uplink_t *ctx,
                                            const uint8_t *payload,
                                            size_t len,
                                            uint32_t start_seq,
                                             uint32_t frames,
                                             bool final,
                                             int64_t batch_started_us,
                                             uint32_t slot_wait_ms) {
    if (!ctx || !payload || len == 0 || len > NUWAX_AUDIO_UPLINK_PACKET_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ctx->free_slots || !ctx->ready_packets || !ctx->pool) {
        return ESP_ERR_INVALID_STATE;
    }
    if (final) {
        memcpy(ctx->terminal_snapshot, payload, len);
        ctx->terminal_snapshot_len = len;
    }
    if (audio_uplink_flag_load(&ctx->send_failed)) {
        const esp_err_t send_result = audio_uplink_result_load(ctx);
        return send_result == ESP_OK ? ESP_FAIL : send_result;
    }

    const bool use_reserved_terminal_slot =
        final && ctx->terminal_slot_reserved;
    uint8_t slot = 0;
    if (use_reserved_terminal_slot) {
        if (ctx->terminal_slot_claimed) {
            ESP_LOGE(TAG,
                     "realtime terminal slot was already claimed start_seq=%lu frames=%lu",
                     (unsigned long)start_seq,
                     (unsigned long)frames);
            return ESP_ERR_INVALID_STATE;
        }
        ctx->terminal_slot_claimed = true;
        slot = ctx->terminal_slot;
    } else if (xQueueReceive(ctx->free_slots,
                             &slot,
                             pdMS_TO_TICKS(slot_wait_ms)) != pdTRUE) {
        ctx->queue_full_events++;
        const esp_err_t pressure_result =
            nuwax_audio_uplink_queue_pressure_result(ctx->profile,
                                                     final,
                                                     frames);
        const bool defer_realtime_batch =
            nuwax_audio_uplink_is_recoverable_backpressure(pressure_result);
        if (defer_realtime_batch) {
            ctx->backpressure_pending = true;
        } else {
            ctx->dropped_batches++;
            ctx->dropped_frames += frames;
        }
        ESP_LOGW(TAG,
                 "mic send queue full start_seq=%lu frames=%lu final=%d ready_depth=%u action=%s events=%lu",
                 (unsigned long)start_seq,
                 (unsigned long)frames,
                 final ? 1 : 0,
                 ctx->ready_packets ? (unsigned)uxQueueMessagesWaiting(ctx->ready_packets) : 0,
                 defer_realtime_batch ? "defer_tail" : "drop",
                 (unsigned long)ctx->queue_full_events);
        const esp_err_t report_result = pressure_result == ESP_OK
                                             ? ESP_ERR_TIMEOUT
                                             : pressure_result;
        char result_detail[NUWAX_DEVICE_ERROR_RESULT_BYTES];
        snprintf(result_detail,
                 sizeof(result_detail),
                 "%s;seq=%lu;frames=%lu;depth=%u;action=%s;events=%lu",
                 esp_err_to_name(report_result),
                 (unsigned long)start_seq,
                 (unsigned long)frames,
                 ctx->ready_packets ? (unsigned)uxQueueMessagesWaiting(ctx->ready_packets) : 0U,
                 defer_realtime_batch ? "defer_tail" : "drop",
                 (unsigned long)ctx->queue_full_events);
        (void)nuwax_device_warning_report_enqueue_detail(
            "audio_uplink",
            "send_queue_full",
            report_result,
            result_detail,
            NULL,
            0U,
            NULL);
        return pressure_result;
    }

    if (audio_uplink_flag_load(&ctx->send_failed) ||
        audio_uplink_flag_load(&ctx->finished)) {
        const bool send_failed = audio_uplink_flag_load(&ctx->send_failed);
        const esp_err_t send_result = audio_uplink_result_load(ctx);
        audio_uplink_release_unqueued_slot(ctx, slot);
        return send_failed ? (send_result == ESP_OK ? ESP_FAIL : send_result)
                           : ESP_ERR_INVALID_STATE;
    }

    memcpy(&ctx->pool[(size_t)slot * NUWAX_AUDIO_UPLINK_PACKET_BYTES], payload, len);
    nuwax_audio_uplink_packet_t packet = {
        .slot = slot,
        .len = len,
        .start_seq = start_seq,
        .frames = frames,
        .final = final,
        .enqueued_us = batch_started_us > 0
                           ? batch_started_us
                           : esp_timer_get_time(),
    };
    if (xQueueSend(ctx->ready_packets, &packet, 0) != pdTRUE) {
        audio_uplink_release_unqueued_slot(ctx, slot);
        ctx->queue_full_events++;
        const esp_err_t pressure_result =
            nuwax_audio_uplink_queue_pressure_result(ctx->profile,
                                                     final,
                                                     frames);
        const bool defer_realtime_batch =
            nuwax_audio_uplink_is_recoverable_backpressure(pressure_result);
        if (defer_realtime_batch) {
            ctx->backpressure_pending = true;
        } else {
            ctx->dropped_batches++;
            ctx->dropped_frames += frames;
        }
        ESP_LOGW(TAG,
                 "mic ready queue full start_seq=%lu frames=%lu final=%d action=%s events=%lu",
                 (unsigned long)start_seq,
                 (unsigned long)frames,
                 final ? 1 : 0,
                 defer_realtime_batch ? "defer_tail" : "drop",
                 (unsigned long)ctx->queue_full_events);
        const esp_err_t report_result = pressure_result == ESP_OK
                                             ? ESP_ERR_TIMEOUT
                                             : pressure_result;
        char result_detail[NUWAX_DEVICE_ERROR_RESULT_BYTES];
        snprintf(result_detail,
                 sizeof(result_detail),
                 "%s;seq=%lu;frames=%lu;depth=%u;action=%s;events=%lu",
                 esp_err_to_name(report_result),
                 (unsigned long)start_seq,
                 (unsigned long)frames,
                 ctx->ready_packets ? (unsigned)uxQueueMessagesWaiting(ctx->ready_packets) : 0U,
                 defer_realtime_batch ? "defer_tail" : "drop",
                 (unsigned long)ctx->queue_full_events);
        (void)nuwax_device_warning_report_enqueue_detail(
            "audio_uplink",
            "ready_queue_full",
            report_result,
            result_detail,
            NULL,
            0U,
            NULL);
        return pressure_result;
    }
    audio_uplink_notify_sender(ctx);

    if (use_reserved_terminal_slot) {
        ESP_LOGI(TAG,
                 "realtime terminal packet admitted without capture-path wait start_seq=%lu frames=%lu ready_depth=%u ready_capacity=%u",
                 (unsigned long)start_seq,
                 (unsigned long)frames,
                 (unsigned)uxQueueMessagesWaiting(ctx->ready_packets),
                 (unsigned)ctx->ready_queue_depth);
    }

    ctx->queued_batches++;
    ctx->queued_frames += frames;
    if (ctx->backpressure_pending) {
        ctx->backpressure_pending = false;
        ctx->backpressure_recoveries++;
        ESP_LOGI(TAG,
                 "mic send queue backpressure recovered start_seq=%lu frames=%lu recoveries=%lu",
                 (unsigned long)start_seq,
                 (unsigned long)frames,
                 (unsigned long)ctx->backpressure_recoveries);
    }
    uint32_t ready_depth = ctx->ready_packets ? (uint32_t)uxQueueMessagesWaiting(ctx->ready_packets) : 0;
    if (ready_depth > ctx->max_ready_depth) {
        ctx->max_ready_depth = ready_depth;
    }
    return ESP_OK;
}

static esp_err_t audio_uplink_enqueue_terminal_marker(nuwax_audio_uplink_t *ctx) {
    if (!ctx || ctx->batch_count != 0 || ctx->batch_len != 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ctx->end_enqueued) {
        return ESP_OK;
    }

    const uint32_t sequence = ctx->next_sequence;
    size_t marker_len = 0;
    esp_err_t build_err = nuwax_audio_uplink_build_terminal_marker(
        s_audio_upload_batch,
        sizeof(s_audio_upload_batch),
#if CONFIG_NUWAX_VOICE_UPLINK_CODEC_OPUS
        NUWAX_AUDIO_CODEC_OPUS,
#else
        NUWAX_AUDIO_CODEC_PCM16,
#endif
        sequence,
        ctx->frame_seen,
        CONFIG_NUWAX_VOICE_SAMPLE_RATE,
        CONFIG_NUWAX_VOICE_CHANNELS,
        CONFIG_NUWAX_VOICE_FRAME_DURATION_MS,
        &marker_len);
    if (build_err != ESP_OK) {
        return build_err;
    }

    esp_err_t enqueue_err = audio_uplink_enqueue_batch(
        ctx,
        s_audio_upload_batch,
        marker_len,
        sequence,
        0,
        true,
        0,
        nuwax_audio_uplink_admission_wait_ms(ctx->profile, true));
    if (enqueue_err == ESP_OK) {
        ctx->end_enqueued = true;
        ctx->next_sequence = sequence + 1U;
        ESP_LOGI(TAG,
                 "mic stream terminal control marker queued seq=%lu flags=0x%02x",
                 (unsigned long)sequence,
                 (unsigned)s_audio_upload_batch[4]);
    }
    return enqueue_err;
}

esp_err_t nuwax_audio_uplink_enqueue_frame(nuwax_audio_uplink_t *ctx,
                                           nuwax_opus_encoder_t *opus_encoder,
                                           const int16_t *samples,
                                           size_t sample_count,
                                           uint32_t seq,
                                           uint8_t flags,
                                           bool final_frame) {
    if (!ctx || !samples) {
        return ESP_ERR_INVALID_ARG;
    }
    ctx->last_encode_us = 0U;
    ctx->last_enqueue_us = 0U;
    if (ctx->end_enqueued) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ctx->backpressure_pending) {
        /* The staging batch owns the shared upload buffer until finish(). */
        return ESP_ERR_INVALID_STATE;
    }
    if (sample_count != NUWAX_AUDIO_UPLINK_PCM_FRAME_SAMPLES) {
        ESP_LOGE(TAG,
                 "rejecting non-contract audio frame seq=%lu samples=%u expected=%u rate=%u channels=%u frame_ms=%u",
                 (unsigned long)seq,
                 (unsigned)sample_count,
                 (unsigned)NUWAX_AUDIO_UPLINK_PCM_FRAME_SAMPLES,
                 (unsigned)CONFIG_NUWAX_VOICE_SAMPLE_RATE,
                 (unsigned)CONFIG_NUWAX_VOICE_CHANNELS,
                 (unsigned)CONFIG_NUWAX_VOICE_FRAME_DURATION_MS);
        return ESP_ERR_INVALID_SIZE;
    }
    const uint32_t expected_sequence = ctx->frame_seen ? ctx->next_sequence : 0U;
    if (seq != expected_sequence) {
        ESP_LOGE(TAG,
                 "mic stream wire sequence violation expected=%lu actual=%lu first=%d",
                 (unsigned long)expected_sequence,
                 (unsigned long)seq,
                 ctx->frame_seen ? 0 : 1);
        return ESP_ERR_INVALID_ARG;
    }
    /* Keep a full unsent batch intact when queue pressure is reported. A
     * final caller can wait longer, replay it, and still append the END frame. */
    if (ctx->batch_count >= nuwax_audio_uplink_batch_target_frames(ctx)) {
        const int64_t retry_enqueue_started_us = esp_timer_get_time();
        esp_err_t retry_err = audio_uplink_enqueue_batch(
            ctx,
            s_audio_upload_batch,
            ctx->batch_len,
            ctx->batch_start_seq,
            ctx->batch_count,
            false,
            ctx->batch_started_us,
            nuwax_audio_uplink_admission_wait_ms(ctx->profile, final_frame));
        const int64_t retry_enqueue_finished_us = esp_timer_get_time();
        if (retry_enqueue_finished_us >= retry_enqueue_started_us) {
            ctx->last_enqueue_us +=
                (uint64_t)(retry_enqueue_finished_us -
                           retry_enqueue_started_us);
        }
        nuwax_audio_uplink_record_batch_result(ctx, retry_err == ESP_OK);
        audio_uplink_refresh_adaptive_batch(ctx);
        if (retry_err != ESP_OK) {
            return retry_err;
        }
        ctx->batch_len = 0;
        ctx->batch_count = 0;
        ctx->batch_started_us = 0;
    }
    if (ctx->batch_count == 0) {
        ctx->batch_start_seq = seq;
        ctx->batch_started_us = esp_timer_get_time();
    }

    const size_t frame_offset = ctx->batch_len;
    const uint8_t frame_flags = flags;
    size_t frame_len = 0;
    const int64_t encode_started_us = esp_timer_get_time();
#if CONFIG_NUWAX_VOICE_UPLINK_CODEC_OPUS
    size_t encoded_len = 0;
    if (!opus_encoder) {
        return ESP_ERR_INVALID_ARG;
    }
    if (seq == 0) {
        nuwax_diagnostics_log_current_task_stack(TAG, "before_first_opus_encode");
    }
    esp_err_t build_err = build_opus_audio_frame(opus_encoder,
                                                 &s_audio_upload_batch[ctx->batch_len],
                                                 sizeof(s_audio_upload_batch) - ctx->batch_len,
                                                 samples,
                                                 sample_count,
                                                 seq,
                                                 frame_flags,
                                                 &frame_len,
                                                 &encoded_len);
    if (seq == 0 && build_err == ESP_OK) {
        ESP_LOGI(TAG,
                 "first opus frame encoded encoded_len=%u frame_len=%u",
                 (unsigned)encoded_len,
                 (unsigned)frame_len);
        nuwax_diagnostics_log_current_task_stack(TAG, "after_first_opus_encode");
    }
    ctx->encoded_bytes_total += encoded_len;
#else
    (void)opus_encoder;
    esp_err_t build_err = nuwax_audio_uplink_build_pcm16_frame(&s_audio_upload_batch[ctx->batch_len],
                                                              sizeof(s_audio_upload_batch) - ctx->batch_len,
                                                              samples,
                                                              sample_count,
                                                              seq,
                                                              frame_flags,
                                                              &frame_len);
#endif
    const int64_t encode_finished_us = esp_timer_get_time();
    if (encode_finished_us >= encode_started_us) {
        ctx->last_encode_us =
            (uint64_t)(encode_finished_us - encode_started_us);
    }
    if (build_err != ESP_OK) {
        ESP_LOGW(TAG,
                 "mic stream frame build failed seq=%lu: %s",
                 (unsigned long)seq,
                 esp_err_to_name(build_err));
        return build_err;
    }

    ctx->batch_len += frame_len;
    ctx->batch_count++;
    ctx->batch_last_frame_offset = frame_offset;
    ctx->frame_seen = true;
    ctx->next_sequence = seq + 1U;

    if (final_frame) {
        esp_err_t mark_err = nuwax_audio_uplink_mark_last_frame_end(
            s_audio_upload_batch,
            ctx->batch_len,
            ctx->batch_last_frame_offset);
        if (mark_err != ESP_OK) {
            ESP_LOGW(TAG,
                     "mic stream final frame mark failed seq=%lu: %s",
                     (unsigned long)seq,
                     esp_err_to_name(mark_err));
            return mark_err;
        }
    }

    if (ctx->batch_count >= nuwax_audio_uplink_batch_target_frames(ctx) || final_frame) {
        const int64_t enqueue_started_us = esp_timer_get_time();
        esp_err_t send_err = audio_uplink_enqueue_batch(ctx,
                                                        s_audio_upload_batch,
                                                        ctx->batch_len,
                                                        ctx->batch_start_seq,
                                                         ctx->batch_count,
                                                         final_frame,
                                                         ctx->batch_started_us,
                                                         nuwax_audio_uplink_admission_wait_ms(
                                                            ctx->profile,
                                                             final_frame));
        const int64_t enqueue_finished_us = esp_timer_get_time();
        if (enqueue_finished_us >= enqueue_started_us) {
            ctx->last_enqueue_us +=
                (uint64_t)(enqueue_finished_us - enqueue_started_us);
        }
        nuwax_audio_uplink_record_batch_result(ctx, send_err == ESP_OK);
        audio_uplink_refresh_adaptive_batch(ctx);
        if (send_err != ESP_OK) {
            ESP_LOGW(TAG,
                     "mic stream batch enqueue failed start_seq=%lu frames=%lu: %s",
                     (unsigned long)ctx->batch_start_seq,
                     (unsigned long)ctx->batch_count,
                     esp_err_to_name(send_err));
            report_audio_uplink_fault("batch_enqueue", send_err);
            if (final_frame && ctx->terminal_recovery_bound &&
                nuwax_ws_is_connected(ctx->ws) &&
                nuwax_ws_connection_epoch(ctx->ws) ==
                    ctx->ws_connection_epoch &&
                nuwax_ws_heartbeat_healthy_if_epoch(
                    ctx->ws, ctx->ws_connection_epoch)) {
                (void)audio_uplink_arm_terminal_recovery(
                    ctx,
                    s_audio_upload_batch,
                    ctx->batch_len,
                    false);
            }
            return send_err;
        }
        if (final_frame) {
            ctx->end_enqueued = true;
        }
        ctx->batch_len = 0;
        ctx->batch_count = 0;
        ctx->batch_started_us = 0;
    }
    return ESP_OK;
}

esp_err_t nuwax_audio_uplink_request_finish(nuwax_audio_uplink_t *ctx) {
    if (!ctx || !ctx->task) {
        return ESP_OK;
    }
    if (audio_uplink_flag_load(&ctx->finish_timed_out)) {
        return ESP_ERR_TIMEOUT;
    }
    bool terminal_recovery_armed = false;
    if (!audio_uplink_flag_load(&ctx->send_failed) &&
        ctx->batch_count > 0 && ctx->batch_len > 0) {
        esp_err_t mark_err = nuwax_audio_uplink_mark_last_frame_end(
            s_audio_upload_batch,
            ctx->batch_len,
            ctx->batch_last_frame_offset);
        if (mark_err != ESP_OK) {
            ESP_LOGE(TAG,
                     "mic send queue tail END mark failed offset=%u len=%u frames=%lu: %s",
                     (unsigned)ctx->batch_last_frame_offset,
                     (unsigned)ctx->batch_len,
                     (unsigned long)ctx->batch_count,
                     esp_err_to_name(mark_err));
            report_audio_uplink_fault("terminal_end_mark", mark_err);
            audio_uplink_result_store_first_error(ctx, mark_err);
        }
        const esp_err_t current_result = audio_uplink_result_load(ctx);
        esp_err_t flush_err = current_result == ESP_OK
                                  ? audio_uplink_enqueue_batch(
                                        ctx,
                                        s_audio_upload_batch,
                                        ctx->batch_len,
                                        ctx->batch_start_seq,
                                        ctx->batch_count,
                                        true,
                                        ctx->batch_started_us,
                                        nuwax_audio_uplink_admission_wait_ms(
                                            ctx->profile,
                                            true))
                                  : current_result;
        if (flush_err != ESP_OK) {
            ESP_LOGW(TAG,
                     "mic send queue tail flush failed start_seq=%lu frames=%lu: %s",
                     (unsigned long)ctx->batch_start_seq,
                     (unsigned long)ctx->batch_count,
                     esp_err_to_name(flush_err));
            audio_uplink_result_store_first_error(ctx, flush_err);
            esp_err_t recovery_err = ESP_ERR_INVALID_STATE;
            if (ctx->terminal_recovery_bound &&
                nuwax_ws_is_connected(ctx->ws) &&
                nuwax_ws_connection_epoch(ctx->ws) ==
                    ctx->ws_connection_epoch &&
                nuwax_ws_heartbeat_healthy_if_epoch(
                    ctx->ws, ctx->ws_connection_epoch)) {
                recovery_err = mark_err == ESP_OK
                                   ? audio_uplink_arm_terminal_recovery(
                                         ctx,
                                         s_audio_upload_batch,
                                         ctx->batch_len,
                                         false)
                                   : audio_uplink_arm_synthesized_terminal_recovery(
                                         ctx, ctx->batch_start_seq);
                terminal_recovery_armed = recovery_err == ESP_OK;
            }
            if (recovery_err != ESP_OK) {
                ESP_LOGE(TAG,
                         "mic tail could not enter terminal recovery err=%s",
                         esp_err_to_name(recovery_err));
                report_audio_uplink_fault("terminal_recovery", recovery_err);
            }
        } else {
            nuwax_audio_uplink_record_batch_result(ctx, true);
            ctx->end_enqueued = true;
        }
        ctx->batch_len = 0;
        ctx->batch_count = 0;
    }
    if (!audio_uplink_flag_load(&ctx->send_failed) &&
        audio_uplink_result_load(ctx) == ESP_OK && !ctx->end_enqueued) {
        esp_err_t marker_err = audio_uplink_enqueue_terminal_marker(ctx);
        if (marker_err != ESP_OK) {
            ESP_LOGW(TAG,
                     "mic stream terminal control marker enqueue failed seq=%lu: %s",
                     (unsigned long)ctx->next_sequence,
                     esp_err_to_name(marker_err));
            audio_uplink_result_store_first_error(ctx, marker_err);
            if (ctx->terminal_recovery_bound &&
                nuwax_ws_is_connected(ctx->ws) &&
                nuwax_ws_connection_epoch(ctx->ws) ==
                    ctx->ws_connection_epoch &&
                nuwax_ws_heartbeat_healthy_if_epoch(
                    ctx->ws, ctx->ws_connection_epoch)) {
                /* The marker is intentionally built in the shared staging
                 * buffer before publication; no sender task reads it. */
                const esp_err_t recovery_err =
                    audio_uplink_arm_terminal_recovery(
                        ctx,
                        s_audio_upload_batch,
                        NUWAX_AUDIO_FRAME_HEADER_BYTES,
                        false);
                terminal_recovery_armed = recovery_err == ESP_OK;
                if (recovery_err != ESP_OK) {
                    ESP_LOGE(TAG,
                             "mic terminal marker could not enter recovery err=%s",
                             esp_err_to_name(recovery_err));
                }
            }
        }
    }
    const esp_err_t final_result = audio_uplink_result_load(ctx);
    if (!audio_uplink_flag_load(&ctx->send_failed) && !ctx->end_enqueued &&
        final_result != ESP_OK) {
        ESP_LOGE(TAG,
                 "mic stream END could not be committed; aborting current audio session result=%s profile=%d",
                 esp_err_to_name(final_result));
        audio_uplink_flag_store(&ctx->send_failed, true);
        if (!terminal_recovery_armed &&
            !nuwax_audio_terminal_recovery_pending() && ctx->ws &&
            ctx->ws_connection_epoch != 0U) {
            (void)audio_uplink_request_integrity_close(
                ctx, "binary_end_not_committed");
        }
    }
    audio_uplink_flag_store(&ctx->capture_done, true);
    audio_uplink_notify_sender(ctx);
    if (final_result != ESP_OK) {
        return final_result;
    }
    return audio_uplink_flag_load(&ctx->send_failed) ? ESP_FAIL : ESP_OK;
}

esp_err_t nuwax_audio_uplink_request_finish_then(
    nuwax_audio_uplink_t *ctx,
    nuwax_audio_uplink_after_end_fn after_end,
    const void *user_ctx,
    size_t user_ctx_size) {
    if (!ctx || !ctx->task || !after_end || !user_ctx ||
        user_ctx_size == 0U ||
        user_ctx_size > sizeof(ctx->after_end_user_ctx)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ctx->end_enqueued ||
        audio_uplink_flag_load(&ctx->capture_done) ||
        audio_uplink_flag_load(&ctx->finished)) {
        return ESP_ERR_INVALID_STATE;
    }

    ctx->after_end = after_end;
    memcpy(ctx->after_end_user_ctx, user_ctx, user_ctx_size);
    ctx->after_end_user_ctx_size = user_ctx_size;
    const esp_err_t result = nuwax_audio_uplink_request_finish(ctx);
    if (result != ESP_OK && !ctx->end_enqueued) {
        ctx->after_end = NULL;
        ctx->after_end_user_ctx_size = 0U;
        memset(ctx->after_end_user_ctx, 0,
               sizeof(ctx->after_end_user_ctx));
    }
    return result;
}

bool nuwax_audio_uplink_finish_is_complete(const nuwax_audio_uplink_t *ctx,
                                           esp_err_t *out_result) {
    if (!ctx || !ctx->task) {
        if (out_result) {
            *out_result = ESP_OK;
        }
        return true;
    }
    if (!audio_uplink_flag_load(&ctx->finished)) {
        return false;
    }
    if (out_result) {
        const esp_err_t send_result = audio_uplink_result_load(ctx);
        *out_result = send_result != ESP_OK
                          ? send_result
                          : (audio_uplink_flag_load(&ctx->send_failed)
                                 ? ESP_FAIL
                                 : ESP_OK);
    }
    return true;
}

bool nuwax_audio_uplink_end_committed(const nuwax_audio_uplink_t *ctx) {
    return ctx && atomic_load_explicit(&ctx->end_committed,
                                       memory_order_acquire);
}

esp_err_t nuwax_audio_uplink_discard_uncommitted(
    nuwax_audio_uplink_t *ctx) {
    if (!ctx) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ctx->task) {
        return ESP_OK;
    }
    if (ctx->frame_seen || ctx->batch_count != 0U || ctx->batch_len != 0U ||
        ctx->queued_batches != 0U || ctx->end_enqueued ||
        ctx->terminal_slot_claimed) {
        ESP_LOGE(TAG,
                 "refusing uncommitted uplink discard after stream data was staged or queued");
        return ESP_ERR_INVALID_STATE;
    }

    int start_state = NUWAX_AUDIO_UPLINK_LISTEN_START_PENDING;
    (void)atomic_compare_exchange_strong_explicit(
        &ctx->listen_start_state, &start_state,
        NUWAX_AUDIO_UPLINK_LISTEN_START_CANCELED,
        memory_order_acq_rel, memory_order_acquire);
    start_state = atomic_load_explicit(&ctx->listen_start_state,
                                       memory_order_acquire);
    if (start_state == NUWAX_AUDIO_UPLINK_LISTEN_START_SENDING ||
        start_state == NUWAX_AUDIO_UPLINK_LISTEN_START_COMMITTED) {
        /* The sender can still commit a text frame. Keep its owner alive for
         * normal finish/preemption to discharge the END/stop obligation. */
        return ESP_ERR_INVALID_STATE;
    }

    audio_uplink_flag_store(&ctx->capture_done, true);
    audio_uplink_notify_sender(ctx);
    const int64_t deadline_ms =
        esp_timer_get_time() / 1000 + NUWAX_AUDIO_UPLINK_ABORT_WAIT_MS;
    while (!audio_uplink_flag_load(&ctx->finished) &&
           esp_timer_get_time() / 1000 < deadline_ms) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!audio_uplink_flag_load(&ctx->finished)) {
        ESP_LOGE(TAG,
                 "uncommitted audio sender did not stop before cleanup timeout");
        /* Do not suspend a live sender here: it must get a scheduler turn to
         * observe capture_done and publish finished.  The owner retains the
         * context and the normal cleanup path will retry deletion after that
         * acknowledgement. */
        ESP_LOGW(TAG,
                 "uncommitted audio sender remains owned after cleanup timeout; waiting for cooperative sender exit");
        return ESP_ERR_NOT_FINISHED;
    }

    nuwax_audio_uplink_destroy(ctx);
    ESP_LOGI(TAG,
             "uncommitted audio sender discarded without binary END");
    return ESP_OK;
}

esp_err_t nuwax_audio_uplink_finish(nuwax_audio_uplink_t *ctx) {
    esp_err_t request_result = nuwax_audio_uplink_request_finish(ctx);
    if (!ctx || !ctx->task) {
        return request_result;
    }
    const int64_t wait_started_ms = esp_timer_get_time() / 1000;
    int64_t last_warn_ms = esp_timer_get_time() / 1000;
    const uint32_t finish_timeout_ms =
        ctx->profile == NUWAX_AUDIO_UPLINK_PROFILE_REALTIME
            ? NUWAX_AUDIO_UPLINK_REALTIME_FINISH_TIMEOUT_MS
            : NUWAX_AUDIO_UPLINK_RELIABLE_FINISH_TIMEOUT_MS;
    while (!audio_uplink_flag_load(&ctx->finished)) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms - wait_started_ms >= finish_timeout_ms) {
            audio_uplink_flag_store(&ctx->finish_timed_out, true);
            audio_uplink_result_store_first_error(ctx, ESP_ERR_TIMEOUT);
            audio_uplink_flag_store(&ctx->send_failed, true);
            audio_uplink_notify_sender(ctx);
            ESP_LOGE(TAG,
                     "mic send queue finish timed out profile=%d timeout_ms=%lu queued_batches=%lu sent_batches=%lu ready_depth=%u",
                     (int)ctx->profile,
                     (unsigned long)finish_timeout_ms,
                     (unsigned long)ctx->queued_batches,
                     (unsigned long)ctx->sent_batches,
                     ctx->ready_packets ? (unsigned)uxQueueMessagesWaiting(ctx->ready_packets) : 0);
            esp_err_t recovery_err = ESP_ERR_INVALID_STATE;
            const bool end_committed =
                nuwax_audio_uplink_end_committed(ctx);
            if (ctx->terminal_recovery_bound && ctx->ws &&
                nuwax_ws_is_connected(ctx->ws) &&
                nuwax_ws_connection_epoch(ctx->ws) ==
                    ctx->ws_connection_epoch &&
                nuwax_ws_heartbeat_healthy_if_epoch(
                    ctx->ws, ctx->ws_connection_epoch)) {
                if (end_committed) {
                    recovery_err = audio_uplink_arm_terminal_recovery(
                        ctx, NULL, 0U, true);
                } else if (ctx->terminal_snapshot_len > 0U) {
                    recovery_err = audio_uplink_arm_terminal_recovery(
                        ctx,
                        ctx->terminal_snapshot,
                        ctx->terminal_snapshot_len,
                        false);
                } else {
                    recovery_err =
                        audio_uplink_arm_synthesized_terminal_recovery(
                            ctx, ctx->next_sequence);
                }
            }
            if (recovery_err != ESP_OK &&
                !nuwax_audio_terminal_recovery_pending() && ctx->ws &&
                ctx->ws_connection_epoch != 0U) {
                (void)audio_uplink_request_integrity_close(
                    ctx, "finish_timeout_without_terminal_recovery");
            }
            const int64_t abort_deadline_ms =
                esp_timer_get_time() / 1000 + NUWAX_AUDIO_UPLINK_ABORT_WAIT_MS;
            while (!audio_uplink_flag_load(&ctx->finished) &&
                   esp_timer_get_time() / 1000 < abort_deadline_ms) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            return ESP_ERR_TIMEOUT;
        }
        if (now_ms - last_warn_ms >= NUWAX_AUDIO_UPLINK_QUEUE_FINAL_WAIT_MS) {
            ESP_LOGW(TAG,
                     "waiting for mic send queue queued_batches=%lu sent_batches=%lu ready_depth=%u",
                     (unsigned long)ctx->queued_batches,
                     (unsigned long)ctx->sent_batches,
                     ctx->ready_packets ? (unsigned)uxQueueMessagesWaiting(ctx->ready_packets) : 0);
            report_audio_uplink_warning("sender_stalled", ESP_ERR_TIMEOUT);
            last_warn_ms = now_ms;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    esp_err_t finish_result = request_result;
    (void)nuwax_audio_uplink_finish_is_complete(ctx, &finish_result);
    return finish_result;
}

esp_err_t nuwax_audio_uplink_preempt_for_local_power(
    nuwax_audio_uplink_t *ctx) {
    if (!ctx) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ctx->task || audio_uplink_flag_load(&ctx->finished)) {
        return ESP_OK;
    }

    /* Queued realtime audio is obsolete once the user explicitly ends the
     * local conversation.  Let an in-flight bounded write finish first: only
     * then is sent_frames stable enough to choose END versus START|END without
     * racing the first packet's commit. */
    audio_uplink_flag_store(&ctx->capture_done, true);
    audio_uplink_flag_store(&ctx->send_failed, true);
    audio_uplink_notify_sender(ctx);

    const int64_t wait_deadline_ms =
        esp_timer_get_time() / 1000 +
        NUWAX_AUDIO_UPLINK_LOCAL_POWER_PREEMPT_WAIT_MS;
    while (!audio_uplink_flag_load(&ctx->finished) &&
           esp_timer_get_time() / 1000 < wait_deadline_ms) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    const bool sender_finished = audio_uplink_flag_load(&ctx->finished);
    const bool bound_transport_alive =
        ctx->terminal_recovery_bound && ctx->ws &&
        ctx->ws_connection_epoch != 0U &&
        nuwax_ws_is_connected(ctx->ws) &&
        nuwax_ws_connection_epoch(ctx->ws) == ctx->ws_connection_epoch;
    const bool start_committed = nuwax_audio_uplink_listen_start_committed(ctx);
    esp_err_t recovery_err = ESP_ERR_INVALID_STATE;
    if (sender_finished && bound_transport_alive && start_committed) {
        if (nuwax_audio_uplink_end_committed(ctx)) {
            recovery_err = audio_uplink_arm_terminal_recovery(
                ctx, NULL, 0U, true);
        } else {
            /* Do not reuse a previously queued terminal snapshot here.  It was
             * built from producer-side frame_seen, whereas a local preempt
             * needs the now-stable sender-side sent_frames to choose whether
             * the replacement marker must also carry START. */
            recovery_err = audio_uplink_arm_synthesized_terminal_recovery(
                ctx, ctx->next_sequence);
        }
    }

    bool close_requested = false;
    if (bound_transport_alive && (!sender_finished || start_committed) &&
        (!sender_finished || recovery_err != ESP_OK)) {
        /* A sender that outlives the realtime packet budget has uncertain
         * first-frame commit state.  Quarantine its epoch instead of emitting
         * a potentially duplicated START or an orphan END. */
        close_requested = audio_uplink_request_integrity_close(
            ctx,
            sender_finished
                ? "local_power_preempt_without_terminal_recovery"
                : "local_power_preempt_sender_not_quiescent");
        ESP_LOGW(TAG,
                 "local power preempt could not transfer terminal recovery epoch=%lu sender_finished=%d err=%s close_requested=%d",
                 (unsigned long)ctx->ws_connection_epoch,
                 sender_finished ? 1 : 0,
                 esp_err_to_name(recovery_err),
                 close_requested ? 1 : 0);
    }
    ESP_LOGI(TAG,
             "local power preempt accepted epoch=%lu terminal=%s sender_finished=%d wait_ms=%u",
             (unsigned long)ctx->ws_connection_epoch,
             recovery_err == ESP_OK
                 ? "recovery"
                 : (bound_transport_alive
                        ? (close_requested ? "connection_close"
                                           : "close_deferred")
                        : "transport_gone"),
             sender_finished ? 1 : 0,
             (unsigned)NUWAX_AUDIO_UPLINK_LOCAL_POWER_PREEMPT_WAIT_MS);
    return ESP_OK;
}

void nuwax_audio_uplink_destroy(nuwax_audio_uplink_t *ctx) {
    if (!ctx) {
        return;
    }
    if (ctx->task && !audio_uplink_flag_load(&ctx->finished) &&
        !audio_uplink_flag_load(&ctx->finish_timed_out)) {
        esp_err_t finish_err = nuwax_audio_uplink_finish(ctx);
        if (finish_err != ESP_OK) {
            ESP_LOGW(TAG, "audio send queue finish during destroy: %s", esp_err_to_name(finish_err));
        }
    }
    if (ctx->task && !audio_uplink_flag_load(&ctx->finished)) {
        if (ctx->ws && ctx->ws_connection_epoch != 0U) {
            (void)audio_uplink_request_integrity_close(
                ctx, "destroy_waiting_for_live_sender");
        }
        audio_uplink_notify_sender(ctx);
        const int64_t stop_deadline_ms =
            esp_timer_get_time() / 1000 + NUWAX_AUDIO_UPLINK_ABORT_WAIT_MS;
        while (!audio_uplink_flag_load(&ctx->finished) &&
               esp_timer_get_time() / 1000 < stop_deadline_ms) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    if (ctx->task && !audio_uplink_flag_load(&ctx->finished)) {
        ESP_LOGE(TAG,
                 "audio send task did not acknowledge stop; retaining session for asynchronous cleanup");
        return;
    }
    if (ctx->task) {
        if (!audio_uplink_delete_task(ctx)) {
            ESP_LOGW(TAG,
                     "audio uplink task deletion deferred; retaining context and queue ownership");
            return;
        }
    }
    audio_uplink_delete_queue(&ctx->free_slots, &ctx->free_slots_with_caps);
    audio_uplink_delete_queue(&ctx->ready_packets, &ctx->ready_packets_with_caps);
    heap_caps_free(ctx->pool);
    memset(ctx, 0, sizeof(*ctx));
}

void nuwax_audio_uplink_get_stats(const nuwax_audio_uplink_t *ctx, nuwax_audio_uplink_stats_t *stats) {
    if (!stats) {
        return;
    }
    memset(stats, 0, sizeof(*stats));
    if (!ctx) {
        return;
    }
    stats->queued_batches = ctx->queued_batches;
    stats->queued_frames = ctx->queued_frames;
    stats->sent_batches = ctx->sent_batches;
    stats->sent_frames = ctx->sent_frames;
    stats->dropped_batches = ctx->dropped_batches;
    stats->dropped_frames = ctx->dropped_frames;
    stats->queue_full_events = ctx->queue_full_events;
    stats->backpressure_recoveries = ctx->backpressure_recoveries;
    stats->stale_batches = ctx->stale_batches;
    stats->stale_frames = ctx->stale_frames;
    stats->recovery_grace_batches = ctx->recovery_grace_batches;
    stats->recovery_grace_frames = ctx->recovery_grace_frames;
    stats->max_recovery_age_us = ctx->max_recovery_age_us;
    stats->max_ready_depth = ctx->max_ready_depth;
    stats->ready_depth = ctx->ready_packets ? (uint32_t)uxQueueMessagesWaiting(ctx->ready_packets) : 0;
    stats->max_queue_wait_us = ctx->max_queue_wait_us;
    stats->last_encode_us = ctx->last_encode_us;
    stats->last_enqueue_us = ctx->last_enqueue_us;
    stats->encoded_bytes_total = ctx->encoded_bytes_total;
}
