#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "emotion_asset_pack.h"
#include "esp_err.h"
#include "lvgl.h"

esp_err_t nuwax_emotion_animation_player_init(lv_obj_t *parent);
esp_err_t nuwax_emotion_animation_player_init_with_initial_asset(
    lv_obj_t *parent, nuwax_emotion_asset_id_t initial_asset);
esp_err_t nuwax_emotion_animation_player_deinit(void);
esp_err_t nuwax_emotion_animation_player_reap(void);
esp_err_t nuwax_emotion_animation_player_set_asset(
    nuwax_emotion_asset_id_t asset_id);
/* Commit the requested asset at the next key frame without the authored
 * component bridge. Used once during boot so the first lifecycle snapshot
 * cannot expose an intermediate transition face. */
esp_err_t nuwax_emotion_animation_player_set_asset_immediate(
    nuwax_emotion_asset_id_t asset_id);
/* Rebuild the current asset timeline after a transient decode/publish error
 * without tearing down the LVGL image or entering the static failsafe. */
esp_err_t nuwax_emotion_animation_player_recover(void);
esp_err_t nuwax_emotion_animation_player_trigger_touch(void);
esp_err_t nuwax_emotion_animation_player_tick(void);
/* LVGL owner service: completion/expiry is independent of the frame timer. */
void nuwax_emotion_animation_player_service_decode(void);
bool nuwax_emotion_animation_player_render_scene(lv_display_t *display,
                                                lv_layer_t *layer);
esp_err_t nuwax_emotion_animation_player_freeze(void);
void nuwax_emotion_animation_player_set_interaction_active(bool active);
void nuwax_emotion_animation_player_set_overlay_progress(
    uint16_t progress_permille);
bool nuwax_emotion_animation_player_frozen_frame_ready(void);
const uint16_t *nuwax_emotion_animation_player_frozen_rgb565(
    size_t *stride_pixels);
bool nuwax_emotion_animation_player_draw_frozen(void *layer);
bool nuwax_emotion_animation_player_ready(void);
/* LVGL owner only: keep optional snapshot work out of an asset transition. */
bool nuwax_emotion_animation_player_transition_active(void);
