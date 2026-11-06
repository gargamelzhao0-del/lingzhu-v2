/* Shared I2C bus manager for ESP-VoCat bring-up (P1-3). */
#include "board/i2c_bus_manager.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>

#include "board/esp_vocat_v1_0_pins.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "nuwax_i2c";

static i2c_master_bus_handle_t s_i2c_bus;
typedef enum {
    NUWAX_I2C_UNINITIALIZED = 0,
    NUWAX_I2C_INITIALIZING,
    NUWAX_I2C_READY,
} nuwax_i2c_init_state_t;
static atomic_int s_i2c_state = ATOMIC_VAR_INIT(NUWAX_I2C_UNINITIALIZED);
static atomic_bool s_scan_logged = ATOMIC_VAR_INIT(false);
static SemaphoreHandle_t s_recovery_lock;
static StaticSemaphore_t s_recovery_lock_storage;
static portMUX_TYPE s_recovery_lock_init_mux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_transaction_lock;
static StaticSemaphore_t s_transaction_lock_storage;
static portMUX_TYPE s_transaction_lock_init_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_stats_mux = portMUX_INITIALIZER_UNLOCKED;
static nuwax_i2c_bus_stats_t s_stats;
static int64_t s_transaction_started_us; /* Protected by transaction lock. */
static int64_t s_sample_driver_done_us;
static esp_err_t s_sample_result;
static nuwax_i2c_client_t s_transaction_client;
static int64_t s_driver_done_us;
static esp_err_t s_driver_result;

void nuwax_i2c_bus_get_stats(nuwax_i2c_bus_stats_t *stats)
{
    if (!stats) return;
    taskENTER_CRITICAL(&s_stats_mux);
    *stats = s_stats;
    s_stats.sample_window_driver_max_us = 0U;
    s_stats.sample_window_release_max_us = 0U;
    memset(s_stats.clients, 0, sizeof(s_stats.clients));
    taskEXIT_CRITICAL(&s_stats_mux);
}

#if CONFIG_NUWAX_BOARD_ESP_VOCAT_V1_0
#define NUWAX_I2C_SCL_GPIO NUWAX_VOCAT_I2C_SCL_GPIO
#define NUWAX_I2C_SDA_GPIO NUWAX_VOCAT_I2C_SDA_GPIO
#else
#define NUWAX_I2C_SCL_GPIO CONFIG_NUWAX_TOUCH_PIN_SCL
#define NUWAX_I2C_SDA_GPIO CONFIG_NUWAX_TOUCH_PIN_SDA
#endif

#ifndef CONFIG_NUWAX_TOUCH_I2C_TIMEOUT_MS
#define CONFIG_NUWAX_TOUCH_I2C_TIMEOUT_MS 100
#endif

#define NUWAX_I2C_RECOVERY_LOCK_TIMEOUT_MS 500U
#define NUWAX_I2C_RECOVERY_SETTLE_MS 10U

static esp_err_t ensure_recovery_lock(void)
{
    taskENTER_CRITICAL(&s_recovery_lock_init_mux);
    if (!s_recovery_lock) {
        s_recovery_lock =
            xSemaphoreCreateMutexStatic(&s_recovery_lock_storage);
    }
    const SemaphoreHandle_t lock = s_recovery_lock;
    taskEXIT_CRITICAL(&s_recovery_lock_init_mux);
    return lock ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t ensure_transaction_lock(void)
{
    taskENTER_CRITICAL(&s_transaction_lock_init_mux);
    if (!s_transaction_lock) {
        s_transaction_lock =
            xSemaphoreCreateMutexStatic(&s_transaction_lock_storage);
    }
    const SemaphoreHandle_t lock = s_transaction_lock;
    taskEXIT_CRITICAL(&s_transaction_lock_init_mux);
    return lock ? ESP_OK : ESP_ERR_NO_MEM;
}

static TickType_t delay_ticks_ceil(uint32_t delay_ms)
{
    const uint64_t ticks =
        ((uint64_t)delay_ms * (uint64_t)configTICK_RATE_HZ + 999ULL) /
        1000ULL;
    return (TickType_t)(ticks > 0U ? ticks : 1U);
}

esp_err_t nuwax_i2c_bus_init(void)
{
    const TickType_t wait_started = xTaskGetTickCount();
    for (;;) {
        int state = atomic_load_explicit(&s_i2c_state, memory_order_acquire);
        if (state == NUWAX_I2C_READY) {
            return ESP_OK;
        }

        if (state == NUWAX_I2C_UNINITIALIZED) {
            int expected = NUWAX_I2C_UNINITIALIZED;
            if (atomic_compare_exchange_strong_explicit(
                    &s_i2c_state, &expected, NUWAX_I2C_INITIALIZING,
                    memory_order_acq_rel, memory_order_acquire)) {
                i2c_master_bus_handle_t bus = NULL;
                const i2c_master_bus_config_t bus_cfg = {
                    .i2c_port = I2C_NUM_0,
                    .sda_io_num = NUWAX_I2C_SDA_GPIO,
                    .scl_io_num = NUWAX_I2C_SCL_GPIO,
                    .clk_source = I2C_CLK_SRC_DEFAULT,
                    .glitch_ignore_cnt = 7,
                    .intr_priority = 0,
                    .trans_queue_depth = 0,
                    .flags.enable_internal_pullup = true,
                };

                const esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
                if (err != ESP_OK) {
                    atomic_store_explicit(&s_i2c_state,
                                          NUWAX_I2C_UNINITIALIZED,
                                          memory_order_release);
                    ESP_LOGE(TAG, "shared I2C bus init failed: %s",
                             esp_err_to_name(err));
                    return err;
                }
                s_i2c_bus = bus;
                if (ensure_transaction_lock() != ESP_OK ||
                    ensure_recovery_lock() != ESP_OK) {
                    (void)i2c_del_master_bus(bus);
                    s_i2c_bus = NULL;
                    atomic_store_explicit(&s_i2c_state,
                                          NUWAX_I2C_UNINITIALIZED,
                                          memory_order_release);
                    ESP_LOGE(TAG, "shared I2C synchronization init failed");
                    return ESP_ERR_NO_MEM;
                }
                atomic_store_explicit(&s_i2c_state, NUWAX_I2C_READY,
                                      memory_order_release);
                ESP_LOGI(TAG,
                         "shared I2C bus initialized port=0 scl=%d sda=%d pullup=internal",
                         NUWAX_I2C_SCL_GPIO,
                         NUWAX_I2C_SDA_GPIO);
                return ESP_OK;
            }
            continue;
        }

        if ((xTaskGetTickCount() - wait_started) >= pdMS_TO_TICKS(2000)) {
            ESP_LOGE(TAG, "timed out waiting for shared I2C initialization");
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
}

esp_err_t nuwax_i2c_bus_get(i2c_master_bus_handle_t *out_bus)
{
    ESP_RETURN_ON_FALSE(out_bus, ESP_ERR_INVALID_ARG, TAG, "out_bus is NULL");
    ESP_RETURN_ON_ERROR(nuwax_i2c_bus_init(), TAG, "shared I2C init failed");
    *out_bus = s_i2c_bus;
    return ESP_OK;
}

esp_err_t nuwax_i2c_bus_add_device(uint8_t address,
                                   uint32_t scl_speed_hz,
                                   i2c_master_dev_handle_t *out_dev)
{
    ESP_RETURN_ON_FALSE(out_dev, ESP_ERR_INVALID_ARG, TAG, "out_dev is NULL");
    ESP_RETURN_ON_ERROR(nuwax_i2c_bus_init(), TAG, "shared I2C init failed");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = scl_speed_hz,
        .scl_wait_us = 0,
    };
    esp_err_t err = nuwax_i2c_bus_transaction_begin(500U);
    if (err != ESP_OK) {
        return err;
    }
    err = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, out_dev);
    nuwax_i2c_bus_transaction_end();
    return err;
}

esp_err_t nuwax_i2c_bus_remove_device(i2c_master_dev_handle_t dev)
{
    if (!dev) {
        return ESP_OK;
    }
    esp_err_t err = nuwax_i2c_bus_transaction_begin(500U);
    if (err != ESP_OK) {
        return err;
    }
    err = i2c_master_bus_rm_device(dev);
    nuwax_i2c_bus_transaction_end();
    return err;
}

esp_err_t nuwax_i2c_bus_probe(uint8_t address, int timeout_ms)
{
    ESP_RETURN_ON_ERROR(nuwax_i2c_bus_init(), TAG, "shared I2C init failed");
    esp_err_t err = nuwax_i2c_bus_transaction_begin(
        timeout_ms > 0 ? (uint32_t)timeout_ms : 500U);
    if (err != ESP_OK) {
        return err;
    }
    err = i2c_master_probe(s_i2c_bus, address, timeout_ms);
    nuwax_i2c_bus_transaction_end();
    return err;
}

esp_err_t nuwax_i2c_bus_transaction_begin(uint32_t timeout_ms)
{
    ESP_RETURN_ON_ERROR(nuwax_i2c_bus_init(), TAG,
                        "shared I2C transaction init failed");
    ESP_RETURN_ON_ERROR(ensure_transaction_lock(), TAG,
                        "shared I2C transaction lock unavailable");
    const TickType_t timeout = timeout_ms ? delay_ticks_ceil(timeout_ms) : 0U;
    const int64_t started_us = esp_timer_get_time();
    if (xSemaphoreTake(s_transaction_lock, timeout) != pdTRUE) {
        const char *task_name = pcTaskGetName(NULL);
        taskENTER_CRITICAL(&s_stats_mux);
        if (timeout_ms == 0U) {
            s_stats.try_skips++;
        } else {
            s_stats.lock_timeouts++;
            strlcpy(s_stats.last_timeout_task, task_name,
                    sizeof(s_stats.last_timeout_task));
        }
        taskEXIT_CRITICAL(&s_stats_mux);
        return ESP_ERR_TIMEOUT;
    }
    s_transaction_started_us = esp_timer_get_time();
    s_sample_driver_done_us = 0;
    s_transaction_client = NUWAX_I2C_CLIENT_OTHER;
    s_driver_done_us = 0;
    s_driver_result = ESP_OK;
    const uint32_t waited_us = (uint32_t)(s_transaction_started_us - started_us);
    const char *task_name = pcTaskGetName(NULL);
    taskENTER_CRITICAL(&s_stats_mux);
    s_stats.transactions++;
    s_stats.total_wait_us += waited_us;
    if (waited_us > s_stats.max_wait_us) {
        s_stats.max_wait_us = waited_us;
        strlcpy(s_stats.max_wait_task, task_name, sizeof(s_stats.max_wait_task));
    }
    taskEXIT_CRITICAL(&s_stats_mux);
    return ESP_OK;
}

void nuwax_i2c_bus_transfer_done(nuwax_i2c_client_t client, esp_err_t result)
{
    s_driver_done_us = esp_timer_get_time();
    s_transaction_client = (unsigned)client < NUWAX_I2C_CLIENT_COUNT
                               ? client : NUWAX_I2C_CLIENT_OTHER;
    s_driver_result = result;
}

void nuwax_i2c_bus_sample_transfer_done(esp_err_t result)
{
    nuwax_i2c_bus_transfer_done(NUWAX_I2C_CLIENT_QMI, result);
    s_sample_driver_done_us = s_driver_done_us;
    s_sample_result = result;
}

void nuwax_i2c_bus_transaction_end(void)
{
    if (s_transaction_lock) {
        const uint32_t held_us = (uint32_t)(esp_timer_get_time() - s_transaction_started_us);
        const char *task_name = pcTaskGetName(NULL);
        taskENTER_CRITICAL(&s_stats_mux);
        nuwax_i2c_client_window_t *client = &s_stats.clients[s_transaction_client];
        client->calls++;
        if (held_us > client->hold_max_us) client->hold_max_us = held_us;
        if (held_us > 10000U) client->long_holds++;
        if (s_driver_done_us != 0) {
            const uint32_t driver_us = (uint32_t)(s_driver_done_us - s_transaction_started_us);
            const uint32_t release_us = held_us >= driver_us ? held_us - driver_us : 0U;
            if (s_driver_result != ESP_OK) client->errors++;
            if (driver_us > client->driver_max_us) client->driver_max_us = driver_us;
            if (release_us > client->release_max_us) client->release_max_us = release_us;
        }
        if (s_sample_driver_done_us != 0) {
            const uint32_t driver_us = (uint32_t)(s_sample_driver_done_us - s_transaction_started_us);
            const uint32_t release_us = held_us >= driver_us ? held_us - driver_us : 0U;
            s_stats.sample_calls++;
            if (s_sample_result != ESP_OK) s_stats.sample_errors++;
            if (driver_us > s_stats.sample_driver_max_us) s_stats.sample_driver_max_us = driver_us;
            if (release_us > s_stats.sample_release_max_us) s_stats.sample_release_max_us = release_us;
            if (driver_us > s_stats.sample_window_driver_max_us) s_stats.sample_window_driver_max_us = driver_us;
            if (release_us > s_stats.sample_window_release_max_us) s_stats.sample_window_release_max_us = release_us;
        }
        s_stats.total_hold_us += held_us;
        if (held_us > 10000U) s_stats.long_holds++;
        if (held_us > s_stats.max_hold_us) {
            s_stats.max_hold_us = held_us;
            strlcpy(s_stats.max_hold_task, task_name, sizeof(s_stats.max_hold_task));
        }
        taskEXIT_CRITICAL(&s_stats_mux);
        (void)xSemaphoreGive(s_transaction_lock);
    }
}

esp_err_t nuwax_i2c_bus_recover(const char *reason)
{
    ESP_RETURN_ON_ERROR(nuwax_i2c_bus_init(), TAG,
                        "shared I2C recovery init failed");
    ESP_RETURN_ON_ERROR(ensure_recovery_lock(), TAG,
                        "shared I2C recovery lock unavailable");
    ESP_RETURN_ON_ERROR(ensure_transaction_lock(), TAG,
                        "shared I2C transaction lock unavailable");
    if (xSemaphoreTake(s_recovery_lock,
                       delay_ticks_ceil(NUWAX_I2C_RECOVERY_LOCK_TIMEOUT_MS)) !=
        pdTRUE) {
        ESP_LOGW(TAG, "shared I2C recovery lock timed out reason=%s",
                 reason ? reason : "unspecified");
        return ESP_ERR_TIMEOUT;
    }

    /* Keep all application-owned transfers out while the master bus is reset.
     * This is deliberately a separate lock from the recovery serialization
     * lock: multiple faulting drivers may request recovery, but none may race
     * the reset itself. */
    if (xSemaphoreTake(s_transaction_lock,
                       delay_ticks_ceil(NUWAX_I2C_RECOVERY_LOCK_TIMEOUT_MS)) !=
        pdTRUE) {
        xSemaphoreGive(s_recovery_lock);
        ESP_LOGW(TAG, "shared I2C transaction barrier timed out reason=%s",
                 reason ? reason : "unspecified");
        return ESP_ERR_TIMEOUT;
    }

    const int scl_before = gpio_get_level(NUWAX_I2C_SCL_GPIO);
    const int sda_before = gpio_get_level(NUWAX_I2C_SDA_GPIO);
    ESP_LOGW(TAG,
             "shared I2C recovery begin reason=%s scl=%d sda=%d",
             reason ? reason : "unspecified",
             scl_before,
             sda_before);
    const esp_err_t err = i2c_master_bus_reset(s_i2c_bus);
    taskENTER_CRITICAL(&s_stats_mux);
    s_stats.recoveries++;
    taskEXIT_CRITICAL(&s_stats_mux);
    if (err == ESP_OK) {
        vTaskDelay(delay_ticks_ceil(NUWAX_I2C_RECOVERY_SETTLE_MS));
    }
    const int scl_after = gpio_get_level(NUWAX_I2C_SCL_GPIO);
    const int sda_after = gpio_get_level(NUWAX_I2C_SDA_GPIO);
    xSemaphoreGive(s_transaction_lock);
    xSemaphoreGive(s_recovery_lock);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "shared I2C recovery complete scl=%d sda=%d",
                 scl_after, sda_after);
    } else {
        ESP_LOGE(TAG,
                 "shared I2C recovery failed err=%s scl=%d sda=%d",
                 esp_err_to_name(err), scl_after, sda_after);
    }
    return err;
}

void nuwax_i2c_bus_log_probe(const char *name, uint8_t address, int timeout_ms)
{
    esp_err_t err = nuwax_i2c_bus_probe(address, timeout_ms);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "P1-3 scan: %-8s addr=0x%02x ACK", name ? name : "device", address);
    } else {
        ESP_LOGW(TAG,
                 "P1-3 scan: %-8s addr=0x%02x %s",
                 name ? name : "device",
                 address,
                 esp_err_to_name(err));
    }
}

void nuwax_i2c_bus_log_full_scan(const char *label, int timeout_ms)
{
    if (nuwax_i2c_bus_init() != ESP_OK) {
        return;
    }

    char ack_text[160] = {0};
    size_t used = 0;
    int ack_count = 0;
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        esp_err_t err = nuwax_i2c_bus_probe(addr, timeout_ms);
        if (err != ESP_OK) {
            continue;
        }
        ack_count++;
        int written = snprintf(ack_text + used,
                               sizeof(ack_text) - used,
                               "%s0x%02x",
                               used > 0 ? " " : "",
                               addr);
        if (written < 0) {
            break;
        }
        if ((size_t)written >= sizeof(ack_text) - used) {
            used = sizeof(ack_text) - 1;
            break;
        }
        used += (size_t)written;
    }

    ESP_LOGI(TAG,
             "P1 diag I2C full scan[%s]: ack_count=%d addrs=%s",
             label ? label : "scan",
             ack_count,
             ack_count > 0 ? ack_text : "<none>");
}

static void log_probe_result(const char *name, uint8_t address)
{
    nuwax_i2c_bus_log_probe(name, address, CONFIG_NUWAX_TOUCH_I2C_TIMEOUT_MS);
}

void nuwax_i2c_bus_log_scan_once(void)
{
    if (nuwax_i2c_bus_init() != ESP_OK) {
        return;
    }
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(
            &s_scan_logged, &expected, true,
            memory_order_acq_rel, memory_order_acquire)) {
        return;
    }

#if CONFIG_NUWAX_BOARD_ESP_VOCAT_V1_0
    log_probe_result("touch", 0x15);
    log_probe_result("ES8311", NUWAX_VOCAT_I2C_ADDR_ES8311);
    log_probe_result("ES7210", NUWAX_VOCAT_I2C_ADDR_ES7210);
    log_probe_result("QMI8658A", NUWAX_VOCAT_I2C_ADDR_QMI8658A);
    log_probe_result("QMI8658A_ALT", NUWAX_VOCAT_I2C_ADDR_QMI8658A_ALT);
    log_probe_result("BQ27220", NUWAX_VOCAT_I2C_ADDR_BQ27220);
#else
    log_probe_result("touch", 0x15);
#endif
}
