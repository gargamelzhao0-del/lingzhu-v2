#include "board/uart_driver.h"

#include <string.h>

#include "board/esp_vocat_v1_0_pins.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board_uart";

typedef struct {
    uart_port_t port;
    int tx_gpio;
    int rx_gpio;
    int baud;
    const char *name;
    bool initialized;
} uart_endpoint_config_t;

static uart_endpoint_config_t s_uart[NUWAX_UART_ENDPOINT_COUNT] = {
    [NUWAX_UART_BASE] = {
        .port = UART_NUM_1,
        .tx_gpio = NUWAX_VOCAT_UART1_TX_GPIO,
        .rx_gpio = NUWAX_VOCAT_UART1_RX_GPIO,
        .baud = 115200,
        .name = "base",
    },
    [NUWAX_UART_4G] = {
        .port = UART_NUM_2,
        .tx_gpio = NUWAX_VOCAT_4G_UART_TX_GPIO,
        .rx_gpio = NUWAX_VOCAT_4G_UART_RX_GPIO,
        .baud = 921600,
        .name = "4g",
    },
};
static bool s_4g_power_known;
static bool s_4g_power_enabled;

static bool endpoint_valid(nuwax_uart_endpoint_t endpoint)
{
    return endpoint >= 0 && endpoint < NUWAX_UART_ENDPOINT_COUNT;
}

esp_err_t nuwax_uart_init(nuwax_uart_endpoint_t endpoint)
{
    if (!endpoint_valid(endpoint)) return ESP_ERR_INVALID_ARG;
    uart_endpoint_config_t *cfg = &s_uart[endpoint];
    if (cfg->initialized) return ESP_OK;

    const uart_config_t uart_config = {
        .baud_rate = cfg->baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_param_config(cfg->port, &uart_config);
    if (err != ESP_OK) return err;
    err = uart_set_pin(cfg->port, cfg->tx_gpio, cfg->rx_gpio,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) return err;
    err = uart_driver_install(cfg->port, 2048, 0, 0, NULL, 0);
    if (err != ESP_OK) return err;
    cfg->initialized = true;
    ESP_LOGI(TAG,
             "%s ready uart=%d tx=%d rx=%d baud=%d",
             cfg->name, cfg->port, cfg->tx_gpio, cfg->rx_gpio, cfg->baud);
    return ESP_OK;
}

esp_err_t nuwax_uart_write(nuwax_uart_endpoint_t endpoint,
                           const void *data,
                           size_t length)
{
    if (!endpoint_valid(endpoint) || !data || length == 0) return ESP_ERR_INVALID_ARG;
    uart_endpoint_config_t *cfg = &s_uart[endpoint];
    if (!cfg->initialized) return ESP_ERR_INVALID_STATE;
    int written = uart_write_bytes(cfg->port, data, length);
    if (written < 0) return ESP_FAIL;
    if ((size_t)written != length) return ESP_ERR_INVALID_SIZE;
    return uart_wait_tx_done(cfg->port, pdMS_TO_TICKS(250));
}

int nuwax_uart_read(nuwax_uart_endpoint_t endpoint,
                    void *data,
                    size_t length,
                    uint32_t timeout_ms)
{
    if (!endpoint_valid(endpoint) || !data || length == 0) return -1;
    uart_endpoint_config_t *cfg = &s_uart[endpoint];
    if (!cfg->initialized) return -1;
    return uart_read_bytes(cfg->port, data, length, pdMS_TO_TICKS(timeout_ms));
}

esp_err_t nuwax_uart_loopback_selftest(nuwax_uart_endpoint_t endpoint,
                                       uint32_t timeout_ms)
{
    if (!endpoint_valid(endpoint)) return ESP_ERR_INVALID_ARG;
    esp_err_t err = nuwax_uart_init(endpoint);
    if (err != ESP_OK) return err;
    uart_endpoint_config_t *cfg = &s_uart[endpoint];
    (void)uart_flush_input(cfg->port);
    static const uint8_t pattern[] = {
        0x4e, 0x55, 0x57, 0x41, 0x58, 0x2d, 0xa5, 0x5a, 0x00, 0xff,
    };
    err = nuwax_uart_write(endpoint, pattern, sizeof(pattern));
    if (err != ESP_OK) return err;
    uint8_t received[sizeof(pattern)] = {0};
    size_t total = 0;
    while (total < sizeof(received)) {
        int count = nuwax_uart_read(endpoint,
                                    received + total,
                                    sizeof(received) - total,
                                    timeout_ms);
        if (count <= 0) break;
        total += (size_t)count;
    }
    if (total != sizeof(pattern) || memcmp(received, pattern, sizeof(pattern)) != 0) {
        ESP_LOGW(TAG,
                 "%s loopback requires external TX-RX fixture; received=%u/%u",
                 cfg->name, (unsigned)total, (unsigned)sizeof(pattern));
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "%s external loopback PASS", cfg->name);
    return ESP_OK;
}

esp_err_t nuwax_uart_deinit(nuwax_uart_endpoint_t endpoint)
{
    if (!endpoint_valid(endpoint)) return ESP_ERR_INVALID_ARG;
    uart_endpoint_config_t *cfg = &s_uart[endpoint];
    if (!cfg->initialized) return ESP_OK;
    esp_err_t err = uart_driver_delete(cfg->port);
    if (err == ESP_OK) cfg->initialized = false;
    return err;
}

esp_err_t nuwax_4g_power_set(bool enabled)
{
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << NUWAX_VOCAT_4G_ENABLE_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) return err;
    err = gpio_set_level(NUWAX_VOCAT_4G_ENABLE_GPIO, enabled ? 1 : 0);
    if (err == ESP_OK) {
        s_4g_power_known = true;
        s_4g_power_enabled = enabled;
        ESP_LOGI(TAG, "4g rail enable gpio=%d state=%d", NUWAX_VOCAT_4G_ENABLE_GPIO, enabled);
    }
    return err;
}

esp_err_t nuwax_4g_at_probe(uint32_t power_settle_ms, uint32_t response_timeout_ms)
{
    const bool restore_power_on = s_4g_power_known && s_4g_power_enabled;
    esp_err_t err = nuwax_4g_power_set(true);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(power_settle_ms));
    err = nuwax_uart_init(NUWAX_UART_4G);
    if (err != ESP_OK) goto done;

    uart_endpoint_config_t *cfg = &s_uart[NUWAX_UART_4G];
    static const int baud_candidates[] = {921600, 115200, 460800};
    static const char command[] = "AT\r\n";
    err = ESP_ERR_NOT_FOUND;
    for (size_t baud_index = 0;
         baud_index < sizeof(baud_candidates) / sizeof(baud_candidates[0]);
         ++baud_index) {
        const int baud = baud_candidates[baud_index];
        esp_err_t baud_err = uart_set_baudrate(cfg->port, baud);
        if (baud_err != ESP_OK) {
            err = baud_err;
            break;
        }
        (void)uart_flush_input(cfg->port);
        if (nuwax_uart_write(NUWAX_UART_4G, command, sizeof(command) - 1) != ESP_OK) {
            continue;
        }

        char response[256] = {0};
        size_t total = 0;
        const int64_t deadline_us = esp_timer_get_time() +
                                    (int64_t)response_timeout_ms * 1000;
        while (total < sizeof(response) - 1 && esp_timer_get_time() < deadline_us) {
            int count = nuwax_uart_read(NUWAX_UART_4G,
                                        response + total,
                                        sizeof(response) - 1 - total,
                                        50);
            if (count > 0) {
                total += (size_t)count;
                response[total] = '\0';
                if (strstr(response, "OK") || strstr(response, "ERROR")) break;
            }
        }
        if (strstr(response, "OK")) {
            cfg->baud = baud;
            ESP_LOGI(TAG, "4g AT probe PASS baud=%d response=%s", baud, response);
            err = ESP_OK;
            break;
        }
        ESP_LOGW(TAG,
                 "4g AT probe no OK baud=%d bytes=%u response=%s",
                 baud, (unsigned)total, total ? response : "<none>");
    }

done:
    (void)nuwax_uart_deinit(NUWAX_UART_4G);
    if (!restore_power_on) {
        (void)nuwax_4g_power_set(false);
    }
    return err;
}
