#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef void (*nuwax_lvgl_port_pre_handler_cb_t)(void *user_data);

/* Optional RGB565 compositor hook. It runs on the LVGL owner task after a
 * partial area is rendered and before the draw buffer is handed to the
 * asynchronous display worker. The callback may update pixels in place but
 * must not call LVGL or block. */
typedef void (*nuwax_lvgl_port_flush_filter_cb_t)(
    int32_t x, int32_t y, int32_t width, int32_t height,
    uint16_t *pixels, size_t stride_pixels, void *user_data);

typedef struct {
    int16_t x;
    int16_t y;
    int16_t width;
    int16_t height;
} nuwax_lvgl_port_rgb565_region_t;

/**
 * Initialize LVGL: display flush (current SPI), touch indev, tick, task.
 * Must be called after nuwax_display_init() and nuwax_touch_init().
 */
esp_err_t nuwax_lvgl_port_init(void);

/* Select the memory profile before initialization.  Provisioning keeps the
 * draw buffer in PSRAM so Wi-Fi/NimBLE management frames retain internal RAM;
 * normal runtime keeps the measured internal-RAM rendering path. */
void nuwax_lvgl_port_set_provisioning_memory_profile(bool enabled);

/* Migrate the live draw buffer while LVGL remains operational. Runtime BLE
 * provisioning uses a PSRAM buffer to return the internal render buffer to
 * NimBLE, then restores the normal internal buffer when enough RAM is
 * available. */
esp_err_t nuwax_lvgl_port_set_runtime_provisioning_memory(bool enabled);

/**
 * Register a lightweight callback that runs on the LVGL task while the LVGL
 * mutex is held, immediately before lv_timer_handler().
 */
void nuwax_lvgl_port_set_pre_handler_cb(nuwax_lvgl_port_pre_handler_cb_t cb, void *user_data);

/* Register the single display compositor hook. Passing NULL unregisters it.
 * Registration is thread-safe; UI owners should unregister before releasing
 * any callback state. */
void nuwax_lvgl_port_set_flush_filter(
    nuwax_lvgl_port_flush_filter_cb_t cb, void *user_data);

/* Wake the LVGL owner after a producer posts UI work. Thread-safe and
 * non-blocking; the next pre-handler pass drains the producer mailbox. */
void nuwax_lvgl_port_wake(void);

/**
 * Acquire LVGL mutex. Must be held when calling any lv_* API from outside the LVGL task.
 */
bool nuwax_lvgl_port_lock(uint32_t timeout_ms);

/**
 * Return true when the current task owns the LVGL mutex. Lifecycle APIs use
 * this to reject unsafe create/deinit calls before touching LVGL objects.
 */
bool nuwax_lvgl_port_lock_is_held_by_current_task(void);

/**
 * Present bounded regions directly from a full native RGB565 framebuffer.
 * The caller must be the current LVGL mutex owner. Outstanding asynchronous
 * LVGL flushes are drained with a bounded wait before the panel is touched.
 */
esp_err_t nuwax_lvgl_port_present_rgb565_regions(
    const uint16_t *framebuffer, size_t source_stride_pixels,
    const nuwax_lvgl_port_rgb565_region_t *regions, size_t region_count,
    uint32_t timeout_ms);

/**
 * Release LVGL mutex.
 */
void nuwax_lvgl_port_unlock(void);
