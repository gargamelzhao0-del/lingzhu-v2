#pragma once

#include <stdint.h>

typedef struct _lv_display_t lv_display_t;

typedef struct lv_obj_t {
    int unused;
} lv_obj_t;

typedef struct lv_event_t {
    int unused;
} lv_event_t;

typedef struct lv_layer_t {
    int unused;
} lv_layer_t;

typedef struct {
    int32_t x;
    int32_t y;
} lv_point_t;

typedef lv_point_t lv_point_precise_t;

typedef struct {
    int32_t x1;
    int32_t y1;
    int32_t x2;
    int32_t y2;
} lv_area_t;

typedef uint32_t lv_color_t;
typedef uint8_t lv_opa_t;

typedef struct {
    const lv_point_precise_t *points;
    uint32_t point_cnt;
    lv_color_t color;
    int32_t width;
    lv_opa_t opa;
    uint8_t round_start;
    uint8_t round_end;
} lv_draw_line_dsc_t;

typedef struct {
    lv_color_t bg_color;
    lv_opa_t bg_opa;
    int32_t radius;
} lv_draw_rect_dsc_t;

#define LV_OPA_COVER 255U
#define LV_RADIUS_CIRCLE 0x7fff
#define LV_OBJ_FLAG_SCROLLABLE (1U << 0)
#define LV_OBJ_FLAG_CLICKABLE (1U << 1)
#define LV_OBJ_FLAG_HIDDEN (1U << 2)
#define LV_EVENT_DRAW_MAIN 1

static inline lv_obj_t *lv_obj_create(lv_obj_t *parent)
{
    static lv_obj_t objects[20];
    static unsigned count;
    (void)parent;
    return count < 20U ? &objects[count++] : (lv_obj_t *)0;
}

static inline void lv_obj_delete(lv_obj_t *object) { (void)object; }
static inline void lv_obj_remove_style_all(lv_obj_t *object) { (void)object; }
static inline void lv_obj_set_size(lv_obj_t *object, int32_t width,
                                   int32_t height)
{
    (void)object;
    (void)width;
    (void)height;
}
static inline void lv_obj_set_pos(lv_obj_t *object, int32_t x, int32_t y)
{
    (void)object;
    (void)x;
    (void)y;
}
static inline lv_color_t lv_color_hex(uint32_t color) { return color; }
static inline void lv_obj_set_style_bg_color(lv_obj_t *object,
                                             lv_color_t color,
                                             int32_t selector)
{
    (void)object;
    (void)color;
    (void)selector;
}
static inline void lv_obj_set_style_bg_opa(lv_obj_t *object, uint32_t opacity,
                                           int32_t selector)
{
    (void)object;
    (void)opacity;
    (void)selector;
}
static inline void lv_obj_set_style_radius(lv_obj_t *object, int32_t radius,
                                           int32_t selector)
{
    (void)object;
    (void)radius;
    (void)selector;
}
static inline void lv_obj_remove_flag(lv_obj_t *object, uint32_t flags)
{
    (void)object;
    (void)flags;
}
static inline void lv_obj_add_flag(lv_obj_t *object, uint32_t flags)
{
    (void)object;
    (void)flags;
}
static inline void lv_obj_add_event_cb(lv_obj_t *object,
                                       void (*callback)(lv_event_t *),
                                       int32_t event_code,
                                       void *user_data)
{
    (void)object;
    (void)callback;
    (void)event_code;
    (void)user_data;
}
static inline lv_layer_t *lv_event_get_layer(lv_event_t *event)
{
    static lv_layer_t layer;
    return event ? &layer : (lv_layer_t *)0;
}
static inline void lv_obj_get_coords(lv_obj_t *object, lv_area_t *area)
{
    (void)object;
    if (area) {
        *area = (lv_area_t){0};
    }
}
static inline void lv_draw_line_dsc_init(lv_draw_line_dsc_t *descriptor)
{
    if (descriptor) {
        *descriptor = (lv_draw_line_dsc_t){0};
    }
}
static inline void lv_draw_rect_dsc_init(lv_draw_rect_dsc_t *descriptor)
{
    if (descriptor) {
        *descriptor = (lv_draw_rect_dsc_t){0};
    }
}
static inline void lv_draw_line(lv_layer_t *layer,
                                const lv_draw_line_dsc_t *descriptor)
{
    (void)layer;
    (void)descriptor;
}
static inline void lv_draw_rect(lv_layer_t *layer,
                                const lv_draw_rect_dsc_t *descriptor,
                                const lv_area_t *area)
{
    (void)layer;
    (void)descriptor;
    (void)area;
}
static inline void lv_obj_invalidate(lv_obj_t *object) { (void)object; }
static inline void lv_obj_move_to_index(lv_obj_t *object, int32_t index)
{
    (void)object;
    (void)index;
}
