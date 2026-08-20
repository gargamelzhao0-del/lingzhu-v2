#include "board/board_gpio_inputs.h"

#include <stdatomic.h>

#include "board/esp_vocat_v1_0_pins.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "board_inputs";

typedef struct {
    int gpio;
    int active_level;
    const char *name;
} input_config_t;

static const input_config_t s_inputs[] = {
    [NUWAX_BOARD_INPUT_HEAD_TOUCH] = {
        .gpio = NUWAX_VOCAT_TOUCH_WAKE_GPIO,
        .active_level = 0,
        .name = "head_touch",
    },
    [NUWAX_BOARD_INPUT_BOOT_BUTTON] = {
        .gpio = NUWAX_VOCAT_BOOT_GPIO,
        .active_level = 0,
        .name = "boot_button",
    },
};

static TaskHandle_t s_task;
static SemaphoreHandle_t s_stopped;
static nuwax_board_input_callback_t s_callback;
static void *s_user_ctx;
static nuwax_head_touch_event_cb_t s_head_touch_callback;
static void *s_head_touch_user_ctx;
static uint32_t s_head_touch_long_press_ms = 1000U;
static uint32_t s_head_touch_gap_tolerance_ms = 750U;
static uint32_t s_head_touch_max_valid_duration_ms = 5000U;
static portMUX_TYPE s_head_touch_config_lock = portMUX_INITIALIZER_UNLOCKED;
static atomic_bool s_initialized = ATOMIC_VAR_INIT(false);
static atomic_bool s_stop_requested = ATOMIC_VAR_INIT(false);

static void emit_head_touch_event(nuwax_head_touch_event_type_t type,
                                  uint64_t duration_ms,
                                  bool active)
{
    nuwax_head_touch_event_cb_t callback;
    void *user_ctx;
    portENTER_CRITICAL(&s_head_touch_config_lock);
    callback = s_head_touch_callback;
    user_ctx = s_head_touch_user_ctx;
    portEXIT_CRITICAL(&s_head_touch_config_lock);
    if (!callback) {
        return;
    }
    const nuwax_head_touch_event_t event = {
        .type = type,
        .duration_ms = duration_ms,
        .active = active,
    };
    callback(&event, user_ctx);
}

esp_err_t nuwax_board_gpio_input_read(nuwax_board_input_t input, bool *active)
{
    if (!active || input < 0 || input >= (int)(sizeof(s_inputs) / sizeof(s_inputs[0]))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!atomic_load_explicit(&s_initialized, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    *active = gpio_get_level(s_inputs[input].gpio) == s_inputs[input].active_level;
    return ESP_OK;
}

static void input_task(void *arg)
{
    (void)arg;
    int stable[sizeof(s_inputs) / sizeof(s_inputs[0])] = {0};
    int raw_last[sizeof(s_inputs) / sizeof(s_inputs[0])] = {0};
    unsigned same_count[sizeof(s_inputs) / sizeof(s_inputs[0])] = {0};
    bool head_touch_active = false;
    bool head_touch_long_reported = false;
    bool head_touch_expired_reported = false;
    bool head_touch_raw_active_started_valid = false;
    uint64_t head_touch_raw_active_started_ms = 0U;
    uint64_t head_touch_press_started_ms = 0U;
    unsigned loop_count = 0;
    for (size_t i = 0; i < sizeof(s_inputs) / sizeof(s_inputs[0]); ++i) {
        stable[i] = raw_last[i] = gpio_get_level(s_inputs[i].gpio);
        if (i == NUWAX_BOARD_INPUT_HEAD_TOUCH &&
            raw_last[i] == s_inputs[i].active_level) {
            head_touch_raw_active_started_ms =
                (uint64_t)(esp_timer_get_time() / 1000ULL);
            head_touch_raw_active_started_valid = true;
            /* Require the ordinary debounce window before publishing an
             * already-active sensor at boot. */
            stable[i] = !raw_last[i];
        }
    }

    while (!atomic_load_explicit(&s_stop_requested, memory_order_acquire)) {
        for (size_t i = 0; i < sizeof(s_inputs) / sizeof(s_inputs[0]); ++i) {
            const int raw = gpio_get_level(s_inputs[i].gpio);
            if (raw == raw_last[i]) {
                same_count[i]++;
            } else {
                raw_last[i] = raw;
                same_count[i] = 1;
                if (i == NUWAX_BOARD_INPUT_HEAD_TOUCH) {
                    if (raw == s_inputs[i].active_level) {
                        head_touch_raw_active_started_ms =
                            (uint64_t)(esp_timer_get_time() / 1000ULL);
                        head_touch_raw_active_started_valid = true;
                    } else {
                        head_touch_raw_active_started_valid = false;
                    }
                }
            }
            if (same_count[i] >= 3 && raw != stable[i]) {
                stable[i] = raw;
                const bool active = raw == s_inputs[i].active_level;
                ESP_LOGI(TAG,
                         "%s gpio=%d active=%d level=%d",
                         s_inputs[i].name, s_inputs[i].gpio, active, raw);
                nuwax_board_input_callback_t callback;
                void *user_ctx;
                portENTER_CRITICAL(&s_head_touch_config_lock);
                callback = s_callback;
                user_ctx = s_user_ctx;
                portEXIT_CRITICAL(&s_head_touch_config_lock);
                if (callback) {
                    callback((nuwax_board_input_t)i, active, user_ctx);
                }
                if (i == NUWAX_BOARD_INPUT_HEAD_TOUCH) {
                    const uint64_t now_ms =
                        (uint64_t)(esp_timer_get_time() / 1000ULL);
                    if (active) {
                        const uint64_t activation_started_ms =
                            head_touch_raw_active_started_valid &&
                                    head_touch_raw_active_started_ms <= now_ms
                                ? head_touch_raw_active_started_ms
                                : now_ms;
                        head_touch_active = true;
                        head_touch_long_reported = false;
                        head_touch_expired_reported = false;
                        head_touch_press_started_ms = activation_started_ms;
                        emit_head_touch_event(NUWAX_HEAD_TOUCH_EVENT_PRESS,
                                              0U,
                                              true);
                    } else {
                        const uint64_t duration_ms =
                            head_touch_active &&
                                    now_ms >= head_touch_press_started_ms
                                ? now_ms - head_touch_press_started_ms
                                : 0U;
                        emit_head_touch_event(NUWAX_HEAD_TOUCH_EVENT_RELEASE,
                                              duration_ms,
                                              false);
                        head_touch_active = false;
                        head_touch_long_reported = false;
                        head_touch_expired_reported = false;
                        head_touch_press_started_ms = 0U;
                    }
                }
            }
        }
        const uint64_t now_ms =
            (uint64_t)(esp_timer_get_time() / 1000ULL);
        uint32_t long_press_ms;
        uint32_t max_valid_duration_ms;
        portENTER_CRITICAL(&s_head_touch_config_lock);
        long_press_ms = s_head_touch_long_press_ms;
        max_valid_duration_ms = s_head_touch_max_valid_duration_ms;
        portEXIT_CRITICAL(&s_head_touch_config_lock);
        if (head_touch_active) {
            const uint64_t press_duration_ms =
                now_ms >= head_touch_press_started_ms
                    ? now_ms - head_touch_press_started_ms
                    : 0U;
            if (!head_touch_expired_reported &&
                press_duration_ms >= max_valid_duration_ms) {
                head_touch_expired_reported = true;
                ESP_LOGW(TAG,
                         "head_touch expired duration_ms=%llu max_valid_ms=%u action=none",
                         (unsigned long long)press_duration_ms,
                         (unsigned)max_valid_duration_ms);
                emit_head_touch_event(NUWAX_HEAD_TOUCH_EVENT_EXPIRED,
                                      press_duration_ms,
                                      true);
            } else if (!head_touch_expired_reported &&
                       !head_touch_long_reported &&
                       press_duration_ms >= long_press_ms) {
                head_touch_long_reported = true;
                ESP_LOGI(TAG,
                         "head_touch qualified continuous_press_ms=%llu",
                         (unsigned long long)press_duration_ms);
                emit_head_touch_event(NUWAX_HEAD_TOUCH_EVENT_LONG_PRESS,
                                      press_duration_ms,
                                      true);
            }
        }
        if (++loop_count == 100U) {
            ESP_LOGI(TAG,
                     "task resources stack=psram bytes=2560 stack_min_free=%u",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGI(TAG, "stopped task_stack=psram stack_min_free=%u",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    xSemaphoreGive(s_stopped);
    vTaskSuspend(NULL);
}

esp_err_t nuwax_board_gpio_inputs_init(nuwax_board_input_callback_t callback,
                                       void *user_ctx)
{
    if (atomic_load_explicit(&s_initialized, memory_order_acquire)) {
        portENTER_CRITICAL(&s_head_touch_config_lock);
        s_callback = callback;
        s_user_ctx = user_ctx;
        portEXIT_CRITICAL(&s_head_touch_config_lock);
        return ESP_OK;
    }

    const gpio_config_t head_touch_config = {
        .pin_bit_mask = 1ULL << NUWAX_VOCAT_TOUCH_WAKE_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&head_touch_config);
    if (err != ESP_OK) return err;

    const gpio_config_t boot_button_config = {
        .pin_bit_mask = 1ULL << NUWAX_VOCAT_BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&boot_button_config);
    if (err != ESP_OK) return err;

    s_stopped = xSemaphoreCreateBinary();
    if (!s_stopped) return ESP_ERR_NO_MEM;
    portENTER_CRITICAL(&s_head_touch_config_lock);
    s_callback = callback;
    s_user_ctx = user_ctx;
    portEXIT_CRITICAL(&s_head_touch_config_lock);
    atomic_store_explicit(&s_stop_requested, false, memory_order_release);
    atomic_store_explicit(&s_initialized, true, memory_order_release);
    if (xTaskCreateWithCaps(input_task, "board_inputs", 2560, NULL, 4,
                            &s_task,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        atomic_store_explicit(&s_initialized, false, memory_order_release);
        vSemaphoreDelete(s_stopped);
        s_stopped = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG,
             "ready head_touch=GPIO%d(active-low) boot=GPIO%d(active-low) task_stack=psram bytes=2560",
             NUWAX_VOCAT_TOUCH_WAKE_GPIO, NUWAX_VOCAT_BOOT_GPIO);
    return ESP_OK;
}

esp_err_t nuwax_board_gpio_inputs_deinit(void)
{
    if (!atomic_load_explicit(&s_initialized, memory_order_acquire)) {
        return ESP_OK;
    }
    atomic_store_explicit(&s_stop_requested, true, memory_order_release);
    if (xSemaphoreTake(s_stopped, pdMS_TO_TICKS(250)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    TaskHandle_t stopped_task = s_task;
    if (stopped_task) {
        vTaskDeleteWithCaps(stopped_task);
    }
    vSemaphoreDelete(s_stopped);
    s_stopped = NULL;
    s_task = NULL;
    portENTER_CRITICAL(&s_head_touch_config_lock);
    s_callback = NULL;
    s_user_ctx = NULL;
    s_head_touch_callback = NULL;
    s_head_touch_user_ctx = NULL;
    portEXIT_CRITICAL(&s_head_touch_config_lock);
    atomic_store_explicit(&s_initialized, false, memory_order_release);
    return ESP_OK;
}

esp_err_t nuwax_head_touch_set_event_callback(nuwax_head_touch_event_cb_t callback,
                                              void *user_ctx)
{
    portENTER_CRITICAL(&s_head_touch_config_lock);
    s_head_touch_callback = callback;
    s_head_touch_user_ctx = user_ctx;
    portEXIT_CRITICAL(&s_head_touch_config_lock);
    return ESP_OK;
}

esp_err_t nuwax_head_touch_set_config(const nuwax_head_touch_config_t *config)
{
    if (!config || config->long_press_ms < 100U ||
        config->long_press_ms > 10000U ||
        config->gap_tolerance_ms > 2000U ||
        config->gap_tolerance_ms > config->long_press_ms ||
        config->max_valid_duration_ms < 100U ||
        config->max_valid_duration_ms > 60000U ||
        config->max_valid_duration_ms <= config->long_press_ms) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_head_touch_config_lock);
    s_head_touch_long_press_ms = config->long_press_ms;
    s_head_touch_gap_tolerance_ms = config->gap_tolerance_ms;
    s_head_touch_max_valid_duration_ms = config->max_valid_duration_ms;
    portEXIT_CRITICAL(&s_head_touch_config_lock);
    return ESP_OK;
}
