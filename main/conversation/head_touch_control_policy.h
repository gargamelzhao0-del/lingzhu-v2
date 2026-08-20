#pragma once

#include <stdbool.h>

#include "app_state.h"

typedef enum {
    NUWAX_HEAD_TOUCH_ACTION_NONE = 0,
    NUWAX_HEAD_TOUCH_ACTION_START_LISTEN,
    NUWAX_HEAD_TOUCH_ACTION_CANCEL_TURN,
    NUWAX_HEAD_TOUCH_ACTION_AGENT_TOUCH,
    NUWAX_HEAD_TOUCH_ACTION_VISUAL_ONLY,
} nuwax_head_touch_action_t;

nuwax_head_touch_action_t nuwax_head_touch_action_for_state(
    nuwax_app_state_t state,
    bool interactions_allowed,
    bool wake_enabled,
    bool interrupt_enabled);
