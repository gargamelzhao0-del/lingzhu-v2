#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "mcp_server.h"

static char s_last_payload[2048];
static unsigned s_emotion_calls;
static unsigned s_servo_calls;

esp_err_t nuwax_ws_send_text(nuwax_ws_client_t *ws, const char *payload) {
    (void)ws;
    assert(payload);
    const size_t length = strlen(payload);
    assert(length < sizeof(s_last_payload));
    memcpy(s_last_payload, payload, length + 1U);
    return ESP_OK;
}

static bool accept_emotion(const char *emotion, void *ctx) {
    (void)emotion;
    (void)ctx;
    s_emotion_calls++;
    return true;
}

static bool shake_servo(int times, void *ctx) {
    (void)times;
    (void)ctx;
    s_servo_calls++;
    return true;
}

static cJSON *send_request(const char *request,
                           const nuwax_mcp_device_status_t *status,
                           const nuwax_mcp_callbacks_t *callbacks) {
    nuwax_ws_client_t ws = {0};
    s_last_payload[0] = '\0';
    assert(nuwax_mcp_handle_message(&ws, request, status, callbacks) == ESP_OK);
    assert(s_last_payload[0] != '\0');
    cJSON *response = cJSON_Parse(s_last_payload);
    assert(response);
    return response;
}

static cJSON *response_result(cJSON *response) {
    cJSON *payload = cJSON_GetObjectItemCaseSensitive(response, "payload");
    assert(cJSON_IsObject(payload));
    cJSON *result = cJSON_GetObjectItemCaseSensitive(payload, "result");
    assert(cJSON_IsObject(result));
    return result;
}

static int response_error_code(cJSON *response) {
    cJSON *payload = cJSON_GetObjectItemCaseSensitive(response, "payload");
    assert(cJSON_IsObject(payload));
    cJSON *error = cJSON_GetObjectItemCaseSensitive(payload, "error");
    assert(cJSON_IsObject(error));
    cJSON *code = cJSON_GetObjectItemCaseSensitive(error, "code");
    assert(cJSON_IsNumber(code));
    return code->valueint;
}

static bool tools_contain(cJSON *response, const char *expected_name) {
    cJSON *tools = cJSON_GetObjectItemCaseSensitive(response_result(response),
                                                    "tools");
    assert(cJSON_IsArray(tools));
    cJSON *tool = NULL;
    cJSON_ArrayForEach(tool, tools) {
        cJSON *name = cJSON_GetObjectItemCaseSensitive(tool, "name");
        if (cJSON_IsString(name) &&
            strcmp(name->valuestring, expected_name) == 0) {
            return true;
        }
    }
    return false;
}

static int tool_count(cJSON *response) {
    cJSON *tools = cJSON_GetObjectItemCaseSensitive(response_result(response),
                                                    "tools");
    assert(cJSON_IsArray(tools));
    return cJSON_GetArraySize(tools);
}

int main(void) {
    const nuwax_mcp_device_status_t status = {
        .state = "idle",
        .ssid = "host-check",
        .ip = "127.0.0.1",
        .free_heap = 1024U,
        .min_free_heap = 512U,
    };

    assert(nuwax_mcp_available_tool_mask(NULL, NULL) == 0U);
    assert(nuwax_mcp_available_tool_mask(&status, NULL) ==
           NUWAX_MCP_AVAILABLE_DEVICE_STATUS);

    nuwax_mcp_callbacks_t callbacks = {
        .set_emotion = accept_emotion,
        .servo_head_shake = shake_servo,
        .ctx = NULL,
        .capability_flags = NUWAX_MCP_CALLBACK_CAP_NONE,
    };
    uint32_t available = nuwax_mcp_available_tool_mask(&status, &callbacks);
    assert((available & NUWAX_MCP_AVAILABLE_DEVICE_STATUS) != 0U);
    assert((available & NUWAX_MCP_AVAILABLE_SET_EMOTION) != 0U);
    assert((available & NUWAX_MCP_AVAILABLE_SERVO_HEAD_SHAKE) == 0U);

    cJSON *response = send_request(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}",
        &status,
        &callbacks);
    assert(tool_count(response) == 2);
    assert(tools_contain(response, "self.get_device_status"));
    assert(tools_contain(response, "self.ui.set_emotion"));
    assert(!tools_contain(response, "self.servo.head_shake"));
    cJSON_Delete(response);

    response = send_request(
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"self.servo.head_shake\","
        "\"arguments\":{\"times\":2}}}",
        &status,
        &callbacks);
    cJSON *result = response_result(response);
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "isError")));
    assert(s_servo_calls == 0U);
    cJSON_Delete(response);

    callbacks.set_emotion = NULL;
    callbacks.capability_flags = NUWAX_MCP_CALLBACK_CAP_SERVO_HEAD_SHAKE;
    available = nuwax_mcp_available_tool_mask(NULL, &callbacks);
    assert(available == NUWAX_MCP_AVAILABLE_SERVO_HEAD_SHAKE);

    response = send_request(
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/list\"}",
        NULL,
        &callbacks);
    assert(tool_count(response) == 1);
    assert(tools_contain(response, "self.servo.head_shake"));
    cJSON_Delete(response);

    response = send_request(
        "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"self.servo.head_shake\","
        "\"arguments\":{\"times\":2}}}",
        NULL,
        &callbacks);
    result = response_result(response);
    assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(result, "isError")));
    assert(s_servo_calls == 1U);
    cJSON_Delete(response);

    callbacks.servo_head_shake = NULL;
    available = nuwax_mcp_available_tool_mask(NULL, &callbacks);
    assert(available == 0U);

    callbacks.set_emotion = accept_emotion;
    callbacks.capability_flags = NUWAX_MCP_CALLBACK_CAP_NONE;
    response = send_request(
        "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"self.ui.set_emotion\","
        "\"arguments\":{\"emotion\":\"happy\"}}}",
        NULL,
        &callbacks);
    result = response_result(response);
    assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(result, "isError")));
    assert(s_emotion_calls == 1U);
    cJSON_Delete(response);

    response = send_request(
        "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\"," 
        "\"method\":\"tools/list\",\"params\":{\"name\":" 
        "\"self.ui.set_emotion\",\"arguments\":{\"emotion\":\"sad\"}}}",
        NULL,
        &callbacks);
    assert(response_error_code(response) == -32600);
    assert(s_emotion_calls == 1U);
    cJSON_Delete(response);

    response = send_request(
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\"," 
        "\"params\":{\"name\":\"self.ui.set_emotion\",\"arguments\":" 
        "{\"emotion\":\"sad\",\"emotion\":\"happy\"}}}",
        NULL,
        &callbacks);
    result = response_result(response);
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "isError")));
    assert(s_emotion_calls == 1U);
    cJSON_Delete(response);

    response = send_request(
        "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"tools/list\"} trailing",
        NULL,
        &callbacks);
    assert(response_error_code(response) == -32700);
    cJSON_Delete(response);

    puts("MCP capability policy host check: PASS");
    return 0;
}
