/* Host-only set_wake_word protocol check. Not part of the ESP-IDF component. */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "board/audio_board.h"
#include "conversation/session_dispatch.h"
#include "esp_app_desc.h"
#include "session_protocol.h"

static char s_last_ack[512];
static unsigned s_set_calls;
static unsigned s_set_array_calls;
static unsigned s_reset_calls;
static bool s_last_persist;
static char s_last_wake_word[64];
static nuwax_wake_word_config_t s_last_wake_config;
static esp_err_t s_set_result = ESP_OK;
static esp_err_t s_reset_result = ESP_OK;
static bool s_active_turn_matches;
static unsigned s_matching_cancel_calls;
static nuwax_session_playback_stop_result_t s_stop_result =
    NUWAX_SESSION_PLAYBACK_STOP_CANCEL_ARMED;
static unsigned s_set_active_turn_calls;
static unsigned s_status_calls;
static unsigned s_return_to_listening_calls;
static int64_t s_last_send_deadline_us;
static uint32_t s_last_send_epoch;
static uint32_t s_connection_epoch = 11U;
static unsigned s_stop_request_seq;

uint32_t nuwax_ws_connection_epoch(const nuwax_ws_client_t *client)
{
    assert(client != NULL);
    return s_connection_epoch;
}

bool nuwax_ws_heartbeat_healthy_if_epoch(const nuwax_ws_client_t *client,
                                         uint32_t expected_epoch)
{
    assert(client != NULL);
    return expected_epoch == s_connection_epoch;
}

bool nuwax_ws_is_connected(const nuwax_ws_client_t *client)
{
    return client != NULL;
}

esp_err_t nuwax_ws_send_text(nuwax_ws_client_t *ws, const char *payload)
{
    assert(ws != NULL);
    assert(payload != NULL);
    assert(strlen(payload) < sizeof(s_last_ack));
    strcpy(s_last_ack, payload);
    return ESP_OK;
}

esp_err_t nuwax_ws_send_text_until(nuwax_ws_client_t *ws,
                                   const char *payload,
                                   int64_t deadline_us)
{
    s_last_send_deadline_us = deadline_us;
    return nuwax_ws_send_text(ws, payload);
}

esp_err_t nuwax_ws_send_text_until_if_epoch(nuwax_ws_client_t *ws,
                                            const char *payload,
                                            int64_t deadline_us,
                                            uint32_t expected_epoch)
{
    s_last_send_epoch = expected_epoch;
    return nuwax_ws_send_text_until(ws, payload, deadline_us);
}

esp_err_t nuwax_ws_send_keepalive_until_if_epoch(nuwax_ws_client_t *ws,
    const char *payload, int64_t deadline_us, uint32_t expected_epoch)
{
    return nuwax_ws_send_text_until_if_epoch(ws,payload,deadline_us,expected_epoch);
}

esp_err_t nuwax_ws_send_text_with_timeout_if_epoch(
    nuwax_ws_client_t *ws,
    const char *payload,
    uint32_t timeout_ms,
    uint32_t expected_epoch)
{
    (void)timeout_ms;
    s_last_send_epoch = expected_epoch;
    return nuwax_ws_send_text(ws, payload);
}

const nuwax_audio_board_caps_t *nuwax_audio_board_get_caps(void)
{
    static const nuwax_audio_board_caps_t caps = {
        .hardware_profile = "host",
    };
    return &caps;
}

nuwax_hardware_ref_health_t nuwax_audio_board_get_hardware_ref_health(void)
{
    return NUWAX_HARDWARE_REF_HEALTH_UNTESTED;
}

const char *nuwax_device_uid_string(void)
{
    return "host-device";
}

nuwax_cloud_message_kind_t nuwax_cloud_classify_message(const char *payload,
                                                        int len,
                                                        const char *device_id)
{
    (void)payload;
    (void)len;
    (void)device_id;
    return NUWAX_CLOUD_MESSAGE_NONE;
}

const esp_app_desc_t *esp_app_get_description(void)
{
    static const esp_app_desc_t app = {
        .project_name = "host",
        .version = "test",
    };
    return &app;
}

static esp_err_t set_wake_word(void *ctx, const char *wake_word, bool persist)
{
    (void)ctx;
    s_set_calls++;
    s_last_persist = persist;
    assert(strlen(wake_word) < sizeof(s_last_wake_word));
    strcpy(s_last_wake_word, wake_word);
    return s_set_result;
}

static esp_err_t set_wake_words(void *ctx,
                                const nuwax_wake_word_config_t *config,
                                bool persist)
{
    (void)ctx;
    assert(config != NULL);
    s_set_array_calls++;
    s_last_persist = persist;
    s_last_wake_config = *config;
    return s_set_result;
}

static esp_err_t reset_wake_word(void *ctx)
{
    (void)ctx;
    s_reset_calls++;
    return s_reset_result;
}

static bool active_turn_matches_playback(void *ctx,
                                         const char *turn_id,
                                         const char *playback_id)
{
    (void)ctx;
    (void)turn_id;
    (void)playback_id;
    return s_active_turn_matches;
}

static nuwax_session_playback_stop_result_t
speaker_cancel_matching_playback(void *ctx,
                                 const char *turn_id,
                                 const char *playback_id,
                                 const char *reason)
{
    (void)ctx;
    assert(strcmp(turn_id, "turn-old") == 0);
    assert(strcmp(playback_id, "playback-old") == 0);
    assert(strcmp(reason, "stale_stop_playback") == 0 ||
           strcmp(reason, "stop_playback") == 0);
    s_matching_cancel_calls++;
    return s_stop_result;
}

static void set_active_turn_id(void *ctx, const char *turn_id)
{
    (void)ctx;
    (void)turn_id;
    s_set_active_turn_calls++;
}

static void set_conversation_status(void *ctx,
                                    const char *title,
                                    const char *phase,
                                    const char *text,
                                    bool show_now)
{
    (void)ctx;
    (void)title;
    (void)phase;
    (void)text;
    (void)show_now;
    s_status_calls++;
}

static void return_to_listening_after_cancel(void *ctx, const char *reason)
{
    (void)ctx;
    (void)reason;
    s_return_to_listening_calls++;
}

static cJSON *dispatch_stop_playback(bool active_turn_matches)
{
    nuwax_ws_client_t ws = {0};
    nuwax_session_command_t command = {0};
    command.has_command = true;
    strcpy(command.command, "stop_playback");
    snprintf(command.request_id,
             sizeof(command.request_id),
             "stop-%s-%u",
             active_turn_matches ? "current" : "old",
             ++s_stop_request_seq);
    strcpy(command.turn_id, "turn-old");
    strcpy(command.playback_id, "playback-old");
    s_active_turn_matches = active_turn_matches;
    memset(s_last_ack, 0, sizeof(s_last_ack));
    nuwax_session_dispatch_callbacks_t callbacks = {
        .set_active_turn_id = set_active_turn_id,
        .active_turn_matches_playback = active_turn_matches_playback,
        .speaker_cancel_matching_playback =
            speaker_cancel_matching_playback,
        .set_conversation_status = set_conversation_status,
        .return_to_listening_after_cancel = return_to_listening_after_cancel,
    };
    nuwax_session_dispatch_command(&ws, &command, &callbacks);
    cJSON *ack = cJSON_Parse(s_last_ack);
    assert(ack != NULL);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(ack, "command")->valuestring,
                  "stop_playback") == 0);
    return ack;
}

static cJSON *dispatch_json(const char *json,
                            nuwax_session_command_t *out_command)
{
    nuwax_ws_client_t ws = {0};
    nuwax_session_dispatch_callbacks_t callbacks = {
        .set_wake_word = set_wake_word,
        .set_wake_words = set_wake_words,
        .reset_wake_word = reset_wake_word,
    };
    memset(s_last_ack, 0, sizeof(s_last_ack));
    nuwax_session_handle_text(json, (int)strlen(json), out_command);
    nuwax_session_dispatch_command(&ws, out_command, &callbacks);
    cJSON *ack = cJSON_Parse(s_last_ack);
    assert(ack != NULL);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(ack, "type")->valuestring,
                  "device_command_ack") == 0);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(ack, "command")->valuestring,
                  out_command->command) == 0);
    return ack;
}

static void assert_ack_ok(const cJSON *ack, bool expected)
{
    const cJSON *ok = cJSON_GetObjectItemCaseSensitive(ack, "ok");
    assert(cJSON_IsBool(ok));
    assert(cJSON_IsTrue(ok) == expected);
}

int main(void)
{
    const char *pongs[] = {
        "{\"type\":\"pong\",\"server_ms\":1792634400123,\"now_ms\":1234}",
        "{\"type\":\"pong\"}",
        "{\"type\":\"pong\",\"server_ms\":1234}",
        "{\"type\":\"pong\",\"server_ms\":\"1792634400123\"}",
        "{\"type\":\"pong\",\"server_ms\":1792634400123.5}",
        "{\"type\":\"pong\",\"server_ms\":null}",
        "{\"type\":\"pong\",\"server_ms\":1e100}"
    };
    nuwax_session_command_t pong;
    for (unsigned i = 0; i < sizeof(pongs) / sizeof(pongs[0]); ++i) {
        memset(&pong, 0x5a, sizeof(pong));
        nuwax_session_handle_text(pongs[i], (int)strlen(pongs[i]), &pong);
        assert(pong.has_pong);
        assert(pong.pong_server_ms == (i == 0 ? UINT64_C(1792634400123) : 0));
    }
    assert(nuwax_session_validate_text_frame(
        "{\"type\":\"device_command\",\"command\":\"wifi_scan\",\"request_id\":\"scan-1\"}",
        (int)strlen("{\"type\":\"device_command\",\"command\":\"wifi_scan\",\"request_id\":\"scan-1\"}")));
    const char *missing_request =
        "{\"type\":\"device_command\",\"command\":\"wifi_scan\"}";
    assert(!nuwax_session_validate_text_frame(
        missing_request, (int)strlen(missing_request)));
    const char *unenveloped_other_command =
        "{\"command\":\"set_volume\",\"volume_percent\":50}";
    assert(!nuwax_session_validate_text_frame(
        unenveloped_other_command,
        (int)strlen(unenveloped_other_command)));
    const char *half_enveloped_wake_word =
        "{\"command\":\"set_wake_word\",\"request_id\":\"wake-half\","
        "\"wake_word\":[\"ni hao ling zhu\"]}";
    assert(!nuwax_session_validate_text_frame(
        half_enveloped_wake_word,
        (int)strlen(half_enveloped_wake_word)));
    const char *unscoped_stop =
        "{\"type\":\"device_command\",\"command\":\"stop_playback\","
        "\"request_id\":\"stop-1\",\"turn_id\":\"turn-1\"}";
    assert(!nuwax_session_validate_text_frame(
        unscoped_stop, (int)strlen(unscoped_stop)));
    const char *scoped_stop =
        "{\"type\":\"device_command\",\"command\":\"stop_playback\","
        "\"request_id\":\"stop-1\",\"turn_id\":\"turn-1\","
        "\"playback_id\":\"pb-1\"}";
    assert(nuwax_session_validate_text_frame(
        scoped_stop, (int)strlen(scoped_stop)));
    const char *device_owned_state =
        "{\"type\":\"device_command\",\"command\":\"set_state\"," 
        "\"request_id\":\"state-1\",\"state\":\"thinking\"}";
    assert(!nuwax_session_validate_text_frame(
        device_owned_state, (int)strlen(device_owned_state)));
    const char *sleep_state =
        "{\"type\":\"device_command\",\"command\":\"set_state\"," 
        "\"request_id\":\"state-2\",\"state\":\"sleeping\"}";
    assert(nuwax_session_validate_text_frame(
        sleep_state, (int)strlen(sleep_state)));
    const char *optional_null_request =
        "{\"type\":\"text_response\",\"ok\":true,"
        "\"request_id\":null,\"turn_id\":\"turn-null\","
        "\"assistant_text\":\"ok\",\"tts_pending\":false}";
    assert(nuwax_session_validate_text_frame(
        optional_null_request, (int)strlen(optional_null_request)));
    nuwax_session_command_t null_request_command = {0};
    nuwax_session_handle_text(optional_null_request,
                              (int)strlen(optional_null_request),
                              &null_request_command);
    assert(null_request_command.has_event);
    assert(null_request_command.event == NUWAX_SESSION_EVENT_TEXT_RESPONSE);
    assert(null_request_command.request_id[0] == '\0');
    assert(strcmp(null_request_command.turn_id, "turn-null") == 0);

    const char *tts_skip =
        "{\"type\":\"tts_skip\",\"ok\":true,"
        "\"request_id\":\"\",\"turn_id\":\"\","
        "\"reason\":\"voice_broadcast_disabled\"}";
    assert(nuwax_session_validate_text_frame(tts_skip,
                                              (int)strlen(tts_skip)));
    nuwax_session_command_t tts_skip_command = {0};
    nuwax_session_handle_text(tts_skip,
                              (int)strlen(tts_skip),
                              &tts_skip_command);
    assert(tts_skip_command.has_event);
    assert(tts_skip_command.event == NUWAX_SESSION_EVENT_TTS_SKIP);
    assert(tts_skip_command.ok);
    assert(tts_skip_command.request_id[0] == '\0');
    assert(tts_skip_command.turn_id[0] == '\0');
    assert(strcmp(tts_skip_command.error,
                  "voice_broadcast_disabled") == 0);

    const char *tts_skip_null_ids =
        "{\"type\":\"tts_skip\",\"ok\":true,"
        "\"request_id\":null,\"turn_id\":null,"
        "\"reason\":\"voice_broadcast_disabled\"}";
    assert(nuwax_session_validate_text_frame(
        tts_skip_null_ids, (int)strlen(tts_skip_null_ids)));
    nuwax_session_command_t tts_skip_null_command = {0};
    nuwax_session_handle_text(tts_skip_null_ids,
                              (int)strlen(tts_skip_null_ids),
                              &tts_skip_null_command);
    assert(tts_skip_null_command.event == NUWAX_SESSION_EVENT_TTS_SKIP);
    assert(tts_skip_null_command.request_id[0] == '\0');
    assert(tts_skip_null_command.turn_id[0] == '\0');

    const char *tts_skip_missing_ids =
        "{\"type\":\"tts_skip\",\"ok\":true,"
        "\"reason\":\"voice_broadcast_disabled\"}";
    assert(nuwax_session_validate_text_frame(
        tts_skip_missing_ids, (int)strlen(tts_skip_missing_ids)));

    const char *tts_skip_camel_ids =
        "{\"type\":\"tts_skip\",\"ok\":true,"
        "\"requestId\":\"req-camel\",\"turnId\":\"turn-camel\","
        "\"reason\":\"voice_broadcast_disabled\"}";
    assert(nuwax_session_validate_text_frame(
        tts_skip_camel_ids, (int)strlen(tts_skip_camel_ids)));
    nuwax_session_command_t tts_skip_camel_command = {0};
    nuwax_session_handle_text(tts_skip_camel_ids,
                              (int)strlen(tts_skip_camel_ids),
                              &tts_skip_camel_command);
    assert(strcmp(tts_skip_camel_command.request_id, "req-camel") == 0);
    assert(strcmp(tts_skip_camel_command.turn_id, "turn-camel") == 0);

    const char *tts_skip_invalid_id_type =
        "{\"type\":\"tts_skip\",\"ok\":true,"
        "\"request_id\":7,\"turn_id\":null,"
        "\"reason\":\"voice_broadcast_disabled\"}";
    assert(!nuwax_session_validate_text_frame(
        tts_skip_invalid_id_type, (int)strlen(tts_skip_invalid_id_type)));

    const char *invalid_tts_skip_reason =
        "{\"type\":\"tts_skip\",\"ok\":true,"
        "\"request_id\":\"\",\"turn_id\":\"\","
        "\"reason\":\"tts_failed\"}";
    assert(!nuwax_session_validate_text_frame(
        invalid_tts_skip_reason, (int)strlen(invalid_tts_skip_reason)));
    const char *failed_tts_skip =
        "{\"type\":\"tts_skip\",\"ok\":false,"
        "\"request_id\":\"\",\"turn_id\":\"\","
        "\"reason\":\"voice_broadcast_disabled\"}";
    assert(!nuwax_session_validate_text_frame(
        failed_tts_skip, (int)strlen(failed_tts_skip)));

    const char *required_null_request =
        "{\"type\":\"device_command\",\"command\":\"wifi_scan\","
        "\"request_id\":null}";
    assert(!nuwax_session_validate_text_frame(
        required_null_request, (int)strlen(required_null_request)));

    const char *cancel_ack =
        "{\"type\":\"cancel_turn_ack\",\"ok\":true,"
        "\"request_id\":null,\"turn_id\":\"turn-cancel\","
        "\"reason\":\"speaker_playback_fault\"}";
    assert(nuwax_session_validate_text_frame(cancel_ack,
                                              (int)strlen(cancel_ack)));
    nuwax_session_command_t cancel_ack_command = {0};
    nuwax_session_handle_text(cancel_ack,
                              (int)strlen(cancel_ack),
                              &cancel_ack_command);
    assert(cancel_ack_command.has_event);
    assert(cancel_ack_command.event == NUWAX_SESSION_EVENT_CANCEL_TURN_ACK);
    assert(cancel_ack_command.ok);
    assert(cancel_ack_command.request_id[0] == '\0');
    assert(strcmp(cancel_ack_command.turn_id, "turn-cancel") == 0);
    const char *playback_limit_cancel_ack =
        "{\"type\":\"cancel_turn_ack\",\"ok\":true,"
        "\"request_id\":\"timeout_cancel_1\","
        "\"turn_id\":\"turn-long-playback\","
        "\"reason\":\"playback_limit_timeout\"}";
    assert(nuwax_session_validate_text_frame(
        playback_limit_cancel_ack,
        (int)strlen(playback_limit_cancel_ack)));
    nuwax_session_command_t playback_limit_ack_command = {0};
    nuwax_session_handle_text(playback_limit_cancel_ack,
                              (int)strlen(playback_limit_cancel_ack),
                              &playback_limit_ack_command);
    assert(playback_limit_ack_command.has_event);
    assert(playback_limit_ack_command.event ==
           NUWAX_SESSION_EVENT_CANCEL_TURN_ACK);
    assert(playback_limit_ack_command.ok);
    assert(strcmp(playback_limit_ack_command.request_id,
                  "timeout_cancel_1") == 0);
    assert(strcmp(playback_limit_ack_command.turn_id,
                  "turn-long-playback") == 0);
    assert(strcmp(playback_limit_ack_command.playback_reason,
                  "playback_limit_timeout") == 0);
    const char *overlong_utf8_request =
        "{\"type\":\"device_command\",\"command\":\"wifi_scan\","
        "\"request_id\":\"\u4e2d\u4e2d\u4e2d\u4e2d\u4e2d\u4e2d\u4e2d"
        "\u4e2d\u4e2d\u4e2d\u4e2d\u4e2d\u4e2d\u4e2d\"}";
    assert(!nuwax_session_validate_text_frame(
        overlong_utf8_request, (int)strlen(overlong_utf8_request)));
    const char *duplicate_enabled =
        "{\"type\":\"device_command\",\"command\":\"set_audio_cleaner\"," 
        "\"request_id\":\"cleaner-1\",\"enabled\":true,\"enabled\":false}";
    assert(!nuwax_session_validate_text_frame(
        duplicate_enabled, (int)strlen(duplicate_enabled)));
    const char *conflicting_volume_alias =
        "{\"type\":\"device_command\",\"command\":\"set_volume\"," 
        "\"request_id\":\"volume-1\",\"volume_percent\":20," 
        "\"volumePercent\":80}";
    assert(!nuwax_session_validate_text_frame(
        conflicting_volume_alias, (int)strlen(conflicting_volume_alias)));
    const char *invalid_request_characters =
        "{\"type\":\"device_command\",\"command\":\"wifi_scan\"," 
        "\"request_id\":\"invalid request\"}";
    assert(!nuwax_session_validate_text_frame(
        invalid_request_characters,
        (int)strlen(invalid_request_characters)));
    const char *invalid_command_characters =
        "{\"type\":\"device_command\",\"command\":\"WiFiScan\"," 
        "\"request_id\":\"scan-2\"}";
    assert(!nuwax_session_validate_text_frame(
        invalid_command_characters,
        (int)strlen(invalid_command_characters)));
    const char *valid_mcp =
        "{\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\","
        "\"id\":\"mcp-1\",\"method\":\"tools/list\",\"params\":{}}}";
    assert(nuwax_session_validate_text_frame(valid_mcp,
                                              (int)strlen(valid_mcp)));
    const char *invalid_mcp =
        "{\"type\":\"mcp\",\"payload\":{\"id\":\"mcp-1\","
        "\"method\":\"tools/list\"}}";
    assert(!nuwax_session_validate_text_frame(invalid_mcp,
                                               (int)strlen(invalid_mcp)));

    nuwax_ws_client_t ping_ws = {0};
    memset(s_last_ack, 0, sizeof(s_last_ack));
    assert(nuwax_session_send_ping_until_if_epoch(&ping_ws,
                                                  123456,
                                                  987654,
                                                  7U) == ESP_OK);
    cJSON *ping = cJSON_Parse(s_last_ack);
    assert(ping != NULL);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(ping, "type")->valuestring,
                  "ping") == 0);
    assert(cJSON_GetObjectItemCaseSensitive(ping, "now_ms")->valueint ==
           123456);
    assert(s_last_send_deadline_us == 987654);
    assert(s_last_send_epoch == 7U);
    cJSON_Delete(ping);
    assert(nuwax_session_send_ping_until_if_epoch(&ping_ws,
                                                  1,
                                                  2,
                                                  0U) ==
           ESP_ERR_INVALID_ARG);

    memset(s_last_ack, 0, sizeof(s_last_ack));
    assert(nuwax_session_send_volume_changed_until_if_epoch(
               &ping_ws,
               "volume_deadbeef_00000001",
               65,
               "device_ui",
               1234567,
               9U) == ESP_OK);
    cJSON *volume_changed = cJSON_Parse(s_last_ack);
    assert(volume_changed != NULL);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(volume_changed, "type")
                      ->valuestring,
                  "volume_changed") == 0);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(volume_changed, "event_id")
                      ->valuestring,
                  "volume_deadbeef_00000001") == 0);
    assert(cJSON_GetObjectItemCaseSensitive(volume_changed, "volume_percent")
               ->valueint == 65);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(volume_changed, "source")
                      ->valuestring,
                  "device_ui") == 0);
    assert(s_last_send_deadline_us == 1234567);
    assert(s_last_send_epoch == 9U);
    cJSON_Delete(volume_changed);
    assert(nuwax_session_send_volume_changed_until_if_epoch(
               &ping_ws,
               "volume_bad",
               101,
               "device_ui",
               1234567,
               9U) == ESP_ERR_INVALID_ARG);
    assert(nuwax_session_send_volume_changed_until_if_epoch(
               &ping_ws,
               "volume_bad",
               50,
               "cloud_command",
               1234567,
               9U) == ESP_ERR_INVALID_ARG);

    nuwax_session_command_t command = {0};
    const char *mcp_json =
        "{\"type\":\"mcp\",\"payload\":{\"method\":\"host.check\"}}";
    nuwax_session_handle_text(mcp_json, (int)strlen(mcp_json), &command);
    assert(command.has_mcp);
    assert(!command.has_command);
    assert(strstr(command.mcp_payload, "host.check") != NULL);

    const char *legacy_unenveloped_wake_word =
        "{\"command\":\"set_wake_word\",\"wake_word\":["
        "\"ni hao ling zhu\",\"ni hao lin zu\","
        "\"ni hao ling zu\",\"ni hao lin zhu\"],\"persist\":true}";
    assert(nuwax_session_validate_text_frame(
        legacy_unenveloped_wake_word,
        (int)strlen(legacy_unenveloped_wake_word)));
    cJSON *ack = dispatch_json(legacy_unenveloped_wake_word, &command);
    assert(command.has_command);
    assert(strcmp(command.command, "set_wake_word") == 0);
    assert(command.request_id[0] == '\0');
    assert(command.wake_word_is_array);
    assert(!command.wake_word_invalid);
    assert(!command.wake_word_truncated);
    assert(command.wake_word_config.count == 4U);
    assert(s_set_array_calls == 1U);
    assert(s_last_persist);
    assert(s_last_wake_config.count == 4U);
    assert(strcmp(s_last_wake_config.variants[0],
                  "ni hao ling zhu") == 0);
    assert(strcmp(s_last_wake_config.variants[1],
                  "ni hao lin zu") == 0);
    assert(strcmp(s_last_wake_config.variants[2],
                  "ni hao ling zu") == 0);
    assert(strcmp(s_last_wake_config.variants[3],
                  "ni hao lin zhu") == 0);
    assert(cJSON_GetObjectItemCaseSensitive(ack, "request_id") == NULL);
    assert_ack_ok(ack, true);
    cJSON_Delete(ack);
    s_set_array_calls = 0U;
    memset(&s_last_wake_config, 0, sizeof(s_last_wake_config));

    ack = dispatch_json(
        "{\"type\":\"device_command\",\"command\":\"set_wake_word\","
        "\"request_id\":\"wake-1\",\"wake_word\":\"Ni  Hao Ling Zhu\"}",
        &command);
    assert(command.has_command);
    assert(command.wake_word_config.count == 1U);
    assert(strcmp(command.wake_word_config.variants[0],
                  "Ni  Hao Ling Zhu") == 0);
    assert(!command.has_wake_word_persist);
    assert(s_set_calls == 1U);
    assert(s_last_persist);
    assert(strcmp(s_last_wake_word, "Ni  Hao Ling Zhu") == 0);
    assert_ack_ok(ack, true);
    cJSON_Delete(ack);

    ack = dispatch_json(
        "{\"type\":\"device_command\","
        "\"request_id\":\"wake-reset-1\","
        "\"command\":\"reset_wake_word\"}",
        &command);
    assert(command.has_command);
    assert(strcmp(command.command, "reset_wake_word") == 0);
    assert(s_reset_calls == 1U);
    assert_ack_ok(ack, true);
    assert(strstr(cJSON_GetObjectItemCaseSensitive(ack, "message")->valuestring,
                  "source=factory_default") != NULL);
    cJSON_Delete(ack);

    s_reset_result = ESP_FAIL;
    ack = dispatch_json(
        "{\"type\":\"device_command\","
        "\"request_id\":\"wake-reset-2\","
        "\"command\":\"reset_wake_word\"}",
        &command);
    assert(s_reset_calls == 2U);
    assert_ack_ok(ack, false);
    cJSON_Delete(ack);
    s_reset_result = ESP_OK;

    ack = dispatch_json(
        "{\"type\":\"device_command\",\"command\":\"set_wake_word\","
        "\"request_id\":\"wake-array-4\",\"wake_word\":["
        "\"ni hao ling zhu\",\"ni hao lin zu\","
        "\"ni hao ling zu\",\"ni hao lin zhu\"],\"persist\":true}",
        &command);
    assert(command.wake_word_is_array);
    assert(!command.wake_word_invalid);
    assert(!command.wake_word_truncated);
    assert(command.wake_word_config.count == 4U);
    assert(s_set_array_calls == 1U);
    assert(s_last_persist);
    assert(s_last_wake_config.count == 4U);
    assert(strcmp(s_last_wake_config.variants[0], "ni hao ling zhu") == 0);
    assert(strcmp(s_last_wake_config.variants[3], "ni hao lin zhu") == 0);
    assert_ack_ok(ack, true);
    assert(strstr(cJSON_GetObjectItemCaseSensitive(ack, "message")->valuestring,
                  "wake_words=4") != NULL);
    assert(strstr(cJSON_GetObjectItemCaseSensitive(ack, "message")->valuestring,
                  "truncated=0") != NULL);
    cJSON_Delete(ack);

    ack = dispatch_json(
        "{\"type\":\"device_command\",\"command\":\"set_wake_word\","
        "\"request_id\":\"wake-array-1\",\"wakeWord\":[\"xiao nu\"],"
        "\"persist\":false}",
        &command);
    assert(command.wake_word_is_array);
    assert(command.wake_word_config.count == 1U);
    assert(s_set_array_calls == 2U);
    assert(!s_last_persist);
    assert(strcmp(s_last_wake_config.variants[0], "xiao nu") == 0);
    assert_ack_ok(ack, true);
    cJSON_Delete(ack);

    ack = dispatch_json(
        "{\"type\":\"device_command\",\"command\":\"set_wake_word\","
        "\"request_id\":\"wake-array-variable\",\"wake_word\":["
        "\"xiao nu\",\"xiao ling zhu\","
        "\"qing bang wo kai deng\"]}",
        &command);
    assert(command.wake_word_is_array);
    assert(command.wake_word_config.count == 3U);
    assert(s_set_array_calls == 3U);
    assert(s_last_persist);
    assert(strcmp(s_last_wake_config.variants[0], "xiao nu") == 0);
    assert(strcmp(s_last_wake_config.variants[1], "xiao ling zhu") == 0);
    assert(strcmp(s_last_wake_config.variants[2],
                  "qing bang wo kai deng") == 0);
    assert_ack_ok(ack, true);
    cJSON_Delete(ack);

    ack = dispatch_json(
        "{\"type\":\"device_command\",\"command\":\"set_wake_word\","
        "\"request_id\":\"wake-array-8\",\"wake_word\":["
        "\"alpha one\",\"bravo two\",\"charlie three\",\"delta four\","
        "\"echo five\",\"foxtrot six\",\"golf seven\",\"hotel eight\"]}",
        &command);
    assert(command.wake_word_is_array);
    assert(!command.wake_word_invalid);
    assert(!command.wake_word_truncated);
    assert(command.wake_word_config.count == 8U);
    assert(s_set_array_calls == 4U);
    assert(s_last_wake_config.count == 8U);
    assert(strcmp(s_last_wake_config.variants[0], "alpha one") == 0);
    assert(strcmp(s_last_wake_config.variants[7], "hotel eight") == 0);
    assert_ack_ok(ack, true);
    assert(strstr(cJSON_GetObjectItemCaseSensitive(ack, "message")->valuestring,
                  "wake_words=8") != NULL);
    cJSON_Delete(ack);

    ack = dispatch_json(
        "{\"type\":\"device_command\",\"command\":\"set_wake_word\","
        "\"request_id\":\"wake-array-empty\",\"wake_word\":[]}",
        &command);
    assert(command.wake_word_is_array);
    assert(command.wake_word_invalid);
    assert(s_set_array_calls == 4U);
    assert_ack_ok(ack, false);
    cJSON_Delete(ack);

    ack = dispatch_json(
        "{\"type\":\"device_command\",\"command\":\"set_wake_word\","
        "\"request_id\":\"wake-array-many\",\"wake_word\":["
        "\"one\",\"two\",\"three\",\"four\",\"five\",\"six\","
        "\"seven\",\"eight\",\"nine\",\"ten\",\"eleven\"]}",
        &command);
    assert(command.wake_word_is_array);
    assert(!command.wake_word_invalid);
    assert(command.wake_word_truncated);
    assert(command.wake_word_config.count == 10U);
    assert(s_set_array_calls == 5U);
    assert(s_last_wake_config.count == 10U);
    assert(strcmp(s_last_wake_config.variants[0], "one") == 0);
    assert(strcmp(s_last_wake_config.variants[9], "ten") == 0);
    assert_ack_ok(ack, true);
    assert(strstr(cJSON_GetObjectItemCaseSensitive(ack, "message")->valuestring,
                  "wake_words=10") != NULL);
    assert(strstr(cJSON_GetObjectItemCaseSensitive(ack, "message")->valuestring,
                  "truncated=1") != NULL);
    cJSON_Delete(ack);

    ack = dispatch_json(
        "{\"type\":\"device_command\",\"command\":\"set_wake_word\","
        "\"request_id\":\"wake-array-type\",\"wake_word\":["
        "\"ni hao\",123]}",
        &command);
    assert(command.wake_word_invalid);
    assert(s_set_array_calls == 5U);
    assert_ack_ok(ack, false);
    cJSON_Delete(ack);

    nuwax_wake_word_config_t raw_config = {0};
    raw_config.count = 4U;
    strcpy(raw_config.variants[0], " Ni  Hao Ling Zhu ");
    strcpy(raw_config.variants[1], "ni hao ling zhu");
    strcpy(raw_config.variants[2], "NI HAO LIN ZU");
    strcpy(raw_config.variants[3], "xiao nu");
    nuwax_wake_word_config_t normalized_config = {0};
    assert(nuwax_wake_word_config_normalize(&raw_config,
                                            &normalized_config));
    assert(normalized_config.count == 3U);
    assert(strcmp(normalized_config.variants[0], "ni hao ling zhu") == 0);
    assert(strcmp(normalized_config.variants[1], "ni hao lin zu") == 0);
    assert(strcmp(normalized_config.variants[2], "xiao nu") == 0);

    nuwax_wake_word_config_t legacy_config = {0};
    assert(nuwax_wake_word_config_from_legacy("NI  HAO LING ZHU",
                                              &legacy_config));
    assert(legacy_config.count == 10U);
    assert(strcmp(legacy_config.variants[0], "xiao zhu xiao zhu") == 0);
    assert(strcmp(legacy_config.variants[5], "ni hao lin zhu") == 0);
    assert(strcmp(legacy_config.variants[9], "ni hao xiao zhu") == 0);
    nuwax_wake_word_config_t factory_config = {0};
    assert(nuwax_wake_word_config_from_legacy("XIAO ZHU XIAO ZHU",
                                              &factory_config));
    assert(factory_config.count == 10U);
    assert(strcmp(factory_config.variants[0], "xiao zhu xiao zhu") == 0);
    assert(strcmp(factory_config.variants[1], "xiao zu xiao zu") == 0);
    assert(strcmp(factory_config.variants[2], "xiao zhu xiao zu") == 0);
    assert(strcmp(factory_config.variants[3], "xiao zu xiao zhu") == 0);
    assert(strcmp(factory_config.variants[4], "ni hao ling zhu") == 0);
    assert(strcmp(factory_config.variants[5], "ni hao lin zhu") == 0);
    assert(strcmp(factory_config.variants[6], "ni hao ling zu") == 0);
    assert(strcmp(factory_config.variants[7], "ni hao lin zu") == 0);
    assert(strcmp(factory_config.variants[8], "ni hao xiao zu") == 0);
    assert(strcmp(factory_config.variants[9], "ni hao xiao zhu") == 0);
    assert(!nuwax_wake_word_config_from_legacy("ni-hao", &legacy_config));

    ack = dispatch_json(
        "{\"type\":\"device_command\",\"command\":\"set_wake_word\","
        "\"request_id\":\"wake-2\",\"wakeWord\":\"xiao nu\",\"persist\":false}",
        &command);
    assert(command.has_wake_word_persist);
    assert(!command.wake_word_persist);
    assert(s_set_calls == 2U);
    assert(!s_last_persist);
    assert_ack_ok(ack, true);
    cJSON_Delete(ack);

    ack = dispatch_json(
        "{\"type\":\"device_command\",\"command\":\"set_wake_word\","
        "\"request_id\":\"wake-3\"}",
        &command);
    assert(s_set_calls == 2U);
    assert_ack_ok(ack, false);
    cJSON_Delete(ack);

    char long_word[65];
    memset(long_word, 'a', sizeof(long_word) - 1U);
    long_word[sizeof(long_word) - 1U] = '\0';
    char long_json[256];
    const int written = snprintf(long_json,
                                 sizeof(long_json),
                                 "{\"type\":\"device_command\","
                                 "\"command\":\"set_wake_word\","
                                 "\"request_id\":\"wake-4\","
                                 "\"wake_word\":\"%s\"}",
                                 long_word);
    assert(written > 0 && written < (int)sizeof(long_json));
    ack = dispatch_json(long_json, &command);
    assert(command.wake_word_config.count == 0U);
    assert(command.wake_word_invalid);
    assert(s_set_calls == 2U);
    assert_ack_ok(ack, false);
    cJSON_Delete(ack);

    s_set_result = ESP_ERR_INVALID_ARG;
    ack = dispatch_json(
        "{\"type\":\"device_command\",\"command\":\"set_wake_word\","
        "\"request_id\":\"wake-5\",\"wake_word\":\"ni-hao\"}",
        &command);
    assert(s_set_calls == 3U);
    assert_ack_ok(ack, false);
    cJSON_Delete(ack);

    s_set_result = ESP_OK;
    const unsigned calls_before_ledger_check = s_set_calls;
    const char *ledger_command =
        "{\"type\":\"device_command\",\"command\":\"set_wake_word\"," 
        "\"request_id\":\"ledger-1\",\"wake_word\":\"xiao nu\"}";
    ack = dispatch_json(ledger_command, &command);
    assert_ack_ok(ack, true);
    cJSON_Delete(ack);
    assert(s_set_calls == calls_before_ledger_check + 1U);

    ack = dispatch_json(ledger_command, &command);
    assert_ack_ok(ack, true);
    cJSON_Delete(ack);
    assert(s_set_calls == calls_before_ledger_check + 1U);

    ack = dispatch_json(
        "{\"type\":\"device_command\",\"command\":\"set_wake_word\"," 
        "\"request_id\":\"ledger-1\",\"wake_word\":\"ni hao ling zhu\"}",
        &command);
    assert_ack_ok(ack, false);
    cJSON_Delete(ack);
    assert(s_set_calls == calls_before_ledger_check + 1U);

    s_connection_epoch = 12U;
    ack = dispatch_json(
        "{\"type\":\"device_command\",\"command\":\"set_wake_word\"," 
        "\"request_id\":\"ledger-1\",\"wake_word\":\"ni hao ling zhu\"}",
        &command);
    assert_ack_ok(ack, true);
    cJSON_Delete(ack);
    assert(s_set_calls == calls_before_ledger_check + 2U);

    s_matching_cancel_calls = 0U;
    s_stop_result = NUWAX_SESSION_PLAYBACK_STOP_CANCEL_ARMED;
    s_set_active_turn_calls = 0U;
    s_status_calls = 0U;
    s_return_to_listening_calls = 0U;
    ack = dispatch_stop_playback(false);
    assert_ack_ok(ack, true);
    assert(s_matching_cancel_calls == 1U);
    assert(s_set_active_turn_calls == 0U);
    assert(s_status_calls == 0U);
    assert(s_return_to_listening_calls == 0U);
    cJSON_Delete(ack);

    s_matching_cancel_calls = 0U;
    const unsigned status_before_natural_drain = s_status_calls;
    s_stop_result = NUWAX_SESSION_PLAYBACK_STOP_NATURAL_DRAINING;
    ack = dispatch_stop_playback(true);
    assert_ack_ok(ack, true);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(ack, "message")->valuestring,
                  "already draining") == 0);
    assert(s_matching_cancel_calls == 1U);
    assert(s_status_calls == status_before_natural_drain);
    assert(s_return_to_listening_calls == 0U);
    cJSON_Delete(ack);

    s_matching_cancel_calls = 0U;
    s_stop_result = NUWAX_SESSION_PLAYBACK_STOP_CANCEL_ARMED;
    ack = dispatch_stop_playback(true);
    assert_ack_ok(ack, true);
    assert(s_matching_cancel_calls == 1U);
    assert(s_set_active_turn_calls == 0U);
    assert(s_status_calls == 1U);
    assert(s_return_to_listening_calls == 0U);
    cJSON_Delete(ack);

    puts("session_wake_word_and_stop_playback_host_check: PASS");
    return 0;
}
