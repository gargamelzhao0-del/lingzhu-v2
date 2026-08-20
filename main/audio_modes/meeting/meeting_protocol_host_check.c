/* Host-only protocol check. This file is intentionally not part of the
 * ESP-IDF component. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "meeting_protocol.h"

static char s_last_text[512];

esp_err_t nuwax_ws_send_text(nuwax_ws_client_t *ws, const char *payload) {
    assert(ws != NULL);
    assert(payload != NULL);
    const size_t length = strlen(payload);
    assert(length < sizeof(s_last_text));
    memcpy(s_last_text, payload, length + 1U);
    return ESP_OK;
}

static void assert_parse(const char *json,
                         nuwax_meeting_protocol_event_type_t expected_type,
                         const char *expected_request_id,
                         const char *expected_state) {
    nuwax_meeting_protocol_event_t event = {0};
    assert(nuwax_meeting_protocol_parse(json, (int)strlen(json), &event));
    assert(event.type == expected_type);
    assert(strcmp(event.request_id, expected_request_id) == 0);
    assert(strcmp(event.state, expected_state) == 0);
}

int main(void) {
    const char *request_ack =
        "{\"type\":\"audio_mode_request_ack\",\"request_id\":\"ui_open_1\","
        "\"ok\":true,\"mode\":\"meeting\",\"state\":\"enabled\","
        "\"audio_session_id\":\"meeting_stream_1\",\"current_turn_cancelled\":true}";
    nuwax_meeting_protocol_event_t event = {0};
    assert(nuwax_meeting_protocol_parse(request_ack, (int)strlen(request_ack), &event));
    assert(event.type == NUWAX_MEETING_PROTOCOL_EVENT_REQUEST_ACK);
    assert(event.ok);
    assert(event.current_turn_cancelled);
    assert(strcmp(event.audio_session_id, "meeting_stream_1") == 0);

    const char *enable_command =
        "{\"type\":\"device_command\",\"command\":\"set_audio_mode\","
        "\"request_id\":\"cloud_open_1\",\"mode\":\"meeting\","
        "\"state\":\"enabled\",\"interrupt_current_turn\":true}";
    memset(&event, 0, sizeof(event));
    assert(nuwax_meeting_protocol_parse(enable_command,
                                        (int)strlen(enable_command),
                                        &event));
    assert(event.type == NUWAX_MEETING_PROTOCOL_EVENT_SET_COMMAND);
    assert(event.interrupt_current_turn);
    assert(strcmp(event.request_id, "cloud_open_1") == 0);

    assert_parse("{\"type\":\"device_command\",\"command\":\"set_audio_mode\","
                 "\"request_id\":\"cloud_close_1\",\"mode\":\"meeting\","
                 "\"state\":\"disabled\"}",
                 NUWAX_MEETING_PROTOCOL_EVENT_SET_COMMAND,
                 "cloud_close_1",
                 "disabled");

    const char *wrong_mode =
        "{\"type\":\"device_command\",\"command\":\"set_audio_mode\","
        "\"request_id\":\"wrong_mode\",\"mode\":\"monitor\",\"state\":\"enabled\"}";
    assert(!nuwax_meeting_protocol_parse(wrong_mode,
                                         (int)strlen(wrong_mode),
                                         &event));
    const char *missing_id =
        "{\"type\":\"device_command\",\"command\":\"set_audio_mode\","
        "\"mode\":\"meeting\",\"state\":\"enabled\"}";
    assert(!nuwax_meeting_protocol_parse(missing_id, (int)strlen(missing_id), &event));

    char overlong_json[256];
    char overlong_id[NUWAX_MEETING_REQUEST_ID_BYTES + 1U];
    memset(overlong_id, 'x', sizeof(overlong_id) - 1U);
    overlong_id[sizeof(overlong_id) - 1U] = '\0';
    int written = snprintf(overlong_json,
                           sizeof(overlong_json),
                           "{\"type\":\"device_command\",\"command\":\"set_audio_mode\","
                           "\"request_id\":\"%s\",\"mode\":\"meeting\",\"state\":\"enabled\"}",
                           overlong_id);
    assert(written > 0 && written < (int)sizeof(overlong_json));
    assert(!nuwax_meeting_protocol_parse(overlong_json, written, &event));

    const char *duplicate_type =
        "{\"type\":\"device_command\",\"type\":\"device_command\"," 
        "\"command\":\"set_audio_mode\",\"request_id\":\"duplicate_1\"," 
        "\"mode\":\"meeting\",\"state\":\"enabled\"}";
    assert(!nuwax_meeting_protocol_parse(duplicate_type,
                                         (int)strlen(duplicate_type),
                                         &event));

    const char *trailing_garbage =
        "{\"type\":\"device_command\",\"command\":\"set_audio_mode\"," 
        "\"request_id\":\"trailing_1\",\"mode\":\"meeting\"," 
        "\"state\":\"enabled\"}garbage";
    assert(!nuwax_meeting_protocol_parse(trailing_garbage,
                                         (int)strlen(trailing_garbage),
                                         &event));

    const char *invalid_bool =
        "{\"type\":\"device_command\",\"command\":\"set_audio_mode\"," 
        "\"request_id\":\"invalid_bool_1\",\"mode\":\"meeting\"," 
        "\"state\":\"enabled\",\"interrupt_current_turn\":1}";
    assert(!nuwax_meeting_protocol_parse(invalid_bool,
                                         (int)strlen(invalid_bool),
                                         &event));

    const char *invalid_request_id =
        "{\"type\":\"device_command\",\"command\":\"set_audio_mode\"," 
        "\"request_id\":\"invalid id\",\"mode\":\"meeting\"," 
        "\"state\":\"enabled\"}";
    assert(!nuwax_meeting_protocol_parse(invalid_request_id,
                                         (int)strlen(invalid_request_id),
                                         &event));

    nuwax_ws_client_t ws = {0};
    memset(s_last_text, 0, sizeof(s_last_text));
    assert(nuwax_meeting_protocol_send_request(&ws,
                                               "device_open_1",
                                               true,
                                               true,
                                               "ui") == ESP_OK);
    assert(strstr(s_last_text, "\"type\":\"audio_mode_request\"") != NULL);
    assert(strstr(s_last_text, "\"interrupt_current_turn\":true") != NULL);
    assert(strstr(s_last_text, "\"request_id\":\"device_open_1\"") != NULL);

    memset(s_last_text, 0, sizeof(s_last_text));
    assert(nuwax_meeting_protocol_send_command_ack(&ws,
                                                   "cloud_close_1",
                                                   false,
                                                   true,
                                                   "disabled") == ESP_OK);
    assert(strstr(s_last_text, "\"type\":\"device_command_ack\"") != NULL);
    assert(strstr(s_last_text, "\"state\":\"disabled\"") != NULL);
    assert(strstr(s_last_text, "\"ok\":true") != NULL);

    puts("meeting_protocol_host_check: PASS");
    return 0;
}
