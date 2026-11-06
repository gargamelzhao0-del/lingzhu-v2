
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#define CONFIG_NUWAX_TOUCH_INTERRUPT_ACTION_ENABLE 1
#define CONFIG_NUWAX_TOUCH_WAKE_ACTION_ENABLE 0
#define taskENTER_CRITICAL(x) ((void)(x))
#define taskEXIT_CRITICAL(x) ((void)(x))
typedef enum { NUWAX_APP_SHELL_ACTION_NONE, NUWAX_APP_SHELL_ACTION_CANCEL, NUWAX_APP_SHELL_ACTION_START_LISTEN } nuwax_app_shell_action_t;
enum { NUWAX_APP_STATE_SLEEPING, NUWAX_APP_STATE_LISTENING, NUWAX_APP_STATE_THINKING, NUWAX_APP_STATE_SPEAKING };
static int s_ui_mailbox_mux;
static struct { bool app_state_valid; int app_state; } s_state_mailbox;
static void app_shell_ensure_state_snapshot_locked(void) {}
static nuwax_app_shell_action_t app_shell_hold_action(void) {
    nuwax_app_shell_action_t action = NUWAX_APP_SHELL_ACTION_NONE;
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    app_shell_ensure_state_snapshot_locked();
    if (s_state_mailbox.app_state_valid &&
        (s_state_mailbox.app_state == NUWAX_APP_STATE_THINKING ||
          s_state_mailbox.app_state == NUWAX_APP_STATE_SPEAKING)) {
        if (CONFIG_NUWAX_TOUCH_INTERRUPT_ACTION_ENABLE) {
            action = NUWAX_APP_SHELL_ACTION_CANCEL;
        }
    } else if (CONFIG_NUWAX_TOUCH_WAKE_ACTION_ENABLE) {
        action = NUWAX_APP_SHELL_ACTION_START_LISTEN;
    }
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    return action;
}

int main(void) {
 s_state_mailbox.app_state_valid=true;
 s_state_mailbox.app_state=NUWAX_APP_STATE_SPEAKING;
 assert(app_shell_hold_action()==NUWAX_APP_SHELL_ACTION_CANCEL);
 s_state_mailbox.app_state=NUWAX_APP_STATE_LISTENING;
 assert(app_shell_hold_action()==NUWAX_APP_SHELL_ACTION_NONE);
 puts("REPRODUCED: current production touch policy cancels SPEAKING but ignores LISTENING (wake greeting playback state)");
 return 0;
}
