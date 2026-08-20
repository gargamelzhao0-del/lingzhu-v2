#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "nuwax_ui_v28/nuwax_face.h"

#define CAPTURE_W 360
#define CAPTURE_H 360
#define DRAW_LINES 24

static uint16_t s_framebuffer[CAPTURE_W * CAPTURE_H];
static uint16_t s_draw_buffer[CAPTURE_W * DRAW_LINES];
static uint32_t s_tick_ms = 1000U;
static lv_display_t *s_display;

static uint32_t capture_tick_cb(void) { return s_tick_ms; }

static void capture_flush_cb(lv_display_t *display,
                             const lv_area_t *area,
                             uint8_t *pixels)
{
    const int32_t width = lv_area_get_width(area);
    const uint16_t *source = (const uint16_t *)pixels;
    for (int32_t y = area->y1; y <= area->y2; ++y) {
        for (int32_t x = area->x1; x <= area->x2; ++x) {
            const uint16_t pixel = *source++;
            if (x >= 0 && x < CAPTURE_W && y >= 0 && y < CAPTURE_H) {
                s_framebuffer[y * CAPTURE_W + x] = pixel;
            }
        }
        if (width <= 0) break;
    }
    lv_display_flush_ready(display);
}

static void run_until(uint32_t target_ms)
{
    while (s_tick_ms < target_ms) {
        const uint32_t remaining = target_ms - s_tick_ms;
        s_tick_ms += remaining > 5U ? 5U : remaining;
        nuwax_face_tick();
        lv_anim_refr_now();
        lv_timer_handler();
        lv_refr_now(s_display);
    }
}

static bool write_ppm(const char *path)
{
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    fprintf(file, "P6\n%d %d\n255\n", CAPTURE_W, CAPTURE_H);
    for (size_t i = 0; i < CAPTURE_W * CAPTURE_H; ++i) {
        const uint16_t pixel = s_framebuffer[i];
        const uint8_t rgb[3] = {
            (uint8_t)((((pixel >> 11) & 0x1f) * 255 + 15) / 31),
            (uint8_t)((((pixel >> 5) & 0x3f) * 255 + 31) / 63),
            (uint8_t)(((pixel & 0x1f) * 255 + 15) / 31),
        };
        fwrite(rgb, 1, sizeof(rgb), file);
    }
    fclose(file);
    return true;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <elapsed_ms> <output.ppm>\n", argv[0]);
        return 2;
    }
    const long elapsed_ms = strtol(argv[1], NULL, 10);
    if (elapsed_ms < 0 || elapsed_ms > 60000) return 2;

    lv_init();
    lv_tick_set_cb(capture_tick_cb);
    s_display = lv_display_create(CAPTURE_W, CAPTURE_H);
    if (!s_display) return 3;
    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_display, capture_flush_cb);
    lv_display_set_buffers(s_display, s_draw_buffer, NULL,
                           sizeof(s_draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    if (nuwax_face_create(screen) != ESP_OK) return 4;

    nuwax_device_state_t state = {0};
    state.emotion = NUWAX_EMOTION_SPEAKING;
    state.semantic_emotion_active = true;
    state.wifi = NUWAX_WIFI_CONNECTED;
    state.cloud = NUWAX_CLOUD_CONNECTED;
    state.dialog = NUWAX_DIALOG_SPEAKING;
    nuwax_face_update(&state);
    lv_obj_invalidate(screen);
    run_until(1000U + (uint32_t)elapsed_ms);
    lv_obj_invalidate(screen);
    lv_refr_now(s_display);
    return write_ppm(argv[2]) ? 0 : 5;
}
