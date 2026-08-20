#pragma once

#include "esp_err.h"
#include "lvgl.h"

/* Gesture thresholds from HTML prototype */
#define NUWAX_GESTURE_OPEN_MENU_DX   42
#define NUWAX_GESTURE_CLOSE_MENU_DX  (-42)
#define NUWAX_GESTURE_VERTICAL_DY    34
#define NUWAX_GESTURE_DRAWER_DY      24   /* HTML: 24px pull threshold */
#define NUWAX_GESTURE_TOP_BAND       96   /* HTML: 96px top touch zone */
#define NUWAX_GESTURE_TAP_MAX        12   /* HTML: 12px max move for tap */
#define NUWAX_GESTURE_VOLUME_STEP    5

esp_err_t nuwax_gesture_init(lv_obj_t *screen);
void nuwax_gesture_deinit(lv_obj_t *screen);
