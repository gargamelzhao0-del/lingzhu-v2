#include "nuwax_app.h"
#include "app_shell.h"
#include "nuwax_device_state.h"
#include "nuwax_face.h"
#include "nuwax_drawer.h"
#include "nuwax_gesture.h"
#include "nuwax_theme.h"
#include "nuwax_ui_config.h"
#include "ota_progress.h"
#include "low_battery_warning.h"
#include "esp_log.h"
#include "nuwax_lvgl_port.h"

static const char *TAG = "nuwax_app";

#define EMOTION_PLAYER_TICK_MS 5
static lv_obj_t *s_screen;
static lv_timer_t *s_emotion_timer;
static bool s_listener_registered;
static bool s_gesture_initialized;
static bool s_created;
static bool s_cleanup_pending;

static void state_listener(const nuwax_device_state_t *state, void *user_data) {
    (void)user_data;
    nuwax_face_update(state);
    nuwax_drawer_update(state);
    nuwax_ota_progress_update(state);
}

static void emotion_tick_cb(lv_timer_t *timer) {
    (void)timer;
    nuwax_face_tick();
}

esp_err_t nuwax_app_create(lv_obj_t *screen) {
    return nuwax_app_create_with_provisioning_face(screen, false);
}

esp_err_t nuwax_app_create_with_provisioning_face(lv_obj_t *screen,
                                                  bool provisioning_face) {
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
    esp_err_t err = provisioning_face
        ? nuwax_face_create_with_initial_asset(
              screen, NUWAX_EMOTION_ASSET_PROVISIONING)
        : nuwax_face_create(screen);
    if (err != ESP_OK) goto fail;
    err = nuwax_drawer_create(screen);
    if (err != ESP_OK) goto fail;
    err = nuwax_ota_progress_create(screen);
    if (err != ESP_OK) goto fail;
    err = nuwax_low_battery_warning_create(screen);
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
    nuwax_low_battery_warning_delete();
    nuwax_ota_progress_delete();
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
