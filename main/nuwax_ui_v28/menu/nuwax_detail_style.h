#pragma once
#include "lvgl.h"
#include "assets/nuwax_control_center_icons.h"
LV_FONT_DECLARE(nuwax_font_lingzhu_14);
LV_FONT_DECLARE(nuwax_font_lingzhu_17);
LV_FONT_DECLARE(nuwax_font_lingzhu_22);
/* Shared visual and input geometry for both detail pages. */
#define NUWAX_DETAIL_BACK_X 123
#define NUWAX_DETAIL_BACK_Y 320
#define NUWAX_DETAIL_BACK_W 114
#define NUWAX_DETAIL_BACK_H 30
static inline lv_obj_t *nuwax_detail_label(lv_obj_t *parent, int x, int y,
    int width, const lv_font_t *font, const char *text) {
    lv_obj_t *label = lv_label_create(parent);
    if (!label) return NULL;
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_line_space(label, 1, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, text);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return label;
}
static inline lv_obj_t *nuwax_detail_panel(lv_obj_t *parent,
    int x, int y, int width, int height) {
    lv_obj_t *panel = lv_obj_create(parent);
    if (!panel) return NULL;
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, width, height);
    lv_obj_set_style_bg_color(panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 13, 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_border_color(panel, lv_color_white(), 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return panel;
}
static inline bool nuwax_detail_header(lv_obj_t *parent, const char *title) {
    lv_point_t size;
    lv_text_get_size(&size, title, &nuwax_font_lingzhu_22, 0, 0, 240, LV_TEXT_FLAG_NONE);
    const int x = (360 - (36 + 10 + size.x)) / 2;
    lv_obj_t *cat = lv_image_create(parent);
    if (!cat) return false;
    lv_image_set_src(cat, nuwax_control_center_icon_get(NUWAX_CC_ICON_CAT));
    lv_obj_set_style_image_recolor(cat, lv_color_white(), 0);
    lv_obj_set_style_image_recolor_opa(cat, LV_OPA_COVER, 0);
    lv_obj_set_pos(cat, x, 22);
    lv_obj_remove_flag(cat, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return nuwax_detail_label(parent, x + 46, 28, size.x + 2,
                              &nuwax_font_lingzhu_22, title) != NULL;
}
static inline lv_obj_t *nuwax_detail_back_button(lv_obj_t *parent) {
    lv_obj_t *button = nuwax_detail_panel(parent, NUWAX_DETAIL_BACK_X,
        NUWAX_DETAIL_BACK_Y, NUWAX_DETAIL_BACK_W, NUWAX_DETAIL_BACK_H);
    if (!button) return NULL;
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    lv_obj_t *arrow = nuwax_detail_label(button, 23, 4, 16,
                                        &lv_font_montserrat_14, LV_SYMBOL_LEFT);
    return arrow && nuwax_detail_label(button, 43, 2, 44,
                                      &nuwax_font_lingzhu_17, "返回") ? button : NULL;
}
static inline bool nuwax_detail_hit_back(uint16_t x, uint16_t y) {
    return x >= NUWAX_DETAIL_BACK_X - 6 && x < NUWAX_DETAIL_BACK_X + NUWAX_DETAIL_BACK_W + 6 &&
           y >= NUWAX_DETAIL_BACK_Y - 2 && y < NUWAX_DETAIL_BACK_Y + NUWAX_DETAIL_BACK_H + 4;
}
