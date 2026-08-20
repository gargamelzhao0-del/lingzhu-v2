#pragma once

#include "nuwax_device_state.h"

static inline nuwax_dialog_state_t nuwax_dialog_policy_resolve(
    const nuwax_device_state_t *state) {
    if (!state) {
        return NUWAX_DIALOG_ERROR;
    }
    if (state->meeting == NUWAX_MEETING_UI_ERROR ||
        state->monitoring == NUWAX_MONITORING_UI_ERROR) {
        return NUWAX_DIALOG_ERROR;
    }
    /* Connection recovery is a device-wide lifecycle state. Do not let an
     * exclusive audio mode keep the bottom indicator on LISTENING while the
     * app state bridge is explicitly reporting network recognition/recovery. */
    if (state->dialog == NUWAX_DIALOG_RECOGNIZING &&
        state->cloud != NUWAX_CLOUD_CONNECTED) {
        return NUWAX_DIALOG_RECOGNIZING;
    }
    if (state->meeting != NUWAX_MEETING_UI_DISABLED ||
        state->monitoring != NUWAX_MONITORING_UI_DISABLED) {
        return NUWAX_DIALOG_LISTENING;
    }
    return state->dialog;
}
