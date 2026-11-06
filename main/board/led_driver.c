#include "board_drivers.h"

#include "board/esp_vocat_v1_0_pins.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "nuwax_led";

#define LED_TIMER              LEDC_TIMER_0
#define LED_CHANNEL            LEDC_CHANNEL_0
#define LED_MODE               LEDC_LOW_SPEED_MODE
#define LED_RESOLUTION         LEDC_TIMER_8_BIT
#define LED_PWM_HZ             1000
#define LED_DUTY_MAX           255u
#define LED_PWM_PERIOD_TICKS   256u
#define LED_DEFAULT_BREATH_MS  2000u

typedef struct {
    SemaphoreHandle_t lock;
    SemaphoreHandle_t stopped;
    TaskHandle_t task;
    led_state_t state;
    uint32_t breath_period_ms;
    bool initialized;
    bool stop_requested;
    esp_err_t last_hw_error;
} led_context_t;

static led_context_t s_led;

static esp_err_t set_brightness(uint8_t brightness)
{
    /* GPIO18 sinks current. OFF must be a constant high level: an 8-bit
     * duty of 255 still has one low tick in every 256 and can visibly glow.
     * update_duty below re-enables output when leaving either static level. */
    if (brightness == 0U) {
        return ledc_stop(LED_MODE, LED_CHANNEL, 1U);
    }
    if (brightness == LED_DUTY_MAX) {
        return ledc_stop(LED_MODE, LED_CHANNEL, 0U);
    }
    const uint32_t hardware_duty = LED_PWM_PERIOD_TICKS -
        ((uint32_t)brightness * LED_PWM_PERIOD_TICKS / LED_DUTY_MAX);
    esp_err_t err = ledc_set_duty(LED_MODE, LED_CHANNEL, hardware_duty);
    if (err != ESP_OK) {
        return err;
    }
    return ledc_update_duty(LED_MODE, LED_CHANNEL);
}

static esp_err_t apply_state(led_state_t state, uint32_t breath_ms, uint32_t now_ms)
{
    switch (state) {
    case LED_STATE_OFF:
        return set_brightness(0);
    case LED_STATE_ON:
        return set_brightness(255);
    case LED_STATE_BLINK_SLOW: {
        const uint32_t period = 1000;
        return set_brightness((now_ms % period) < (period / 2) ? 255 : 0);
    }
    case LED_STATE_BLINK_FAST: {
        const uint32_t period = 250;
        return set_brightness((now_ms % period) < (period / 2) ? 255 : 0);
    }
    case LED_STATE_BREATH: {
        const uint32_t period = breath_ms >= 200 ? breath_ms : LED_DEFAULT_BREATH_MS;
        const uint32_t phase = now_ms % period;
        const uint32_t half = period / 2;
        const uint32_t linear = phase <= half
                                    ? (phase * 255u) / half
                                    : ((period - phase) * 255u) / half;
        const uint8_t gamma = (uint8_t)((linear * linear + 127u) / 255u);
        return set_brightness(gamma);
    }
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

static void led_task(void *arg)
{
    (void)arg;
    bool resource_logged = false;
    bool output_logged = false;
    led_state_t last_logged_state = LED_STATE_OFF;
    uint32_t last_output_log_ms = 0;
    while (true) {
        xSemaphoreTake(s_led.lock, portMAX_DELAY);
        const bool stop = s_led.stop_requested;
        if (stop) {
            xSemaphoreGive(s_led.lock);
            break;
        }

        uint32_t wait_ms = 20;
        const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        switch (s_led.state) {
        case LED_STATE_OFF:
            wait_ms = 1000;
            break;
        case LED_STATE_ON:
            wait_ms = 1000;
            break;
        case LED_STATE_BLINK_SLOW:
            break;
        case LED_STATE_BLINK_FAST:
            break;
        case LED_STATE_BREATH:
            break;
        default:
            break;
        }
        const esp_err_t err = apply_state(s_led.state, s_led.breath_period_ms, now_ms);
        if (err != s_led.last_hw_error && err != ESP_OK) {
            ESP_LOGE(TAG, "PWM update failed: %s", esp_err_to_name(err));
        }
        s_led.last_hw_error = err;
        if (!output_logged || last_logged_state != s_led.state ||
            (uint32_t)(now_ms - last_output_log_ms) >= 30000U) {
            /* Input sensing is enabled without changing the LEDC output mux.
             * Duty register alone does not describe a stopped channel. Read
             * the pad to distinguish logical OFF from a physical low level.
             * An instantaneous read during PWM is not a brightness measure. */
            const int expected_static = s_led.state == LED_STATE_OFF ? 1 :
                                        s_led.state == LED_STATE_ON ? 0 : -1;
            ESP_LOGI(TAG,
                     "output check state=%d gpio=%d level=%d expected_static=%d duty_reg=%lu freq_hz=%lu err=%s",
                     (int)s_led.state, NUWAX_VOCAT_LED_G_GPIO,
                     gpio_get_level(NUWAX_VOCAT_LED_G_GPIO), expected_static,
                     (unsigned long)ledc_get_duty(LED_MODE, LED_CHANNEL),
                     (unsigned long)ledc_get_freq(LED_MODE, LED_TIMER),
                     esp_err_to_name(err));
            last_logged_state = s_led.state;
            last_output_log_ms = now_ms;
            output_logged = true;
        }
        xSemaphoreGive(s_led.lock);
        if (!resource_logged) {
            resource_logged = true;
            ESP_LOGI(TAG,
                     "task resources stack=psram bytes=2048 stack_min_free=%u",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
    }

    xSemaphoreTake(s_led.lock, portMAX_DELAY);
    s_led.last_hw_error = set_brightness(0);
    if (s_led.last_hw_error != ESP_OK) {
        ESP_LOGE(TAG, "final PWM off failed: %s", esp_err_to_name(s_led.last_hw_error));
    }
    xSemaphoreGive(s_led.lock);
    ESP_LOGI(TAG, "stopped task_stack=psram stack_min_free=%u",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    xSemaphoreGive(s_led.stopped);
    vTaskSuspend(NULL);
}

esp_err_t nuwax_led_init(void)
{
    if (s_led.initialized) return ESP_OK;
    s_led.lock = xSemaphoreCreateMutex();
    s_led.stopped = xSemaphoreCreateBinary();
    if (!s_led.lock || !s_led.stopped) {
        if (s_led.lock) vSemaphoreDelete(s_led.lock);
        if (s_led.stopped) vSemaphoreDelete(s_led.stopped);
        s_led.lock = NULL;
        s_led.stopped = NULL;
        return ESP_ERR_NO_MEM;
    }

    const ledc_timer_config_t timer = {
        .speed_mode = LED_MODE,
        .duty_resolution = LED_RESOLUTION,
        .timer_num = LED_TIMER,
        .freq_hz = LED_PWM_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) goto fail;
    const ledc_channel_config_t channel = {
        .gpio_num = NUWAX_VOCAT_LED_G_GPIO,
        .speed_mode = LED_MODE,
        .channel = LED_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LED_TIMER,
        .duty = LED_DUTY_MAX,
        .hpoint = 0,
    };
    err = ledc_channel_config(&channel);
    if (err != ESP_OK) goto fail;
    err = set_brightness(0);
    if (err != ESP_OK) goto fail;
    err = gpio_input_enable(NUWAX_VOCAT_LED_G_GPIO);
    if (err != ESP_OK) goto fail;

    s_led.state = LED_STATE_OFF;
    s_led.breath_period_ms = LED_DEFAULT_BREATH_MS;
    s_led.stop_requested = false;
    s_led.last_hw_error = ESP_OK;
    s_led.initialized = true;
    if (xTaskCreateWithCaps(led_task, "nuwax_led", 2048, NULL, 3,
                            &s_led.task,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_led.initialized = false;
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    ESP_LOGI(TAG, "ready gpio=%d active_low=1 task_stack=psram bytes=2048 revision=static-level-v1 off=high on=low",
             NUWAX_VOCAT_LED_G_GPIO);
    return ESP_OK;

fail:
    {
        const esp_err_t stop_err = ledc_stop(LED_MODE, LED_CHANNEL, 1);
        if (stop_err != ESP_OK) {
            ESP_LOGW(TAG, "PWM cleanup failed after init error: %s", esp_err_to_name(stop_err));
        }
    }
    vSemaphoreDelete(s_led.lock);
    vSemaphoreDelete(s_led.stopped);
    s_led.lock = NULL;
    s_led.stopped = NULL;
    return err;
}

esp_err_t nuwax_led_set_state(led_color_t color, led_state_t state)
{
    if (color != LED_COLOR_GREEN || state < LED_STATE_OFF || state > LED_STATE_BREATH) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_led.initialized || !s_led.lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_led.lock, portMAX_DELAY);
    if (s_led.stop_requested) {
        xSemaphoreGive(s_led.lock);
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = apply_state(state,
                                      s_led.breath_period_ms,
                                      (uint32_t)(esp_timer_get_time() / 1000));
    s_led.last_hw_error = err;
    if (err != ESP_OK) {
        xSemaphoreGive(s_led.lock);
        return err;
    }
    s_led.state = state;
    TaskHandle_t task = s_led.task;
    xSemaphoreGive(s_led.lock);
    if (task) xTaskNotifyGive(task);
    return ESP_OK;
}

esp_err_t nuwax_led_on(led_color_t color)
{
    return nuwax_led_set_state(color, LED_STATE_ON);
}

esp_err_t nuwax_led_off(led_color_t color)
{
    return nuwax_led_set_state(color, LED_STATE_OFF);
}

esp_err_t nuwax_led_toggle(led_color_t color)
{
    if (color != LED_COLOR_GREEN) return ESP_ERR_INVALID_ARG;
    if (!s_led.initialized || !s_led.lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_led.lock, portMAX_DELAY);
    if (s_led.stop_requested) {
        xSemaphoreGive(s_led.lock);
        return ESP_ERR_INVALID_STATE;
    }
    const led_state_t state = s_led.state == LED_STATE_ON ? LED_STATE_OFF : LED_STATE_ON;
    const esp_err_t err = apply_state(state,
                                      s_led.breath_period_ms,
                                      (uint32_t)(esp_timer_get_time() / 1000));
    s_led.last_hw_error = err;
    if (err != ESP_OK) {
        xSemaphoreGive(s_led.lock);
        return err;
    }
    s_led.state = state;
    TaskHandle_t task = s_led.task;
    xSemaphoreGive(s_led.lock);
    if (task) xTaskNotifyGive(task);
    return ESP_OK;
}

esp_err_t nuwax_led_start_breath(led_color_t color, uint32_t period_ms)
{
    if (color != LED_COLOR_GREEN || period_ms < 200) return ESP_ERR_INVALID_ARG;
    if (!s_led.initialized || !s_led.lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_led.lock, portMAX_DELAY);
    if (s_led.stop_requested) {
        xSemaphoreGive(s_led.lock);
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = apply_state(LED_STATE_BREATH,
                                      period_ms,
                                      (uint32_t)(esp_timer_get_time() / 1000));
    s_led.last_hw_error = err;
    if (err != ESP_OK) {
        xSemaphoreGive(s_led.lock);
        return err;
    }
    s_led.breath_period_ms = period_ms;
    s_led.state = LED_STATE_BREATH;
    TaskHandle_t task = s_led.task;
    xSemaphoreGive(s_led.lock);
    if (task) xTaskNotifyGive(task);
    return ESP_OK;
}

esp_err_t nuwax_led_stop_breath(led_color_t color)
{
    return nuwax_led_set_state(color, LED_STATE_OFF);
}

esp_err_t nuwax_led_deinit(void)
{
    if (!s_led.initialized || !s_led.lock) return ESP_OK;
    xSemaphoreTake(s_led.lock, portMAX_DELAY);
    s_led.stop_requested = true;
    TaskHandle_t task = s_led.task;
    xSemaphoreGive(s_led.lock);
    if (task) xTaskNotifyGive(task);
    if (xSemaphoreTake(s_led.stopped, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (task) {
        vTaskDeleteWithCaps(task);
    }
    esp_err_t result = s_led.last_hw_error;
    const esp_err_t stop_err = ledc_stop(LED_MODE, LED_CHANNEL, 1);
    if (result == ESP_OK) result = stop_err;
    const esp_err_t timer_err = ledc_timer_rst(LED_MODE, LED_TIMER);
    if (result == ESP_OK) result = timer_err;
    vSemaphoreDelete(s_led.lock);
    vSemaphoreDelete(s_led.stopped);
    s_led.lock = NULL;
    s_led.stopped = NULL;
    s_led.task = NULL;
    s_led.initialized = false;
    s_led.stop_requested = false;
    s_led.last_hw_error = ESP_OK;
    return result;
}
