#include "conversation/head_touch_control_policy.h"

nuwax_head_touch_action_t nuwax_head_touch_action_for_state(
    nuwax_app_state_t state,
    bool interactions_allowed,
    bool wake_enabled,
    bool interrupt_enabled)
{
    if (!interactions_allowed) {
        return NUWAX_HEAD_TOUCH_ACTION_NONE;
    }

    switch (state) {
        case NUWAX_APP_STATE_IDLE:
        case NUWAX_APP_STATE_SLEEPING:
            return wake_enabled ? NUWAX_HEAD_TOUCH_ACTION_START_LISTEN
                                : NUWAX_HEAD_TOUCH_ACTION_NONE;
        case NUWAX_APP_STATE_THINKING:
        case NUWAX_APP_STATE_SPEAKING:
        case NUWAX_APP_STATE_LISTENING:
            return interrupt_enabled ? NUWAX_HEAD_TOUCH_ACTION_AGENT_TOUCH
                                     : NUWAX_HEAD_TOUCH_ACTION_NONE;
        case NUWAX_APP_STATE_BOOTING:
        case NUWAX_APP_STATE_WIFI_CONNECTING:
        case NUWAX_APP_STATE_WS_CONNECTING:
        case NUWAX_APP_STATE_ERROR:
        default:
            return NUWAX_HEAD_TOUCH_ACTION_NONE;
    }
}
