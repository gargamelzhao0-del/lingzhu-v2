#pragma once

#include <stdint.h>

#include "esp_err.h"

enum {
    NUWAX_HW_MANUAL_LED_VISUAL       = 1u << 0,
    NUWAX_HW_MANUAL_BACKLIGHT_VISUAL = 1u << 1,
    NUWAX_HW_MANUAL_HEAD_TOUCH       = 1u << 2,
    NUWAX_HW_MANUAL_BOOT_BUTTON      = 1u << 3,
    NUWAX_HW_MANUAL_QMI_MOTION       = 1u << 4,
    NUWAX_HW_MANUAL_DISPLAY_PIXELS   = 1u << 5,
    NUWAX_HW_MANUAL_TOUCH_GESTURES   = 1u << 6,
    NUWAX_HW_MANUAL_POWER_KEY_LATCH  = 1u << 7,
    NUWAX_HW_MANUAL_AUDIO_CHAIN      = 1u << 8,
    NUWAX_HW_MANUAL_USB_SERIAL       = 1u << 9,
    NUWAX_HW_MANUAL_CHARGER_STATUS   = 1u << 10,
    NUWAX_HW_MANUAL_POWER_RAILS      = 1u << 11,
    NUWAX_HW_MANUAL_LCD_TE           = 1u << 12,
};

enum {
    NUWAX_HW_FIXTURE_SD_CARD   = 1u << 0,
    NUWAX_HW_FIXTURE_BASE_UART = 1u << 1,
    NUWAX_HW_FIXTURE_4G_MODEM  = 1u << 2,
    NUWAX_HW_FIXTURE_BATTERY_PROFILE = 1u << 3,
};

typedef struct {
    esp_err_t platform_memory;
    esp_err_t i2c_expected_devices;
    esp_err_t qmi8658a;
    esp_err_t bq27220;
    esp_err_t green_led;
    esp_err_t gpio_inputs;
    esp_err_t display_backlight;
    esp_err_t lcd_te;
    esp_err_t sd_card;
    esp_err_t base_uart;
    esp_err_t modem_4g;
    esp_err_t automated_result;
    uint32_t manual_pending_mask;
    uint32_t fixture_pending_mask;
} nuwax_hardware_selftest_result_t;

esp_err_t nuwax_hardware_selftest_run(nuwax_hardware_selftest_result_t *result);
