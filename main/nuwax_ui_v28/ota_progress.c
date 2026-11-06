#include "ota_progress.h"
#include "nuwax_face.h"
#include "nuwax_drawer.h"
#include <stdio.h>

LV_FONT_DECLARE(nuwax_font_ota_16);
extern const lv_image_dsc_t nuwax_ota_logo;
extern const lv_image_dsc_t nuwax_ota_capsule;

/* Fixed 360px layout; all artwork stays inside the round display safe area. */
#define OTA_W 224
#define OTA_H 52
#define OTA_Y 252

static lv_obj_t *s_root;
static lv_obj_t *s_capsule;
static lv_obj_t *s_percent;
static lv_obj_t *s_bar;
static bool s_visible;
static unsigned s_value;
static char s_percent_text[5];
static void render_progress(bool active, unsigned value);

static void capsule_outline(lv_event_t *event)
{
    lv_area_t a;
    lv_obj_get_coords(lv_event_get_target_obj(event), &a);
    lv_layer_t *layer = lv_event_get_layer(event);
    /* Area coverage smooths thin outlines and rounded joins without runtime
     * supersampling, geometry masks or additional LVGL objects. */
    lv_draw_image_dsc_t draw;
    lv_draw_image_dsc_init(&draw);
    draw.src = &nuwax_ota_capsule;
    draw.recolor = lv_color_white();
    draw.recolor_opa = LV_OPA_COVER;
    lv_draw_image(layer, &draw, &a);
}

static lv_obj_t *make_label(int x, int width, lv_text_align_t align)
{
    lv_obj_t *label = lv_label_create(s_capsule);
    if (!label) return NULL;
    lv_obj_remove_style_all(label);
    lv_obj_set_pos(label, x, 9);
    lv_obj_set_size(label, width, 22);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(label, &nuwax_font_ota_16, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_align(label, align, 0);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return label;
}

esp_err_t nuwax_ota_progress_create(lv_obj_t *screen)
{
    if (!screen) return ESP_ERR_INVALID_ARG;
    if (s_root) return ESP_ERR_INVALID_STATE;
    /* The drawer's cached scene bypasses screen traversal. Use the top layer
     * so actual OTA remains above that compositor. */
    s_root = lv_obj_create(lv_display_get_layer_top(lv_obj_get_display(screen)));
    if (!s_root) return ESP_ERR_NO_MEM;
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, 360, 360);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_bg_color(s_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *logo = lv_image_create(s_root);
    s_capsule = lv_obj_create(s_root);
    if (!logo || !s_capsule) {
        nuwax_ota_progress_delete();
        return ESP_ERR_NO_MEM;
    }
    lv_image_set_src(logo, &nuwax_ota_logo);
    lv_obj_set_pos(logo, 96, 52);
    lv_obj_set_style_image_recolor(logo, lv_color_white(), 0);
    lv_obj_set_style_image_recolor_opa(logo, LV_OPA_COVER, 0);
    lv_obj_remove_flag(logo, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_remove_style_all(s_capsule);
    lv_obj_set_size(s_capsule, OTA_W, OTA_H);
    lv_obj_set_pos(s_capsule, 68, OTA_Y);
    lv_obj_set_style_bg_color(s_capsule, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_capsule, LV_OPA_COVER, 0);
    /* Retain the former 1px border's child inset without drawing it twice. */
    lv_obj_set_style_pad_all(s_capsule, 1, 0);
    lv_obj_remove_flag(s_capsule, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_capsule, capsule_outline, LV_EVENT_DRAW_MAIN, NULL);

    lv_obj_t *title = make_label(58, 64, LV_TEXT_ALIGN_LEFT);
    s_percent = make_label(155, 46, LV_TEXT_ALIGN_RIGHT);
    s_bar = lv_bar_create(s_capsule);
    if (!title || !s_percent || !s_bar) {
        nuwax_ota_progress_delete();
        return ESP_ERR_NO_MEM;
    }
    lv_label_set_text_static(title, "更新中");
    lv_obj_remove_style_all(s_bar);
    lv_obj_set_pos(s_bar, 20, 37);
    lv_obj_set_size(s_bar, 182, 7);
    lv_obj_set_style_radius(s_bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_bar, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_anim_duration(s_bar, 180, 0);
    lv_obj_remove_flag(s_bar, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_bar_set_range(s_bar, 0, 100);
    s_visible = false;
    s_value = 0;
    return ESP_OK;
}

void nuwax_ota_progress_update(const nuwax_device_state_t *state)
{
    if (!s_root || !state) return;
    render_progress(state->ota_update_active, state->ota_update_percent);
}

static void render_progress(bool active, unsigned value)
{
    if (!active) {
        if (s_visible) {
            lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
            /* lv_bar_set_value returns early for the current target, even
             * during interpolation. Force a different value to cancel it. */
            lv_bar_set_value(s_bar, s_value == 0U ? 1 : 0, LV_ANIM_OFF);
            lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
            s_visible = false;
            nuwax_drawer_set_ota_blocked(false);
            nuwax_face_set_overlay_covered(false);
            lv_obj_invalidate(lv_display_get_screen_active(lv_obj_get_display(s_root)));
        }
        return;
    }
    if (value > 100U) value = 100U;
    if (s_visible && value == s_value) return;
    if (!s_visible) {
        /* One LVGL-owner transaction, before the first OTA pixels are drawn.
         * Pause first: retiring menu interaction must not restart the face. */
        nuwax_face_set_overlay_covered(true);
        nuwax_drawer_set_ota_blocked(true);
    }
    snprintf(s_percent_text, sizeof(s_percent_text), "%u%%", value);
    lv_label_set_text_static(s_percent, s_percent_text);
    lv_bar_set_value(s_bar, (int32_t)value, s_visible ? LV_ANIM_ON : LV_ANIM_OFF);
    s_value = value;
    if (!s_visible) {
        lv_obj_remove_flag(s_root, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_root);
        s_visible = true;
        lv_obj_invalidate(s_root);
    }
}

void nuwax_ota_progress_delete(void)
{
    if (s_root) lv_obj_delete(s_root);
    if (s_visible) {
        nuwax_drawer_set_ota_blocked(false);
        nuwax_face_set_overlay_covered(false);
    }
    s_root = NULL;
    s_capsule = NULL;
    s_percent = NULL;
    s_bar = NULL;
    s_visible = false;
    s_value = 0;
}

bool nuwax_ota_progress_is_visible(void)
{
    return s_visible;
}
