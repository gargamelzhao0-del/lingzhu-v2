#include "board_drivers.h"

#include <string.h>

#include "board/esp_vocat_v1_0_pins.h"
#include "board/i2c_bus_manager.h"
#include "driver/i2c_master.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "qmi8658a";

/* QMI8658A register layout. The CTRL2/CTRL3 range fields are bits 6:4 and
 * ODR is bits 3:0. CTRL7 enables the sensors; it is not a reset register. */
#define QMI8658A_REG_WHO_AM_I   0x00
#define QMI8658A_REG_REVISION   0x01
#define QMI8658A_REG_CTRL1      0x02
#define QMI8658A_REG_CTRL2      0x03
#define QMI8658A_REG_CTRL3      0x04
#define QMI8658A_REG_CTRL7      0x08
#define QMI8658A_REG_STATUS     0x2e
#define QMI8658A_REG_AX_L       0x35

#define QMI8658A_WHO_AM_I       0x05
#define QMI8658A_CTRL1_VENDOR_V2 0x60
#define QMI8658A_ENABLE_ACCEL_GYRO 0x03
#define QMI8658A_RANGE_SHIFT    4
#define QMI8658A_RANGE_MASK     0x70
#define QMI8658A_ODR_MASK       0x0f
#define QMI8658A_DEFAULT_ODR_CODE 0x05 /* vendor V2.0 profile: 250 Hz */
#define QMI8658A_I2C_HZ         400000
#define QMI8658A_TIMEOUT_MS     100
#define QMI8658A_BUS_LOCK_TIMEOUT_MS (QMI8658A_TIMEOUT_MS + 100U)
/* Runtime samples are optional; initialization/configuration retain their
 * longer timeout. Never occupy the touch bus for a 100 ms polling retry. */
#define QMI8658A_SAMPLE_TIMEOUT_MS 10
#define QMI8658A_SAMPLE_LOCK_TIMEOUT_MS 10U

typedef struct {
    i2c_master_dev_handle_t dev;
    SemaphoreHandle_t lock;
    uint8_t address;
    uint8_t accel_range;
    uint8_t gyro_range;
    float accel_scale;
    float gyro_scale;
    bool initialized;
} qmi8658a_context_t;

static qmi8658a_context_t s_qmi;
static DRAM_ATTR uint8_t s_qmi_tx_scratch[2];
static DRAM_ATTR uint8_t s_qmi_rx_scratch[12];
static SemaphoreHandle_t s_init_lock;
static StaticSemaphore_t s_init_lock_storage;
static portMUX_TYPE s_init_lock_mux = portMUX_INITIALIZER_UNLOCKED;

static esp_err_t ensure_init_lock(void)
{
    taskENTER_CRITICAL(&s_init_lock_mux);
    if (!s_init_lock) {
        s_init_lock = xSemaphoreCreateMutexStatic(&s_init_lock_storage);
    }
    const SemaphoreHandle_t lock = s_init_lock;
    taskEXIT_CRITICAL(&s_init_lock_mux);
    return lock ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t write_reg(uint8_t reg, uint8_t value)
{
    if (!s_qmi.dev) {
        return ESP_ERR_INVALID_STATE;
    }
    s_qmi_tx_scratch[0] = reg;
    s_qmi_tx_scratch[1] = value;
    esp_err_t err = nuwax_i2c_bus_transaction_begin(
        QMI8658A_BUS_LOCK_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    err = i2c_master_transmit(s_qmi.dev, s_qmi_tx_scratch,
                              sizeof(s_qmi_tx_scratch), QMI8658A_TIMEOUT_MS);
    nuwax_i2c_bus_transaction_end();
    return err;
}

static esp_err_t read_reg(uint8_t reg, uint8_t *value)
{
    if (!s_qmi.dev || !value) {
        return ESP_ERR_INVALID_ARG;
    }
    s_qmi_tx_scratch[0] = reg;
    s_qmi_rx_scratch[0] = 0;
    esp_err_t err = nuwax_i2c_bus_transaction_begin(
        QMI8658A_BUS_LOCK_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    err = i2c_master_transmit_receive(
        s_qmi.dev, s_qmi_tx_scratch, 1, s_qmi_rx_scratch, 1,
        QMI8658A_TIMEOUT_MS);
    nuwax_i2c_bus_transaction_end();
    if (err == ESP_OK) {
        *value = s_qmi_rx_scratch[0];
    }
    return err;
}

static esp_err_t read_regs(uint8_t reg, uint8_t *data, size_t len)
{
    if (!s_qmi.dev || !data || len == 0 || len > sizeof(s_qmi_rx_scratch)) {
        return ESP_ERR_INVALID_ARG;
    }
    s_qmi_tx_scratch[0] = reg;
    memset(s_qmi_rx_scratch, 0, len);
    /* One bounded scheduler slice breaks phase-locked collisions with touch.
     * Combined with the 10 ms transfer deadline this fits the 50 ms sampler;
     * the worker independently rejects any end-to-end stale sample. */
    esp_err_t err = nuwax_i2c_bus_transaction_begin(QMI8658A_SAMPLE_LOCK_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err == ESP_ERR_TIMEOUT ? ESP_ERR_NOT_FINISHED : err;
    }
    err = i2c_master_transmit_receive(
        s_qmi.dev, s_qmi_tx_scratch, 1, s_qmi_rx_scratch, len,
        QMI8658A_SAMPLE_TIMEOUT_MS);
    nuwax_i2c_bus_sample_transfer_done(err);
    nuwax_i2c_bus_transaction_end();
    if (err == ESP_OK) {
        memcpy(data, s_qmi_rx_scratch, len);
    }
    return err;
}

static uint8_t odr_code_from_hz(uint16_t hz)
{
    if (hz == 0 || hz >= 250) return QMI8658A_DEFAULT_ODR_CODE;
    if (hz >= 125) return 0x06;
    if (hz >= 62) return 0x07;
    return 0x08;
}

static float accel_scale_for_range(uint8_t range)
{
    static const float scale[] = {0.061035f, 0.122070f, 0.244141f, 0.488281f};
    return range < 4 ? scale[range] : scale[QMI8658A_ACCEL_RANGE_8G];
}

static float gyro_scale_for_range(uint8_t range)
{
    static const float scale[] = {
        0.000488281f, 0.000976563f, 0.001953125f, 0.003906250f,
        0.007812500f, 0.015625000f, 0.031250000f, 0.062500000f,
    };
    return range < 8 ? scale[range] : scale[QMI8658A_GYRO_RANGE_256DPS];
}

static int16_t le16(const uint8_t *p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

esp_err_t qmi8658a_init(const qmi8658a_config_t *config)
{
    esp_err_t err = ensure_init_lock();
    if (err != ESP_OK) return err;
    xSemaphoreTake(s_init_lock, portMAX_DELAY);
    if (s_qmi.initialized) {
        xSemaphoreGive(s_init_lock);
        return ESP_OK;
    }
    if (config && config->use_interrupt) {
        xSemaphoreGive(s_init_lock);
        ESP_LOGE(TAG, "interrupt mode is not implemented; use polling mode");
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (!s_qmi.lock) {
        s_qmi.lock = xSemaphoreCreateMutex();
        if (!s_qmi.lock) {
            xSemaphoreGive(s_init_lock);
            return ESP_ERR_NO_MEM;
        }
    }

    uint8_t address = config && config->i2c_address
                          ? config->i2c_address
                          : NUWAX_VOCAT_I2C_ADDR_QMI8658A;
    if (s_qmi.dev && s_qmi.address != address) {
        err = ESP_ERR_INVALID_STATE;
        goto fail;
    }
    if (!s_qmi.dev) {
        err = nuwax_i2c_bus_add_device(address, QMI8658A_I2C_HZ,
                                       &s_qmi.dev);
        if (err != ESP_OK) {
            goto fail;
        }
        s_qmi.address = address;
    }

    uint8_t who = 0;
    uint8_t revision = 0;
    err = read_reg(QMI8658A_REG_WHO_AM_I, &who);
    if (err != ESP_OK || who != QMI8658A_WHO_AM_I) {
        err = err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
        goto fail;
    }
    (void)read_reg(QMI8658A_REG_REVISION, &revision);

    const uint8_t accel_range = config ? config->accel_range : QMI8658A_ACCEL_RANGE_8G;
    const uint8_t gyro_range = config ? config->gyro_range : QMI8658A_GYRO_RANGE_256DPS;
    if (accel_range > QMI8658A_ACCEL_RANGE_16G ||
        gyro_range > QMI8658A_GYRO_RANGE_2048DPS) {
        err = ESP_ERR_INVALID_ARG;
        goto fail;
    }
    const uint8_t accel_odr = odr_code_from_hz(config ? config->accel_odr : 0);
    const uint8_t gyro_odr = odr_code_from_hz(config ? config->gyro_odr : 0);

    /* This sequence is identical to the vendor V2.0 board implementation. */
    if ((err = write_reg(QMI8658A_REG_CTRL1, QMI8658A_CTRL1_VENDOR_V2)) != ESP_OK ||
        (err = write_reg(QMI8658A_REG_CTRL2,
                         (uint8_t)((accel_range << QMI8658A_RANGE_SHIFT) | accel_odr))) != ESP_OK ||
        (err = write_reg(QMI8658A_REG_CTRL3,
                         (uint8_t)((gyro_range << QMI8658A_RANGE_SHIFT) | gyro_odr))) != ESP_OK ||
        (err = write_reg(QMI8658A_REG_CTRL7, QMI8658A_ENABLE_ACCEL_GYRO)) != ESP_OK) {
        goto fail;
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    s_qmi.accel_range = accel_range;
    s_qmi.gyro_range = gyro_range;
    s_qmi.accel_scale = accel_scale_for_range(accel_range);
    s_qmi.gyro_scale = gyro_scale_for_range(gyro_range);
    s_qmi.initialized = true;
    xSemaphoreGive(s_init_lock);
    ESP_LOGI(TAG,
             "ready addr=0x%02x who=0x%02x revision=0x%02x accel_range_code=%u gyro_range_code=%u odr=%u/%u",
             address, who, revision, accel_range, gyro_range, accel_odr, gyro_odr);
    return ESP_OK;

fail:
    s_qmi.initialized = false;
    xSemaphoreGive(s_init_lock);
    ESP_LOGE(TAG, "initialization failed: %s", esp_err_to_name(err));
    return err;
}

esp_err_t qmi8658a_read_data(qmi8658a_sensor_data_t *data)
{
    if (!data) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_qmi.initialized || !s_qmi.lock || !s_qmi.dev) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Sampling is optional. Configuration owns this mutex too; never queue
     * the polling worker indefinitely behind a slow control operation. */
    if (xSemaphoreTake(s_qmi.lock, 0) != pdTRUE) {
        memset(data, 0, sizeof(*data));
        return ESP_ERR_NOT_FINISHED;
    }
    uint8_t raw[12] = {0};
    esp_err_t err = read_regs(QMI8658A_REG_AX_L, raw, sizeof(raw));
    if (err == ESP_OK) {
        data->accel_x = le16(&raw[0]);
        data->accel_y = le16(&raw[2]);
        data->accel_z = le16(&raw[4]);
        data->gyro_x = le16(&raw[6]);
        data->gyro_y = le16(&raw[8]);
        data->gyro_z = le16(&raw[10]);
        data->accel_scale = s_qmi.accel_scale;
        data->gyro_scale = s_qmi.gyro_scale;
        data->timestamp_us = (uint64_t)esp_timer_get_time();
    } else {
        memset(data, 0, sizeof(*data));
    }
    xSemaphoreGive(s_qmi.lock);
    return err;
}

bool qmi8658a_is_present(void)
{
    return s_qmi.initialized;
}

esp_err_t qmi8658a_get_device_id(uint8_t *id)
{
    if (!id) return ESP_ERR_INVALID_ARG;
    if (!s_qmi.initialized || !s_qmi.lock || !s_qmi.dev) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_qmi.lock, portMAX_DELAY);
    esp_err_t err = read_reg(QMI8658A_REG_WHO_AM_I, id);
    xSemaphoreGive(s_qmi.lock);
    return err;
}

static esp_err_t set_range(uint8_t reg, uint8_t range, bool accel)
{
    if (!s_qmi.initialized || !s_qmi.lock || !s_qmi.dev) return ESP_ERR_INVALID_STATE;
    if ((accel && range > QMI8658A_ACCEL_RANGE_16G) ||
        (!accel && range > QMI8658A_GYRO_RANGE_2048DPS)) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_qmi.lock, portMAX_DELAY);
    uint8_t value = 0;
    esp_err_t err = read_reg(reg, &value);
    if (err == ESP_OK) {
        value = (uint8_t)((value & ~QMI8658A_RANGE_MASK) | (range << QMI8658A_RANGE_SHIFT));
        err = write_reg(reg, value);
    }
    if (err == ESP_OK) {
        if (accel) {
            s_qmi.accel_range = range;
            s_qmi.accel_scale = accel_scale_for_range(range);
        } else {
            s_qmi.gyro_range = range;
            s_qmi.gyro_scale = gyro_scale_for_range(range);
        }
    }
    xSemaphoreGive(s_qmi.lock);
    return err;
}

esp_err_t qmi8658a_set_accel_range(uint8_t range)
{
    return set_range(QMI8658A_REG_CTRL2, range, true);
}

esp_err_t qmi8658a_set_gyro_range(uint8_t range)
{
    return set_range(QMI8658A_REG_CTRL3, range, false);
}

esp_err_t qmi8658a_enter_low_power(void)
{
    if (!s_qmi.initialized || !s_qmi.lock || !s_qmi.dev) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_qmi.lock, portMAX_DELAY);
    esp_err_t err = write_reg(QMI8658A_REG_CTRL7, 0x00);
    xSemaphoreGive(s_qmi.lock);
    return err;
}

esp_err_t qmi8658a_wakeup(void)
{
    if (!s_qmi.initialized || !s_qmi.lock || !s_qmi.dev) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_qmi.lock, portMAX_DELAY);
    esp_err_t err = write_reg(QMI8658A_REG_CTRL7, QMI8658A_ENABLE_ACCEL_GYRO);
    xSemaphoreGive(s_qmi.lock);
    return err;
}

esp_err_t qmi8658a_deinit(void)
{
    if (!s_qmi.initialized || !s_qmi.lock) return ESP_OK;
    xSemaphoreTake(s_qmi.lock, portMAX_DELAY);
    esp_err_t err = write_reg(QMI8658A_REG_CTRL7, 0x00);
    esp_err_t remove_err = nuwax_i2c_bus_remove_device(s_qmi.dev);
    s_qmi.dev = NULL;
    s_qmi.address = 0;
    s_qmi.initialized = false;
    xSemaphoreGive(s_qmi.lock);
    vSemaphoreDelete(s_qmi.lock);
    s_qmi.lock = NULL;
    return err != ESP_OK ? err : remove_err;
}
