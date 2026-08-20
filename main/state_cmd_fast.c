/*
 * Retired command-dispatch prototype.
 *
 * Product device commands are parsed by session_protocol.c and executed by
 * conversation/session_dispatch.c with acknowledgement handling. This file
 * remains outside the product build, and all legacy entry points fail closed
 * so they cannot acknowledge or log a command that was never applied.
 */
#include <stdint.h>

#include "esp_log.h"

typedef enum {
    CMD_PRIO_HIGH = 0,
    CMD_PRIO_NORMAL = 1,
} cmd_prio_t;

static const char *TAG = "state_cmd_legacy";

int state_cmd_init(void)
{
    ESP_LOGE(TAG, "retired prototype rejected; use session_dispatch");
    return -1;
}

int state_cmd_push_volume(int volume, cmd_prio_t priority)
{
    (void)volume;
    (void)priority;
    return -1;
}

int state_cmd_push_emotion(int emotion, cmd_prio_t priority)
{
    (void)emotion;
    (void)priority;
    return -1;
}

int state_cmd_push_led(int led, cmd_prio_t priority)
{
    (void)led;
    (void)priority;
    return -1;
}

int state_cmd_process_one(void)
{
    return 0;
}

int state_cmd_handle_json(const char *json, int len)
{
    (void)json;
    (void)len;
    return -1;
}
