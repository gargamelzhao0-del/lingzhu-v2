#include "shell_action_dispatch.h"

bool nuwax_shell_action_is_listen_control(nuwax_app_shell_action_t action) {
    return action == NUWAX_APP_SHELL_ACTION_START_LISTEN ||
           action == NUWAX_APP_SHELL_ACTION_STOP_LISTEN;
}

static void clear_followup_for_shell_action(const nuwax_app_shell_result_t *action,
                                            const nuwax_shell_action_dispatch_callbacks_t *callbacks) {
    if (!action || !callbacks || !callbacks->clear_auto_followup) {
        return;
    }
    if (action->action != NUWAX_APP_SHELL_ACTION_NONE &&
        action->action != NUWAX_APP_SHELL_ACTION_START_LISTEN &&
        action->action != NUWAX_APP_SHELL_ACTION_CANCEL &&
        action->action != NUWAX_APP_SHELL_ACTION_HEAD_TOUCH_AGENT &&
        action->action != NUWAX_APP_SHELL_ACTION_SHAKE_AGENT) {
        callbacks->clear_auto_followup(callbacks->ctx, "shell_action");
    }
}

void nuwax_shell_action_dispatch(nuwax_ws_client_t *ws,
                                 const nuwax_app_shell_result_t *action,
                                 const nuwax_shell_action_dispatch_callbacks_t *callbacks) {
    if (!action) {
        return;
    }

    clear_followup_for_shell_action(action, callbacks);

    switch (action->action) {
        case NUWAX_APP_SHELL_ACTION_SCAN_WIFI:
            if (callbacks && callbacks->request_wifi_scan) {
                callbacks->request_wifi_scan(callbacks->ctx);
            }
            break;
        case NUWAX_APP_SHELL_ACTION_SHOW_SAVED_WIFI:
            if (callbacks && callbacks->refresh_saved_wifi_list_ui) {
                callbacks->refresh_saved_wifi_list_ui(callbacks->ctx);
            }
            break;
        case NUWAX_APP_SHELL_ACTION_REFRESH_PC_STATUS:
            if (callbacks && callbacks->request_pc_status) {
                callbacks->request_pc_status(callbacks->ctx, ws);
            }
            break;
        case NUWAX_APP_SHELL_ACTION_REFRESH_WORK_STATUS:
            if (callbacks && callbacks->request_status_app) {
                callbacks->request_status_app(callbacks->ctx, ws, "work");
            }
            break;
        case NUWAX_APP_SHELL_ACTION_REFRESH_ISSUES_STATUS:
            if (callbacks && callbacks->request_status_app) {
                callbacks->request_status_app(callbacks->ctx, ws, "issues");
            }
            break;
        case NUWAX_APP_SHELL_ACTION_CANCEL:
            if (callbacks && callbacks->request_cancel_turn) {
                callbacks->request_cancel_turn(callbacks->ctx, ws, "touch_cancel");
            }
            break;
        case NUWAX_APP_SHELL_ACTION_HEAD_TOUCH_AGENT:
            if (callbacks && callbacks->request_head_touch_agent) {
                callbacks->request_head_touch_agent(callbacks->ctx, ws);
            }
            break;
        case NUWAX_APP_SHELL_ACTION_SHAKE_AGENT:
            if (callbacks && callbacks->request_shake_agent) {
                callbacks->request_shake_agent(callbacks->ctx, ws);
            }
            break;
        case NUWAX_APP_SHELL_ACTION_SET_VOLUME:
            if (callbacks && callbacks->apply_volume_percent) {
                callbacks->apply_volume_percent(callbacks->ctx, action->index);
            }
            break;
        case NUWAX_APP_SHELL_ACTION_SET_MEETING_MODE:
            if (callbacks && callbacks->set_meeting_mode) {
                callbacks->set_meeting_mode(callbacks->ctx, ws, action->index != 0);
            }
            break;
        case NUWAX_APP_SHELL_ACTION_SELECT_WIFI:
            if (callbacks && callbacks->select_wifi) {
                callbacks->select_wifi(callbacks->ctx, action->index);
            }
            break;
        case NUWAX_APP_SHELL_ACTION_CONNECT_SAVED_WIFI:
            if (callbacks && callbacks->connect_saved_wifi) {
                callbacks->connect_saved_wifi(callbacks->ctx, action->index);
            }
            break;
        case NUWAX_APP_SHELL_ACTION_REPROVISION_WIFI:
            if (callbacks && callbacks->set_reprovisioning) {
                callbacks->set_reprovisioning(callbacks->ctx,
                                              action->index != 0);
            }
            break;
        case NUWAX_APP_SHELL_ACTION_NONE:
        case NUWAX_APP_SHELL_ACTION_START_LISTEN:
        case NUWAX_APP_SHELL_ACTION_STOP_LISTEN:
        default:
            break;
    }
}
