#pragma once

#include "lvgl.h"
#include "src/core/lv_refr_private.h"
#include "src/draw/lv_draw_private.h"
#include "src/misc/lv_area_private.h"

/* Called only by LVGL's scene hook, after its buffer ownership fence. The
 * original framebuffer stays under the same LVGL mutex as normal rendering.
 * Overlays retain their normal draw order; this never writes to the panel. */
static inline bool nuwax_rgb565_scene_render(
    lv_display_t *display, lv_layer_t *layer, lv_obj_t *image,
    lv_obj_t *patch_cover, const uint16_t *frame, int32_t width, int32_t height)
{
    if (!image || !frame || !layer || !layer->draw_buf ||
        layer->draw_task_head || layer->color_format != LV_COLOR_FORMAT_RGB565 ||
        layer->opa != LV_OPA_COVER || layer->recolor.alpha != LV_OPA_TRANSP ||
        lv_obj_get_parent(image) != lv_display_get_screen_active(display) ||
        lv_obj_has_flag(image, LV_OBJ_FLAG_HIDDEN) ||
        lv_obj_get_style_opa_recursive(image, 0) != LV_OPA_COVER ||
        lv_obj_get_style_image_opa(image, 0) != LV_OPA_COVER ||
        lv_obj_get_style_image_recolor_opa(image, 0) != LV_OPA_TRANSP ||
        lv_image_get_rotation(image) != 0 ||
        lv_image_get_scale_x(image) != LV_SCALE_NONE ||
        lv_image_get_scale_y(image) != LV_SCALE_NONE ||
        lv_obj_get_style_transform_rotation(image, 0) != 0 ||
        lv_obj_get_style_transform_scale_x_safe(image, 0) != LV_SCALE_NONE ||
        lv_obj_get_style_transform_scale_y_safe(image, 0) != LV_SCALE_NONE)
        return false;
    lv_area_t bounds;
    lv_obj_get_coords(image, &bounds);
    const lv_area_t *clip = &layer->_clip_area;
    if (bounds.x1 != 0 || bounds.y1 != 0 ||
        lv_area_get_width(&bounds) != width || lv_area_get_height(&bounds) != height ||
        clip->x1 < 0 || clip->y1 < 0 || clip->x2 >= width || clip->y2 >= height)
        return false;
    for (int32_t y = clip->y1; y <= clip->y2; ++y) {
        uint16_t *dst = (uint16_t *)(layer->draw_buf->data +
            (size_t)(y - layer->buf_area.y1) * layer->draw_buf->header.stride) +
            clip->x1 - layer->buf_area.x1;
        const uint16_t *src = frame + (size_t)y * width + clip->x1;
        size_t n = lv_area_get_width(clip);
        /* Explicit fixed-size copies keep IDF's no-builtin-memcpy setting
         * from routing every row through the byte-at-a-time ROM path. */
        if (((uintptr_t)dst & 3U) && n) { *dst++ = *src++; --n; }
        while (n >= 2) {
            uint32_t pair;
            if (((uintptr_t)src & 3U) == 0)
                __builtin_memcpy(&pair, src, sizeof(pair));
            else pair = src[0] | ((uint32_t)src[1] << 16);
            __builtin_memcpy(dst, &pair, sizeof(pair));
            src += 2; dst += 2; n -= 2;
        }
        if (n) *dst = *src;
    }
    lv_obj_t *screen = lv_obj_get_parent(image);
    bool above = false;
    for (uint32_t i = 0; i < lv_obj_get_child_count(screen); ++i) {
        lv_obj_t *child = lv_obj_get_child(screen, i);
        if (above && child != patch_cover) lv_obj_refr(layer, child);
        if (child == image) above = true;
    }
    return true;
}
