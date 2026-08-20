/* Host-only logic check. This file is excluded from firmware CMake sources. */

#include <stdbool.h>
#include <stdio.h>

#include "conversation/head_touch_control_policy.h"

static int expect_action(const char *name,
                         nuwax_app_state_t state,
                         bool interactions_allowed,
                         bool wake_enabled,
                         bool interrupt_enabled,
                         nuwax_head_touch_action_t expected)
{
    const nuwax_head_touch_action_t actual =
        nuwax_head_touch_action_for_state(state, interactions_allowed,
                                          wake_enabled, interrupt_enabled);
    const bool passed = actual == expected;
    printf("[%s] %s state=%s allowed=%d action=%d\n",
           passed ? "PASS" : "FAIL",
           name,
           nuwax_app_state_name(state),
           interactions_allowed ? 1 : 0,
           (int)actual);
    return passed ? 0 : 1;
}

int main(void)
{
    int failures = 0;
    failures += expect_action("sleep_wakes", NUWAX_APP_STATE_SLEEPING, true,
                              true, true,
                              NUWAX_HEAD_TOUCH_ACTION_START_LISTEN);
    failures += expect_action("idle_listens", NUWAX_APP_STATE_IDLE, true,
                              true, true,
                              NUWAX_HEAD_TOUCH_ACTION_START_LISTEN);
    failures += expect_action("thinking_starts_agent_turn", NUWAX_APP_STATE_THINKING,
                              true, true, true,
                              NUWAX_HEAD_TOUCH_ACTION_AGENT_TOUCH);
    failures += expect_action("speaking_cancels", NUWAX_APP_STATE_SPEAKING,
                              true, true, true,
                              NUWAX_HEAD_TOUCH_ACTION_AGENT_TOUCH);
    failures += expect_action("sleep_wake_disabled",
                              NUWAX_APP_STATE_SLEEPING, true, false, true,
                              NUWAX_HEAD_TOUCH_ACTION_NONE);
    failures += expect_action("interrupt_only_thinking",
                              NUWAX_APP_STATE_THINKING, true, false, true,
                              NUWAX_HEAD_TOUCH_ACTION_AGENT_TOUCH);
    failures += expect_action("interrupt_only_speaking",
                              NUWAX_APP_STATE_SPEAKING, true, false, true,
                              NUWAX_HEAD_TOUCH_ACTION_AGENT_TOUCH);
    failures += expect_action("speaking_interrupt_disabled",
                              NUWAX_APP_STATE_SPEAKING, true, true, false,
                              NUWAX_HEAD_TOUCH_ACTION_NONE);
    failures += expect_action("listening_requests_agent_touch",
                              NUWAX_APP_STATE_LISTENING, true, true, true,
                              NUWAX_HEAD_TOUCH_ACTION_AGENT_TOUCH);
    failures += expect_action("menu_blocks_wake", NUWAX_APP_STATE_SLEEPING,
                              false, true, true,
                              NUWAX_HEAD_TOUCH_ACTION_NONE);
    failures += expect_action("exclusive_mode_blocks_cancel",
                              NUWAX_APP_STATE_SPEAKING, false, true, true,
                              NUWAX_HEAD_TOUCH_ACTION_NONE);
    failures += expect_action("boot_is_inert", NUWAX_APP_STATE_BOOTING, true,
                              true, true,
                              NUWAX_HEAD_TOUCH_ACTION_NONE);
    failures += expect_action("offline_is_inert",
                              NUWAX_APP_STATE_WS_CONNECTING, true, true, true,
                              NUWAX_HEAD_TOUCH_ACTION_NONE);
    failures += expect_action("error_is_inert", NUWAX_APP_STATE_ERROR, true,
                              true, true,
                              NUWAX_HEAD_TOUCH_ACTION_NONE);

    printf("head touch policy host check: %s (%d failures)\n",
           failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
