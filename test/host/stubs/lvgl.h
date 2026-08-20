#pragma once

/*
 * Minimal lvgl.h stub for host unit tests.
 *
 * Provides ONLY the LVGL types and constants that the pure-logic focus-menu
 * modules reference at compile time (color/opacity types, a couple of macros,
 * the precise-point struct). This is NOT a functional LVGL: any module that
 * actually draws or creates objects must not be compiled against this stub.
 *
 * Kept intentionally faithful to LVGL semantics for the values we test:
 *  - lv_opa_t is a 0..255 byte
 *  - lv_color_hex packs 0xRRGGBB into a 16-bit RGB565 value (matches the
 *    LV_COLOR_DEPTH==16 build the firmware uses), so color helpers can be
 *    checked for equality.
 */

#include <stdint.h>

typedef uint8_t lv_opa_t;

typedef struct {
    uint16_t full; /* RGB565 */
} lv_color_t;

typedef struct {
    int32_t x;
    int32_t y;
} lv_point_precise_t;

/* Opaque widget handle. The pure-logic modules never dereference it; it only
 * appears in function signatures pulled in via nuwax_focus_menu.h. */
typedef struct _lv_obj lv_obj_t;

#define LV_OPA_TRANSP 0
#define LV_OPA_COVER  255

#define LV_RADIUS_CIRCLE 0x7fff
#define LV_SCALE_NONE    256

/* Symbol glyphs: in real LVGL these are UTF-8 string literals from the icon
 * font. The stub only needs them to be distinct non-empty strings so the
 * data module's symbol lookup is testable. */
#define LV_SYMBOL_SETTINGS   "\xEF\x80\x93"
#define LV_SYMBOL_EDIT       "\xEF\x8C\x84"
#define LV_SYMBOL_WARNING    "\xEF\x81\xB1"
#define LV_SYMBOL_WIFI       "\xEF\x87\xAB"
#define LV_SYMBOL_VOLUME_MAX "\xEF\x80\xA8"
#define LV_SYMBOL_POWER      "\xEF\x80\x91"

static inline lv_color_t lv_color_hex(uint32_t c) {
    uint8_t r = (uint8_t)((c >> 16) & 0xff);
    uint8_t g = (uint8_t)((c >> 8) & 0xff);
    uint8_t b = (uint8_t)(c & 0xff);
    lv_color_t out;
    out.full = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    return out;
}
