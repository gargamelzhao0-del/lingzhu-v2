/* Host-only transaction check for the production meeting capture path. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "audio_frame.h"
#include "audio_uplink.h"
#include "meeting_mode.h"
#include "mic_input.h"
#include "session_protocol.h"

struct nuwax_host_queue {
    uint8_t *storage;
    UBaseType_t length;
    UBaseType_t item_size;
    UBaseType_t head;
    UBaseType_t count;
};

static int64_t s_now_us;
static uint32_t s_random_value = 0x40506070U;
static uint32_t s_order;
static uint32_t s_uplink_start_order;
static uint32_t s_listen_start_order;
static uint32_t s_finish_order;
static uint32_t s_listen_stop_order;
static size_t s_listen_start_attempts;
static size_t s_listen_start_commits;
static size_t s_listen_stop_count;
static size_t s_uplink_finish_count;
static size_t s_uplink_discard_count;
static size_t s_close_request_count;
static size_t s_enqueue_count;
static uint8_t s_last_enqueue_flags;
static bool s_fail_listen_start;
static bool s_fail_finish;
static bool s_fail_mic_deinit;
static bool s_disable_queued;
static size_t s_successful_reads;
static size_t s_disable_after_successful_reads;
static size_t s_read_calls;
static size_t s_discontinuity_call;
static bool s_discontinuity_injected;
static size_t s_active_ack_failures_remaining;
static size_t s_active_ack_attempts;
static nuwax_meeting_mode_t *s_mode;
static nuwax_ws_client_t *s_ws;

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size) {
    struct nuwax_host_queue *queue = calloc(1U, sizeof(*queue));
    assert(queue != NULL && length > 0U && item_size > 0U);
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
    if (queue) {
        free(queue->storage);
        free(queue);
    }
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
    assert(nuwax_ws_connection_epoch(ws) == expected_ws_epoch);
    assert(deadline_us > s_now_us);
    assert(strcmp(mode, "meeting") == 0);
    assert(audio_session_id && audio_session_id[0] != '\0');
    if (strcmp(state, "start") == 0) {
        assert(request_id && strcmp(request_id, "meeting_capture_open") == 0);
        s_listen_start_attempts++;
        s_listen_start_order = ++s_order;
        if (s_fail_listen_start) {
            return ESP_ERR_TIMEOUT;
        }
        s_listen_start_commits++;
        return ESP_OK;
    }
    assert(strcmp(state, "stop") == 0);
    assert(request_id == NULL);
    s_listen_stop_count++;
    s_listen_stop_order = ++s_order;
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

esp_err_t nuwax_mic_input_init_with_owner(nuwax_mic_input_t *mic,
                                          nuwax_mic_owner_t owner) {
    assert(mic != NULL && owner == NUWAX_MIC_OWNER_MEETING_CAPTURE);
    mic->initialized = true;
    mic->owner = owner;
    return ESP_OK;
}

esp_err_t nuwax_mic_input_read_pcm16(nuwax_mic_input_t *mic,
                                     int16_t *samples,
                                     size_t sample_count,
                                     uint32_t timeout_ms,
                                     size_t *samples_read) {
    assert(mic && mic->initialized && samples && samples_read);
    assert(timeout_ms == CONFIG_NUWAX_MIC_READ_TIMEOUT_MS);
    s_read_calls++;
    if (!s_discontinuity_injected && s_read_calls == s_discontinuity_call) {
        s_discontinuity_injected = true;
        *samples_read = 0U;
        s_now_us +=
            (int64_t)CONFIG_NUWAX_VOICE_FRAME_DURATION_MS * 1000LL;
        return ESP_ERR_INVALID_RESPONSE;
    }
    for (size_t i = 0U; i < sample_count; ++i) {
        samples[i] = (i & 1U) ? -1800 : 1800;
    }
    *samples_read = sample_count;
    s_successful_reads++;
    s_now_us += (int64_t)CONFIG_NUWAX_VOICE_FRAME_DURATION_MS * 1000LL;
    if (!s_disable_queued &&
        s_successful_reads >= s_disable_after_successful_reads) {
        static const char disable_command[] =
            "{\"type\":\"device_command\",\"command\":\"set_audio_mode\"," 
            "\"request_id\":\"meeting_capture_close\",\"mode\":\"meeting\"," 
            "\"state\":\"disabled\"}";
        assert(nuwax_meeting_mode_handle_protocol_text(
            s_mode, s_ws, disable_command, (int)strlen(disable_command)));
        s_disable_queued = true;
    }
    return ESP_OK;
}

esp_err_t nuwax_mic_input_deinit(nuwax_mic_input_t *mic) {
    assert(mic != NULL);
    memset(mic, 0, sizeof(*mic));
    return s_fail_mic_deinit ? ESP_ERR_TIMEOUT : ESP_OK;
}

esp_err_t nuwax_audio_uplink_start_if_epoch(nuwax_audio_uplink_t *ctx,
                                            nuwax_ws_client_t *ws,
                                            uint32_t expected_ws_epoch) {
    assert(ctx && ws && nuwax_ws_is_connected(ws));
    assert(nuwax_ws_connection_epoch(ws) == expected_ws_epoch);
    ctx->ws = ws;
    ctx->ws_connection_epoch = expected_ws_epoch;
    s_uplink_start_order = ++s_order;
    return ESP_OK;
}

esp_err_t nuwax_audio_uplink_bind_terminal_recovery(
    nuwax_audio_uplink_t *ctx,
    const char *mode,
    const char *audio_session_id,
    const char *request_id,
    uint32_t attempt_timeout_ms) {
    assert(ctx && strcmp(mode, "meeting") == 0);
    assert(audio_session_id && audio_session_id[0] != '\0');
    assert(request_id == NULL);
    assert(attempt_timeout_ms > 0U);
    return ESP_OK;
}

bool nuwax_audio_uplink_end_committed(const nuwax_audio_uplink_t *ctx) {
    assert(ctx != NULL);
    return !s_fail_finish;
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
    assert(strcmp(mode, "meeting") == 0);
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
                                           nuwax_opus_encoder_t *encoder,
                                           const int16_t *samples,
                                           size_t sample_count,
                                           uint32_t sequence,
                                           uint8_t flags,
                                           bool final_frame) {
    (void)encoder;
    assert(ctx && ctx->ws == s_ws && samples && sample_count > 0U);
    assert(sequence == s_enqueue_count);
    assert(final_frame == ((flags & NUWAX_AUDIO_FLAG_END) != 0U));
    s_enqueue_count++;
    s_last_enqueue_flags = flags;
    return ESP_OK;
}

esp_err_t nuwax_audio_uplink_finish(nuwax_audio_uplink_t *ctx) {
    assert(ctx != NULL);
    s_uplink_finish_count++;
    s_finish_order = ++s_order;
    return s_fail_finish ? ESP_ERR_TIMEOUT : ESP_OK;
}

esp_err_t nuwax_audio_uplink_discard_uncommitted(nuwax_audio_uplink_t *ctx) {
    assert(ctx && ctx->ws == s_ws);
    assert(s_enqueue_count == 0U);
    s_uplink_discard_count++;
    memset(ctx, 0, sizeof(*ctx));
    return ESP_OK;
}

void nuwax_audio_uplink_get_stats(const nuwax_audio_uplink_t *ctx,
                                  nuwax_audio_uplink_stats_t *stats) {
    assert(ctx && stats);
    memset(stats, 0, sizeof(*stats));
    stats->queued_frames = (uint32_t)s_enqueue_count;
    stats->sent_frames = (uint32_t)s_enqueue_count;
}

void nuwax_audio_uplink_destroy(nuwax_audio_uplink_t *ctx) {
    assert(ctx != NULL);
}

static void reset_fakes(void) {
    s_now_us = 0;
    s_order = 0U;
    s_uplink_start_order = 0U;
    s_listen_start_order = 0U;
    s_finish_order = 0U;
    s_listen_stop_order = 0U;
    s_listen_start_attempts = 0U;
    s_listen_start_commits = 0U;
    s_listen_stop_count = 0U;
    s_uplink_finish_count = 0U;
    s_uplink_discard_count = 0U;
    s_close_request_count = 0U;
    s_enqueue_count = 0U;
    s_last_enqueue_flags = 0U;
    s_fail_listen_start = false;
    s_fail_finish = false;
    s_fail_mic_deinit = false;
    s_disable_queued = false;
    s_successful_reads = 0U;
    s_disable_after_successful_reads = 1U;
    s_read_calls = 0U;
    s_discontinuity_call = SIZE_MAX;
    s_discontinuity_injected = false;
    s_active_ack_failures_remaining = 0U;
    s_active_ack_attempts = 0U;
}

static esp_err_t run_capture(bool fail_start,
                             bool fail_finish,
                             bool fail_mic_deinit,
                             bool inject_discontinuity,
                             size_t active_ack_failures) {
    reset_fakes();
    s_fail_listen_start = fail_start;
    s_fail_finish = fail_finish;
    s_fail_mic_deinit = fail_mic_deinit;
    s_discontinuity_call = inject_discontinuity ? 2U : SIZE_MAX;
    s_disable_after_successful_reads = active_ack_failures > 0U
                                           ? 30U
                                           : (inject_discontinuity ? 2U : 1U);
    s_active_ack_failures_remaining = active_ack_failures;
    nuwax_meeting_mode_t mode = {0};
    nuwax_ws_client_t ws = {
        .connected = true,
        .connection_epoch = fail_start ? 11U
                            : fail_finish ? 12U
                            : fail_mic_deinit ? 13U
                                              : 10U,
    };
    s_mode = &mode;
    s_ws = &ws;
    assert(nuwax_meeting_mode_init(&mode, NULL) == ESP_OK);
    static const char enable_command[] =
        "{\"type\":\"device_command\",\"command\":\"set_audio_mode\"," 
        "\"request_id\":\"meeting_capture_open\",\"mode\":\"meeting\"," 
        "\"state\":\"enabled\"}";
    assert(nuwax_meeting_mode_handle_protocol_text(
        &mode, &ws, enable_command, (int)strlen(enable_command)));
    const esp_err_t result = nuwax_meeting_mode_run(&mode, &ws);
    nuwax_meeting_mode_deinit(&mode);
    return result;
}

int main(void) {
    assert(run_capture(false, false, false, false, 0U) == ESP_OK);
    assert(s_uplink_start_order < s_listen_start_order);
    assert(s_listen_start_order < s_finish_order);
    assert(s_finish_order < s_listen_stop_order);
    assert(s_listen_start_commits == 1U);
    assert(s_uplink_finish_count == 1U);
    assert(s_listen_stop_count == 1U);
    assert(s_enqueue_count == 1U);
    assert((s_last_enqueue_flags & NUWAX_AUDIO_FLAG_END) != 0U);
    assert(s_close_request_count == 0U);

    assert(run_capture(true, false, false, false, 0U) == ESP_ERR_TIMEOUT);
    assert(s_uplink_start_order < s_listen_start_order);
    assert(s_listen_start_attempts == 1U);
    assert(s_listen_start_commits == 0U);
    assert(s_uplink_discard_count == 1U);
    assert(s_uplink_finish_count == 0U);
    assert(s_enqueue_count == 0U);
    assert(s_listen_stop_count == 0U);
    assert(s_close_request_count == 0U);

    assert(run_capture(false, true, false, false, 0U) == ESP_ERR_TIMEOUT);
    assert(s_listen_start_commits == 1U);
    assert(s_uplink_finish_count == 1U);
    assert(s_enqueue_count == 1U);
    assert((s_last_enqueue_flags & NUWAX_AUDIO_FLAG_END) != 0U);
    assert(s_listen_stop_count == 0U);
    assert(s_close_request_count >= 1U);

    assert(run_capture(false, false, true, false, 0U) == ESP_ERR_TIMEOUT);
    assert(s_listen_start_commits == 1U);
    assert(s_uplink_finish_count == 1U);
    assert(s_listen_stop_count == 1U);
    assert(s_close_request_count == 0U);

    assert(run_capture(false, false, false, true, 0U) == ESP_OK);
    assert(s_discontinuity_injected);
    assert(s_successful_reads == 2U);
    assert(s_enqueue_count == 2U);
    assert((s_last_enqueue_flags & NUWAX_AUDIO_FLAG_END) != 0U);
    assert(s_close_request_count == 0U);

    assert(run_capture(false, false, false, false, 1U) == ESP_OK);
    assert(s_active_ack_attempts == 2U);
    assert(s_successful_reads == 30U);
    assert(s_enqueue_count == 30U);
    assert(s_close_request_count == 0U);

    puts("meeting_capture_transaction_host_check: PASS");
    return 0;
}
