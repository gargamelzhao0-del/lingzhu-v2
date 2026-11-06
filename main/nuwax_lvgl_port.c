#include "nuwax_lvgl_port.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "display.h"
#include "display_maintenance_policy.h"
#include "device_error_report.h"
#include "diagnostics.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "touch_input.h"
#include "replay_ui_schedule.h"
#include "ui_memory_pressure.h"

static const char *TAG = "nuwax_lvgl_port";
static atomic_uint s_interaction_deadline_ms;
static atomic_uint s_external_wake_generation;
/* Written only by the LVGL owner before it drains producer state. */
static unsigned s_external_wake_serviced;

void nuwax_lvgl_port_note_interaction(void) {
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    atomic_store_explicit(&s_interaction_deadline_ms, now + 300U, memory_order_relaxed);
}

bool nuwax_lvgl_port_background_work_deferred(void) {
    const uint32_t deadline = atomic_load_explicit(&s_interaction_deadline_ms, memory_order_relaxed);
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    return deadline != 0U && (int32_t)(deadline - now) > 0;
}

static void report_ui_warning(const char *code, esp_err_t result) {
    (void)nuwax_device_warning_report_enqueue(
        "display", code, result, "ui", 0U, NULL);
}

static void report_ui_warning_detail(const char *code,
                                     esp_err_t result,
                                     size_t internal_free,
                                     size_t internal_largest,
                                     size_t required_free,
                                     size_t buffer_size) {
    char detail[NUWAX_DEVICE_ERROR_RESULT_BYTES];
    snprintf(detail,
             sizeof(detail),
             "%s;internal=%u;largest=%u;required=%u;buffer=%u",
             esp_err_to_name(result),
             (unsigned)internal_free,
             (unsigned)internal_largest,
             (unsigned)required_free,
             (unsigned)buffer_size);
    (void)nuwax_device_warning_report_enqueue_detail(
        "display", code, result, detail, "ui", 0U, NULL);
}

#ifndef CONFIG_NUWAX_UI_ASYNC_FLUSH_ENABLE
#define CONFIG_NUWAX_UI_ASYNC_FLUSH_ENABLE 1
#endif
#ifndef CONFIG_NUWAX_UI_DOUBLE_BUFFER_ENABLE
#define CONFIG_NUWAX_UI_DOUBLE_BUFFER_ENABLE 1
#endif
#ifndef CONFIG_NUWAX_UI_DRAW_BUFFER_LINES
#define CONFIG_NUWAX_UI_DRAW_BUFFER_LINES 20
#endif
#ifndef CONFIG_NUWAX_UI_FLUSH_TASK_STACK_SIZE
#define CONFIG_NUWAX_UI_FLUSH_TASK_STACK_SIZE 4096
#endif

/* 24-minute perf telemetry (stack_hw=15552 on the former 20 KiB stack) shows a
 * ~4.9 KiB peak, so 12 KiB keeps a 2.5x margin while returning 8 KiB of scarce
 * internal RAM. The 512 B underflow guard and the <4 KiB reserve warning below
 * still protect the deep render paths. */
#define LVGL_TASK_STACK_SIZE (12 * 1024)
#define LVGL_TASK_STACK_GUARD_BYTES 512U
#define LVGL_TASK_STACK_GUARD_PATTERN 0xa7U
#define LVGL_TASK_STACK_WARN_BYTES (4 * 1024U)
#define LVGL_TASK_STACK_CHECK_INTERVAL_US (2LL * 1000LL * 1000LL)
#define LVGL_TASK_PRIORITY   7
/* Keep rendering, emotion decode, and display flush on CPU0. Playback wake
 * capture and MultiNet own CPU1's realtime audio budget; mixing both pipelines
 * on CPU1 can starve IDLE1 and makes animation cadence collapse during TTS. */
#define LVGL_TASK_CORE       0
#define LVGL_TICK_PERIOD_MS  2
#define LVGL_MIN_SLEEP_MS    1
#define LVGL_MAX_SLEEP_MS    4
#define LVGL_EXTERNAL_WAKE_STREAK_LIMIT 8U
#define DISPLAY_KEEPALIVE_MS 15000
#define DISPLAY_KEEPALIVE_REFRESH_PX 4

#define DISP_WIDTH   360
#define DISP_HEIGHT  360
#define DRAW_BUF_LINES CONFIG_NUWAX_UI_DRAW_BUFFER_LINES
#define DRAW_BUF_SIZE  (DISP_WIDTH * DRAW_BUF_LINES * sizeof(uint16_t))
/* Two non-overlapping strips inside the existing display DMA lease. This
 * fallback costs no extra internal RAM and preserves the driver's ownership. */
#define DRAW_BUF_SPLIT_SIZE (DISP_WIDTH * (DRAW_BUF_LINES / 2U) * sizeof(uint16_t))
#define DRAW_BUF_DOUBLE_BUFFER_ENABLE CONFIG_NUWAX_UI_DOUBLE_BUFFER_ENABLE
/* Keep the owner above background work, while allowing the worker to run as
 * soon as the owner blocks in flush_wait_cb. */
/* Start DMA as soon as a frame stripe is queued. The worker blocks in
 * spi_device_transmit(), allowing the LVGL owner to compose the other buffer.
 * A lower-priority worker cannot start while LVGL draws that next stripe on
 * the same core, defeating overlap even with two buffers. */
#define FLUSH_TASK_PRIORITY (LVGL_TASK_PRIORITY + 1)
#define FLUSH_QUEUE_DEPTH 2
#define FLUSH_PIPELINE_CAPACITY FLUSH_QUEUE_DEPTH
#define FLUSH_PIPELINE_WARN_MS 100
#define FLUSH_PIPELINE_CONTROL_TIMEOUT_MS 1000
#define FLUSH_PIPELINE_FATAL_TIMEOUT_MS 2000
#define FLUSH_WAIT_TIMEOUT_MS 2500
#define FLUSH_TASK_STOP_TIMEOUT_MS 500
#define DOUBLE_BUFFER_HEALTH_INTERVAL_MS 500
#define DOUBLE_BUFFER_RESTORE_STABLE_MS 2000
#define DOUBLE_BUFFER_RESTORE_COOLDOWN_MS 30000
#define DOUBLE_BUFFER_RESTORE_WARNING_COOLDOWN_MS (5U * 60U * 1000U)
#define DOUBLE_BUFFER_RESTORE_HEADROOM 8000U
/* Internal RAM to keep free AFTER carving out the LVGL draw buffer.
 *
 * LVGL renders pixels on the CPU into this buffer; on the ESP32-S3, writing to
 * PSRAM is ~an order of magnitude slower than internal SRAM, so a PSRAM buffer
 * makes animation-heavy UI render in ~88ms/frame instead of single-digit ms
 * (measured: render avg 88ms vs QSPI flush avg 1.6ms -- the bottleneck is the
 * buffer location, not the display driver).
 *
 * The old 32 KiB reserve forced the 7.2 KiB strip into PSRAM on the product
 * build, even when a safe single internal strip was available. Keep a 12 KiB
 * post-allocation reserve for one strip; require a separate 24 KiB reserve
 * before taking a second strip so BLE, Wi-Fi, audio DMA and OTA TLS retain
 * headroom under load. A pair of 20-line buffers uses 28.8 KiB total, halves
 * the strip traversal count for the control center, and is enabled only when
 * that larger reserve remains available. Runtime pressure can still release
 * the second 14.4 KiB strip without stopping the UI. */
#define LVGL_INTERNAL_SINGLE_BUFFER_MIN_FREE 12000U
#define LVGL_INTERNAL_DRAW_BUFFER_MIN_FREE 24000U
static lv_display_t *s_display;
#if CONFIG_NUWAX_UI_LVGL_TOUCH_INDEV_ENABLE
static lv_indev_t *s_indev;
#endif
static SemaphoreHandle_t s_lvgl_mutex;
static nuwax_lvgl_port_pre_handler_cb_t s_pre_handler_cb;
static void *s_pre_handler_user_data;
static nuwax_lvgl_port_flush_filter_cb_t s_flush_filter_cb;
static void *s_flush_filter_user_data;
static esp_timer_handle_t s_tick_timer;
static esp_timer_handle_t s_wake_timer;
static bool s_provisioning_memory_profile;
static uint8_t *s_draw_buf1;
static uint8_t *s_draw_buf2;
static bool s_draw_buf1_internal;
static bool s_draw_buf2_internal;
static bool s_draw_buffers_split;
/* Returned leases stay allocated in the driver; only owned buffers are freed.
 * Call after both the LVGL owner and the transfer pipeline are fenced. */
static uint8_t *s_driver_draw_buffer;
static void release_draw_buffer(void *buffer) {
    if (!buffer) return;
    if (display_release_draw_buffer(buffer)) s_driver_draw_buffer = NULL;
    else heap_caps_free(buffer);
}

/* A compact pair has one allocation/lease. The second pointer is an interior
 * view and must never be freed or returned to the display driver separately. */
static void release_draw_buffer_pair(void *first, void *second, bool split) {
    release_draw_buffer(first);
    if (!split) release_draw_buffer(second);
}

static QueueHandle_t s_flush_queue;
/* Completion tokens are consumed by LVGL's flush_wait_cb.  Without this
 * semaphore LVGL falls back to a tight `while (flushing)` loop, which can
 * starve IDLE1 while a display transfer is delayed. */
static SemaphoreHandle_t s_flush_done;
/* One token owns one LVGL draw buffer until its LCD transfer completes.
 * Keeping two tokens lets LVGL render into the second buffer while the flush
 * worker transfers the first. Control operations take every token, which is
 * the pipeline-wide quiesce barrier used before changing buffer ownership. */
static SemaphoreHandle_t s_flush_slots;
static TaskHandle_t s_flush_task;
static StaticTask_t *s_flush_task_tcb;
static StackType_t *s_flush_task_stack;
static bool s_async_flush_active;
static atomic_bool s_flush_task_exited;
static atomic_bool s_panel_output_enabled = ATOMIC_VAR_INIT(false);
static uint32_t s_flush_queue_fallbacks;
static uint32_t s_flush_errors;
static uint32_t s_flush_wait_timeouts;
static int64_t s_double_buffer_last_health_us;
static nuwax_ui_memory_pressure_t s_double_buffer_pressure;
static int64_t s_double_buffer_restore_candidate_us;
static int64_t s_double_buffer_released_us;
static bool s_internal_restore_incident_active;
/* Keep a low-memory incident quiet across a UI teardown/rebuild. The
 * incident flag is intentionally reset by a successful buffer restore, while
 * this timestamp prevents a failed rebuild from starting a new warning storm
 * every few seconds. */
static int64_t s_buffer_restore_last_report_us;
static bool s_runtime_internal_preferred = true;
static TaskHandle_t s_lvgl_task;
static uint8_t *s_lvgl_task_stack_allocation;
static StackType_t *s_lvgl_task_stack;
static int64_t s_lvgl_stack_last_check_us;
static atomic_bool s_wake_timer_fired;
static uint32_t s_external_wake_streak;

typedef struct {
    bool stop;
    lv_display_t *display;
    const uint16_t *pixels;
    int32_t x1;
    int32_t y1;
    int32_t src_w;
    int32_t src_h;
    int32_t clip_x1;
    int32_t clip_y1;
    int32_t clip_w;
    int32_t clip_h;
    int64_t queued_us;
#if CONFIG_NUWAX_UI_PERF_LOG_ENABLE
    int64_t frame_started_us;
    bool last_in_frame;
#endif
} lvgl_flush_job_t;

typedef enum {
    LVGL_PORT_UNINITIALIZED = 0,
    LVGL_PORT_INITIALIZING,
    LVGL_PORT_READY,
} lvgl_port_init_state_t;

static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_pre_handler_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_flush_filter_mux = portMUX_INITIALIZER_UNLOCKED;
static lvgl_port_init_state_t s_init_state;

static bool lvgl_async_flush_deinit(void);

#if CONFIG_NUWAX_UI_PERF_LOG_ENABLE
static int64_t s_refr_start_us;
static int64_t s_render_start_us;
static uint32_t s_refr_count;
static uint64_t s_refr_total_us;
static uint32_t s_refr_max_us;
static uint32_t s_render_count;
static uint64_t s_render_total_us;
static uint32_t s_render_max_us;
/* Owner-thread counters; transport-worker totals are reported separately. */
static uint64_t s_filter_us, s_owner_wait_us, s_render_work_us;
static uint32_t s_filter_max_us, s_owner_wait_max_us, s_render_work_max_us;
static uint64_t s_render_filter_start, s_render_wait_start;
static uint64_t s_refr_phase_us[3];
static uint32_t s_refr_phase_max_us[3], s_refr_parts, s_refr_culled, s_refr_cached;
void nuwax_lvgl_record_refr_phase(unsigned phase, uint32_t elapsed_us);
void nuwax_lvgl_record_refr_phase(unsigned phase, uint32_t elapsed_us) {
    if (phase == 4) { s_refr_cached++; return; }
    if (phase == 3) { s_refr_culled++; return; }
    if (phase >= 3) return;
    s_refr_phase_us[phase] += elapsed_us;
    if (elapsed_us > s_refr_phase_max_us[phase]) s_refr_phase_max_us[phase] = elapsed_us;
    if (phase == 0) s_refr_parts++;
}
static uint64_t s_sw_draw_us[7];
static uint32_t s_sw_draw_count, s_sw_draw_max_us;
void nuwax_lvgl_record_sw_draw(unsigned type, uint32_t elapsed_us);
void nuwax_lvgl_record_sw_draw(unsigned type, uint32_t elapsed_us) {
    unsigned group;
    switch (type) {
        case LV_DRAW_TASK_TYPE_FILL: group = 0; break;
        case LV_DRAW_TASK_TYPE_BORDER: group = 1; break;
        case LV_DRAW_TASK_TYPE_LETTER:
        case LV_DRAW_TASK_TYPE_LABEL: group = 2; break;
        case LV_DRAW_TASK_TYPE_IMAGE: group = 3; break;
        case LV_DRAW_TASK_TYPE_LAYER: group = 4; break;
        case LV_DRAW_TASK_TYPE_MASK_RECTANGLE: group = 5; break;
        default: group = 6; break;
    }
    s_sw_draw_us[group] += elapsed_us;
    s_sw_draw_count++;
    if (elapsed_us > s_sw_draw_max_us) s_sw_draw_max_us = elapsed_us;
}
static uint32_t s_flush_count;
static uint64_t s_flush_pixels;
static uint64_t s_flush_total_us;
static uint32_t s_flush_max_us;
static uint32_t s_handler_count;
static uint64_t s_handler_total_us;
static uint32_t s_handler_max_us;
static uint32_t s_handler_over_16ms;
static uint32_t s_handler_over_33ms;
static int64_t s_handler_last_log_us;
#endif

static int64_t s_display_last_keepalive_us;
static nuwax_display_maintenance_t s_display_maintenance;

static void lvgl_port_set_init_state(lvgl_port_init_state_t state) {
    taskENTER_CRITICAL(&s_init_mux);
    s_init_state = state;
    taskEXIT_CRITICAL(&s_init_mux);
}

static bool lvgl_port_init_cleanup(bool tick_started) {
    atomic_store_explicit(&s_panel_output_enabled, false,
                          memory_order_release);
    if (s_tick_timer) {
        if (tick_started) {
            esp_err_t stop_err = esp_timer_stop(s_tick_timer);
            if (stop_err != ESP_OK && stop_err != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "LVGL tick timer stop during rollback failed: %s",
                         esp_err_to_name(stop_err));
            }
        }
        esp_err_t delete_err = esp_timer_delete(s_tick_timer);
        if (delete_err != ESP_OK) {
            ESP_LOGW(TAG, "LVGL tick timer delete during rollback failed: %s",
                     esp_err_to_name(delete_err));
        }
        s_tick_timer = NULL;
    }
    if (s_wake_timer) {
        (void)esp_timer_stop(s_wake_timer);
        (void)esp_timer_delete(s_wake_timer);
        s_wake_timer = NULL;
    }
    if (!lvgl_async_flush_deinit()) {
        ESP_LOGE(TAG,
                 "LVGL async flush rollback did not quiesce; preserving display and draw-buffer ownership");
        return false;
    }

#if CONFIG_NUWAX_UI_LVGL_TOUCH_INDEV_ENABLE
    if (s_indev) {
        lv_indev_delete(s_indev);
        s_indev = NULL;
    }
#endif
    if (s_display) {
        lv_display_delete(s_display);
        s_display = NULL;
    }
    release_draw_buffer_pair(s_draw_buf1, s_draw_buf2, s_draw_buffers_split);
    s_draw_buf1 = s_draw_buf2 = NULL;
    s_draw_buffers_split = false;
    s_draw_buf1_internal = false;
    s_draw_buf2_internal = false;
    s_driver_draw_buffer = NULL;
    taskENTER_CRITICAL(&s_pre_handler_mux);
    s_pre_handler_cb = NULL;
    s_pre_handler_user_data = NULL;
    taskEXIT_CRITICAL(&s_pre_handler_mux);
    taskENTER_CRITICAL(&s_flush_filter_mux);
    s_flush_filter_cb = NULL;
    s_flush_filter_user_data = NULL;
    taskEXIT_CRITICAL(&s_flush_filter_mux);
    s_display_last_keepalive_us = 0;
    s_display_maintenance = (nuwax_display_maintenance_t){0};
    s_double_buffer_last_health_us = 0;
    s_double_buffer_pressure.deadline_us = 0;
    s_double_buffer_restore_candidate_us = 0;
    s_double_buffer_released_us = 0;
    s_internal_restore_incident_active = false;
    s_runtime_internal_preferred = !s_provisioning_memory_profile;
    atomic_store_explicit(&s_wake_timer_fired, false, memory_order_release);
    s_external_wake_streak = 0U;
    if (s_lvgl_mutex) {
        vSemaphoreDelete(s_lvgl_mutex);
        s_lvgl_mutex = NULL;
    }
    return true;
}

static TickType_t delay_ticks_min1(uint32_t delay_ms) {
    TickType_t ticks = pdMS_TO_TICKS(delay_ms);
    return ticks > 0 ? ticks : 1;
}

#if CONFIG_NUWAX_UI_PERF_LOG_ENABLE
/* P3-2 diagnostic: bytes/blocks the custom LVGL allocator currently holds in
 * internal SRAM. Defined in lvgl_psram_mem.c (no public header). Reads 0 under
 * the current pure psram-first policy; becomes meaningful if internal-first
 * borrowing is re-enabled after P3-1 wake-word slimming. */
#if CONFIG_LV_USE_CUSTOM_MALLOC
void nuwax_lvgl_mem_internal_usage(size_t *out_internal_bytes, size_t *out_internal_count);
#endif

static void perf_reset_window(int64_t now_us) {
    s_refr_count = 0;
    s_refr_total_us = 0;
    s_refr_max_us = 0;
    s_render_count = 0;
    s_render_total_us = 0;
    s_render_max_us = 0;
    memset(s_refr_phase_us, 0, sizeof(s_refr_phase_us));
    memset(s_refr_phase_max_us, 0, sizeof(s_refr_phase_max_us));
    s_refr_parts = s_refr_culled = s_refr_cached = 0;
    memset(s_sw_draw_us, 0, sizeof(s_sw_draw_us));
    s_sw_draw_count = s_sw_draw_max_us = 0;
    s_filter_us = s_owner_wait_us = s_render_work_us = 0;
    s_filter_max_us = s_owner_wait_max_us = s_render_work_max_us = 0;
    s_flush_count = 0;
    s_flush_pixels = 0;
    s_flush_total_us = 0;
    s_flush_max_us = 0;
    s_handler_count = 0;
    s_handler_total_us = 0;
    s_handler_max_us = 0;
    s_handler_over_16ms = 0;
    s_handler_over_33ms = 0;
    s_handler_last_log_us = now_us;
}

static uint32_t perf_rate_x10(uint32_t count, uint32_t window_ms) {
    if (window_ms == 0) return 0;
    return (uint32_t)(((uint64_t)count * 10000ULL) / (uint64_t)window_ms);
}

static uint32_t perf_percent_x10(uint32_t value, uint32_t total) {
    if (total == 0) return 0;
    return (uint32_t)(((uint64_t)value * 1000ULL) / (uint64_t)total);
}

static void perf_display_event_cb(lv_event_t *e) {
    const lv_event_code_t code = lv_event_get_code(e);
    const int64_t now_us = esp_timer_get_time();
    if (code == LV_EVENT_REFR_START) {
        s_refr_start_us = now_us;
        return;
    }
    if (code == LV_EVENT_RENDER_START) {
        s_render_start_us = now_us;
        s_render_filter_start = s_filter_us;
        s_render_wait_start = s_owner_wait_us;
        return;
    }
    if (code == LV_EVENT_RENDER_READY) {
        if (s_render_start_us > 0 && now_us >= s_render_start_us) {
            const uint32_t elapsed_us = (uint32_t)(now_us - s_render_start_us);
            s_render_count++;
            s_render_total_us += elapsed_us;
            const uint64_t accounted = (s_filter_us - s_render_filter_start) +
                                       (s_owner_wait_us - s_render_wait_start);
            const uint32_t work = accounted < elapsed_us ? elapsed_us - accounted : 0;
            s_render_work_us += work;
            if (work > s_render_work_max_us) s_render_work_max_us = work;
            if (elapsed_us > s_render_max_us) {
                s_render_max_us = elapsed_us;
            }
        }
        s_render_start_us = 0;
        return;
    }
    if (code == LV_EVENT_REFR_READY) {
        if (s_refr_start_us > 0 && now_us >= s_refr_start_us) {
            const uint32_t elapsed_us = (uint32_t)(now_us - s_refr_start_us);
            s_refr_count++;
            s_refr_total_us += elapsed_us;
            if (elapsed_us > s_refr_max_us) {
                s_refr_max_us = elapsed_us;
            }
        }
        s_refr_start_us = 0;
    }
}

static void record_handler_stats(uint32_t elapsed_us) {
    s_handler_count++;
    s_handler_total_us += elapsed_us;
    if (elapsed_us > s_handler_max_us) {
        s_handler_max_us = elapsed_us;
    }
    if (elapsed_us > 16000) {
        s_handler_over_16ms++;
    }
    if (elapsed_us > 33000) {
        s_handler_over_33ms++;
    }

    const int64_t now_us = esp_timer_get_time();
    if (s_handler_last_log_us == 0) {
        s_handler_last_log_us = now_us;
    }
    if (now_us - s_handler_last_log_us >= (int64_t)CONFIG_NUWAX_UI_PERF_LOG_INTERVAL_MS * 1000LL) {
        const uint32_t window_ms = (uint32_t)((now_us - s_handler_last_log_us) / 1000);
        const uint32_t avg_us = s_handler_count > 0 ? (uint32_t)(s_handler_total_us / s_handler_count) : 0;
        const uint32_t refr_avg_us = s_refr_count > 0 ? (uint32_t)(s_refr_total_us / s_refr_count) : 0;
        const uint32_t render_avg_us = s_render_count > 0 ? (uint32_t)(s_render_total_us / s_render_count) : 0;
        const uint32_t flush_avg_us = s_flush_count > 0 ? (uint32_t)(s_flush_total_us / s_flush_count) : 0;
        const uint32_t handler_hz_x10 = perf_rate_x10(s_handler_count, window_ms);
        const uint32_t render_fps_x10 = perf_rate_x10(s_render_count, window_ms);
        const uint32_t refr_fps_x10 = perf_rate_x10(s_refr_count, window_ms);
        const uint32_t flush_hz_x10 = perf_rate_x10(s_flush_count, window_ms);
        const uint32_t long_16_pct_x10 = perf_percent_x10(s_handler_over_16ms, s_handler_count);
        const uint32_t long_33_pct_x10 = perf_percent_x10(s_handler_over_33ms, s_handler_count);
        const uint32_t avg_flush_px = s_flush_count > 0 ? (uint32_t)(s_flush_pixels / s_flush_count) : 0;
        const uint32_t flush_px_per_s = window_ms > 0 ? (uint32_t)((s_flush_pixels * 1000ULL) / window_ms) : 0;
        const uint32_t dirty_pct_x10 = s_refr_count > 0
            ? (uint32_t)((s_flush_pixels * 1000ULL) / ((uint64_t)DISP_WIDTH * DISP_HEIGHT * s_refr_count))
            : 0;
        const UBaseType_t lvgl_stack = uxTaskGetStackHighWaterMark(NULL);
        const size_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        const size_t internal_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        const size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        const size_t psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        const size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        size_t lvgl_internal_bytes = 0;
        size_t lvgl_internal_blocks = 0;
#if CONFIG_LV_USE_CUSTOM_MALLOC
        nuwax_lvgl_mem_internal_usage(&lvgl_internal_bytes, &lvgl_internal_blocks);
#endif
        ESP_LOGI(TAG,
                 "ui perf window=%ums handler count=%u hz=%u.%u avg_us=%u max_us=%u over16=%u pct=%u.%u over33=%u pct=%u.%u render count=%u fps=%u.%u avg_us=%u max_us=%u refr count=%u fps=%u.%u avg_us=%u max_us=%u flush count=%u hz=%u.%u pixels=%llu avg_px=%u px_s=%u dirty_pct=%u.%u avg_us=%u max_us=%u stack_hw=%u internal=%u internal_min=%u internal_largest=%u psram=%u psram_largest=%u lvgl_internal=%u lvgl_internal_blk=%u",
                 (unsigned)window_ms,
                 (unsigned)s_handler_count,
                 (unsigned)(handler_hz_x10 / 10),
                 (unsigned)(handler_hz_x10 % 10),
                 (unsigned)avg_us,
                 (unsigned)s_handler_max_us,
                 (unsigned)s_handler_over_16ms,
                 (unsigned)(long_16_pct_x10 / 10),
                 (unsigned)(long_16_pct_x10 % 10),
                 (unsigned)s_handler_over_33ms,
                 (unsigned)(long_33_pct_x10 / 10),
                 (unsigned)(long_33_pct_x10 % 10),
                 (unsigned)s_render_count,
                 (unsigned)(render_fps_x10 / 10),
                 (unsigned)(render_fps_x10 % 10),
                 (unsigned)render_avg_us,
                 (unsigned)s_render_max_us,
                 (unsigned)s_refr_count,
                 (unsigned)(refr_fps_x10 / 10),
                 (unsigned)(refr_fps_x10 % 10),
                 (unsigned)refr_avg_us,
                 (unsigned)s_refr_max_us,
                 (unsigned)s_flush_count,
                 (unsigned)(flush_hz_x10 / 10),
                 (unsigned)(flush_hz_x10 % 10),
                 (unsigned long long)s_flush_pixels,
                 (unsigned)avg_flush_px,
                 (unsigned)flush_px_per_s,
                 (unsigned)(dirty_pct_x10 / 10),
                 (unsigned)(dirty_pct_x10 % 10),
                 (unsigned)flush_avg_us,
                 (unsigned)s_flush_max_us,
                 (unsigned)lvgl_stack,
                 (unsigned)internal,
                 (unsigned)internal_min,
                 (unsigned)internal_largest,
                 (unsigned)psram,
                 (unsigned)psram_largest,
                 (unsigned)lvgl_internal_bytes,
                 (unsigned)lvgl_internal_blocks);
        if (s_handler_max_us >= CONFIG_NUWAX_UI_LONG_HANDLER_WARN_US) {
            ESP_LOGW(TAG,
                     "ui long handler max_us=%u threshold_us=%u over16=%u over33=%u",
                     (unsigned)s_handler_max_us,
                     (unsigned)CONFIG_NUWAX_UI_LONG_HANDLER_WARN_US,
                     (unsigned)s_handler_over_16ms,
                     (unsigned)s_handler_over_33ms);
            report_ui_warning("long_handler", ESP_ERR_TIMEOUT);
        }
        nuwax_display_perf_t display_perf;
        ESP_LOGI(TAG, "ui stages draw_other_us=%llu draw_other_max_us=%u compose_us=%llu compose_max_us=%u owner_wait_us=%llu owner_wait_max_us=%u",
                 (unsigned long long)s_render_work_us, (unsigned)s_render_work_max_us,
                 (unsigned long long)s_filter_us, (unsigned)s_filter_max_us,
                 (unsigned long long)s_owner_wait_us, (unsigned)s_owner_wait_max_us);
        ESP_LOGI(TAG, "ui refresh parts=%u culled=%u cached_parts=%u cover_us=%llu cover_max_us=%u tree_us=%llu tree_max_us=%u drain_us=%llu drain_max_us=%u",
                 (unsigned)s_refr_parts, (unsigned)s_refr_culled, (unsigned)s_refr_cached,
                 (unsigned long long)s_refr_phase_us[0], (unsigned)s_refr_phase_max_us[0],
                 (unsigned long long)s_refr_phase_us[1], (unsigned)s_refr_phase_max_us[1],
                 (unsigned long long)s_refr_phase_us[2], (unsigned)s_refr_phase_max_us[2]);
        ESP_LOGI(TAG, "ui sw draw tasks=%u max_us=%u fill_us=%llu border_us=%llu text_us=%llu image_us=%llu layer_us=%llu mask_us=%llu other_us=%llu buffers=%s/%s bytes_each=%u split_lease=%u",
                 (unsigned)s_sw_draw_count, (unsigned)s_sw_draw_max_us,
                 (unsigned long long)s_sw_draw_us[0], (unsigned long long)s_sw_draw_us[1],
                 (unsigned long long)s_sw_draw_us[2], (unsigned long long)s_sw_draw_us[3],
                 (unsigned long long)s_sw_draw_us[4], (unsigned long long)s_sw_draw_us[5],
                 (unsigned long long)s_sw_draw_us[6],
                 s_draw_buf1_internal ? "internal" : "psram",
                 !s_draw_buf2 ? "none" : (s_draw_buf2_internal ? "internal" : "psram"),
                 (unsigned)(s_draw_buffers_split ? DRAW_BUF_SPLIT_SIZE : DRAW_BUF_SIZE),
                 s_draw_buffers_split ? 1U : 0U);
        nuwax_display_perf_take(&display_perf);
        ESP_LOGI(TAG, "panel complete frames=%u avg_us=%u max_us=%u shared_dma=%d",
                 (unsigned)display_perf.frames,
                 (unsigned)(display_perf.frames ? display_perf.frame_us / display_perf.frames : 0U),
                 (unsigned)display_perf.frame_max_us, s_driver_draw_buffer != NULL);
        ESP_LOGI(TAG,
                 "display pipeline jobs=%u queue_us=%llu copy_us=%llu swap_us=%llu tx_wait_us=%llu bounce_bytes=%llu direct_bytes=%llu chunks=%u failures=%u",
                 (unsigned)display_perf.jobs,
                 (unsigned long long)display_perf.queue_us,
                 (unsigned long long)display_perf.copy_us,
                 (unsigned long long)display_perf.swap_us,
                 (unsigned long long)display_perf.tx_wait_us,
                 (unsigned long long)display_perf.bounce_bytes,
                 (unsigned long long)display_perf.direct_bytes,
                 (unsigned)display_perf.chunks,
                 (unsigned)display_perf.failures);
        perf_reset_window(now_us);
    }
}

static void record_flush_stats(uint32_t pixels, uint32_t elapsed_us) {
    s_flush_count++;
    s_flush_pixels += pixels;
    s_flush_total_us += elapsed_us;
    if (elapsed_us > s_flush_max_us) {
        s_flush_max_us = elapsed_us;
    }
}
#endif

/* --- Display flush pipeline --- */
static esp_err_t lvgl_flush_write_job(const lvgl_flush_job_t *job) {
#if CONFIG_NUWAX_UI_PERF_LOG_ENABLE
    const nuwax_display_perf_t perf = {
        .queue_us = esp_timer_get_time() - job->queued_us,
        .jobs = 1,
    };
    nuwax_display_perf_record(&perf);
#endif
    const uint16_t *src = job->pixels;
    if (job->clip_x1 == job->x1 && job->clip_y1 == job->y1 &&
        job->clip_w == job->src_w && job->clip_h == job->src_h) {
        /* The flush job owns the draw buffer until its completion token is
         * consumed, so the panel byte swap may happen in place and a
         * DMA-capable internal buffer streams straight to the QSPI engine.
         * PSRAM or misaligned buffers fall back to the copy path inside the
         * display driver. */
        return display_write_region_pixels_swapped_inplace(
            (int16_t)job->clip_x1,
            (int16_t)job->clip_y1,
            (int16_t)job->clip_w,
            (int16_t)job->clip_h,
            (uint16_t *)src,
            (size_t)job->clip_w *
                (size_t)job->clip_h);
    }

    const size_t src_ofs =
        (size_t)(job->clip_y1 - job->y1) * (size_t)job->src_w +
        (size_t)(job->clip_x1 - job->x1);
    return display_write_region_pixels_strided(
        (int16_t)job->clip_x1,
        (int16_t)job->clip_y1,
        (int16_t)job->clip_w,
        (int16_t)job->clip_h,
        src + src_ofs,
        (size_t)job->src_w);
}

static void lvgl_flush_finish_job(const lvgl_flush_job_t *job,
                                  esp_err_t err) {
#if CONFIG_NUWAX_UI_PERF_LOG_ENABLE
    if (err == ESP_OK && job->last_in_frame && job->frame_started_us > 0) {
        const uint32_t duration = (uint32_t)(esp_timer_get_time() - job->frame_started_us);
        const nuwax_display_perf_t complete = {
            .frames = 1U, .frame_us = duration, .frame_max_us = duration,
        };
        nuwax_display_perf_record(&complete);
    }
#endif
    const uint32_t pixels =
        (uint32_t)job->clip_w * (uint32_t)job->clip_h;
    const uint32_t elapsed_us =
        (uint32_t)(esp_timer_get_time() - job->queued_us);
    if (err != ESP_OK) {
        s_flush_errors++;
        ESP_LOGW(TAG,
                 "display flush failed area=%ld,%ld %ldx%ld clipped=%ld,%ld %ldx%ld err=%s errors=%u",
                 (long)job->x1,
                 (long)job->y1,
                 (long)job->src_w,
                 (long)job->src_h,
                 (long)job->clip_x1,
                 (long)job->clip_y1,
                 (long)job->clip_w,
                 (long)job->clip_h,
                 esp_err_to_name(err),
                 (unsigned)s_flush_errors);
        report_ui_warning("flush_failed", err);
    }
#if CONFIG_NUWAX_UI_PERF_LOG_ENABLE
    record_flush_stats(pixels, elapsed_us);
#else
    (void)pixels;
    (void)elapsed_us;
#endif
}

#if CONFIG_NUWAX_UI_ASYNC_FLUSH_ENABLE
static void lvgl_flush_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG,
             "display flush task started core=%d priority=%d stack=internal",
             xPortGetCoreID(),
             FLUSH_TASK_PRIORITY);
    lvgl_flush_job_t job;
    while (true) {
        if (xQueueReceive(s_flush_queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (job.stop) {
            atomic_store_explicit(&s_flush_task_exited, true,
                                  memory_order_release);
            vTaskSuspend(NULL);
            continue;
        }
        const esp_err_t err = lvgl_flush_write_job(&job);
        lvgl_flush_finish_job(&job, err);

        /* With a flush_wait_cb installed, LVGL owns the `flushing` flag and
         * clears it after consuming exactly one completion token. Calling
         * lv_display_flush_ready() here as well would leave a stale token that
         * could release the next draw buffer before its transfer completes. */
        if (s_flush_done) {
            xSemaphoreGive(s_flush_done);
        }
        xSemaphoreGive(s_flush_slots);
    }
}

static bool lvgl_async_flush_init(void) {
    atomic_store_explicit(&s_flush_task_exited, false,
                          memory_order_release);
    s_flush_queue = xQueueCreate(FLUSH_QUEUE_DEPTH,
                                 sizeof(lvgl_flush_job_t));
    s_flush_slots = xSemaphoreCreateCounting(FLUSH_PIPELINE_CAPACITY,
                                             FLUSH_PIPELINE_CAPACITY);
    s_flush_done = xSemaphoreCreateCounting(FLUSH_PIPELINE_CAPACITY, 0);
    if (!s_flush_queue || !s_flush_slots || !s_flush_done) {
        ESP_LOGW(TAG,
                 "async flush synchronization allocation failed; using synchronous flush");
        report_ui_warning("async_sync_alloc", ESP_ERR_NO_MEM);
        if (s_flush_queue) vQueueDelete(s_flush_queue);
        if (s_flush_slots) vSemaphoreDelete(s_flush_slots);
        if (s_flush_done) vSemaphoreDelete(s_flush_done);
        s_flush_queue = NULL;
        s_flush_slots = NULL;
        s_flush_done = NULL;
        return false;
    }

    s_flush_task_tcb = heap_caps_calloc(
        1, sizeof(*s_flush_task_tcb),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_flush_task_stack = heap_caps_malloc(
        CONFIG_NUWAX_UI_FLUSH_TASK_STACK_SIZE,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_flush_task_tcb || !s_flush_task_stack) {
        ESP_LOGW(TAG,
                 "async flush task allocation failed; using synchronous flush");
        report_ui_warning("async_task_alloc", ESP_ERR_NO_MEM);
        heap_caps_free(s_flush_task_tcb);
        heap_caps_free(s_flush_task_stack);
        s_flush_task_tcb = NULL;
        s_flush_task_stack = NULL;
        vQueueDelete(s_flush_queue);
        vSemaphoreDelete(s_flush_slots);
        vSemaphoreDelete(s_flush_done);
        s_flush_queue = NULL;
        s_flush_slots = NULL;
        s_flush_done = NULL;
        return false;
    }

    s_flush_task = xTaskCreateStaticPinnedToCore(
        lvgl_flush_task,
        "lvgl_flush",
        CONFIG_NUWAX_UI_FLUSH_TASK_STACK_SIZE,
        NULL,
        FLUSH_TASK_PRIORITY,
        s_flush_task_stack,
        s_flush_task_tcb,
        LVGL_TASK_CORE);
    if (!s_flush_task) {
        ESP_LOGW(TAG,
                 "async flush task creation failed; using synchronous flush");
        report_ui_warning("async_task_create", ESP_ERR_INVALID_STATE);
        heap_caps_free(s_flush_task_tcb);
        heap_caps_free(s_flush_task_stack);
        s_flush_task_tcb = NULL;
        s_flush_task_stack = NULL;
        vQueueDelete(s_flush_queue);
        vSemaphoreDelete(s_flush_slots);
        vSemaphoreDelete(s_flush_done);
        s_flush_queue = NULL;
        s_flush_slots = NULL;
        s_flush_done = NULL;
        return false;
    }
    return true;
}
#else
static bool lvgl_async_flush_init(void) {
    return false;
}
#endif

static bool lvgl_async_flush_deinit(void) {
#if CONFIG_NUWAX_UI_ASYNC_FLUSH_ENABLE
    if (s_flush_task) {
        const lvgl_flush_job_t stop_job = {.stop = true};
        if (xQueueSend(s_flush_queue, &stop_job,
                       delay_ticks_min1(FLUSH_TASK_STOP_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGE(TAG,
                     "display flush task stop request was not accepted");
            return false;
        }
        const int64_t deadline_us = esp_timer_get_time() +
            (int64_t)FLUSH_TASK_STOP_TIMEOUT_MS * 1000LL;
        bool stopped = false;
        while (esp_timer_get_time() < deadline_us) {
            const bool exited = atomic_load_explicit(
                &s_flush_task_exited, memory_order_acquire);
            const eTaskState task_state = eTaskGetState(s_flush_task);
            if (exited && task_state == eSuspended) {
                stopped = true;
                break;
            }
            vTaskDelay(delay_ticks_min1(5));
        }
        if (!stopped) {
            ESP_LOGE(TAG,
                     "display flush task did not quiesce timeout_ms=%u state=%d exited=%d",
                     (unsigned)FLUSH_TASK_STOP_TIMEOUT_MS,
                     (int)eTaskGetState(s_flush_task),
                     atomic_load_explicit(&s_flush_task_exited,
                                          memory_order_acquire) ? 1 : 0);
            return false;
        }
        vTaskDelete(s_flush_task);
        s_flush_task = NULL;
    }
    if (s_flush_queue) {
        vQueueDelete(s_flush_queue);
        s_flush_queue = NULL;
    }
    if (s_flush_slots) {
        vSemaphoreDelete(s_flush_slots);
        s_flush_slots = NULL;
    }
    if (s_flush_done) {
        vSemaphoreDelete(s_flush_done);
        s_flush_done = NULL;
    }
    heap_caps_free(s_flush_task_tcb);
    heap_caps_free(s_flush_task_stack);
    s_flush_task_tcb = NULL;
    s_flush_task_stack = NULL;
#endif
    s_async_flush_active = false;
    return true;
}

static void lvgl_flush_wait_cb(lv_display_t *disp) {
    (void)disp;
    if (!s_async_flush_active || !s_flush_done) {
        return;
    }
#if CONFIG_NUWAX_UI_PERF_LOG_ENABLE
    const int64_t started = esp_timer_get_time();
#endif
    const BaseType_t done = xSemaphoreTake(s_flush_done, delay_ticks_min1(FLUSH_WAIT_TIMEOUT_MS));
#if CONFIG_NUWAX_UI_PERF_LOG_ENABLE
    const uint32_t waited = (uint32_t)(esp_timer_get_time() - started);
    s_owner_wait_us += waited;
    if (waited > s_owner_wait_max_us) s_owner_wait_max_us = waited;
#endif
    if (done == pdTRUE) {
        return;
    }

    s_flush_wait_timeouts++;
    ESP_LOGE(TAG,
             "display flush completion timeout timeout_ms=%u count=%u; controlled restart before LVGL busy-wait",
             (unsigned)FLUSH_WAIT_TIMEOUT_MS,
             (unsigned)s_flush_wait_timeouts);
    nuwax_diagnostics_log_heap_label(TAG, "lvgl_flush_wait_timeout");
    vTaskDelay(delay_ticks_min1(20));
    esp_restart();
    vTaskDelay(portMAX_DELAY);
}

static bool lvgl_flush_pipeline_take(uint32_t timeout_ms) {
    if (!s_async_flush_active) {
        return true;
    }
    const TickType_t ticks = timeout_ms == 0 ? 0 : delay_ticks_min1(timeout_ms);
    unsigned acquired = 0;
    for (; acquired < FLUSH_PIPELINE_CAPACITY; acquired++) {
        if (xSemaphoreTake(s_flush_slots, ticks) != pdTRUE) {
            while (acquired > 0) {
                xSemaphoreGive(s_flush_slots);
                acquired--;
            }
            return false;
        }
    }
    return true;
}

static void lvgl_flush_pipeline_give(void) {
    if (s_async_flush_active) {
        for (unsigned i = 0; i < FLUSH_PIPELINE_CAPACITY; i++) {
            xSemaphoreGive(s_flush_slots);
        }
    }
}

static void lvgl_maybe_rebalance_internal_buffers(int64_t now_us) {
    /* A temporary DMA loan is rechecked after 10 ms, not at the ordinary
     * 500 ms health cadence. New loans never extend a pending deadline. */
    const int64_t health_interval_us = s_double_buffer_pressure.deadline_us
        ? 10000LL : (int64_t)DOUBLE_BUFFER_HEALTH_INTERVAL_MS * 1000LL;
    if (s_double_buffer_last_health_us != 0 &&
        now_us - s_double_buffer_last_health_us <
            health_interval_us) {
        return;
    }
    s_double_buffer_last_health_us = now_us;
    size_t internal_free = heap_caps_get_free_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (s_draw_buf1_internal && s_draw_buf2 && s_draw_buf2_internal && !s_draw_buffers_split) {
        s_double_buffer_restore_candidate_us = 0;
        if (!nuwax_ui_memory_release_due(&s_double_buffer_pressure, now_us,
                internal_free, LVGL_INTERNAL_DRAW_BUFFER_MIN_FREE,
                nuwax_replay_dma_loan_bytes((uint32_t)now_us)) ||
            !lvgl_flush_pipeline_take(0)) {
            return;
        }

        uint8_t *released = s_draw_buf2;
        if (s_draw_buf2 == s_driver_draw_buffer) {
            released = s_draw_buf1;
            s_draw_buf1 = s_draw_buf2;
        }
        const size_t internal_largest_before = heap_caps_get_largest_free_block(
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        s_draw_buffers_split = s_draw_buf1 == s_driver_draw_buffer && DRAW_BUF_SPLIT_SIZE != 0U;
        s_draw_buf2 = s_draw_buffers_split ? s_draw_buf1 + DRAW_BUF_SPLIT_SIZE : NULL;
        s_draw_buf2_internal = s_draw_buffers_split;
        lv_display_set_buffers(s_display, s_draw_buf1, s_draw_buf2,
                               s_draw_buffers_split ? DRAW_BUF_SPLIT_SIZE : DRAW_BUF_SIZE,
                               LV_DISPLAY_RENDER_MODE_PARTIAL);
        release_draw_buffer(released);
        s_double_buffer_released_us = now_us;
        s_double_buffer_pressure.deadline_us = 0;
        lvgl_flush_pipeline_give();
        ESP_LOGW(TAG,
                 "LVGL second internal draw buffer released for system headroom free_before=%u free_after=%u threshold=%u split_lease=%u bytes_each=%u",
                 (unsigned)internal_free,
                 (unsigned)heap_caps_get_free_size(
                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     (unsigned)LVGL_INTERNAL_DRAW_BUFFER_MIN_FREE,
                     s_draw_buffers_split ? 1U : 0U,
                     (unsigned)(s_draw_buffers_split ? DRAW_BUF_SPLIT_SIZE : DRAW_BUF_SIZE));
        report_ui_warning_detail("second_buffer_released",
                                 ESP_ERR_NO_MEM,
                                 internal_free,
                                 internal_largest_before,
                                 LVGL_INTERNAL_DRAW_BUFFER_MIN_FREE,
                                 DRAW_BUF_SIZE);
        return;
    }

    s_double_buffer_pressure.deadline_us = 0;
    if (!s_runtime_internal_preferred ||
        (s_draw_buf1_internal && !DRAW_BUF_DOUBLE_BUFFER_ENABLE)) {
        s_double_buffer_restore_candidate_us = 0;
        s_internal_restore_incident_active = false;
        return;
    }
    /* Recover a pre-existing single borrowed buffer even when the heap is
     * fragmented. Both halves remain internal/DMA-capable. Repartition only
     * after every queued flush has completed; no allocation or cooldown needed. */
    if (DRAW_BUF_DOUBLE_BUFFER_ENABLE && DRAW_BUF_SPLIT_SIZE != 0U &&
        s_draw_buf1_internal && !s_draw_buf2 && s_draw_buf1 == s_driver_draw_buffer &&
        lvgl_flush_pipeline_take(0)) {
        s_draw_buf2 = s_draw_buf1 + DRAW_BUF_SPLIT_SIZE;
        s_draw_buf2_internal = s_draw_buffers_split = true;
        lv_display_set_buffers(s_display, s_draw_buf1, s_draw_buf2,
                               DRAW_BUF_SPLIT_SIZE, LV_DISPLAY_RENDER_MODE_PARTIAL);
        lvgl_flush_pipeline_give();
        ESP_LOGI(TAG, "LVGL compact double buffer restored split_lease=1 bytes_each=%u extra_heap=0",
                 (unsigned)DRAW_BUF_SPLIT_SIZE);
        return;
    }
    if (s_draw_buf1_internal && !s_draw_buf2 && !s_driver_draw_buffer &&
        DRAW_BUF_DOUBLE_BUFFER_ENABLE && lvgl_flush_pipeline_take(0)) {
        uint8_t *loan = display_acquire_draw_buffer(DRAW_BUF_SIZE);
        if (loan) {
            s_driver_draw_buffer = loan;
            s_draw_buf2 = loan;
            s_draw_buf2_internal = true;
            lv_display_set_buffers(s_display, s_draw_buf1, loan, DRAW_BUF_SIZE,
                                   LV_DISPLAY_RENDER_MODE_PARTIAL);
            ESP_LOGI(TAG, "LVGL shared DMA buffer restored without heap allocation");
        }
        lvgl_flush_pipeline_give();
        if (loan) return;
    }
    if (s_double_buffer_released_us != 0 &&
        now_us - s_double_buffer_released_us <
            (int64_t)DOUBLE_BUFFER_RESTORE_COOLDOWN_MS * 1000LL) {
        s_double_buffer_restore_candidate_us = 0;
        return;
    }

    const size_t internal_largest = heap_caps_get_largest_free_block(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t required_free = DRAW_BUF_SIZE +
        LVGL_INTERNAL_DRAW_BUFFER_MIN_FREE +
        DOUBLE_BUFFER_RESTORE_HEADROOM;
    if (internal_free < required_free || internal_largest < DRAW_BUF_SIZE) {
        s_double_buffer_restore_candidate_us = 0;
        const bool cooldown_elapsed =
            s_buffer_restore_last_report_us == 0 ||
            now_us - s_buffer_restore_last_report_us >=
                (int64_t)DOUBLE_BUFFER_RESTORE_WARNING_COOLDOWN_MS * 1000LL;
        if (!s_draw_buf1_internal && !s_internal_restore_incident_active) {
            s_internal_restore_incident_active = true;
            ESP_LOGW(TAG,
                     "LVGL internal draw-buffer restore pending internal=%u largest=%u need=%u largest_need=%u",
                     (unsigned)internal_free,
                     (unsigned)internal_largest,
                     (unsigned)required_free,
                     (unsigned)DRAW_BUF_SIZE);
            if (cooldown_elapsed) {
                report_ui_warning_detail("buffer_restore_pending",
                                         ESP_ERR_NO_MEM,
                                         internal_free,
                                         internal_largest,
                                         required_free,
                                         DRAW_BUF_SIZE);
                s_buffer_restore_last_report_us = now_us;
            }
        }
        return;
    }
    if (s_double_buffer_restore_candidate_us == 0) {
        s_double_buffer_restore_candidate_us = now_us;
        return;
    }
    if (now_us - s_double_buffer_restore_candidate_us <
        (int64_t)DOUBLE_BUFFER_RESTORE_STABLE_MS * 1000LL) {
        return;
    }

    uint8_t *restored = heap_caps_aligned_alloc(
        16, DRAW_BUF_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!restored) {
        s_double_buffer_restore_candidate_us = 0;
        return;
    }
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) <
        LVGL_INTERNAL_DRAW_BUFFER_MIN_FREE + DOUBLE_BUFFER_RESTORE_HEADROOM) {
        release_draw_buffer(restored);
        s_double_buffer_restore_candidate_us = 0;
        return;
    }
    if (!lvgl_flush_pipeline_take(0)) {
        release_draw_buffer(restored);
        return;
    }

    if (s_draw_buf1_internal) {
        lv_display_set_buffers(s_display, s_draw_buf1, restored, DRAW_BUF_SIZE,
                               LV_DISPLAY_RENDER_MODE_PARTIAL);
        s_draw_buf2 = restored;
        s_draw_buf2_internal = true;
    } else {
        uint8_t *previous1 = s_draw_buf1;
        uint8_t *previous2 = s_draw_buf2;
        lv_display_set_buffers(s_display, restored, NULL, DRAW_BUF_SIZE,
                               LV_DISPLAY_RENDER_MODE_PARTIAL);
        s_draw_buf1 = restored;
        s_draw_buf2 = NULL;
        s_draw_buf1_internal = true;
        s_draw_buf2_internal = false;
        release_draw_buffer_pair(previous1, previous2, s_draw_buffers_split);
    }
    s_draw_buffers_split = false;
    lvgl_flush_pipeline_give();
    s_double_buffer_released_us = 0;
    s_double_buffer_restore_candidate_us = 0;
    s_internal_restore_incident_active = false;
    s_buffer_restore_last_report_us = 0;
    ESP_LOGI(TAG,
             "LVGL internal draw buffer restored mode=%s internal_free=%u largest=%u",
             s_draw_buf2 ? "double" : "single",
             (unsigned)heap_caps_get_free_size(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area,
                          uint8_t *px_map) {
    if (!atomic_load_explicit(&s_panel_output_enabled,
                              memory_order_acquire)) {
        /* Preserve the boot logo until the complete UI is publishable. LVGL
         * still resolves its internal invalidation state. */
        lv_display_flush_ready(disp);
        return;
    }
    const int32_t x1 = area->x1;
    const int32_t y1 = area->y1;
    const int32_t x2 = area->x2;
    const int32_t y2 = area->y2;
    const int32_t src_w = x2 - x1 + 1;
    const int32_t src_h = y2 - y1 + 1;

    if (!px_map || src_w <= 0 || src_h <= 0 || x2 < 0 || y2 < 0 ||
        x1 >= DISP_WIDTH || y1 >= DISP_HEIGHT) {
        lv_display_flush_ready(disp);
        return;
    }

    const int32_t clip_x1 = x1 < 0 ? 0 : x1;
    const int32_t clip_y1 = y1 < 0 ? 0 : y1;
    const int32_t clip_x2 = x2 >= DISP_WIDTH ? DISP_WIDTH - 1 : x2;
    const int32_t clip_y2 = y2 >= DISP_HEIGHT ? DISP_HEIGHT - 1 : y2;

    nuwax_lvgl_port_flush_filter_cb_t flush_filter;
    void *flush_filter_user_data;
    taskENTER_CRITICAL(&s_flush_filter_mux);
    flush_filter = s_flush_filter_cb;
    flush_filter_user_data = s_flush_filter_user_data;
    taskEXIT_CRITICAL(&s_flush_filter_mux);
    if (flush_filter) {
#if CONFIG_NUWAX_UI_PERF_LOG_ENABLE
        const int64_t started = esp_timer_get_time();
#endif
        flush_filter(x1, y1, src_w, src_h, (uint16_t *)px_map,
                     (size_t)src_w, flush_filter_user_data);
#if CONFIG_NUWAX_UI_PERF_LOG_ENABLE
        const uint32_t elapsed = (uint32_t)(esp_timer_get_time() - started);
        s_filter_us += elapsed;
        if (elapsed > s_filter_max_us) s_filter_max_us = elapsed;
#endif
    }

    lvgl_flush_job_t job = {
        .stop = false,
        .display = disp,
        .pixels = (const uint16_t *)px_map,
        .x1 = x1,
        .y1 = y1,
        .src_w = src_w,
        .src_h = src_h,
        .clip_x1 = clip_x1,
        .clip_y1 = clip_y1,
        .clip_w = clip_x2 - clip_x1 + 1,
        .clip_h = clip_y2 - clip_y1 + 1,
        .queued_us = esp_timer_get_time(),
#if CONFIG_NUWAX_UI_PERF_LOG_ENABLE
        .frame_started_us = s_refr_start_us,
        .last_in_frame = lv_display_flush_is_last(disp),
#endif
    };

    if (s_async_flush_active) {
#if CONFIG_NUWAX_UI_PERF_LOG_ENABLE
        const int64_t owner_started = esp_timer_get_time();
#endif
        if (xSemaphoreTake(s_flush_slots,
                           delay_ticks_min1(FLUSH_PIPELINE_WARN_MS)) !=
            pdTRUE) {
            ESP_LOGW(TAG,
                     "display flush ownership delayed; bounded recovery wait started");
            report_ui_warning("flush_ownership_delayed", ESP_ERR_TIMEOUT);
            if (xSemaphoreTake(
                    s_flush_slots,
                    delay_ticks_min1(FLUSH_PIPELINE_FATAL_TIMEOUT_MS)) !=
                pdTRUE) {
                ESP_LOGE(TAG,
                         "display flush pipeline stalled for %u ms; restarting before draw-buffer ownership is lost",
                         (unsigned)(FLUSH_PIPELINE_WARN_MS +
                                    FLUSH_PIPELINE_FATAL_TIMEOUT_MS));
                vTaskDelay(delay_ticks_min1(20));
                esp_restart();
                return;
            }
        }
#if CONFIG_NUWAX_UI_PERF_LOG_ENABLE
        const uint32_t owner_waited = (uint32_t)(esp_timer_get_time() - owner_started);
        s_owner_wait_us += owner_waited;
        if (owner_waited > s_owner_wait_max_us) s_owner_wait_max_us = owner_waited;
#endif
        if (xQueueSend(s_flush_queue, &job, 0) == pdTRUE) {
            return;
        }
        s_flush_queue_fallbacks++;
        ESP_LOGW(TAG,
                 "async flush queue unavailable; synchronous fallback count=%u",
                 (unsigned)s_flush_queue_fallbacks);
        report_ui_warning("flush_queue_fallback", ESP_ERR_INVALID_STATE);
        const esp_err_t err = lvgl_flush_write_job(&job);
        lvgl_flush_finish_job(&job, err);
        xSemaphoreGive(s_flush_slots);
        lv_display_flush_ready(disp);
        return;
    }

    const esp_err_t err = lvgl_flush_write_job(&job);
    lvgl_flush_finish_job(&job, err);
    lv_display_flush_ready(disp);
}

#if CONFIG_NUWAX_UI_LVGL_TOUCH_INDEV_ENABLE
/* --- Touch indev callback --- */
static void lvgl_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;
    nuwax_touch_point_t point;
    if (nuwax_touch_read(&point) == ESP_OK && point.touched) {
        data->point.x = (int32_t)point.x;
        data->point.y = (int32_t)point.y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}
#endif

/* --- LVGL tick timer callback --- */
static void lvgl_tick_cb(void *arg) {
    (void)arg;
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

static void lvgl_wake_cb(void *arg) {
    (void)arg;
    atomic_store_explicit(&s_wake_timer_fired, true, memory_order_release);
    TaskHandle_t task = s_lvgl_task;
    if (task) {
        xTaskNotifyGive(task);
    }
}

static void lvgl_wait_until_due(uint32_t delay_ms) {
    if (!s_wake_timer) {
        vTaskDelay(delay_ticks_min1(delay_ms));
        return;
    }

    (void)esp_timer_stop(s_wake_timer);
    /* Tokens coalesce. A completion/state change DURING the handler is new
     * work even if its notify is already pending here. Compare the persistent
     * generation after draining; a later producer's notify remains pending. */
    (void)ulTaskNotifyTake(pdTRUE, 0);
    if (atomic_load_explicit(&s_external_wake_generation, memory_order_acquire) !=
        s_external_wake_serviced) {
        if (++s_external_wake_streak >= LVGL_EXTERNAL_WAKE_STREAK_LIMIT) {
            s_external_wake_streak = 0U;
            vTaskDelay(1);
        }
        return;
    }
    atomic_store_explicit(&s_wake_timer_fired, false, memory_order_release);
    if (esp_timer_start_once(s_wake_timer,
                             (uint64_t)delay_ms * 1000ULL) != ESP_OK) {
        vTaskDelay(delay_ticks_min1(delay_ms));
        return;
    }
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    const bool timer_fired = atomic_exchange_explicit(
        &s_wake_timer_fired, false, memory_order_acq_rel);
    if (timer_fired) {
        s_external_wake_streak = 0U;
        return;
    }

    /* State changes may legitimately wake LVGL before its frame deadline.
     * Bound a pathological notification storm without charging every normal
     * animation iteration one full 10 ms RTOS tick. */
    (void)esp_timer_stop(s_wake_timer);
    s_external_wake_streak++;
    if (s_external_wake_streak >= LVGL_EXTERNAL_WAKE_STREAK_LIMIT) {
        s_external_wake_streak = 0U;
        vTaskDelay(1);
    }
}

static void lvgl_block_between_handlers(uint32_t next_ms) {
    /* The one-shot timer blocks the owner task for the real LVGL deadline and
     * therefore already gives IDLE0 and lower-priority work a scheduling
     * window. At 100 Hz, an unconditional vTaskDelay(1) costs 10 ms per frame
     * and makes a 40 FPS transition impossible once rendering exceeds 15 ms. */
    lvgl_wait_until_due(next_ms);
}

static bool lvgl_task_stack_guard_intact(void) {
    if (!s_lvgl_task_stack_allocation || !s_lvgl_task_stack) return false;
    for (size_t offset = 0; offset < LVGL_TASK_STACK_GUARD_BYTES; ++offset) {
        if (s_lvgl_task_stack_allocation[offset] !=
            LVGL_TASK_STACK_GUARD_PATTERN) {
            return false;
        }
    }
    return true;
}

static void lvgl_check_task_stack(int64_t now_us) {
    if (s_lvgl_stack_last_check_us != 0 &&
        now_us - s_lvgl_stack_last_check_us <
            LVGL_TASK_STACK_CHECK_INTERVAL_US) {
        return;
    }
    s_lvgl_stack_last_check_us = now_us;
    const UBaseType_t reserve_bytes = uxTaskGetStackHighWaterMark(NULL);
    if (!lvgl_task_stack_guard_intact()) {
        ESP_LOGE(TAG,
                 "LVGL task stack guard corrupted reserve=%u stack_bytes=%u; restarting before scheduler metadata is damaged",
                 (unsigned)reserve_bytes,
                 (unsigned)LVGL_TASK_STACK_SIZE);
        esp_restart();
        return;
    }
    if (reserve_bytes < LVGL_TASK_STACK_WARN_BYTES) {
        ESP_LOGW(TAG,
                 "LVGL task stack reserve low reserve=%u warn=%u stack_bytes=%u",
                 (unsigned)reserve_bytes,
                 (unsigned)LVGL_TASK_STACK_WARN_BYTES,
                 (unsigned)LVGL_TASK_STACK_SIZE);
        report_ui_warning("lvgl_stack_low", ESP_ERR_NO_MEM);
    }
}

/* --- LVGL task --- */
static void lvgl_task(void *arg) {
    (void)arg;
    s_lvgl_task = xTaskGetCurrentTaskHandle();
    ESP_LOGI(TAG, "LVGL task started on core %d", xPortGetCoreID());
    while (1) {
        if (xSemaphoreTake(s_lvgl_mutex, pdMS_TO_TICKS(50))) {
            s_external_wake_serviced = atomic_load_explicit(
                &s_external_wake_generation, memory_order_acquire);
#if CONFIG_NUWAX_UI_PERF_LOG_ENABLE
            const int64_t handler_start_us = esp_timer_get_time();
#endif
            nuwax_lvgl_port_pre_handler_cb_t pre_handler;
            void *pre_handler_user_data;
            taskENTER_CRITICAL(&s_pre_handler_mux);
            pre_handler = s_pre_handler_cb;
            pre_handler_user_data = s_pre_handler_user_data;
            taskEXIT_CRITICAL(&s_pre_handler_mux);
            if (pre_handler) {
                pre_handler(pre_handler_user_data);
            }
            uint32_t next_ms = lv_timer_handler();
#if CONFIG_NUWAX_UI_PERF_LOG_ENABLE
            record_handler_stats((uint32_t)(esp_timer_get_time() - handler_start_us));
#endif
            const int64_t now_us = esp_timer_get_time();
            lvgl_maybe_rebalance_internal_buffers(now_us);
            if (s_display_last_keepalive_us == 0 ||
                now_us - s_display_last_keepalive_us >= DISPLAY_KEEPALIVE_MS * 1000LL) {
                s_display_last_keepalive_us = now_us;
                if (lvgl_flush_pipeline_take(
                        FLUSH_PIPELINE_CONTROL_TIMEOUT_MS)) {
                    esp_err_t err = display_keepalive();
                    lvgl_flush_pipeline_give();
                    if (err != ESP_OK) {
                        ESP_LOGW(TAG, "display keepalive failed: %s",
                                 esp_err_to_name(err));
                        report_ui_warning("display_keepalive_failed", err);
                    }
                } else {
                    ESP_LOGW(TAG,
                             "display keepalive skipped: flush pipeline busy");
                    report_ui_warning("display_keepalive_skipped", ESP_ERR_TIMEOUT);
                }
                lv_obj_t *screen = lv_screen_active();
                if (screen) {
                    lv_area_t area = {
                        .x1 = 0, .y1 = 0,
                        .x2 = DISPLAY_KEEPALIVE_REFRESH_PX - 1,
                        .y2 = DISPLAY_KEEPALIVE_REFRESH_PX - 1,
                    };
                    lv_obj_invalidate_area(screen, &area);
                }
            }
            /* Periodic maintenance stays on the LVGL owner and uses current
             * pixels. Touch/animations defer it; normal redraw and recovery
             * paths retain their existing full-screen invalidations. */
            uint16_t first_row, last_row;
            lv_obj_t *maintenance_screen = lv_screen_active();
            if (nuwax_display_maintenance_next(&s_display_maintenance,
                    (uintptr_t)maintenance_screen, now_us,
                    nuwax_lvgl_port_background_work_deferred() || lv_anim_count_running() != 0U,
                    DISP_HEIGHT, &first_row, &last_row)) {
                const lv_area_t stripe = {.x1=0, .y1=first_row,
                    .x2=DISP_WIDTH-1, .y2=last_row};
                lv_obj_invalidate_area(maintenance_screen, &stripe);
            }
            lvgl_check_task_stack(now_us);
            xSemaphoreGive(s_lvgl_mutex);
            if (next_ms < LVGL_MIN_SLEEP_MS) next_ms = LVGL_MIN_SLEEP_MS;
            if (next_ms > LVGL_MAX_SLEEP_MS) next_ms = LVGL_MAX_SLEEP_MS;
            lvgl_block_between_handlers(next_ms);
        } else {
            vTaskDelay(delay_ticks_min1(10));
        }
    }
}

/* --- Public API --- */
esp_err_t nuwax_lvgl_port_init(void) {
    atomic_store_explicit(&s_panel_output_enabled, false,
                          memory_order_release);
    taskENTER_CRITICAL(&s_init_mux);
    if (s_init_state == LVGL_PORT_READY) {
        taskEXIT_CRITICAL(&s_init_mux);
        ESP_LOGW(TAG, "LVGL port already initialized");
        return ESP_OK;
    }
    if (s_init_state == LVGL_PORT_INITIALIZING) {
        taskEXIT_CRITICAL(&s_init_mux);
        ESP_LOGE(TAG, "LVGL port initialization already in progress");
        return ESP_ERR_INVALID_STATE;
    }
    s_init_state = LVGL_PORT_INITIALIZING;
    taskEXIT_CRITICAL(&s_init_mux);

    esp_err_t err = ESP_OK;
    bool tick_started = false;
    StaticTask_t *task_tcb = NULL;
    uint8_t *task_stack_allocation = NULL;
    StackType_t *task_stack = NULL;

    ESP_LOGI(TAG,
             "Initializing LVGL port: %dx%d buf=%d lines double_preferred=%d async_preferred=%d task_prio=%d sleep=%d-%dms",
             DISP_WIDTH,
             DISP_HEIGHT,
             DRAW_BUF_LINES,
             DRAW_BUF_DOUBLE_BUFFER_ENABLE ? 1 : 0,
             CONFIG_NUWAX_UI_ASYNC_FLUSH_ENABLE ? 1 : 0,
             LVGL_TASK_PRIORITY,
             LVGL_MIN_SLEEP_MS,
             LVGL_MAX_SLEEP_MS);

    /* Mutex */
    s_lvgl_mutex = xSemaphoreCreateMutex();
    if (!s_lvgl_mutex) {
        ESP_LOGE(TAG, "Failed to create LVGL mutex");
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    /* LVGL has no global deinit API in the pinned 9.5 tree. Initialize its
     * core exactly once; failed port attempts delete their display/indev and
     * can safely retry without rebuilding the global registries. */
    static bool lvgl_core_initialized;
    if (!lvgl_core_initialized) {
        lv_init();
        lvgl_core_initialized = true;
    }

    /* Keep LVGL draw buffers in internal RAM when possible. LVGL renders into
     * these buffers on the CPU, so PSRAM buffers make animation-heavy UI feel
     * much slower even when the QSPI flush itself is fast. */
    const size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const bool prefer_internal = !s_provisioning_memory_profile &&
                                 internal_free >=
                                     (DRAW_BUF_SIZE +
                                      LVGL_INTERNAL_SINGLE_BUFFER_MIN_FREE) &&
                                 internal_largest >= DRAW_BUF_SIZE;
    uint8_t *buf1 = NULL;
    uint8_t *buf2 = NULL;
    bool draw_buf1_internal = false;
    bool draw_buf2_internal = false;
    if (prefer_internal) {
        buf1 = heap_caps_aligned_alloc(16, DRAW_BUF_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (buf1) {
            draw_buf1_internal = true;
            if (DRAW_BUF_DOUBLE_BUFFER_ENABLE) {
                buf2 = display_acquire_draw_buffer(DRAW_BUF_SIZE);
                if (buf2) s_driver_draw_buffer = buf2;
                if (!buf2) {
                    const size_t after_first_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                    const size_t after_first_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                    if (after_first_free >= DRAW_BUF_SIZE + LVGL_INTERNAL_DRAW_BUFFER_MIN_FREE &&
                        after_first_largest >= DRAW_BUF_SIZE)
                        buf2 = heap_caps_aligned_alloc(16, DRAW_BUF_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                }
                draw_buf2_internal = buf2 != NULL;
            }
        } else {
            if (buf1) release_draw_buffer(buf1);
            buf1 = NULL;
        }
    }
    if (!buf1) {
        buf1 = heap_caps_aligned_alloc(16, DRAW_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (DRAW_BUF_DOUBLE_BUFFER_ENABLE) {
            buf2 = heap_caps_aligned_alloc(
                16, DRAW_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
    }
    if (!buf1) {
        if (buf2) {
            release_draw_buffer(buf2);
        }
        ESP_LOGE(TAG, "Failed to allocate PSRAM draw buffer (%u bytes)", (unsigned)DRAW_BUF_SIZE);
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    if (!buf2) {
        ESP_LOGI(TAG, "Using single LVGL draw buffer to keep animation rendering in internal RAM");
        buf2 = NULL;
    }

    ESP_LOGI(TAG, "LVGL shared DMA buffer lease=%d bytes=%u", s_driver_draw_buffer != NULL, s_driver_draw_buffer ? (unsigned)DRAW_BUF_SIZE : 0U);
    s_draw_buf1 = buf1;
    s_draw_buf2 = buf2;
    s_draw_buffers_split = false;
    s_draw_buf1_internal = draw_buf1_internal;
    s_draw_buf2_internal = draw_buf2_internal;
    s_runtime_internal_preferred = !s_provisioning_memory_profile;
    s_double_buffer_restore_candidate_us = 0;
    s_double_buffer_released_us = 0;
    s_internal_restore_incident_active = false;

    /* Display */
    s_display = lv_display_create(DISP_WIDTH, DISP_HEIGHT);
    if (!s_display) {
        ESP_LOGE(TAG, "Failed to create LVGL display");
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_display, lvgl_flush_cb);
    lv_display_set_buffers(s_display, buf1, buf2, DRAW_BUF_SIZE, LV_DISPLAY_RENDER_MODE_PARTIAL);
#if CONFIG_NUWAX_UI_PERF_LOG_ENABLE
    lv_display_add_event_cb(s_display, perf_display_event_cb, LV_EVENT_REFR_START, NULL);
    lv_display_add_event_cb(s_display, perf_display_event_cb, LV_EVENT_REFR_READY, NULL);
    lv_display_add_event_cb(s_display, perf_display_event_cb, LV_EVENT_RENDER_START, NULL);
    lv_display_add_event_cb(s_display, perf_display_event_cb, LV_EVENT_RENDER_READY, NULL);
#endif
    ESP_LOGI(TAG,
             "Draw buffers: buf1=%s buf2=%s %s async_preferred=%d (%u bytes each) internal_free=%u internal_largest=%u",
             draw_buf1_internal ? "internal" : "psram",
             !buf2 ? "none" : (draw_buf2_internal ? "internal" : "psram"),
             buf2 ? "double-buffered" : "single-buffered",
             CONFIG_NUWAX_UI_ASYNC_FLUSH_ENABLE ? 1 : 0,
             (unsigned)DRAW_BUF_SIZE,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    /* Touch indev */
#if CONFIG_NUWAX_UI_LVGL_TOUCH_INDEV_ENABLE
    s_indev = lv_indev_create();
    if (!s_indev) {
        ESP_LOGE(TAG, "Failed to create LVGL touch input device");
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, lvgl_touch_read_cb);
#else
    ESP_LOGI(TAG, "LVGL pointer indev disabled; gestures are routed by app_shell");
#endif

    s_async_flush_active = lvgl_async_flush_init();
    /* The asynchronous pipeline is created after the LVGL display object.
     * Register the wait callback only after init succeeds; registering at
     * display creation time silently skipped the callback because the flag
     * was still false. */
    if (s_async_flush_active) {
        lv_display_set_flush_wait_cb(s_display, lvgl_flush_wait_cb);
    }

    /* Tick timer */
    const esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_cb,
        .name = "lvgl_tick",
    };
    err = esp_timer_create(&tick_args, &s_tick_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tick timer create failed: %s", esp_err_to_name(err));
        goto fail;
    }
    err = esp_timer_start_periodic(s_tick_timer, LVGL_TICK_PERIOD_MS * 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tick timer start failed: %s", esp_err_to_name(err));
        goto fail;
    }
    tick_started = true;

    const esp_timer_create_args_t wake_args = {
        .callback = lvgl_wake_cb,
        .name = "lvgl_wake",
    };
    err = esp_timer_create(&wake_args, &s_wake_timer);
    if (err != ESP_OK) {
        s_wake_timer = NULL;
        ESP_LOGW(TAG,
                 "high-resolution LVGL wake timer unavailable; using tick-rounded delay: %s",
                 esp_err_to_name(err));
    }

    /* The emotion player reads its compressed stream from a raw flash
     * partition on this task. ESP-IDF disables the flash/PSRAM cache around
     * esp_partition_read(), so the active stack must remain in internal DRAM. */
    /* Keep a dedicated underflow guard below the downward-growing Xtensa
     * stack. Historical device telemetry reached only 40 bytes of reserve on
     * the former 10 KiB stack; without separation, the next deep LVGL render
     * could overwrite its adjacent TCB before the FreeRTOS canary ran. */
    task_stack_allocation = heap_caps_aligned_alloc(
        16, LVGL_TASK_STACK_GUARD_BYTES + LVGL_TASK_STACK_SIZE,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (task_stack_allocation) {
        memset(task_stack_allocation, LVGL_TASK_STACK_GUARD_PATTERN,
               LVGL_TASK_STACK_GUARD_BYTES);
        task_stack = (StackType_t *)(task_stack_allocation +
                                     LVGL_TASK_STACK_GUARD_BYTES);
    }
    task_tcb = heap_caps_calloc(1, sizeof(StaticTask_t),
                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const bool stack_cache_safe =
        task_stack_allocation && task_stack &&
        esp_ptr_in_dram(task_stack_allocation) &&
        esp_ptr_in_dram(task_stack) &&
        esp_ptr_in_dram((const uint8_t *)task_stack +
                        LVGL_TASK_STACK_SIZE - 1U);
    if (!task_tcb || !stack_cache_safe) {
        ESP_LOGE(TAG,
                 "Failed to allocate guarded cache-safe LVGL task memory stack_bytes=%u guard_bytes=%u tcb=%p allocation=%p stack=%p internal_free=%u internal_largest=%u",
                 (unsigned)LVGL_TASK_STACK_SIZE,
                 (unsigned)LVGL_TASK_STACK_GUARD_BYTES,
                 task_tcb,
                 task_stack_allocation,
                 task_stack,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    ESP_LOGI(TAG,
             "LVGL task stack=internal cache_safe=1 bytes=%u guard=%u allocation=%p stack=%p tcb=%p internal_free=%u internal_largest=%u",
             (unsigned)LVGL_TASK_STACK_SIZE,
             (unsigned)LVGL_TASK_STACK_GUARD_BYTES,
             task_stack_allocation,
             task_stack,
             task_tcb,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    s_lvgl_task_stack_allocation = task_stack_allocation;
    s_lvgl_task_stack = task_stack;
    s_lvgl_stack_last_check_us = 0;
    s_lvgl_task = xTaskCreateStaticPinnedToCore(
        lvgl_task, "lvgl", LVGL_TASK_STACK_SIZE,
        NULL, LVGL_TASK_PRIORITY, task_stack, task_tcb, LVGL_TASK_CORE);
    if (!s_lvgl_task) {
        s_lvgl_task_stack_allocation = NULL;
        s_lvgl_task_stack = NULL;
        ESP_LOGE(TAG, "Failed to create LVGL task");
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    lvgl_port_set_init_state(LVGL_PORT_READY);
    ESP_LOGI(TAG, "LVGL port initialized successfully style_cache=%d profile=cached-pipeline-v7", LV_OBJ_STYLE_CACHE);
    return ESP_OK;

fail:
    if (task_tcb) {
        heap_caps_free(task_tcb);
    }
    if (task_stack_allocation) {
        heap_caps_free(task_stack_allocation);
    }
    if (!lvgl_port_init_cleanup(tick_started)) {
        ESP_LOGE(TAG,
                 "LVGL initialization rollback could not quiesce display I/O; restarting before dependent resources are released");
        vTaskDelay(delay_ticks_min1(20));
        esp_restart();
        return ESP_ERR_INVALID_STATE;
    }
    lvgl_port_set_init_state(LVGL_PORT_UNINITIALIZED);
    ESP_LOGE(TAG, "LVGL port initialization rolled back: %s",
             esp_err_to_name(err));
    return err;
}

void nuwax_lvgl_port_set_panel_output_enabled(bool enabled)
{
    if (!nuwax_lvgl_port_lock_is_held_by_current_task()) {
        ESP_LOGE(TAG,
                 "panel output gate change rejected without LVGL mutex ownership");
        return;
    }
    atomic_store_explicit(&s_panel_output_enabled, enabled,
                          memory_order_release);
    if (enabled) {
        lv_obj_t *screen = lv_screen_active();
        if (screen) {
            lv_obj_invalidate(screen);
        }
        nuwax_lvgl_port_wake();
    }
    ESP_LOGI(TAG, "panel output gate=%s",
             enabled ? "ui" : "held");
}

void nuwax_lvgl_port_set_provisioning_memory_profile(bool enabled) {
    /* Refuse a live profile migration; lv_display_set_buffers() must not race
     * the LVGL task or an in-flight QSPI flush. */
    if (s_display || s_lvgl_mutex) {
        ESP_LOGW(TAG, "ignoring late LVGL memory profile change provisioning=%d",
                 enabled ? 1 : 0);
        return;
    }
    s_provisioning_memory_profile = enabled;
    ESP_LOGI(TAG, "LVGL memory profile=%s",
             enabled ? "provisioning_psram" : "runtime_internal_preferred");
}

esp_err_t nuwax_lvgl_port_set_runtime_provisioning_memory(bool enabled) {
    if (!s_display || !s_lvgl_mutex || !s_draw_buf1) {
        return ESP_ERR_INVALID_STATE;
    }

    const bool target_internal = !enabled;
    /* Preserve the requested end-state even if the immediate migration cannot
     * allocate memory. A failed internal restore remains eligible for the
     * periodic bounded retry; provisioning mode must not recreate buffers that
     * it deliberately moved out of scarce internal SRAM. */
    s_runtime_internal_preferred = target_internal;
    s_double_buffer_restore_candidate_us = 0;
    s_double_buffer_released_us = 0;
    const bool want_double = DRAW_BUF_DOUBLE_BUFFER_ENABLE != 0;
    if (s_draw_buf1_internal == target_internal &&
        (!want_double ||
         (s_draw_buf2 && s_draw_buf2_internal == target_internal))) {
        return ESP_OK;
    }

    unsigned target_buffer_count = want_double ? 2U : 1U;
    if (target_internal) {
        const size_t internal_free =
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        const size_t internal_largest =
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (internal_free < DRAW_BUF_SIZE +
                                LVGL_INTERNAL_SINGLE_BUFFER_MIN_FREE ||
            internal_largest < DRAW_BUF_SIZE) {
            ESP_LOGW(TAG,
                     "runtime draw buffer restore deferred internal=%u largest=%u need=%u+%u",
                     (unsigned)internal_free,
                     (unsigned)internal_largest,
                     (unsigned)DRAW_BUF_SIZE,
                     (unsigned)LVGL_INTERNAL_DRAW_BUFFER_MIN_FREE);
            report_ui_warning("runtime_buffer_restore_deferred", ESP_ERR_NO_MEM);
            return ESP_ERR_NO_MEM;
        }

    }

    const uint32_t caps = target_internal
                              ? (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
                              : (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *next1 = heap_caps_aligned_alloc(16, DRAW_BUF_SIZE, caps);
    uint8_t *next2 = NULL;
    if (next1 && target_buffer_count == 2U && !target_internal) {
        next2 = heap_caps_aligned_alloc(16, DRAW_BUF_SIZE, caps);
        if (!next2) {
            target_buffer_count = 1U;
            ESP_LOGW(TAG,
                     "runtime draw buffer second allocation failed target=%s; continuing single-buffered",
                     target_internal ? "internal" : "psram");
            report_ui_warning("runtime_second_buffer_alloc", ESP_ERR_NO_MEM);
        }
    }
    if (!next1) {
        if (next1) {
            release_draw_buffer(next1);
        }
        if (next2) {
            release_draw_buffer(next2);
        }
        ESP_LOGE(TAG,
                 "runtime draw buffer migration allocation failed target=%s bytes=%u",
                 target_internal ? "internal" : "psram",
                 (unsigned)DRAW_BUF_SIZE);
        return ESP_ERR_NO_MEM;
    }

    if (!nuwax_lvgl_port_lock(1000)) {
        release_draw_buffer(next1);
        if (next2) {
            release_draw_buffer(next2);
        }
        ESP_LOGE(TAG, "runtime draw buffer migration LVGL lock timeout");
        return ESP_ERR_TIMEOUT;
    }
    if (!lvgl_flush_pipeline_take(FLUSH_PIPELINE_CONTROL_TIMEOUT_MS)) {
        nuwax_lvgl_port_unlock();
        release_draw_buffer(next1);
        if (next2) release_draw_buffer(next2);
        ESP_LOGE(TAG, "runtime draw buffer migration flush pipeline timeout");
        return ESP_ERR_TIMEOUT;
    }

    uint8_t *previous1 = s_draw_buf1;
    uint8_t *previous2 = s_draw_buf2;
    const bool previous_split = s_draw_buffers_split;
    lv_display_set_buffers(s_display, next1, next2, DRAW_BUF_SIZE,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    s_draw_buf1 = next1;
    s_draw_buf2 = next2;
    s_draw_buffers_split = false;
    s_draw_buf1_internal = target_internal;
    s_draw_buf2_internal = next2 && target_internal;
    s_runtime_internal_preferred = target_internal;
    lv_obj_t *screen = lv_screen_active();
    if (screen) {
        lv_obj_invalidate(screen);
    }
    release_draw_buffer_pair(previous1, previous2, previous_split);
    if (target_internal && want_double) {
        uint8_t *loan = display_acquire_draw_buffer(DRAW_BUF_SIZE);
        if (loan) {
            s_driver_draw_buffer = loan;
            s_draw_buf2 = loan;
            s_draw_buf2_internal = true;
            lv_display_set_buffers(s_display, s_draw_buf1, loan, DRAW_BUF_SIZE,
                                   LV_DISPLAY_RENDER_MODE_PARTIAL);
        }
    }
    target_buffer_count = s_draw_buf2 ? 2U : 1U;
    lvgl_flush_pipeline_give();
    nuwax_lvgl_port_unlock();
    ESP_LOGI(TAG,
             "runtime draw buffer migrated target=%s buffers=%u bytes_each=%u internal=%u largest=%u psram=%u",
             target_internal ? "internal" : "psram",
             target_buffer_count,
             (unsigned)DRAW_BUF_SIZE,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    return ESP_OK;
}

void nuwax_lvgl_port_set_pre_handler_cb(nuwax_lvgl_port_pre_handler_cb_t cb, void *user_data) {
    taskENTER_CRITICAL(&s_pre_handler_mux);
    s_pre_handler_cb = cb;
    s_pre_handler_user_data = user_data;
    taskEXIT_CRITICAL(&s_pre_handler_mux);
}

void nuwax_lvgl_port_set_flush_filter(
    nuwax_lvgl_port_flush_filter_cb_t cb, void *user_data) {
    taskENTER_CRITICAL(&s_flush_filter_mux);
    s_flush_filter_cb = cb;
    s_flush_filter_user_data = cb ? user_data : NULL;
    taskEXIT_CRITICAL(&s_flush_filter_mux);
}

void nuwax_lvgl_port_wake(void) {
    atomic_fetch_add_explicit(&s_external_wake_generation, 1U, memory_order_release);
    TaskHandle_t task = s_lvgl_task;
    if (task) {
        xTaskNotifyGive(task);
    }
}

bool nuwax_lvgl_port_lock(uint32_t timeout_ms) {
    if (!s_lvgl_mutex) return false;
    return xSemaphoreTake(s_lvgl_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

bool nuwax_lvgl_port_lock_is_held_by_current_task(void) {
    if (!s_lvgl_mutex) return false;
    return xSemaphoreGetMutexHolder(s_lvgl_mutex) == xTaskGetCurrentTaskHandle();
}

esp_err_t nuwax_lvgl_port_present_rgb565_regions(
    const uint16_t *framebuffer, size_t source_stride_pixels,
    const nuwax_lvgl_port_rgb565_region_t *regions, size_t region_count,
    uint32_t timeout_ms) {
    if (!nuwax_lvgl_port_lock_is_held_by_current_task() || !s_display) {
        return ESP_ERR_INVALID_STATE;
    }
    if (region_count == 0U) return ESP_OK;
    if (!framebuffer || !regions || source_stride_pixels < DISP_WIDTH) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t total_pixels = 0U;
    for (size_t index = 0U; index < region_count; ++index) {
        const nuwax_lvgl_port_rgb565_region_t region = regions[index];
        if (region.x < 0 || region.y < 0 || region.width <= 0 ||
            region.height <= 0 ||
            (int32_t)region.x + region.width > DISP_WIDTH ||
            (int32_t)region.y + region.height > DISP_HEIGHT ||
            (size_t)region.y >
                (SIZE_MAX - (size_t)region.x) / source_stride_pixels) {
            return ESP_ERR_INVALID_ARG;
        }
        const uint32_t pixels =
            (uint32_t)region.width * (uint32_t)region.height;
        if (pixels > UINT32_MAX - total_pixels) {
            return ESP_ERR_INVALID_SIZE;
        }
        total_pixels += pixels;
    }

    if (!lvgl_flush_pipeline_take(timeout_ms)) {
        return ESP_ERR_TIMEOUT;
    }

#if CONFIG_NUWAX_UI_PERF_LOG_ENABLE
    const int64_t started_us = esp_timer_get_time();
#endif
    esp_err_t err = ESP_OK;
    for (size_t index = 0U; index < region_count; ++index) {
        const nuwax_lvgl_port_rgb565_region_t region = regions[index];
        const size_t source_offset =
            (size_t)region.y * source_stride_pixels + (size_t)region.x;
        err = display_write_region_pixels_strided(
            region.x, region.y, region.width, region.height,
            framebuffer + source_offset, source_stride_pixels);
        if (err != ESP_OK) break;
    }
    lvgl_flush_pipeline_give();

    const int64_t completed_us = esp_timer_get_time();
    if (err == ESP_OK) {
        s_display_last_keepalive_us = completed_us;
#if CONFIG_NUWAX_UI_PERF_LOG_ENABLE
        record_flush_stats(total_pixels,
                           (uint32_t)(completed_us - started_us));
#else
        (void)total_pixels;
#endif
    }
    return err;
}

void nuwax_lvgl_port_unlock(void) {
    if (!s_lvgl_mutex) return;
    xSemaphoreGive(s_lvgl_mutex);
}
