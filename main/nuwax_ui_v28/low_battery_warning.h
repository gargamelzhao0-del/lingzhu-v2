#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "lvgl.h"

/* LVGL owner/port mutex required. The warning is a modal top-layer screen. */
esp_err_t nuwax_low_battery_warning_create(lv_obj_t *screen);
esp_err_t nuwax_low_battery_warning_show(bool visible);
void nuwax_low_battery_warning_delete(void);
bool nuwax_low_battery_warning_is_visible(void);
