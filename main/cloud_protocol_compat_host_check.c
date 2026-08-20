#include "cloud_protocol_compat.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *const DEVICE_ID = "NX-FCBB-6906-0438";

static void expect_kind(const char *json, nuwax_cloud_message_kind_t expected)
{
    assert(nuwax_cloud_classify_message(
               json, (int)strlen(json), DEVICE_ID) == expected);
}

int main(void)
{
    expect_kind(
        "{\"type\":\"hello\",\"ok\":true,\"authenticated\":true,"
        "\"device_id\":\"NX-FCBB-6906-0438\"}",
        NUWAX_CLOUD_MESSAGE_HELLO_OK);
    expect_kind(
        "  {\"type\":\"hello\",\"ok\":true,\"session_id\":\"formal-1\"} \r\n",
        NUWAX_CLOUD_MESSAGE_HELLO_OK);
    expect_kind(
        "{\"type\":\"hello\",\"ok\":true,\"authenticated\":false}",
        NUWAX_CLOUD_MESSAGE_HELLO_REJECTED);
    expect_kind(
        "{\"type\":\"hello\",\"ok\":true,\"authenticated\":true}",
        NUWAX_CLOUD_MESSAGE_HELLO_REJECTED);
    expect_kind(
        "{\"type\":\"hello\",\"ok\":true,\"authenticated\":true,"
        "\"device_id\":\"NX-WRONG\"}",
        NUWAX_CLOUD_MESSAGE_HELLO_REJECTED);
    expect_kind(
        "{\"type\":\"hello\",\"ok\":true,\"device_id\":\"NX-WRONG\"}",
        NUWAX_CLOUD_MESSAGE_HELLO_REJECTED);
    expect_kind(
        "{\"type\":\"hello\",\"ok\":true,\"authenticated\":true,"
        "\"device_id\":\"NX-FCBB-6906-0438\","
        "\"device_id\":\"NX-FCBB-6906-0438\"}",
        NUWAX_CLOUD_MESSAGE_HELLO_REJECTED);
    expect_kind("{\"type\":\"hello\",\"ok\":false}",
                NUWAX_CLOUD_MESSAGE_HELLO_REJECTED);
    expect_kind("{\"type\":\"hello\",\"ok\":true,\"ok\":true}",
                NUWAX_CLOUD_MESSAGE_HELLO_REJECTED);
    expect_kind("{\"type\":\"pong\",\"now\":123}",
                NUWAX_CLOUD_MESSAGE_PONG);
    expect_kind(
        "{\"type\":\"device_command\",\"command\":\"set_volume\","
        "\"request_id\":\"cmd_1\",\"volume_percent\":50}",
        NUWAX_CLOUD_MESSAGE_DEVICE_COMMAND);
    expect_kind(
        "{\"command\":\"set_wake_word\",\"wake_word\":["
        "\"ni hao ling zhu\",\"ni hao lin zu\","
        "\"ni hao ling zu\",\"ni hao lin zhu\"],\"persist\":true}",
        NUWAX_CLOUD_MESSAGE_DEVICE_COMMAND);
    expect_kind(
        "{\"command\":\"set_volume\",\"volume_percent\":50}",
        NUWAX_CLOUD_MESSAGE_NONE);
    expect_kind(
        "{\"command\":\"set_wake_word\",\"request_id\":\"wake-half\","
        "\"wake_word\":[\"ni hao ling zhu\"]}",
        NUWAX_CLOUD_MESSAGE_NONE);
    expect_kind("{\"type\":\"device_command\"}",
                NUWAX_CLOUD_MESSAGE_NONE);
    expect_kind(
        "{\"type\":\"device_command\",\"command\":\"set_volume\"}",
        NUWAX_CLOUD_MESSAGE_NONE);
    expect_kind(
        "{\"type\":\"device_command\",\"command\":\"\","
        "\"request_id\":\"cmd_1\"}",
        NUWAX_CLOUD_MESSAGE_NONE);
    expect_kind(
        "{\"type\":\"device_command\",\"command\":\"set_volume\","
        "\"command\":\"ota_update\",\"request_id\":\"cmd_1\"}",
        NUWAX_CLOUD_MESSAGE_NONE);
    expect_kind("{\"type\":\"error\",\"error\":\"DEVICE_AUTH_FAILED\"}",
                NUWAX_CLOUD_MESSAGE_AUTH_REJECTED);
    expect_kind("{\"type\":\"error\",\"code\":\"DEVICE_SECRET_INVALID\"}",
                NUWAX_CLOUD_MESSAGE_AUTH_REJECTED);
    expect_kind("{\"type\":\"error\",\"code\":\"DEVICE_NOT_BOUND\"}",
                NUWAX_CLOUD_MESSAGE_PROVISION_REQUIRED);
    expect_kind(
        "{\"type\":\"error\",\"code\":\"DEVICE_NOT_BOUND\","
        "\"error\":\"invalid device secret\"}",
        NUWAX_CLOUD_MESSAGE_PROVISION_REQUIRED);
    expect_kind("{\"type\":\"error\",\"error\":\"DEVICE_NOT_BOUND\"}",
                NUWAX_CLOUD_MESSAGE_NONE);
    expect_kind(
        "{\"type\":\"error\",\"code\":\"DEVICE_NOT_BOUND\","
        "\"code\":\"DEVICE_NOT_BOUND\"}",
        NUWAX_CLOUD_MESSAGE_NONE);
    expect_kind(
        "{\"type\":\"error\",\"code\":\"UPSTREAM_ERROR\","
        "\"message\":\"DEVICE_NOT_BOUND\"}",
        NUWAX_CLOUD_MESSAGE_NONE);
    expect_kind("{\"type\":\"error\",\"error\":\"TTS_FAILED\"}",
                NUWAX_CLOUD_MESSAGE_NONE);
    expect_kind("{\"type\":\"unknown\"}",
                NUWAX_CLOUD_MESSAGE_NONE);
    expect_kind("{\"type\":\"pong\"} trailing",
                NUWAX_CLOUD_MESSAGE_NONE);
    expect_kind("not-json", NUWAX_CLOUD_MESSAGE_NONE);

    assert(nuwax_cloud_classify_message(NULL, 0, DEVICE_ID) ==
           NUWAX_CLOUD_MESSAGE_NONE);
    puts("cloud_protocol_compat_host_check: PASS");
    return 0;
}
