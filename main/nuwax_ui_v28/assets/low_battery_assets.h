#pragma once
#include "lvgl.h"
#include "asset_rle.h"

#define NUWAX_LOW_BATTERY_ICON_WIDTH 104U
#define NUWAX_LOW_BATTERY_ICON_HEIGHT 78U
#define NUWAX_LOW_BATTERY_ICON_BYTES (NUWAX_LOW_BATTERY_ICON_WIDTH * NUWAX_LOW_BATTERY_ICON_HEIGHT)
#define NUWAX_LOW_BATTERY_ICON_STEPS 16U

LV_FONT_DECLARE(nuwax_font_low_battery_22);
LV_FONT_DECLARE(nuwax_font_low_battery_14);
extern const nuwax_asset_rle_t nuwax_low_battery_frames;
extern const lv_image_dsc_t nuwax_low_battery_icon;
