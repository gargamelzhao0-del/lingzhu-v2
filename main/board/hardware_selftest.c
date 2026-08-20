#include "board/hardware_selftest.h"

#include <string.h>

#include "board/board_drivers.h"
#include "board/board_gpio_inputs.h"
#include "board/esp_vocat_v1_0_pins.h"
#include "board/i2c_bus_manager.h"
#include "board/sd_card_driver.h"
#include "board/uart_driver.h"
#include "display.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "hw_accept";

#if CONFIG_NUWAX_HW_ACCEPTANCE_BASE_UART_LOOPBACK_ENABLE && CONFIG_ESP_CONSOLE_UART
#error "Base UART uses GPIO43/44; select USB Serial/JTAG-only console for loopback acceptance"
#endif

static esp_err_t first_error(esp_err_t current, esp_err_t next)
{
    return current == ESP_OK && next != ESP_OK ? next : current;
}

static esp_err_t platform_memory_test(void)
{
    esp_chip_info_t chip = {0};
    esp_chip_info(&chip);
    uint32_t flash_size = 0;
    esp_err_t err = esp_flash_get_size(NULL, &flash_size);
    if (err != ESP_OK) return err;
    const size_t psram_size = esp_psram_get_size();
    ESP_LOGI(TAG,
             "platform cores=%u revision=%u flash=%u psram=%u internal_free=%u psram_free=%u",
             chip.cores,
             chip.revision,
             (unsigned)flash_size,
             (unsigned)psram_size,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (flash_size < 16u * 1024u * 1024u || psram_size < 8u * 1024u * 1024u) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t expected_i2c_devices_test(void)
{
    typedef struct { const char *name; uint8_t address; } expected_t;
    static const expected_t expected[] = {
        {"ES8311", NUWAX_VOCAT_I2C_ADDR_ES8311},
        {"ES7210", NUWAX_VOCAT_I2C_ADDR_ES7210},
        {"BQ27220", NUWAX_VOCAT_I2C_ADDR_BQ27220},
    };
    static const uint8_t qmi_addresses[] = {
        NUWAX_VOCAT_I2C_ADDR_QMI8658A,
        NUWAX_VOCAT_I2C_ADDR_QMI8658A_ALT,
    };
    static const uint8_t touch_addresses[] = {
        NUWAX_VOCAT_I2C_ADDR_TOUCH_PRIMARY,
        NUWAX_VOCAT_I2C_ADDR_TOUCH_ALT_2A,
        NUWAX_VOCAT_I2C_ADDR_TOUCH_ALT_2D,
    };
    esp_err_t overall = ESP_OK;
    esp_err_t touch_result = ESP_ERR_NOT_FOUND;
    for (size_t i = 0; i < sizeof(touch_addresses) / sizeof(touch_addresses[0]); ++i) {
        const esp_err_t err = nuwax_i2c_bus_probe(touch_addresses[i], 100);
        ESP_LOGI(TAG,
                 "I2C touch candidate address=0x%02x result=%s",
                 touch_addresses[i], esp_err_to_name(err));
        if (err == ESP_OK) {
            touch_result = ESP_OK;
        }
    }
    overall = first_error(overall, touch_result);
    esp_err_t qmi_result = ESP_ERR_NOT_FOUND;
    for (size_t i = 0; i < sizeof(qmi_addresses) / sizeof(qmi_addresses[0]); ++i) {
        const esp_err_t err = nuwax_i2c_bus_probe(qmi_addresses[i], 100);
        ESP_LOGI(TAG,
                 "I2C QMI8658A candidate address=0x%02x result=%s",
                 qmi_addresses[i], esp_err_to_name(err));
        if (err == ESP_OK) {
            qmi_result = ESP_OK;
        }
    }
    overall = first_error(overall, qmi_result);
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        esp_err_t err = nuwax_i2c_bus_probe(expected[i].address, 100);
        ESP_LOGI(TAG,
                 "I2C expected name=%s address=0x%02x result=%s",
                 expected[i].name, expected[i].address, esp_err_to_name(err));
        overall = first_error(overall, err);
    }
    nuwax_i2c_bus_log_full_scan("full_hw_accept", 20);
    return overall;
}

static esp_err_t qmi_test(void)
{
    esp_err_t err = qmi8658a_init(NULL);
    if (err != ESP_OK) return err;
    qmi8658a_sensor_data_t sample = {0};
    for (int attempt = 0; attempt < 5; ++attempt) {
        err = qmi8658a_read_data(&sample);
        if (err == ESP_OK &&
            (sample.accel_x || sample.accel_y || sample.accel_z ||
             sample.gyro_x || sample.gyro_y || sample.gyro_z)) {
            ESP_LOGI(TAG,
                     "QMI8658A PASS raw_accel=%d/%d/%d raw_gyro=%d/%d/%d scale=%.6f/%.6f",
                     sample.accel_x, sample.accel_y, sample.accel_z,
                     sample.gyro_x, sample.gyro_y, sample.gyro_z,
                     (double)sample.accel_scale, (double)sample.gyro_scale);
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
}

static esp_err_t battery_test(void)
{
    const bq27220_config_t config = {
        .design_capacity = 1200,
        .low_battery_threshold = 10,
    };
    esp_err_t err = bq27220_init(&config);
    if (err != ESP_OK) return err;
    bq27220_battery_status_t status = {0};
    const int64_t ready_deadline_us = esp_timer_get_time() + 5000000;
    do {
        err = bq27220_get_status(&status);
        if (err != ESP_OK) return err;
        if (status.battery_present && status.initialization_complete) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    } while (esp_timer_get_time() < ready_deadline_us);
    ESP_LOGI(TAG,
             "BQ27220 sample voltage=%umV current=%dmA soc=%u%% remaining=%umAh full=%umAh design=%umAh temp=%dC cycles=%u battery_present=%d init_complete=%d cfgupdate=%d profile_verified=%d charging=%d full_flag=%d low=%d battery_flags=0x%04x operation=0x%04x",
             status.voltage_mv, status.current_ma, status.soc,
             status.remaining_mah, status.full_charge_mah, status.design_capacity_mah,
             status.temperature_c, status.cycle_count,
             status.battery_present, status.initialization_complete,
             status.config_update_mode, status.profile_verified,
             status.is_charging, status.is_full, status.is_low,
             status.battery_status_flags, status.operation_status_flags);
    if (status.voltage_mv < 2500 || status.voltage_mv > 5000 ||
        status.soc > 100 || status.temperature_c < -30 || status.temperature_c > 90 ||
        !status.battery_present || !status.initialization_complete ||
        status.config_update_mode ||
        status.design_capacity_mah < 100 || status.design_capacity_mah > 20000 ||
        status.full_charge_mah == 0 ||
        status.remaining_mah > status.full_charge_mah + 50) {
        ESP_LOGE(TAG, "BQ27220 basic profile/status validation failed");
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (!status.profile_verified) {
        ESP_LOGW(TAG,
                 "BQ27220 communication and basic values are plausible, but the cell-specific CEDV profile is not authenticated");
        return ESP_ERR_NOT_FINISHED;
    }
    return ESP_OK;
}

static esp_err_t led_visual_test(void)
{
    esp_err_t err = nuwax_led_init();
    if (err != ESP_OK) return err;
    ESP_LOGW(TAG, "VISUAL CHECK: green LED should turn on, blink, breathe, then turn off");
    if ((err = nuwax_led_on(LED_COLOR_GREEN)) != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(350));
    if ((err = nuwax_led_set_state(LED_COLOR_GREEN, LED_STATE_BLINK_FAST)) != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(800));
    if ((err = nuwax_led_start_breath(LED_COLOR_GREEN, 1000)) != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(1200));
    return nuwax_led_off(LED_COLOR_GREEN);
}

static esp_err_t display_backlight_command_test(void)
{
    ESP_LOGW(TAG, "VISUAL CHECK: display backlight should step 25%% -> 50%% -> 100%%");
    esp_err_t err = nuwax_display_backlight_set_percent(25);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(300));
    if ((err = nuwax_display_backlight_set_percent(50)) != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(300));
    return nuwax_display_backlight_set_percent(100);
}

static esp_err_t lcd_te_test(void)
{
#if CONFIG_NUWAX_HW_ACCEPTANCE_LCD_TE_ENABLE
    uint32_t edges = 0;
    esp_err_t err = nuwax_display_te_selftest(1000, &edges);
    if (err == ESP_OK && (edges < 30 || edges > 240)) {
        ESP_LOGW(TAG, "LCD TE pulse count outside expected display range: %u", (unsigned)edges);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return err;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t nuwax_hardware_selftest_run(nuwax_hardware_selftest_result_t *result)
{
    if (!result) return ESP_ERR_INVALID_ARG;
    memset(result, 0, sizeof(*result));
    ESP_LOGW(TAG, "========== MIAOPAN V2 HARDWARE DRIVER PRECHECK START ==========");

    /* Keep an unpopulated 4G rail off unless the explicit AT test is enabled. */
#if !CONFIG_NUWAX_HW_ACCEPTANCE_4G_AT_ENABLE
    (void)nuwax_4g_power_set(false);
#endif

    result->platform_memory = platform_memory_test();
    result->i2c_expected_devices = expected_i2c_devices_test();
    result->qmi8658a = qmi_test();
    result->bq27220 = battery_test();
    if (result->bq27220 == ESP_ERR_NOT_FINISHED) {
        result->fixture_pending_mask |= NUWAX_HW_FIXTURE_BATTERY_PROFILE;
        ESP_LOGW(TAG,
                 "BQ27220 profile/cell capacity is not authenticated; classified as configuration pending, no Data Memory was written");
    }
    result->green_led = led_visual_test();
    result->gpio_inputs = nuwax_board_gpio_inputs_init(NULL, NULL);
    if (result->gpio_inputs == ESP_OK) {
        ESP_LOGW(TAG, "INTERACTIVE CHECK: touch the head sensor and press GPIO0; transitions must be logged");
    }
    result->display_backlight = display_backlight_command_test();
    result->lcd_te = lcd_te_test();

#if CONFIG_NUWAX_HW_ACCEPTANCE_SD_ENABLE && CONFIG_NUWAX_HW_ACCEPTANCE_SD_CARD_INSERTED
    nuwax_sd_card_info_t sd_info = {0};
    result->sd_card = nuwax_sd_card_mount(&sd_info);
    if (result->sd_card == ESP_OK) result->sd_card = nuwax_sd_card_read_write_selftest();
    (void)nuwax_sd_card_unmount();
#else
    result->sd_card = ESP_ERR_NOT_FINISHED;
    result->fixture_pending_mask |= NUWAX_HW_FIXTURE_SD_CARD;
    ESP_LOGW(TAG,
             "SD validation skipped: no card-detect input and inserted FAT card was not confirmed");
#endif

#if CONFIG_NUWAX_HW_ACCEPTANCE_BASE_UART_LOOPBACK_ENABLE
    result->base_uart = nuwax_uart_loopback_selftest(NUWAX_UART_BASE, 300);
    (void)nuwax_uart_deinit(NUWAX_UART_BASE);
#else
    result->base_uart = ESP_ERR_NOT_SUPPORTED;
    result->fixture_pending_mask |= NUWAX_HW_FIXTURE_BASE_UART;
#endif

#if CONFIG_NUWAX_HW_ACCEPTANCE_4G_AT_ENABLE
    result->modem_4g = nuwax_4g_at_probe(2000, 1500);
#else
    result->modem_4g = ESP_ERR_NOT_SUPPORTED;
    result->fixture_pending_mask |= NUWAX_HW_FIXTURE_4G_MODEM;
#endif

    result->manual_pending_mask =
        NUWAX_HW_MANUAL_LED_VISUAL |
        NUWAX_HW_MANUAL_BACKLIGHT_VISUAL |
        NUWAX_HW_MANUAL_HEAD_TOUCH |
        NUWAX_HW_MANUAL_BOOT_BUTTON |
        NUWAX_HW_MANUAL_QMI_MOTION |
        NUWAX_HW_MANUAL_DISPLAY_PIXELS |
        NUWAX_HW_MANUAL_TOUCH_GESTURES |
        NUWAX_HW_MANUAL_POWER_KEY_LATCH |
        NUWAX_HW_MANUAL_AUDIO_CHAIN |
        NUWAX_HW_MANUAL_USB_SERIAL |
        NUWAX_HW_MANUAL_CHARGER_STATUS |
        NUWAX_HW_MANUAL_POWER_RAILS;
#if !CONFIG_NUWAX_HW_ACCEPTANCE_LCD_TE_ENABLE
    result->manual_pending_mask |= NUWAX_HW_MANUAL_LCD_TE;
#endif

    esp_err_t overall = ESP_OK;
    overall = first_error(overall, result->platform_memory);
    overall = first_error(overall, result->i2c_expected_devices);
    overall = first_error(overall, result->qmi8658a);
    if (result->bq27220 != ESP_ERR_NOT_FINISHED) {
        overall = first_error(overall, result->bq27220);
    }
    overall = first_error(overall, result->green_led);
    overall = first_error(overall, result->gpio_inputs);
    overall = first_error(overall, result->display_backlight);
    if (result->lcd_te != ESP_ERR_NOT_SUPPORTED) {
        overall = first_error(overall, result->lcd_te);
    }
    if (result->sd_card != ESP_ERR_NOT_SUPPORTED &&
        result->sd_card != ESP_ERR_NOT_FINISHED) {
        overall = first_error(overall, result->sd_card);
    }
    if (result->base_uart != ESP_ERR_NOT_SUPPORTED) {
        overall = first_error(overall, result->base_uart);
    }
    if (result->modem_4g != ESP_ERR_NOT_SUPPORTED) {
        overall = first_error(overall, result->modem_4g);
    }
    result->automated_result = overall;
    ESP_LOGW(TAG,
             "HW-PRECHECK-SUMMARY automated=%s platform=%s i2c=%s qmi=%s bq=%s led_command=%s inputs_init=%s backlight_command=%s lcd_te=%s sd=%s base_uart=%s 4g=%s manual_pending=0x%08x fixture_pending=0x%08x",
             esp_err_to_name(overall),
             esp_err_to_name(result->platform_memory),
             esp_err_to_name(result->i2c_expected_devices),
             esp_err_to_name(result->qmi8658a),
             esp_err_to_name(result->bq27220),
             esp_err_to_name(result->green_led),
             esp_err_to_name(result->gpio_inputs),
             esp_err_to_name(result->display_backlight),
             esp_err_to_name(result->lcd_te),
             esp_err_to_name(result->sd_card),
             esp_err_to_name(result->base_uart),
             esp_err_to_name(result->modem_4g),
             (unsigned)result->manual_pending_mask,
             (unsigned)result->fixture_pending_mask);
    ESP_LOGW(TAG,
             "PRECHECK IS NOT FINAL ACCEPTANCE: manual observations, P2 audio/touch, power cycle, USB, base UART fixture and 4G fixture remain separate evidence");
    ESP_LOGW(TAG, "========== MIAOPAN V2 HARDWARE DRIVER PRECHECK END ==========");
    if (overall != ESP_OK) return overall;
    return result->manual_pending_mask || result->fixture_pending_mask
               ? ESP_ERR_NOT_FINISHED
               : ESP_OK;
}
