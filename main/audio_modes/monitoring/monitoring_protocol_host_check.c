/* Host-only protocol check. This file is intentionally not part of the
 * ESP-IDF component. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "monitoring_protocol.h"

static char s_last_text[512];
int64_t esp_timer_get_time(void) { return 1000000; }
uint32_t nuwax_ws_connection_epoch(const nuwax_ws_client_t *ws) { return ws ? ws->connection_epoch : 0; }
static esp_err_t s_send_result = ESP_OK;

esp_err_t nuwax_ws_send_text(nuwax_ws_client_t *ws, const char *payload) {
    assert(ws != NULL);
    assert(payload != NULL);
    const size_t length = strlen(payload);
    assert(length < sizeof(s_last_text));
    memcpy(s_last_text, payload, length + 1U);
    return s_send_result;
}

static void assert_parse(const char *json,
                         const char *expected_request_id,
                         bool expected_enabled) {
    nuwax_monitoring_protocol_event_t event = {0};
    assert(nuwax_monitoring_protocol_parse(json, (int)strlen(json), &event));
    assert(event.type == NUWAX_MONITORING_PROTOCOL_EVENT_SET_COMMAND);
    assert(strcmp(event.request_id, expected_request_id) == 0);
    assert(strcmp(event.mode, "monitoring") == 0);
    assert(strcmp(event.state, expected_enabled ? "enabled" : "disabled") == 0);
    assert(event.enabled == expected_enabled);
}

static void assert_rejected(const char *json) {
    nuwax_monitoring_protocol_event_t event;
    memset(&event, 0x5a, sizeof(event));
    assert(!nuwax_monitoring_protocol_parse(json, (int)strlen(json), &event));
    assert(event.type == NUWAX_MONITORING_PROTOCOL_EVENT_NONE);
    assert(event.request_id[0] == '\0');
    assert(event.mode[0] == '\0');
    assert(event.state[0] == '\0');
    assert(event.audio_session_id[0] == '\0');
    assert(event.reason[0] == '\0');
    assert(!event.enabled);
    assert(!event.ok);
}

static const cJSON *required_item(const cJSON *root, const char *name) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    assert(item != NULL);
    return item;
}

static void assert_ack(const char *request_id,
                       bool enabled,
                       bool ok,
                       const char *message) {
    nuwax_ws_client_t ws = {0};
    memset(s_last_text, 0, sizeof(s_last_text));
    s_send_result = ESP_OK;
    assert(nuwax_monitoring_protocol_send_command_ack(&ws,
                                                      request_id,
                                                      enabled,
                                                      ok,
                                                      message) == ESP_OK);

    cJSON *root = cJSON_Parse(s_last_text);
    assert(root != NULL && cJSON_IsObject(root));
    assert(strcmp(required_item(root, "type")->valuestring,
                  "device_command_ack") == 0);
    assert(strcmp(required_item(root, "request_id")->valuestring, request_id) == 0);
    assert(strcmp(required_item(root, "command")->valuestring,
                  "set_audio_mode") == 0);
    assert(cJSON_IsBool(required_item(root, "ok")));
    assert(cJSON_IsTrue(required_item(root, "ok")) == ok);
    assert(strcmp(required_item(root, "mode")->valuestring, "monitoring") == 0);
    assert(strcmp(required_item(root, "state")->valuestring,
                  enabled ? "enabled" : "disabled") == 0);
    assert(strcmp(required_item(root, "message")->valuestring,
                  message ? message : "") == 0);
    assert(cJSON_GetObjectItemCaseSensitive(root, "audio_session_id") == NULL);
    assert(cJSON_GetObjectItemCaseSensitive(root, "revision") == NULL);
    cJSON_Delete(root);
}

static void assert_capture_control_ack_parse(const char *state, bool ok) {
    char json[320];
    const int written = snprintf(
        json,
        sizeof(json),
        "{\"type\":\"monitoring_capture_control_ack\"," 
        "\"request_id\":\"mon_%s_1\",\"mode\":\"monitoring\"," 
        "\"state\":\"%s\",\"audio_session_id\":\"monitoring_stream_1\"," 
        "\"ok\":%s,\"reason\":\"%s\"}",
        state,
        state,
        ok ? "true" : "false",
        ok ? "acknowledged" : "cloud_rejected");
    assert(written > 0 && written < (int)sizeof(json));
    nuwax_monitoring_protocol_event_t event = {0};
    assert(nuwax_monitoring_protocol_parse(json, written, &event));
    assert(event.type ==
           NUWAX_MONITORING_PROTOCOL_EVENT_CAPTURE_CONTROL_ACK);
    assert(strcmp(event.state, state) == 0);
    assert(strcmp(event.audio_session_id, "monitoring_stream_1") == 0);
    assert(event.ok == ok);
}

static void assert_capture_control_send(bool pause) {
    nuwax_ws_client_t ws = {0};
    memset(s_last_text, 0, sizeof(s_last_text));
    s_send_result = ESP_OK;
    assert(nuwax_monitoring_protocol_send_capture_control(
               &ws,
               pause ? "mon_pause_1" : "mon_resume_1",
               pause,
               pause ? "monitoring_stream_1" : "monitoring_stream_2",
               pause ? "no_speech" : "speech_detected",
               pause ? 120000U : 0U) == ESP_OK);
    cJSON *root = cJSON_Parse(s_last_text);
    assert(root != NULL && cJSON_IsObject(root));
    assert(strcmp(required_item(root, "type")->valuestring,
                  "monitoring_capture_control") == 0);
    assert(strcmp(required_item(root, "mode")->valuestring,
                  "monitoring") == 0);
    assert(strcmp(required_item(root, "state")->valuestring,
                  pause ? "pause" : "resume") == 0);
    if (pause) {
        assert(cJSON_IsNumber(required_item(root, "silence_ms")));
        assert(required_item(root, "silence_ms")->valuedouble == 120000.0);
    } else {
        assert(cJSON_GetObjectItemCaseSensitive(root, "silence_ms") == NULL);
    }
    cJSON_Delete(root);
}

int main(void) {
    assert_parse("{\"type\":\"device_command\",\"command\":\"set_audio_mode\","
                 "\"request_id\":\"cloud_monitoring_open_001\","
                 "\"mode\":\"monitoring\",\"state\":\"enabled\"}",
                 "cloud_monitoring_open_001",
                 true);
    assert_parse(" \r\n{\"state\":\"disabled\",\"mode\":\"monitoring\"," 
                 "\"request_id\":\"close.2-A\",\"command\":\"set_audio_mode\","
                 "\"type\":\"device_command\",\"future_field\":1}\t",
                 "close.2-A",
                 false);
    assert_capture_control_ack_parse("pause", true);
    assert_capture_control_ack_parse("resume", false);

    assert_rejected("{}");
    assert_rejected("[]");
    assert_rejected("{\"type\":\"audio_mode_request_ack\","
                    "\"command\":\"set_audio_mode\",\"request_id\":\"x\","
                    "\"mode\":\"monitoring\",\"state\":\"enabled\"}");
    assert_rejected("{\"type\":\"device_command\",\"command\":\"other\","
                    "\"request_id\":\"x\",\"mode\":\"monitoring\","
                    "\"state\":\"enabled\"}");
    assert_rejected("{\"type\":\"device_command\"," 
                    "\"command\":\"set_audio_mode\",\"request_id\":\"x\","
                    "\"mode\":\"monitor\",\"state\":\"enabled\"}");
    assert_rejected("{\"type\":\"device_command\","
                    "\"command\":\"set_audio_mode\",\"request_id\":\"x\","
                    "\"mode\":\"monitoring\",\"state\":\"start\"}");
    assert_rejected("{\"type\":\"device_command\","
                    "\"command\":\"set_audio_mode\",\"mode\":\"monitoring\","
                    "\"state\":\"enabled\"}");
    assert_rejected("{\"type\":\"device_command\","
                    "\"command\":\"set_audio_mode\",\"request_id\":\"\","
                    "\"mode\":\"monitoring\",\"state\":\"enabled\"}");
    assert_rejected("{\"type\":\"device_command\","
                    "\"command\":\"set_audio_mode\",\"request_id\":\"bad/id\","
                    "\"mode\":\"monitoring\",\"state\":\"enabled\"}");
    assert_rejected("{\"type\":\"device_command\","
                    "\"command\":\"set_audio_mode\",\"request_id\":5,"
                    "\"mode\":\"monitoring\",\"state\":\"enabled\"}");
    assert_rejected("{\"type\":\"device_command\",\"type\":\"device_command\","
                    "\"command\":\"set_audio_mode\",\"request_id\":\"x\","
                    "\"mode\":\"monitoring\",\"state\":\"enabled\"}");
    assert_rejected("{\"type\":\"device_command\","
                    "\"command\":\"set_audio_mode\",\"request_id\":\"x\","
                    "\"mode\":\"monitoring\",\"state\":\"enabled\"}{}");
    assert_rejected("{\"type\":\"monitoring_capture_control_ack\"," 
                    "\"request_id\":\"mon_pause_1\",\"mode\":\"monitoring\"," 
                    "\"state\":\"pause\",\"audio_session_id\":\"stream_1\"}");
    assert_rejected("{\"type\":\"monitoring_capture_control_ack\"," 
                    "\"request_id\":\"mon_pause_1\",\"mode\":\"monitoring\"," 
                    "\"state\":\"paused\",\"audio_session_id\":\"stream_1\"," 
                    "\"ok\":true}");

    char overlong_json[256];
    char overlong_id[NUWAX_MONITORING_REQUEST_ID_BYTES + 1U];
    memset(overlong_id, 'x', sizeof(overlong_id) - 1U);
    overlong_id[sizeof(overlong_id) - 1U] = '\0';
    const int written = snprintf(overlong_json,
                                 sizeof(overlong_json),
                                 "{\"type\":\"device_command\","
                                 "\"command\":\"set_audio_mode\","
                                 "\"request_id\":\"%s\",\"mode\":\"monitoring\","
                                 "\"state\":\"enabled\"}",
                                 overlong_id);
    assert(written > 0 && written < (int)sizeof(overlong_json));
    assert_rejected(overlong_json);

    nuwax_monitoring_protocol_event_t event = {0};
    assert(!nuwax_monitoring_protocol_parse(NULL, 1, &event));
    assert(!nuwax_monitoring_protocol_parse("{}", 0, &event));
    assert(!nuwax_monitoring_protocol_parse("{}", 2, NULL));

    assert_ack("cloud_monitoring_open_001", true, true, "active");
    assert_ack("cloud_monitoring_close_001", false, true, "disabled");
    assert_ack("cloud_monitoring_busy_001", false, false, "audio_mode_busy");
    assert_ack("empty_message", false, false, NULL);
    assert_capture_control_send(true);
    assert_capture_control_send(false);

    nuwax_ws_client_t ws = {0};
    assert(nuwax_monitoring_protocol_send_command_ack(NULL,
                                                      "request_1",
                                                      true,
                                                      true,
                                                      "active") ==
           ESP_ERR_INVALID_ARG);
    assert(nuwax_monitoring_protocol_send_command_ack(&ws,
                                                      "bad request",
                                                      true,
                                                      true,
                                                      "active") ==
           ESP_ERR_INVALID_ARG);
    s_send_result = ESP_ERR_TIMEOUT;
    assert(nuwax_monitoring_protocol_send_command_ack(&ws,
                                                      "request_2",
                                                      true,
                                                      true,
                                                      "active") == ESP_ERR_TIMEOUT);
    assert(nuwax_monitoring_protocol_send_capture_control(
               &ws, "bad request", true, "monitoring_stream_1",
               "no_speech", 120000U) == ESP_ERR_INVALID_ARG);

    const char *sync_ack = "{\"type\":\"audio_mode_request_ack\",\"mode\":\"monitoring\","
        "\"request_id\":\"sync_1\",\"state\":\"enabled\",\"ok\":true,\"audio_session_id\":\"scene_1\"}";
    assert(nuwax_monitoring_protocol_parse(sync_ack, (int)strlen(sync_ack), &event));
    assert(event.type == NUWAX_MONITORING_PROTOCOL_EVENT_REQUEST_ACK && event.ok && event.enabled);
    assert(strcmp(event.audio_session_id, "scene_1") == 0);
    assert_rejected("{\"type\":\"audio_mode_request_ack\",\"mode\":\"meeting\","
        "\"request_id\":\"sync_1\",\"state\":\"enabled\",\"ok\":true}");
    assert_rejected("{\"type\":\"audio_mode_request_ack\",\"mode\":\"monitoring\","
        "\"request_id\":\"sync_1\",\"state\":\"enabled\",\"ok\":\"true\"}");
    assert_rejected("{\"type\":\"audio_mode_request_ack\",\"mode\":\"monitoring\","
        "\"request_id\":\"sync_1\",\"state\":\"enabled\",\"ok\":true,\"ok\":false}");
    assert_rejected("{\"type\":\"device_command\",\"command\":\"set_audio_mode\",\"mode\":\"monitoring\","
        "\"request_id\":\"stop_1\",\"state\":\"disabled\",\"audio_session_id\":3}");
    puts("monitoring_protocol_host_check: PASS");
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
