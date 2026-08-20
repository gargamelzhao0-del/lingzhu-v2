#include "nuwax_device_state.h"
#include <string.h>

#define MAX_LISTENERS 8

static nuwax_device_state_t s_state;
static struct {
    nuwax_state_listener_t cb;
    void *user_data;
} s_listeners[MAX_LISTENERS];
static int s_listener_count;

void nuwax_device_state_init(void) {
    memset(&s_state, 0, sizeof(s_state));
    memset(s_listeners, 0, sizeof(s_listeners));
    s_state.emotion = NUWAX_EMOTION_NEUTRAL;
    s_state.wifi = NUWAX_WIFI_DISCONNECTED;
    s_state.cloud = NUWAX_CLOUD_DISCONNECTED;
    s_state.dialog = NUWAX_DIALOG_RECOGNIZING;
    s_state.power = NUWAX_POWER_UNKNOWN;
    s_state.meeting = NUWAX_MEETING_UI_DISABLED;
    s_state.monitoring = NUWAX_MONITORING_UI_DISABLED;
    s_state.volume = 80;
    s_state.wifi_rssi_dbm = -127;
    s_state.wifi_signal = NUWAX_WIFI_SIGNAL_UNKNOWN;
    s_listener_count = 0;
}

const nuwax_device_state_t *nuwax_device_state_get(void) {
    return &s_state;
}

void nuwax_device_state_set(const nuwax_device_state_t *state) {
    if (state) {
        if (memcmp(&s_state, state, sizeof(s_state)) == 0) {
            return;
        }
        s_state = *state;
        nuwax_device_state_notify();
    }
}

void nuwax_device_state_set_emotion(nuwax_emotion_t emotion) {
    if (s_state.emotion == emotion) return;
    s_state.emotion = emotion;
    nuwax_device_state_notify();
}

void nuwax_device_state_set_volume(uint8_t volume) {
    uint8_t next = volume > 100 ? 100 : volume;
    if (s_state.volume == next) return;
    s_state.volume = next;
    nuwax_device_state_notify();
}

void nuwax_device_state_set_low_power(bool enabled) {
    if (s_state.app_low_power == enabled) return;
    s_state.app_low_power = enabled;
    s_state.low_power = s_state.app_low_power;
    nuwax_device_state_notify();
}

void nuwax_device_state_set_wifi_signal(bool valid, int8_t rssi_dbm) {
    nuwax_device_state_t next = s_state;
    next.wifi_signal_valid = valid;
    next.wifi_rssi_dbm = valid ? rssi_dbm : -127;
    if (!valid) {
        next.wifi_signal = NUWAX_WIFI_SIGNAL_UNKNOWN;
    } else if (rssi_dbm >= -55) {
        next.wifi_signal = NUWAX_WIFI_SIGNAL_STRONG;
    } else if (rssi_dbm >= -67) {
        next.wifi_signal = NUWAX_WIFI_SIGNAL_FAIR;
    } else {
        next.wifi_signal = NUWAX_WIFI_SIGNAL_WEAK;
    }
    nuwax_device_state_set(&next);
}

void nuwax_device_state_set_battery(bool valid,
                                    bool soc_trusted,
                                    nuwax_power_state_t power,
                                    uint8_t percent,
                                    uint16_t voltage_mv,
                                    int16_t current_ma) {
    if ((unsigned)power > (unsigned)NUWAX_POWER_LOW) {
        power = NUWAX_POWER_UNKNOWN;
        valid = false;
    }
    if (!valid) power = NUWAX_POWER_UNKNOWN;

    nuwax_device_state_t next = s_state;
    next.battery_valid = valid;
    next.battery_soc_trusted = valid && soc_trusted;
    next.power = power;
    next.battery_low = valid && power == NUWAX_POWER_LOW;
    next.low_power = next.app_low_power;
    if (valid) {
        next.battery_percent = percent > 100 ? 100 : percent;
        next.battery_voltage_mv = voltage_mv;
        next.battery_current_ma = current_ma;
    }
    nuwax_device_state_set(&next);
}

bool nuwax_device_state_add_listener(nuwax_state_listener_t listener, void *user_data) {
    if (!listener) return false;
    for (int i = 0; i < s_listener_count; i++) {
        if (s_listeners[i].cb == listener &&
            s_listeners[i].user_data == user_data) {
            return false;
        }
    }
    if (s_listener_count >= MAX_LISTENERS) return false;
    s_listeners[s_listener_count].cb = listener;
    s_listeners[s_listener_count].user_data = user_data;
    s_listener_count++;
    return true;
}

bool nuwax_device_state_remove_listener(nuwax_state_listener_t listener, void *user_data) {
    if (!listener) return false;
    for (int i = 0; i < s_listener_count; i++) {
        if (s_listeners[i].cb != listener ||
            s_listeners[i].user_data != user_data) {
            continue;
        }
        for (int j = i + 1; j < s_listener_count; j++) {
            s_listeners[j - 1] = s_listeners[j];
        }
        s_listener_count--;
        memset(&s_listeners[s_listener_count], 0, sizeof(s_listeners[0]));
        return true;
    }
    return false;
}

void nuwax_device_state_notify(void) {
    for (int i = 0; i < s_listener_count; i++) {
        if (s_listeners[i].cb) {
            s_listeners[i].cb(&s_state, s_listeners[i].user_data);
        }
    }
}
