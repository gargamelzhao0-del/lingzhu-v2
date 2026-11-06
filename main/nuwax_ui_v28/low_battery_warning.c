#include "low_battery_warning.h"
#include "nuwax_drawer.h"
#include "nuwax_face.h"
#include "ota_progress.h"
#include "assets/low_battery_assets.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "src/misc/cache/instance/lv_image_cache.h"

#define PULSE_CYCLE_MS 2500U
#define PULSE_FRAME_MS 25U
#define PULSE_PHASE_MAX 4096U

static lv_obj_t *s_root;
static lv_obj_t *s_icon;
static bool s_visible;
static uint8_t *s_pixels;
/* Allocated only while the warning is visible; no allocations per frame. */
static uint8_t *s_masks;
static uint8_t *s_mask_a, *s_mask_b;
static unsigned s_mask_a_index, s_mask_b_index;
static bool s_decode_failed;
static lv_image_dsc_t s_frame;
static uint32_t s_frame_tick;
static uint32_t s_last_mix;

static bool load_masks(unsigned first, unsigned next)
{
    if (!s_masks) return false;
    if (s_mask_b_index == first || (s_mask_a_index == next && first != next)) {
        uint8_t *pixels = s_mask_a; s_mask_a = s_mask_b; s_mask_b = pixels;
        unsigned index = s_mask_a_index; s_mask_a_index = s_mask_b_index; s_mask_b_index = index;
    }
    if (s_mask_a_index != first) {
        s_mask_a_index = UINT32_MAX;
        if (!nuwax_asset_rle_decode_frame(&nuwax_low_battery_frames, first,
                                         s_mask_a, NUWAX_LOW_BATTERY_ICON_BYTES)) return false;
        s_mask_a_index = first;
    }
    if (next != first && s_mask_b_index != next) {
        s_mask_b_index = UINT32_MAX;
        if (!nuwax_asset_rle_decode_frame(&nuwax_low_battery_frames, next,
                                         s_mask_b, NUWAX_LOW_BATTERY_ICON_BYTES)) return false;
        s_mask_b_index = next;
    }
    return true;
}

static void render_pulse(lv_obj_t *object, uint32_t phase)
{
    if (!s_pixels || !s_masks || s_decode_failed) return;
    if (phase > PULSE_PHASE_MAX) phase = PULSE_PHASE_MAX;
    const uint32_t half = PULSE_PHASE_MAX / 2U;
    const float t = (float)(phase <= half ? phase : PULSE_PHASE_MAX-phase) / (float)half;
    /* Quintic easing has zero velocity AND acceleration at both reversals.
     * One forward timeline avoids a separate playback/repeat pause. */
    const float eased = t*t*t*(t*(t*6.0f-15.0f)+10.0f);
    uint32_t mix = (uint32_t)(eased * (NUWAX_LOW_BATTERY_ICON_STEPS*256U) + 0.5f);
    const uint32_t max_mix = NUWAX_LOW_BATTERY_ICON_STEPS*256U;
    if (mix > max_mix) mix = max_mix;
    if (mix == s_last_mix) return;
    s_last_mix = mix;
    const uint32_t first = mix >> 8;
    const uint32_t next = first < NUWAX_LOW_BATTERY_ICON_STEPS ? first+1U : first;
    const uint32_t fraction = mix & 255U;
    const uint32_t opacity = 256U - (mix*26U + max_mix/2U)/max_mix;
    if (!load_masks(first, next)) {
        s_decode_failed = true;
        lv_image_set_src(s_icon, &nuwax_low_battery_icon);
        ESP_LOGE("low_battery_ui", "invalid pulse asset; retaining static warning");
        return;
    }
    const uint8_t *a = s_mask_a;
    const uint8_t *b = next == first ? a : s_mask_b;
    /* 17 antialiased key shapes + fractional blending remove the old 11-step
     * scale quantisation. Fixed-size A8, 100..104%, brightness 100..90%.
     * No transform layer, allocator, file read or geometry update per frame. */
    for (uint32_t i=0; i<NUWAX_LOW_BATTERY_ICON_BYTES; ++i) {
        const uint32_t alpha = (a[i]*(256U-fraction) + b[i]*fraction + 128U) >> 8;
        s_pixels[i] = (uint8_t)((alpha*opacity + 128U) >> 8);
    }
    lv_obj_invalidate(object);
}

static void pulse(void *object, int32_t phase)
{
    /* The product LVGL clock runs at 5 ms. Bound this small overlay to 40 fps
     * without changing other UI scheduling; use elapsed animation time so a
     * late frame skips ahead instead of producing a burst of catch-up frames. */
    if (lv_tick_elaps(s_frame_tick) < PULSE_FRAME_MS) return;
    s_frame_tick = lv_tick_get();
    render_pulse(object, (uint32_t)phase);
}

static lv_obj_t *label(int y, const lv_font_t *font, const char *text,
                       lv_color_t color)
{
    lv_obj_t *obj = lv_label_create(s_root);
    if (!obj) return NULL;
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, 30, y);
    lv_obj_set_size(obj, 300, font->line_height);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(obj, color, 0);
    lv_label_set_text_static(obj, text);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

esp_err_t nuwax_low_battery_warning_create(lv_obj_t *screen)
{
    if (!screen) return ESP_ERR_INVALID_ARG;
    if (s_root) return ESP_ERR_INVALID_STATE;
    s_root = lv_obj_create(lv_display_get_layer_top(lv_obj_get_display(screen)));
    if (!s_root) return ESP_ERR_NO_MEM;
    lv_obj_remove_style_all(s_root);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_size(s_root, 360, 360);
    lv_obj_set_style_bg_color(s_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    s_icon = lv_image_create(s_root);
    lv_obj_t *title = label(190, &nuwax_font_low_battery_22, "电量不足", lv_color_white());
    lv_obj_t *detail = label(234, &nuwax_font_low_battery_14,
                           "即将关机，请连接充电器", lv_color_hex(0xAAAAAA));
    if (!s_icon || !title || !detail) {
        nuwax_low_battery_warning_delete();
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_style_text_letter_space(title, 1, 0);
    s_pixels = heap_caps_malloc(NUWAX_LOW_BATTERY_ICON_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_pixels) {
        s_frame = nuwax_low_battery_icon;
        s_frame.header.flags |= LV_IMAGE_FLAGS_MODIFIABLE;
        s_frame.data = s_pixels;
        s_last_mix = UINT32_MAX;
        memcpy(s_pixels, nuwax_low_battery_icon.data, NUWAX_LOW_BATTERY_ICON_BYTES);
        lv_image_set_src(s_icon, &s_frame);
    } else {
        /* Low memory must retain a readable warning and shutdown protection. */
        lv_image_set_src(s_icon, &nuwax_low_battery_icon);
        ESP_LOGW("low_battery_ui", "pulse buffer unavailable; retaining static warning");
    }
    lv_obj_set_pos(s_icon, 128, 95);
    lv_obj_set_style_image_recolor(s_icon, lv_color_white(), 0);
    lv_obj_set_style_image_recolor_opa(s_icon, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_icon, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return ESP_OK;
}

esp_err_t nuwax_low_battery_warning_show(bool visible)
{
    if (!s_root) return visible ? ESP_ERR_INVALID_STATE : ESP_OK;
    if (visible == s_visible) return ESP_OK;
    if (visible && nuwax_ota_progress_is_visible()) return ESP_ERR_INVALID_STATE;
    if (visible) {
        if (s_pixels && !s_masks)
            s_masks = heap_caps_malloc(2U * NUWAX_LOW_BATTERY_ICON_BYTES,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_mask_a = s_masks;
        s_mask_b = s_masks ? s_masks + NUWAX_LOW_BATTERY_ICON_BYTES : NULL;
        s_mask_a_index = s_mask_b_index = UINT32_MAX;
        s_decode_failed = false;
        lv_image_set_src(s_icon, s_pixels && s_masks ? &s_frame : &nuwax_low_battery_icon);
        if (s_pixels && !s_masks)
            ESP_LOGW("low_battery_ui", "pulse cache unavailable; retaining static warning");
        nuwax_face_set_overlay_covered(true);
        nuwax_drawer_set_power_blocked(true);
        lv_obj_remove_flag(s_root, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_root);
        s_last_mix = UINT32_MAX;
        s_frame_tick = lv_tick_get();
        render_pulse(s_icon, 0);
        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, s_icon);
        lv_anim_set_exec_cb(&anim, pulse);
        lv_anim_set_values(&anim, 0, PULSE_PHASE_MAX);
        lv_anim_set_duration(&anim, PULSE_CYCLE_MS);
        lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&anim, lv_anim_path_linear);
        if (s_pixels && s_masks && !s_decode_failed) lv_anim_start(&anim);
    } else {
        lv_anim_delete(s_icon, pulse);
        heap_caps_free(s_masks);
        s_masks = s_mask_a = s_mask_b = NULL;
        lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
        nuwax_drawer_set_power_blocked(false);
        nuwax_face_set_overlay_covered(nuwax_ota_progress_is_visible());
        lv_obj_invalidate(lv_display_get_screen_active(lv_obj_get_display(s_root)));
    }
    s_visible = visible;
    return ESP_OK;
}

void nuwax_low_battery_warning_delete(void)
{
    (void)nuwax_low_battery_warning_show(false);
    if (s_icon) lv_anim_delete(s_icon, pulse);
    if (s_root) lv_obj_delete(s_root);
    lv_image_cache_drop(&s_frame);
    if (s_pixels) heap_caps_free(s_pixels);
    heap_caps_free(s_masks);
    s_masks = s_mask_a = s_mask_b = NULL;
    s_pixels = NULL;
    s_root = NULL;
    s_icon = NULL;
    s_visible = false;
}

bool nuwax_low_battery_warning_is_visible(void) { return s_visible; }
