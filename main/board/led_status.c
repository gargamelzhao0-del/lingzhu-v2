#include "board/led_status.h"

#include <stdatomic.h>

#include "board/board_drivers.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static atomic_int s_status = ATOMIC_VAR_INIT(NUWAX_LED_STATUS_OFF);
static bool s_sleeping_requested;
static bool s_audio_rx_requested;
static bool s_charging_requested;
static nuwax_led_status_t s_base_status = NUWAX_LED_STATUS_OFF;
static uint32_t s_sleep_breath_period_ms = 2000U;
static bool s_initialized;
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_storage;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;

static esp_err_t ensure_lock(void)
{
    portENTER_CRITICAL(&s_init_mux);
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    }
    const bool ready = s_lock != NULL;
    portEXIT_CRITICAL(&s_init_mux);
    return ready ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t apply_status(nuwax_led_status_t status)
{
    switch (status) {
    case NUWAX_LED_STATUS_OFF:
        return nuwax_led_off(LED_COLOR_GREEN);
    case NUWAX_LED_STATUS_SLEEPING:
        return nuwax_led_start_breath(LED_COLOR_GREEN,
                                      s_sleep_breath_period_ms);
    case NUWAX_LED_STATUS_AUDIO_RX:
        return nuwax_led_set_state(LED_COLOR_GREEN, LED_STATE_BLINK_FAST);
    case NUWAX_LED_STATUS_ERROR:
        return nuwax_led_set_state(LED_COLOR_GREEN, LED_STATE_BLINK_SLOW);
    case NUWAX_LED_STATUS_PROVISIONING:
        return nuwax_led_set_state(LED_COLOR_GREEN, LED_STATE_BLINK_FAST);
    case NUWAX_LED_STATUS_CHARGING:
        return nuwax_led_start_breath(LED_COLOR_GREEN, 1200U);
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

static nuwax_led_status_t resolve_status_locked(void)
{
    /* Charging must neither erase other requests nor hide an error. */
    if (s_base_status == NUWAX_LED_STATUS_ERROR ||
        s_base_status == NUWAX_LED_STATUS_PROVISIONING) {
        return s_base_status;
    }
    if (s_audio_rx_requested) {
        return NUWAX_LED_STATUS_AUDIO_RX;
    }
    if (s_charging_requested) {
        return NUWAX_LED_STATUS_CHARGING;
    }
    if (s_sleeping_requested) {
        return NUWAX_LED_STATUS_SLEEPING;
    }
    return s_base_status;
}

static esp_err_t apply_resolved_status_locked(void)
{
    const nuwax_led_status_t resolved = resolve_status_locked();
    if (resolved == (nuwax_led_status_t)atomic_load_explicit(
                        &s_status, memory_order_acquire)) {
        return ESP_OK;
    }
    const esp_err_t err = apply_status(resolved);
    if (err == ESP_OK) {
        atomic_store_explicit(&s_status, resolved, memory_order_release);
    }
    return err;
}

esp_err_t nuwax_led_status_init(const nuwax_led_status_config_t *config)
{
    const nuwax_led_status_config_t resolved =
        config ? *config : NUWAX_LED_STATUS_DEFAULT_CONFIG();
    if (resolved.sleep_breath_period_ms < 200U ||
        resolved.sleep_breath_period_ms > 10000U) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ensure_lock();
    if (err != ESP_OK) {
        return err;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_initialized) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    err = nuwax_led_init();
    if (err != ESP_OK) {
        xSemaphoreGive(s_lock);
        return err;
    }
    s_sleep_breath_period_ms = resolved.sleep_breath_period_ms;
    err = apply_status(NUWAX_LED_STATUS_OFF);
    if (err == ESP_OK) {
        atomic_store_explicit(&s_status, NUWAX_LED_STATUS_OFF,
                              memory_order_release);
        s_base_status = NUWAX_LED_STATUS_OFF;
        s_sleeping_requested = false;
        s_audio_rx_requested = false;
        s_charging_requested = false;
        s_initialized = true;
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t nuwax_led_status_set(nuwax_led_status_t status)
{
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (status < NUWAX_LED_STATUS_OFF || status > NUWAX_LED_STATUS_CHARGING) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_initialized) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (status == NUWAX_LED_STATUS_SLEEPING) {
        s_sleeping_requested = true;
        s_audio_rx_requested = false;
        s_base_status = NUWAX_LED_STATUS_OFF;
    } else if (status == NUWAX_LED_STATUS_AUDIO_RX) {
        s_audio_rx_requested = true;
    } else {
        s_sleeping_requested = false;
        s_audio_rx_requested = false;
        s_base_status = status;
    }
    const esp_err_t err = apply_resolved_status_locked();
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t nuwax_led_status_set_sleeping(bool sleeping)
{
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_initialized) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_sleeping_requested = sleeping;
    const esp_err_t err = apply_resolved_status_locked();
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t nuwax_led_status_set_audio_rx(bool active)
{
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_initialized) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_audio_rx_requested = active;
    const esp_err_t err = apply_resolved_status_locked();
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t nuwax_led_status_set_charging(bool active)
{
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_initialized) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_charging_requested = active;
    const esp_err_t err = apply_resolved_status_locked();
    xSemaphoreGive(s_lock);
    return err;
}

nuwax_led_status_t nuwax_led_status_get(void)
{
    return (nuwax_led_status_t)atomic_load_explicit(&s_status,
                                                     memory_order_acquire);
}

esp_err_t nuwax_led_status_deinit(void)
{
    if (!s_lock) {
        return ESP_OK;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_initialized) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    const esp_err_t err = nuwax_led_deinit();
    if (err == ESP_OK) {
        s_initialized = false;
        atomic_store_explicit(&s_status, NUWAX_LED_STATUS_OFF,
                              memory_order_release);
        s_base_status = NUWAX_LED_STATUS_OFF;
        s_sleeping_requested = false;
        s_audio_rx_requested = false;
        s_charging_requested = false;
    }
    xSemaphoreGive(s_lock);
    return err;
}
