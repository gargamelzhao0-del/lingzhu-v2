#include "touch_input.h"

#include <stdatomic.h>
#include <string.h>

#include "board/i2c_bus_manager.h"
#include "board/esp_vocat_v1_0_pins.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#if CONFIG_NUWAX_TOUCH_ENABLE

static const char *TAG = "nuwax_touch";

#define CST816D_I2C_ADDR NUWAX_VOCAT_I2C_ADDR_TOUCH_PRIMARY
#define CST816T_ALT_2A_I2C_ADDR NUWAX_VOCAT_I2C_ADDR_TOUCH_ALT_2A
#define CST816T_ALT_2D_I2C_ADDR NUWAX_VOCAT_I2C_ADDR_TOUCH_ALT_2D
#define CST816D_REG_TOUCH_DATA 0x01
#define CST816D_REG_CHIP_VERSION 0x15
#define CST816D_REG_VERSION_INFO 0xA7
#define CST816D_REG_DISABLE_AUTO_SLEEP 0xFE
#define CST816D_DISABLE_AUTO_SLEEP_VALUE 0xFE
#define NUWAX_TOUCH_CONFIG_RETRY_COUNT 3
#define NUWAX_TOUCH_CONFIG_RETRY_DELAY_MS 20
#define NUWAX_TOUCH_RECOVERY_COOLDOWN_US 5000000
#define NUWAX_TOUCH_INIT_RETRY_COOLDOWN_US 5000000
#define NUWAX_TOUCH_INIT_RETRY_MAX_US 60000000
#define NUWAX_TOUCH_INIT_BUSY_RETRY_US 250000
#define NUWAX_TOUCH_READ_RETRY_COUNT 2
#define NUWAX_TOUCH_READ_RETRY_DELAY_MS 20
#define NUWAX_TOUCH_BUS_RECOVERY_SETTLE_MS 20
#define NUWAX_TOUCH_IDLE_FALLBACK_POLL_MS 250
#define NUWAX_TOUCH_IO_LOCK_TIMEOUT_MS 1000U
#define NUWAX_TOUCH_STATUS_LOCK_TIMEOUT_MS 100U
#define NUWAX_TOUCH_BUS_LOCK_TIMEOUT_MS 50U
#define NUWAX_TOUCH_PANEL_WIDTH 360u
#define NUWAX_TOUCH_PANEL_HEIGHT 360u

typedef struct {
    const char *name;
    uint8_t address;
} nuwax_touch_i2c_candidate_t;

static const nuwax_touch_i2c_candidate_t s_touch_candidates[] = {
    {.name = "CST816@15", .address = CST816D_I2C_ADDR},
    {.name = "CST816@2A", .address = CST816T_ALT_2A_I2C_ADDR},
    {.name = "CST816@2D", .address = CST816T_ALT_2D_I2C_ADDR},
};

static i2c_master_dev_handle_t s_touch_dev;
static uint8_t s_touch_address;
static atomic_bool s_initialized = ATOMIC_VAR_INIT(false);
static TaskHandle_t s_touch_log_task;
static bool s_touch_isr_registered;
static bool s_p1_diag_task_started;
static nuwax_touch_event_cb_t s_event_callback;
static void *s_event_user_ctx;
static portMUX_TYPE s_touch_callback_mux = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_touch_queue;
static SemaphoreHandle_t s_touch_io_lock;
static StaticSemaphore_t s_touch_io_lock_storage;
static portMUX_TYPE s_touch_lock_init_mux = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_next_recovery_us;
static int64_t s_next_init_retry_us;
static uint32_t s_read_failure_count;
static uint32_t s_bus_recovery_count;
static uint32_t s_bus_recovery_success_count;
static uint32_t s_init_failure_streak;
/* Every buffer passed to the I2C driver stays in internal DRAM. This keeps
 * touch task stacks in PSRAM without exposing them to an in-flight ISR. */
static DRAM_ATTR uint8_t s_touch_i2c_tx_scratch[2];
static DRAM_ATTR uint8_t s_touch_i2c_rx_scratch[6];
static DRAM_ATTR uint8_t s_touch_read_scratch[6];

static void IRAM_ATTR touch_int_isr(void *arg)
{
    (void)arg;
    TaskHandle_t task = s_touch_log_task;
    if (!task) {
        return;
    }
    BaseType_t higher_priority_task_woken = pdFALSE;
    vTaskNotifyGiveFromISR(task, &higher_priority_task_woken);
    portYIELD_FROM_ISR(higher_priority_task_woken);
}

static esp_err_t ensure_touch_io_lock(void)
{
    taskENTER_CRITICAL(&s_touch_lock_init_mux);
    if (!s_touch_io_lock) {
        s_touch_io_lock = xSemaphoreCreateMutexStatic(&s_touch_io_lock_storage);
    }
    const SemaphoreHandle_t lock = s_touch_io_lock;
    taskEXIT_CRITICAL(&s_touch_lock_init_mux);
    return lock ? ESP_OK : ESP_ERR_NO_MEM;
}

#if CONFIG_NUWAX_BOARD_ESP_VOCAT_V1_0
#define NUWAX_TOUCH_WAKE_DIAG_GPIO NUWAX_VOCAT_TOUCH_WAKE_GPIO
#else
#define NUWAX_TOUCH_WAKE_DIAG_GPIO (-1)
#endif

#ifndef CONFIG_NUWAX_P1_HW_DIAG_INTERVAL_MS
#define CONFIG_NUWAX_P1_HW_DIAG_INTERVAL_MS 1000
#endif

#define NUWAX_P1_DIAG_I2C_TIMEOUT_MS 10

static bool touch_queue_send_latest(const nuwax_touch_point_t *point, TickType_t wait_ticks) {
    if (!s_touch_queue || !point) {
        return false;
    }
    if (xQueueSend(s_touch_queue, point, wait_ticks) == pdTRUE) {
        return true;
    }
    nuwax_touch_point_t dropped = {0};
    (void)xQueueReceive(s_touch_queue, &dropped, 0);
    return xQueueSend(s_touch_queue, point, 0) == pdTRUE;
}

static esp_err_t touch_read_reg_locked(uint8_t reg, uint8_t *data, size_t len) {
    ESP_RETURN_ON_FALSE(s_touch_dev, ESP_ERR_INVALID_STATE, TAG, "touch device is not open");
    ESP_RETURN_ON_FALSE(data && len > 0, ESP_ERR_INVALID_ARG, TAG, "invalid touch read buffer");
    ESP_RETURN_ON_FALSE(len <= sizeof(s_touch_i2c_rx_scratch),
                        ESP_ERR_INVALID_SIZE, TAG, "touch read is too large");
    s_touch_i2c_tx_scratch[0] = reg;
    memset(s_touch_i2c_rx_scratch, 0, len);
    esp_err_t err = nuwax_i2c_bus_transaction_begin(
        NUWAX_TOUCH_BUS_LOCK_TIMEOUT_MS);
    if (err != ESP_OK) {
        /* No transfer was issued. Do not let ordinary bus contention trigger
         * touch-controller recovery or a shared-bus reset. */
        return ESP_ERR_NOT_FINISHED;
    }
    err = i2c_master_transmit_receive(
        s_touch_dev, s_touch_i2c_tx_scratch, 1, s_touch_i2c_rx_scratch, len,
        CONFIG_NUWAX_TOUCH_I2C_TIMEOUT_MS);
    nuwax_i2c_bus_transaction_end();
    if (err == ESP_OK) {
        memcpy(data, s_touch_i2c_rx_scratch, len);
    }
    return err;
}

static esp_err_t touch_write_reg_locked(uint8_t reg, uint8_t value) {
    ESP_RETURN_ON_FALSE(s_touch_dev, ESP_ERR_INVALID_STATE, TAG, "touch device is not open");
    s_touch_i2c_tx_scratch[0] = reg;
    s_touch_i2c_tx_scratch[1] = value;
    esp_err_t err = nuwax_i2c_bus_transaction_begin(
        NUWAX_TOUCH_BUS_LOCK_TIMEOUT_MS);
    if (err != ESP_OK) {
        return ESP_ERR_NOT_FINISHED;
    }
    err = i2c_master_transmit(s_touch_dev, s_touch_i2c_tx_scratch,
                              sizeof(s_touch_i2c_tx_scratch),
                              CONFIG_NUWAX_TOUCH_I2C_TIMEOUT_MS);
    nuwax_i2c_bus_transaction_end();
    return err;
}

static TickType_t touch_delay_ticks_ceil(uint32_t delay_ms) {
    const uint64_t ticks =
        ((uint64_t)delay_ms * (uint64_t)configTICK_RATE_HZ + 999ULL) /
        1000ULL;
    return (TickType_t)(ticks > 0U ? ticks : 1U);
}

static uint32_t touch_schedule_init_retry_locked(void)
{
    if (s_init_failure_streak < UINT32_MAX) {
        s_init_failure_streak++;
    }
    uint64_t delay_us = NUWAX_TOUCH_INIT_RETRY_COOLDOWN_US;
    uint32_t shifts = s_init_failure_streak > 0U
                          ? s_init_failure_streak - 1U
                          : 0U;
    if (shifts > 4U) {
        shifts = 4U;
    }
    delay_us <<= shifts;
    if (delay_us > NUWAX_TOUCH_INIT_RETRY_MAX_US) {
        delay_us = NUWAX_TOUCH_INIT_RETRY_MAX_US;
    }
    s_next_init_retry_us = esp_timer_get_time() + (int64_t)delay_us;
    return (uint32_t)(delay_us / 1000U);
}

static uint32_t touch_schedule_busy_retry_locked(void)
{
    s_next_init_retry_us = esp_timer_get_time() +
                           NUWAX_TOUCH_INIT_BUSY_RETRY_US;
    return (uint32_t)(NUWAX_TOUCH_INIT_BUSY_RETRY_US / 1000U);
}

static bool touch_take_io_lock(uint32_t timeout_ms, const char *operation) {
    if (!s_touch_io_lock ||
        xSemaphoreTake(s_touch_io_lock,
                       touch_delay_ticks_ceil(timeout_ms)) != pdTRUE) {
        ESP_LOGW(TAG,
                 "touch I/O lock timeout operation=%s timeout_ms=%lu",
                 operation ? operation : "-",
                 (unsigned long)timeout_ms);
        return false;
    }
    return true;
}

static esp_err_t touch_reset(void) {
#if CONFIG_NUWAX_TOUCH_PIN_RST < 0
    ESP_LOGI(TAG, "touch reset skipped (RST GPIO disabled)");
    return ESP_OK;
#else
    gpio_config_t rst_cfg = {
        .pin_bit_mask = 1ULL << CONFIG_NUWAX_TOUCH_PIN_RST,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&rst_cfg), TAG, "touch rst gpio config failed");

    gpio_set_level(CONFIG_NUWAX_TOUCH_PIN_RST, 1);
    vTaskDelay(touch_delay_ticks_ceil(50U));
    gpio_set_level(CONFIG_NUWAX_TOUCH_PIN_RST, 0);
    esp_rom_delay_us(5000U);
    gpio_set_level(CONFIG_NUWAX_TOUCH_PIN_RST, 1);
    vTaskDelay(touch_delay_ticks_ceil(50U));
    return ESP_OK;
#endif
}

static void touch_log_pin_levels(const char *stage) {
    int int_level = gpio_get_level(CONFIG_NUWAX_TOUCH_PIN_INT);
#if CONFIG_NUWAX_TOUCH_PIN_RST >= 0
    int rst_level = gpio_get_level(CONFIG_NUWAX_TOUCH_PIN_RST);
#else
    int rst_level = -1;
#endif
    ESP_LOGI(TAG,
             "P1-4 touch diag[%s]: scl=%d sda=%d int=GPIO%d level=%d rst=GPIO%d level=%d",
             stage,
             CONFIG_NUWAX_TOUCH_PIN_SCL,
             CONFIG_NUWAX_TOUCH_PIN_SDA,
             CONFIG_NUWAX_TOUCH_PIN_INT,
             int_level,
             CONFIG_NUWAX_TOUCH_PIN_RST,
             rst_level);
}

static esp_err_t touch_disable_auto_sleep_locked(void) {
#if CONFIG_NUWAX_TOUCH_DISABLE_AUTO_SLEEP
    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= NUWAX_TOUCH_CONFIG_RETRY_COUNT; ++attempt) {
        /* Bundled CST816S driver uses non-zero 0xFE at register 0xFE. */
        err = touch_write_reg_locked(CST816D_REG_DISABLE_AUTO_SLEEP,
                                     CST816D_DISABLE_AUTO_SLEEP_VALUE);
        if (err == ESP_OK) {
            if (attempt > 1) {
                ESP_LOGI(TAG, "touch auto-sleep disable succeeded on retry=%d", attempt);
            }
            return ESP_OK;
        }
        if (err == ESP_ERR_NOT_FINISHED) {
            return err;
        }
        ESP_LOGW(TAG,
                 "touch auto-sleep disable failed attempt=%d/%d addr=0x%02x err=%s",
                 attempt,
                 NUWAX_TOUCH_CONFIG_RETRY_COUNT,
                 s_touch_address,
                 esp_err_to_name(err));
        if (attempt < NUWAX_TOUCH_CONFIG_RETRY_COUNT) {
            vTaskDelay(pdMS_TO_TICKS(NUWAX_TOUCH_CONFIG_RETRY_DELAY_MS));
        }
    }
    return err;
#else
    return ESP_OK;
#endif
}

static esp_err_t touch_configure_device_locked(uint8_t *out_version,
                                               uint8_t out_version_info[3]) {
    uint8_t version = 0;
    uint8_t version_info[3] = {0};
    uint8_t touch_data[6] = {0};
    const esp_err_t sleep_err = touch_disable_auto_sleep_locked();
    if (sleep_err != ESP_OK) {
        return sleep_err;
    }

    const esp_err_t version_err =
        touch_read_reg_locked(CST816D_REG_CHIP_VERSION, &version, sizeof(version));
    const esp_err_t info_err =
        touch_read_reg_locked(CST816D_REG_VERSION_INFO, version_info, sizeof(version_info));
    const esp_err_t data_err =
        touch_read_reg_locked(CST816D_REG_TOUCH_DATA, touch_data, sizeof(touch_data));
    if (data_err != ESP_OK) {
        ESP_LOGW(TAG,
                 "candidate touch-data validation failed addr=0x%02x err=%s",
                 s_touch_address,
                 esp_err_to_name(data_err));
        return data_err;
    }
    if (version_err != ESP_OK || info_err != ESP_OK) {
        /* The vendor V2 profile intentionally disables CST816S ID reads.
         * A valid touch-data transaction is the compatibility gate; version
         * registers are diagnostic only because some controller revisions do
         * not implement one or both of them. */
        ESP_LOGW(TAG,
                 "touch version registers unavailable addr=0x%02x chip=%s info=%s; data path accepted",
                 s_touch_address,
                 esp_err_to_name(version_err),
                 esp_err_to_name(info_err));
    }

    if (out_version) {
        *out_version = version;
    }
    if (out_version_info) {
        memcpy(out_version_info, version_info, sizeof(version_info));
    }
    return ESP_OK;
}

static esp_err_t touch_connect_locked(uint8_t *out_version,
                                      uint8_t out_version_info[3]) {
#if CONFIG_NUWAX_TOUCH_PIN_RST < 0
    ESP_LOGI(TAG, "touch recovery keeps one I2C handle; shared display reset remains untouched");
#endif

    /* V2 hardware fixes the CST816 address at 0x15. Keep this handle for the
     * entire boot: after a timed-out receive, removing/re-adding devices or
     * probing alternate addresses can race the driver's retiring ISR. */
    const nuwax_touch_i2c_candidate_t *candidate = &s_touch_candidates[0];
    if (!s_touch_dev) {
        esp_err_t add_err = nuwax_i2c_bus_add_device(candidate->address,
                                                     CONFIG_NUWAX_TOUCH_I2C_HZ,
                                                     &s_touch_dev);
        if (add_err != ESP_OK) {
            return add_err;
        }
        s_touch_address = candidate->address;
    }

    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= NUWAX_TOUCH_CONFIG_RETRY_COUNT; ++attempt) {
        err = touch_configure_device_locked(out_version, out_version_info);
        if (err == ESP_OK) {
            ESP_LOGI(TAG,
                     "selected CST816-family controller name=%s addr=0x%02x",
                     candidate->name,
                     candidate->address);
            return ESP_OK;
        }
        if (err == ESP_ERR_NOT_FINISHED) {
            return err;
        }
        ESP_LOGW(TAG,
                 "touch validation retry=%d/%d addr=0x%02x err=%s",
                 attempt,
                 NUWAX_TOUCH_CONFIG_RETRY_COUNT,
                 s_touch_address,
                 esp_err_to_name(err));
        if (attempt < NUWAX_TOUCH_CONFIG_RETRY_COUNT) {
            vTaskDelay(pdMS_TO_TICKS(NUWAX_TOUCH_CONFIG_RETRY_DELAY_MS));
        }
    }
    return err;
}

static esp_err_t touch_read_with_recovery_locked(uint8_t reg, uint8_t *data, size_t len) {
    ESP_RETURN_ON_FALSE(s_touch_io_lock, ESP_ERR_INVALID_STATE, TAG, "touch I/O lock is unavailable");

    const int64_t now_us = esp_timer_get_time();
    if (s_next_recovery_us > 0 && now_us < s_next_recovery_us) {
        return ESP_ERR_NOT_FINISHED;
    }

    esp_err_t err = touch_read_reg_locked(reg, data, len);
    if (err == ESP_OK) {
        s_next_recovery_us = 0;
        return ESP_OK;
    }
    if (err == ESP_ERR_NOT_FINISHED) {
        return err;
    }

    s_read_failure_count++;
    s_next_recovery_us = now_us + NUWAX_TOUCH_RECOVERY_COOLDOWN_US;
    ESP_LOGW(TAG,
             "touch read failed addr=0x%02x err=%s failures=%lu; bounded local recovery",
             s_touch_address,
             esp_err_to_name(err),
             (unsigned long)s_read_failure_count);
    for (int attempt = 1; attempt <= NUWAX_TOUCH_READ_RETRY_COUNT; ++attempt) {
        vTaskDelay(touch_delay_ticks_ceil(NUWAX_TOUCH_READ_RETRY_DELAY_MS));
        err = touch_read_reg_locked(reg, data, len);
        if (err == ESP_OK) {
            s_next_recovery_us = 0;
            ESP_LOGI(TAG,
                     "touch read recovered locally retry=%d addr=0x%02x",
                     attempt,
                     s_touch_address);
            return ESP_OK;
        }
        if (err == ESP_ERR_NOT_FINISHED) {
            s_next_recovery_us = 0;
            return err;
        }
        ESP_LOGW(TAG,
                 "touch read retry=%d/%d addr=0x%02x err=%s",
                 attempt,
                 NUWAX_TOUCH_READ_RETRY_COUNT,
                 s_touch_address,
                 esp_err_to_name(err));
    }

    s_bus_recovery_count++;
    const esp_err_t bus_err =
        nuwax_i2c_bus_recover("touch read retries exhausted");
    if (bus_err == ESP_OK) {
        vTaskDelay(touch_delay_ticks_ceil(
            NUWAX_TOUCH_BUS_RECOVERY_SETTLE_MS));
        const esp_err_t config_err = touch_disable_auto_sleep_locked();
        if (config_err == ESP_OK) {
            err = touch_read_reg_locked(reg, data, len);
        } else {
            err = config_err;
        }
        if (err == ESP_OK) {
            s_bus_recovery_success_count++;
            s_next_recovery_us = 0;
            ESP_LOGI(TAG,
                     "touch recovered after shared I2C reset recoveries=%lu successes=%lu",
                     (unsigned long)s_bus_recovery_count,
                     (unsigned long)s_bus_recovery_success_count);
            return ESP_OK;
        }
    } else {
        err = bus_err;
    }

    atomic_store_explicit(&s_initialized, false, memory_order_release);
    const uint32_t retry_ms = err == ESP_ERR_NOT_FINISHED
                                  ? touch_schedule_busy_retry_locked()
                                  : touch_schedule_init_retry_locked();
    s_next_recovery_us = s_next_init_retry_us;
    ESP_LOGW(TAG,
             "touch recovery failed err=%s; controller offline retry_ms=%lu streak=%lu",
             esp_err_to_name(err),
             (unsigned long)retry_ms,
             (unsigned long)s_init_failure_streak);
    return err;
}

static esp_err_t touch_diag_read_reg_at(uint8_t address, uint8_t reg, uint8_t *data, size_t len, int timeout_ms)
{
    ESP_RETURN_ON_ERROR(ensure_touch_io_lock(), TAG,
                        "touch diagnostic lock unavailable");
    ESP_RETURN_ON_FALSE(data != NULL && len > 0 &&
                            len <= sizeof(s_touch_i2c_rx_scratch),
                        ESP_ERR_INVALID_SIZE, TAG,
                        "touch diagnostic read is too large");
    const uint32_t lock_timeout_ms = timeout_ms > 0
                                         ? (uint32_t)timeout_ms
                                         : NUWAX_TOUCH_STATUS_LOCK_TIMEOUT_MS;
    ESP_RETURN_ON_FALSE(touch_take_io_lock(lock_timeout_ms,
                                           "diagnostic_read"),
                        ESP_ERR_TIMEOUT, TAG,
                        "touch diagnostic lock timeout");
    const esp_err_t err = s_touch_dev && address == s_touch_address
                              ? touch_read_reg_locked(reg, data, len)
                              : ESP_ERR_NOT_FOUND;

    xSemaphoreGive(s_touch_io_lock);
    return err;
}

static void touch_log_candidate_regs(const char *name, uint8_t address)
{
    if (!s_touch_dev || address != s_touch_address) {
        return;
    }

    uint8_t chip_version = 0;
    uint8_t version_info[3] = {0};
    uint8_t touch_data[6] = {0};
    esp_err_t chip_err = touch_diag_read_reg_at(address,
                                                CST816D_REG_CHIP_VERSION,
                                                &chip_version,
                                                sizeof(chip_version),
                                                CONFIG_NUWAX_TOUCH_I2C_TIMEOUT_MS);
    esp_err_t info_err = touch_diag_read_reg_at(address,
                                                CST816D_REG_VERSION_INFO,
                                                version_info,
                                                sizeof(version_info),
                                                CONFIG_NUWAX_TOUCH_I2C_TIMEOUT_MS);
    esp_err_t data_err = touch_diag_read_reg_at(address,
                                                CST816D_REG_TOUCH_DATA,
                                                touch_data,
                                                sizeof(touch_data),
                                                CONFIG_NUWAX_TOUCH_I2C_TIMEOUT_MS);
    ESP_LOGI(TAG,
             "P1 touch candidate regs: %s addr=0x%02x chip=%s/0x%02x info=%s/%02x.%02x.%02x data=%s/%02x %02x %02x %02x %02x %02x",
             name ? name : "candidate",
             address,
             esp_err_to_name(chip_err),
             chip_version,
             esp_err_to_name(info_err),
             version_info[0],
             version_info[1],
             version_info[2],
             esp_err_to_name(data_err),
             touch_data[0],
             touch_data[1],
             touch_data[2],
             touch_data[3],
             touch_data[4],
             touch_data[5]);
}

static void touch_log_candidate_registers(void)
{
    if (s_touch_dev && s_touch_address != 0) {
        touch_log_candidate_regs(s_touch_candidates[0].name, s_touch_address);
    }
}

static esp_err_t touch_diag_config_input(int gpio_num, bool pullup)
{
    if (gpio_num < 0) {
        return ESP_OK;
    }
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << gpio_num,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = pullup ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg);
}

static void p1_touch_diag_task(void *arg)
{
    (void)arg;
    /* Fast inner poll so a real finger press (<1s) is actually captured via the
     * production read path. Slow periodic diagnostics (pin levels, probe, scan)
     * still run roughly every CONFIG_NUWAX_P1_HW_DIAG_INTERVAL_MS. */
    const uint32_t poll_ms = CONFIG_NUWAX_TOUCH_POLL_INTERVAL_MS;
    const uint32_t slow_period = (CONFIG_NUWAX_P1_HW_DIAG_INTERVAL_MS / (poll_ms ? poll_ms : 1)) + 1;
    uint32_t loop = 0;
    uint32_t slow = 0;
    bool was_touched = false;

    while (true) {
        nuwax_touch_point_t point = {0};
        if (nuwax_touch_read(&point) == ESP_OK) {
            if (point.touched) {
                ESP_LOGI(TAG,
                         "P1-4 TOUCH %s gesture=0x%02x points=%u event=%u x=%u y=%u",
                         was_touched ? "move " : "PRESS",
                         point.gesture, point.points, point.event, point.x, point.y);
                was_touched = true;
            } else if (was_touched) {
                ESP_LOGI(TAG, "P1-4 TOUCH RELEASE");
                was_touched = false;
            }
        }

        if ((loop % slow_period) == 0U) {
            int int_level = gpio_get_level(CONFIG_NUWAX_TOUCH_PIN_INT);
            int wake_level = NUWAX_TOUCH_WAKE_DIAG_GPIO >= 0 ? gpio_get_level(NUWAX_TOUCH_WAKE_DIAG_GPIO) : -1;
            ESP_LOGI(TAG,
                     "P1 diag touch lines: loop=%lu tp_int=GPIO%d level=%d touch_wake=GPIO%d level=%d",
                     (unsigned long)slow,
                     CONFIG_NUWAX_TOUCH_PIN_INT,
                     int_level,
                     NUWAX_TOUCH_WAKE_DIAG_GPIO,
                     wake_level);
            if ((slow % 5U) == 0U) {
                touch_log_candidate_registers();
            }
            slow++;
        }
        loop++;
        vTaskDelay(pdMS_TO_TICKS(poll_ms ? poll_ms : 20));
    }
}

static void touch_log_task(void *arg) {
    (void)arg;
    nuwax_touch_point_t point = {0};
    bool was_touched = false;

    while (true) {
        const uint32_t wait_ms = was_touched
                                     ? CONFIG_NUWAX_TOUCH_POLL_INTERVAL_MS
                                     : NUWAX_TOUCH_IDLE_FALLBACK_POLL_MS;
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));

        const esp_err_t read_err = nuwax_touch_read(&point);
        if (read_err == ESP_OK) {
            if (point.touched) {
                if (!was_touched) {
                    ESP_LOGD(TAG,
                             "touch press gesture=0x%02x points=%u event=%u x=%u y=%u",
                             point.gesture,
                             point.points,
                             point.event,
                             point.x,
                             point.y);
                    nuwax_touch_event_cb_t callback = NULL;
                    void *user_ctx = NULL;
                    taskENTER_CRITICAL(&s_touch_callback_mux);
                    callback = s_event_callback;
                    user_ctx = s_event_user_ctx;
                    taskEXIT_CRITICAL(&s_touch_callback_mux);
                    if (callback) {
                        callback(&point, user_ctx);
                    }
                }
                if (s_touch_queue) {
                    (void)touch_queue_send_latest(&point, 0);
                }
                was_touched = true;
            } else if (was_touched) {
                ESP_LOGD(TAG, "touch release");
                point.touched = false;
                point.event = 3;
                if (s_touch_queue) {
                    if (!touch_queue_send_latest(&point, pdMS_TO_TICKS(20))) {
                        ESP_LOGW(TAG, "touch release queue send failed");
                    }
                }
                was_touched = false;
            }
        } else if (was_touched) {
            nuwax_touch_point_t cancelled = point;
            cancelled.touched = false;
            cancelled.cancelled = true;
            cancelled.event = 3;
            if (!touch_queue_send_latest(&cancelled, pdMS_TO_TICKS(20))) {
                ESP_LOGW(TAG, "touch cancel queue send failed");
            }
            was_touched = false;
            ESP_LOGW(TAG,
                     "active touch cancelled after I2C read failure: %s",
                     esp_err_to_name(read_err));
        }
    }
}

esp_err_t nuwax_touch_init(void) {
    if (atomic_load_explicit(&s_initialized, memory_order_acquire)) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(ensure_touch_io_lock(), TAG,
                        "touch I/O lock create failed");
    ESP_RETURN_ON_FALSE(touch_take_io_lock(NUWAX_TOUCH_IO_LOCK_TIMEOUT_MS,
                                           "init"),
                        ESP_ERR_TIMEOUT, TAG,
                        "touch init lock timeout");
    if (atomic_load_explicit(&s_initialized, memory_order_acquire)) {
        xSemaphoreGive(s_touch_io_lock);
        return ESP_OK;
    }
    const int64_t now_us = esp_timer_get_time();
    if (now_us < s_next_init_retry_us) {
        xSemaphoreGive(s_touch_io_lock);
        return ESP_ERR_NOT_FINISHED;
    }
    if (!s_touch_queue) {
        s_touch_queue = xQueueCreate(8, sizeof(nuwax_touch_point_t));
        if (!s_touch_queue) {
            const uint32_t retry_ms = touch_schedule_init_retry_locked();
            xSemaphoreGive(s_touch_io_lock);
            ESP_LOGE(TAG, "touch queue create failed retry_ms=%lu",
                     (unsigned long)retry_ms);
            return ESP_ERR_NO_MEM;
        }
    }

    gpio_config_t int_cfg = {
        .pin_bit_mask = 1ULL << CONFIG_NUWAX_TOUCH_PIN_INT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    esp_err_t err = gpio_config(&int_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch int gpio config failed: %s", esp_err_to_name(err));
        goto fail_locked;
    }
    touch_log_pin_levels("before_reset");
    err = touch_reset();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch reset failed: %s", esp_err_to_name(err));
        goto fail_locked;
    }
    touch_log_pin_levels("after_reset");

    err = nuwax_i2c_bus_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "shared i2c bus init failed: %s", esp_err_to_name(err));
        goto fail_locked;
    }
    uint8_t version = 0;
    uint8_t version_info[3] = {0};
    err = touch_connect_locked(&version, version_info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CST816-family touch initialization failed: %s", esp_err_to_name(err));
        goto fail_locked;
    }
    s_next_init_retry_us = 0;
    s_next_recovery_us = 0;
    s_init_failure_streak = 0;
    atomic_store_explicit(&s_initialized, true, memory_order_release);
    const uint8_t selected_address = s_touch_address;
    xSemaphoreGive(s_touch_io_lock);
    ESP_LOGI(TAG,
             "CST816-family touch initialized addr=0x%02x sda=%d scl=%d rst=%d int=%d version=0x%02x info=%02x.%02x.%02x",
             selected_address,
             CONFIG_NUWAX_TOUCH_PIN_SDA,
             CONFIG_NUWAX_TOUCH_PIN_SCL,
             CONFIG_NUWAX_TOUCH_PIN_RST,
             CONFIG_NUWAX_TOUCH_PIN_INT,
             version,
             version_info[0],
             version_info[1],
             version_info[2]);
    return ESP_OK;

fail_locked: {
        atomic_store_explicit(&s_initialized, false, memory_order_release);
        s_next_recovery_us = 0;
        const uint32_t retry_ms = err == ESP_ERR_NOT_FINISHED
                                      ? touch_schedule_busy_retry_locked()
                                      : touch_schedule_init_retry_locked();
        xSemaphoreGive(s_touch_io_lock);
        ESP_LOGW(TAG,
                 "touch initialization deferred retry_ms=%lu streak=%lu err=%s",
                 (unsigned long)retry_ms,
                 (unsigned long)s_init_failure_streak,
                 esp_err_to_name(err));
        return err;
    }
}

esp_err_t nuwax_touch_get_status(nuwax_touch_status_t *status) {
    ESP_RETURN_ON_FALSE(status, ESP_ERR_INVALID_ARG, TAG, "status is NULL");
    memset(status, 0, sizeof(*status));
    ESP_RETURN_ON_ERROR(ensure_touch_io_lock(), TAG,
                        "touch status lock unavailable");
    ESP_RETURN_ON_FALSE(touch_take_io_lock(
                            NUWAX_TOUCH_STATUS_LOCK_TIMEOUT_MS,
                            "get_status"),
                        ESP_ERR_TIMEOUT, TAG,
                        "touch status lock timeout");
    status->initialized = atomic_load_explicit(&s_initialized,
                                               memory_order_acquire);
    status->controller_ready = s_touch_dev != NULL;
    status->event_producer_ready = s_touch_log_task != NULL;
    status->i2c_address = s_touch_address;
    xSemaphoreGive(s_touch_io_lock);
    return ESP_OK;
}

esp_err_t nuwax_touch_read(nuwax_touch_point_t *point) {
    ESP_RETURN_ON_FALSE(point, ESP_ERR_INVALID_ARG, TAG, "point is NULL");
    memset(point, 0, sizeof(*point));
    const esp_err_t init_err = nuwax_touch_init();
    if (init_err != ESP_OK) {
        return init_err;
    }

    ESP_RETURN_ON_FALSE(touch_take_io_lock(NUWAX_TOUCH_IO_LOCK_TIMEOUT_MS,
                                           "read"),
                        ESP_ERR_TIMEOUT, TAG,
                        "touch read lock timeout");
    memset(s_touch_read_scratch, 0, sizeof(s_touch_read_scratch));
    esp_err_t read_err = touch_read_with_recovery_locked(CST816D_REG_TOUCH_DATA,
                                                         s_touch_read_scratch,
                                                         sizeof(s_touch_read_scratch));
    if (read_err != ESP_OK) {
        xSemaphoreGive(s_touch_io_lock);
        if (read_err != ESP_ERR_NOT_FINISHED) {
            ESP_LOGW(TAG, "nuwax_touch_read failed: %s", esp_err_to_name(read_err));
        }
        return read_err;
    }

    point->gesture = s_touch_read_scratch[0];
    point->points = s_touch_read_scratch[1] & 0x0f;
    point->event = s_touch_read_scratch[2] >> 6;
    point->x = (uint16_t)(((s_touch_read_scratch[2] & 0x0f) << 8) | s_touch_read_scratch[3]);
    point->y = (uint16_t)(((s_touch_read_scratch[4] & 0x0f) << 8) | s_touch_read_scratch[5]);
    /* Vendor V2.0 config is native 360x360 with no swap or mirroring. */
    point->touched = point->points > 0 &&
                     point->x < NUWAX_TOUCH_PANEL_WIDTH &&
                     point->y < NUWAX_TOUCH_PANEL_HEIGHT;
    xSemaphoreGive(s_touch_io_lock);
    return ESP_OK;
}

esp_err_t nuwax_touch_set_event_callback(nuwax_touch_event_cb_t callback, void *user_ctx) {
    ESP_RETURN_ON_ERROR(nuwax_touch_init(), TAG, "touch init failed");
    taskENTER_CRITICAL(&s_touch_callback_mux);
    s_event_callback = callback;
    s_event_user_ctx = user_ctx;
    taskEXIT_CRITICAL(&s_touch_callback_mux);
    return ESP_OK;
}

esp_err_t nuwax_touch_poll_event(nuwax_touch_point_t *point, uint32_t timeout_ms) {
    ESP_RETURN_ON_FALSE(point, ESP_ERR_INVALID_ARG, TAG, "point is NULL");
    esp_err_t err = nuwax_touch_init();
    if (err != ESP_OK) {
        return err;
    }
    err = nuwax_touch_start_log_task();
    if (err != ESP_OK) {
        return err;
    }
    if (!s_touch_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    return xQueueReceive(s_touch_queue, point, pdMS_TO_TICKS(timeout_ms)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t nuwax_touch_start_log_task(void) {
    ESP_RETURN_ON_ERROR(nuwax_touch_init(), TAG, "touch init failed");
    ESP_RETURN_ON_ERROR(ensure_touch_io_lock(), TAG,
                        "touch task lock unavailable");
    ESP_RETURN_ON_FALSE(touch_take_io_lock(NUWAX_TOUCH_IO_LOCK_TIMEOUT_MS,
                                           "start_task"),
                        ESP_ERR_TIMEOUT, TAG,
                        "touch task lock timeout");
    if (s_touch_log_task) {
        xSemaphoreGive(s_touch_io_lock);
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreateWithCaps(touch_log_task,
                                        "nuwax_touch",
                                        CONFIG_NUWAX_TOUCH_TASK_STACK_SIZE,
                                        NULL,
                                        5,
                                        &s_touch_log_task,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        s_touch_log_task = NULL;
        xSemaphoreGive(s_touch_io_lock);
        ESP_LOGE(TAG, "touch task create failed");
        return ESP_ERR_NO_MEM;
    }
    esp_err_t isr_err = gpio_install_isr_service(0);
    if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG,
                 "touch GPIO ISR service unavailable; using fallback poll: %s",
                 esp_err_to_name(isr_err));
    } else {
        isr_err = gpio_isr_handler_add(CONFIG_NUWAX_TOUCH_PIN_INT,
                                       touch_int_isr,
                                       NULL);
        if (isr_err == ESP_OK) {
            s_touch_isr_registered = true;
            (void)gpio_intr_enable(CONFIG_NUWAX_TOUCH_PIN_INT);
        } else {
            ESP_LOGW(TAG,
                     "touch INT handler unavailable; using fallback poll: %s",
                     esp_err_to_name(isr_err));
        }
    }
    xSemaphoreGive(s_touch_io_lock);
    ESP_LOGI(TAG,
             "touch event task started mode=%s active_poll_ms=%u idle_fallback_ms=%u",
             s_touch_isr_registered ? "interrupt+fallback" : "fallback-poll",
             (unsigned)CONFIG_NUWAX_TOUCH_POLL_INTERVAL_MS,
             (unsigned)NUWAX_TOUCH_IDLE_FALLBACK_POLL_MS);
    return ESP_OK;
}

esp_err_t nuwax_touch_start_p1_diag_task(void) {
    if (s_p1_diag_task_started) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(touch_diag_config_input(CONFIG_NUWAX_TOUCH_PIN_INT, true),
                        TAG,
                        "P1 touch diag int gpio config failed");
    ESP_RETURN_ON_ERROR(touch_diag_config_input(NUWAX_TOUCH_WAKE_DIAG_GPIO, false),
                        TAG,
                        "P1 touch diag wake gpio config failed");
    ESP_RETURN_ON_ERROR(nuwax_i2c_bus_init(), TAG, "P1 touch diag shared i2c init failed");
    ESP_RETURN_ON_ERROR(nuwax_touch_init(), TAG, "P1 touch diag controller init failed");
    touch_log_pin_levels("p1_diag_start");
    touch_log_candidate_registers();

    BaseType_t ok = xTaskCreate(p1_touch_diag_task,
                                "p1_touch_diag",
                                CONFIG_NUWAX_TOUCH_TASK_STACK_SIZE,
                                NULL,
                                4,
                                NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "P1 touch diag task create failed");
    s_p1_diag_task_started = true;
    return ESP_OK;
}

#else

esp_err_t nuwax_touch_init(void) {
    return ESP_OK;
}

esp_err_t nuwax_touch_get_status(nuwax_touch_status_t *status) {
    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(status, 0, sizeof(*status));
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t nuwax_touch_read(nuwax_touch_point_t *point) {
    if (point) {
        memset(point, 0, sizeof(*point));
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t nuwax_touch_set_event_callback(nuwax_touch_event_cb_t callback, void *user_ctx) {
    (void)callback;
    (void)user_ctx;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t nuwax_touch_poll_event(nuwax_touch_point_t *point, uint32_t timeout_ms) {
    (void)timeout_ms;
    if (point) {
        memset(point, 0, sizeof(*point));
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t nuwax_touch_start_log_task(void) {
    return ESP_OK;
}

esp_err_t nuwax_touch_start_p1_diag_task(void) {
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
