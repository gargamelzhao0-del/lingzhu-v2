#include <stdio.h>
#include <string.h>

#include "nuwax_ui_v28/nuwax_dialog_policy.h"

static int expect_dialog(const char *name,
                         const nuwax_device_state_t *state,
                         nuwax_dialog_state_t expected) {
    const nuwax_dialog_state_t actual = nuwax_dialog_policy_resolve(state);
    if (actual == expected) {
        printf("[PASS] %s dialog=%d\n", name, (int)actual);
        return 0;
    }
    printf("[FAIL] %s dialog=%d expected=%d\n",
           name,
           (int)actual,
           (int)expected);
    return 1;
}

int main(void) {
    int failures = 0;
    nuwax_device_state_t state;
    memset(&state, 0, sizeof(state));
    state.dialog = NUWAX_DIALOG_SLEEP;
    failures += expect_dialog("sleep without exclusive mode",
                              &state,
                              NUWAX_DIALOG_SLEEP);

    state.monitoring = NUWAX_MONITORING_UI_STARTING;
    failures += expect_dialog("monitoring starting overrides moon",
                              &state,
                              NUWAX_DIALOG_LISTENING);
    state.monitoring = NUWAX_MONITORING_UI_ACTIVE;
    failures += expect_dialog("monitoring active overrides moon",
                              &state,
                              NUWAX_DIALOG_LISTENING);
    state.dialog = NUWAX_DIALOG_RECOGNIZING;
    state.cloud = NUWAX_CLOUD_CONNECTING;
    failures += expect_dialog("cloud recovery overrides monitoring",
                              &state,
                              NUWAX_DIALOG_RECOGNIZING);
    state.cloud = NUWAX_CLOUD_CONNECTED;
    state.dialog = NUWAX_DIALOG_SLEEP;
    state.monitoring = NUWAX_MONITORING_UI_STOPPING;
    failures += expect_dialog("monitoring stopping overrides moon",
                              &state,
                              NUWAX_DIALOG_LISTENING);
    state.monitoring = NUWAX_MONITORING_UI_ERROR;
    failures += expect_dialog("monitoring error is visible",
                              &state,
                              NUWAX_DIALOG_ERROR);

    state.monitoring = NUWAX_MONITORING_UI_DISABLED;
    state.meeting = NUWAX_MEETING_UI_ACTIVE;
    failures += expect_dialog("meeting active overrides moon",
                              &state,
                              NUWAX_DIALOG_LISTENING);
    state.dialog = NUWAX_DIALOG_RECOGNIZING;
    state.cloud = NUWAX_CLOUD_DISCONNECTED;
    failures += expect_dialog("cloud outage overrides meeting",
                              &state,
                              NUWAX_DIALOG_RECOGNIZING);

    state.meeting = NUWAX_MEETING_UI_DISABLED;
    state.cloud = NUWAX_CLOUD_CONNECTED;
    state.dialog = NUWAX_DIALOG_READY;
    failures += expect_dialog("idle remains ready",
                              &state,
                              NUWAX_DIALOG_READY);
    failures += expect_dialog("null state fails closed",
                              NULL,
                              NUWAX_DIALOG_ERROR);

    printf("dialog state policy host check: %s (%d failures)\n",
           failures == 0 ? "PASS" : "FAIL",
           failures);
    return failures == 0 ? 0 : 1;
}
