#pragma once

typedef enum {
    NUWAX_APP_STATE_BOOTING = 0,
    NUWAX_APP_STATE_WIFI_CONNECTING,
    NUWAX_APP_STATE_WS_CONNECTING,
    NUWAX_APP_STATE_IDLE,
    NUWAX_APP_STATE_SLEEPING,
    NUWAX_APP_STATE_LISTENING,
    NUWAX_APP_STATE_THINKING,
    NUWAX_APP_STATE_SPEAKING,
    NUWAX_APP_STATE_ERROR,
} nuwax_app_state_t;

const char *nuwax_app_state_name(nuwax_app_state_t state);
