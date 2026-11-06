#pragma once

#include "lvgl.h"
#include "esp_err.h"
#include "face/emotion_asset_pack.h"
#include "nuwax_device_state.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

esp_err_t nuwax_face_create(lv_obj_t *parent);
esp_err_t nuwax_face_create_with_initial_asset(
    lv_obj_t *parent, nuwax_emotion_asset_id_t initial_asset);
/* Keeps the provisioning asset visible during the boot handoff until the
 * provisioning owner publishes either active or a terminal failure. */
void nuwax_face_clear_provisioning_boot_visual(void);
esp_err_t nuwax_face_deinit(void);
void nuwax_face_update(const nuwax_device_state_t *state);
void nuwax_face_tick(void);
void nuwax_face_service_decode(void);
void nuwax_face_set_menu_open(bool open);
/* Independent full-screen cover owner; closing a drawer cannot resume it. */
void nuwax_face_set_overlay_covered(bool covered);
esp_err_t nuwax_face_freeze_interaction(void);
void nuwax_face_set_interaction_active(bool active);
void nuwax_face_set_overlay_progress(uint16_t progress_permille);
bool nuwax_face_frozen_frame_ready(void);
const uint16_t *nuwax_face_frozen_rgb565(size_t *stride_pixels);
bool nuwax_face_draw_frozen(void *layer);
bool nuwax_face_next_expression(void);
void nuwax_face_start_test_mode(void);
