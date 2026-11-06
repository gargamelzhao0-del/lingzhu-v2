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
#include "nuwax_ui_v28/ota_progress.h"
#include "nuwax_state_bridge.h"
#include "nuwax_ui_v28/ui_bridge_revision_policy.h"
#include "nuwax_ui_v28/control_center_input_policy.h"
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
#define APP_SHELL_PROVISIONING_FACE_HOLD_MS 3000U

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
static void app_shell_cancel_touch_locked(void);
static void app_shell_cancel_volume_preview_locked(void);

typedef struct {
    uint32_t contact_sequence;
    int16_t delta_y;
    bool begin_pull;
} app_shell_panel_motion_t;

static portMUX_TYPE s_ui_mailbox_mux = portMUX_INITIALIZER_UNLOCKED;
static int8_t s_pending_menu_scroll;
static bool s_pending_menu_drag;
static int16_t s_pending_menu_drag_delta_y;
static bool s_pending_menu_finish;
static int8_t s_pending_detail_adjust;
static bool s_pending_drawer_contact, s_drawer_contact_down;
static uint16_t s_drawer_contact_x, s_drawer_contact_y;
static app_shell_panel_motion_t s_pending_panel_motion;
static app_shell_panel_motion_t s_touch_panel_motion;
static bool s_pending_close_overlays;
static bool s_system_closing;
static bool s_pending_drawer_page_drag;
static int16_t s_pending_drawer_page_delta_x;
static bool s_pending_provisioning_qr;
static int64_t s_pending_provisioning_qr_deadline_us;
static bool s_pending_provisioning_qr_close;
static bool s_ui_ready;
static QueueHandle_t s_shell_actions;
static TaskHandle_t s_shell_action_owner;
static nuwax_app_shell_reprovision_cb_t s_reprovision_cb;
static void *s_reprovision_ctx;
static nuwax_drawer_input_state_t s_drawer_input;
static nuwax_drawer_input_state_t s_touch_input;
static uint32_t s_touch_contact_sequence, s_touch_completed_sequence;
static uint32_t s_touch_cancelled_sequence;
static uint32_t s_visual_contact_sequence;
static uint32_t s_contact_sequence, s_page_sequence;
static uint32_t s_panel_anchor_sequence;
static int16_t s_panel_anchor_y;
static bool s_touch_page_moved;
static int16_t s_touch_last_page_dx;
static nuwax_app_shell_volume_preview_cb_t s_volume_preview_cb;
static nuwax_app_shell_volume_cancel_cb_t s_volume_cancel_cb;
static nuwax_app_shell_volume_queued_cb_t s_volume_queued_cb;
static uint32_t s_volume_setting_sequence;
static struct {
    uint32_t contact_sequence;
    uint32_t volume_sequence;
    uint8_t value;
} s_volume_preview;

typedef enum {
    APP_SHELL_INPUT_TOUCH,
    APP_SHELL_INPUT_PANEL_FINISH,
    APP_SHELL_INPUT_PAGE_FINISH,
    APP_SHELL_INPUT_VOLUME_FINISH,
} app_shell_input_kind_t;

typedef struct {
    app_shell_input_kind_t kind;
    uint32_t contact_sequence;
    uint32_t scene_generation;
    uint32_t volume_sequence;
    bool finish_target, begin_pull, has_motion;
    bool tap_allowed;
    int16_t final_position;
    int16_t panel_total_dy;
    int16_t panel_velocity_y;
    nuwax_app_shell_event_t event;
    nuwax_touch_point_t point;
    bool has_point;
} app_shell_touch_event_t;

static StaticQueue_t s_touch_event_queue_storage;
static uint8_t s_touch_event_queue_buffer[
    APP_SHELL_TOUCH_EVENT_QUEUE_DEPTH * sizeof(app_shell_touch_event_t)];
static QueueHandle_t s_touch_event_queue;

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
    uint32_t bridge_field_revisions[17];
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

/* Must run on the LVGL task or while the LVGL port mutex is held. */
static void app_shell_overlay_state_sync_from_lvgl(void) {
    const app_shell_overlay_state_t state = {
        .menu_open = false,
        .menu_detail = nuwax_drawer_is_detail(),
        .drawer_open = nuwax_drawer_is_open(),
    };
    nuwax_drawer_input_state_t input;
    nuwax_drawer_get_input_state(&input);
    input.modal_blocked = input.modal_blocked || nuwax_ota_progress_is_visible();
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    s_overlay_state = state;
    if (input.modal_blocked || s_drawer_input.modal_blocked) {
        app_shell_cancel_touch_locked();
    } else if (!input.volume_available ||
               input.scene_generation != s_drawer_input.scene_generation) {
        app_shell_cancel_volume_preview_locked();
    }
    s_drawer_input = input;
    if (!state.drawer_open && !s_pending_close_overlays) {
        s_system_closing = false;
    }
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
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
    s_shell_action_owner = queue ? xTaskGetCurrentTaskHandle() : NULL;
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
}

void nuwax_app_shell_action_queued(void) {
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    const TaskHandle_t owner = s_shell_action_owner;
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    /* The registered business owner waits on notifications between drains.
     * Queue admission must wake it, rather than wait out its 100 ms idle poll. */
    if (owner) xTaskNotifyGive(owner);
}

void nuwax_app_shell_set_reprovision_callback(
    nuwax_app_shell_reprovision_cb_t callback,
    void *ctx) {
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    s_reprovision_cb = callback;
    s_reprovision_ctx = ctx;
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
}

void nuwax_app_shell_set_volume_callbacks(
    nuwax_app_shell_volume_preview_cb_t preview,
    nuwax_app_shell_volume_cancel_cb_t cancel,
    nuwax_app_shell_volume_queued_cb_t queued) {
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    s_volume_preview_cb = preview;
    s_volume_cancel_cb = cancel;
    s_volume_queued_cb = queued;
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

static bool app_shell_touch_allowed_locked(uint32_t sequence) {
    return !s_system_closing && !s_drawer_input.modal_blocked &&
           !s_state_mailbox.snapshot.ota_update_active &&
           nuwax_cc_sequence_after(sequence, s_touch_cancelled_sequence);
}

static void app_shell_clear_volume_preview_locked(void) {
    if (s_volume_preview.contact_sequence == 0U) return;
    memset(&s_volume_preview, 0, sizeof(s_volume_preview));
    (void)app_shell_next_state_revision_locked();
}

static void app_shell_restore_volume_locked(uint32_t sequence) {
    if (s_volume_cancel_cb && sequence != 0U) {
        s_volume_cancel_cb(sequence);
    }
}

static void app_shell_cancel_volume_preview_locked(void) {
    app_shell_restore_volume_locked(s_volume_preview.volume_sequence);
    app_shell_clear_volume_preview_locked();
}

static void app_shell_cancel_touch_locked(void) {
    s_touch_cancelled_sequence = s_touch_contact_sequence;
    s_pending_drawer_contact = false;
    s_drawer_contact_down = false;
    s_pending_panel_motion.contact_sequence = 0U;
    s_touch_panel_motion.contact_sequence = 0U;
    s_pending_drawer_page_drag = false;
    s_touch_page_moved = false;
    app_shell_cancel_volume_preview_locked();
}

static void app_shell_clear_transient_ui_locked(void) {
    app_shell_cancel_touch_locked();
    s_pending_menu_scroll = 0;
    s_pending_drawer_contact = false;
    s_drawer_contact_down = false;
    s_pending_menu_drag = false;
    s_pending_menu_drag_delta_y = 0;
    s_pending_menu_finish = false;
    s_pending_detail_adjust = 0;
    s_pending_close_overlays = false;
    s_system_closing = false;
    s_pending_drawer_page_delta_x = 0;
    s_pending_provisioning_qr = false;
    s_pending_provisioning_qr_deadline_us = 0;
    s_pending_provisioning_qr_close = false;
    memset(&s_drawer_input, 0, sizeof(s_drawer_input));
    s_panel_anchor_sequence = 0U;
    s_touch_completed_sequence = s_touch_contact_sequence;
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
    if ((fields & NUWAX_APP_SHELL_STATE_BRIGHTNESS) != 0U) {
        state->brightness_percent = patch->brightness_percent == 40U || patch->brightness_percent == 70U ? patch->brightness_percent : 100U;
    }
    if ((fields & NUWAX_APP_SHELL_STATE_PROVISIONING) != 0U) {
        state->provisioning_active = patch->provisioning_active;
        state->provisioning_starting = patch->provisioning_starting;
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
    if ((fields & NUWAX_APP_SHELL_STATE_SIMULATED_OFFLINE) != 0U) {
        state->simulated_offline = patch->simulated_offline;
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

static void app_shell_post_volume_locked(uint8_t volume, uint32_t sequence) {
    if (s_volume_setting_sequence != 0U &&
        (sequence == 0U || nuwax_cc_sequence_after(s_volume_setting_sequence, sequence))) {
        return;
    }
    s_volume_setting_sequence = sequence;
    app_shell_ensure_state_snapshot_locked();
    if (s_state_mailbox.snapshot.volume == volume) return;
    nuwax_device_state_t patch = {.volume = volume};
    app_shell_merge_device_state_locked(&patch, NUWAX_APP_SHELL_STATE_VOLUME);
    (void)app_shell_next_state_revision_locked();
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
        /* Keep the latest admitted target available for a later rollback.
         * An owner confirmation can arrive before this producer resumes. */
        taskENTER_CRITICAL(&s_ui_mailbox_mux);
        if (s_volume_queued_cb) {
            s_volume_queued_cb(result->index, result->volume_sequence);
        }
        app_shell_post_volume_locked((uint8_t)(result->index < 0 ? 0 :
            result->index > 100 ? 100 : result->index), result->volume_sequence);
        taskEXIT_CRITICAL(&s_ui_mailbox_mux);
        nuwax_lvgl_port_wake();
    }
    nuwax_app_shell_action_queued();
    return true;
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

static bool app_shell_enqueue_input(app_shell_touch_event_t *queued) {
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    if (!s_ui_ready || !app_shell_touch_allowed_locked(s_touch_contact_sequence)) {
        taskEXIT_CRITICAL(&s_ui_mailbox_mux);
        return false;
    }
    queued->contact_sequence = s_touch_contact_sequence;
    queued->scene_generation = s_touch_input.scene_generation;
    queued->tap_allowed = s_touch_input.tap_allowed;
    if (queued->kind == APP_SHELL_INPUT_VOLUME_FINISH) {
        if (s_volume_preview.contact_sequence != queued->contact_sequence) {
            taskEXIT_CRITICAL(&s_ui_mailbox_mux);
            return false;
        }
        queued->volume_sequence = s_volume_preview.volume_sequence;
    }
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    if (!s_touch_event_queue || xQueueSend(s_touch_event_queue, queued,
            pdMS_TO_TICKS(APP_SHELL_TOUCH_EVENT_QUEUE_WAIT_MS)) != pdTRUE) {
        /* The same fence handles overflow and a system-requested close. */
        nuwax_app_shell_request_close_overlays();
        ESP_LOGE(TAG, "touch boundary queue full kind=%d", (int)queued->kind);
        return false;
    }
    nuwax_lvgl_port_wake();
    return true;
}

static void app_shell_post_drawer_page_drag(int16_t delta_x) {
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    const bool allowed = s_ui_ready && app_shell_touch_allowed_locked(s_touch_contact_sequence);
    if (allowed) {
        s_pending_drawer_page_delta_x = delta_x;
        s_touch_last_page_dx = delta_x;
        s_touch_page_moved = true;
        s_page_sequence = s_touch_contact_sequence;
        s_pending_drawer_page_drag = true;
    }
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    if (allowed) nuwax_lvgl_port_wake();
}

static void app_shell_post_drawer_page_finish(bool commit) {
    app_shell_touch_event_t queued = {
        .kind = APP_SHELL_INPUT_PAGE_FINISH, .finish_target = commit,
    };
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    queued.has_motion = s_touch_page_moved;
    queued.final_position = s_touch_last_page_dx;
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    (void)app_shell_enqueue_input(&queued);
}

static bool app_shell_touch_allowed_on_lvgl(uint32_t sequence, bool current_contact) {
    if (!nuwax_cc_sequence_after(sequence, s_touch_completed_sequence)) return false;
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    const bool allowed = s_ui_ready && app_shell_touch_allowed_locked(sequence) &&
        (!current_contact || sequence == s_touch_contact_sequence);
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    return allowed;
}

static void app_shell_apply_pending_close(void) {
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    const bool close = s_pending_close_overlays;
    if (close) {
        s_pending_close_overlays = false;
        app_shell_cancel_touch_locked();
    }
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    if (close) {
        s_panel_anchor_sequence = 0U;
        s_visual_contact_sequence = 0U;
        nuwax_drawer_contact(false, 0, 0);
        if (nuwax_drawer_is_open()) nuwax_drawer_close();
    }
}

static bool app_shell_apply_panel_motion(const app_shell_panel_motion_t *motion) {
    if (motion->begin_pull && !nuwax_drawer_is_open()) nuwax_drawer_begin_pull();
    if (!nuwax_drawer_is_open()) return false;
    if (s_panel_anchor_sequence != motion->contact_sequence) {
        /* This runs under LVGL ownership, after any preceding timer tick
         * or FIFO boundary. An old touch-task snapshot cannot move the view. */
        s_panel_anchor_y = nuwax_drawer_current_y();
        s_panel_anchor_sequence = motion->contact_sequence;
    }
    nuwax_drawer_drag_to((int16_t)(s_panel_anchor_y + motion->delta_y));
    return true;
}

static void app_shell_finish_volume_on_lvgl(const app_shell_touch_event_t *queued,
                                             bool allowed) {
    nuwax_drawer_input_state_t input;
    nuwax_drawer_get_input_state(&input);
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    const bool commit = allowed && input.volume_available &&
        input.scene_generation == queued->scene_generation &&
        app_shell_touch_allowed_locked(queued->contact_sequence) &&
        (queued->volume_sequence == 0U ||
         !nuwax_cc_sequence_after(s_volume_setting_sequence, queued->volume_sequence));
    /* Claim this release before sending the command. A later close cancels
     * the live finger, while a release already claimed here keeps its order. */
    if (s_volume_preview.contact_sequence == queued->contact_sequence) {
        app_shell_clear_volume_preview_locked();
    }
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);

    const nuwax_app_shell_result_t result = {
        .action = NUWAX_APP_SHELL_ACTION_SET_VOLUME,
        .index = queued->final_position,
        .volume_sequence = queued->volume_sequence,
    };
    if (!commit || !app_shell_queue_result(&result)) {
        taskENTER_CRITICAL(&s_ui_mailbox_mux);
        app_shell_restore_volume_locked(queued->volume_sequence);
        taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    }
}

/* The terminal FIFO preserves releases. Only an active contact's movement
 * may coalesce, and every final pose is applied before its settle decision. */
static bool app_shell_drain_touch_events(bool discard) {
    const int64_t started_us = esp_timer_get_time();
    unsigned drained = 0U;
    while (s_touch_event_queue && drained++ < APP_SHELL_TOUCH_EVENT_QUEUE_DEPTH &&
           esp_timer_get_time() - started_us < APP_SHELL_TOUCH_EVENT_DRAIN_BUDGET_US) {
        app_shell_touch_event_t queued = {0};
        if (xQueueReceive(s_touch_event_queue, &queued, 0) != pdTRUE) break;
        const bool live = !discard && app_shell_touch_allowed_on_lvgl(queued.contact_sequence, false);
        if (live && s_visual_contact_sequence == queued.contact_sequence) {
            nuwax_drawer_contact(false, 0, 0);
            s_visual_contact_sequence = 0U;
        }
        if (queued.kind == APP_SHELL_INPUT_VOLUME_FINISH) {
            app_shell_finish_volume_on_lvgl(&queued, live);
        } else if (live && queued.kind == APP_SHELL_INPUT_PANEL_FINISH && queued.has_motion) {
            const app_shell_panel_motion_t motion = {
                .contact_sequence = queued.contact_sequence,
                .delta_y = queued.final_position,
                .begin_pull = queued.begin_pull,
            };
            if (app_shell_apply_panel_motion(&motion)) {
                const bool close = queued.finish_target ? queued.begin_pull :
                    nuwax_cc_drawer_release_close(queued.begin_pull, queued.panel_total_dy,
                        queued.panel_velocity_y, nuwax_drawer_current_y());
                nuwax_drawer_finish_drag(close);
            }
        } else if (live && queued.kind == APP_SHELL_INPUT_PAGE_FINISH) {
            if (queued.has_motion) nuwax_drawer_page_drag_to(queued.final_position);
            nuwax_drawer_page_finish_drag(queued.finish_target);
        } else if (live && queued.kind == APP_SHELL_INPUT_TOUCH) {
            nuwax_drawer_input_state_t input;
            nuwax_drawer_get_input_state(&input);
            if (input.scene_generation == queued.scene_generation &&
                (queued.event != NUWAX_APP_SHELL_EVENT_TAP ||
                 (queued.tap_allowed && input.tap_allowed))) {
                const nuwax_app_shell_result_t result = app_shell_handle_touch_event_on_lvgl(
                    queued.event, queued.has_point ? &queued.point : NULL);
                (void)app_shell_queue_result(&result);
            }
        }
        if (nuwax_cc_sequence_after(queued.contact_sequence, s_touch_completed_sequence)) {
            s_touch_completed_sequence = queued.contact_sequence;
        }
    }
    const bool pending = s_touch_event_queue && uxQueueMessagesWaiting(s_touch_event_queue) > 0U;
    if (pending) nuwax_lvgl_port_wake();
    return pending;
}

static void app_shell_lvgl_pre_handler(void *user_data) {
    (void)user_data;
    if (!nuwax_app_shell_is_ui_ready()) return;
    nuwax_face_service_decode();
    app_shell_apply_pending_close();
    /* LVGL timers may have changed the scene since the last pre-handler.
     * Retire that scene's preview before copying state for this frame. */
    app_shell_overlay_state_sync_from_lvgl();

    bool do_contact = false, contact_down = false;
    uint16_t contact_x = 0, contact_y = 0;
    uint32_t contact_seq = 0U, contact_scene = 0U;
    app_shell_panel_motion_t panel_motion;
    bool do_page_drag = false;
    int16_t page_delta_x = 0;
    uint32_t page_seq = 0U;
    bool do_provisioning_qr = false;
    int64_t provisioning_qr_retry_deadline_us = 0;
    bool do_provisioning_qr_close = false;
    nuwax_device_state_t state_snapshot = {0};
    uint32_t state_revision = 0U;
    bool have_state_update = false;
    uint32_t rejected_actions;

    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    if (s_state_mailbox.initialized &&
        s_state_mailbox.revision != s_state_mailbox.published_revision) {
        state_snapshot = s_state_mailbox.snapshot;
        if (s_volume_preview.contact_sequence != 0U) {
            state_snapshot.volume = s_volume_preview.value;
        }
        state_revision = s_state_mailbox.revision;
        have_state_update = true;
    }
    if (s_wifi_detail_mailbox.revision != s_wifi_detail_mailbox.published_revision) {
        s_wifi_detail_mailbox.published_revision = s_wifi_detail_mailbox.revision;
    }
    if (s_status_detail_mailbox.revision != s_status_detail_mailbox.published_revision) {
        s_status_detail_mailbox.published_revision = s_status_detail_mailbox.revision;
    }
    if (s_pending_drawer_contact) {
        do_contact = true;
        contact_down = s_drawer_contact_down;
        contact_x = s_drawer_contact_x;
        contact_y = s_drawer_contact_y;
        contact_seq = s_contact_sequence;
        contact_scene = s_touch_input.scene_generation;
        s_pending_drawer_contact = false;
    }
    s_pending_menu_drag = false;
    s_pending_menu_finish = false;
    s_pending_detail_adjust = 0;
    panel_motion = s_pending_panel_motion;
    s_pending_panel_motion.contact_sequence = 0U;
    if (s_pending_drawer_page_drag) {
        do_page_drag = true;
        page_delta_x = s_pending_drawer_page_delta_x;
        page_seq = s_page_sequence;
        s_pending_drawer_page_drag = false;
    }
    const int64_t now_us = esp_timer_get_time();
    if (s_pending_provisioning_qr &&
        now_us >= s_pending_provisioning_qr_deadline_us) {
        if (s_state_mailbox.initialized && s_state_mailbox.snapshot.provisioning_active) {
            do_provisioning_qr = true;
            provisioning_qr_retry_deadline_us = now_us +
                (int64_t)APP_SHELL_PROVISIONING_QR_RETRY_MS * 1000LL;
            s_pending_provisioning_qr_deadline_us = provisioning_qr_retry_deadline_us;
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
        s_state_mailbox.published_revision = state_revision;
        taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    }

    nuwax_drawer_input_state_t input;
    nuwax_drawer_get_input_state(&input);
    const bool blocked = input.modal_blocked || nuwax_ota_progress_is_visible();
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    const bool system_closing = s_system_closing;
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    if (do_provisioning_qr_close) {
        nuwax_drawer_hide_provisioning_qr();
        nuwax_drawer_input_state_t after;
        nuwax_drawer_get_input_state(&after);
        if (after.scene_generation != input.scene_generation) {
            taskENTER_CRITICAL(&s_ui_mailbox_mux);
            app_shell_cancel_touch_locked();
            taskEXIT_CRITICAL(&s_ui_mailbox_mux);
        }
    }
    if (do_provisioning_qr && !blocked && !system_closing) {
        const bool shown = nuwax_drawer_show_provisioning_qr();
        if (shown) {
            taskENTER_CRITICAL(&s_ui_mailbox_mux);
            app_shell_cancel_touch_locked();
            if (s_pending_provisioning_qr &&
                s_pending_provisioning_qr_deadline_us == provisioning_qr_retry_deadline_us) {
                s_pending_provisioning_qr = false;
                s_pending_provisioning_qr_deadline_us = 0;
            }
            taskEXIT_CRITICAL(&s_ui_mailbox_mux);
        }
    }
    const bool boundaries_pending = app_shell_drain_touch_events(blocked);
    if (boundaries_pending && !blocked) {
        /* A newer contact may begin while this frame drains older releases. */
        taskENTER_CRITICAL(&s_ui_mailbox_mux);
        if (panel_motion.contact_sequence == s_touch_contact_sequence &&
            s_pending_panel_motion.contact_sequence == 0U &&
            app_shell_touch_allowed_locked(panel_motion.contact_sequence)) {
            s_pending_panel_motion = panel_motion;
        }
        if (do_page_drag && !s_pending_drawer_page_drag && page_seq == s_touch_contact_sequence &&
            app_shell_touch_allowed_locked(page_seq)) {
            s_pending_drawer_page_drag = true;
            s_pending_drawer_page_delta_x = page_delta_x;
            s_page_sequence = page_seq;
        }
        taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    }
    if (do_contact && !blocked) {
        if (!contact_down && s_visual_contact_sequence == contact_seq) {
            nuwax_drawer_contact(false, 0, 0);
            s_visual_contact_sequence = 0U;
        } else if (contact_down && app_shell_touch_allowed_on_lvgl(contact_seq, true)) {
            nuwax_drawer_get_input_state(&input);
            if (input.tap_allowed && input.scene_generation == contact_scene) {
                if (contact_seq != s_visual_contact_sequence) {
                    nuwax_drawer_contact(false, 0, 0);
                    s_visual_contact_sequence = contact_seq;
                }
                nuwax_drawer_contact(true, contact_x, contact_y);
            }
        }
    }
    if (!blocked && !boundaries_pending) {
        if (app_shell_touch_allowed_on_lvgl(panel_motion.contact_sequence, true)) {
            (void)app_shell_apply_panel_motion(&panel_motion);
        }
        if (do_page_drag && app_shell_touch_allowed_on_lvgl(page_seq, true)) {
            nuwax_drawer_page_drag_to(page_delta_x);
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
    if ((rejected_actions &
         app_shell_action_bit(NUWAX_APP_SHELL_ACTION_SET_SIMULATED_OFFLINE)) != 0U) {
        nuwax_drawer_reject_action(NUWAX_DRAWER_ACTION_SET_SIMULATED_OFFLINE);
    }
    if ((rejected_actions & app_shell_action_bit(NUWAX_APP_SHELL_ACTION_CYCLE_BRIGHTNESS)) != 0U) {
        nuwax_drawer_reject_action(NUWAX_DRAWER_ACTION_CYCLE_BRIGHTNESS);
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
    /* Fence input before waiting for the renderer. If the wait expires,
     * the LVGL task still owns the pending close. */
    nuwax_app_shell_request_close_overlays();
    if (!nuwax_app_shell_is_ui_ready()) return;
    if (!nuwax_lvgl_port_lock(500)) {
        ESP_LOGW(TAG, "overlay close deferred to LVGL task");
        return;
    }
    if (nuwax_app_shell_is_ui_ready()) {
        app_shell_apply_pending_close();
        app_shell_lvgl_unlock();
    } else {
        nuwax_lvgl_port_unlock();
    }
}

void nuwax_app_shell_request_close_overlays(void) {
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    const bool ready = s_ui_ready;
    if (ready) {
        s_system_closing = true;
        s_pending_close_overlays = true;
        app_shell_cancel_touch_locked();
    }
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    if (ready) nuwax_lvgl_port_wake();
}

bool nuwax_app_shell_drawer_is_open(void) {
    if (!nuwax_app_shell_is_ui_ready()) return false;
    return app_shell_overlay_state_get().drawer_open;
}

bool nuwax_app_shell_drawer_is_detail(void) {
    if (!nuwax_app_shell_is_ui_ready()) return false;
    return app_shell_overlay_state_get().menu_detail;
}

void nuwax_app_shell_touch_begin(void) {
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    s_touch_contact_sequence = app_shell_next_revision(s_touch_contact_sequence);
    s_touch_input = s_drawer_input;
    if (!app_shell_touch_allowed_locked(s_touch_contact_sequence)) {
        s_touch_cancelled_sequence = s_touch_contact_sequence;
    }
    /* Earlier releases carry their own final pose in the FIFO. */
    s_touch_panel_motion.contact_sequence = 0U;
    s_pending_panel_motion.contact_sequence = 0U;
    s_touch_page_moved = false;
    s_pending_drawer_page_drag = false;
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
}

bool nuwax_app_shell_drawer_can_page_drag(int16_t delta_x) {
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    const bool allowed = s_ui_ready && app_shell_touch_allowed_locked(s_touch_contact_sequence) &&
        s_touch_input.open && !s_touch_input.detail &&
        (delta_x < 0 ? s_drawer_input.page_left : s_drawer_input.page_right);
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    return allowed;
}

bool nuwax_app_shell_drawer_preview_volume_drag_at(uint16_t x, uint16_t y,
                                                   bool captured, uint8_t *volume_out) {
    uint8_t volume;
    if (!nuwax_drawer_volume_value_at(x, y, captured, &volume)) return false;
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    const bool same_contact = s_volume_preview.contact_sequence == s_touch_contact_sequence;
    const bool allowed = s_ui_ready && app_shell_touch_allowed_locked(s_touch_contact_sequence) &&
        s_touch_input.volume_available && s_drawer_input.volume_available &&
        s_touch_input.scene_generation == s_drawer_input.scene_generation &&
        (!captured || same_contact);
    const bool changed = allowed && (!same_contact || s_volume_preview.value != volume);
    if (changed) {
        s_volume_preview.contact_sequence = s_touch_contact_sequence;
        s_volume_preview.value = volume;
        s_volume_preview.volume_sequence = s_volume_preview_cb ? s_volume_preview_cb(volume) : 0U;
        (void)app_shell_next_state_revision_locked();
    }
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    if (changed) nuwax_lvgl_port_wake();
    if (allowed && volume_out) *volume_out = volume;
    return allowed;
}

void nuwax_app_shell_drawer_finish_volume_drag(int volume) {
    if (volume >= 0) {
        app_shell_touch_event_t queued = {
            .kind = APP_SHELL_INPUT_VOLUME_FINISH,
            .final_position = volume > 100 ? 100 : volume,
        };
        if (app_shell_enqueue_input(&queued)) return;
    }
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    if (s_volume_preview.contact_sequence == s_touch_contact_sequence) {
        app_shell_cancel_volume_preview_locked();
    }
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    nuwax_lvgl_port_wake();
}

void nuwax_app_shell_drawer_contact(bool down, uint16_t x, uint16_t y) {
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    const bool allowed = s_ui_ready && app_shell_touch_allowed_locked(s_touch_contact_sequence) &&
        (!down || s_touch_input.tap_allowed);
    const bool changed = allowed && (s_drawer_contact_down != down ||
        s_drawer_contact_x != x || s_drawer_contact_y != y ||
        s_contact_sequence != s_touch_contact_sequence);
    if (changed) {
        s_pending_drawer_contact = true;
        s_drawer_contact_down = down;
        s_drawer_contact_x = x;
        s_drawer_contact_y = y;
        s_contact_sequence = s_touch_contact_sequence;
    }
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    if (changed) nuwax_lvgl_port_wake();
}

void nuwax_app_shell_drawer_drag_by(int16_t delta_y, bool begin_pull) {
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    const bool allowed = s_ui_ready && app_shell_touch_allowed_locked(s_touch_contact_sequence);
    if (allowed) {
        s_touch_panel_motion = (app_shell_panel_motion_t){
            .contact_sequence = s_touch_contact_sequence,
            .delta_y = delta_y,
            .begin_pull = begin_pull,
        };
        s_pending_panel_motion = s_touch_panel_motion;
    }
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    if (allowed) nuwax_lvgl_port_wake();
}

void nuwax_app_shell_drawer_finish_drag(int16_t final_delta_y, int16_t total_dy,
                                       int16_t velocity_y, bool cancelled) {
    app_shell_touch_event_t queued = {
        .kind = APP_SHELL_INPUT_PANEL_FINISH,
        .finish_target = cancelled,
        .panel_total_dy = total_dy,
        .panel_velocity_y = velocity_y,
    };
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    queued.has_motion = s_touch_panel_motion.contact_sequence != 0U;
    queued.begin_pull = s_touch_panel_motion.begin_pull;
    queued.final_position = cancelled ? s_touch_panel_motion.delta_y : final_delta_y;
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    (void)app_shell_enqueue_input(&queued);
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
    /* Reflect a lost IP link even while a transport owner is blocked in TLS
     * cleanup. Keep this in the normal UI mailbox path, not the Wi-Fi event
     * callback, and never change meeting/monitoring or conversation state. */
    nuwax_device_state_t snapshot;
    if (nuwax_app_shell_get_device_state_snapshot(&snapshot, NULL)) {
        if ((snapshot.wifi == NUWAX_WIFI_CONNECTED || snapshot.wifi == NUWAX_WIFI_WEAK) &&
            !nuwax_wifi_is_connected()) {
            nuwax_app_shell_set_wifi_status(NULL, NULL);
            nuwax_app_shell_set_cloud_state(NUWAX_CLOUD_DISCONNECTED);
        } else if (!snapshot.provisioning_active && !snapshot.provisioning_starting &&
                   !nuwax_wifi_provisioning_active() && nuwax_wifi_is_connected()) {
            char ssid[33] = {0}, ip[16] = {0};
            if (nuwax_wifi_get_status(ssid, sizeof(ssid), ip, sizeof(ip)) == ESP_OK &&
                ssid[0] && ip[0] && nuwax_wifi_is_connected() &&
                !nuwax_wifi_provisioning_active() &&
                ((snapshot.wifi != NUWAX_WIFI_CONNECTED && snapshot.wifi != NUWAX_WIFI_WEAK) ||
                 strcmp(snapshot.ssid, ssid) != 0 || strcmp(snapshot.ip, ip) != 0)) {
                /* Link truth repairs stale UI in both directions. Cloud
                 * authentication and conversation ownership stay independent. */
                nuwax_app_shell_set_wifi_status(ssid, ip);
            }
        }
    }
    int8_t rssi_dbm = -127;
    const bool rssi_valid = nuwax_wifi_get_rssi(&rssi_dbm) == ESP_OK;
    nuwax_app_shell_set_wifi_signal(rssi_valid, rssi_dbm);
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

void nuwax_app_shell_set_brightness_percent(uint8_t percent) {
    nuwax_device_state_t patch = {.brightness_percent = percent};
    (void)nuwax_app_shell_publish_device_state_delta(&patch, NUWAX_APP_SHELL_STATE_BRIGHTNESS);
}

void nuwax_app_shell_confirm_volume(int volume_percent, uint32_t sequence) {
    const uint8_t volume = (uint8_t)(volume_percent < 0 ? 0 :
        volume_percent > 100 ? 100 : volume_percent);
    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    app_shell_post_volume_locked(volume, sequence);
    if (sequence != 0U && s_volume_preview.volume_sequence != 0U &&
        !nuwax_cc_sequence_after(s_volume_preview.volume_sequence, sequence)) {
        /* The audio owner already retired this preview when it committed. */
        app_shell_clear_volume_preview_locked();
    }
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    nuwax_lvgl_port_wake();
}

void nuwax_app_shell_set_volume_percent(int volume_percent) {
    nuwax_app_shell_confirm_volume(volume_percent, 0U);
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

void nuwax_app_shell_set_provisioning_starting(void) {
    nuwax_device_state_t patch = {.provisioning_starting = true};
    (void)nuwax_app_shell_publish_device_state_delta(
        &patch, NUWAX_APP_SHELL_STATE_PROVISIONING);
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
        if (active) app_shell_cancel_touch_locked();
    }
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    if (changed) {
        nuwax_lvgl_port_wake();
    }
}

void nuwax_app_shell_set_simulated_offline(bool enabled) {
    nuwax_device_state_t patch = {.simulated_offline = enabled};
    (void)nuwax_app_shell_publish_device_state_delta(
        &patch, NUWAX_APP_SHELL_STATE_SIMULATED_OFFLINE);
}

void nuwax_app_shell_schedule_provisioning_qr(uint32_t delay_ms) {
    /* Hold the live provisioning face after service readiness. Failed QR
     * admission retries use their existing deadline, not another full hold. */
    if (delay_ms < APP_SHELL_PROVISIONING_FACE_HOLD_MS)
        delay_ms = APP_SHELL_PROVISIONING_FACE_HOLD_MS;
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
        case NUWAX_DRAWER_ACTION_CYCLE_BRIGHTNESS:
            result.action = NUWAX_APP_SHELL_ACTION_CYCLE_BRIGHTNESS;
            result.index = drawer_action.value;
            break;
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
        case NUWAX_DRAWER_ACTION_SET_SIMULATED_OFFLINE:
            result.action = NUWAX_APP_SHELL_ACTION_SET_SIMULATED_OFFLINE;
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
    nuwax_lvgl_port_note_interaction();
    nuwax_app_shell_result_t result = {.action = NUWAX_APP_SHELL_ACTION_NONE, .index = 0};

    taskENTER_CRITICAL(&s_ui_mailbox_mux);
    const bool allowed = app_shell_touch_allowed_locked(s_touch_contact_sequence);
    taskEXIT_CRITICAL(&s_ui_mailbox_mux);
    if (!allowed) return result;

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

    app_shell_touch_event_t queued = {
        .kind = APP_SHELL_INPUT_TOUCH,
        .event = event,
        .has_point = point != NULL,
    };
    if (point) queued.point = *point;
    (void)app_shell_enqueue_input(&queued);
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

    if (nuwax_ota_progress_is_visible()) return result;

    if (nuwax_drawer_is_open()) {
        if (nuwax_drawer_is_detail()) {
            if (event == NUWAX_APP_SHELL_EVENT_SWIPE_RIGHT) {
                nuwax_drawer_back();
            } else if (event == NUWAX_APP_SHELL_EVENT_SWIPE_UP) {
                nuwax_drawer_finish_drag(true);
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
        } else if (event == NUWAX_APP_SHELL_EVENT_TAP) {
            result = drawer_action_to_shell_action(
                nuwax_drawer_tap(point ? point->x : 0, y));
        }
        return result;
    }

    if (event == NUWAX_APP_SHELL_EVENT_SWIPE_DOWN && y < NUWAX_GESTURE_TOP_BAND) {
        nuwax_drawer_open();
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
