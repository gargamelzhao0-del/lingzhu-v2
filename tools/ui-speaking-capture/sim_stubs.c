#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"
#include "lvgl.h"
#include "nuwax_ui_v28/face/emotion_animation_player.h"

static lv_obj_t *s_background;
static lv_image_dsc_t s_background_dsc;
static uint16_t s_black_pixels[360 * 360];

int nuwax_sim_log(const char *level, const char *tag, const char *fmt, ...)
{
    va_list args;
    fprintf(stderr, "[%s] %s: ", level, tag ? tag : "sim");
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    return 0;
}

const char *esp_err_to_name(esp_err_t err)
{
    switch (err) {
        case ESP_OK: return "ESP_OK";
        case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
        case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
        case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
        default: return "ESP_FAIL";
    }
}

int64_t esp_timer_get_time(void)
{
    return (int64_t)lv_tick_get() * 1000LL;
}

esp_err_t nuwax_emotion_animation_player_init(lv_obj_t *parent)
{
    s_background_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_background_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_background_dsc.header.w = 360;
    s_background_dsc.header.h = 360;
    s_background_dsc.header.stride = 360 * 2;
    s_background_dsc.data_size = sizeof(s_black_pixels);
    s_background_dsc.data = (const uint8_t *)s_black_pixels;
    s_background = lv_image_create(parent);
    if (!s_background) return ESP_ERR_NO_MEM;
    lv_image_set_src(s_background, &s_background_dsc);
    lv_obj_set_pos(s_background, 0, 0);
    lv_obj_set_size(s_background, 360, 360);
    return ESP_OK;
}

esp_err_t nuwax_emotion_animation_player_deinit(void)
{
    if (s_background) lv_obj_delete(s_background);
    s_background = NULL;
    return ESP_OK;
}

esp_err_t nuwax_emotion_animation_player_reap(void) { return ESP_OK; }
esp_err_t nuwax_emotion_animation_player_set_asset(
    nuwax_emotion_asset_id_t asset_id)
{
    (void)asset_id;
    return ESP_OK;
}
esp_err_t nuwax_emotion_animation_player_trigger_touch(void) { return ESP_OK; }
esp_err_t nuwax_emotion_animation_player_tick(void) { return ESP_OK; }
esp_err_t nuwax_emotion_animation_player_freeze(void) { return ESP_OK; }
void nuwax_emotion_animation_player_set_interaction_active(bool active)
{
    (void)active;
}
void nuwax_emotion_animation_player_set_overlay_progress(uint16_t progress)
{
    (void)progress;
}
bool nuwax_emotion_animation_player_frozen_frame_ready(void) { return false; }
bool nuwax_emotion_animation_player_draw_frozen(void *layer)
{
    (void)layer;
    return false;
}
bool nuwax_emotion_animation_player_ready(void) { return true; }
