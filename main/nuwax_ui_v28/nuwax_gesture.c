#include "nuwax_gesture.h"

#include "esp_log.h"

static const char *TAG = "nuwax_gesture";
static lv_obj_t *s_screen;
static bool s_added_clickable;

esp_err_t nuwax_gesture_init(lv_obj_t *screen) {
    if (!screen) return ESP_ERR_INVALID_ARG;
    if (s_screen) return ESP_ERR_INVALID_STATE;

    s_screen = screen;
    s_added_clickable = !lv_obj_has_flag(screen, LV_OBJ_FLAG_CLICKABLE);
    if (s_added_clickable) {
        lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);
    }
    ESP_LOGI(TAG, "Gesture bridge initialized; touch routing is handled by app_shell");
    return ESP_OK;
}

void nuwax_gesture_deinit(lv_obj_t *screen) {
    if (!s_screen || (screen && screen != s_screen)) return;
    if (s_added_clickable) {
        lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_CLICKABLE);
    }
    s_screen = NULL;
    s_added_clickable = false;
}
