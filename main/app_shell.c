#include "app_shell.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif

#ifndef CONFIG_NUWAX_TOUCH_WAKE_ACTION_ENABLE
#define CONFIG_NUWAX_TOUCH_WAKE_ACTION_ENABLE 0
#endif

#ifndef CONFIG_NUWAX_TOUCH_INTERRUPT_ACTION_ENABLE
#define CONFIG_NUWAX_TOUCH_INTERRUPT_ACTION_ENABLE 0
#endif

/* LVGL mode: app_shell API routes state changes to nuwax_device_state.
 * The LVGL UI is driven via the state listener mechanism. */

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <string.h>
#include "nuwax_device_state.h"
#include "nuwax_lvgl_port.h"
#include "nuwax_drawer.h"
#include "nuwax_gesture.h"
#include "nuwax_face.h"
#include "nuwax_state_bridge.h"
#include "nuwax_ui_v28/ui_bridge_revision_policy.h"
#include "wifi_manager.h"

static const char *TAG = "app_shell_lvgl";

#define LVGL_DRAWER_DRAG_LOCK_TIMEOUT_MS 80
#define APP_SHELL_CLEANUP_ACTION_QUEUE_WAIT_MS 100
#define APP_SHELL_TOUCH_EVENT_QUEUE_DEPTH 12U
#define APP_SHELL_TOUCH_EVENT_DRAIN_BUDGET_US 2000U
#define APP_SHELL_TOUCH_EVENT_QUEUE_WAIT_MS 10U
#define APP_SHELL_WIFI_MAX_ITEMS 8
#define APP_SHELL_WIFI_SSID_LEN 33
#define APP_SHELL_WIFI_RESULT_MESSAGE_LEN 48
#define APP_SHELL_STATUS_TITLE_LEN 32
#define APP_SHELL_STATUS_LINE_COUNT 6
#define APP_SHELL_STATUS_LINE_LEN 48
#define APP_SHELL_WIFI_SIGNAL_POLL_MS 2000U
#define APP_SHELL_PROVISIONING_QR_RETRY_MS 500U

static bool phase_equals(const char *phase, const char *expected) {
    if (!phase || !expected) {
        return false;
    }
    while (*phase && *expected) {
        if (tolower((unsigned char)*phase) != tolower((unsigned char)*expected)) {
            return false;
        }
        phase++;
        expected++;
    }
    return *phase == '\0' && *expected == '\0';
}
static nuwax_app_shell_result_t drawer_action_to_shell_action(
    nuwax_drawer_action_result_t drawer_action);
static nuwax_app_shell_result_t app_shell_handle_touch_event_on_lvgl(
    nuwax_app_shell_event_t event,
    const nuwax_touch_point_t *point);

static portMUX_TYPE s_ui_mailbox_mux = portMUX_INITIALIZER_UNLOCKED;
static int8_t s_pending_menu_scroll;
static bool s_pending_menu_drag;
static int16_t s_pending_menu_drag_delta_y;
static bool s_pending_menu_finish;
static int8_t s_pending_detail_adjust;
static bool s_pending_drawer_drag;
static int16_t s_pending_drawer_y = -226;
static bool s_pending_drawer_begin_pull;
static bool s_pending_drawer_finish;
static bool s_pending_drawer_finish_close;
static bool s_pending_drawer_page_drag;
static int16_t s_pending_drawer_page_delta_x;
static bool s_pending_drawer_page_finish;
static bool s_pending_drawer_page_commit;
static bool s_pending_drawer_volume_preview;
static uint8_t s_pending_drawer_volume;
static bool s_pending_provisioning_qr;
static int64_t s_pending_provisioning_qr_deadline_us;
static bool s_pending_provisioning_qr_close;
static int16_t s_cached_drawer_y = -226;
static bool s_ui_ready;
static QueueHandle_t s_shell_actions;
static nuwax_app_shell_reprovision_cb_t s_reprovision_cb;
static void *s_reprovision_ctx;

typedef struct {
    nuwax_app_shell_event_t event;
    nuwax_touch_point_t point;
    bool has_point;
    uint32_t sequence;
    int64_t queued_ms;
} app_shell_touch_event_t;

static StaticQueue_t s_touch_event_queue_storage;
static uint8_t s_touch_event_queue_buffer[
    APP_SHELL_TOUCH_EVENT_QUEUE_DEPTH * sizeof(app_shell_touch_event_t)];
static QueueHandle_t s_touch_event_queue;
static uint32_t s_touch_event_sequence;

typedef struct {
    bool menu_open;
    bool menu_detail;
    bool drawer_open;
} app_shell_overlay_state_t;

static app_shell_overlay_state_t s_overlay_state;

#define APP_SHELL_STATUS_TEXT_LEN 192

typedef enum {
    APP_SHELL_WIFI_DETAIL_NONE = 0,
    APP_SHELL_WIFI_DETAIL_SCAN_BUSY,
    APP_SHELL_WIFI_DETAIL_NETWORKS,
    APP_SHELL_WIFI_DETAIL_SAVED_NETWORKS,
    APP_SHELL_WIFI_DETAIL_CONNECTING,
    APP_SHELL_WIFI_DETAIL_RESULT,
} app_shell_wifi_detail_kind_t;

typedef struct {
    app_shell_wifi_detail_kind_t kind;
    int count;
    char ssids[APP_SHELL_WIFI_MAX_ITEMS][APP_SHELL_WIFI_SSID_LEN];
    int8_t rssis[APP_SHELL_WIFI_MAX_ITEMS];
    uint8_t authmodes[APP_SHELL_WIFI_MAX_ITEMS];
    char ssid[APP_SHELL_WIFI_SSID_LEN];
    bool ok;
    char message[APP_SHELL_WIFI_RESULT_MESSAGE_LEN];
} app_shell_wifi_detail_t;

typedef struct {
    uint32_t revision;
    uint32_t published_revision;
    app_shell_wifi_detail_t detail;
} app_shell_wifi_detail_mailbox_t;

typedef struct {
    char title[APP_SHELL_STATUS_TITLE_LEN];
    char lines[APP_SHELL_STATUS_LINE_COUNT][APP_SHELL_STATUS_LINE_LEN];
    int line_count;
    char text[APP_SHELL_STATUS_TEXT_LEN];
} app_shell_status_detail_t;

typedef struct {
    uint32_t revision;
    uint32_t published_revision;
    app_shell_status_detail_t detail;
} app_shell_status_detail_mailbox_t;

typedef struct {
    uint32_t revision;
    uint32_t published_revision;
    /* Each bridge-owned field has an independent producer watermark. A newer
     * narrow update must not discard unrelated fields from an older delayed
     * transaction. */
    uint32_t bridge_field_revisions[16];
    bool initialized;
    bool app_state_valid;
    nuwax_app_state_t app_state;
    /* The requested expression is retained while a low-battery warning owns
     * the visible face, then restored when the warning clears. */
    nuwax_emotion_t base_emotion;
    nuwax_device_state_t snapshot;
} app_shell_state_mailbox_t;

static app_shell_state_mailbox_t s_state_mailbox;
static app_shell_wifi_detail_mailbox_t s_wifi_detail_mailbox;
static app_shell_status_detail_mailbox_t s_status_detail_mailbox;
static uint32_t s_wifi_signal_last_poll_ms;
static uint32_t s_drawer_action_reject_mask;

static app_shell_overlay_state_t app_shell_overlay_state_get(void) {
    app_shell_overlay_state_t state;
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    state = s_overlay_state;
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    return state;
}

static void app_shell_overlay_state_set(
    const app_shell_overlay_state_t *state) {
    if (!state) {
        return;
    }
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    s_overlay_state = *state;
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
}

/* Must run on the LVGL task or while the LVGL port mutex is held. */
static void app_shell_overlay_state_sync_from_lvgl(void) {
    const app_shell_overlay_state_t state = {
        .menu_open = false,
        .menu_detail = nuwax_drawer_is_detail(),
        .drawer_open = nuwax_drawer_is_open(),
    };
    app_shell_overlay_state_set(&state);
}

static void app_shell_lvgl_unlock(void) {
    app_shell_overlay_state_sync_from_lvgl();
    nuwax_lvgl_port_unlock();
}

static uint32_t app_shell_next_revision(uint32_t revision) {
    revision++;
    return revision == 0U ? 1U : revision;
}

bool nuwax_app_shell_is_ui_ready(void) {
    bool ready;
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    ready = s_ui_ready;
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    return ready;
}

void nuwax_app_shell_set_action_queue(QueueHandle_t queue) {
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    s_shell_actions = queue;
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
}

void nuwax_app_shell_set_reprovision_callback(
    nuwax_app_shell_reprovision_cb_t callback,
    void *ctx) {
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    s_reprovision_cb = callback;
    s_reprovision_ctx = ctx;
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
}

static void app_shell_set_ui_ready(bool ready) {
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    s_ui_ready = ready;
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
}

static uint32_t app_shell_next_state_revision_locked(void) {
    s_state_mailbox.revision =
        app_shell_next_revision(s_state_mailbox.revision);
    return s_state_mailbox.revision;
}

static void app_shell_clear_transient_ui_locked(void) {
    s_pending_menu_scroll = 0;
    s_pending_menu_drag = false;
    s_pending_menu_drag_delta_y = 0;
    s_pending_menu_finish = false;
    s_pending_detail_adjust = 0;
    s_pending_drawer_drag = false;
    s_pending_drawer_y = -226;
    s_pending_drawer_begin_pull = false;
    s_pending_drawer_finish = false;
    s_pending_drawer_finish_close = false;
    s_pending_drawer_page_drag = false;
    s_pending_drawer_page_delta_x = 0;
    s_pending_drawer_page_finish = false;
    s_pending_drawer_page_commit = false;
    s_pending_drawer_volume_preview = false;
    s_pending_drawer_volume = 0;
    s_pending_provisioning_qr = false;
    s_pending_provisioning_qr_deadline_us = 0;
    s_pending_provisioning_qr_close = false;
    s_cached_drawer_y = -226;
    s_drawer_action_reject_mask = 0U;
    memset(&s_overlay_state, 0, sizeof(s_overlay_state));
}

static uint32_t app_shell_action_bit(nuwax_app_shell_action_t action) {
    const unsigned value = (unsigned)action;
    return value < 32U ? (1UL << value) : 0U;
}

void nuwax_app_shell_notify_action_rejected(nuwax_app_shell_action_t action) {
    const uint32_t bit = app_shell_action_bit(action);
    if (bit == 0U) {
        return;
    }
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    s_drawer_action_reject_mask |= bit;
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
}

static void app_shell_post_wifi_detail(
    const app_shell_wifi_detail_t *detail) {
    if (!detail || detail->kind == APP_SHELL_WIFI_DETAIL_NONE) {
        return;
    }
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    s_wifi_detail_mailbox.detail = *detail;
    s_wifi_detail_mailbox.revision =
        app_shell_next_revision(s_wifi_detail_mailbox.revision);
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
}

static void app_shell_post_status_detail(
    const app_shell_status_detail_t *detail) {
    if (!detail) {
        return;
    }
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    s_status_detail_mailbox.detail = *detail;
    s_status_detail_mailbox.revision =
        app_shell_next_revision(s_status_detail_mailbox.revision);
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
}

static void app_shell_ensure_state_snapshot_locked(void) {
    if (s_state_mailbox.initialized) {
        return;
    }
    s_state_mailbox.snapshot = *nuwax_device_state_get();
    s_state_mailbox.base_emotion = s_state_mailbox.snapshot.emotion;
    s_state_mailbox.app_state = NUWAX_APP_STATE_BOOTING;
    s_state_mailbox.initialized = true;
}

static bool app_shell_state_fields_valid(
    nuwax_app_shell_state_fields_t fields) {
    return fields != 0U &&
           (fields & ~((nuwax_app_shell_state_fields_t)
                           NUWAX_APP_SHELL_STATE_ALL)) == 0U;
}

static void app_shell_merge_device_state_locked(
    const nuwax_device_state_t *patch,
    nuwax_app_shell_state_fields_t fields) {
    nuwax_device_state_t *state = &s_state_mailbox.snapshot;
    if ((fields & NUWAX_APP_SHELL_STATE_EMOTION) != 0U) {
        s_state_mailbox.base_emotion =
            patch->emotion < NUWAX_EMOTION_COUNT
                ? patch->emotion
                : NUWAX_EMOTION_NEUTRAL;
    }
    if ((fields & NUWAX_APP_SHELL_STATE_SEMANTIC_EMOTION) != 0U) {
        state->semantic_emotion_active = patch->semantic_emotion_active;
    }
    if ((fields & NUWAX_APP_SHELL_STATE_WIFI) != 0U) {
        state->wifi = patch->wifi;
        strlcpy(state->ssid, patch->ssid, sizeof(state->ssid));
        strlcpy(state->ip, patch->ip, sizeof(state->ip));
    }
    if ((fields & NUWAX_APP_SHELL_STATE_CLOUD) != 0U) {
        state->cloud = patch->cloud;
    }
    if ((fields & NUWAX_APP_SHELL_STATE_DIALOG) != 0U) {
        state->dialog = patch->dialog;
    }
    if ((fields & NUWAX_APP_SHELL_STATE_APP_LOW_POWER) != 0U) {
        state->app_low_power = patch->app_low_power;
    }
    if ((fields & NUWAX_APP_SHELL_STATE_BATTERY) != 0U) {
        state->battery_valid = patch->battery_valid;
        state->battery_soc_trusted =
            patch->battery_valid && patch->battery_soc_trusted;
        state->power = patch->battery_valid ? patch->power
                                           : NUWAX_POWER_UNKNOWN;
        if ((unsigned)state->power > (unsigned)NUWAX_POWER_LOW) {
            state->power = NUWAX_POWER_UNKNOWN;
            state->battery_valid = false;
            state->battery_soc_trusted = false;
        }
        state->battery_low =
            state->battery_valid && state->power == NUWAX_POWER_LOW;
        if (state->battery_valid) {
            state->battery_percent =
                patch->battery_percent > 100U ? 100U
                                              : patch->battery_percent;
            state->battery_voltage_mv = patch->battery_voltage_mv;
            state->battery_current_ma = patch->battery_current_ma;
        }
    }
    if ((fields & NUWAX_APP_SHELL_STATE_MEETING) != 0U) {
        state->meeting = patch->meeting;
    }
    if ((fields & NUWAX_APP_SHELL_STATE_MONITORING) != 0U) {
        state->monitoring = patch->monitoring;
    }
    if ((fields & NUWAX_APP_SHELL_STATE_VOLUME) != 0U) {
        state->volume = patch->volume > 100U ? 100U : patch->volume;
    }
    if ((fields & NUWAX_APP_SHELL_STATE_PROVISIONING) != 0U) {
        state->provisioning_active = patch->provisioning_active;
    }
    if ((fields & NUWAX_APP_SHELL_STATE_TOUCH_SEQUENCE) != 0U) {
        state->touch_sequence = patch->touch_sequence;
    }
    if ((fields & NUWAX_APP_SHELL_STATE_TOUCH_EMOTION) != 0U) {
        state->touch_emotion_active = patch->touch_emotion_active;
    }
    if ((fields & NUWAX_APP_SHELL_STATE_SPEAKING_PCM) != 0U) {
        state->speaking_pcm_active = patch->speaking_pcm_active;
    }
    if ((fields & NUWAX_APP_SHELL_STATE_OTA_PROGRESS) != 0U) {
        state->ota_update_active = patch->ota_update_active;
        state->ota_update_percent = patch->ota_update_active
                                        ? (patch->ota_update_percent > 100U
                                               ? 100U
                                               : patch->ota_update_percent)
                                        : 0U;
    }
    if ((fields & NUWAX_APP_SHELL_STATE_WIFI_SIGNAL) != 0U) {
        state->wifi_signal_valid = patch->wifi_signal_valid;
        state->wifi_rssi_dbm = patch->wifi_signal_valid
                                   ? patch->wifi_rssi_dbm
                                   : -127;
        state->wifi_signal = patch->wifi_signal_valid
                                 ? patch->wifi_signal
                                 : NUWAX_WIFI_SIGNAL_UNKNOWN;
    }
    /* Battery warnings are represented by the status bar.  Do not replace
     * the active face or mark the application low-power solely because the
     * gauge reports a low SOC; wake and conversation remain available. */
    state->low_power = state->app_low_power;
    state->emotion = s_state_mailbox.base_emotion;
}

bool nuwax_app_shell_publish_device_state_delta(
    const nuwax_device_state_t *patch,
    nuwax_app_shell_state_fields_t fields) {
    if (!patch || !app_shell_state_fields_valid(fields)) {
        return false;
    }
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    app_shell_ensure_state_snapshot_locked();
    app_shell_merge_device_state_locked(patch, fields);
    (void)app_shell_next_state_revision_locked();
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    nuwax_lvgl_port_wake();
    return true;
}

bool nuwax_app_shell_publish_device_state_delta_from_bridge(
    const nuwax_device_state_t *patch,
    nuwax_app_shell_state_fields_t fields,
    uint32_t bridge_revision) {
    if (!patch || !app_shell_state_fields_valid(fields) ||
        bridge_revision == 0U) {
        return false;
    }
    bool accepted = false;
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    app_shell_ensure_state_snapshot_locked();
    const nuwax_app_shell_state_fields_t accepted_fields =
        nuwax_ui_bridge_filter_newer_fields(
            bridge_revision, fields,
            s_state_mailbox.bridge_field_revisions,
            sizeof(s_state_mailbox.bridge_field_revisions) /
                sizeof(s_state_mailbox.bridge_field_revisions[0]));
    if (accepted_fields != 0U) {
        app_shell_merge_device_state_locked(patch, accepted_fields);
        (void)app_shell_next_state_revision_locked();
        accepted = true;
    }
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    if (accepted) {
        nuwax_lvgl_port_wake();
    }
    return accepted;
}

bool nuwax_app_shell_publish_app_state_delta(
    nuwax_app_state_t app_state,
    const nuwax_device_state_t *patch,
    nuwax_app_shell_state_fields_t fields) {
    if (!patch || !app_shell_state_fields_valid(fields) ||
        (unsigned)app_state > (unsigned)NUWAX_APP_STATE_ERROR) {
        return false;
    }
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    app_shell_ensure_state_snapshot_locked();
    s_state_mailbox.app_state = app_state;
    s_state_mailbox.app_state_valid = true;
    app_shell_merge_device_state_locked(patch, fields);
    (void)app_shell_next_state_revision_locked();
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    nuwax_lvgl_port_wake();
    return true;
}

bool nuwax_app_shell_publish_app_state_delta_from_bridge(
    nuwax_app_state_t app_state,
    const nuwax_device_state_t *patch,
    nuwax_app_shell_state_fields_t fields,
    uint32_t bridge_revision) {
    if (!patch || !app_shell_state_fields_valid(fields) ||
        (unsigned)app_state > (unsigned)NUWAX_APP_STATE_ERROR ||
        bridge_revision == 0U) {
        return false;
    }
    bool accepted = false;
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    app_shell_ensure_state_snapshot_locked();
    const nuwax_app_shell_state_fields_t accepted_fields =
        nuwax_ui_bridge_filter_newer_fields(
            bridge_revision, fields,
            s_state_mailbox.bridge_field_revisions,
            sizeof(s_state_mailbox.bridge_field_revisions) /
                sizeof(s_state_mailbox.bridge_field_revisions[0]));
    if (accepted_fields != 0U) {
        if ((accepted_fields & NUWAX_APP_SHELL_STATE_DIALOG) != 0U) {
            s_state_mailbox.app_state = app_state;
            s_state_mailbox.app_state_valid = true;
        }
        app_shell_merge_device_state_locked(patch, accepted_fields);
        (void)app_shell_next_state_revision_locked();
        accepted = true;
    }
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    if (accepted) {
        nuwax_lvgl_port_wake();
    }
    return accepted;
}

bool nuwax_app_shell_get_device_state_snapshot(nuwax_device_state_t *out_state,
                                               uint32_t *out_revision) {
    if (!out_state) {
        return false;
    }
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    app_shell_ensure_state_snapshot_locked();
    *out_state = s_state_mailbox.snapshot;
    if (out_revision) {
        *out_revision = s_state_mailbox.revision;
    }
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    return true;
}

static void app_shell_post_dialog(nuwax_dialog_state_t dialog) {
    if ((unsigned)dialog > (unsigned)NUWAX_DIALOG_READY) {
        return;
    }
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    app_shell_ensure_state_snapshot_locked();
    const bool sleeping = s_state_mailbox.app_state_valid &&
                          s_state_mailbox.app_state ==
                              NUWAX_APP_STATE_SLEEPING;
    if (sleeping && dialog != NUWAX_DIALOG_SLEEP &&
        s_state_mailbox.snapshot.meeting == NUWAX_MEETING_UI_DISABLED &&
        s_state_mailbox.snapshot.monitoring ==
            NUWAX_MONITORING_UI_DISABLED) {
        dialog = NUWAX_DIALOG_SLEEP;
    } else if (!sleeping && dialog == NUWAX_DIALOG_SLEEP) {
        taskEXIT_CRITICAL(&s_ui_mailbox_mux);
        return;
    }
    nuwax_device_state_t patch = {.dialog = dialog};
    app_shell_merge_device_state_locked(
        &patch, NUWAX_APP_SHELL_STATE_DIALOG);
    (void)app_shell_next_state_revision_locked();
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
}

static void app_shell_post_meeting(nuwax_meeting_ui_state_t meeting) {
    nuwax_device_state_t patch = {.meeting = meeting};
    (void)nuwax_app_shell_publish_device_state_delta(
        &patch, NUWAX_APP_SHELL_STATE_MEETING);
}

static void app_shell_post_monitoring(nuwax_monitoring_ui_state_t monitoring) {
    nuwax_device_state_t patch = {.monitoring = monitoring};
    (void)nuwax_app_shell_publish_device_state_delta(
        &patch, NUWAX_APP_SHELL_STATE_MONITORING);
}

static void app_shell_post_volume(uint8_t volume) {
    nuwax_device_state_t patch = {.volume = volume};
    (void)nuwax_app_shell_publish_device_state_delta(
        &patch, NUWAX_APP_SHELL_STATE_VOLUME);
}

static bool app_shell_queue_result(const nuwax_app_shell_result_t *result) {
    if (!result || result->action == NUWAX_APP_SHELL_ACTION_NONE) {
        return false;
    }
    if (result->action == NUWAX_APP_SHELL_ACTION_REPROVISION_WIFI) {
        nuwax_app_shell_reprovision_cb_t callback;
        void *callback_ctx;
        taskENTER_CRITICAL(&s_ui_mailbox_mux);
        callback = s_reprovision_cb;
        callback_ctx = s_reprovision_ctx;
        taskEXIT_CRITICAL(&s_ui_mailbox_mux);
        if (!callback) {
            ESP_LOGE(TAG, "provisioning control callback unavailable target=%d",
                     result->index != 0 ? 1 : 0);
            nuwax_app_shell_notify_action_rejected(result->action);
            return false;
        }
        callback(callback_ctx, result->index != 0);
        return true;
    }
    QueueHandle_t queue;
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    queue = s_shell_actions;
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    if (!queue) {
        nuwax_app_shell_notify_action_rejected(result->action);
        return false;
    }
    const bool cleanup_action =
        result->action == NUWAX_APP_SHELL_ACTION_STOP_LISTEN ||
        result->action == NUWAX_APP_SHELL_ACTION_CANCEL;
    const TickType_t wait_ticks = cleanup_action
                                      ? pdMS_TO_TICKS(
                                            APP_SHELL_CLEANUP_ACTION_QUEUE_WAIT_MS)
                                      : 0;
    if (xQueueSend(queue, result, wait_ticks) != pdTRUE) {
        ESP_LOGW(TAG, "shell action queue full action=%d", (int)result->action);
        nuwax_app_shell_notify_action_rejected(result->action);
        return false;
    }
    if (result->action == NUWAX_APP_SHELL_ACTION_SET_VOLUME) {
        /* Volume application is non-rejecting once admitted to the owner
         * queue. Publish the accepted target immediately so a later failed
         * preview rolls back to this command instead of an older UI sample. */
        app_shell_post_volume((uint8_t)(result->index < 0
                                            ? 0
                                            : (result->index > 100
                                                   ? 100
                                                   : result->index)));
    }
    return true;
}

static int16_t app_shell_cached_drawer_y_get(void) {
    int16_t y;
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    y = s_cached_drawer_y;
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    return y;
}

static void app_shell_cached_drawer_y_set(int16_t y) {
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    s_cached_drawer_y = y;
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
}

static void app_shell_post_menu_scroll(int direction) {
    if (direction == 0) return;
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    if (!s_ui_ready) {
        taskEXIT_CRITICAL(&s_ui_mailbox_mux);
        return;
    }
    int pending = (int)s_pending_menu_scroll + (direction > 0 ? 1 : -1);
    if (pending > 2) pending = 2;
    if (pending < -2) pending = -2;
    s_pending_menu_scroll = (int8_t)pending;
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    nuwax_lvgl_port_wake();
}

static void app_shell_post_menu_drag(int16_t delta_y) {
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    if (!s_ui_ready) {
        taskEXIT_CRITICAL(&s_ui_mailbox_mux);
        return;
    }
    s_pending_menu_drag_delta_y = delta_y;
    s_pending_menu_drag = true;
    s_pending_menu_scroll = 0;
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    nuwax_lvgl_port_wake();
}

static void app_shell_post_menu_finish(void) {
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    if (!s_ui_ready) {
        taskEXIT_CRITICAL(&s_ui_mailbox_mux);
        return;
    }
    s_pending_menu_finish = true;
    s_pending_menu_scroll = 0;
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    nuwax_lvgl_port_wake();
}

static void app_shell_post_detail_adjust(int direction) {
    if (direction == 0) return;
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    if (!s_ui_ready) {
        taskEXIT_CRITICAL(&s_ui_mailbox_mux);
        return;
    }
    int pending = (int)s_pending_detail_adjust + (direction > 0 ? 1 : -1);
    if (pending > 2) pending = 2;
    if (pending < -2) pending = -2;
    s_pending_detail_adjust = (int8_t)pending;
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    nuwax_lvgl_port_wake();
}

static void app_shell_post_drawer_drag(int16_t y) {
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    if (!s_ui_ready) {
        taskEXIT_CRITICAL(&s_ui_mailbox_mux);
        return;
    }
    s_pending_drawer_y = y;
    s_cached_drawer_y = y;
    s_pending_drawer_drag = true;
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    nuwax_lvgl_port_wake();
}

static void app_shell_post_drawer_begin_pull(void) {
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    if (s_ui_ready) {
        s_pending_drawer_begin_pull = true;
        s_cached_drawer_y = -226;
    }
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    nuwax_lvgl_port_wake();
}

static void app_shell_post_drawer_finish(bool close) {
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    if (!s_ui_ready) {
        taskEXIT_CRITICAL(&s_ui_mailbox_mux);
        return;
    }
    s_pending_drawer_finish = true;
    s_pending_drawer_finish_close = close;
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    nuwax_lvgl_port_wake();
}

static void app_shell_post_drawer_page_drag(int16_t delta_x) {
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    if (s_ui_ready) {
        s_pending_drawer_page_delta_x = delta_x;
        s_pending_drawer_page_drag = true;
    }
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    nuwax_lvgl_port_wake();
}

static void app_shell_post_drawer_page_finish(bool commit) {
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    if (s_ui_ready) {
        s_pending_drawer_page_finish = true;
        s_pending_drawer_page_commit = commit;
    }
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    nuwax_lvgl_port_wake();
}

static void app_shell_post_drawer_volume_preview(uint8_t volume) {
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    if (!s_ui_ready) {
        taskEXIT_CRITICAL(&s_ui_mailbox_mux);
        return;
    }
    s_pending_drawer_volume_preview = true;
    s_pending_drawer_volume = volume > 100 ? 100 : volume;
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    nuwax_lvgl_port_wake();
}

static void app_shell_lvgl_pre_handler(void *user_data) {
    (void)user_data;
    if (!nuwax_app_shell_is_ui_ready()) {
        return;
    }
    bool do_drag = false;
    int16_t drag_y = -226;
    bool do_begin_pull = false;
    bool do_finish = false;
    bool finish_close = false;
    bool do_page_drag = false;
    int16_t page_delta_x = 0;
    bool do_page_finish = false;
    bool page_commit = false;
    bool do_volume_preview = false;
    uint8_t volume_preview = 0;
    bool do_provisioning_qr = false;
    int64_t provisioning_qr_retry_deadline_us = 0;
    bool do_provisioning_qr_close = false;
    nuwax_device_state_t state_snapshot = {0};
    uint32_t state_revision = 0U;
    bool have_state_update = false;
    uint32_t rejected_actions = 0U;

    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    if (s_state_mailbox.initialized &&
        s_state_mailbox.revision != s_state_mailbox.published_revision) {
        state_snapshot = s_state_mailbox.snapshot;
        state_revision = s_state_mailbox.revision;
        have_state_update = true;
    }
    if (s_wifi_detail_mailbox.revision !=
        s_wifi_detail_mailbox.published_revision) {
        s_wifi_detail_mailbox.published_revision =
            s_wifi_detail_mailbox.revision;
    }
    if (s_status_detail_mailbox.revision !=
        s_status_detail_mailbox.published_revision) {
        s_status_detail_mailbox.published_revision =
            s_status_detail_mailbox.revision;
    }
    if (s_pending_menu_drag) {
        s_pending_menu_drag = false;
    }
    if (s_pending_menu_finish) {
        s_pending_menu_finish = false;
    }
    s_pending_detail_adjust = 0;
    if (s_pending_drawer_begin_pull) {
        do_begin_pull = true;
        s_pending_drawer_begin_pull = false;
    }
    if (s_pending_drawer_drag) {
        do_drag = true;
        drag_y = s_pending_drawer_y;
        s_pending_drawer_drag = false;
    }
    if (s_pending_drawer_finish) {
        do_finish = true;
        finish_close = s_pending_drawer_finish_close;
        s_pending_drawer_finish = false;
    }
    if (s_pending_drawer_page_drag) {
        do_page_drag = true;
        page_delta_x = s_pending_drawer_page_delta_x;
        s_pending_drawer_page_drag = false;
    }
    if (s_pending_drawer_page_finish) {
        do_page_finish = true;
        page_commit = s_pending_drawer_page_commit;
        s_pending_drawer_page_finish = false;
    }
    if (s_pending_drawer_volume_preview) {
        do_volume_preview = true;
        volume_preview = s_pending_drawer_volume;
        s_pending_drawer_volume_preview = false;
    }
    const int64_t now_us = esp_timer_get_time();
    if (s_pending_provisioning_qr &&
        now_us >= s_pending_provisioning_qr_deadline_us) {
        if (s_state_mailbox.initialized &&
            s_state_mailbox.snapshot.provisioning_active) {
            do_provisioning_qr = true;
            provisioning_qr_retry_deadline_us =
                now_us +
                (int64_t)APP_SHELL_PROVISIONING_QR_RETRY_MS * 1000LL;
            s_pending_provisioning_qr_deadline_us =
                provisioning_qr_retry_deadline_us;
        } else {
            s_pending_provisioning_qr = false;
            s_pending_provisioning_qr_deadline_us = 0;
        }
    }
    if (s_pending_provisioning_qr_close) {
        do_provisioning_qr_close = true;
        s_pending_provisioning_qr_close = false;
    }
    rejected_actions = s_drawer_action_reject_mask;
    s_drawer_action_reject_mask = 0U;
    s_pending_menu_scroll = 0;
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);

    if (have_state_update) {
        nuwax_device_state_set(&state_snapshot);
        taskENTER_CRITICAL(&s_ui_mailbox_mux);
        if (s_state_mailbox.published_revision != state_revision) {
            s_state_mailbox.published_revision = state_revision;
        }
        taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    }

    if (do_begin_pull && !nuwax_drawer_is_open()) {
        nuwax_drawer_begin_pull();
    }
    if (do_drag && nuwax_drawer_is_open()) {
        nuwax_drawer_drag_to(drag_y);
        app_shell_cached_drawer_y_set(nuwax_drawer_current_y());
    }
    if (do_finish && nuwax_drawer_is_open()) {
        nuwax_drawer_finish_drag(finish_close);
        app_shell_cached_drawer_y_set(nuwax_drawer_current_y());
    }
    if (do_page_drag && nuwax_drawer_is_open()) {
        nuwax_drawer_page_drag_to(page_delta_x);
    }
    if (do_page_finish && nuwax_drawer_is_open()) {
        nuwax_drawer_page_finish_drag(page_commit);
    }
    if (do_volume_preview && nuwax_drawer_is_open()) {
        nuwax_drawer_preview_volume(volume_preview);
    }
    if (do_provisioning_qr_close) {
        nuwax_drawer_hide_provisioning_qr();
    }
    if (do_provisioning_qr) {
        const bool shown = nuwax_drawer_show_provisioning_qr();
        if (shown) {
            taskENTER_CRITICAL(&s_ui_mailbox_mux);
            if (s_pending_provisioning_qr &&
                s_pending_provisioning_qr_deadline_us ==
                    provisioning_qr_retry_deadline_us) {
                s_pending_provisioning_qr = false;
                s_pending_provisioning_qr_deadline_us = 0;
            }
            taskEXIT_CRITICAL(&s_ui_mailbox_mux);
        }
    }
    if ((rejected_actions &
         app_shell_action_bit(NUWAX_APP_SHELL_ACTION_SET_MEETING_MODE)) != 0U) {
        nuwax_drawer_reject_action(NUWAX_DRAWER_ACTION_SET_MEETING_MODE);
    }
    if ((rejected_actions &
         app_shell_action_bit(NUWAX_APP_SHELL_ACTION_REPROVISION_WIFI)) != 0U) {
        nuwax_drawer_reject_action(NUWAX_DRAWER_ACTION_REPROVISION_WIFI);
    }
    if ((rejected_actions &
         app_shell_action_bit(NUWAX_APP_SHELL_ACTION_SET_VOLUME)) != 0U) {
        nuwax_drawer_reject_action(NUWAX_DRAWER_ACTION_SET_VOLUME);
    }
    const int64_t touch_drain_started_us = esp_timer_get_time();
    while (s_touch_event_queue &&
           esp_timer_get_time() - touch_drain_started_us <
               APP_SHELL_TOUCH_EVENT_DRAIN_BUDGET_US) {
        app_shell_touch_event_t queued = {0};
        if (xQueueReceive(s_touch_event_queue, &queued, 0) != pdTRUE) {
            break;
        }
        const int64_t age_ms = esp_timer_get_time() / 1000 - queued.queued_ms;
        if (age_ms > (int64_t)APP_SHELL_TOUCH_EVENT_QUEUE_WAIT_MS) {
            ESP_LOGW(TAG,
                     "delayed terminal touch event sequence=%lu event=%d age_ms=%lld",
                     (unsigned long)queued.sequence,
                     (int)queued.event,
                     (long long)age_ms);
        }
        const nuwax_app_shell_result_t result =
            app_shell_handle_touch_event_on_lvgl(
                queued.event,
                queued.has_point ? &queued.point : NULL);
        (void)app_shell_queue_result(&result);
    }
    if (s_touch_event_queue && uxQueueMessagesWaiting(s_touch_event_queue) > 0U) {
        nuwax_lvgl_port_wake();
    }
    if (nuwax_drawer_is_open()) {
        app_shell_cached_drawer_y_set(nuwax_drawer_current_y());
    }
    app_shell_overlay_state_sync_from_lvgl();
}

esp_err_t nuwax_app_shell_init(void) {
    if (nuwax_app_shell_is_ui_ready()) {
        return ESP_OK;
    }
    if (!s_touch_event_queue) {
        s_touch_event_queue = xQueueCreateStatic(
            APP_SHELL_TOUCH_EVENT_QUEUE_DEPTH,
            sizeof(app_shell_touch_event_t),
            s_touch_event_queue_buffer,
            &s_touch_event_queue_storage);
        if (!s_touch_event_queue) {
            ESP_LOGE(TAG, "terminal touch event queue creation failed");
            return ESP_ERR_NO_MEM;
        }
    }
    xQueueReset(s_touch_event_queue);
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    app_shell_ensure_state_snapshot_locked();
    app_shell_clear_transient_ui_locked();
    /* Rebuilds reset nuwax_device_state before registering its UI listener.
     * Publish the preserved canonical snapshot on the first LVGL tick even
     * when no producer changed state while the UI was absent. */
    (void)app_shell_next_state_revision_locked();
    s_wifi_detail_mailbox.published_revision = 0U;
    s_status_detail_mailbox.published_revision = 0U;
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    nuwax_lvgl_port_set_pre_handler_cb(app_shell_lvgl_pre_handler, NULL);
    app_shell_set_ui_ready(true);
    ESP_LOGI(TAG, "LVGL mode - shell bridge active");
    return ESP_OK;
}

void nuwax_app_shell_deinit(void) {
    /* Stop new UI work before detaching the callback. A callback already
     * copied by the LVGL task will observe s_ui_ready=false and return. */
    app_shell_set_ui_ready(false);
    nuwax_lvgl_port_set_pre_handler_cb(NULL, NULL);
    if (s_touch_event_queue) {
        xQueueReset(s_touch_event_queue);
    }

    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    app_shell_clear_transient_ui_locked();
    /* Canonical state and business actions survive a UI rebuild. Detail data
     * also survives, but is marked unpublished so a reopened detail view sees
     * the latest producer snapshot rather than stale widget-local state. */
    if (s_state_mailbox.initialized) {
        s_state_mailbox.published_revision = 0U;
    }
    s_wifi_detail_mailbox.published_revision = 0U;
    s_status_detail_mailbox.published_revision = 0U;
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    ESP_LOGI(TAG, "LVGL shell bridge inactive");
}

bool nuwax_app_shell_is_menu_active(void) {
    if (!nuwax_app_shell_is_ui_ready()) {
        return false;
    }
    const app_shell_overlay_state_t state = app_shell_overlay_state_get();
    return state.menu_open || state.drawer_open;
}

bool nuwax_app_shell_blocks_auto_followup(void) {
    if (!nuwax_app_shell_is_ui_ready()) {
        return false;
    }
    const app_shell_overlay_state_t state = app_shell_overlay_state_get();
    return state.menu_open || state.drawer_open;
}

bool nuwax_app_shell_can_start_listen(void) {
    if (!nuwax_app_shell_is_ui_ready()) {
        return true;
    }
    const app_shell_overlay_state_t state = app_shell_overlay_state_get();
    return !state.menu_open && !state.drawer_open;
}

void nuwax_app_shell_close_overlays(void) {
    if (!nuwax_app_shell_is_ui_ready()) {
        return;
    }
    if (!nuwax_lvgl_port_lock(500)) {
        ESP_LOGW(TAG, "failed to close overlays before sleep");
        return;
    }
    if (!nuwax_app_shell_is_ui_ready()) {
        nuwax_lvgl_port_unlock();
        return;
    }
    if (nuwax_drawer_is_open()) {
        nuwax_drawer_close();
        app_shell_cached_drawer_y_set(nuwax_drawer_current_y());
    }
    app_shell_lvgl_unlock();
}

bool nuwax_app_shell_drawer_is_open(void) {
    if (!nuwax_app_shell_is_ui_ready()) {
        return false;
    }
    return app_shell_overlay_state_get().drawer_open;
}

bool nuwax_app_shell_drawer_is_detail(void) {
    if (!nuwax_app_shell_is_ui_ready()) return false;
    return app_shell_overlay_state_get().menu_detail;
}

void nuwax_app_shell_drawer_begin_pull(void) {
    if (!nuwax_app_shell_is_ui_ready()) return;
    app_shell_post_drawer_begin_pull();
}

int16_t nuwax_app_shell_drawer_current_y(void) {
    int16_t y = app_shell_cached_drawer_y_get();
    if (!nuwax_app_shell_is_ui_ready()) {
        return y;
    }
    if (!nuwax_lvgl_port_lock(20)) {
        return y;
    }
    if (!nuwax_app_shell_is_ui_ready()) {
        nuwax_lvgl_port_unlock();
        return y;
    }
    y = nuwax_drawer_current_y();
    app_shell_cached_drawer_y_set(y);
    app_shell_lvgl_unlock();
    return y;
}

bool nuwax_app_shell_drawer_preview_volume_at(uint16_t x, uint16_t y, uint8_t *volume_out) {
    if (!nuwax_app_shell_is_ui_ready()) {
        return false;
    }
    if (!app_shell_overlay_state_get().drawer_open) {
        return false;
    }
    uint8_t volume = 0;
    if (!nuwax_drawer_hit_volume_at_panel_y(x, y, app_shell_cached_drawer_y_get(), &volume)) {
        return false;
    }
    if (volume_out) {
        *volume_out = volume;
    }
    app_shell_post_drawer_volume_preview(volume);
    return true;
}

void nuwax_app_shell_drawer_drag_to(int16_t y) {
    if (!nuwax_app_shell_is_ui_ready()) {
        return;
    }
    app_shell_post_drawer_drag(y);
}

void nuwax_app_shell_drawer_finish_drag(bool close) {
    if (!nuwax_app_shell_is_ui_ready()) {
        return;
    }
    app_shell_post_drawer_finish(close);
}

void nuwax_app_shell_drawer_page_drag_to(int16_t delta_x) {
    if (!nuwax_app_shell_is_ui_ready()) return;
    app_shell_post_drawer_page_drag(delta_x);
}

void nuwax_app_shell_drawer_page_finish_drag(bool commit) {
    if (!nuwax_app_shell_is_ui_ready()) return;
    app_shell_post_drawer_page_finish(commit);
}

bool nuwax_app_shell_focus_menu_main_is_open(void) {
    return false;
}

bool nuwax_app_shell_focus_menu_detail_is_open(void) {
    return false;
}

void nuwax_app_shell_menu_drag_to(int16_t delta_y) {
    if (!nuwax_app_shell_is_ui_ready()) {
        return;
    }
    app_shell_post_menu_drag(delta_y);
}

void nuwax_app_shell_menu_finish_drag(void) {
    if (!nuwax_app_shell_is_ui_ready()) {
        return;
    }
    app_shell_post_menu_finish();
}

void nuwax_app_shell_menu_detail_adjust(int direction) {
    if (!nuwax_app_shell_is_ui_ready()) {
        return;
    }
    app_shell_post_detail_adjust(direction);
}

void nuwax_app_shell_update(uint32_t now_ms) {
    nuwax_state_bridge_service(now_ms);
    if (!nuwax_app_shell_is_ui_ready() ||
        (uint32_t)(now_ms - s_wifi_signal_last_poll_ms) <
            APP_SHELL_WIFI_SIGNAL_POLL_MS) {
        return;
    }
    s_wifi_signal_last_poll_ms = now_ms;
    int8_t rssi_dbm = -127;
    nuwax_app_shell_set_wifi_signal(
        nuwax_wifi_get_rssi(&rssi_dbm) == ESP_OK, rssi_dbm);
}
void nuwax_app_shell_render_status_bar(void) {}

void nuwax_app_shell_set_app_state(nuwax_app_state_t state) {
    nuwax_state_bridge_update(state);
}

void nuwax_app_shell_set_bridge_status(bool connected) {
    nuwax_app_shell_set_cloud_state(connected ? NUWAX_CLOUD_CONNECTED
                                              : NUWAX_CLOUD_DISCONNECTED);
}

void nuwax_app_shell_set_cloud_state(nuwax_cloud_state_t state) {
    if ((unsigned)state > (unsigned)NUWAX_CLOUD_AUTHFAIL) {
        state = NUWAX_CLOUD_DISCONNECTED;
    }
    nuwax_device_state_t patch = {.cloud = state};
    if (state != NUWAX_CLOUD_CONNECTED) {
        nuwax_state_bridge_clear_semantic_emotion();
        nuwax_state_bridge_clear_playback();
        nuwax_state_bridge_cancel_touch();
    }
    (void)nuwax_app_shell_publish_device_state_delta(
        &patch, NUWAX_APP_SHELL_STATE_CLOUD);
}

void nuwax_app_shell_set_wifi_status(const char *ssid, const char *ip) {
    nuwax_device_state_t patch = {0};
    patch.wifi = (ssid && ssid[0]) ? NUWAX_WIFI_CONNECTED
                                   : NUWAX_WIFI_DISCONNECTED;
    strlcpy(patch.ssid, ssid ? ssid : "", sizeof(patch.ssid));
    strlcpy(patch.ip, ip ? ip : "", sizeof(patch.ip));
    (void)nuwax_app_shell_publish_device_state_delta(
        &patch, NUWAX_APP_SHELL_STATE_WIFI);
    if (!ssid || !ssid[0]) {
        nuwax_app_shell_set_wifi_signal(false, -127);
    }
}

void nuwax_app_shell_set_wifi_signal(bool valid, int8_t rssi_dbm) {
    nuwax_device_state_t patch = {0};
    patch.wifi_signal_valid = valid;
    patch.wifi_rssi_dbm = valid ? rssi_dbm : -127;
    if (!valid) {
        patch.wifi_signal = NUWAX_WIFI_SIGNAL_UNKNOWN;
    } else if (rssi_dbm >= -55) {
        patch.wifi_signal = NUWAX_WIFI_SIGNAL_STRONG;
    } else if (rssi_dbm >= -67) {
        patch.wifi_signal = NUWAX_WIFI_SIGNAL_FAIR;
    } else {
        patch.wifi_signal = NUWAX_WIFI_SIGNAL_WEAK;
    }
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    app_shell_ensure_state_snapshot_locked();
    const nuwax_device_state_t *current = &s_state_mailbox.snapshot;
    const bool unchanged =
        current->wifi_signal_valid == patch.wifi_signal_valid &&
        current->wifi_rssi_dbm == patch.wifi_rssi_dbm &&
        current->wifi_signal == patch.wifi_signal;
    if (!unchanged) {
        app_shell_merge_device_state_locked(
            &patch, NUWAX_APP_SHELL_STATE_WIFI_SIGNAL);
        (void)app_shell_next_state_revision_locked();
    }
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
}

void nuwax_app_shell_set_wifi_connecting(const char *ssid) {
    nuwax_device_state_t patch = {.wifi = NUWAX_WIFI_CONNECTING};
    strlcpy(patch.ssid, ssid ? ssid : "", sizeof(patch.ssid));
    (void)nuwax_app_shell_publish_device_state_delta(
        &patch, NUWAX_APP_SHELL_STATE_WIFI);
    nuwax_app_shell_set_wifi_signal(false, -127);
    app_shell_wifi_detail_t detail = {
        .kind = APP_SHELL_WIFI_DETAIL_CONNECTING,
    };
    strlcpy(detail.ssid, ssid ? ssid : "", sizeof(detail.ssid));
    app_shell_post_wifi_detail(&detail);
}

void nuwax_app_shell_set_wifi_result(const char *ssid, bool ok, const char *message) {
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    app_shell_ensure_state_snapshot_locked();
    nuwax_device_state_t patch = s_state_mailbox.snapshot;
    patch.wifi = ok ? NUWAX_WIFI_CONNECTED : NUWAX_WIFI_DISCONNECTED;
    if (ssid) {
        strlcpy(patch.ssid, ssid, sizeof(patch.ssid));
    }
    if (!ok) {
        patch.ip[0] = '\0';
    }
    app_shell_merge_device_state_locked(&patch, NUWAX_APP_SHELL_STATE_WIFI);
    (void)app_shell_next_state_revision_locked();
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    if (!ok) {
        nuwax_app_shell_set_wifi_signal(false, -127);
    }
    app_shell_wifi_detail_t detail = {
        .kind = APP_SHELL_WIFI_DETAIL_RESULT,
        .ok = ok,
    };
    strlcpy(detail.ssid, ssid ? ssid : "", sizeof(detail.ssid));
    strlcpy(detail.message, message ? message : "",
            sizeof(detail.message));
    app_shell_post_wifi_detail(&detail);
}

void nuwax_app_shell_set_volume_percent(int volume_percent) {
    app_shell_post_volume((uint8_t)(volume_percent < 0
                                        ? 0
                                        : (volume_percent > 100
                                               ? 100
                                               : volume_percent)));
}

void nuwax_app_shell_set_meeting_state(nuwax_meeting_ui_state_t state) {
    if (state > NUWAX_MEETING_UI_ERROR) {
        state = NUWAX_MEETING_UI_ERROR;
    }
    if (state != NUWAX_MEETING_UI_DISABLED) {
        nuwax_state_bridge_clear_semantic_emotion();
        nuwax_state_bridge_clear_playback();
        nuwax_state_bridge_cancel_touch();
    }
    app_shell_post_meeting(state);
}

void nuwax_app_shell_set_monitoring_state(nuwax_monitoring_ui_state_t state) {
    if (state > NUWAX_MONITORING_UI_ERROR) {
        state = NUWAX_MONITORING_UI_ERROR;
    }
    if (state != NUWAX_MONITORING_UI_DISABLED) {
        nuwax_state_bridge_clear_semantic_emotion();
        nuwax_state_bridge_clear_playback();
        nuwax_state_bridge_cancel_touch();
    }
    app_shell_post_monitoring(state);
}

void nuwax_app_shell_set_provisioning_active(bool active) {
    if (active) {
        nuwax_state_bridge_clear_semantic_emotion();
        nuwax_state_bridge_clear_playback();
        nuwax_state_bridge_cancel_touch();
    }
    nuwax_device_state_t patch = {.provisioning_active = active};
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    app_shell_ensure_state_snapshot_locked();
    const bool was_active =
        s_state_mailbox.snapshot.provisioning_active;
    app_shell_merge_device_state_locked(
        &patch, NUWAX_APP_SHELL_STATE_PROVISIONING);
    if (active) {
        s_pending_provisioning_qr_close = false;
    } else {
        s_pending_provisioning_qr = false;
        s_pending_provisioning_qr_deadline_us = 0;
        if (was_active) {
            s_pending_provisioning_qr_close = true;
        }
    }
    (void)app_shell_next_state_revision_locked();
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    nuwax_lvgl_port_wake();
}

void nuwax_app_shell_set_ota_progress(bool active, int percent) {
    const uint8_t bounded_percent = active
                                        ? (uint8_t)(percent < 0
                                                        ? 0
                                                        : (percent > 100
                                                               ? 100
                                                               : percent))
                                        : 0U;
    nuwax_device_state_t patch = {
        .ota_update_active = active,
        .ota_update_percent = bounded_percent,
    };
    bool changed = false;
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    app_shell_ensure_state_snapshot_locked();
    changed = s_state_mailbox.snapshot.ota_update_active != active ||
              s_state_mailbox.snapshot.ota_update_percent != bounded_percent;
    if (changed) {
        app_shell_merge_device_state_locked(
            &patch, NUWAX_APP_SHELL_STATE_OTA_PROGRESS);
        (void)app_shell_next_state_revision_locked();
    }
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    if (changed) {
        nuwax_lvgl_port_wake();
    }
}

void nuwax_app_shell_schedule_provisioning_qr(uint32_t delay_ms) {
    const int64_t now_us = esp_timer_get_time();
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    app_shell_ensure_state_snapshot_locked();
    if (s_ui_ready && s_state_mailbox.snapshot.provisioning_active) {
        s_pending_provisioning_qr = true;
        s_pending_provisioning_qr_deadline_us =
            now_us + (int64_t)delay_ms * 1000LL;
    }
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    nuwax_lvgl_port_wake();
}

void nuwax_app_shell_trigger_touch_animation(void) {
    nuwax_state_bridge_trigger_touch();
}

void nuwax_app_shell_set_battery_state(bool valid,
                                       bool soc_trusted,
                                       nuwax_power_state_t power,
                                       uint8_t percent,
                                       uint16_t voltage_mv,
                                       int16_t current_ma) {
    if ((unsigned)power > (unsigned)NUWAX_POWER_LOW) {
        power = NUWAX_POWER_UNKNOWN;
        valid = false;
    }
    if (!valid) {
        power = NUWAX_POWER_UNKNOWN;
        soc_trusted = false;
    }
    nuwax_device_state_t patch = {0};
    patch.battery_valid = valid;
    patch.battery_soc_trusted = valid && soc_trusted;
    patch.power = power;
    patch.battery_low = valid && power == NUWAX_POWER_LOW;
    if (valid) {
        patch.battery_percent = percent > 100U ? 100U : percent;
        patch.battery_voltage_mv = voltage_mv;
        patch.battery_current_ma = current_ma;
    }
    (void)nuwax_app_shell_publish_device_state_delta(
        &patch, NUWAX_APP_SHELL_STATE_BATTERY);
}

void nuwax_app_shell_set_wifi_scan_busy(void) {
    const app_shell_wifi_detail_t detail = {
        .kind = APP_SHELL_WIFI_DETAIL_SCAN_BUSY,
    };
    app_shell_post_wifi_detail(&detail);
}

void nuwax_app_shell_set_wifi_networks(const char ssids[][33], const int8_t *rssis, const uint8_t *authmodes, int count) {
    app_shell_wifi_detail_t detail = {
        .kind = APP_SHELL_WIFI_DETAIL_NETWORKS,
    };
    if (count < 0 || !ssids) {
        count = 0;
    } else if (count > APP_SHELL_WIFI_MAX_ITEMS) {
        count = APP_SHELL_WIFI_MAX_ITEMS;
    }
    detail.count = count;
    for (int i = 0; i < count; ++i) {
        strlcpy(detail.ssids[i], ssids[i], sizeof(detail.ssids[i]));
        detail.rssis[i] = rssis ? rssis[i] : 0;
        detail.authmodes[i] = authmodes ? authmodes[i] : 0;
    }
    app_shell_post_wifi_detail(&detail);
}

void nuwax_app_shell_set_saved_wifi_networks(const char ssids[][33], int count) {
    app_shell_wifi_detail_t detail = {
        .kind = APP_SHELL_WIFI_DETAIL_SAVED_NETWORKS,
    };
    if (count < 0 || !ssids) {
        count = 0;
    } else if (count > APP_SHELL_WIFI_MAX_ITEMS) {
        count = APP_SHELL_WIFI_MAX_ITEMS;
    }
    detail.count = count;
    for (int i = 0; i < count; ++i) {
        strlcpy(detail.ssids[i], ssids[i], sizeof(detail.ssids[i]));
    }
    app_shell_post_wifi_detail(&detail);
}

void nuwax_app_shell_set_status_card(const char *title, const char *line1, const char *line2, const char *line3) {
    char lines[3][48] = {0};
    if (line1) { strncpy(lines[0], line1, sizeof(lines[0]) - 1); }
    if (line2) { strncpy(lines[1], line2, sizeof(lines[1]) - 1); }
    if (line3) { strncpy(lines[2], line3, sizeof(lines[2]) - 1); }
    nuwax_app_shell_set_status_card_lines(title, lines, 3, "");
}

void nuwax_app_shell_set_status_card_lines(const char *title, const char lines[][48], int line_count, const char *text) {
    app_shell_status_detail_t detail = {0};
    strlcpy(detail.title, title ? title : "", sizeof(detail.title));
    if (line_count < 0 || !lines) {
        line_count = 0;
    } else if (line_count > APP_SHELL_STATUS_LINE_COUNT) {
        line_count = APP_SHELL_STATUS_LINE_COUNT;
    }
    detail.line_count = line_count;
    for (int i = 0; i < line_count; ++i) {
        strlcpy(detail.lines[i], lines[i], sizeof(detail.lines[i]));
    }
    strlcpy(detail.text, text ? text : "", sizeof(detail.text));
    app_shell_post_status_detail(&detail);
}

void nuwax_app_shell_set_conversation_status(const char *title, const char *phase, const char *text, bool show_now) {
    (void)text; (void)show_now;
    if (!phase) return;
    nuwax_dialog_state_t dialog = NUWAX_DIALOG_READY;
    const bool sleep_request =
        (title && phase_equals(title, "sleep")) || phase_equals(phase, "sleep");
    if (sleep_request) {
        dialog = NUWAX_DIALOG_SLEEP;
    } else if (phase_equals(phase, "error")) {
        dialog = NUWAX_DIALOG_ERROR;
    } else if (phase_equals(phase, "listening") ||
               phase_equals(phase, "wake") ||
               phase_equals(phase, "cancel")) {
        dialog = NUWAX_DIALOG_LISTENING;
    } else if (phase_equals(phase, "final") ||
               phase_equals(phase, "recognizing")) {
        dialog = NUWAX_DIALOG_RECOGNIZING;
    } else if (phase_equals(phase, "thinking") ||
               phase_equals(phase, "answer")) {
        dialog = NUWAX_DIALOG_THINKING;
    } else if (phase_equals(phase, "speaking")) {
        dialog = NUWAX_DIALOG_SPEAKING;
    } else {
        return;
    }
    app_shell_post_dialog(dialog);
}

static nuwax_app_shell_result_t drawer_action_to_shell_action(
    nuwax_drawer_action_result_t drawer_action) {
    nuwax_app_shell_result_t result = {.action = NUWAX_APP_SHELL_ACTION_NONE, .index = 0};
    switch (drawer_action.action) {
        case NUWAX_DRAWER_ACTION_SET_VOLUME:
            result.action = NUWAX_APP_SHELL_ACTION_SET_VOLUME;
            result.index = drawer_action.value;
            break;
        case NUWAX_DRAWER_ACTION_SET_MEETING_MODE:
            result.action = NUWAX_APP_SHELL_ACTION_SET_MEETING_MODE;
            result.index = drawer_action.value ? 1 : 0;
            break;
        case NUWAX_DRAWER_ACTION_REPROVISION_WIFI:
            result.action = NUWAX_APP_SHELL_ACTION_REPROVISION_WIFI;
            result.index = drawer_action.value ? 1 : 0;
            break;
        case NUWAX_DRAWER_ACTION_NONE:
        default:
            break;
    }
    return result;
}

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

nuwax_app_shell_result_t nuwax_app_shell_handle_touch_event(nuwax_app_shell_event_t event, const nuwax_touch_point_t *point) {
    nuwax_app_shell_result_t result = {.action = NUWAX_APP_SHELL_ACTION_NONE, .index = 0};

    if (!nuwax_app_shell_is_ui_ready()) {
        if (event == NUWAX_APP_SHELL_EVENT_PRESS ||
            event == NUWAX_APP_SHELL_EVENT_LONG_PRESS) {
            result.action = app_shell_hold_action();
        }
        return result;
    }
    const app_shell_overlay_state_t overlay =
        app_shell_overlay_state_get();

    if (overlay.menu_open && !overlay.menu_detail &&
        (event == NUWAX_APP_SHELL_EVENT_SWIPE_UP || event == NUWAX_APP_SHELL_EVENT_SWIPE_DOWN)) {
        app_shell_post_menu_scroll(event == NUWAX_APP_SHELL_EVENT_SWIPE_UP ? 1 : -1);
        return result;
    }

    if ((event == NUWAX_APP_SHELL_EVENT_PRESS || event == NUWAX_APP_SHELL_EVENT_LONG_PRESS) &&
        !overlay.menu_open && !overlay.drawer_open) {
        result.action = app_shell_hold_action();
        return result;
    }

    if (!s_touch_event_queue) {
        ESP_LOGE(TAG,
                 "terminal touch event rejected before queue initialization event=%d",
                 (int)event);
        return result;
    }
    app_shell_touch_event_t queued = {
        .event = event,
        .has_point = point != NULL,
        .queued_ms = esp_timer_get_time() / 1000,
    };
    if (point) {
        queued.point = *point;
    }
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    queued.sequence = app_shell_next_revision(s_touch_event_sequence);
    s_touch_event_sequence = queued.sequence;
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    if (xQueueSend(s_touch_event_queue,
                   &queued,
                   pdMS_TO_TICKS(APP_SHELL_TOUCH_EVENT_QUEUE_WAIT_MS)) !=
        pdTRUE) {
        ESP_LOGE(TAG,
                 "terminal touch event queue saturated sequence=%lu event=%d depth=%u; event was not overwritten",
                 (unsigned long)queued.sequence,
                 (int)event,
                 (unsigned)APP_SHELL_TOUCH_EVENT_QUEUE_DEPTH);
        return result;
    }
    nuwax_lvgl_port_wake();
    return result;
}

static nuwax_app_shell_result_t app_shell_handle_touch_event_on_lvgl(
    nuwax_app_shell_event_t event,
    const nuwax_touch_point_t *point) {
    nuwax_app_shell_result_t result = {
        .action = NUWAX_APP_SHELL_ACTION_NONE,
        .index = 0,
    };
    const uint16_t y = point ? point->y : 0;
    if (!nuwax_lvgl_port_lock_is_held_by_current_task()) {
        ESP_LOGE(TAG,
                 "terminal touch event reached UI handler without LVGL ownership event=%d",
                 (int)event);
        return result;
    }
    if (!nuwax_app_shell_is_ui_ready()) {
        return result;
    }

    if (nuwax_drawer_is_open()) {
        if (event == NUWAX_APP_SHELL_EVENT_TAP ||
            event == NUWAX_APP_SHELL_EVENT_PRESS ||
            event == NUWAX_APP_SHELL_EVENT_SWIPE_LEFT ||
            event == NUWAX_APP_SHELL_EVENT_SWIPE_RIGHT ||
            event == NUWAX_APP_SHELL_EVENT_SWIPE_UP ||
            event == NUWAX_APP_SHELL_EVENT_SWIPE_DOWN) {
            uint8_t volume = 0;
            if (point && nuwax_drawer_hit_volume(point->x, point->y, &volume)) {
                nuwax_drawer_preview_volume(volume);
                result.action = NUWAX_APP_SHELL_ACTION_SET_VOLUME;
                result.index = volume;
                ESP_LOGD(TAG, "drawer volume touch x=%u y=%u volume=%u",
                         point->x,
                         point->y,
                         (unsigned)volume);
                return result;
            }
        }
        if (nuwax_drawer_is_detail()) {
            if (event == NUWAX_APP_SHELL_EVENT_SWIPE_RIGHT) {
                nuwax_drawer_back();
            } else if (event == NUWAX_APP_SHELL_EVENT_SWIPE_UP) {
                nuwax_drawer_finish_drag(true);
                app_shell_cached_drawer_y_set(nuwax_drawer_current_y());
            } else if (event == NUWAX_APP_SHELL_EVENT_TAP) {
                result = drawer_action_to_shell_action(
                    nuwax_drawer_tap(point ? point->x : 0, y));
            }
            return result;
        }
        if (event == NUWAX_APP_SHELL_EVENT_SWIPE_LEFT) {
            nuwax_drawer_next_page();
        } else if (event == NUWAX_APP_SHELL_EVENT_SWIPE_RIGHT) {
            nuwax_drawer_prev_page();
        } else if (event == NUWAX_APP_SHELL_EVENT_SWIPE_UP) {
            nuwax_drawer_finish_drag(true);
            app_shell_cached_drawer_y_set(nuwax_drawer_current_y());
        } else if (event == NUWAX_APP_SHELL_EVENT_TAP) {
            result = drawer_action_to_shell_action(
                nuwax_drawer_tap(point ? point->x : 0, y));
        }
        return result;
    }

    if (event == NUWAX_APP_SHELL_EVENT_SWIPE_DOWN && y < NUWAX_GESTURE_TOP_BAND) {
        nuwax_drawer_open_menu();
        app_shell_cached_drawer_y_set(nuwax_drawer_current_y());
    }
#if CONFIG_NUWAX_FACE_TEST_MODE_ENABLE
    else if (event == NUWAX_APP_SHELL_EVENT_SWIPE_RIGHT) {
        bool accepted = nuwax_face_next_expression();
        ESP_LOGI(TAG,
                 "home swipe right -> next emotion expression %s",
                 accepted ? "accepted" : "ignored");
    }
#endif
    else if (event == NUWAX_APP_SHELL_EVENT_PRESS || event == NUWAX_APP_SHELL_EVENT_LONG_PRESS) {
        result.action = app_shell_hold_action();
    }

    return result;
}

/* Queue action to main loop from LVGL context */
bool nuwax_app_shell_request_action(nuwax_app_shell_action_t action, int value) {
    nuwax_app_shell_result_t r = {.action = action, .index = value};
    return app_shell_queue_result(&r);
}
