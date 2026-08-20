#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_state.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "touch_input.h"
#include "nuwax_device_state.h"

typedef enum {
    NUWAX_APP_SHELL_EVENT_TAP = 0,
    NUWAX_APP_SHELL_EVENT_PRESS,
    NUWAX_APP_SHELL_EVENT_RELEASE,
    NUWAX_APP_SHELL_EVENT_LONG_PRESS,
    NUWAX_APP_SHELL_EVENT_VERY_LONG_PRESS,
    NUWAX_APP_SHELL_EVENT_SWIPE_LEFT,
    NUWAX_APP_SHELL_EVENT_SWIPE_RIGHT,
    NUWAX_APP_SHELL_EVENT_SWIPE_UP,
    NUWAX_APP_SHELL_EVENT_SWIPE_DOWN,
} nuwax_app_shell_event_t;

typedef enum {
    NUWAX_APP_SHELL_ACTION_NONE = 0,
    NUWAX_APP_SHELL_ACTION_START_LISTEN,
    NUWAX_APP_SHELL_ACTION_STOP_LISTEN,
    NUWAX_APP_SHELL_ACTION_SCAN_WIFI,
    NUWAX_APP_SHELL_ACTION_SHOW_SAVED_WIFI,
    NUWAX_APP_SHELL_ACTION_SELECT_WIFI,
    NUWAX_APP_SHELL_ACTION_CONNECT_SAVED_WIFI,
    NUWAX_APP_SHELL_ACTION_REPROVISION_WIFI,
    NUWAX_APP_SHELL_ACTION_REFRESH_PC_STATUS,
    NUWAX_APP_SHELL_ACTION_REFRESH_WORK_STATUS,
    NUWAX_APP_SHELL_ACTION_REFRESH_ISSUES_STATUS,
    NUWAX_APP_SHELL_ACTION_CANCEL,
    NUWAX_APP_SHELL_ACTION_SET_VOLUME,
    NUWAX_APP_SHELL_ACTION_SET_MEETING_MODE,
    NUWAX_APP_SHELL_ACTION_HEAD_TOUCH_AGENT,
    NUWAX_APP_SHELL_ACTION_SHAKE_AGENT,
} nuwax_app_shell_action_t;

typedef struct {
    nuwax_app_shell_action_t action;
    int index;
} nuwax_app_shell_result_t;

typedef void (*nuwax_app_shell_reprovision_cb_t)(void *ctx, bool enabled);

typedef uint32_t nuwax_app_shell_state_fields_t;

enum {
    NUWAX_APP_SHELL_STATE_EMOTION = 1U << 0,
    NUWAX_APP_SHELL_STATE_WIFI = 1U << 1,
    NUWAX_APP_SHELL_STATE_CLOUD = 1U << 2,
    NUWAX_APP_SHELL_STATE_DIALOG = 1U << 3,
    NUWAX_APP_SHELL_STATE_APP_LOW_POWER = 1U << 4,
    NUWAX_APP_SHELL_STATE_BATTERY = 1U << 5,
    NUWAX_APP_SHELL_STATE_MEETING = 1U << 6,
    NUWAX_APP_SHELL_STATE_MONITORING = 1U << 7,
    NUWAX_APP_SHELL_STATE_VOLUME = 1U << 8,
    NUWAX_APP_SHELL_STATE_PROVISIONING = 1U << 9,
    NUWAX_APP_SHELL_STATE_TOUCH_SEQUENCE = 1U << 10,
    NUWAX_APP_SHELL_STATE_WIFI_SIGNAL = 1U << 11,
    NUWAX_APP_SHELL_STATE_SEMANTIC_EMOTION = 1U << 12,
    NUWAX_APP_SHELL_STATE_TOUCH_EMOTION = 1U << 13,
    NUWAX_APP_SHELL_STATE_SPEAKING_PCM = 1U << 14,
    NUWAX_APP_SHELL_STATE_OTA_PROGRESS = 1U << 15,
    NUWAX_APP_SHELL_STATE_ALL = (1U << 16) - 1U,
};

esp_err_t nuwax_app_shell_init(void);
/* Detach the LVGL bridge before destroying any V28 UI object. The caller must
 * hold the LVGL port mutex when this is paired with nuwax_app_deinit(). */
void nuwax_app_shell_deinit(void);
void nuwax_app_shell_set_action_queue(QueueHandle_t queue);
/* Provisioning can park the ordinary shell-action consumer, so its UI command
 * uses a dedicated non-blocking callback carrying the explicit target state. */
void nuwax_app_shell_set_reprovision_callback(
    nuwax_app_shell_reprovision_cb_t callback,
    void *ctx);
bool nuwax_app_shell_is_ui_ready(void);
bool nuwax_app_shell_is_menu_active(void);
bool nuwax_app_shell_blocks_auto_followup(void);
bool nuwax_app_shell_can_start_listen(void);
/* Close menu/drawer before entering wake-only state so touch wake cannot be
 * masked by an overlay left open in the previous conversation. */
void nuwax_app_shell_close_overlays(void);
void nuwax_app_shell_update(uint32_t now_ms);
void nuwax_app_shell_render_status_bar(void);
void nuwax_app_shell_set_app_state(nuwax_app_state_t state);
void nuwax_app_shell_set_bridge_status(bool connected);
void nuwax_app_shell_set_cloud_state(nuwax_cloud_state_t state);
void nuwax_app_shell_set_wifi_status(const char *ssid, const char *ip);
void nuwax_app_shell_set_wifi_signal(bool valid, int8_t rssi_dbm);
void nuwax_app_shell_set_wifi_scan_busy(void);
void nuwax_app_shell_set_wifi_networks(const char ssids[][33], const int8_t *rssis, const uint8_t *authmodes, int count);
void nuwax_app_shell_set_saved_wifi_networks(const char ssids[][33], int count);
void nuwax_app_shell_set_wifi_connecting(const char *ssid);
void nuwax_app_shell_set_wifi_result(const char *ssid, bool ok, const char *message);
void nuwax_app_shell_set_volume_percent(int volume_percent);
void nuwax_app_shell_set_meeting_state(nuwax_meeting_ui_state_t state);
void nuwax_app_shell_set_monitoring_state(nuwax_monitoring_ui_state_t state);
void nuwax_app_shell_set_provisioning_active(bool active);
void nuwax_app_shell_set_ota_progress(bool active, int percent);
/* Thread-safe one-shot request. The LVGL owner opens the dismissible QR detail
 * after delay_ms only while provisioning is active. An already-open menu may
 * transition directly to the QR; unrelated pages and details are preserved. */
void nuwax_app_shell_schedule_provisioning_qr(uint32_t delay_ms);
void nuwax_app_shell_trigger_touch_animation(void);
void nuwax_app_shell_set_battery_state(bool valid,
                                       bool soc_trusted,
                                       nuwax_power_state_t power,
                                       uint8_t percent,
                                       uint16_t voltage_mv,
                                       int16_t current_ma);
/* Thread-safe canonical state access. Producers merge only the fields they
 * own; the LVGL task is the sole writer of nuwax_device_state and widgets. */
bool nuwax_app_shell_get_device_state_snapshot(nuwax_device_state_t *out_state,
                                               uint32_t *out_revision);
bool nuwax_app_shell_publish_device_state_delta(
    const nuwax_device_state_t *patch,
    nuwax_app_shell_state_fields_t fields);
/* State-bridge producers are ordered by bridge_revision. A late publication
 * from an older transaction is rejected without changing the UI mailbox. */
bool nuwax_app_shell_publish_device_state_delta_from_bridge(
    const nuwax_device_state_t *patch,
    nuwax_app_shell_state_fields_t fields,
    uint32_t bridge_revision);
/* State-bridge variant: publishes the derived UI fields and the source app
 * state in one mailbox transaction so dialog gating cannot race sleep/wake. */
bool nuwax_app_shell_publish_app_state_delta(
    nuwax_app_state_t app_state,
    const nuwax_device_state_t *patch,
    nuwax_app_shell_state_fields_t fields);
bool nuwax_app_shell_publish_app_state_delta_from_bridge(
    nuwax_app_state_t app_state,
    const nuwax_device_state_t *patch,
    nuwax_app_shell_state_fields_t fields,
    uint32_t bridge_revision);
void nuwax_app_shell_set_status_card(const char *title, const char *line1, const char *line2, const char *line3);
void nuwax_app_shell_set_status_card_lines(const char *title, const char lines[][48], int line_count, const char *text);
void nuwax_app_shell_set_conversation_status(const char *title,
                                             const char *phase,
                                             const char *text,
                                             bool show_now);
nuwax_app_shell_result_t nuwax_app_shell_handle_touch_event(nuwax_app_shell_event_t event,
                                                            const nuwax_touch_point_t *point);
bool nuwax_app_shell_drawer_is_open(void);
bool nuwax_app_shell_drawer_is_detail(void);
void nuwax_app_shell_drawer_begin_pull(void);
int16_t nuwax_app_shell_drawer_current_y(void);
bool nuwax_app_shell_drawer_preview_volume_at(uint16_t x, uint16_t y, uint8_t *volume_out);
void nuwax_app_shell_drawer_drag_to(int16_t y);
void nuwax_app_shell_drawer_finish_drag(bool close);
void nuwax_app_shell_drawer_page_drag_to(int16_t delta_x);
void nuwax_app_shell_drawer_page_finish_drag(bool commit);
bool nuwax_app_shell_focus_menu_main_is_open(void);
bool nuwax_app_shell_focus_menu_detail_is_open(void);
void nuwax_app_shell_menu_drag_to(int16_t delta_y);
void nuwax_app_shell_menu_finish_drag(void);
void nuwax_app_shell_menu_detail_adjust(int direction);

/* Queue a business action from any context (LVGL task, etc.) */
bool nuwax_app_shell_request_action(nuwax_app_shell_action_t action, int value);
/* Thread-safe feedback from the business owner when a control-center action
 * could not be accepted. The LVGL task clears only the matching optimistic
 * visual state. */
void nuwax_app_shell_notify_action_rejected(nuwax_app_shell_action_t action);
