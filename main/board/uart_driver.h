#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    NUWAX_UART_BASE = 0,
    NUWAX_UART_4G,
    NUWAX_UART_ENDPOINT_COUNT,
} nuwax_uart_endpoint_t;

esp_err_t nuwax_uart_init(nuwax_uart_endpoint_t endpoint);
esp_err_t nuwax_uart_write(nuwax_uart_endpoint_t endpoint,
                           const void *data,
                           size_t length);
int nuwax_uart_read(nuwax_uart_endpoint_t endpoint,
                    void *data,
                    size_t length,
                    uint32_t timeout_ms);
esp_err_t nuwax_uart_loopback_selftest(nuwax_uart_endpoint_t endpoint,
                                       uint32_t timeout_ms);
esp_err_t nuwax_uart_deinit(nuwax_uart_endpoint_t endpoint);

esp_err_t nuwax_4g_power_set(bool enabled);
esp_err_t nuwax_4g_at_probe(uint32_t power_settle_ms, uint32_t response_timeout_ms);
