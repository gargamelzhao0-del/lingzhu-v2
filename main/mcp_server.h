#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "ws_transport.h"

typedef struct {
    const char *state;
    const char *ssid;
    const char *ip;
    uint32_t free_heap;
    uint32_t min_free_heap;
} nuwax_mcp_device_status_t;

typedef bool (*nuwax_mcp_set_emotion_fn_t)(const char *emotion, void *ctx);
typedef bool (*nuwax_mcp_servo_head_shake_fn_t)(int times, void *ctx);

typedef enum {
    NUWAX_MCP_CALLBACK_CAP_NONE = 0,
    /* A non-NULL callback alone is insufficient because legacy firmware
     * supplies a placeholder that always returns false. Set this bit only
     * when servo hardware and its executable driver are both present. */
    NUWAX_MCP_CALLBACK_CAP_SERVO_HEAD_SHAKE = 1U << 0,
} nuwax_mcp_callback_capability_t;

typedef struct {
    nuwax_mcp_set_emotion_fn_t set_emotion;
    nuwax_mcp_servo_head_shake_fn_t servo_head_shake;
    void *ctx;
    uint32_t capability_flags;
} nuwax_mcp_callbacks_t;

typedef enum {
    NUWAX_MCP_AVAILABLE_DEVICE_STATUS = 1U << 0,
    NUWAX_MCP_AVAILABLE_SET_EMOTION = 1U << 1,
    NUWAX_MCP_AVAILABLE_SERVO_HEAD_SHAKE = 1U << 2,
} nuwax_mcp_available_tool_t;

/* Pure capability policy shared by tools/list and tools/call. Keeping one
 * policy prevents a hidden or placeholder callback from being invoked through
 * a direct tools/call request. */
static inline uint32_t nuwax_mcp_available_tool_mask(
    const nuwax_mcp_device_status_t *status,
    const nuwax_mcp_callbacks_t *callbacks) {
    uint32_t available = 0;
    if (status) {
        available |= NUWAX_MCP_AVAILABLE_DEVICE_STATUS;
    }
    if (callbacks && callbacks->set_emotion) {
        available |= NUWAX_MCP_AVAILABLE_SET_EMOTION;
    }
    if (callbacks && callbacks->servo_head_shake &&
        (callbacks->capability_flags &
         NUWAX_MCP_CALLBACK_CAP_SERVO_HEAD_SHAKE) != 0U) {
        available |= NUWAX_MCP_AVAILABLE_SERVO_HEAD_SHAKE;
    }
    return available;
}

esp_err_t nuwax_mcp_handle_message(nuwax_ws_client_t *ws,
                                   const char *payload_json,
                                   const nuwax_mcp_device_status_t *status,
                                   const nuwax_mcp_callbacks_t *callbacks);
