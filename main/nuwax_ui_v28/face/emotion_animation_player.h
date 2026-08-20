#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "emotion_asset_pack.h"
#include "esp_err.h"
#include "lvgl.h"

esp_err_t nuwax_emotion_animation_player_init(lv_obj_t *parent);
esp_err_t nuwax_emotion_animation_player_deinit(void);
esp_err_t nuwax_emotion_animation_player_reap(void);
esp_err_t nuwax_emotion_animation_player_set_asset(
    nuwax_emotion_asset_id_t asset_id);
esp_err_t nuwax_emotion_animation_player_trigger_touch(void);
esp_err_t nuwax_emotion_animation_player_tick(void);
esp_err_t nuwax_emotion_animation_player_freeze(void);
void nuwax_emotion_animation_player_set_interaction_active(bool active);
void nuwax_emotion_animation_player_set_overlay_progress(
    uint16_t progress_permille);
bool nuwax_emotion_animation_player_frozen_frame_ready(void);
const uint16_t *nuwax_emotion_animation_player_frozen_rgb565(
    size_t *stride_pixels);
bool nuwax_emotion_animation_player_draw_frozen(void *layer);
bool nuwax_emotion_animation_player_ready(void);
