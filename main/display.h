#pragma once

#include "app_state.h"
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

esp_err_t nuwax_display_init(void);
void nuwax_display_set_state(nuwax_app_state_t state);
esp_err_t nuwax_display_backlight_set_percent(uint8_t percent);
esp_err_t nuwax_display_backlight_force_off(void);
esp_err_t nuwax_display_te_selftest(uint32_t duration_ms, uint32_t *rising_edges);

esp_err_t display_clear_screen(void);
void display_draw_pixel(int16_t x, int16_t y, uint16_t color);
esp_err_t display_flush(void);
esp_err_t display_keepalive(void);
uint16_t *display_begin_frame_line(int16_t y);
esp_err_t display_flush_frame_line(int16_t y);
esp_err_t display_end_frame(void);
uint16_t *display_begin_region_line(int16_t x, int16_t y, int16_t w, int16_t h);
esp_err_t display_flush_region_line(void);
esp_err_t display_end_region(void);

/**
 * Write a block of pixels to a rectangular region.
 * Used by LVGL flush callback. Pixels use native RGB565 order; the driver
 * converts them to the panel byte order.
 */
esp_err_t display_write_region_pixels(int16_t x, int16_t y, int16_t w, int16_t h,
                                      const uint16_t *pixels, size_t pixel_count);

/**
 * Write a rectangular view from a larger native RGB565 framebuffer. `pixels`
 * points at the first pixel of the requested region and `source_stride_pixels`
 * is the distance between source rows. No temporary framebuffer is allocated.
 */
esp_err_t display_write_region_pixels_strided(
    int16_t x, int16_t y, int16_t w, int16_t h,
    const uint16_t *pixels, size_t source_stride_pixels);
