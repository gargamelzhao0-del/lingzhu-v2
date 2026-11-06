#include "display.h"
#include "boot_logo.h"

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "board/esp_vocat_v1_0_pins.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Low-rate telemetry only; no allocations and no logging in the SPI path.
 * Times are elapsed wall time (including preemption), not hardware wire time. */
#if CONFIG_NUWAX_UI_PERF_LOG_ENABLE
static portMUX_TYPE s_display_perf_mux = portMUX_INITIALIZER_UNLOCKED;
static nuwax_display_perf_t s_display_perf;

void nuwax_display_perf_record(const nuwax_display_perf_t *sample) {
    taskENTER_CRITICAL(&s_display_perf_mux);
    s_display_perf.queue_us += sample->queue_us;
    s_display_perf.copy_us += sample->copy_us;
    s_display_perf.swap_us += sample->swap_us;
    s_display_perf.tx_wait_us += sample->tx_wait_us;
    s_display_perf.bounce_bytes += sample->bounce_bytes;
    s_display_perf.direct_bytes += sample->direct_bytes;
    s_display_perf.jobs += sample->jobs;
    s_display_perf.chunks += sample->chunks;
    s_display_perf.failures += sample->failures;
    s_display_perf.frames += sample->frames;
    s_display_perf.frame_us += sample->frame_us;
    if (sample->frame_max_us > s_display_perf.frame_max_us)
        s_display_perf.frame_max_us = sample->frame_max_us;
    taskEXIT_CRITICAL(&s_display_perf_mux);
}
#else
void nuwax_display_perf_record(const nuwax_display_perf_t *sample) {
    (void)sample;
}
#endif

void nuwax_display_perf_take(nuwax_display_perf_t *result) {
    if (!result) return;
#if CONFIG_NUWAX_UI_PERF_LOG_ENABLE
    taskENTER_CRITICAL(&s_display_perf_mux);
    *result = s_display_perf;
    memset(&s_display_perf, 0, sizeof(s_display_perf));
    taskEXIT_CRITICAL(&s_display_perf_mux);
#else
    memset(result, 0, sizeof(*result));
#endif
}

#if CONFIG_NUWAX_DISPLAY_ENABLE

static const char *TAG = "nuwax_display";

#if defined(CONFIG_NUWAX_DISPLAY_BL_ACTIVE_LOW)
#define NUWAX_DISPLAY_BL_ACTIVE_LOW_VALUE 1
#else
#define NUWAX_DISPLAY_BL_ACTIVE_LOW_VALUE 0
#endif

#if defined(CONFIG_NUWAX_DISPLAY_RST_ACTIVE_HIGH)
#define NUWAX_DISPLAY_RST_ACTIVE_HIGH_VALUE 1
#else
#define NUWAX_DISPLAY_RST_ACTIVE_HIGH_VALUE 0
#endif

#define ST77916_WIDTH 360
#define ST77916_HEIGHT 360
/* Preserve internal RAM for concurrent NimBLE Security 2, Wi-Fi and OTA TLS. */
#define ST77916_FLUSH_LINES 20
#define ST77916_LINE_BUFFER_PIXELS ST77916_WIDTH
#define ST77916_TX_BUFFER_PIXELS (ST77916_WIDTH * ST77916_FLUSH_LINES)
#define ST77916_SPI_SAFE_TRANSFER_BYTES (30U * 1024U)
#define ST77916_SPI_SAFE_TRANSFER_PIXELS (ST77916_SPI_SAFE_TRANSFER_BYTES / sizeof(uint16_t))
#define ST77916_INTERRUPT_TRANSFER_MIN_BYTES 2048U
#define ST77916_CMD_BUFFER_BYTES 64
#define ST77916_COLOR_BLACK 0x0000
#define ST77916_COLOR_WHITE 0xffff
#define ST77916_RST_DELAY_MS 120
#define ST77916_SLPOUT_DELAY_MS 120
#define ST77916_FLUSH_YIELD_CHUNKS 8

#define ST77916_CASET 0x2a
#define ST77916_RASET 0x2b
#define ST77916_RAMWR 0x2c
#define ST77916_MADCTL 0x36
#define ST77916_SLPOUT 0x11
#define ST77916_DISPON 0x29
#define ST77916_INVON 0x21
#define ST77916_INVOFF 0x20
#define ST77916_TEOFF 0x34
#define ST77916_TEON 0x35
#define ST77916_QSPI_WRITE_CMD 0x02
#define ST77916_QSPI_WRITE_PIXELS_CMD 0x32
#define ST77916_QSPI_WRITE_PIXELS_ADDR 0x003c00

typedef enum {
    DISP_BEGIN_WRITE = 0,
    DISP_WRITE_COMMAND_8,
    DISP_WRITE_COMMAND_16,
    DISP_WRITE_COMMAND_BYTES,
    DISP_WRITE_DATA_8,
    DISP_WRITE_DATA_16,
    DISP_WRITE_BYTES,
    DISP_WRITE_C8_D8,
    DISP_WRITE_C8_D16,
    DISP_WRITE_C8_BYTES,
    DISP_WRITE_C16_D16,
    DISP_END_WRITE,
    DISP_DELAY,
} display_operation_t;

static const uint8_t s_st77916_init_operations[] = {
    DISP_BEGIN_WRITE,
    DISP_WRITE_C8_D8, 0xF0, 0x28,
    DISP_WRITE_C8_D8, 0xF2, 0x28,
    DISP_WRITE_C8_D8, 0x73, 0xF0,
    DISP_WRITE_C8_D8, 0x7C, 0xD1,
    DISP_WRITE_C8_D8, 0x83, 0xE0,
    DISP_WRITE_C8_D8, 0x84, 0x61,
    DISP_WRITE_C8_D8, 0xF2, 0x82,
    DISP_WRITE_C8_D8, 0xF0, 0x00,
    DISP_WRITE_C8_D8, 0xF0, 0x01,
    DISP_WRITE_C8_D8, 0xF1, 0x01,
    DISP_WRITE_C8_D8, 0xB0, 0x5E,
    DISP_WRITE_C8_D8, 0xB1, 0x55,
    DISP_WRITE_C8_D8, 0xB2, 0x24,
    DISP_WRITE_C8_D8, 0xB3, 0x01,
    DISP_WRITE_C8_D8, 0xB4, 0x87,
    DISP_WRITE_C8_D8, 0xB5, 0x44,
    DISP_WRITE_C8_D8, 0xB6, 0x8B,
    DISP_WRITE_C8_D8, 0xB7, 0x40,
    DISP_WRITE_C8_D8, 0xB8, 0x86,
    DISP_WRITE_C8_D8, 0xB9, 0x15,
    DISP_WRITE_C8_D8, 0xBA, 0x00,
    DISP_WRITE_C8_D8, 0xBB, 0x08,
    DISP_WRITE_C8_D8, 0xBC, 0x08,
    DISP_WRITE_C8_D8, 0xBD, 0x00,
    DISP_WRITE_C8_D8, 0xBE, 0x00,
    DISP_WRITE_C8_D8, 0xBF, 0x07,
    DISP_WRITE_C8_D8, 0xC0, 0x80,
    DISP_WRITE_C8_D8, 0xC1, 0x10,
    DISP_WRITE_C8_D8, 0xC2, 0x37,
    DISP_WRITE_C8_D8, 0xC3, 0x80,
    DISP_WRITE_C8_D8, 0xC4, 0x10,
    DISP_WRITE_C8_D8, 0xC5, 0x37,
    DISP_WRITE_C8_D8, 0xC6, 0xA9,
    DISP_WRITE_C8_D8, 0xC7, 0x41,
    DISP_WRITE_C8_D8, 0xC8, 0x01,
    DISP_WRITE_C8_D8, 0xC9, 0xA9,
    DISP_WRITE_C8_D8, 0xCA, 0x41,
    DISP_WRITE_C8_D8, 0xCB, 0x01,
    DISP_WRITE_C8_D8, 0xCC, 0x7F,
    DISP_WRITE_C8_D8, 0xCD, 0x7F,
    DISP_WRITE_C8_D8, 0xCE, 0xFF,
    DISP_WRITE_C8_D8, 0xD0, 0x91,
    DISP_WRITE_C8_D8, 0xD1, 0x68,
    DISP_WRITE_C8_D8, 0xD2, 0x68,
    DISP_WRITE_C8_D16, 0xF5, 0x00, 0xA5,
    DISP_WRITE_C8_D8, 0xDD, 0x40,
    DISP_WRITE_C8_D8, 0xDE, 0x40,
    DISP_WRITE_C8_D8, 0xF1, 0x10,
    DISP_WRITE_C8_D8, 0xF0, 0x00,
    DISP_WRITE_C8_D8, 0xF0, 0x02,
    DISP_WRITE_COMMAND_8, 0xE0,
    DISP_WRITE_BYTES, 14,
    0xF0, 0x10, 0x18, 0x0D, 0x0C, 0x38, 0x3E, 0x44, 0x51, 0x39, 0x15, 0x15, 0x30, 0x34,
    DISP_WRITE_COMMAND_8, 0xE1,
    DISP_WRITE_BYTES, 14,
    0xF0, 0x0F, 0x17, 0x0D, 0x0B, 0x07, 0x3E, 0x33, 0x51, 0x39, 0x15, 0x15, 0x30, 0x34,
    DISP_WRITE_C8_D8, 0xF0, 0x10,
    DISP_WRITE_C8_D8, 0xF3, 0x10,
    DISP_WRITE_C8_D8, 0xE0, 0x08,
    DISP_WRITE_C8_D8, 0xE1, 0x00,
    DISP_WRITE_C8_D8, 0xE2, 0x00,
    DISP_WRITE_C8_D8, 0xE3, 0x00,
    DISP_WRITE_C8_D8, 0xE4, 0xE0,
    DISP_WRITE_C8_D8, 0xE5, 0x06,
    DISP_WRITE_C8_D8, 0xE6, 0x21,
    DISP_WRITE_C8_D8, 0xE7, 0x03,
    DISP_WRITE_C8_D8, 0xE8, 0x05,
    DISP_WRITE_C8_D8, 0xE9, 0x02,
    DISP_WRITE_C8_D8, 0xEA, 0xE9,
    DISP_WRITE_C8_D8, 0xEB, 0x00,
    DISP_WRITE_C8_D8, 0xEC, 0x00,
    DISP_WRITE_C8_D8, 0xED, 0x14,
    DISP_WRITE_C8_D8, 0xEE, 0xFF,
    DISP_WRITE_C8_D8, 0xEF, 0x00,
    DISP_WRITE_C8_D8, 0xF8, 0xFF,
    DISP_WRITE_C8_D8, 0xF9, 0x00,
    DISP_WRITE_C8_D8, 0xFA, 0x00,
    DISP_WRITE_C8_D8, 0xFB, 0x30,
    DISP_WRITE_C8_D8, 0xFC, 0x00,
    DISP_WRITE_C8_D8, 0xFD, 0x00,
    DISP_WRITE_C8_D8, 0xFE, 0x00,
    DISP_WRITE_C8_D8, 0xFF, 0x00,
    DISP_WRITE_C8_D8, 0x60, 0x40,
    DISP_WRITE_C8_D8, 0x61, 0x05,
    DISP_WRITE_C8_D8, 0x62, 0x00,
    DISP_WRITE_C8_D8, 0x63, 0x42,
    DISP_WRITE_C8_D8, 0x64, 0xDA,
    DISP_WRITE_C8_D8, 0x65, 0x00,
    DISP_WRITE_C8_D8, 0x66, 0x00,
    DISP_WRITE_C8_D8, 0x67, 0x00,
    DISP_WRITE_C8_D8, 0x68, 0x00,
    DISP_WRITE_C8_D8, 0x69, 0x00,
    DISP_WRITE_C8_D8, 0x6A, 0x00,
    DISP_WRITE_C8_D8, 0x6B, 0x00,
    DISP_WRITE_C8_D8, 0x70, 0x40,
    DISP_WRITE_C8_D8, 0x71, 0x04,
    DISP_WRITE_C8_D8, 0x72, 0x00,
    DISP_WRITE_C8_D8, 0x73, 0x42,
    DISP_WRITE_C8_D8, 0x74, 0xD9,
    DISP_WRITE_C8_D8, 0x75, 0x00,
    DISP_WRITE_C8_D8, 0x76, 0x00,
    DISP_WRITE_C8_D8, 0x77, 0x00,
    DISP_WRITE_C8_D8, 0x78, 0x00,
    DISP_WRITE_C8_D8, 0x79, 0x00,
    DISP_WRITE_C8_D8, 0x7A, 0x00,
    DISP_WRITE_C8_D8, 0x7B, 0x00,
    DISP_WRITE_C8_D8, 0x80, 0x48,
    DISP_WRITE_C8_D8, 0x81, 0x00,
    DISP_WRITE_C8_D8, 0x82, 0x07,
    DISP_WRITE_C8_D8, 0x83, 0x02,
    DISP_WRITE_C8_D8, 0x84, 0xD7,
    DISP_WRITE_C8_D8, 0x85, 0x04,
    DISP_WRITE_C8_D8, 0x86, 0x00,
    DISP_WRITE_C8_D8, 0x87, 0x00,
    DISP_WRITE_C8_D8, 0x88, 0x48,
    DISP_WRITE_C8_D8, 0x89, 0x00,
    DISP_WRITE_C8_D8, 0x8A, 0x09,
    DISP_WRITE_C8_D8, 0x8B, 0x02,
    DISP_WRITE_C8_D8, 0x8C, 0xD9,
    DISP_WRITE_C8_D8, 0x8D, 0x04,
    DISP_WRITE_C8_D8, 0x8E, 0x00,
    DISP_WRITE_C8_D8, 0x8F, 0x00,
    DISP_WRITE_C8_D8, 0x90, 0x48,
    DISP_WRITE_C8_D8, 0x91, 0x00,
    DISP_WRITE_C8_D8, 0x92, 0x0B,
    DISP_WRITE_C8_D8, 0x93, 0x02,
    DISP_WRITE_C8_D8, 0x94, 0xDB,
    DISP_WRITE_C8_D8, 0x95, 0x04,
    DISP_WRITE_C8_D8, 0x96, 0x00,
    DISP_WRITE_C8_D8, 0x97, 0x00,
    DISP_WRITE_C8_D8, 0x98, 0x48,
    DISP_WRITE_C8_D8, 0x99, 0x00,
    DISP_WRITE_C8_D8, 0x9A, 0x0D,
    DISP_WRITE_C8_D8, 0x9B, 0x02,
    DISP_WRITE_C8_D8, 0x9C, 0xDD,
    DISP_WRITE_C8_D8, 0x9D, 0x04,
    DISP_WRITE_C8_D8, 0x9E, 0x00,
    DISP_WRITE_C8_D8, 0x9F, 0x00,
    DISP_WRITE_C8_D8, 0xA0, 0x48,
    DISP_WRITE_C8_D8, 0xA1, 0x00,
    DISP_WRITE_C8_D8, 0xA2, 0x06,
    DISP_WRITE_C8_D8, 0xA3, 0x02,
    DISP_WRITE_C8_D8, 0xA4, 0xD6,
    DISP_WRITE_C8_D8, 0xA5, 0x04,
    DISP_WRITE_C8_D8, 0xA6, 0x00,
    DISP_WRITE_C8_D8, 0xA7, 0x00,
    DISP_WRITE_C8_D8, 0xA8, 0x48,
    DISP_WRITE_C8_D8, 0xA9, 0x00,
    DISP_WRITE_C8_D8, 0xAA, 0x08,
    DISP_WRITE_C8_D8, 0xAB, 0x02,
    DISP_WRITE_C8_D8, 0xAC, 0xD8,
    DISP_WRITE_C8_D8, 0xAD, 0x04,
    DISP_WRITE_C8_D8, 0xAE, 0x00,
    DISP_WRITE_C8_D8, 0xAF, 0x00,
    DISP_WRITE_C8_D8, 0xB0, 0x48,
    DISP_WRITE_C8_D8, 0xB1, 0x00,
    DISP_WRITE_C8_D8, 0xB2, 0x0A,
    DISP_WRITE_C8_D8, 0xB3, 0x02,
    DISP_WRITE_C8_D8, 0xB4, 0xDA,
    DISP_WRITE_C8_D8, 0xB5, 0x04,
    DISP_WRITE_C8_D8, 0xB6, 0x00,
    DISP_WRITE_C8_D8, 0xB7, 0x00,
    DISP_WRITE_C8_D8, 0xB8, 0x48,
    DISP_WRITE_C8_D8, 0xB9, 0x00,
    DISP_WRITE_C8_D8, 0xBA, 0x0C,
    DISP_WRITE_C8_D8, 0xBB, 0x02,
    DISP_WRITE_C8_D8, 0xBC, 0xDC,
    DISP_WRITE_C8_D8, 0xBD, 0x04,
    DISP_WRITE_C8_D8, 0xBE, 0x00,
    DISP_WRITE_C8_D8, 0xBF, 0x00,
    DISP_WRITE_C8_D8, 0xC0, 0x10,
    DISP_WRITE_C8_D8, 0xC1, 0x47,
    DISP_WRITE_C8_D8, 0xC2, 0x56,
    DISP_WRITE_C8_D8, 0xC3, 0x65,
    DISP_WRITE_C8_D8, 0xC4, 0x74,
    DISP_WRITE_C8_D8, 0xC5, 0x88,
    DISP_WRITE_C8_D8, 0xC6, 0x99,
    DISP_WRITE_C8_D8, 0xC7, 0x01,
    DISP_WRITE_C8_D8, 0xC8, 0xBB,
    DISP_WRITE_C8_D8, 0xC9, 0xAA,
    DISP_WRITE_C8_D8, 0xD0, 0x10,
    DISP_WRITE_C8_D8, 0xD1, 0x47,
    DISP_WRITE_C8_D8, 0xD2, 0x56,
    DISP_WRITE_C8_D8, 0xD3, 0x65,
    DISP_WRITE_C8_D8, 0xD4, 0x74,
    DISP_WRITE_C8_D8, 0xD5, 0x88,
    DISP_WRITE_C8_D8, 0xD6, 0x99,
    DISP_WRITE_C8_D8, 0xD7, 0x01,
    DISP_WRITE_C8_D8, 0xD8, 0xBB,
    DISP_WRITE_C8_D8, 0xD9, 0xAA,
    DISP_WRITE_C8_D8, 0xF3, 0x01,
    DISP_WRITE_C8_D8, 0xF0, 0x00,
    DISP_WRITE_C8_D8, 0x3A, 0x55,
    DISP_WRITE_COMMAND_8, 0x21,
    DISP_WRITE_COMMAND_8, 0x11,
    DISP_END_WRITE,
    DISP_DELAY, ST77916_SLPOUT_DELAY_MS,
    DISP_BEGIN_WRITE,
    DISP_WRITE_COMMAND_8, 0x29,
    DISP_END_WRITE,
};

static spi_device_handle_t s_spi;
static bool s_spi_bus_initialized;
static bool s_spi_device_added;
static bool s_spi_bus_acquired;
static uint16_t *s_line_buffer;
static uint16_t *s_tx_buffer;
static bool s_tx_buffer_leased;
static uint8_t *s_cmd_buffer;
static bool s_initialized;

void *display_acquire_draw_buffer(size_t bytes) {
#if CONFIG_NUWAX_DISPLAY_QSPI_ENABLE
    if (s_initialized && s_tx_buffer && !s_tx_buffer_leased && bytes > 0 &&
        bytes <= ST77916_TX_BUFFER_PIXELS * sizeof(uint16_t)) {
        s_tx_buffer_leased = true;
        return s_tx_buffer;
    }
#endif
    (void)bytes;
    return NULL;
}

bool display_release_draw_buffer(void *buffer) {
    if (buffer && buffer == s_tx_buffer && s_tx_buffer_leased) {
        s_tx_buffer_leased = false;
        return true;
    }
    return false;
}
static bool s_backlight_pwm_initialized;
/* LVGL's periodic keepalive runs before the first UI frame during boot. Keep
 * its panel refresh independent from explicit backlight permission. */
typedef enum {
    NUWAX_BACKLIGHT_STAGE_OFF = 0,
    NUWAX_BACKLIGHT_STAGE_DIM,
    NUWAX_BACKLIGHT_STAGE_FULL,
} nuwax_backlight_stage_t;
static atomic_uint s_backlight_stage = ATOMIC_VAR_INIT(
    NUWAX_BACKLIGHT_STAGE_OFF);
static bool s_frame_active;
static bool s_region_active;
static int16_t s_region_x;
static int16_t s_region_y;
static int16_t s_region_w;
static int16_t s_region_h;
static nuwax_app_state_t s_current_state = NUWAX_APP_STATE_BOOTING;

static void st77916_drive_backlight_on(void) {
#if CONFIG_NUWAX_DISPLAY_BL_ACTIVE_LOW
    gpio_set_level(CONFIG_NUWAX_DISPLAY_PIN_BL, 0);
#else
    gpio_set_level(CONFIG_NUWAX_DISPLAY_PIN_BL, 1);
#endif
}

static esp_err_t st77916_backlight_configure_off(void) {
    const gpio_config_t bl_cfg = {
        .pin_bit_mask = 1ULL << CONFIG_NUWAX_DISPLAY_PIN_BL,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&bl_cfg), TAG, "backlight gpio config failed");
#if CONFIG_NUWAX_DISPLAY_BL_ACTIVE_LOW
    const int off_level = 1;
#else
    const int off_level = 0;
#endif
    ESP_RETURN_ON_ERROR(gpio_set_level(CONFIG_NUWAX_DISPLAY_PIN_BL, off_level),
                        TAG, "backlight off level failed");
    return ESP_OK;
}

#if !CONFIG_NUWAX_DISPLAY_QSPI_ENABLE
static esp_err_t spi_write_raw(const void *data, size_t len) {
    if (len == 0) {
        return ESP_OK;
    }

    spi_transaction_t transaction = {
        .length = len * 8,
        .tx_buffer = data,
    };
    return spi_device_polling_transmit(s_spi, &transaction);
}
#endif

#if CONFIG_NUWAX_DISPLAY_QSPI_ENABLE
static esp_err_t qspi_transmit(uint32_t flags,
                               uint32_t command,
                               uint32_t address,
                               const void *data,
                               size_t len) {
    spi_transaction_ext_t transaction = {0};
    transaction.base.flags = flags;
    transaction.base.cmd = command;
    transaction.base.addr = address;
    transaction.base.length = len * 8;
    transaction.base.tx_buffer = data;
    return spi_device_polling_transmit(s_spi, (spi_transaction_t *)&transaction);
}

static esp_err_t qspi_write_command_payload(uint8_t command, const void *data, size_t len) {
    uint32_t flags = SPI_TRANS_MULTILINE_CMD | SPI_TRANS_MULTILINE_ADDR;
    if (len > 0 && !data) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > ST77916_CMD_BUFFER_BYTES) {
        ESP_LOGW(TAG, "qspi command payload too large: cmd=0x%02x len=%u", command, (unsigned)len);
        return ESP_ERR_INVALID_SIZE;
    }

    spi_transaction_ext_t transaction = {0};
    transaction.base.flags = flags;
    transaction.base.cmd = ST77916_QSPI_WRITE_CMD;
    transaction.base.addr = ((uint32_t)command) << 8;
    transaction.base.length = len * 8;
    if (len > 0) {
        memcpy(s_cmd_buffer, data, len);
        transaction.base.tx_buffer = s_cmd_buffer;
    }
    return spi_device_polling_transmit(s_spi, (spi_transaction_t *)&transaction);
}

static esp_err_t qspi_write_pixels_bytes(const void *data, size_t len) {
    if (len == 0) {
        return ESP_OK;
    }
    return qspi_transmit(SPI_TRANS_MODE_QIO,
                         ST77916_QSPI_WRITE_PIXELS_CMD,
                         ST77916_QSPI_WRITE_PIXELS_ADDR,
                         data,
                         len);
}

#endif

static esp_err_t spi_command(uint8_t command) {
#if CONFIG_NUWAX_DISPLAY_QSPI_ENABLE
    gpio_set_level(CONFIG_NUWAX_DISPLAY_PIN_CS, 0);
    esp_err_t err = qspi_write_command_payload(command, NULL, 0);
    gpio_set_level(CONFIG_NUWAX_DISPLAY_PIN_CS, 1);
    return err;
#else
    gpio_set_level(CONFIG_NUWAX_DISPLAY_PIN_CS, 0);
    gpio_set_level(CONFIG_NUWAX_DISPLAY_PIN_DC, 0);
    esp_err_t err = spi_write_raw(&command, 1);
    gpio_set_level(CONFIG_NUWAX_DISPLAY_PIN_CS, 1);
    return err;
#endif
}

static esp_err_t spi_write_c8_d8(uint8_t command, uint8_t data) {
#if CONFIG_NUWAX_DISPLAY_QSPI_ENABLE
    gpio_set_level(CONFIG_NUWAX_DISPLAY_PIN_CS, 0);
    esp_err_t err = qspi_write_command_payload(command, &data, sizeof(data));
    gpio_set_level(CONFIG_NUWAX_DISPLAY_PIN_CS, 1);
    return err;
#else
    gpio_set_level(CONFIG_NUWAX_DISPLAY_PIN_CS, 0);
    gpio_set_level(CONFIG_NUWAX_DISPLAY_PIN_DC, 0);
    esp_err_t err = spi_write_raw(&command, 1);
    if (err == ESP_OK) {
        gpio_set_level(CONFIG_NUWAX_DISPLAY_PIN_DC, 1);
        err = spi_write_raw(&data, 1);
    }
    gpio_set_level(CONFIG_NUWAX_DISPLAY_PIN_CS, 1);
    return err;
#endif
}

static esp_err_t spi_write_c8_d16(uint8_t command, uint16_t data) {
    const uint8_t tx_data[2] = {(uint8_t)(data >> 8), (uint8_t)data};
#if CONFIG_NUWAX_DISPLAY_QSPI_ENABLE
    gpio_set_level(CONFIG_NUWAX_DISPLAY_PIN_CS, 0);
    esp_err_t err = qspi_write_command_payload(command, tx_data, sizeof(tx_data));
    gpio_set_level(CONFIG_NUWAX_DISPLAY_PIN_CS, 1);
    return err;
#else
    gpio_set_level(CONFIG_NUWAX_DISPLAY_PIN_CS, 0);
    gpio_set_level(CONFIG_NUWAX_DISPLAY_PIN_DC, 0);
    esp_err_t err = spi_write_raw(&command, 1);
    if (err == ESP_OK) {
        gpio_set_level(CONFIG_NUWAX_DISPLAY_PIN_DC, 1);
        err = spi_write_raw(tx_data, sizeof(tx_data));
    }
    gpio_set_level(CONFIG_NUWAX_DISPLAY_PIN_CS, 1);
    return err;
#endif
}

static esp_err_t spi_write_bytes(const uint8_t *data, size_t len) {
    if (len == 0) {
        return ESP_OK;
    }
#if CONFIG_NUWAX_DISPLAY_QSPI_ENABLE
    gpio_set_level(CONFIG_NUWAX_DISPLAY_PIN_CS, 0);
    esp_err_t err = qspi_write_pixels_bytes(data, len);
    gpio_set_level(CONFIG_NUWAX_DISPLAY_PIN_CS, 1);
    return err;
#else
    gpio_set_level(CONFIG_NUWAX_DISPLAY_PIN_CS, 0);
    gpio_set_level(CONFIG_NUWAX_DISPLAY_PIN_DC, 1);
    esp_err_t err = spi_write_raw(data, len);
    gpio_set_level(CONFIG_NUWAX_DISPLAY_PIN_CS, 1);
    return err;
#endif
}

static void copy_pixels_byte_swapped(uint16_t *destination,
                                     const uint16_t *source,
                                     size_t count) {
    /* Keep the paired byte swap without assuming callers provide 32-bit
     * alignment. A strided row can start at a 16-bit offset within the aligned
     * DMA buffer, so peel that pixel before using aligned 32-bit stores. */
    if ((((uintptr_t)destination) & (sizeof(uint32_t) - 1U)) != 0U &&
        count > 0U) {
        *destination++ = __builtin_bswap16(*source++);
        count--;
    }
    uint32_t *dst32 = (uint32_t *)destination;
    const size_t pairs = count / 2U;
    if ((((uintptr_t)source) & (sizeof(uint32_t) - 1U)) == 0U) {
        const uint32_t *src32 = (const uint32_t *)__builtin_assume_aligned(source, 4);
        for (size_t pair = 0; pair < pairs; ++pair) {
            uint32_t value;
            /* Explicit builtin bypasses IDF's -fno-builtin-memcpy. Known
             * alignment permits a word load without violating aliasing. */
            __builtin_memcpy(&value, src32 + pair, sizeof(value));
            dst32[pair] = ((value & 0x00FF00FFU) << 8) |
                          ((value & 0xFF00FF00U) >> 8);
        }
    } else {
        /* Odd clipping/stride can leave the source only halfword aligned.
         * Two halfword loads avoid both unaligned words and stack memcpy. */
        for (size_t pair = 0; pair < pairs; ++pair) {
            const uint32_t value = (uint32_t)source[pair * 2U] |
                                   ((uint32_t)source[pair * 2U + 1U] << 16);
            dst32[pair] = ((value & 0x00FF00FFU) << 8) |
                          ((value & 0xFF00FF00U) >> 8);
        }
    }
    if ((count & 1U) != 0U) {
        destination[count - 1U] = __builtin_bswap16(source[count - 1U]);
    }
}

static esp_err_t spi_write_pixels_strided(const uint16_t *pixels,
                                          size_t width,
                                          size_t height,
                                          size_t source_stride_pixels) {
    if (!pixels || width == 0U || height == 0U ||
        source_stride_pixels < width || height > SIZE_MAX / width) {
        return ESP_ERR_INVALID_ARG;
    }
#if CONFIG_NUWAX_UI_PERF_LOG_ENABLE
    nuwax_display_perf_t perf = {0};
#endif
    esp_err_t err = ESP_OK;
    uint8_t yield_counter = 0;
    const size_t count = width * height;
    size_t source_pixel = 0U;
    size_t remaining = count;

    /* The staging allocation can also be the next LVGL drawing surface.
     * Never overwrite it in a clipped/PSRAM/unaligned fallback. The separate
     * line buffer remains available for these uncommon, bounded transfers. */
    uint16_t *const staging = s_tx_buffer_leased ? s_line_buffer : s_tx_buffer;
    const size_t capacity = s_tx_buffer_leased ? ST77916_LINE_BUFFER_PIXELS
                                              : ST77916_TX_BUFFER_PIXELS;

    gpio_set_level(CONFIG_NUWAX_DISPLAY_PIN_CS, 0);
#if !CONFIG_NUWAX_DISPLAY_QSPI_ENABLE
    gpio_set_level(CONFIG_NUWAX_DISPLAY_PIN_DC, 1);
#else
    bool first_chunk = true;
#endif
    while (remaining > 0U) {
        size_t chunk = remaining > capacity
                           ? capacity
                           : remaining;
        if (chunk > ST77916_SPI_SAFE_TRANSFER_PIXELS) {
            chunk = ST77916_SPI_SAFE_TRANSFER_PIXELS;
        }

#if CONFIG_NUWAX_UI_PERF_LOG_ENABLE
        const int64_t copy_started_us = esp_timer_get_time();
#endif
        size_t packed = 0U;
        while (packed < chunk) {
            const size_t source_row = source_pixel / width;
            const size_t source_column = source_pixel % width;
            size_t run = width - source_column;
            if (run > chunk - packed) run = chunk - packed;
            copy_pixels_byte_swapped(
                staging + packed,
                pixels + source_row * source_stride_pixels + source_column,
                run);
            packed += run;
            source_pixel += run;
        }

#if CONFIG_NUWAX_UI_PERF_LOG_ENABLE
        perf.copy_us += esp_timer_get_time() - copy_started_us;
        const int64_t tx_started_us = esp_timer_get_time();
#endif
#if CONFIG_NUWAX_DISPLAY_QSPI_ENABLE
        spi_transaction_ext_t transaction = {0};
        transaction.base.length = chunk * sizeof(uint16_t) * 8;
        transaction.base.tx_buffer = staging;
        if (first_chunk) {
            transaction.base.flags = SPI_TRANS_MODE_QIO;
            transaction.base.cmd = ST77916_QSPI_WRITE_PIXELS_CMD;
            transaction.base.addr = ST77916_QSPI_WRITE_PIXELS_ADDR;
            first_chunk = false;
        } else {
            transaction.base.flags = SPI_TRANS_MODE_QIO | SPI_TRANS_VARIABLE_CMD |
                                     SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY;
            transaction.command_bits = 0;
            transaction.address_bits = 0;
            transaction.dummy_bits = 0;
        }
        const size_t transfer_bytes = chunk * sizeof(uint16_t);
        if (transfer_bytes >= ST77916_INTERRUPT_TRANSFER_MIN_BYTES) {
            err = spi_device_transmit(s_spi, (spi_transaction_t *)&transaction);
        } else {
            err = spi_device_polling_transmit(s_spi,
                                              (spi_transaction_t *)&transaction);
        }
#else
        err = spi_write_raw(staging, chunk * sizeof(uint16_t));
#endif
#if CONFIG_NUWAX_UI_PERF_LOG_ENABLE
        perf.tx_wait_us += esp_timer_get_time() - tx_started_us;
        perf.chunks++;
        if (err == ESP_OK) perf.bounce_bytes += chunk * sizeof(uint16_t);
        else perf.failures++;
#endif
        if (err != ESP_OK) {
            break;
        }
        remaining -= chunk;
        if (++yield_counter >= ST77916_FLUSH_YIELD_CHUNKS) {
            yield_counter = 0;
            taskYIELD();
        }
    }
    gpio_set_level(CONFIG_NUWAX_DISPLAY_PIN_CS, 1);
#if CONFIG_NUWAX_UI_PERF_LOG_ENABLE
    nuwax_display_perf_record(&perf);
#endif
    return err;
}

static esp_err_t spi_write_pixels(const uint16_t *pixels, size_t count) {
    return spi_write_pixels_strided(pixels, count, 1U, count);
}

/* Swap native little-endian RGB565 to the panel byte order in place. `pixels`
 * must be 32-bit aligned so the paired word swap keeps DMA alignment; a
 * misaligned tail pixel is peeled like the copy variant above. */
static void swap_pixels_inplace(uint16_t *pixels, size_t count) {
    if ((((uintptr_t)pixels) & (sizeof(uint32_t) - 1U)) != 0U) {
        for (size_t i = 0; i < count; ++i) {
            pixels[i] = __builtin_bswap16(pixels[i]);
        }
        return;
    }
    uint32_t *pairs = (uint32_t *)pixels;
    const size_t pair_count = count / 2U;
    for (size_t pair = 0; pair < pair_count; ++pair) {
        const uint32_t value = pairs[pair];
        pairs[pair] = ((value & 0x00FF00FFU) << 8) |
                      ((value & 0xFF00FF00U) >> 8);
    }
    if ((count & 1U) != 0U) {
        pixels[count - 1U] = __builtin_bswap16(pixels[count - 1U]);
    }
}

/* DMA-fast transmit: `pixels` is already byte-swapped and resides in internal
 * DMA-capable RAM, so chunks stream straight from the caller's buffer without
 * the intermediate tx-buffer copy. Chunk sizing follows the same interrupt vs
 * polling and yield rules as the copy path. */
static esp_err_t st77916_transmit_pixels_direct(const uint16_t *pixels,
                                                size_t count) {
#if CONFIG_NUWAX_UI_PERF_LOG_ENABLE
    nuwax_display_perf_t perf = {0};
#endif
    esp_err_t err = ESP_OK;
    uint8_t yield_counter = 0;
    size_t offset = 0U;
    size_t remaining = count;

    gpio_set_level(CONFIG_NUWAX_DISPLAY_PIN_CS, 0);
    bool first_chunk = true;
    while (remaining > 0U) {
        size_t chunk = remaining > ST77916_SPI_SAFE_TRANSFER_PIXELS
                           ? ST77916_SPI_SAFE_TRANSFER_PIXELS
                           : remaining;
        spi_transaction_ext_t transaction = {0};
        transaction.base.length = chunk * sizeof(uint16_t) * 8;
        transaction.base.tx_buffer = pixels + offset;
        if (first_chunk) {
            transaction.base.flags = SPI_TRANS_MODE_QIO;
            transaction.base.cmd = ST77916_QSPI_WRITE_PIXELS_CMD;
            transaction.base.addr = ST77916_QSPI_WRITE_PIXELS_ADDR;
            first_chunk = false;
        } else {
            transaction.base.flags = SPI_TRANS_MODE_QIO | SPI_TRANS_VARIABLE_CMD |
                                     SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY;
            transaction.command_bits = 0;
            transaction.address_bits = 0;
            transaction.dummy_bits = 0;
        }
#if CONFIG_NUWAX_UI_PERF_LOG_ENABLE
        const int64_t tx_started_us = esp_timer_get_time();
#endif
        const size_t transfer_bytes = chunk * sizeof(uint16_t);
        if (transfer_bytes >= ST77916_INTERRUPT_TRANSFER_MIN_BYTES) {
            err = spi_device_transmit(s_spi, (spi_transaction_t *)&transaction);
        } else {
            err = spi_device_polling_transmit(s_spi,
                                              (spi_transaction_t *)&transaction);
        }
#if CONFIG_NUWAX_UI_PERF_LOG_ENABLE
        perf.tx_wait_us += esp_timer_get_time() - tx_started_us;
        perf.chunks++;
        if (err == ESP_OK) perf.direct_bytes += chunk * sizeof(uint16_t);
        else perf.failures++;
#endif
        if (err != ESP_OK) {
            break;
        }
        offset += chunk;
        remaining -= chunk;
        if (++yield_counter >= ST77916_FLUSH_YIELD_CHUNKS) {
            yield_counter = 0;
            taskYIELD();
        }
    }
    gpio_set_level(CONFIG_NUWAX_DISPLAY_PIN_CS, 1);
#if CONFIG_NUWAX_UI_PERF_LOG_ENABLE
    nuwax_display_perf_record(&perf);
#endif
    return err;
}

static esp_err_t spi_write_c8_bytes(uint8_t command, const uint8_t *data, size_t len) {
#if CONFIG_NUWAX_DISPLAY_QSPI_ENABLE
    gpio_set_level(CONFIG_NUWAX_DISPLAY_PIN_CS, 0);
    esp_err_t err = qspi_write_command_payload(command, data, len);
    gpio_set_level(CONFIG_NUWAX_DISPLAY_PIN_CS, 1);
    return err;
#else
    gpio_set_level(CONFIG_NUWAX_DISPLAY_PIN_CS, 0);
    gpio_set_level(CONFIG_NUWAX_DISPLAY_PIN_DC, 0);
    esp_err_t err = spi_write_raw(&command, 1);
    if (err == ESP_OK && len > 0) {
        gpio_set_level(CONFIG_NUWAX_DISPLAY_PIN_DC, 1);
        err = spi_write_raw(data, len);
    }
    gpio_set_level(CONFIG_NUWAX_DISPLAY_PIN_CS, 1);
    return err;
#endif
}

static esp_err_t st77916_set_addr_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    uint8_t tx_data[4] = {(uint8_t)(x >> 8), (uint8_t)x, (uint8_t)((x + w - 1) >> 8), (uint8_t)(x + w - 1)};
    esp_err_t err = spi_write_c8_bytes(ST77916_CASET, tx_data, sizeof(tx_data));
    if (err == ESP_OK) {
        tx_data[0] = (uint8_t)(y >> 8);
        tx_data[1] = (uint8_t)y;
        tx_data[2] = (uint8_t)((y + h - 1) >> 8);
        tx_data[3] = (uint8_t)(y + h - 1);
        err = spi_write_c8_bytes(ST77916_RASET, tx_data, sizeof(tx_data));
    }
    if (err == ESP_OK) {
        err = spi_command(ST77916_RAMWR);
    }
    return err;
}

static esp_err_t st77916_fill_screen(uint16_t color) {
    for (uint16_t i = 0; i < ST77916_WIDTH; i++) {
        s_line_buffer[i] = color;
    }
    ESP_RETURN_ON_ERROR(st77916_set_addr_window(0, 0, ST77916_WIDTH, ST77916_HEIGHT), TAG, "set full window failed");
    for (uint16_t row = 0; row < ST77916_HEIGHT; row++) {
        ESP_RETURN_ON_ERROR(spi_write_pixels(s_line_buffer, ST77916_WIDTH), TAG, "screen fill failed");
    }
    return ESP_OK;
}

/* Boot-only writer: called before LVGL exists, with the backlight still off.
 * Reuse the existing 720-byte line buffer; no framebuffer or runtime heap.
 * The panel retains the logo while LVGL output is gated during bootstrap. */
static esp_err_t st77916_draw_boot_logo(void) {
    _Static_assert(NUWAX_BOOT_LOGO_WIDTH == ST77916_WIDTH &&
                   NUWAX_BOOT_LOGO_HEIGHT == ST77916_HEIGHT,
                   "Boot logo must match the panel");
    ESP_RETURN_ON_ERROR(st77916_set_addr_window(0, 0, ST77916_WIDTH,
                                               ST77916_HEIGHT),
                        TAG, "boot logo window failed");
    for (size_t y = 0U; y < NUWAX_BOOT_LOGO_HEIGHT; ++y) {
        if (!nuwax_boot_logo_decode_row(y, s_line_buffer, ST77916_WIDTH)) {
            ESP_LOGE(TAG, "boot logo invalid row=%u", (unsigned)y);
            return ESP_ERR_INVALID_SIZE;
        }
        ESP_RETURN_ON_ERROR(spi_write_pixels(s_line_buffer, ST77916_WIDTH),
                            TAG, "boot logo write failed");
    }
    return ESP_OK;
}

static void draw_ellipse_to_line(uint16_t *line,
                                 int16_t y,
                                 int16_t cx,
                                 int16_t cy,
                                 int16_t rx,
                                 int16_t ry,
                                 uint16_t color) {
    const int32_t dy = y - cy;
    const int32_t ry2 = ry * ry;
    if (dy * dy > ry2) {
        return;
    }

    const int32_t rx2 = rx * rx;
    const int32_t lhs_y = dy * dy * rx2;
    const int32_t rhs = rx2 * ry2;
    for (int16_t x = 0; x < ST77916_WIDTH; x++) {
        const int32_t dx = x - cx;
        if ((dx * dx * ry2 + lhs_y) <= rhs) {
            line[x] = color;
        }
    }
}

static __attribute__((unused)) esp_err_t st77916_draw_eyes(int16_t eye_rx, int16_t eye_ry, int16_t eye_gap, int16_t eye_y) {
    const int16_t left_cx = (ST77916_WIDTH / 2) - (eye_gap / 2);
    const int16_t right_cx = (ST77916_WIDTH / 2) + (eye_gap / 2);
    const uint16_t y_start = eye_y - eye_ry - 2;
    const uint16_t y_end = eye_y + eye_ry + 2;

    ESP_RETURN_ON_ERROR(st77916_fill_screen(ST77916_COLOR_BLACK), TAG, "clear screen failed");
    ESP_RETURN_ON_ERROR(st77916_set_addr_window(0, y_start, ST77916_WIDTH, y_end - y_start + 1),
                        TAG,
                        "set eyes window failed");

    for (uint16_t y = y_start; y <= y_end; y++) {
        memset(s_line_buffer, 0, ST77916_WIDTH * sizeof(uint16_t));
        draw_ellipse_to_line(s_line_buffer, y, left_cx, eye_y, eye_rx, eye_ry, ST77916_COLOR_WHITE);
        draw_ellipse_to_line(s_line_buffer, y, right_cx, eye_y, eye_rx, eye_ry, ST77916_COLOR_WHITE);
        ESP_RETURN_ON_ERROR(spi_write_pixels(s_line_buffer, ST77916_WIDTH), TAG, "eyes write failed");
    }
    return ESP_OK;
}

static esp_err_t st77916_run_init_sequence(void) {
    for (size_t i = 0; i < sizeof(s_st77916_init_operations); i++) {
        uint8_t command = 0;
        uint16_t data16 = 0;
        uint8_t len = 0;
        switch ((display_operation_t)s_st77916_init_operations[i]) {
            case DISP_BEGIN_WRITE:
            case DISP_END_WRITE:
                break;
            case DISP_WRITE_COMMAND_8:
                command = s_st77916_init_operations[++i];
                if ((i + 1) < sizeof(s_st77916_init_operations) &&
                    s_st77916_init_operations[i + 1] == DISP_WRITE_BYTES) {
                    i += 2;
                    len = s_st77916_init_operations[i];
                    ESP_RETURN_ON_ERROR(spi_write_c8_bytes(command, &s_st77916_init_operations[i + 1], len),
                                        TAG,
                                        "command bytes failed");
                    i += len;
                } else {
                    ESP_RETURN_ON_ERROR(spi_command(command), TAG, "command failed");
                }
                break;
            case DISP_WRITE_BYTES:
                len = s_st77916_init_operations[++i];
                ESP_RETURN_ON_ERROR(spi_write_bytes(&s_st77916_init_operations[i + 1], len), TAG, "bytes failed");
                i += len;
                break;
            case DISP_WRITE_C8_D8:
                command = s_st77916_init_operations[++i];
                ESP_RETURN_ON_ERROR(spi_write_c8_d8(command, s_st77916_init_operations[++i]), TAG, "c8d8 failed");
                break;
            case DISP_WRITE_C8_D16:
                command = s_st77916_init_operations[++i];
                data16 = ((uint16_t)s_st77916_init_operations[++i]) << 8;
                data16 |= s_st77916_init_operations[++i];
                ESP_RETURN_ON_ERROR(spi_write_c8_d16(command, data16), TAG, "c8d16 failed");
                break;
            case DISP_DELAY:
                vTaskDelay(pdMS_TO_TICKS(s_st77916_init_operations[++i]));
                break;
            default:
                ESP_LOGW(TAG, "unsupported init op=%u index=%u", s_st77916_init_operations[i], (unsigned)i);
                break;
        }
    }
    return ESP_OK;
}

static esp_err_t st77916_reset(void) {
    gpio_config_t reset_cfg = {
        .pin_bit_mask = 1ULL << CONFIG_NUWAX_DISPLAY_PIN_RST,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&reset_cfg), TAG, "reset gpio config failed");
#if defined(CONFIG_NUWAX_DISPLAY_RST_ACTIVE_HIGH)
    const int reset_active = 1;
    const int reset_inactive = 0;
#else
    const int reset_active = 0;
    const int reset_inactive = 1;
#endif
    gpio_set_level(CONFIG_NUWAX_DISPLAY_PIN_RST, reset_inactive);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(CONFIG_NUWAX_DISPLAY_PIN_RST, reset_active);
    vTaskDelay(pdMS_TO_TICKS(ST77916_RST_DELAY_MS));
    gpio_set_level(CONFIG_NUWAX_DISPLAY_PIN_RST, reset_inactive);
    vTaskDelay(pdMS_TO_TICKS(ST77916_RST_DELAY_MS));
    ESP_LOGI(TAG, "display reset pulsed gpio=%d active_high=%d",
             CONFIG_NUWAX_DISPLAY_PIN_RST,
             NUWAX_DISPLAY_RST_ACTIVE_HIGH_VALUE);
    return ESP_OK;
}

esp_err_t nuwax_display_backlight_set_percent(uint8_t percent)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (percent > 100) return ESP_ERR_INVALID_ARG;
    uint32_t duty = ((uint32_t)percent * 1023u + 50u) / 100u;
#if CONFIG_NUWAX_DISPLAY_BL_ACTIVE_LOW
    duty = 1023u - duty;
#endif
    if (!s_backlight_pwm_initialized) {
        const ledc_timer_config_t timer = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = LEDC_TIMER_10_BIT,
            .timer_num = LEDC_TIMER_1,
            .freq_hz = 5000,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight PWM timer failed");
        const ledc_channel_config_t channel = {
            .gpio_num = CONFIG_NUWAX_DISPLAY_PIN_BL,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = LEDC_CHANNEL_1,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER_1,
            /* Start at the requested duty; do not briefly expose full
             * brightness while the LEDC channel is being attached. */
            .duty = duty,
            .hpoint = 0,
        };
        ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "backlight PWM channel failed");
        s_backlight_pwm_initialized = true;
    }
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty),
                        TAG, "backlight PWM duty failed");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1),
                        TAG, "backlight PWM update failed");
    atomic_store_explicit(
        &s_backlight_stage,
        percent == 0U
            ? NUWAX_BACKLIGHT_STAGE_OFF
            : (percent >= 100U ? NUWAX_BACKLIGHT_STAGE_FULL
                               : NUWAX_BACKLIGHT_STAGE_DIM),
        memory_order_release);
    ESP_LOGI(TAG, "backlight brightness=%u%% duty=%u", percent, (unsigned)duty);
    return ESP_OK;
}

esp_err_t nuwax_display_backlight_force_off(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
#if CONFIG_NUWAX_DISPLAY_BL_ACTIVE_LOW
    const int off_level = 1;
#else
    const int off_level = 0;
#endif
    atomic_store_explicit(&s_backlight_stage,
                          NUWAX_BACKLIGHT_STAGE_OFF,
                          memory_order_release);
    if (s_backlight_pwm_initialized) {
        ESP_RETURN_ON_ERROR(ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, off_level),
                            TAG, "backlight PWM stop failed");
        s_backlight_pwm_initialized = false;
    }
    const gpio_config_t bl_cfg = {
        .pin_bit_mask = 1ULL << CONFIG_NUWAX_DISPLAY_PIN_BL,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&bl_cfg), TAG, "backlight off gpio config failed");
    return gpio_set_level(CONFIG_NUWAX_DISPLAY_PIN_BL, off_level);
}

esp_err_t nuwax_display_te_selftest(uint32_t duration_ms, uint32_t *rising_edges)
{
    if (rising_edges) *rising_edges = 0;
    if (!s_initialized || duration_ms < 100) return ESP_ERR_INVALID_ARG;
#if CONFIG_NUWAX_BOARD_ESP_VOCAT_V1_0
    gpio_config_t te_config = {
        .pin_bit_mask = 1ULL << NUWAX_VOCAT_LCD_TE_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&te_config), TAG, "LCD TE gpio config failed");
    ESP_RETURN_ON_ERROR(spi_write_c8_d8(ST77916_TEON, 0x00), TAG, "LCD TE enable failed");

    const int64_t deadline = esp_timer_get_time() + (int64_t)duration_ms * 1000;
    int previous = gpio_get_level(NUWAX_VOCAT_LCD_TE_GPIO);
    uint32_t edges = 0;
    while (esp_timer_get_time() < deadline) {
        const int level = gpio_get_level(NUWAX_VOCAT_LCD_TE_GPIO);
        if (level && !previous) edges++;
        previous = level;
        esp_rom_delay_us(100);
    }
    (void)spi_command(ST77916_TEOFF);
    if (rising_edges) *rising_edges = edges;
    ESP_LOGI(TAG,
             "LCD TE selftest gpio=%d duration=%ums rising_edges=%u",
             NUWAX_VOCAT_LCD_TE_GPIO, (unsigned)duration_ms, (unsigned)edges);
    return edges > 0 ? ESP_OK : ESP_ERR_TIMEOUT;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static esp_err_t st77916_spi_init(void) {
    gpio_config_t cs_cfg = {
#if CONFIG_NUWAX_DISPLAY_QSPI_ENABLE
        .pin_bit_mask = (1ULL << CONFIG_NUWAX_DISPLAY_PIN_CS),
#else
        .pin_bit_mask = (1ULL << CONFIG_NUWAX_DISPLAY_PIN_CS) | (1ULL << CONFIG_NUWAX_DISPLAY_PIN_DC),
#endif
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cs_cfg), TAG, "display control gpio config failed");
    gpio_set_level(CONFIG_NUWAX_DISPLAY_PIN_CS, 1);
#if !CONFIG_NUWAX_DISPLAY_QSPI_ENABLE
    gpio_set_level(CONFIG_NUWAX_DISPLAY_PIN_DC, 1);
#endif

    spi_bus_config_t bus_cfg = {
        .data0_io_num = CONFIG_NUWAX_DISPLAY_PIN_MOSI,
#if CONFIG_NUWAX_DISPLAY_QSPI_ENABLE
        .data1_io_num = CONFIG_NUWAX_DISPLAY_PIN_DATA1,
        .sclk_io_num = CONFIG_NUWAX_DISPLAY_PIN_SCK,
        .data2_io_num = CONFIG_NUWAX_DISPLAY_PIN_DATA2,
        .data3_io_num = CONFIG_NUWAX_DISPLAY_PIN_DATA3,
#else
        .data1_io_num = -1,
        .sclk_io_num = CONFIG_NUWAX_DISPLAY_PIN_SCK,
        .data2_io_num = -1,
        .data3_io_num = -1,
#endif
        .data4_io_num = -1,
        .data5_io_num = -1,
        .data6_io_num = -1,
        .data7_io_num = -1,
        .max_transfer_sz = ST77916_SPI_SAFE_TRANSFER_BYTES + 8,
        .flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS,
        .isr_cpu_id = ESP_INTR_CPU_AFFINITY_AUTO,
        .intr_flags = 0,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi bus init failed: %s", esp_err_to_name(err));
        return err;
    }
    s_spi_bus_initialized = true;

    spi_device_interface_config_t dev_cfg = {
#if CONFIG_NUWAX_DISPLAY_QSPI_ENABLE
        .command_bits = 8,
        .address_bits = 24,
#else
        .command_bits = 0,
        .address_bits = 0,
#endif
        .dummy_bits = 0,
        .mode = 0,
        .clock_speed_hz = CONFIG_NUWAX_DISPLAY_SPI_HZ,
        .spics_io_num = -1,
        .flags = SPI_DEVICE_HALFDUPLEX,
        .queue_size = 2,
    };
    err = spi_bus_add_device(SPI2_HOST, &dev_cfg, &s_spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi add device failed: %s", esp_err_to_name(err));
        return err;
    }
    s_spi_device_added = true;

    err = spi_device_acquire_bus(s_spi, portMAX_DELAY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi acquire failed: %s", esp_err_to_name(err));
        return err;
    }
    s_spi_bus_acquired = true;
    return ESP_OK;
}

static esp_err_t st77916_release_init_resources(void) {
    esp_err_t first_err = ESP_OK;

    s_initialized = false;
    atomic_store_explicit(&s_backlight_stage,
                          NUWAX_BACKLIGHT_STAGE_OFF,
                          memory_order_release);
    s_frame_active = false;
    s_region_active = false;
    s_region_x = 0;
    s_region_y = 0;
    s_region_w = 0;
    s_region_h = 0;

    if (s_spi_bus_acquired) {
        if (s_spi) {
            spi_device_release_bus(s_spi);
        }
        s_spi_bus_acquired = false;
    }

    if (s_spi_device_added) {
        esp_err_t err = s_spi ? spi_bus_remove_device(s_spi) : ESP_ERR_INVALID_STATE;
        if (err == ESP_OK) {
            s_spi_device_added = false;
            s_spi = NULL;
        } else {
            ESP_LOGE(TAG, "spi remove device during init rollback failed: %s", esp_err_to_name(err));
            first_err = err;
        }
    } else {
        s_spi = NULL;
    }

    if (s_spi_bus_initialized && !s_spi_device_added) {
        esp_err_t err = spi_bus_free(SPI2_HOST);
        if (err == ESP_OK) {
            s_spi_bus_initialized = false;
        } else {
            ESP_LOGE(TAG, "spi bus free during init rollback failed: %s", esp_err_to_name(err));
            if (first_err == ESP_OK) {
                first_err = err;
            }
        }
    }

    heap_caps_free(s_cmd_buffer);
    s_cmd_buffer = NULL;
    heap_caps_free(s_tx_buffer);
    s_tx_buffer = NULL;
    heap_caps_free(s_line_buffer);
    s_line_buffer = NULL;
    return first_err;
}

esp_err_t nuwax_display_init(void) {
    if (s_initialized) {
        return ESP_OK;
    }

    if (s_spi_bus_initialized || s_spi_device_added || s_spi_bus_acquired || s_spi ||
        s_line_buffer || s_tx_buffer || s_cmd_buffer) {
        ESP_LOGW(TAG, "recovering resources left by an earlier display init failure");
        esp_err_t cleanup_err = st77916_release_init_resources();
        if (cleanup_err != ESP_OK) {
            return cleanup_err;
        }
    }

    esp_err_t err = ESP_OK;
    atomic_store_explicit(&s_backlight_stage,
                          NUWAX_BACKLIGHT_STAGE_OFF,
                          memory_order_release);

    /* Keep the panel dark until the complete boot logo has been written.
     * Never expose the controller reset colour or a partial logo. */
    err = st77916_backlight_configure_off();
    if (err != ESP_OK) {
        goto fail;
    }

    s_line_buffer = heap_caps_aligned_alloc(16,
                                            ST77916_LINE_BUFFER_PIXELS * sizeof(uint16_t),
                                            MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_tx_buffer = heap_caps_aligned_alloc(16,
                                          ST77916_TX_BUFFER_PIXELS * sizeof(uint16_t),
                                          MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_cmd_buffer = heap_caps_aligned_alloc(16,
                                           ST77916_CMD_BUFFER_BYTES,
                                           MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_line_buffer || !s_tx_buffer || !s_cmd_buffer) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    ESP_LOGI(TAG,
             "Display DMA buffers line=%u tx=%u cmd=%u bytes flush_lines=%u",
             (unsigned)(ST77916_LINE_BUFFER_PIXELS * sizeof(uint16_t)),
             (unsigned)(ST77916_TX_BUFFER_PIXELS * sizeof(uint16_t)),
             (unsigned)ST77916_CMD_BUFFER_BYTES,
             (unsigned)ST77916_FLUSH_LINES);

    err = st77916_spi_init();
    if (err != ESP_OK) {
        goto fail;
    }
    err = st77916_reset();
    if (err != ESP_OK) {
        goto fail;
    }
    err = st77916_run_init_sequence();
    if (err != ESP_OK) {
        goto fail;
    }
    err = spi_write_c8_d8(ST77916_MADCTL, 0x00);
    if (err != ESP_OK) {
        goto fail;
    }
#if CONFIG_NUWAX_DISPLAY_IPS_INVERT
    err = spi_command(ST77916_INVON);
#else
    err = spi_command(ST77916_INVOFF);
#endif
    if (err != ESP_OK) {
        goto fail;
    }
    err = st77916_fill_screen(ST77916_COLOR_BLACK);
    if (err != ESP_OK) {
        goto fail;
    }
    err = st77916_draw_boot_logo();
    if (err != ESP_OK) {
        goto fail;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "boot logo ready brand=ling_zhu size=360x360 buffer_bytes=720");

#if CONFIG_NUWAX_DISPLAY_QSPI_ENABLE
    ESP_LOGI(TAG,
             "ST77916 QSPI display initialized %dx%d sck=%d data0=%d data1=%d data2=%d data3=%d cs=%d rst=%d rst_active_high=%d bl=%d bl_active_low=%d hz=%d",
             ST77916_WIDTH,
             ST77916_HEIGHT,
             CONFIG_NUWAX_DISPLAY_PIN_SCK,
             CONFIG_NUWAX_DISPLAY_PIN_MOSI,
             CONFIG_NUWAX_DISPLAY_PIN_DATA1,
             CONFIG_NUWAX_DISPLAY_PIN_DATA2,
             CONFIG_NUWAX_DISPLAY_PIN_DATA3,
             CONFIG_NUWAX_DISPLAY_PIN_CS,
             CONFIG_NUWAX_DISPLAY_PIN_RST,
             NUWAX_DISPLAY_RST_ACTIVE_HIGH_VALUE,
             CONFIG_NUWAX_DISPLAY_PIN_BL,
             NUWAX_DISPLAY_BL_ACTIVE_LOW_VALUE,
             CONFIG_NUWAX_DISPLAY_SPI_HZ);
#else
    ESP_LOGI(TAG,
             "ST77916 SPI display initialized %dx%d sck=%d mosi=%d dc=%d cs=%d rst=%d rst_active_high=%d bl=%d bl_active_low=%d hz=%d",
             ST77916_WIDTH,
             ST77916_HEIGHT,
             CONFIG_NUWAX_DISPLAY_PIN_SCK,
             CONFIG_NUWAX_DISPLAY_PIN_MOSI,
             CONFIG_NUWAX_DISPLAY_PIN_DC,
             CONFIG_NUWAX_DISPLAY_PIN_CS,
             CONFIG_NUWAX_DISPLAY_PIN_RST,
             NUWAX_DISPLAY_RST_ACTIVE_HIGH_VALUE,
             CONFIG_NUWAX_DISPLAY_PIN_BL,
             NUWAX_DISPLAY_BL_ACTIVE_LOW_VALUE,
             CONFIG_NUWAX_DISPLAY_SPI_HZ);
#endif

    return ESP_OK;

fail:
    (void)st77916_backlight_configure_off();
    esp_err_t cleanup_err = st77916_release_init_resources();
    if (cleanup_err != ESP_OK) {
        ESP_LOGE(TAG,
                 "display init failed (%s) and rollback was incomplete (%s)",
                 esp_err_to_name(err),
                 esp_err_to_name(cleanup_err));
    } else {
        ESP_LOGE(TAG, "display init failed and was rolled back: %s", esp_err_to_name(err));
    }
    return err;
}

void nuwax_display_set_state(nuwax_app_state_t state) {
    s_current_state = state;
}

esp_err_t display_clear_screen(void) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    return st77916_fill_screen(ST77916_COLOR_BLACK);
}

void display_draw_pixel(int16_t x, int16_t y, uint16_t color) {
    if (!s_initialized) return;
    if (x < 0 || x >= ST77916_WIDTH || y < 0 || y >= ST77916_HEIGHT) return;

    if (s_frame_active || s_region_active) {
        return;
    }

    s_line_buffer[0] = color;
    if (st77916_set_addr_window(x, y, 1, 1) == ESP_OK) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(spi_write_pixels(s_line_buffer, 1));
    }
}

esp_err_t display_flush(void) {
    return ESP_OK;
}

esp_err_t display_keepalive(void) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_frame_active || s_region_active) {
        return ESP_ERR_INVALID_STATE;
    }
    if (atomic_load_explicit(&s_backlight_stage,
                             memory_order_acquire) !=
        NUWAX_BACKLIGHT_STAGE_OFF) {
        if (!s_backlight_pwm_initialized) st77916_drive_backlight_on();
    }
    ESP_RETURN_ON_ERROR(spi_command(ST77916_DISPON), TAG, "display on keepalive failed");
    return ESP_OK;
}

uint16_t *display_begin_frame_line(int16_t y) {
    if (!s_initialized || s_region_active || y < 0 || y >= ST77916_HEIGHT) {
        return NULL;
    }
    if (!s_frame_active) {
        esp_err_t err = st77916_set_addr_window(0, 0, ST77916_WIDTH, ST77916_HEIGHT);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "begin frame failed: %s", esp_err_to_name(err));
            return NULL;
        }
        s_frame_active = true;
    }
    memset(s_line_buffer, 0, ST77916_WIDTH * sizeof(uint16_t));
    return s_line_buffer;
}

esp_err_t display_flush_frame_line(int16_t y) {
    (void)y;
    if (!s_initialized || !s_frame_active) {
        return ESP_ERR_INVALID_STATE;
    }
    return spi_write_pixels(s_line_buffer, ST77916_WIDTH);
}

esp_err_t display_end_frame(void) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    s_frame_active = false;
    return ESP_OK;
}

uint16_t *display_begin_region_line(int16_t x, int16_t y, int16_t w, int16_t h) {
    if (!s_initialized || s_frame_active) {
        return NULL;
    }
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > ST77916_WIDTH || y + h > ST77916_HEIGHT) {
        return NULL;
    }
    if ((size_t)w > ST77916_LINE_BUFFER_PIXELS) {
        return NULL;
    }

    if (!s_region_active) {
        esp_err_t err = st77916_set_addr_window((uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "begin region failed: %s", esp_err_to_name(err));
            return NULL;
        }
        s_region_active = true;
        s_region_x = x;
        s_region_y = y;
        s_region_w = w;
        s_region_h = h;
    } else if (x != s_region_x || y != s_region_y || w != s_region_w || h != s_region_h) {
        return NULL;
    }

    memset(s_line_buffer, 0, (size_t)w * sizeof(uint16_t));
    return s_line_buffer;
}

esp_err_t display_flush_region_line(void) {
    if (!s_initialized || !s_region_active) {
        return ESP_ERR_INVALID_STATE;
    }
    return spi_write_pixels(s_line_buffer, (size_t)s_region_w);
}

esp_err_t display_end_region(void) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    s_region_active = false;
    s_region_x = 0;
    s_region_y = 0;
    s_region_w = 0;
    s_region_h = 0;
    return ESP_OK;
}

esp_err_t display_write_region_pixels(int16_t x, int16_t y, int16_t w, int16_t h,
                                      const uint16_t *pixels, size_t pixel_count) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!pixels || w <= 0 || h <= 0 || pixel_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (x < 0 || y < 0 || x + w > ST77916_WIDTH || y + h > ST77916_HEIGHT) {
        ESP_LOGW(TAG, "reject out-of-bounds region x=%d y=%d w=%d h=%d",
                 (int)x, (int)y, (int)w, (int)h);
        return ESP_ERR_INVALID_ARG;
    }
    if (pixel_count != (size_t)w * (size_t)h) {
        ESP_LOGW(TAG, "reject region pixel_count mismatch x=%d y=%d w=%d h=%d pixels=%u",
                 (int)x, (int)y, (int)w, (int)h, (unsigned)pixel_count);
        return ESP_ERR_INVALID_ARG;
    }
    if (s_frame_active || s_region_active) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(st77916_set_addr_window((uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h),
                        TAG,
                        "set window failed");

    return spi_write_pixels(pixels, pixel_count);
}

esp_err_t display_write_region_pixels_strided(
    int16_t x, int16_t y, int16_t w, int16_t h,
    const uint16_t *pixels, size_t source_stride_pixels) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!pixels || w <= 0 || h <= 0 ||
        source_stride_pixels < (size_t)w) {
        return ESP_ERR_INVALID_ARG;
    }
    if (x < 0 || y < 0 || x + w > ST77916_WIDTH ||
        y + h > ST77916_HEIGHT) {
        ESP_LOGW(TAG, "reject out-of-bounds strided region x=%d y=%d w=%d h=%d",
                 (int)x, (int)y, (int)w, (int)h);
        return ESP_ERR_INVALID_ARG;
    }
    if ((size_t)(h - 1) > (SIZE_MAX - (size_t)w) /
                                source_stride_pixels) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (s_frame_active || s_region_active) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(
        st77916_set_addr_window((uint16_t)x, (uint16_t)y,
                                (uint16_t)w, (uint16_t)h),
        TAG, "set strided window failed");
    return spi_write_pixels_strided(pixels, (size_t)w, (size_t)h,
                                    source_stride_pixels);
}

esp_err_t display_write_region_pixels_swapped_inplace(
    int16_t x, int16_t y, int16_t w, int16_t h,
    uint16_t *pixels, size_t pixel_count) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!pixels || w <= 0 || h <= 0 || pixel_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (x < 0 || y < 0 || x + w > ST77916_WIDTH || y + h > ST77916_HEIGHT) {
        ESP_LOGW(TAG, "reject inplace region x=%d y=%d w=%d h=%d",
                 (int)x, (int)y, (int)w, (int)h);
        return ESP_ERR_INVALID_ARG;
    }
    if (pixel_count != (size_t)w * (size_t)h) {
        ESP_LOGW(TAG, "reject inplace pixel_count mismatch x=%d y=%d w=%d h=%d pixels=%u",
                 (int)x, (int)y, (int)w, (int)h, (unsigned)pixel_count);
        return ESP_ERR_INVALID_ARG;
    }
    if (s_frame_active || s_region_active) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(st77916_set_addr_window((uint16_t)x, (uint16_t)y,
                                                (uint16_t)w, (uint16_t)h),
                        TAG, "set inplace window failed");

    /* The caller owns the buffer until the transfer completes (the LVGL flush
     * job holds the pipeline slot), so mutating it in place cannot race
     * rendering. A DMA-capable, 32-bit-aligned internal buffer skips the
     * tx-buffer copy entirely; PSRAM or misaligned buffers keep the proven
     * swap-copy path. */
    if (esp_ptr_dma_capable(pixels) &&
        (((uintptr_t)pixels) & (sizeof(uint32_t) - 1U)) == 0U) {
#if CONFIG_NUWAX_UI_PERF_LOG_ENABLE
        const int64_t swap_started_us = esp_timer_get_time();
#endif
        swap_pixels_inplace(pixels, pixel_count);
#if CONFIG_NUWAX_UI_PERF_LOG_ENABLE
        const nuwax_display_perf_t perf = {
            .swap_us = esp_timer_get_time() - swap_started_us,
        };
        nuwax_display_perf_record(&perf);
#endif
        return st77916_transmit_pixels_direct(pixels, pixel_count);
    }
    return spi_write_pixels(pixels, pixel_count);
}

#else

esp_err_t nuwax_display_init(void) {
    return ESP_OK;
}

void nuwax_display_set_state(nuwax_app_state_t state) {
    (void)state;
}

esp_err_t nuwax_display_backlight_set_percent(uint8_t percent) {
    (void)percent;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t nuwax_display_backlight_force_off(void) {
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t nuwax_display_te_selftest(uint32_t duration_ms, uint32_t *rising_edges) {
    (void)duration_ms;
    if (rising_edges) *rising_edges = 0;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t display_clear_screen(void) {
    return ESP_OK;
}

void display_draw_pixel(int16_t x, int16_t y, uint16_t color) {
    (void)x;
    (void)y;
    (void)color;
}

esp_err_t display_flush(void) {
    return ESP_OK;
}

esp_err_t display_keepalive(void) {
    return ESP_OK;
}

uint16_t *display_begin_frame_line(int16_t y) {
    (void)y;
    return NULL;
}

esp_err_t display_flush_frame_line(int16_t y) {
    (void)y;
    return ESP_OK;
}

esp_err_t display_end_frame(void) {
    return ESP_OK;
}

uint16_t *display_begin_region_line(int16_t x, int16_t y, int16_t w, int16_t h) {
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    return NULL;
}

esp_err_t display_flush_region_line(void) {
    return ESP_OK;
}

esp_err_t display_end_region(void) {
    return ESP_OK;
}

esp_err_t display_write_region_pixels(int16_t x, int16_t y, int16_t w, int16_t h,
                                      const uint16_t *pixels, size_t pixel_count) {
    (void)x; (void)y; (void)w; (void)h; (void)pixels; (void)pixel_count;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t display_write_region_pixels_strided(
    int16_t x, int16_t y, int16_t w, int16_t h,
    const uint16_t *pixels, size_t source_stride_pixels) {
    (void)x; (void)y; (void)w; (void)h; (void)pixels;
    (void)source_stride_pixels;
    return ESP_ERR_NOT_SUPPORTED;
}

void *display_acquire_draw_buffer(size_t bytes) { (void)bytes; return NULL; }
bool display_release_draw_buffer(void *buffer) { (void)buffer; return false; }

esp_err_t display_write_region_pixels_swapped_inplace(
    int16_t x, int16_t y, int16_t w, int16_t h,
    uint16_t *pixels, size_t pixel_count) {
    (void)x; (void)y; (void)w; (void)h; (void)pixels; (void)pixel_count;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
