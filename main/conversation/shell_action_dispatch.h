#pragma once

#include <stdbool.h>

#include "app_shell.h"
#include "ws_transport.h"

typedef struct {
    void *ctx;
    void (*clear_auto_followup)(void *ctx, const char *reason);
    void (*request_wifi_scan)(void *ctx);
    void (*refresh_saved_wifi_list_ui)(void *ctx);
    void (*request_pc_status)(void *ctx, nuwax_ws_client_t *ws);
    void (*request_status_app)(void *ctx, nuwax_ws_client_t *ws, const char *app);
    void (*request_cancel_turn)(void *ctx, nuwax_ws_client_t *ws, const char *reason);
    void (*request_head_touch_agent)(void *ctx, nuwax_ws_client_t *ws);
    void (*request_shake_agent)(void *ctx, nuwax_ws_client_t *ws);
    void (*apply_volume_percent)(void *ctx, int volume_percent);
    void (*apply_volume_commit_percent)(void *ctx, int volume_percent, uint32_t sequence);
    void (*cycle_brightness)(void *ctx);
    void (*set_meeting_mode)(void *ctx, nuwax_ws_client_t *ws, bool enabled);
    void (*set_simulated_offline)(void *ctx, nuwax_ws_client_t *ws, bool enabled);
    void (*select_wifi)(void *ctx, int index);
    void (*connect_saved_wifi)(void *ctx, int index);
    void (*set_reprovisioning)(void *ctx, bool enabled);
} nuwax_shell_action_dispatch_callbacks_t;

bool nuwax_shell_action_is_listen_control(nuwax_app_shell_action_t action);

void nuwax_shell_action_dispatch(nuwax_ws_client_t *ws,
                                 const nuwax_app_shell_result_t *action,
                                 const nuwax_shell_action_dispatch_callbacks_t *callbacks);
