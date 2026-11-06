/* Host-only capture-path check. This compiles the production monitoring mode
 * with I2S enabled and replaces hardware/uplink functions with deterministic
 * fakes so frame preservation and teardown ordering are directly verified. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "audio_frame.h"
#include "audio_uplink.h"
#include "cJSON.h"
#include "mic_input.h"
#include "monitoring_mode.h"
#include "session_protocol.h"
#include "audio_uplink_producer_host.h"
#if CONFIG_NUWAX_OFFLINE_AUDIO_ENABLE
#include "offline_audio_voice.h"
#endif

#define HOST_FRAME_SAMPLES \
    ((CONFIG_NUWAX_VOICE_SAMPLE_RATE * CONFIG_NUWAX_VOICE_FRAME_DURATION_MS) / 1000)
#define HOST_FRAME_COUNT 20U
#define HOST_READ_CHUNK_SAMPLES 137U
#define HOST_SENT_MESSAGE_CAPACITY 32U
#define HOST_SENT_MESSAGE_BYTES 256U
#define HOST_EXPECTED_UPLOADED_FRAMES 13U
#define HOST_EXPECTED_STREAMS 2U
#define HOST_MAX_UPLOADED_FRAMES 32U
#define HOST_MAX_STREAMS 4U

typedef enum {
    HOST_CAPTURE_ACK_SUCCESS = 0,
    HOST_CAPTURE_ACK_DROP_PAUSE,
    HOST_CAPTURE_ACK_REJECT_RESUME,
} host_capture_ack_policy_t;

struct nuwax_host_queue {
    uint8_t *storage;
    UBaseType_t length;
    UBaseType_t item_size;
    UBaseType_t head;
    UBaseType_t count;
};

static int64_t s_now_us;
static int64_t s_mic_start_delay_us;
static uint32_t s_random_value = 0x30405060U;
static nuwax_monitoring_mode_t *s_mode;
static nuwax_ws_client_t *s_ws;
static uint32_t s_read_index;
static size_t s_read_offset;
static uint32_t s_enqueued_count;
static uint32_t s_enqueued_sequences[HOST_MAX_UPLOADED_FRAMES];
static uint8_t s_enqueued_flags[HOST_MAX_UPLOADED_FRAMES];
static uint8_t s_enqueued_streams[HOST_MAX_UPLOADED_FRAMES];
static int16_t s_enqueued_samples[HOST_MAX_UPLOADED_FRAMES][HOST_FRAME_SAMPLES];
static size_t s_enqueued_sample_counts[HOST_MAX_UPLOADED_FRAMES];
static char s_sent_messages[HOST_SENT_MESSAGE_CAPACITY][HOST_SENT_MESSAGE_BYTES];
static size_t s_sent_count;
static bool s_playback_active;
static uint32_t s_playback_checks_remaining;
static uint32_t s_fade_calls;
static uint32_t s_order_counter;
static uint32_t s_finish_order[HOST_MAX_STREAMS];
static uint32_t s_uplink_start_order[HOST_MAX_STREAMS];
static uint32_t s_mic_release_order;
static uint32_t s_listen_start_order[HOST_MAX_STREAMS];
static uint32_t s_listen_stop_order[HOST_MAX_STREAMS];
static uint32_t s_disabled_ack_order;
static uint32_t s_pause_request_order;
static uint32_t s_resume_request_order;
static uint32_t s_pause_request_count;
static uint32_t s_resume_request_count;
static host_capture_ack_policy_t s_capture_ack_policy;
static char s_resume_audio_session_id[NUWAX_MONITORING_AUDIO_SESSION_ID_BYTES];
static char s_audio_session_ids[HOST_MAX_STREAMS]
                               [NUWAX_MONITORING_AUDIO_SESSION_ID_BYTES];
static size_t s_listen_start_count;
static size_t s_listen_start_committed_count;
static size_t s_listen_stop_count;
static size_t s_uplink_start_count;
static size_t s_uplink_finish_count;
static size_t s_uplink_discard_count;
static size_t s_close_request_count;
static bool s_fail_listen_start;
static bool s_fail_uplink_finish;
static bool s_fail_mic_deinit;
static uint32_t s_read_discontinuity_frame;
static bool s_read_discontinuity_injected;
static uint32_t s_active_ack_failures_remaining;
static uint32_t s_active_ack_attempts;

#if CONFIG_NUWAX_OFFLINE_AUDIO_ENABLE
static uint32_t s_archived_mask;
static uint32_t s_archived_while_paused;
#if defined(NUWAX_HOST_HEAP_CAPS_FAULT_INJECT)
bool nuwax_host_heap_caps_fail_calloc;
#endif

esp_err_t nuwax_offline_audio_mirror_service(nuwax_offline_audio_mirror_t *m,
    nuwax_offline_audio_mode_t mode, const char *session, uint32_t rate,
    uint8_t channels, uint16_t duration, uint32_t now) {
    (void)now;
    assert(strcmp(session, s_mode->identity.session_id) == 0);
    assert(mode == NUWAX_OFFLINE_AUDIO_MODE_MONITORING);
    assert(rate == 16000U && channels == 1U && duration == 20U);
    m->started = true;
    return ESP_OK;
}

esp_err_t nuwax_offline_audio_mirror_append(nuwax_offline_audio_mirror_t *m,
    const nuwax_offline_audio_frame_t *frame, uint32_t now) {
    (void)now;
    assert(m->started && frame->sample_count == HOST_FRAME_SAMPLES);
    const uint32_t source_sequence = frame->sequence -
        (s_mode->identity.resuming ? 2829U : 0U);
    assert(source_sequence < 32U);
    const uint32_t bit = 1U << source_sequence;
    assert((s_archived_mask & bit) == 0U); /* Uplink preroll cannot duplicate SD. */
    s_archived_mask |= bit;
    const nuwax_monitoring_capture_phase_t phase =
        nuwax_monitoring_mode_capture_phase(s_mode);
    if (phase == NUWAX_MONITORING_CAPTURE_PAUSED ||
        phase == NUWAX_MONITORING_CAPTURE_RESUME_PENDING) {
        ++s_archived_while_paused;
    }
    return ESP_OK;
}

esp_err_t nuwax_offline_audio_mirror_request_close(nuwax_offline_audio_mirror_t *m,
    bool complete, uint32_t now) {
    (void)complete; (void)now;
    m->started = false;
    return ESP_OK;
}
#endif

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size) {
    if (length == 0U || item_size == 0U) {
        return NULL;
    }
    struct nuwax_host_queue *queue = calloc(1U, sizeof(*queue));
    assert(queue != NULL);
    queue->storage = calloc(length, item_size);
    assert(queue->storage != NULL);
    queue->length = length;
    queue->item_size = item_size;
    return queue;
}

BaseType_t xQueueSend(QueueHandle_t queue,
                      const void *item,
                      TickType_t wait_ticks) {
    (void)wait_ticks;
    if (!queue || !item || queue->count >= queue->length) {
        return pdFALSE;
    }
    const UBaseType_t tail = (queue->head + queue->count) % queue->length;
    memcpy(queue->storage + (size_t)tail * queue->item_size,
           item,
           queue->item_size);
    queue->count++;
    return pdTRUE;
}

BaseType_t xQueueOverwrite(QueueHandle_t queue, const void *item) {
    if (!queue || !item || queue->length != 1U) {
        return pdFALSE;
    }
    memcpy(queue->storage, item, queue->item_size);
    queue->head = 0U;
    queue->count = 1U;
    return pdTRUE;
}

BaseType_t xQueuePeek(QueueHandle_t queue, void *item, TickType_t wait_ticks) {
    (void)wait_ticks;
    if (!queue || !item || queue->count == 0U) return pdFALSE;
    memcpy(item, queue->storage + (size_t)queue->head * queue->item_size, queue->item_size);
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t queue,
                         void *item,
                         TickType_t wait_ticks) {
    (void)wait_ticks;
    if (!queue || !item || queue->count == 0U) {
        return pdFALSE;
    }
    memcpy(item,
           queue->storage + (size_t)queue->head * queue->item_size,
           queue->item_size);
    queue->head = (queue->head + 1U) % queue->length;
    queue->count--;
    return pdTRUE;
}

UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue) {
    return queue ? queue->count : 0U;
}

void vQueueDelete(QueueHandle_t queue) {
    if (!queue) {
        return;
    }
    free(queue->storage);
    free(queue);
}

void vTaskDelay(TickType_t ticks) {
    s_now_us += (int64_t)ticks * 1000LL;
}

int64_t esp_timer_get_time(void) {
    return s_now_us;
}

uint32_t esp_random(void) {
    return ++s_random_value;
}

bool nuwax_ws_is_connected(const nuwax_ws_client_t *client) {
    return client && client->connected;
}

uint32_t nuwax_ws_connection_epoch(const nuwax_ws_client_t *client) {
    return client ? (uint32_t)client->connection_epoch : 0U;
}

bool nuwax_ws_request_close_if_epoch(nuwax_ws_client_t *client,
                                     uint32_t expected_epoch) {
    assert(client != NULL);
    assert(nuwax_ws_connection_epoch(client) == expected_epoch);
    s_close_request_count++;
    return true;
}

esp_err_t nuwax_ws_send_text(nuwax_ws_client_t *client, const char *payload) {
    assert(nuwax_ws_is_connected(client));
    assert(payload != NULL);
    if (strstr(payload, "\"type\":\"device_command_ack\"") &&
        strstr(payload, "\"message\":\"active\"")) {
        s_active_ack_attempts++;
        if (s_active_ack_failures_remaining > 0U) {
            s_active_ack_failures_remaining--;
            s_now_us += 500000LL;
            return ESP_ERR_TIMEOUT;
        }
    }
    assert(s_sent_count < HOST_SENT_MESSAGE_CAPACITY);
    const size_t length = strlen(payload);
    assert(length < HOST_SENT_MESSAGE_BYTES);
    memcpy(s_sent_messages[s_sent_count], payload, length + 1U);
    s_sent_count++;
    if (strstr(payload, "\"message\":\"disabled\"")) {
        s_disabled_ack_order = ++s_order_counter;
    }
    if (strstr(payload, "\"type\":\"monitoring_capture_control\"")) {
        cJSON *request = cJSON_Parse(payload);
        assert(request != NULL);
        const cJSON *request_id =
            cJSON_GetObjectItemCaseSensitive(request, "request_id");
        const cJSON *state = cJSON_GetObjectItemCaseSensitive(request, "state");
        const cJSON *audio_session_id =
            cJSON_GetObjectItemCaseSensitive(request, "audio_session_id");
        assert(cJSON_IsString(request_id) && request_id->valuestring);
        assert(cJSON_IsString(state) && state->valuestring);
        assert(cJSON_IsString(audio_session_id) && audio_session_id->valuestring);
        const bool pause = strcmp(state->valuestring, "pause") == 0;
        if (pause) {
            s_pause_request_order = ++s_order_counter;
            s_pause_request_count++;
        } else {
            assert(strcmp(state->valuestring, "resume") == 0);
            s_resume_request_order = ++s_order_counter;
            s_resume_request_count++;
            strcpy(s_resume_audio_session_id, audio_session_id->valuestring);
        }
        if (pause &&
            s_capture_ack_policy == HOST_CAPTURE_ACK_DROP_PAUSE) {
            cJSON_Delete(request);
            return ESP_OK;
        }
        const bool reject =
            !pause &&
            s_capture_ack_policy == HOST_CAPTURE_ACK_REJECT_RESUME;
        char ack[320];
        const int written = snprintf(
            ack,
            sizeof(ack),
            "{\"type\":\"monitoring_capture_control_ack\"," 
            "\"request_id\":\"%s\",\"mode\":\"monitoring\"," 
            "\"state\":\"%s\",\"audio_session_id\":\"%s\"," 
            "\"ok\":%s,\"reason\":\"%s\"}",
            request_id->valuestring,
            state->valuestring,
            audio_session_id->valuestring,
            reject ? "false" : "true",
            reject ? "monitoring_not_paused" : "acknowledged");
        assert(written > 0 && written < (int)sizeof(ack));
        assert(nuwax_monitoring_mode_handle_protocol_text(
            s_mode, s_ws, ack, written));
        if (!reject) {
            const int stale_written = snprintf(
                ack,
                sizeof(ack),
                "{\"type\":\"monitoring_capture_control_ack\","
                "\"request_id\":\"stale_after_correct\","
                "\"mode\":\"monitoring\",\"state\":\"%s\","
                "\"audio_session_id\":\"%s\",\"ok\":true}",
                state->valuestring,
                audio_session_id->valuestring);
            assert(stale_written > 0 && stale_written < (int)sizeof(ack));
            assert(nuwax_monitoring_mode_handle_protocol_text(
                s_mode, s_ws, ack, stale_written));
        }
        cJSON_Delete(request);
    }
    return ESP_OK;
}

esp_err_t nuwax_session_send_listen_with_audio_session_until_if_epoch(
    nuwax_ws_client_t *ws,
    const char *state,
    const char *mode,
    const char *audio_session_id,
    const char *request_id,
    int64_t deadline_us,
    uint32_t expected_ws_epoch) {
    assert(nuwax_ws_is_connected(ws));
    assert(deadline_us > s_now_us);
    assert(nuwax_ws_connection_epoch(ws) == expected_ws_epoch);
    assert(strcmp(mode, "monitoring") == 0);
    assert(audio_session_id != NULL && audio_session_id[0] != '\0');
    if (strcmp(state, "start") == 0) {
        assert(s_listen_start_count < HOST_MAX_STREAMS);
        if (s_listen_start_count == 0U) {
            assert(request_id != NULL && strcmp(request_id, "capture_open") == 0);
        } else {
            assert(request_id == NULL);
        }
        strcpy(s_audio_session_ids[s_listen_start_count], audio_session_id);
        if (s_listen_start_count > 0U) {
            assert(strcmp(s_audio_session_ids[s_listen_start_count - 1U],
                          audio_session_id) == 0);
            assert(strcmp(s_resume_audio_session_id, audio_session_id) == 0);
        }
        s_listen_start_order[s_listen_start_count] = ++s_order_counter;
        s_listen_start_count++;
        if (s_fail_listen_start) {
            return ESP_ERR_TIMEOUT;
        }
        s_listen_start_committed_count++;
    } else {
        assert(strcmp(state, "stop") == 0);
        assert(request_id == NULL);
        assert(s_listen_stop_count < s_listen_start_committed_count);
        assert(strcmp(s_audio_session_ids[s_listen_stop_count],
                      audio_session_id) == 0);
        s_listen_stop_order[s_listen_stop_count] = ++s_order_counter;
        s_listen_stop_count++;
    }
    return ESP_OK;
}

esp_err_t nuwax_session_send_listen_stop_retry_if_epoch(
    nuwax_ws_client_t *ws,
    const char *mode,
    const char *audio_session_id,
    const char *request_id,
    uint32_t attempt_timeout_ms,
    uint32_t expected_ws_epoch) {
    assert(attempt_timeout_ms > 0U);
    return nuwax_session_send_listen_with_audio_session_until_if_epoch(
        ws,
        "stop",
        mode,
        audio_session_id,
        request_id,
        s_now_us + (int64_t)attempt_timeout_ms * 1000LL,
        expected_ws_epoch);
}

static bool host_frame_is_speech(uint32_t frame) {
    return (frame >= 3U && frame <= 10U) ||
           (frame >= 15U && frame <= 18U);
}

static int16_t host_sample(uint32_t frame, size_t offset) {
    if (frame >= 3U && frame <= 5U) {
        return (int16_t)(2000 + (int32_t)frame * 100 + (int32_t)(offset % 50U));
    }
    if ((frame >= 6U && frame <= 10U) ||
        (frame >= 15U && frame <= 18U)) {
        return (offset % 40U) == 0U ? 900 : 240;
    }
    return (int16_t)((int32_t)frame * 10 + (int32_t)(offset % 5U));
}

esp_err_t nuwax_mic_input_init_with_owner(nuwax_mic_input_t *mic,
                                          nuwax_mic_owner_t owner) {
    assert(mic != NULL);
    assert(owner == NUWAX_MIC_OWNER_MONITORING_CAPTURE);
    assert(s_listen_start_committed_count > 0U);
    mic->initialized = true;
    mic->owner = owner;
    mic->owner_acquired = true;
    mic->rx_handle = mic;
    return ESP_OK;
}

static esp_err_t acquire_mic_for_resume(void *ctx, nuwax_mic_input_t *mic, nuwax_mic_owner_t owner) {
    (void)ctx;
    s_now_us += s_mic_start_delay_us;
    return nuwax_mic_input_init_with_owner(mic, owner);
}

esp_err_t nuwax_mic_input_read_pcm16(nuwax_mic_input_t *mic,
                                     int16_t *samples,
                                     size_t sample_count,
                                     uint32_t timeout_ms,
                                     size_t *samples_read) {
    assert(mic != NULL && mic->initialized);
    assert(samples != NULL && samples_read != NULL);
    assert(sample_count > 0U && sample_count <= HOST_FRAME_SAMPLES);
    assert(timeout_ms == CONFIG_NUWAX_MIC_READ_TIMEOUT_MS);
    assert(s_read_index < HOST_FRAME_COUNT);
    if (!s_read_discontinuity_injected && s_read_offset == 0U &&
        s_read_index == s_read_discontinuity_frame) {
        s_read_discontinuity_injected = true;
        *samples_read = 0U;
        s_now_us +=
            (int64_t)CONFIG_NUWAX_VOICE_FRAME_DURATION_MS * 1000LL;
        return ESP_ERR_INVALID_RESPONSE;
    }
    size_t chunk_samples = sample_count;
    if (chunk_samples > HOST_READ_CHUNK_SAMPLES) {
        chunk_samples = HOST_READ_CHUNK_SAMPLES;
    }
    for (size_t i = 0U; i < chunk_samples; ++i) {
        samples[i] = host_sample(s_read_index, s_read_offset + i);
    }
    *samples_read = chunk_samples;
    s_read_offset += chunk_samples;
    if (s_read_offset == HOST_FRAME_SAMPLES) {
        s_read_offset = 0U;
        s_read_index++;
        s_now_us +=
            (int64_t)CONFIG_NUWAX_VOICE_FRAME_DURATION_MS * 1000LL;
        if (s_read_index == HOST_FRAME_COUNT) {
            static const char close_command[] =
                "{\"type\":\"device_command\",\"command\":\"set_audio_mode\"," 
                "\"request_id\":\"capture_close\",\"mode\":\"monitoring\"," 
                "\"state\":\"disabled\"}";
            assert(nuwax_monitoring_mode_handle_protocol_text(
                s_mode, s_ws, close_command, (int)strlen(close_command)));
        }
    }
    return ESP_OK;
}

esp_err_t nuwax_mic_input_deinit(nuwax_mic_input_t *mic) {
    assert(mic != NULL && mic->owner == NUWAX_MIC_OWNER_MONITORING_CAPTURE);
    s_mic_release_order = ++s_order_counter;
    memset(mic, 0, sizeof(*mic));
    return s_fail_mic_deinit ? ESP_ERR_TIMEOUT : ESP_OK;
}

esp_err_t nuwax_audio_uplink_start_if_epoch(nuwax_audio_uplink_t *ctx,
                                            nuwax_ws_client_t *ws,
                                            uint32_t expected_ws_epoch) {
    assert(ctx != NULL && nuwax_ws_is_connected(ws));
    assert(nuwax_ws_connection_epoch(ws) == expected_ws_epoch);
    assert(s_uplink_start_count < HOST_MAX_STREAMS);
    memset(ctx, 0, sizeof(*ctx));
    ctx->ws = ws;
    ctx->ws_connection_epoch = expected_ws_epoch;
    s_uplink_start_order[s_uplink_start_count] = ++s_order_counter;
    s_uplink_start_count++;
    return ESP_OK;
}

esp_err_t nuwax_audio_uplink_bind_terminal_recovery(
    nuwax_audio_uplink_t *ctx,
    const char *mode,
    const char *audio_session_id,
    const char *request_id,
    uint32_t attempt_timeout_ms) {
    assert(ctx && strcmp(mode, "monitoring") == 0);
    assert(audio_session_id && audio_session_id[0] != '\0');
    assert(request_id == NULL);
    assert(attempt_timeout_ms > 0U);
    return ESP_OK;
}

bool nuwax_audio_uplink_end_committed(const nuwax_audio_uplink_t *ctx) {
    assert(ctx != NULL);
    return !s_fail_uplink_finish;
}

bool nuwax_audio_terminal_recovery_pending(void) {
    return false;
}

esp_err_t nuwax_audio_terminal_recovery_arm(
    nuwax_ws_client_t *ws,
    uint32_t ws_epoch,
    const uint8_t *packet,
    size_t packet_len,
    bool end_committed,
    const char *mode,
    const char *audio_session_id,
    const char *request_id,
    uint32_t attempt_timeout_ms) {
    assert(ws == s_ws && ws_epoch == nuwax_ws_connection_epoch(ws));
    assert(packet == NULL && packet_len == 0U && end_committed);
    assert(strcmp(mode, "monitoring") == 0);
    assert(audio_session_id && audio_session_id[0] != '\0');
    assert(request_id == NULL && attempt_timeout_ms > 0U);
    return ESP_OK;
}

nuwax_audio_uplink_t *nuwax_audio_uplink_create(void) {
    return (nuwax_audio_uplink_t *)calloc(1, sizeof(nuwax_audio_uplink_t));
}

esp_err_t nuwax_audio_uplink_destroy_owned(nuwax_audio_uplink_t **owner) {
    if (owner && *owner) {
        nuwax_audio_uplink_destroy(*owner);
        free(*owner);
        *owner = NULL;
    }
    return ESP_OK;
}

esp_err_t nuwax_audio_uplink_enqueue_frame(nuwax_audio_uplink_t *ctx,
                                           nuwax_opus_encoder_t *opus_encoder,
                                           const int16_t *samples,
                                           size_t sample_count,
                                           uint32_t seq,
                                           uint8_t flags,
                                           bool final_frame) {
    assert(ctx != NULL && ctx->ws == s_ws);
    assert(opus_encoder == NULL);
    assert(samples != NULL && sample_count == HOST_FRAME_SAMPLES);
    assert(s_enqueued_count < HOST_MAX_UPLOADED_FRAMES);
    const nuwax_monitoring_capture_phase_t phase =
        nuwax_monitoring_mode_capture_phase(s_mode);
    assert(phase == NUWAX_MONITORING_CAPTURE_STREAMING ||
           phase == NUWAX_MONITORING_CAPTURE_PAUSE_PENDING);
    assert(final_frame == ((flags & NUWAX_AUDIO_FLAG_END) != 0U));
    assert(host_production_enqueue_frame(ctx, opus_encoder, samples, sample_count,
                                        seq, flags, final_frame) == ESP_OK);
    s_enqueued_sequences[s_enqueued_count] = seq;
    s_enqueued_flags[s_enqueued_count] = flags;
    s_enqueued_streams[s_enqueued_count] = (uint8_t)(s_uplink_start_count - 1U);
    s_enqueued_sample_counts[s_enqueued_count] = sample_count;
    memcpy(s_enqueued_samples[s_enqueued_count],
           samples,
           sample_count * sizeof(int16_t));
    s_enqueued_count++;
    return ESP_OK;
}

esp_err_t nuwax_audio_uplink_finish(nuwax_audio_uplink_t *ctx) {
    assert(ctx != NULL);
    assert(s_uplink_finish_count < HOST_MAX_STREAMS);
    s_finish_order[s_uplink_finish_count] = ++s_order_counter;
    s_uplink_finish_count++;
    return s_fail_uplink_finish ? ESP_ERR_TIMEOUT : ESP_OK;
}

esp_err_t nuwax_audio_uplink_discard_uncommitted(nuwax_audio_uplink_t *ctx) {
    assert(ctx != NULL && ctx->ws == s_ws);
    assert(s_enqueued_count == 0U);
    s_uplink_discard_count++;
    memset(ctx, 0, sizeof(*ctx));
    return ESP_OK;
}

void nuwax_audio_uplink_destroy(nuwax_audio_uplink_t *ctx) {
    assert(ctx != NULL);
}

void nuwax_audio_uplink_get_stats(const nuwax_audio_uplink_t *ctx,
                                  nuwax_audio_uplink_stats_t *stats) {
    assert(ctx != NULL && stats != NULL);
    memset(stats, 0, sizeof(*stats));
    stats->queued_frames = s_enqueued_count;
    stats->sent_frames = s_enqueued_count;
}

static bool playback_active(void *ctx) {
    (void)ctx;
    if (!s_playback_active) {
        return false;
    }
    if (s_playback_checks_remaining > 0U) {
        s_playback_checks_remaining--;
        return true;
    }
    s_playback_active = false;
    return false;
}

static void fade_playback(void *ctx, uint32_t fade_ms, const char *reason) {
    (void)ctx;
    assert(fade_ms == 200U);
    assert(reason != NULL && strcmp(reason, "monitoring_cloud_enable") == 0);
    s_fade_calls++;
}

static bool sent_message_contains(const char *request_id,
                                  const char *fragment) {
    for (size_t i = 0U; i < s_sent_count; ++i) {
        if (strstr(s_sent_messages[i], request_id) &&
            strstr(s_sent_messages[i], fragment)) {
            return true;
        }
    }
    return false;
}

static void reset_capture_fakes(void) {
#if CONFIG_NUWAX_OFFLINE_AUDIO_ENABLE
    s_archived_mask = 0U;
    s_archived_while_paused = 0U;
#endif
    s_now_us = 0;
    s_read_index = 0U;
    s_read_offset = 0U;
    s_enqueued_count = 0U;
    memset(s_enqueued_sequences, 0, sizeof(s_enqueued_sequences));
    memset(s_enqueued_flags, 0, sizeof(s_enqueued_flags));
    memset(s_enqueued_streams, 0, sizeof(s_enqueued_streams));
    memset(s_enqueued_samples, 0, sizeof(s_enqueued_samples));
    memset(s_enqueued_sample_counts, 0, sizeof(s_enqueued_sample_counts));
    memset(s_sent_messages, 0, sizeof(s_sent_messages));
    s_sent_count = 0U;
    s_playback_active = false;
    s_playback_checks_remaining = 0U;
    s_fade_calls = 0U;
    s_order_counter = 0U;
    memset(s_finish_order, 0, sizeof(s_finish_order));
    memset(s_uplink_start_order, 0, sizeof(s_uplink_start_order));
    s_mic_release_order = 0U;
    memset(s_listen_start_order, 0, sizeof(s_listen_start_order));
    memset(s_listen_stop_order, 0, sizeof(s_listen_stop_order));
    s_disabled_ack_order = 0U;
    s_pause_request_order = 0U;
    s_resume_request_order = 0U;
    s_pause_request_count = 0U;
    s_resume_request_count = 0U;
    s_capture_ack_policy = HOST_CAPTURE_ACK_SUCCESS;
    memset(s_resume_audio_session_id, 0, sizeof(s_resume_audio_session_id));
    memset(s_audio_session_ids, 0, sizeof(s_audio_session_ids));
    s_listen_start_count = 0U;
    s_listen_start_committed_count = 0U;
    s_listen_stop_count = 0U;
    s_uplink_start_count = 0U;
    s_uplink_finish_count = 0U;
    s_uplink_discard_count = 0U;
    s_close_request_count = 0U;
    s_fail_listen_start = false;
    s_fail_uplink_finish = false;
    s_fail_mic_deinit = false;
    s_read_discontinuity_frame = UINT32_MAX;
    s_read_discontinuity_injected = false;
    s_active_ack_failures_remaining = 0U;
    s_active_ack_attempts = 0U;
}

int main(void) {
    host_check_reject_nonzero_stream_start();
    reset_capture_fakes();
    nuwax_monitoring_mode_t mode = {0};
    nuwax_ws_client_t ws = {.connected = true, .connection_epoch = 1U};
    const nuwax_monitoring_mode_callbacks_t callbacks = {
        .acquire_mic = acquire_mic_for_resume,
        .is_playback_active = playback_active,
        .fade_and_cancel_playback = fade_playback,
    };
    s_mode = &mode;
    s_ws = &ws;
    s_playback_active = true;
    s_playback_checks_remaining = 75U;
    s_read_discontinuity_frame = 2U;
    s_active_ack_failures_remaining = 1U;
    assert(nuwax_monitoring_mode_init(&mode, &callbacks) == ESP_OK);

    static const char open_command[] =
        "{\"type\":\"device_command\",\"command\":\"set_audio_mode\","
        "\"request_id\":\"capture_open\",\"mode\":\"monitoring\","
        "\"state\":\"enabled\"}";
    assert(nuwax_monitoring_mode_handle_protocol_text(
        &mode, &ws, open_command, (int)strlen(open_command)));
    assert(nuwax_monitoring_mode_run(&mode, &ws) == ESP_OK);

    assert(s_fade_calls == 1U);
    assert(s_active_ack_attempts == 2U);
    assert(s_listen_start_count == HOST_EXPECTED_STREAMS);
    assert(s_listen_start_committed_count == HOST_EXPECTED_STREAMS);
    assert(s_listen_stop_count == HOST_EXPECTED_STREAMS);
    assert(s_uplink_start_count == HOST_EXPECTED_STREAMS);
    assert(s_uplink_finish_count == HOST_EXPECTED_STREAMS);
    assert(s_read_discontinuity_injected);
    assert(s_read_index == HOST_FRAME_COUNT);
    assert(s_read_offset == 0U);
    assert(s_enqueued_count == HOST_EXPECTED_UPLOADED_FRAMES);

    const uint32_t expected_source_frames[HOST_EXPECTED_UPLOADED_FRAMES] = {
        0U, 1U, 2U, 3U, 4U, 5U,
        13U, 14U, 15U, 16U, 17U, 18U, 19U,
    };
    for (uint32_t uploaded = 0U;
         uploaded < HOST_EXPECTED_UPLOADED_FRAMES;
         ++uploaded) {
        const uint32_t source_frame = expected_source_frames[uploaded];
        const uint32_t stream = uploaded < 6U ? 0U : 1U;
        const uint32_t sequence = uploaded < 6U ? uploaded : uploaded - 6U;
        assert(s_enqueued_streams[uploaded] == stream);
        assert(s_enqueued_sequences[uploaded] == sequence);
        assert(s_enqueued_sample_counts[uploaded] == HOST_FRAME_SAMPLES);
        assert(((s_enqueued_flags[uploaded] & NUWAX_AUDIO_FLAG_VAD_SPEECH) != 0U) ==
               host_frame_is_speech(source_frame));
        for (size_t i = 0U; i < HOST_FRAME_SAMPLES; ++i) {
            assert(s_enqueued_samples[uploaded][i] == host_sample(source_frame, i));
        }
    }
    assert(s_enqueued_flags[0] == NUWAX_AUDIO_FLAG_START);
    /* Frames 3..5 reach the short candidate threshold but not the stricter
     * activity threshold, so the first stream still pauses. Frames 6..10 model
     * unchanged false-positive audio immediately after the pause: the guard
     * consumes 6..7 and the remaining frames cannot rearm without quiet.
     * Frames 11..12 establish a quiet-to-speech edge; sustained speech at
     * 15..18 is retained by pre-roll and does resume the stream. */
    assert(s_enqueued_flags[5] ==
           (NUWAX_AUDIO_FLAG_VAD_SPEECH | NUWAX_AUDIO_FLAG_END));
    assert(s_enqueued_flags[6] == NUWAX_AUDIO_FLAG_START);
    assert(s_enqueued_flags[12] == NUWAX_AUDIO_FLAG_END);
    assert(sent_message_contains("capture_open", "\"message\":\"active\""));
    assert(sent_message_contains("capture_close", "\"message\":\"disabled\""));
    assert(s_uplink_start_order[0] < s_listen_start_order[0]);
    assert(s_listen_start_order[0] < s_finish_order[0]);
    assert(s_pause_request_order < s_finish_order[0]);
    assert(s_finish_order[0] < s_listen_stop_order[0]);
    assert(s_listen_stop_order[0] < s_resume_request_order);
    assert(s_resume_request_order < s_uplink_start_order[1]);
    assert(s_listen_stop_order[0] < s_listen_start_order[1]);
    assert(s_uplink_start_order[1] < s_listen_start_order[1]);
    assert(s_listen_start_order[1] < s_finish_order[1]);
    assert(s_finish_order[1] < s_listen_stop_order[1]);
    assert(s_listen_stop_order[1] < s_mic_release_order);
    assert(s_mic_release_order < s_disabled_ack_order);
    assert(nuwax_monitoring_mode_capture_phase(&mode) ==
           NUWAX_MONITORING_CAPTURE_INACTIVE);
    assert(!nuwax_monitoring_mode_has_pending(&mode));

    nuwax_monitoring_mode_deinit(&mode);

    reset_capture_fakes();
    s_capture_ack_policy = HOST_CAPTURE_ACK_DROP_PAUSE;
    nuwax_monitoring_mode_t pause_timeout_mode = {0};
    nuwax_ws_client_t pause_timeout_ws = {
        .connected = true,
        .connection_epoch = 5U,
    };
    s_mode = &pause_timeout_mode;
    s_ws = &pause_timeout_ws;
    assert(nuwax_monitoring_mode_init(&pause_timeout_mode, &callbacks) == ESP_OK);
    assert(nuwax_monitoring_mode_handle_protocol_text(
        &pause_timeout_mode,
        &pause_timeout_ws,
        open_command,
        (int)strlen(open_command)));
    assert(nuwax_monitoring_mode_run(&pause_timeout_mode, &pause_timeout_ws) ==
           ESP_OK);
    assert(s_pause_request_count >= 2U);
    assert(s_resume_request_count == 0U);
    assert(s_listen_start_count == 1U);
    assert(s_listen_stop_count == 1U);
    assert(s_enqueued_count == HOST_FRAME_COUNT);
    assert(sent_message_contains("capture_close", "\"message\":\"disabled\""));
    assert(nuwax_monitoring_mode_capture_phase(&pause_timeout_mode) ==
           NUWAX_MONITORING_CAPTURE_INACTIVE);
    nuwax_monitoring_mode_deinit(&pause_timeout_mode);

    reset_capture_fakes();
    s_capture_ack_policy = HOST_CAPTURE_ACK_REJECT_RESUME;
    nuwax_monitoring_mode_t resume_reject_mode = {0};
    nuwax_ws_client_t resume_reject_ws = {
        .connected = true,
        .connection_epoch = 6U,
    };
    s_mode = &resume_reject_mode;
    s_ws = &resume_reject_ws;
    assert(nuwax_monitoring_mode_init(&resume_reject_mode, &callbacks) == ESP_OK);
    assert(nuwax_monitoring_mode_handle_protocol_text(
        &resume_reject_mode,
        &resume_reject_ws,
        open_command,
        (int)strlen(open_command)));
    assert(nuwax_monitoring_mode_run(&resume_reject_mode, &resume_reject_ws) ==
           ESP_OK);
    assert(s_pause_request_count == 1U);
    assert(s_resume_request_count == 1U);
    assert(s_listen_start_count == 1U);
    assert(s_listen_stop_count == 1U);
    assert(s_enqueued_count == 6U);
#if CONFIG_NUWAX_OFFLINE_AUDIO_ENABLE
    /* The cloud rejected resume. Local speech frames must still be saved. */
    assert(s_archived_while_paused > 0U);
    assert((s_archived_mask & (1U << 18U)) != 0U);
#endif
    assert((s_enqueued_flags[s_enqueued_count - 1U] &
            NUWAX_AUDIO_FLAG_END) != 0U);
    assert(nuwax_monitoring_mode_capture_phase(&resume_reject_mode) ==
           NUWAX_MONITORING_CAPTURE_INACTIVE);
    nuwax_monitoring_mode_deinit(&resume_reject_mode);

    reset_capture_fakes();
    s_fail_listen_start = true;
    nuwax_monitoring_mode_t start_failed_mode = {0};
    nuwax_ws_client_t start_failed_ws = {
        .connected = true,
        .connection_epoch = 2U,
    };
    s_mode = &start_failed_mode;
    s_ws = &start_failed_ws;
    assert(nuwax_monitoring_mode_init(&start_failed_mode, &callbacks) == ESP_OK);
    assert(nuwax_monitoring_mode_handle_protocol_text(
        &start_failed_mode,
        &start_failed_ws,
        open_command,
        (int)strlen(open_command)));
    assert(nuwax_monitoring_mode_run(&start_failed_mode, &start_failed_ws) ==
           ESP_ERR_TIMEOUT);
    assert(s_uplink_start_count == 1U);
    assert(s_listen_start_count == 1U);
    assert(s_listen_start_committed_count == 0U);
    assert(s_uplink_discard_count == 1U);
    assert(s_uplink_finish_count == 0U);
    assert(s_listen_stop_count == 0U);
    assert(s_enqueued_count == 0U);
    assert(s_close_request_count == 0U);
    assert(s_uplink_start_order[0] < s_listen_start_order[0]);
    nuwax_monitoring_mode_deinit(&start_failed_mode);

    reset_capture_fakes();
    s_fail_uplink_finish = true;
    nuwax_monitoring_mode_t finish_failed_mode = {0};
    nuwax_ws_client_t finish_failed_ws = {
        .connected = true,
        .connection_epoch = 3U,
    };
    s_mode = &finish_failed_mode;
    s_ws = &finish_failed_ws;
    assert(nuwax_monitoring_mode_init(&finish_failed_mode, &callbacks) == ESP_OK);
    assert(nuwax_monitoring_mode_handle_protocol_text(
        &finish_failed_mode,
        &finish_failed_ws,
        open_command,
        (int)strlen(open_command)));
    assert(nuwax_monitoring_mode_run(&finish_failed_mode, &finish_failed_ws) ==
           ESP_ERR_TIMEOUT);
    assert(s_uplink_start_count == 1U);
    assert(s_listen_start_committed_count == 1U);
    assert(s_uplink_finish_count == 1U);
    assert(s_uplink_discard_count == 0U);
    assert(s_listen_stop_count == 0U);
    assert(s_close_request_count >= 1U);
    assert(s_enqueued_count > 0U);
    assert((s_enqueued_flags[s_enqueued_count - 1U] &
             NUWAX_AUDIO_FLAG_END) != 0U);
    nuwax_monitoring_mode_deinit(&finish_failed_mode);

    reset_capture_fakes();
    s_fail_mic_deinit = true;
    nuwax_monitoring_mode_t mic_deinit_failed_mode = {0};
    nuwax_ws_client_t mic_deinit_failed_ws = {
        .connected = true,
        .connection_epoch = 4U,
    };
    s_mode = &mic_deinit_failed_mode;
    s_ws = &mic_deinit_failed_ws;
    assert(nuwax_monitoring_mode_init(&mic_deinit_failed_mode, &callbacks) ==
           ESP_OK);
    assert(nuwax_monitoring_mode_handle_protocol_text(
        &mic_deinit_failed_mode,
        &mic_deinit_failed_ws,
        open_command,
        (int)strlen(open_command)));
    assert(nuwax_monitoring_mode_run(&mic_deinit_failed_mode,
                                     &mic_deinit_failed_ws) ==
           ESP_ERR_TIMEOUT);
    assert(s_listen_start_committed_count == HOST_EXPECTED_STREAMS);
    assert(s_uplink_finish_count == HOST_EXPECTED_STREAMS);
    assert(s_listen_stop_count == HOST_EXPECTED_STREAMS);
    assert(s_close_request_count == 0U);
    nuwax_monitoring_mode_deinit(&mic_deinit_failed_mode);

#if CONFIG_NUWAX_OFFLINE_AUDIO_ENABLE && \
    defined(NUWAX_HOST_HEAP_CAPS_FAULT_INJECT)
    /* Failure to reserve the optional local voice ring must not prevent the
     * cloud monitoring stream from running, and no invalid archive writer may
     * be called after the failed initialization. */
    reset_capture_fakes();
    nuwax_host_heap_caps_fail_calloc = true;
    nuwax_monitoring_mode_t archive_alloc_failed_mode = {0};
    nuwax_ws_client_t archive_alloc_failed_ws = {
        .connected = true, .connection_epoch = 7U,
    };
    s_mode = &archive_alloc_failed_mode;
    s_ws = &archive_alloc_failed_ws;
    assert(nuwax_monitoring_mode_init(&archive_alloc_failed_mode,
                                      &callbacks) == ESP_OK);
    assert(nuwax_monitoring_mode_handle_protocol_text(
        &archive_alloc_failed_mode,
        &archive_alloc_failed_ws,
        open_command,
        (int)strlen(open_command)));
    assert(nuwax_monitoring_mode_run(&archive_alloc_failed_mode,
                                     &archive_alloc_failed_ws) == ESP_OK);
    assert(s_enqueued_count > 0U);
    assert(s_archived_mask == 0U);
    assert(s_archived_while_paused == 0U);
    nuwax_monitoring_mode_deinit(&archive_alloc_failed_mode);
    nuwax_host_heap_caps_fail_calloc = false;
#endif

    reset_capture_fakes();
    nuwax_monitoring_mode_t resumed = {0};
    nuwax_ws_client_t resumed_ws = {.connected = true, .connection_epoch = 8U};
    s_mode = &resumed;
    s_ws = &resumed_ws;
    assert(nuwax_monitoring_mode_init(&resumed, &callbacks) == ESP_OK);
    strcpy(resumed.identity.session_id, "monitoring_original");
    resumed.identity.resuming = true;
    resumed.identity.next_capture_sequence = 2829U;
    const char resume_command[] =
        "{\"type\":\"device_command\",\"command\":\"set_audio_mode\","
        "\"request_id\":\"capture_open\",\"mode\":\"monitoring\","
        "\"state\":\"enabled\",\"audio_session_id\":\"cloud_new_session\"}";
    assert(nuwax_monitoring_mode_handle_protocol_text(&resumed, &resumed_ws,
        resume_command, (int)strlen(resume_command)));
    assert(nuwax_monitoring_mode_run(&resumed, &resumed_ws) == ESP_OK);
    for (size_t i = 0; i < s_listen_start_count; ++i) {
        assert(strcmp(s_audio_session_ids[i], "monitoring_original") == 0);
    }
    assert(s_enqueued_count > 0U);
    for (size_t i = 0; i < s_enqueued_count; ++i) {
        assert(s_enqueued_sequences[i] == (i < 6U ? i : i - 6U));
    }
    assert(resumed.identity.session_id[0] == '\0');
    nuwax_monitoring_mode_deinit(&resumed);
    /* A MIC initialization returning after the command deadline must not
     * publish ACTIVE, send PCM, or revive the request after cleanup. */
    reset_capture_fakes();
    s_mic_start_delay_us = 23000000LL;
    nuwax_monitoring_mode_t late_mode = {0};
    nuwax_ws_client_t late_ws = {.connected=true, .connection_epoch=900};
    const nuwax_monitoring_mode_callbacks_t late_callbacks = {.acquire_mic=acquire_mic_for_resume};
    s_mode = &late_mode; s_ws = &late_ws;
    assert(nuwax_monitoring_mode_init(&late_mode, &late_callbacks) == ESP_OK);
    const char late_command[] = "{\"type\":\"device_command\",\"command\":\"set_audio_mode\","
        "\"request_id\":\"capture_open\",\"mode\":\"monitoring\",\"state\":\"enabled\"}";
    assert(nuwax_monitoring_mode_handle_protocol_text(&late_mode, &late_ws, late_command, strlen(late_command)));
    assert(nuwax_monitoring_mode_run(&late_mode, &late_ws) == ESP_ERR_TIMEOUT);
    assert(s_read_index == 0 && s_active_ack_attempts == 0);
    assert(late_mode.state == NUWAX_MONITORING_STATE_DISABLED && !late_mode.capture_running);
    assert(late_mode.command_deadline.decided && !late_mode.command_deadline.ok);
    assert(!late_mode.offline_resume_required && !late_mode.identity.resuming);
    nuwax_monitoring_mode_deinit(&late_mode);
    s_mic_start_delay_us = 0;
    puts("PASS: late monitoring MIC readiness cancels without ACTIVE or PCM");
    puts("monitoring_capture_host_check: PASS");
    return 0;
}

/* Bounded transport fake: protocol JSON remains the same. */
esp_err_t nuwax_ws_send_text_until_if_epoch(nuwax_ws_client_t *ws, const char *payload,
    int64_t deadline_us, uint32_t epoch)
{
    if (deadline_us <= 0 || !ws || ws->connection_epoch != epoch) return ESP_ERR_INVALID_STATE;
    #ifdef nuwax_ws_send_text
    #undef nuwax_ws_send_text
    #define SD_RESTORE_SEND_TEXT
    #endif
    extern esp_err_t nuwax_ws_send_text(nuwax_ws_client_t *, const char *);
    esp_err_t result = nuwax_ws_send_text(ws, payload);
    #ifdef SD_RESTORE_SEND_TEXT
    #define nuwax_ws_send_text sd_base_send_text
    #undef SD_RESTORE_SEND_TEXT
    #endif
    return result;
}
