#include "nuwax_app.h"
#include "app_shell.h"
#include "nuwax_device_state.h"
#include "nuwax_face.h"
#include "nuwax_drawer.h"
#include "nuwax_gesture.h"
#include "nuwax_theme.h"
#include "nuwax_ui_config.h"
#include "esp_log.h"
#include "nuwax_lvgl_port.h"

static const char *TAG = "nuwax_app";

#define EMOTION_PLAYER_TICK_MS 5
#define OTA_PROGRESS_LABEL_W 208
#define OTA_PROGRESS_LABEL_H 36
#define OTA_PROGRESS_LABEL_Y 8

static lv_obj_t *s_screen;
static lv_obj_t *s_ota_progress_label;
static lv_timer_t *s_emotion_timer;
static bool s_listener_registered;
static bool s_gesture_initialized;
static bool s_created;
static bool s_cleanup_pending;

static void ota_progress_update(const nuwax_device_state_t *state) {
    if (!s_ota_progress_label || !state) {
        return;
    }
    if (!state->ota_update_active) {
        lv_obj_add_flag(s_ota_progress_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    const unsigned percent = state->ota_update_percent > 100U
                                 ? 100U
                                 : state->ota_update_percent;
    lv_label_set_text_fmt(s_ota_progress_label, "update: %u%%", percent);
    lv_obj_remove_flag(s_ota_progress_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ota_progress_label);
}

static esp_err_t ota_progress_create(lv_obj_t *screen) {
    s_ota_progress_label = lv_label_create(screen);
    if (!s_ota_progress_label) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_size(s_ota_progress_label,
                    OTA_PROGRESS_LABEL_W, OTA_PROGRESS_LABEL_H);
    lv_obj_align(s_ota_progress_label, LV_ALIGN_TOP_MID,
                 0, OTA_PROGRESS_LABEL_Y);
    lv_label_set_long_mode(s_ota_progress_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_ota_progress_label,
                                LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_ota_progress_label,
                               &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_ota_progress_label,
                                lv_color_hex(0xEAF6FF), 0);
    lv_obj_set_style_bg_color(s_ota_progress_label,
                              NUWAX_COLOR_BG_DARK, 0);
    lv_obj_set_style_bg_opa(s_ota_progress_label, LV_OPA_70, 0);
    lv_obj_set_style_radius(s_ota_progress_label, 8, 0);
    lv_obj_set_style_pad_top(s_ota_progress_label, 4, 0);
    lv_obj_set_style_pad_left(s_ota_progress_label, 4, 0);
    lv_obj_set_style_pad_right(s_ota_progress_label, 4, 0);
    lv_obj_remove_flag(s_ota_progress_label,
                       LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ota_progress_label, LV_OBJ_FLAG_HIDDEN);
    return ESP_OK;
}

static void state_listener(const nuwax_device_state_t *state, void *user_data) {
    (void)user_data;
    nuwax_face_update(state);
    nuwax_drawer_update(state);
    ota_progress_update(state);
}

static void emotion_tick_cb(lv_timer_t *timer) {
    (void)timer;
    nuwax_face_tick();
}

esp_err_t nuwax_app_create(lv_obj_t *screen) {
    if (!screen) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!nuwax_lvgl_port_lock_is_held_by_current_task()) {
        ESP_LOGE(TAG, "V28 UI create rejected without LVGL mutex ownership");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_created || s_screen) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_cleanup_pending) {
        esp_err_t cleanup_err = nuwax_app_deinit();
        if (cleanup_err != ESP_OK) {
            ESP_LOGE(TAG, "previous UI cleanup still pending: %s",
                     esp_err_to_name(cleanup_err));
            return cleanup_err;
        }
    }
    ESP_LOGI(TAG, "Creating V28 UI");
    s_screen = screen;

    nuwax_device_state_init();
    nuwax_theme_init();

    lv_obj_set_style_bg_color(screen, NUWAX_COLOR_BG_DARK, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    /* Create layers bottom-to-top */
    esp_err_t err = nuwax_face_create(screen);
    if (err != ESP_OK) goto fail;
    err = nuwax_drawer_create(screen);
    if (err != ESP_OK) goto fail;
    err = ota_progress_create(screen);
    if (err != ESP_OK) goto fail;
    err = nuwax_gesture_init(screen);
    if (err != ESP_OK) goto fail;
    s_gesture_initialized = true;

    /* The player advances frames only from the LVGL task. */
    s_emotion_timer =
        lv_timer_create(emotion_tick_cb, EMOTION_PLAYER_TICK_MS, NULL);
    if (!s_emotion_timer) {
        ESP_LOGE(TAG, "failed to create emotion player tick timer");
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    /* Publish the UI only after every fallible component exists. */
    if (!nuwax_device_state_add_listener(state_listener, NULL)) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    s_listener_registered = true;

#if CONFIG_NUWAX_FACE_TEST_MODE_ENABLE
    nuwax_face_start_test_mode();
#endif

    s_created = true;
    ESP_LOGI(TAG,
             "V28 UI created control_center=status+menu legacy_overlays=removed");
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "V28 UI creation failed: %s", esp_err_to_name(err));
    esp_err_t cleanup_err = nuwax_app_deinit();
    if (cleanup_err != ESP_OK) {
        ESP_LOGE(TAG, "V28 UI rollback incomplete: %s",
                 esp_err_to_name(cleanup_err));
    }
    return err;
}

esp_err_t nuwax_app_deinit(void) {
    if (!nuwax_lvgl_port_lock_is_held_by_current_task()) {
        ESP_LOGE(TAG, "V28 UI deinit rejected without LVGL mutex ownership");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = ESP_OK;

    /* The shell owns an LVGL pre-handler and cross-task gesture mailbox. Stop
     * that producer boundary before deleting listeners, timers or widgets. */
    nuwax_app_shell_deinit();

    if (s_listener_registered) {
        if (!nuwax_device_state_remove_listener(state_listener, NULL)) {
            ESP_LOGW(TAG, "UI state listener was already absent during deinit");
        }
        s_listener_registered = false;
    }
    if (s_emotion_timer) {
        lv_timer_delete(s_emotion_timer);
        s_emotion_timer = NULL;
    }
    if (s_gesture_initialized) {
        nuwax_gesture_deinit(s_screen);
        s_gesture_initialized = false;
    }
    if (s_ota_progress_label) {
        lv_obj_delete(s_ota_progress_label);
        s_ota_progress_label = NULL;
    }
    nuwax_drawer_deinit();
    esp_err_t face_err = nuwax_face_deinit();
    if (face_err != ESP_OK) {
        result = face_err;
    }

    s_screen = NULL;
    s_created = false;
    s_cleanup_pending = result != ESP_OK;
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "V28 UI deinitialized");
    }
    return result;
}

void nuwax_app_on_state_changed(const nuwax_device_state_t *state) {
    nuwax_device_state_set(state);
}
