#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    NUWAX_EMOTION_NEUTRAL = 0,
    NUWAX_EMOTION_HAPPY,
    NUWAX_EMOTION_SURPRISED,
    NUWAX_EMOTION_WINK,
    NUWAX_EMOTION_TONGUE,
    NUWAX_EMOTION_THINKING,
    NUWAX_EMOTION_FOCUSED,
    NUWAX_EMOTION_PENSIVE,
    NUWAX_EMOTION_ANALYZING,
    NUWAX_EMOTION_PROCESSING,
    NUWAX_EMOTION_RECEIVED,
    NUWAX_EMOTION_WHAT,
    NUWAX_EMOTION_UNDERSTOOD,
    NUWAX_EMOTION_AWESOME,
    NUWAX_EMOTION_LIKE,
    NUWAX_EMOTION_SAD,
    NUWAX_EMOTION_LOST,
    NUWAX_EMOTION_ANGRY,
    NUWAX_EMOTION_SHY,
    NUWAX_EMOTION_TIRED,
    NUWAX_EMOTION_BOOTING,
    NUWAX_EMOTION_LISTENING,
    NUWAX_EMOTION_SPEAKING,
    NUWAX_EMOTION_LOW_BATTERY,
    NUWAX_EMOTION_CHARGING,
    /* Appended values preserve every deployed emotion numeric value. */
    NUWAX_EMOTION_DIZZINESS,
    NUWAX_EMOTION_EXCITED,
    NUWAX_EMOTION_THOUGHT_COMPLETE,
    NUWAX_EMOTION_SLEEPING_FACE,
    NUWAX_EMOTION_PROVISIONING,
    NUWAX_EMOTION_DISCONNECTED_FACE,
    NUWAX_EMOTION_MEETING_FACE,
    NUWAX_EMOTION_MONITORING_FACE,
    NUWAX_EMOTION_TOUCH,
    NUWAX_EMOTION_PLAYFUL,
    NUWAX_EMOTION_QUESTION,
    NUWAX_EMOTION_COUNT
} nuwax_emotion_t;

typedef enum {
    NUWAX_WIFI_CONNECTED = 0,
    NUWAX_WIFI_CONNECTING,
    NUWAX_WIFI_WEAK,
    NUWAX_WIFI_DISCONNECTED
} nuwax_wifi_state_t;

typedef enum {
    NUWAX_WIFI_SIGNAL_UNKNOWN = 0,
    NUWAX_WIFI_SIGNAL_WEAK,
    NUWAX_WIFI_SIGNAL_FAIR,
    NUWAX_WIFI_SIGNAL_STRONG,
} nuwax_wifi_signal_t;

typedef enum {
    NUWAX_CLOUD_CONNECTED = 0,
    NUWAX_CLOUD_CONNECTING,
    NUWAX_CLOUD_DISCONNECTED,
    NUWAX_CLOUD_AUTHFAIL
} nuwax_cloud_state_t;

typedef enum {
    NUWAX_DIALOG_SLEEP = 0,
    NUWAX_DIALOG_LISTENING,
    NUWAX_DIALOG_RECOGNIZING,
    NUWAX_DIALOG_THINKING,
    NUWAX_DIALOG_SPEAKING,
    NUWAX_DIALOG_ERROR,
    NUWAX_DIALOG_READY
} nuwax_dialog_state_t;

typedef enum {
    NUWAX_POWER_UNKNOWN = 0,
    NUWAX_POWER_EXTERNAL,
    NUWAX_POWER_BATTERY,
    NUWAX_POWER_CHARGING,
    NUWAX_POWER_LOW
} nuwax_power_state_t;

/* Meeting-mode state is kept in the shared UI snapshot so the switch never
 * guesses from the last user action.  The audio controller is authoritative
 * and updates this value after every state transition. */
typedef enum {
    NUWAX_MEETING_UI_DISABLED = 0,
    NUWAX_MEETING_UI_STARTING,
    NUWAX_MEETING_UI_ACTIVE,
    NUWAX_MEETING_UI_STOPPING,
    NUWAX_MEETING_UI_ERROR,
} nuwax_meeting_ui_state_t;

typedef enum {
    NUWAX_MONITORING_UI_DISABLED = 0,
    NUWAX_MONITORING_UI_STARTING,
    NUWAX_MONITORING_UI_ACTIVE,
    NUWAX_MONITORING_UI_STOPPING,
    NUWAX_MONITORING_UI_ERROR,
} nuwax_monitoring_ui_state_t;

typedef struct {
    nuwax_emotion_t emotion;
    /* True only when emotion was explicitly selected by a semantic/cloud
     * command.  Business-mode flags remain authoritative and are never
     * inferred from an emotion value. */
    bool semantic_emotion_active;
    nuwax_wifi_state_t wifi;
    nuwax_cloud_state_t cloud;
    nuwax_dialog_state_t dialog;
    nuwax_power_state_t power;
    nuwax_meeting_ui_state_t meeting;
    nuwax_monitoring_ui_state_t monitoring;
    uint8_t volume;
    uint8_t brightness_percent;
    bool low_power;
    bool app_low_power;
    bool battery_low;
    bool battery_valid;
    bool battery_soc_trusted;
    uint8_t battery_percent;
    uint16_t battery_voltage_mv;
    int16_t battery_current_ma;
    char ssid[32];
    char ip[16];
    bool wifi_signal_valid;
    int8_t wifi_rssi_dbm;
    nuwax_wifi_signal_t wifi_signal;
    bool provisioning_active;
    bool provisioning_starting;
    uint32_t touch_sequence;
    bool touch_emotion_active;
    bool speaking_pcm_active;
    bool ota_update_active;
    uint8_t ota_update_percent;
    /* Test-only cloud isolation flag.  The physical STA link remains up so
     * an offline replay gateway can be reached after the test is disabled. */
    bool simulated_offline;
} nuwax_device_state_t;

typedef void (*nuwax_state_listener_t)(const nuwax_device_state_t *state, void *user_data);

void nuwax_device_state_init(void);
const nuwax_device_state_t *nuwax_device_state_get(void);
void nuwax_device_state_set(const nuwax_device_state_t *state);
void nuwax_device_state_set_emotion(nuwax_emotion_t emotion);
void nuwax_device_state_set_volume(uint8_t volume);
void nuwax_device_state_set_low_power(bool enabled);
void nuwax_device_state_set_wifi_signal(bool valid, int8_t rssi_dbm);
/* Callers outside the LVGL task must hold the application's LVGL lock because
 * state listeners update widgets directly. */
void nuwax_device_state_set_battery(bool valid,
                                    bool soc_trusted,
                                    nuwax_power_state_t power,
                                    uint8_t percent,
                                    uint16_t voltage_mv,
                                    int16_t current_ma);
bool nuwax_device_state_add_listener(nuwax_state_listener_t listener, void *user_data);
bool nuwax_device_state_remove_listener(nuwax_state_listener_t listener, void *user_data);
void nuwax_device_state_notify(void);
