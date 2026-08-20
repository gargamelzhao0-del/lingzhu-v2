#pragma once

#include "lvgl.h"

/* Color tokens from HTML prototype */
#define NUWAX_COLOR_OK       lv_color_hex(0x36CE7D)
#define NUWAX_COLOR_WARN     lv_color_hex(0xEFB73D)
#define NUWAX_COLOR_ERROR    lv_color_hex(0xFF6172)
#define NUWAX_COLOR_SPEAK    lv_color_hex(0x589FF4)
#define NUWAX_COLOR_THINK    lv_color_hex(0xA487FF)
#define NUWAX_COLOR_ACCENT   lv_color_hex(0x5D91FF)  /* menu --m-accent */
#define NUWAX_COLOR_TEXT     lv_color_hex(0x0F1115)
#define NUWAX_COLOR_BG_DARK  lv_color_hex(0x000000)
#define NUWAX_COLOR_BG_PANEL lv_color_hex(0x2A2D34)

/* Screen dimensions */
#define NUWAX_SCREEN_W  360
#define NUWAX_SCREEN_H  360

/* Status bar positions */
#define NUWAX_STATUS_Y       22
#define NUWAX_STATUS_ICON_SZ 24
#define NUWAX_WIFI_X         108
#define NUWAX_CLOUD_X        144
#define NUWAX_VOLUME_X       192
#define NUWAX_POWER_X        228

/* Dialog position */
#define NUWAX_DIALOG_X       165
#define NUWAX_DIALOG_Y       310
#define NUWAX_DIALOG_SZ      30

void nuwax_theme_init(void);
