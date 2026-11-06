/* Host-only controller check. This file is intentionally not part of the
 * ESP-IDF component. It compiles the production meeting_mode.c with hardware
 * capture disabled and exercises the real control queues and state machine. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "meeting_mode.h"

#define HOST_SENT_MESSAGE_CAPACITY 32U
#define HOST_SENT_MESSAGE_BYTES 256U

struct nuwax_host_queue {
    uint8_t *storage;
    UBaseType_t length;
    UBaseType_t item_size;
    UBaseType_t head;
    UBaseType_t count;
};

static int64_t s_now_us;
static uint32_t s_random_value = 0x10203040U;
static char s_sent_messages[HOST_SENT_MESSAGE_CAPACITY][HOST_SENT_MESSAGE_BYTES];
static size_t s_sent_count;
static unsigned int s_state_changes;
static bool s_can_start = true;
static nuwax_meeting_mode_t *s_stop_before_publish;
static nuwax_ws_client_t *s_stop_ws;

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size) {
    if (length == 0U || item_size == 0U) {
        return NULL;
    }
    struct nuwax_host_queue *queue = calloc(1U, sizeof(*queue));
    if (!queue) {
        return NULL;
    }
    queue->storage = calloc(length, item_size);
    if (!queue->storage) {
        free(queue);
        return NULL;
    }
    queue->length = length;
    queue->item_size = item_size;
    return queue;
}

BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait_ticks) {
    (void)wait_ticks;
    if (!queue || !item || queue->count >= queue->length) {
        return pdFALSE;
    }
    if (s_stop_before_publish) {
        nuwax_meeting_mode_t *mode = s_stop_before_publish;
        s_stop_before_publish = NULL;
        assert(nuwax_meeting_mode_request_ui(mode, s_stop_ws, false) == ESP_OK);
    }
    const UBaseType_t tail = (queue->head + queue->count) % queue->length;
    memcpy(queue->storage + (size_t)tail * queue->item_size, item, queue->item_size);
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

BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait_ticks) {
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
    client->connected = false;
    return true;
}

esp_err_t nuwax_ws_send_text(nuwax_ws_client_t *client, const char *payload) {
    if (!nuwax_ws_is_connected(client) || !payload) {
        return ESP_ERR_INVALID_STATE;
    }
    assert(s_sent_count < HOST_SENT_MESSAGE_CAPACITY);
    const size_t length = strlen(payload);
    assert(length < HOST_SENT_MESSAGE_BYTES);
    memcpy(s_sent_messages[s_sent_count], payload, length + 1U);
    s_sent_count++;
    return ESP_OK;
}

static void reset_sent_messages(void) {
    memset(s_sent_messages, 0, sizeof(s_sent_messages));
    s_sent_count = 0U;
}

static bool sent_message_contains(const char *request_id,
                                  const char *fragment_a,
                                  const char *fragment_b) {
    for (size_t i = 0; i < s_sent_count; ++i) {
        if ((!request_id || strstr(s_sent_messages[i], request_id)) &&
            (!fragment_a || strstr(s_sent_messages[i], fragment_a)) &&
            (!fragment_b || strstr(s_sent_messages[i], fragment_b))) {
            return true;
        }
    }
    return false;
}

static void on_state_changed(void *ctx,
                             nuwax_meeting_state_t state,
                             const char *reason) {
    (void)ctx;
    (void)state;
    (void)reason;
    s_state_changes++;
}

static bool can_start(void *ctx) {
    (void)ctx;
    return s_can_start;
}

static void send_protocol(nuwax_meeting_mode_t *mode,
                          nuwax_ws_client_t *ws,
                          const char *json);

static void test_exclusive_mode_busy(nuwax_meeting_mode_t *mode,
                                     nuwax_ws_client_t *ws) {
    reset_sent_messages();
    s_can_start = false;
    assert(nuwax_meeting_mode_request_ui(mode, ws, true) ==
           ESP_ERR_INVALID_STATE);
    assert(!nuwax_meeting_mode_has_pending(mode));

    send_protocol(mode,
                  ws,
                  "{\"type\":\"device_command\",\"command\":\"set_audio_mode\","
                  "\"request_id\":\"meeting_while_monitoring\",\"mode\":\"meeting\","
                  "\"state\":\"enabled\"}");
    assert(!nuwax_meeting_mode_has_pending(mode));
    assert(sent_message_contains("meeting_while_monitoring",
                                 "\"ok\":false",
                                 "audio_mode_busy"));
    s_can_start = true;
}

static void assert_transient_state_reset(const nuwax_meeting_mode_t *mode) {
    assert(mode->state == NUWAX_MEETING_STATE_DISABLED);
    assert(!mode->capture_running);
    assert(!mode->cloud_authorized);
    assert(!mode->pending_command_ack);
    assert(!mode->pending_command_enabled);
    assert(!mode->pending_ui_close);
    assert(!mode->interrupt_current_turn);
    assert(mode->start_deadline_ms == 0U);
    assert(mode->pending_command_ack_attempts == 0U);
    assert(mode->pending_command_ack_deadline_ms == 0U);
    assert(mode->inactivity.inactive_ms == 0U);
    assert(mode->inactivity.speech_activity_ms == 0U);
    assert(mode->inactivity.sound_activity_ms == 0U);
    assert(mode->capture_frames == 0U);
    assert(mode->uploaded_frames == 0U);
    assert(mode->dropped_frames == 0U);
    assert(mode->consecutive_read_timeouts == 0U);
    assert(mode->pending_request_id[0] == '\0');
    assert(mode->pending_command_request_id[0] == '\0');
    assert(mode->audio_session_id[0] == '\0');
    assert(mode->stop_reason[0] == '\0');
    assert(mode->processor.gain_q15 == 32768U);
    assert(!mode->processor.noise_floor_initialized);
}

static void send_protocol(nuwax_meeting_mode_t *mode,
                          nuwax_ws_client_t *ws,
                          const char *json) {
    assert(nuwax_meeting_mode_handle_protocol_text(mode,
                                                   ws,
                                                   json,
                                                   (int)strlen(json)));
}

static void test_ui_rejection_and_timeout(nuwax_meeting_mode_t *mode,
                                          nuwax_ws_client_t *ws) {
    reset_sent_messages();
    assert(nuwax_meeting_mode_request_ui(mode, ws, true) == ESP_OK);
    assert(mode->state == NUWAX_MEETING_STATE_STARTING);
    assert(mode->pending_request_id[0] != '\0');
    assert(sent_message_contains(mode->pending_request_id,
                                 "\"type\":\"audio_mode_request\"",
                                 "\"state\":\"enabled\""));

    char ack[256];
    const int ack_len = snprintf(
        ack,
        sizeof(ack),
        "{\"type\":\"audio_mode_request_ack\",\"request_id\":\"%s\","
        "\"ok\":false,\"mode\":\"meeting\",\"state\":\"disabled\","
        "\"reason\":\"policy_rejected\"}",
        mode->pending_request_id);
    assert(ack_len > 0 && ack_len < (int)sizeof(ack));
    send_protocol(mode, ws, ack);
    assert(nuwax_meeting_mode_run(mode, ws) == ESP_OK);
    assert_transient_state_reset(mode);

    s_now_us += 1000000LL;
    assert(nuwax_meeting_mode_request_ui(mode, ws, true) == ESP_OK);
    s_now_us += 6000000LL;
    assert(nuwax_meeting_mode_run(mode, ws) == ESP_ERR_TIMEOUT);
    assert_transient_state_reset(mode);
}

static void test_correct_ack_survives_later_stale_ack(
    nuwax_meeting_mode_t *mode,
    nuwax_ws_client_t *ws) {
    reset_sent_messages();
    assert(nuwax_meeting_mode_request_ui(mode, ws, true) == ESP_OK);
    char expected_request[NUWAX_MEETING_REQUEST_ID_BYTES];
    strcpy(expected_request, mode->pending_request_id);

    char ack[320];
    const int ack_len = snprintf(
        ack,
        sizeof(ack),
        "{\"type\":\"audio_mode_request_ack\",\"request_id\":\"%s\","
        "\"ok\":true,\"mode\":\"meeting\",\"state\":\"enabled\","
        "\"audio_session_id\":\"meeting_ack_fifo\"}",
        expected_request);
    assert(ack_len > 0 && ack_len < (int)sizeof(ack));
    send_protocol(mode, ws, ack);
    send_protocol(mode,
                  ws,
                  "{\"type\":\"audio_mode_request_ack\","
                  "\"request_id\":\"stale_after_correct\",\"ok\":true,"
                  "\"mode\":\"meeting\",\"state\":\"enabled\"}");
    assert(uxQueueMessagesWaiting(mode->request_acks) == 2U);

    assert(nuwax_meeting_mode_run(mode, ws) == ESP_ERR_NOT_SUPPORTED);
    assert_transient_state_reset(mode);
}

static void test_command_queue_overflow(nuwax_meeting_mode_t *mode,
                                        nuwax_ws_client_t *ws) {
    reset_sent_messages();
    for (unsigned int i = 0; i < 8U; ++i) {
        char command[256];
        const int length = snprintf(
            command,
            sizeof(command),
            "{\"type\":\"device_command\",\"command\":\"set_audio_mode\","
            "\"request_id\":\"overflow_%u\",\"mode\":\"meeting\","
            "\"state\":\"disabled\"}",
            i);
        assert(length > 0 && length < (int)sizeof(command));
        send_protocol(mode, ws, command);
    }
    assert(nuwax_meeting_mode_has_pending(mode));
    assert(!nuwax_meeting_mode_blocks_ota(mode));
    send_protocol(mode,
                  ws,
                  "{\"type\":\"device_command\",\"command\":\"set_audio_mode\","
                  "\"request_id\":\"overflow_8\",\"mode\":\"meeting\","
                  "\"state\":\"enabled\"}");
    assert(uxQueueMessagesWaiting(mode->events) == 8U);
    assert(!nuwax_meeting_mode_blocks_ota(mode));
    assert(sent_message_contains("overflow_8",
                                 "\"ok\":false",
                                 "control_queue_full"));
    nuwax_meeting_mode_notify_ws_disconnected(mode);
    assert(!nuwax_meeting_mode_has_pending(mode));
    assert_transient_state_reset(mode);
}

static void test_ota_admission_classifies_queued_events(
    nuwax_meeting_mode_t *mode,
    nuwax_ws_client_t *ws) {
    reset_sent_messages();
    assert(!nuwax_meeting_mode_blocks_ota(mode));
    const nuwax_meeting_event_t request_ack = {
        .type = NUWAX_MEETING_EVENT_REQUEST_ACK,
        .ws_connection_epoch = ws->connection_epoch,
    };
    assert(xQueueSend(mode->request_acks, &request_ack, 0) == pdTRUE);
    assert(nuwax_meeting_mode_has_pending(mode));
    assert(!nuwax_meeting_mode_has_audio_claim(mode));
    assert(!nuwax_meeting_mode_blocks_ota(mode));
    nuwax_meeting_mode_notify_ws_disconnected(mode);

    send_protocol(mode,
                  ws,
                  "{\"type\":\"device_command\",\"command\":\"set_audio_mode\","
                  "\"request_id\":\"ota_disable\",\"mode\":\"meeting\","
                  "\"state\":\"disabled\"}");
    assert(nuwax_meeting_mode_has_pending(mode));
    assert(!nuwax_meeting_mode_has_audio_claim(mode));
    assert(!nuwax_meeting_mode_blocks_ota(mode));
    assert(nuwax_meeting_mode_run(mode, ws) == ESP_OK);
    assert(!nuwax_meeting_mode_blocks_ota(mode));

    send_protocol(mode,
                  ws,
                  "{\"type\":\"device_command\",\"command\":\"set_audio_mode\","
                  "\"request_id\":\"ota_enable\",\"mode\":\"meeting\","
                  "\"state\":\"enabled\"}");
    assert(nuwax_meeting_mode_has_audio_claim(mode));
    assert(nuwax_meeting_mode_blocks_ota(mode));
    nuwax_meeting_mode_notify_ws_disconnected(mode);
    assert(!nuwax_meeting_mode_blocks_ota(mode));

    mode->state = NUWAX_MEETING_STATE_STARTING;
    assert(nuwax_meeting_mode_has_audio_claim(mode));
    assert(nuwax_meeting_mode_blocks_ota(mode));
    mode->state = NUWAX_MEETING_STATE_DISABLED;
}

static void test_enable_superseded_by_disable(nuwax_meeting_mode_t *mode,
                                              nuwax_ws_client_t *ws) {
    reset_sent_messages();
    send_protocol(mode,
                  ws,
                  "{\"type\":\"device_command\",\"command\":\"set_audio_mode\","
                  "\"request_id\":\"cloud_open\",\"mode\":\"meeting\","
                  "\"state\":\"enabled\"}");
    send_protocol(mode,
                  ws,
                  "{\"type\":\"device_command\",\"command\":\"set_audio_mode\","
                  "\"request_id\":\"cloud_close\",\"mode\":\"meeting\","
                  "\"state\":\"disabled\"}");
    assert(nuwax_meeting_mode_run(mode, ws) == ESP_OK);
    assert(sent_message_contains("cloud_open",
                                 "\"ok\":false",
                                 "superseded_by_disable"));
    assert(sent_message_contains("cloud_close",
                                 "\"ok\":true",
                                 "\"state\":\"disabled\""));
    assert_transient_state_reset(mode);
}

static void test_connection_epoch_and_recovery_abort(
    nuwax_meeting_mode_t *mode,
    nuwax_ws_client_t *ws) {
    reset_sent_messages();
    ws->connection_epoch = 1U;
    send_protocol(mode,
                  ws,
                  "{\"type\":\"device_command\",\"command\":\"set_audio_mode\"," 
                  "\"request_id\":\"meeting_stale_epoch\",\"mode\":\"meeting\"," 
                  "\"state\":\"disabled\"}");
    ws->connection_epoch = 2U;
    assert(nuwax_meeting_mode_run(mode, ws) == ESP_OK);
    assert(!nuwax_meeting_mode_has_pending(mode));
    assert(!sent_message_contains("meeting_stale_epoch", NULL, NULL));

    reset_sent_messages();
    send_protocol(mode,
                  ws,
                  "{\"type\":\"device_command\",\"command\":\"set_audio_mode\"," 
                  "\"request_id\":\"meeting_recovery_abort\",\"mode\":\"meeting\"," 
                  "\"state\":\"disabled\"}");
    assert(nuwax_meeting_mode_abort_pending_controls(
               mode, ws, 2U, "device_recovery_timeout") == ESP_OK);
    assert(!nuwax_meeting_mode_has_pending(mode));
    assert(sent_message_contains("meeting_recovery_abort",
                                 "\"ok\":false",
                                 "device_recovery_timeout"));

    mode->state = NUWAX_MEETING_STATE_STARTING;
    assert(nuwax_meeting_mode_abort_pending_controls(
               mode, ws, 2U, "device_recovery_timeout") ==
           ESP_ERR_INVALID_STATE);
    mode->state = NUWAX_MEETING_STATE_DISABLED;
}

static void test_handoff_stop(nuwax_meeting_mode_t *mode,
                              nuwax_ws_client_t *ws) {
    reset_sent_messages();
    assert(nuwax_meeting_mode_request_handoff_stop(
               mode, ws, "superseded_by_monitoring") == ESP_OK);
    assert(uxQueueMessagesWaiting(mode->events) == 0U);
    assert_transient_state_reset(mode);

    mode->state = NUWAX_MEETING_STATE_STARTING;
    mode->cloud_authorized = true;
    mode->pending_command_ack = true;
    mode->pending_command_enabled = true;
    strcpy(mode->pending_command_request_id, "meeting_handoff_open");
    strcpy(mode->pending_request_id, "meeting_handoff_open");

    assert(nuwax_meeting_mode_request_handoff_stop(
               mode, ws, "superseded_by_monitoring") == ESP_OK);
    assert(uxQueueMessagesWaiting(mode->events) == 1U);
    assert(nuwax_meeting_mode_run(mode, ws) == ESP_OK);
    assert(sent_message_contains("meeting_handoff_open",
                                 "\"ok\":false",
                                 "superseded_by_disable"));
    assert_transient_state_reset(mode);
}

static void test_cloud_start_failure_and_disconnect(nuwax_meeting_mode_t *mode,
                                                    nuwax_ws_client_t *ws) {
    reset_sent_messages();
    send_protocol(mode,
                  ws,
                  "{\"type\":\"device_command\",\"command\":\"set_audio_mode\","
                  "\"request_id\":\"unsupported_start\",\"mode\":\"meeting\","
                  "\"state\":\"enabled\"}");
    assert(nuwax_meeting_mode_run(mode, ws) == ESP_ERR_NOT_SUPPORTED);
    assert(sent_message_contains("unsupported_start",
                                 "\"ok\":false",
                                 "capture_failed"));
    assert_transient_state_reset(mode);
    assert(!nuwax_meeting_mode_blocks_ota(mode));

    mode->capture_running = true;
    mode->cloud_authorized = true;
    mode->pending_command_ack = true;
    mode->pending_command_enabled = true;
    mode->pending_ui_close = true;
    mode->interrupt_current_turn = true;
    mode->start_deadline_ms = 123U;
    mode->inactivity.inactive_ms = 456U;
    mode->inactivity.speech_activity_ms = 457U;
    mode->inactivity.sound_activity_ms = 458U;
    mode->capture_frames = 7U;
    mode->uploaded_frames = 8U;
    mode->dropped_frames = 9U;
    mode->consecutive_read_timeouts = 10U;
    strcpy(mode->pending_request_id, "stale_request");
    strcpy(mode->pending_command_request_id, "stale_command");
    strcpy(mode->audio_session_id, "stale_audio");
    strcpy(mode->stop_reason, "stale_stop");
    mode->processor.noise_floor_initialized = true;
    mode->processor.noise_floor_abs = 999U;
    mode->processor.gain_q15 = 39000U;
    mode->state = NUWAX_MEETING_STATE_ACTIVE;
    nuwax_meeting_mode_notify_ws_disconnected(mode);
    assert(!nuwax_meeting_mode_has_pending(mode));
    assert_transient_state_reset(mode);
}

static void test_resume_identity(nuwax_meeting_mode_t *mode, nuwax_ws_client_t *ws) {
    nuwax_audio_mode_identity_clear(&mode->identity);
    strcpy(mode->audio_session_id, "meeting_original");
    mode->state = NUWAX_MEETING_STATE_ACTIVE;
    nuwax_meeting_mode_notify_ws_disconnected(mode);
    assert(mode->identity.resuming);
    assert(strcmp(mode->identity.session_id, "meeting_original") == 0);
    reset_sent_messages();
    assert(nuwax_meeting_mode_request_ui(mode, ws, true) == ESP_OK);
    char first[48];
    strcpy(first, mode->pending_request_id);
    assert(sent_message_contains(first, "\"audio_session_id\":\"meeting_original\"", "offline_audio_recovered"));
    s_now_us += 6000000LL;
    assert(nuwax_meeting_mode_run(mode, ws) == ESP_ERR_TIMEOUT);
    assert(mode->state == NUWAX_MEETING_STATE_DISABLED);
    assert(mode->identity.resuming && strcmp(mode->identity.session_id, "meeting_original") == 0);
    assert(nuwax_meeting_mode_request_ui(mode, ws, true) == ESP_OK);
    assert(strcmp(first, mode->pending_request_id) != 0);
    nuwax_meeting_mode_notify_ws_disconnected(mode);
    assert(nuwax_meeting_mode_request_ui(mode, ws, true) == ESP_OK);
    assert(strcmp(first, mode->pending_request_id) != 0);
    assert(sent_message_contains(mode->pending_request_id, "meeting_original", "offline_audio_recovered"));
    nuwax_meeting_mode_notify_ws_disconnected(mode);
    assert(nuwax_meeting_mode_request_ui(mode, ws, false) == ESP_OK);
    assert(mode->identity.session_id[0] == '\0');
    reset_sent_messages();
    assert(nuwax_meeting_mode_request_ui(mode, ws, true) == ESP_OK);
    assert(!sent_message_contains(NULL, "meeting_original", NULL));
    nuwax_meeting_mode_notify_ws_disconnected(mode);
}


static void test_local_stop_fences_received_enable(nuwax_meeting_mode_t *mode,
                                                  nuwax_ws_client_t *ws) {
    const char *enable_format = "{\"type\":\"device_command\",\"command\":\"set_audio_mode\","
        "\"request_id\":\"%s\",\"mode\":\"meeting\",\"state\":\"enabled\"}";
    char request[48], enable[400];
    for (unsigned race = 0; race < 3; ++race) {
        snprintf(request, sizeof(request), "stop_race_%u", race);
        snprintf(enable, sizeof(enable), enable_format, request);

        reset_sent_messages();
        assert(mode->state == NUWAX_MEETING_STATE_DISABLED);
        mode->offline_resume_required = true;
        mode->identity.resuming = true;
        strcpy(mode->identity.session_id, "offline_original");
        if (race == 1) { // RX paused after receiving, before queue publication.
            s_stop_before_publish = mode;
            s_stop_ws = ws;
        }
        send_protocol(mode, ws, enable);
        assert(atomic_load(&mode->queued_enable_events) == 1U);
        if (race != 1) {
            if (race == 2) ws->connected = false;
            assert(nuwax_meeting_mode_request_ui(mode, ws, false) == ESP_OK);
            ws->connected = true;
        }
        assert(!mode->offline_resume_required && !mode->identity.resuming);
        const unsigned changes = s_state_changes;
        assert(nuwax_meeting_mode_run(mode, ws) == ESP_OK);
        assert(s_state_changes == changes); // Never enters STARTING/capture.
        assert(sent_message_contains(request, "\"ok\":false", "local_stop_superseded"));
        assert(atomic_load(&mode->queued_enable_events) == 0U);
        assert(!nuwax_meeting_mode_has_pending(mode));
        assert(!nuwax_meeting_mode_blocks_ota(mode));
        // A genuinely new enable remains admissible after the barrier.
        reset_sent_messages();
        send_protocol(mode, ws, enable);
        assert(nuwax_meeting_mode_run(mode, ws) == ESP_ERR_NOT_SUPPORTED);
        assert(sent_message_contains(request, "\"ok\":false", "capture_failed"));
        assert(atomic_load(&mode->queued_enable_events) == 0U);
    }
    for(unsigned active=0;active<2;active++) {
        reset_sent_messages();
        mode->state = active ? NUWAX_MEETING_STATE_ACTIVE : NUWAX_MEETING_STATE_STARTING;
        send_protocol(mode, ws, enable);
        assert(nuwax_meeting_mode_request_ui(mode, ws, false) == ESP_OK);
        assert(mode->state == NUWAX_MEETING_STATE_STOPPING);
        assert(nuwax_meeting_mode_run(mode, ws) == ESP_OK);
        assert(mode->state == NUWAX_MEETING_STATE_DISABLED);
        assert(sent_message_contains(request, "\"ok\":false", "local_stop_superseded"));
        assert(atomic_load(&mode->queued_enable_events) == 0U);
    }
}

int main(void) {
    nuwax_meeting_mode_t mode = {0};
    nuwax_ws_client_t ws = {.connected = true, .connection_epoch = 1U};
    const nuwax_meeting_mode_callbacks_t callbacks = {
        .can_start = can_start,
        .on_state_changed = on_state_changed,
    };
    assert(nuwax_meeting_mode_init(&mode, &callbacks) == ESP_OK);

    test_exclusive_mode_busy(&mode, &ws);
    test_ui_rejection_and_timeout(&mode, &ws);
    test_correct_ack_survives_later_stale_ack(&mode, &ws);
    test_ota_admission_classifies_queued_events(&mode, &ws);
    test_command_queue_overflow(&mode, &ws);
    test_enable_superseded_by_disable(&mode, &ws);
    test_connection_epoch_and_recovery_abort(&mode, &ws);
    test_handoff_stop(&mode, &ws);
    test_cloud_start_failure_and_disconnect(&mode, &ws);
    test_resume_identity(&mode, &ws);
    test_local_stop_fences_received_enable(&mode, &ws);

    assert(s_state_changes > 0U);
    nuwax_meeting_mode_deinit(&mode);
    puts("meeting_mode_host_check: PASS");
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
