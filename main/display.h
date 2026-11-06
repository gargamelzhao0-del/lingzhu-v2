#pragma once

#include <stdbool.h>

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
/* Exclusive LVGL lease of the driver's existing DMA staging allocation.
 * Acquire/return only with the display pipeline drained. While leased, copy
 * fallbacks use the separate line buffer; the driver never writes this memory
 * except when it is explicitly submitted as an owned pixel source. */
void *display_acquire_draw_buffer(size_t bytes);
bool display_release_draw_buffer(void *buffer);
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

/**
 * Write a rectangular region, converting `pixels` to the panel byte order in
 * place. The caller retains buffer ownership until the transfer completes, so
 * a DMA-capable, 32-bit-aligned internal buffer streams to the QSPI engine
 * without the intermediate copy; other buffers fall back to the copy path.
 * The pixel data is byte-swapped on return and must not be re-read as native
 * RGB565 by the caller afterwards.
 */
esp_err_t display_write_region_pixels_swapped_inplace(
    int16_t x, int16_t y, int16_t w, int16_t h,
    uint16_t *pixels, size_t pixel_count);

/* Snapshot-and-reset diagnostics. Byte counts include only successful pixel
 * chunks, failures count failed pixel transactions. Phases may cross windows;
 * these totals are not a per-frame latency or pure SPI bandwidth measurement. */
typedef struct {
    uint64_t queue_us, copy_us, swap_us, tx_wait_us;
    uint64_t bounce_bytes, direct_bytes;
    uint32_t jobs, chunks, failures;
    uint64_t frame_us;
    uint32_t frames, frame_max_us;
} nuwax_display_perf_t;
void nuwax_display_perf_take(nuwax_display_perf_t *result);
/* Only called by perf-enabled display/flush code. */
void nuwax_display_perf_record(const nuwax_display_perf_t *sample);
