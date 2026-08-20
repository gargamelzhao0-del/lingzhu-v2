#include "app_state.h"

const char *nuwax_app_state_name(nuwax_app_state_t state) {
    switch (state) {
        case NUWAX_APP_STATE_BOOTING:
            return "booting";
        case NUWAX_APP_STATE_WIFI_CONNECTING:
            return "wifi_connecting";
        case NUWAX_APP_STATE_WS_CONNECTING:
            return "ws_connecting";
        case NUWAX_APP_STATE_IDLE:
            return "idle";
        case NUWAX_APP_STATE_SLEEPING:
            return "sleeping";
        case NUWAX_APP_STATE_LISTENING:
            return "listening";
        case NUWAX_APP_STATE_THINKING:
            return "thinking";
        case NUWAX_APP_STATE_SPEAKING:
            return "speaking";
        case NUWAX_APP_STATE_ERROR:
            return "error";
        default:
            return "unknown";
    }
}
