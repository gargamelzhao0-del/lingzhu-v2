#pragma once
#include "esp_err.h"
#include "lvgl.h"
#include "nuwax_device_state.h"

esp_err_t nuwax_ota_progress_create(lv_obj_t *screen);
void nuwax_ota_progress_update(const nuwax_device_state_t *state);
void nuwax_ota_progress_delete(void);
/* LVGL task only: true only while actual OTA progress is displayed. */
bool nuwax_ota_progress_is_visible(void);
