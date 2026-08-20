#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    NUWAX_LED_STATUS_OFF = 0,
    NUWAX_LED_STATUS_SLEEPING,
    NUWAX_LED_STATUS_AUDIO_RX,
    NUWAX_LED_STATUS_ERROR,
    NUWAX_LED_STATUS_PROVISIONING,
    NUWAX_LED_STATUS_CHARGING,
} nuwax_led_status_t;

typedef struct {
    uint32_t sleep_breath_period_ms;
} nuwax_led_status_config_t;

#define NUWAX_LED_STATUS_DEFAULT_CONFIG() \
    ((nuwax_led_status_config_t){         \
        .sleep_breath_period_ms = 2000U, \
    })

/* Policy layer over led_driver. Once initialized it must be the sole product
 * owner of the green LED; direct nuwax_led_* calls are reserved for isolated
 * hardware tests. It deliberately does not subscribe to the application state
 * machine yet, so callers can bind it later without changing the driver ABI. */
esp_err_t nuwax_led_status_init(const nuwax_led_status_config_t *config);
esp_err_t nuwax_led_status_set(nuwax_led_status_t status);
esp_err_t nuwax_led_status_set_sleeping(bool sleeping);
esp_err_t nuwax_led_status_set_audio_rx(bool active);
nuwax_led_status_t nuwax_led_status_get(void);
esp_err_t nuwax_led_status_deinit(void);
