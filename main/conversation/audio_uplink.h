#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>

#include "sdkconfig.h"
#include "esp_err.h"
#include "audio_frame.h"
#include "conversation_latency_contract.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "opus_encoder_wrapper.h"
#include "ws_transport.h"

#define NUWAX_AUDIO_UPLINK_STREAM_BATCH_FRAMES 4
#define NUWAX_AUDIO_UPLINK_LOW_LATENCY_FIRST_BATCH_FRAMES 1
#define NUWAX_AUDIO_UPLINK_LOW_LATENCY_STEADY_BATCH_FRAMES 1
_Static_assert(NUWAX_AUDIO_UPLINK_LOW_LATENCY_FIRST_BATCH_FRAMES <=
                   NUWAX_AUDIO_UPLINK_STREAM_BATCH_FRAMES,
               "low-latency first batch exceeds packet capacity");
_Static_assert(NUWAX_AUDIO_UPLINK_LOW_LATENCY_STEADY_BATCH_FRAMES <=
                   NUWAX_AUDIO_UPLINK_STREAM_BATCH_FRAMES,
               "low-latency steady batch exceeds packet capacity");
#define NUWAX_AUDIO_UPLINK_REALTIME_QUEUE_DEPTH \
    NUWAX_CONVERSATION_UPLINK_PACKET_SLOT_DEPTH
#define NUWAX_AUDIO_UPLINK_REALTIME_MAX_AGE_MS \
    NUWAX_CONVERSATION_UPLINK_STAGING_MAX_AGE_MS
#define NUWAX_AUDIO_UPLINK_REALTIME_RECOVERY_MAX_AGE_MS \
    NUWAX_CONVERSATION_PROFILE_UPLINK_RECOVERY_MAX_AGE_MS
#if CONFIG_NUWAX_VOICE_UPLINK_CODEC_OPUS
#define NUWAX_AUDIO_UPLINK_RELIABLE_QUEUE_DEPTH 32
#else
#define NUWAX_AUDIO_UPLINK_RELIABLE_QUEUE_DEPTH 128
#endif
/* Preserve the historical name for long-running reliable streams. */
#define NUWAX_AUDIO_UPLINK_SEND_QUEUE_DEPTH NUWAX_AUDIO_UPLINK_RELIABLE_QUEUE_DEPTH
#define NUWAX_AUDIO_UPLINK_CAPTURE_REALTIME_WARN_PERCENT 85
#define NUWAX_AUDIO_UPLINK_REALTIME_ADMISSION_WAIT_MS 0U
#define NUWAX_AUDIO_UPLINK_RELIABLE_ADMISSION_WAIT_MS 80U
#define NUWAX_AUDIO_UPLINK_REALTIME_FINAL_ADMISSION_WAIT_MS 0U
#define NUWAX_AUDIO_UPLINK_FINAL_ADMISSION_WAIT_MS 5000U
#define NUWAX_AUDIO_UPLINK_REALTIME_TERMINAL_SEND_TIMEOUT_MS 2000U
#define NUWAX_AUDIO_UPLINK_AFTER_END_CONTEXT_MAX_BYTES 96U
#define NUWAX_AUDIO_UPLINK_PCM_FRAME_SAMPLES \
    (((size_t)CONFIG_NUWAX_VOICE_SAMPLE_RATE * \
      CONFIG_NUWAX_VOICE_FRAME_DURATION_MS * \
      CONFIG_NUWAX_VOICE_CHANNELS) / 1000U)
#if CONFIG_NUWAX_VOICE_UPLINK_CODEC_OPUS
#define NUWAX_AUDIO_UPLINK_FRAME_BYTES \
    (NUWAX_AUDIO_FRAME_HEADER_BYTES + \
     CONFIG_NUWAX_VOICE_MAX_SAMPLES_PER_FRAME * sizeof(int16_t))
#else
#define NUWAX_AUDIO_UPLINK_FRAME_BYTES \
    (NUWAX_AUDIO_FRAME_HEADER_BYTES + \
     NUWAX_AUDIO_UPLINK_PCM_FRAME_SAMPLES * sizeof(int16_t))
#endif
#define NUWAX_AUDIO_UPLINK_PACKET_BYTES \
    (NUWAX_AUDIO_UPLINK_STREAM_BATCH_FRAMES * NUWAX_AUDIO_UPLINK_FRAME_BYTES)

_Static_assert(NUWAX_AUDIO_UPLINK_REALTIME_QUEUE_DEPTH > 0 &&
                   NUWAX_AUDIO_UPLINK_REALTIME_QUEUE_DEPTH <= UINT8_MAX,
               "realtime uplink queue depth must fit the packet slot id");
_Static_assert(NUWAX_AUDIO_UPLINK_RELIABLE_QUEUE_DEPTH > 0 &&
                   NUWAX_AUDIO_UPLINK_RELIABLE_QUEUE_DEPTH <= UINT8_MAX,
               "reliable uplink queue depth must fit the packet slot id");
_Static_assert(NUWAX_AUDIO_UPLINK_REALTIME_MAX_AGE_MS >= 20U,
               "realtime uplink age budget must cover at least one audio frame");

#if CONFIG_NUWAX_VOICE_UPLINK_CODEC_OPUS
#define NUWAX_AUDIO_UPLINK_CODEC_NAME "opus"
#else
#define NUWAX_AUDIO_UPLINK_CODEC_NAME "pcm16"
#endif

typedef struct {
    uint32_t queued_batches;
    uint32_t queued_frames;
    uint32_t sent_batches;
    uint32_t sent_frames;
    uint32_t dropped_batches;
    uint32_t dropped_frames;
    uint32_t queue_full_events;
    uint32_t backpressure_recoveries;
    uint32_t stale_batches;
    uint32_t stale_frames;
    uint32_t recovery_grace_batches;
    uint32_t recovery_grace_frames;
    uint64_t max_recovery_age_us;
    uint32_t max_ready_depth;
    uint32_t ready_depth;
    uint64_t max_queue_wait_us;
    uint64_t last_encode_us;
    uint64_t last_enqueue_us;
    size_t encoded_bytes_total;
} nuwax_audio_uplink_stats_t;

typedef enum {
    /* Meeting, monitoring and diagnostic streams favor continuity. */
    NUWAX_AUDIO_UPLINK_PROFILE_RELIABLE = 0,
    /* Normal conversation bounds queueing delay to preserve interactivity. */
    NUWAX_AUDIO_UPLINK_PROFILE_REALTIME,
} nuwax_audio_uplink_profile_t;

typedef enum {
    NUWAX_AUDIO_UPLINK_SENDER_IDLE = 0,
    NUWAX_AUDIO_UPLINK_SENDER_WAIT_PACKET,
    NUWAX_AUDIO_UPLINK_SENDER_PREPARE_PACKET,
    NUWAX_AUDIO_UPLINK_SENDER_SEND_PACKET,
    NUWAX_AUDIO_UPLINK_SENDER_AFTER_END,
    NUWAX_AUDIO_UPLINK_SENDER_EXITED,
} nuwax_audio_uplink_sender_stage_t;

/* Runs on the uplink sender task only after the terminal binary packet has
 * been delivered on the bound WebSocket epoch. */
typedef esp_err_t (*nuwax_audio_uplink_after_end_fn)(void *user_ctx,
                                                     uint32_t ws_epoch);

typedef struct {
    nuwax_ws_client_t *ws;
    uint32_t ws_connection_epoch;
    QueueHandle_t free_slots;
    QueueHandle_t ready_packets;
    bool free_slots_with_caps;
    bool ready_packets_with_caps;
    TaskHandle_t task;
    void *task_stack;
    uint8_t *pool;
    uint16_t queue_depth;
    uint16_t ready_queue_depth;
    uint16_t pool_slot_count;
    uint8_t terminal_slot;
    bool terminal_slot_reserved;
    bool terminal_slot_claimed;
    nuwax_audio_uplink_after_end_fn after_end;
    size_t after_end_user_ctx_size;
    uint8_t after_end_user_ctx[NUWAX_AUDIO_UPLINK_AFTER_END_CONTEXT_MAX_BYTES];
    nuwax_audio_uplink_profile_t profile;
    _Atomic bool task_started;
    _Atomic bool capture_done;
    _Atomic bool finished;
    _Atomic bool send_failed;
    _Atomic bool finish_timed_out;
    _Atomic bool end_committed;
    _Atomic int send_result;
    _Atomic int sender_stage;
    _Atomic uint_fast32_t sender_stage_since_ms;
    _Atomic uint_fast32_t sender_packet_sequence;
    _Atomic uint_fast32_t sender_packet_bytes;
    _Atomic bool integrity_close_requested;
    bool terminal_recovery_bound;
    char terminal_recovery_mode[32];
    char terminal_recovery_audio_session_id[48];
    char terminal_recovery_request_id[48];
    uint32_t terminal_recovery_attempt_timeout_ms;
    size_t terminal_snapshot_len;
    uint8_t terminal_snapshot[NUWAX_AUDIO_UPLINK_PACKET_BYTES];
    uint32_t queued_batches;
    uint32_t queued_frames;
    uint32_t sent_batches;
    uint32_t sent_frames;
    uint32_t dropped_batches;
    uint32_t dropped_frames;
    uint32_t queue_full_events;
    uint32_t backpressure_recoveries;
    uint32_t stale_batches;
    uint32_t stale_frames;
    uint32_t recovery_grace_batches;
    uint32_t recovery_grace_frames;
    uint64_t max_recovery_age_us;
    uint32_t realtime_drop_log_ms;
    uint32_t realtime_drop_log_suppressed_batches;
    uint32_t realtime_drop_log_suppressed_frames;
    uint64_t realtime_drop_log_max_queue_wait_us;
    uint32_t max_ready_depth;
    uint64_t max_queue_wait_us;
    bool low_latency_enabled;
    bool low_latency_first_batch_queued;
    uint8_t adaptive_batch_frames;
    size_t batch_len;
    uint32_t batch_count;
    uint32_t batch_start_seq;
    int64_t batch_started_us;
    size_t batch_last_frame_offset;
    uint32_t next_sequence;
    bool frame_seen;
    bool end_enqueued;
    bool backpressure_pending;
    uint64_t last_encode_us;
    uint64_t last_enqueue_us;
    size_t encoded_bytes_total;
} nuwax_audio_uplink_t;

static inline uint32_t nuwax_audio_uplink_send_budget_ms(
    nuwax_audio_uplink_profile_t profile,
    uint64_t enqueued_us,
    uint64_t now_us) {
    if (profile != NUWAX_AUDIO_UPLINK_PROFILE_REALTIME) {
        return UINT32_MAX;
    }
    if (enqueued_us == 0U || now_us < enqueued_us) {
        return NUWAX_AUDIO_UPLINK_REALTIME_MAX_AGE_MS;
    }
    const uint64_t max_age_us =
        (uint64_t)NUWAX_AUDIO_UPLINK_REALTIME_MAX_AGE_MS * 1000ULL;
    const uint64_t age_us = now_us - enqueued_us;
    if (age_us >= max_age_us) {
        return 0U;
    }
    return (uint32_t)((max_age_us - age_us + 999ULL) / 1000ULL);
}

static inline uint32_t nuwax_audio_uplink_recovery_send_budget_ms(
    nuwax_audio_uplink_profile_t profile,
    uint64_t enqueued_us,
    uint64_t now_us) {
    if (profile != NUWAX_AUDIO_UPLINK_PROFILE_REALTIME) {
        return UINT32_MAX;
    }
    if (enqueued_us == 0U || now_us < enqueued_us) {
        return NUWAX_AUDIO_UPLINK_REALTIME_RECOVERY_MAX_AGE_MS;
    }
    const uint64_t max_age_us =
        (uint64_t)NUWAX_AUDIO_UPLINK_REALTIME_RECOVERY_MAX_AGE_MS * 1000ULL;
    const uint64_t age_us = now_us - enqueued_us;
    if (age_us >= max_age_us) {
        return 0U;
    }
    return (uint32_t)((max_age_us - age_us + 999ULL) / 1000ULL);
}

static inline uint32_t nuwax_audio_uplink_packet_send_budget_ms(
    nuwax_audio_uplink_profile_t profile,
    bool final,
    uint64_t enqueued_us,
    uint64_t now_us) {
    if (profile == NUWAX_AUDIO_UPLINK_PROFILE_REALTIME && final) {
        return NUWAX_AUDIO_UPLINK_REALTIME_TERMINAL_SEND_TIMEOUT_MS;
    }
    return nuwax_audio_uplink_send_budget_ms(profile, enqueued_us, now_us);
}

static inline uint32_t nuwax_audio_uplink_packet_recovery_send_budget_ms(
    nuwax_audio_uplink_profile_t profile,
    bool final,
    uint64_t enqueued_us,
    uint64_t now_us) {
    if (profile == NUWAX_AUDIO_UPLINK_PROFILE_REALTIME && final) {
        return NUWAX_AUDIO_UPLINK_REALTIME_TERMINAL_SEND_TIMEOUT_MS;
    }
    return nuwax_audio_uplink_recovery_send_budget_ms(
        profile, enqueued_us, now_us);
}

/* ws_transport retains a connection after ESP_ERR_TIMEOUT only when no
 * WebSocket frame byte was committed and the transport can safely abandon
 * that input. Ordinary realtime audio may be skipped. END remains
 * transactional and the sender retries the exact retained terminal buffer. */
static inline bool nuwax_audio_uplink_can_skip_unsent_data_packet(
    nuwax_audio_uplink_profile_t profile,
    bool final,
    esp_err_t send_result,
    bool ws_connected,
    bool ws_epoch_matches) {
    return profile == NUWAX_AUDIO_UPLINK_PROFILE_REALTIME && !final &&
           send_result == ESP_ERR_TIMEOUT && ws_connected &&
           ws_epoch_matches;
}

/* ESP_ERR_NOT_FINISHED means the realtime queue could not admit the batch
 * within its latency budget, but the last supplied frame is still retained in
 * the staging batch. The caller must stop supplying frames and call finish().
 * A successful finish only closes the partial stream; it does not make the
 * truncated capture complete. */
static inline bool nuwax_audio_uplink_is_recoverable_backpressure(
    esp_err_t result) {
    return result == ESP_ERR_NOT_FINISHED;
}

static inline esp_err_t nuwax_audio_uplink_queue_pressure_result(
    nuwax_audio_uplink_profile_t profile,
    bool final,
    uint32_t frames) {
    if (profile == NUWAX_AUDIO_UPLINK_PROFILE_REALTIME && !final &&
        frames > 0U) {
        return ESP_ERR_NOT_FINISHED;
    }
    return final ? ESP_ERR_TIMEOUT : ESP_ERR_NO_MEM;
}

static inline uint32_t nuwax_audio_uplink_admission_wait_ms(
    nuwax_audio_uplink_profile_t profile,
    bool final) {
    if (final) {
        return profile == NUWAX_AUDIO_UPLINK_PROFILE_REALTIME
                   ? NUWAX_AUDIO_UPLINK_REALTIME_FINAL_ADMISSION_WAIT_MS
                   : NUWAX_AUDIO_UPLINK_FINAL_ADMISSION_WAIT_MS;
    }
    return profile == NUWAX_AUDIO_UPLINK_PROFILE_REALTIME
               ? NUWAX_AUDIO_UPLINK_REALTIME_ADMISSION_WAIT_MS
               : NUWAX_AUDIO_UPLINK_RELIABLE_ADMISSION_WAIT_MS;
}

static inline bool nuwax_audio_uplink_reserves_terminal_slot(
    nuwax_audio_uplink_profile_t profile) {
    return profile == NUWAX_AUDIO_UPLINK_PROFILE_REALTIME;
}

static inline uint32_t nuwax_audio_uplink_pool_slot_count(
    nuwax_audio_uplink_profile_t profile,
    uint32_t ordinary_queue_depth) {
    return ordinary_queue_depth +
           (nuwax_audio_uplink_reserves_terminal_slot(profile) ? 1U : 0U);
}

static inline uint32_t nuwax_audio_uplink_ready_queue_depth(
    nuwax_audio_uplink_profile_t profile,
    uint32_t ordinary_queue_depth) {
    return nuwax_audio_uplink_pool_slot_count(profile, ordinary_queue_depth);
}

static inline bool nuwax_audio_uplink_enqueue_frame_accepted(
    esp_err_t result) {
    return result == ESP_OK ||
           nuwax_audio_uplink_is_recoverable_backpressure(result);
}

static inline esp_err_t nuwax_audio_uplink_resolve_capture_finish_result(
    esp_err_t capture_result,
    esp_err_t finish_result) {
    if (nuwax_audio_uplink_is_recoverable_backpressure(capture_result)) {
        return finish_result == ESP_OK ? capture_result : finish_result;
    }
    return capture_result == ESP_OK ? finish_result : capture_result;
}

static inline uint32_t nuwax_audio_uplink_batch_target_frames(
    const nuwax_audio_uplink_t *ctx) {
    if (!ctx || !ctx->low_latency_enabled) {
        return NUWAX_AUDIO_UPLINK_STREAM_BATCH_FRAMES;
    }
    if (!ctx->low_latency_first_batch_queued) {
        return NUWAX_AUDIO_UPLINK_LOW_LATENCY_FIRST_BATCH_FRAMES;
    }
    return ctx->adaptive_batch_frames == 0U
               ? NUWAX_AUDIO_UPLINK_LOW_LATENCY_STEADY_BATCH_FRAMES
               : ctx->adaptive_batch_frames;
}

static inline uint32_t nuwax_audio_uplink_adaptive_batch_frames(
    uint32_t ready_depth,
    uint32_t capacity) {
    if (capacity < 4U) {
        return NUWAX_AUDIO_UPLINK_LOW_LATENCY_STEADY_BATCH_FRAMES;
    }
    if (ready_depth >= (capacity + 1U) / 2U) {
        return NUWAX_AUDIO_UPLINK_STREAM_BATCH_FRAMES;
    }
    if (ready_depth >= (capacity + 3U) / 4U) {
        return 2U;
    }
    return NUWAX_AUDIO_UPLINK_LOW_LATENCY_STEADY_BATCH_FRAMES;
}

static inline void nuwax_audio_uplink_record_batch_result(
    nuwax_audio_uplink_t *ctx,
    bool queued) {
    if (ctx && queued && ctx->low_latency_enabled) {
        ctx->low_latency_first_batch_queued = true;
    }
}

/* Call after start and before the first frame. start/destroy reset the mode. */
static inline esp_err_t nuwax_audio_uplink_set_low_latency(
    nuwax_audio_uplink_t *ctx,
    bool enabled) {
    if (!ctx) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ctx->frame_seen || ctx->batch_count != 0 || ctx->queued_batches != 0 ||
        ctx->end_enqueued) {
        return ESP_ERR_INVALID_STATE;
    }
    ctx->low_latency_enabled = enabled;
    ctx->low_latency_first_batch_queued = false;
    ctx->adaptive_batch_frames =
        NUWAX_AUDIO_UPLINK_LOW_LATENCY_STEADY_BATCH_FRAMES;
    return ESP_OK;
}

size_t nuwax_audio_uplink_samples_per_frame(void);
uint32_t nuwax_audio_uplink_batch_frames(void);
uint32_t nuwax_audio_uplink_queue_depth(void);
uint32_t nuwax_audio_uplink_queue_depth_for_profile(
    nuwax_audio_uplink_profile_t profile);
uint32_t nuwax_audio_uplink_context_queue_depth(
    const nuwax_audio_uplink_t *ctx);

esp_err_t nuwax_audio_uplink_build_pcm16_frame(uint8_t *out,
                                               size_t out_capacity,
                                               const int16_t *samples,
                                               size_t sample_count,
                                               uint32_t sequence,
                                               uint8_t flags,
                                               size_t *frame_len);

#if CONFIG_NUWAX_VOICE_UPLINK_CODEC_OPUS
esp_err_t nuwax_audio_uplink_ensure_encoded_frame_capacity(size_t required_bytes);
#endif

esp_err_t nuwax_audio_uplink_start(nuwax_audio_uplink_t *ctx, nuwax_ws_client_t *ws);
/* Heap-owned session helpers. The detached cleanup path keeps the session
 * alive until an asynchronous sender has acknowledged cancellation. */
nuwax_audio_uplink_t *nuwax_audio_uplink_create(void);
esp_err_t nuwax_audio_uplink_destroy_owned(nuwax_audio_uplink_t **owner);
esp_err_t nuwax_audio_uplink_start_if_epoch(nuwax_audio_uplink_t *ctx,
                                            nuwax_ws_client_t *ws,
                                            uint32_t expected_ws_epoch);
esp_err_t nuwax_audio_uplink_start_with_profile(
    nuwax_audio_uplink_t *ctx,
    nuwax_ws_client_t *ws,
    nuwax_audio_uplink_profile_t profile);
esp_err_t nuwax_audio_uplink_start_with_profile_if_epoch(
    nuwax_audio_uplink_t *ctx,
    nuwax_ws_client_t *ws,
    nuwax_audio_uplink_profile_t profile,
    uint32_t expected_ws_epoch);

/* Bind terminal recovery metadata after the local sender is prepared and
 * before the cloud listen start is committed. The metadata is reset
 * automatically when the heap-owned uplink is reused for a new session. */
esp_err_t nuwax_audio_uplink_bind_terminal_recovery(
    nuwax_audio_uplink_t *ctx,
    const char *mode,
    const char *audio_session_id,
    const char *request_id,
    uint32_t attempt_timeout_ms);

esp_err_t nuwax_audio_uplink_enqueue_frame(nuwax_audio_uplink_t *ctx,
                                           nuwax_opus_encoder_t *opus_encoder,
                                           const int16_t *samples,
                                           size_t sample_count,
                                           uint32_t seq,
                                           uint8_t flags,
                                           bool final_frame);

esp_err_t nuwax_audio_uplink_finish(nuwax_audio_uplink_t *ctx);
/* Queue the terminal END without waiting for the sender task.  The capture
 * owner can keep draining its independent RT-capture queue and poll completion
 * at subsequent frame boundaries. */
esp_err_t nuwax_audio_uplink_request_finish(nuwax_audio_uplink_t *ctx);
esp_err_t nuwax_audio_uplink_request_finish_then(
    nuwax_audio_uplink_t *ctx,
    nuwax_audio_uplink_after_end_fn after_end,
    const void *user_ctx,
    size_t user_ctx_size);
bool nuwax_audio_uplink_finish_is_complete(const nuwax_audio_uplink_t *ctx,
                                           esp_err_t *out_result);
bool nuwax_audio_uplink_end_committed(const nuwax_audio_uplink_t *ctx);
/* True while the singleton sender task is still owned by an uplink context.
 * Terminal recovery must not retransmit an END until this becomes false,
 * because the original sender may still be completing the same frame. */
bool nuwax_audio_uplink_sender_busy(void);
/* Tear down a prepared sender before listen-start has committed. This path
 * deliberately emits neither binary audio nor a terminal marker. */
esp_err_t nuwax_audio_uplink_discard_uncommitted(nuwax_audio_uplink_t *ctx);
void nuwax_audio_uplink_destroy(nuwax_audio_uplink_t *ctx);
void nuwax_audio_uplink_get_stats(const nuwax_audio_uplink_t *ctx, nuwax_audio_uplink_stats_t *stats);
