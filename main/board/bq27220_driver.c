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

static const char *TAG = "bq27220";

/* Standard-command register addresses from the vendor V2.0 BQ27220 driver. */
#define BQ27220_REG_CONTROL                 0x00
#define BQ27220_REG_MAC_DATA                0x40
#define BQ27220_REG_TEMPERATURE             0x06
#define BQ27220_REG_VOLTAGE                 0x08
#define BQ27220_REG_BATTERY_STATUS          0x0a
#define BQ27220_REG_CURRENT                 0x0c
#define BQ27220_REG_REMAINING_CAPACITY      0x10
#define BQ27220_REG_FULL_CHARGE_CAPACITY    0x12
#define BQ27220_REG_CYCLE_COUNT             0x2a
#define BQ27220_REG_STATE_OF_CHARGE         0x2c
#define BQ27220_REG_OPERATION_STATUS         0x3a
#define BQ27220_REG_DESIGN_CAPACITY          0x3c
#define BQ27220_REG_SELECT_SUBCLASS          0x3e
#define BQ27220_REG_MAC_DATA_CHECKSUM        0x60

#define BQ27220_CONTROL_DEVICE_NUMBER       0x0001
#define BQ27220_CONTROL_SEALED              0x0030
#define BQ27220_CONTROL_ENTER_CFG_UPDATE    0x0090
#define BQ27220_CONTROL_EXIT_CFG_REINIT     0x0091
#define BQ27220_UNSEAL_KEY_1                0x0414
#define BQ27220_UNSEAL_KEY_2                0x3672
#define BQ27220_DEVICE_NUMBER               0x0220

#define BQ27220_STATUS_DSG                   (1u << 0)
#define BQ27220_STATUS_TDA                   (1u << 2)
#define BQ27220_STATUS_BATTPRES              (1u << 3)
#define BQ27220_STATUS_CHGINH                (1u << 8)
#define BQ27220_STATUS_FC                    (1u << 9)
#define BQ27220_STATUS_FD                    (1u << 15)
#define BQ27220_OPERATION_INITCOMP            (1u << 5)
#define BQ27220_OPERATION_CFGUPDATE           (1u << 10)
#define BQ27220_OPERATION_SECURITY_SHIFT       1
#define BQ27220_OPERATION_SECURITY_MASK        0x3u
#define BQ27220_SECURITY_FULL_ACCESS           0x1u
#define BQ27220_SECURITY_UNSEALED              0x2u
#define BQ27220_SECURITY_SEALED                0x3u

#define BQ27220_I2C_HZ                       400000
#define BQ27220_TIMEOUT_MS                   100
#define BQ27220_BUS_LOCK_TIMEOUT_MS          (BQ27220_TIMEOUT_MS + 100U)
#define BQ27220_STATE_TIMEOUT_MS             1500
#define BQ27220_DM_COMMIT_DELAY_MS           10
#define BQ27220_DM_WRITE_ATTEMPTS             2
#define BQ27220_DM_FULL_CHARGE_CAPACITY       0x929d

typedef enum {
    BQ27220_DM_U8 = 1,
    BQ27220_DM_U16 = 2,
} bq27220_dm_width_t;

typedef struct {
    uint16_t address;
    uint16_t value;
    bq27220_dm_width_t width;
} bq27220_profile_parameter_t;

/* CyberVoc-Board-V2_0 CEDV Profile 1 for the product's 1200 mAh cell.
 * Keep single-byte parameters as U8 transactions; a U16 transaction at those
 * addresses would overwrite the adjacent Data Memory parameter. */
static const bq27220_profile_parameter_t s_vendor_profile[] = {
    {0x929b, 0x011b, BQ27220_DM_U16}, /* Gauging Configuration */
    {0x929d, 1200, BQ27220_DM_U16},   /* Full Charge Capacity */
    {0x929f, 1200, BQ27220_DM_U16},   /* Design Capacity */
    {0x926b, 1140, BQ27220_DM_U16},   /* Near Full */
    {0x9268, 8, BQ27220_DM_U8},       /* Self Discharge Rate */
    {0x926d, 60, BQ27220_DM_U16},     /* Reserve Capacity */
    {0x92a7, 4050, BQ27220_DM_U16},   /* EMF */
    {0x92a9, 1400, BQ27220_DM_U16},   /* C0 */
    {0x92ab, 55, BQ27220_DM_U16},     /* R0 */
    {0x92ad, 100, BQ27220_DM_U16},    /* T0 */
    {0x92af, 28, BQ27220_DM_U16},     /* R1 */
    {0x92b1, 125, BQ27220_DM_U8},     /* TC */
    {0x92b2, 35, BQ27220_DM_U8},      /* C1 */
    {0x92b4, 3200, BQ27220_DM_U16},   /* EDV0 */
    {0x92b7, 3300, BQ27220_DM_U16},   /* EDV1 */
    {0x92ba, 3400, BQ27220_DM_U16},   /* EDV2 */
    {0x92bd, 4100, BQ27220_DM_U16},   /* DOD0 */
    {0x92bf, 4030, BQ27220_DM_U16},   /* DOD10 */
    {0x92c1, 3970, BQ27220_DM_U16},   /* DOD20 */
    {0x92c3, 3910, BQ27220_DM_U16},   /* DOD30 */
    {0x92c5, 3850, BQ27220_DM_U16},   /* DOD40 */
    {0x92c7, 3800, BQ27220_DM_U16},   /* DOD50 */
    {0x92c9, 3750, BQ27220_DM_U16},   /* DOD60 */
    {0x92cb, 3700, BQ27220_DM_U16},   /* DOD70 */
    {0x92cd, 3620, BQ27220_DM_U16},   /* DOD80 */
    {0x92cf, 3520, BQ27220_DM_U16},   /* DOD90 */
    {0x92d1, 3200, BQ27220_DM_U16},   /* DOD100 */
};

typedef struct {
    i2c_master_dev_handle_t dev;
    SemaphoreHandle_t lock;
    uint16_t design_capacity;
    uint8_t low_threshold;
    bool profile_verified;
    bool initialized;
} bq27220_context_t;

static bq27220_context_t s_bq;
/* I2C payload pointers remain valid even if a timed-out transaction finishes
 * asynchronously in the driver ISR. All accesses are serialized by s_bq.lock. */
static DRAM_ATTR uint8_t s_bq_tx_scratch[5];
static DRAM_ATTR uint8_t s_bq_rx_scratch[2];
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

static esp_err_t read_word(uint8_t reg, uint16_t *value)
{
    if (!s_bq.dev || !value) return ESP_ERR_INVALID_ARG;
    s_bq_tx_scratch[0] = reg;
    memset(s_bq_rx_scratch, 0, sizeof(s_bq_rx_scratch));
    esp_err_t err = nuwax_i2c_bus_transaction_begin(
        BQ27220_BUS_LOCK_TIMEOUT_MS);
    if (err != ESP_OK) return err;
    err = i2c_master_transmit_receive(
        s_bq.dev, s_bq_tx_scratch, 1, s_bq_rx_scratch,
        sizeof(s_bq_rx_scratch), BQ27220_TIMEOUT_MS);
    nuwax_i2c_bus_transaction_end();
    if (err == ESP_OK) {
        *value = (uint16_t)s_bq_rx_scratch[0] |
                 ((uint16_t)s_bq_rx_scratch[1] << 8);
    }
    return err;
}

static esp_err_t write_word(uint8_t reg, uint16_t value)
{
    if (!s_bq.dev) return ESP_ERR_INVALID_STATE;
    s_bq_tx_scratch[0] = reg;
    s_bq_tx_scratch[1] = (uint8_t)value;
    s_bq_tx_scratch[2] = (uint8_t)(value >> 8);
    esp_err_t err = nuwax_i2c_bus_transaction_begin(
        BQ27220_BUS_LOCK_TIMEOUT_MS);
    if (err != ESP_OK) return err;
    err = i2c_master_transmit(s_bq.dev, s_bq_tx_scratch,
                              3, BQ27220_TIMEOUT_MS);
    nuwax_i2c_bus_transaction_end();
    return err;
}

static esp_err_t read_control_subcommand(uint16_t subcommand, uint16_t *result)
{
    esp_err_t err = write_word(BQ27220_REG_CONTROL, subcommand);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(15));
    return read_word(BQ27220_REG_MAC_DATA, result);
}

static esp_err_t read_security_state(uint8_t *state)
{
    if (!state) return ESP_ERR_INVALID_ARG;
    uint16_t operation = 0;
    esp_err_t err = read_word(BQ27220_REG_OPERATION_STATUS, &operation);
    if (err == ESP_OK) {
        *state = (uint8_t)((operation >> BQ27220_OPERATION_SECURITY_SHIFT) &
                           BQ27220_OPERATION_SECURITY_MASK);
    }
    return err;
}

static uint8_t security_state_from_operation(uint16_t operation)
{
    return (uint8_t)((operation >> BQ27220_OPERATION_SECURITY_SHIFT) &
                     BQ27220_OPERATION_SECURITY_MASK);
}

static bool operation_is_cfgupdate(uint16_t operation)
{
    return (operation & BQ27220_OPERATION_CFGUPDATE) != 0;
}

static esp_err_t wait_for_security_state(uint8_t expected, uint32_t timeout_ms)
{
    const int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    esp_err_t last_err = ESP_ERR_TIMEOUT;
    do {
        uint8_t state = 0;
        esp_err_t err = read_security_state(&state);
        if (err == ESP_OK) {
            if (state == expected) return ESP_OK;
            last_err = ESP_ERR_TIMEOUT;
        } else {
            last_err = err;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    } while (esp_timer_get_time() < deadline_us);
    return last_err;
}

static esp_err_t wait_for_cfgupdate_state(bool expected, uint32_t timeout_ms)
{
    const int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    esp_err_t last_err = ESP_ERR_TIMEOUT;
    do {
        uint16_t operation = 0;
        esp_err_t err = read_word(BQ27220_REG_OPERATION_STATUS, &operation);
        if (err == ESP_OK) {
            if (operation_is_cfgupdate(operation) == expected) return ESP_OK;
            last_err = ESP_ERR_TIMEOUT;
        } else {
            last_err = err;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    } while (esp_timer_get_time() < deadline_us);
    return last_err;
}

static esp_err_t restore_sealed_state(bool must_reseal);

static esp_err_t temporarily_unseal(uint8_t initial_state, bool *must_reseal)
{
    if (!must_reseal) return ESP_ERR_INVALID_ARG;
    *must_reseal = false;

    if (initial_state != BQ27220_SECURITY_SEALED) {
        ESP_LOGE(TAG,
                 "refusing Data Memory access from non-sealed security state=%u",
                 initial_state);
        return ESP_ERR_INVALID_RESPONSE;
    }

    esp_err_t err = write_word(BQ27220_REG_CONTROL, BQ27220_UNSEAL_KEY_1);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(10));
        err = write_word(BQ27220_REG_CONTROL, BQ27220_UNSEAL_KEY_2);
    }
    if (err == ESP_OK) {
        err = wait_for_security_state(BQ27220_SECURITY_UNSEALED, 300);
    }
    if (err != ESP_OK) {
        /* A partial key sequence must not leave the gauge protection ambiguous. */
        esp_err_t restore_err = restore_sealed_state(true);
        if (restore_err != ESP_OK) return restore_err;
        return err;
    }

    *must_reseal = true;
    ESP_LOGI(TAG, "temporarily unsealed for Data Memory access");
    return ESP_OK;
}

static esp_err_t restore_sealed_state(bool must_reseal)
{
    if (!must_reseal) return ESP_OK;
    esp_err_t last_err = ESP_FAIL;
    for (int attempt = 0; attempt < 3; attempt++) {
        last_err = write_word(BQ27220_REG_CONTROL, BQ27220_CONTROL_SEALED);
        if (last_err == ESP_OK) {
            last_err = wait_for_security_state(BQ27220_SECURITY_SEALED, 300);
        }
        if (last_err == ESP_OK) {
            ESP_LOGI(TAG, "verified sealed security state");
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGE(TAG, "failed to restore sealed security state: %s",
             esp_err_to_name(last_err));
    return last_err;
}

static esp_err_t restore_normal_sealed_state(bool *restored)
{
    if (!restored) return ESP_ERR_INVALID_ARG;
    *restored = false;

    uint16_t operation = 0;
    esp_err_t err = read_word(BQ27220_REG_OPERATION_STATUS, &operation);
    if (err != ESP_OK) return err;

    uint8_t security = security_state_from_operation(operation);
    if (operation_is_cfgupdate(operation)) {
        ESP_LOGW(TAG,
                 "recovering BQ27220 CFGUPDATE state operation=0x%04x security=%u",
                 operation, security);
        if (security == BQ27220_SECURITY_SEALED) {
            bool must_reseal = false;
            err = temporarily_unseal(security, &must_reseal);
            if (err != ESP_OK) return err;
            security = BQ27220_SECURITY_UNSEALED;
        }
        err = write_word(BQ27220_REG_CONTROL,
                         BQ27220_CONTROL_EXIT_CFG_REINIT);
        if (err == ESP_OK) {
            err = wait_for_cfgupdate_state(false,
                                           BQ27220_STATE_TIMEOUT_MS);
        }
        if (err != ESP_OK) {
            (void)restore_sealed_state(security != BQ27220_SECURITY_SEALED);
            return err;
        }
    }

    err = read_word(BQ27220_REG_OPERATION_STATUS, &operation);
    if (err != ESP_OK) return err;
    security = security_state_from_operation(operation);
    if (security != BQ27220_SECURITY_SEALED) {
        err = restore_sealed_state(true);
        if (err != ESP_OK) return err;
    }

    err = read_word(BQ27220_REG_OPERATION_STATUS, &operation);
    if (err != ESP_OK) return err;
    security = security_state_from_operation(operation);
    *restored = security == BQ27220_SECURITY_SEALED &&
                !operation_is_cfgupdate(operation);
    if (!*restored) {
        ESP_LOGE(TAG,
                 "BQ27220 safety invariant failed operation=0x%04x security=%u cfgupdate=%d",
                 operation, security, operation_is_cfgupdate(operation));
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static esp_err_t select_data_memory(uint16_t address)
{
    if (!s_bq.dev) return ESP_ERR_INVALID_STATE;

    s_bq_tx_scratch[0] = BQ27220_REG_SELECT_SUBCLASS;
    s_bq_tx_scratch[1] = (uint8_t)address;
    s_bq_tx_scratch[2] = (uint8_t)(address >> 8);
    esp_err_t err = nuwax_i2c_bus_transaction_begin(
        BQ27220_BUS_LOCK_TIMEOUT_MS);
    if (err != ESP_OK) return err;
    err = i2c_master_transmit(s_bq.dev, s_bq_tx_scratch, 3,
                              BQ27220_TIMEOUT_MS);
    nuwax_i2c_bus_transaction_end();
    if (err != ESP_OK) return err;

    vTaskDelay(pdMS_TO_TICKS(BQ27220_DM_COMMIT_DELAY_MS));
    s_bq_tx_scratch[0] = BQ27220_REG_SELECT_SUBCLASS;
    memset(s_bq_rx_scratch, 0, sizeof(s_bq_rx_scratch));
    err = nuwax_i2c_bus_transaction_begin(BQ27220_BUS_LOCK_TIMEOUT_MS);
    if (err != ESP_OK) return err;
    err = i2c_master_transmit_receive(s_bq.dev, s_bq_tx_scratch, 1,
                                      s_bq_rx_scratch, sizeof(s_bq_rx_scratch),
                                      BQ27220_TIMEOUT_MS);
    nuwax_i2c_bus_transaction_end();
    if (err != ESP_OK) return err;
    if (s_bq_rx_scratch[0] != (uint8_t)address ||
        s_bq_rx_scratch[1] != (uint8_t)(address >> 8)) {
        ESP_LOGE(TAG,
                 "Data Memory selector stale requested=0x%04x echoed=0x%02x%02x",
                 address, s_bq_rx_scratch[1], s_bq_rx_scratch[0]);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t read_data_memory_value(
    const bq27220_profile_parameter_t *parameter,
    uint16_t *value)
{
    if (!parameter || !value ||
        (parameter->width != BQ27220_DM_U8 &&
         parameter->width != BQ27220_DM_U16)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = select_data_memory(parameter->address);
    if (err != ESP_OK) return err;

    s_bq_tx_scratch[0] = BQ27220_REG_MAC_DATA;
    memset(s_bq_rx_scratch, 0, sizeof(s_bq_rx_scratch));
    err = nuwax_i2c_bus_transaction_begin(BQ27220_BUS_LOCK_TIMEOUT_MS);
    if (err != ESP_OK) return err;
    err = i2c_master_transmit_receive(s_bq.dev, s_bq_tx_scratch, 1,
                                      s_bq_rx_scratch,
                                      (size_t)parameter->width,
                                      BQ27220_TIMEOUT_MS);
    nuwax_i2c_bus_transaction_end();
    if (err == ESP_OK) {
        *value = parameter->width == BQ27220_DM_U8
                     ? s_bq_rx_scratch[0]
                     : ((uint16_t)s_bq_rx_scratch[0] << 8) |
                           s_bq_rx_scratch[1];
    }
    return err;
}

static esp_err_t write_data_memory_value(
    const bq27220_profile_parameter_t *parameter)
{
    if (!parameter ||
        (parameter->width != BQ27220_DM_U8 &&
         parameter->width != BQ27220_DM_U16)) {
        return ESP_ERR_INVALID_ARG;
    }

    s_bq_tx_scratch[0] = BQ27220_REG_SELECT_SUBCLASS;
    s_bq_tx_scratch[1] = (uint8_t)parameter->address;
    s_bq_tx_scratch[2] = (uint8_t)(parameter->address >> 8);
    if (parameter->width == BQ27220_DM_U8) {
        s_bq_tx_scratch[3] = (uint8_t)parameter->value;
    } else {
        s_bq_tx_scratch[3] = (uint8_t)(parameter->value >> 8);
        s_bq_tx_scratch[4] = (uint8_t)parameter->value;
    }
    const size_t transaction_size = 3U + (size_t)parameter->width;
    esp_err_t err = nuwax_i2c_bus_transaction_begin(
        BQ27220_BUS_LOCK_TIMEOUT_MS);
    if (err != ESP_OK) return err;
    err = i2c_master_transmit(s_bq.dev, s_bq_tx_scratch,
                              transaction_size, BQ27220_TIMEOUT_MS);
    nuwax_i2c_bus_transaction_end();
    if (err != ESP_OK) return err;

    vTaskDelay(pdMS_TO_TICKS(BQ27220_DM_COMMIT_DELAY_MS));
    uint8_t sum = 0;
    for (size_t i = 1; i < transaction_size; i++) {
        sum = (uint8_t)(sum + s_bq_tx_scratch[i]);
    }
    s_bq_tx_scratch[0] = BQ27220_REG_MAC_DATA_CHECKSUM;
    s_bq_tx_scratch[1] = (uint8_t)(0xffU - sum);
    s_bq_tx_scratch[2] = (uint8_t)(parameter->width + 4U);
    err = nuwax_i2c_bus_transaction_begin(BQ27220_BUS_LOCK_TIMEOUT_MS);
    if (err != ESP_OK) return err;
    err = i2c_master_transmit(s_bq.dev, s_bq_tx_scratch, 3,
                              BQ27220_TIMEOUT_MS);
    nuwax_i2c_bus_transaction_end();
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(BQ27220_DM_COMMIT_DELAY_MS));
    }
    return err;
}

static esp_err_t read_vendor_profile(bool *matches)
{
    if (!matches) return ESP_ERR_INVALID_ARG;
    *matches = false;
    size_t mismatch_count = 0;
    uint16_t first_address = 0;
    uint16_t first_actual = 0;
    uint16_t first_expected = 0;
    for (size_t i = 0;
         i < sizeof(s_vendor_profile) / sizeof(s_vendor_profile[0]); i++) {
        uint16_t actual = 0;
        esp_err_t err = read_data_memory_value(&s_vendor_profile[i], &actual);
        if (err != ESP_OK) return err;
        bool value_matches = actual == s_vendor_profile[i].value;
        if (s_vendor_profile[i].address ==
            BQ27220_DM_FULL_CHARGE_CAPACITY) {
            /* FCC is learned over the cell lifetime. Correct an obviously
             * foreign profile, but never erase a valid aging result merely
             * because it no longer equals the initial 1200 mAh value. */
            const uint16_t learned_min = s_vendor_profile[i].value / 5U;
            const uint16_t learned_max =
                (uint16_t)(s_vendor_profile[i].value * 5U / 4U);
            value_matches = actual >= learned_min && actual <= learned_max;
        }
        if (!value_matches) {
            if (mismatch_count == 0U) {
                first_address = s_vendor_profile[i].address;
                first_actual = actual;
                first_expected = s_vendor_profile[i].value;
            }
            mismatch_count++;
        }
    }
    *matches = mismatch_count == 0U;
    if (!*matches) {
        ESP_LOGW(TAG,
                 "1200mAh CEDV profile mismatch count=%u first_addr=0x%04x actual=%u expected=%u",
                 (unsigned)mismatch_count, first_address, first_actual,
                 first_expected);
    }
    return ESP_OK;
}

static esp_err_t write_and_verify_profile_parameter(
    const bq27220_profile_parameter_t *parameter)
{
    esp_err_t last_err = ESP_FAIL;
    for (unsigned attempt = 0; attempt < BQ27220_DM_WRITE_ATTEMPTS; attempt++) {
        last_err = write_data_memory_value(parameter);
        uint16_t actual = 0;
        if (last_err == ESP_OK) {
            last_err = read_data_memory_value(parameter, &actual);
        }
        if (last_err == ESP_OK && actual == parameter->value) {
            return ESP_OK;
        }
        if (last_err == ESP_OK) last_err = ESP_ERR_INVALID_RESPONSE;
        vTaskDelay(pdMS_TO_TICKS(BQ27220_DM_COMMIT_DELAY_MS));
    }
    ESP_LOGE(TAG,
             "Data Memory write failed addr=0x%04x expected=%u err=%s",
             parameter->address, parameter->value, esp_err_to_name(last_err));
    return last_err;
}

static esp_err_t write_vendor_profile(void)
{
    esp_err_t err = write_word(BQ27220_REG_CONTROL,
                               BQ27220_CONTROL_ENTER_CFG_UPDATE);
    if (err == ESP_OK) {
        err = wait_for_cfgupdate_state(true, BQ27220_STATE_TIMEOUT_MS);
    }
    if (err != ESP_OK) return err;

    for (size_t i = 0;
         i < sizeof(s_vendor_profile) / sizeof(s_vendor_profile[0]); i++) {
        err = write_and_verify_profile_parameter(&s_vendor_profile[i]);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

static esp_err_t verify_vendor_cedv_profile(bool *verified,
                                            bool *security_restored)
{
    if (!verified || !security_restored) return ESP_ERR_INVALID_ARG;
    *verified = false;
    *security_restored = false;

    esp_err_t err = restore_normal_sealed_state(security_restored);
    if (err != ESP_OK || !*security_restored) return err;

    uint16_t operation = 0;
    err = read_word(BQ27220_REG_OPERATION_STATUS, &operation);
    if (err != ESP_OK) return err;
    bool must_reseal = false;
    err = temporarily_unseal(security_state_from_operation(operation),
                             &must_reseal);
    if (err != ESP_OK) {
        (void)restore_normal_sealed_state(security_restored);
        return err;
    }

    bool matches = false;
    err = read_vendor_profile(&matches);
    if (err == ESP_OK && !matches) {
        ESP_LOGW(TAG, "provisioning CyberVoc V2 1200mAh CEDV profile");
        err = write_vendor_profile();
    }

    bool cleanup_safe = false;
    esp_err_t cleanup_err = restore_normal_sealed_state(&cleanup_safe);
    *security_restored = cleanup_safe;
    if (cleanup_err != ESP_OK || !cleanup_safe) return cleanup_err;
    if (err != ESP_OK) return err;

    if (!matches) {
        err = read_word(BQ27220_REG_OPERATION_STATUS, &operation);
        if (err != ESP_OK) return err;
        must_reseal = false;
        err = temporarily_unseal(security_state_from_operation(operation),
                                 &must_reseal);
        if (err == ESP_OK) {
            err = read_vendor_profile(&matches);
        }
        cleanup_safe = false;
        cleanup_err = restore_normal_sealed_state(&cleanup_safe);
        *security_restored = cleanup_safe;
        if (cleanup_err != ESP_OK || !cleanup_safe) return cleanup_err;
        if (err != ESP_OK) return err;
    }

    if (!matches) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *verified = true;
    ESP_LOGI(TAG,
             "CyberVoc V2 1200mAh CEDV profile verified parameters=%u",
             (unsigned)(sizeof(s_vendor_profile) /
                        sizeof(s_vendor_profile[0])));
    return ESP_OK;
}

static int16_t temperature_c_from_raw(uint16_t raw)
{
    /* Temperature is 0.1 K. Keep integer Celsius in the public API. */
    int32_t deci_c = (int32_t)raw - 2732;
    return (int16_t)(deci_c >= 0 ? (deci_c + 5) / 10 : (deci_c - 5) / 10);
}

esp_err_t bq27220_init(const bq27220_config_t *config)
{
    esp_err_t err = ensure_init_lock();
    if (err != ESP_OK) return err;
    xSemaphoreTake(s_init_lock, portMAX_DELAY);
    if (s_bq.initialized) {
        xSemaphoreGive(s_init_lock);
        return ESP_OK;
    }

    if (!s_bq.lock) {
        s_bq.lock = xSemaphoreCreateMutex();
        if (!s_bq.lock) {
            xSemaphoreGive(s_init_lock);
            return ESP_ERR_NO_MEM;
        }
    }
    const uint8_t address = config && config->i2c_address
                                ? config->i2c_address
                                : NUWAX_VOCAT_I2C_ADDR_BQ27220;
    if (!s_bq.dev) {
        err = nuwax_i2c_bus_add_device(address, BQ27220_I2C_HZ, &s_bq.dev);
        if (err != ESP_OK) goto fail;
    }

    uint16_t device_number = 0;
    err = read_control_subcommand(BQ27220_CONTROL_DEVICE_NUMBER, &device_number);
    if (err != ESP_OK || device_number != BQ27220_DEVICE_NUMBER) {
        ESP_LOGE(TAG, "device number read=0x%04x expected=0x%04x", device_number, BQ27220_DEVICE_NUMBER);
        err = err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
        goto fail;
    }

    s_bq.design_capacity = config && config->design_capacity ? config->design_capacity : 1200;
    s_bq.low_threshold = config && config->low_battery_threshold
                             ? config->low_battery_threshold
                             : 10;
    bool profile_verified = false;
    bool security_restored = false;
    esp_err_t profile_err = verify_vendor_cedv_profile(&profile_verified,
                                                       &security_restored);
    if (!security_restored) {
        ESP_LOGE(TAG,
                 "CEDV verification did not prove security-state restoration; refusing driver startup");
        err = profile_err == ESP_OK ? ESP_ERR_INVALID_STATE : profile_err;
        goto fail;
    }
    s_bq.profile_verified = profile_err == ESP_OK && profile_verified;
    if (profile_err != ESP_OK) {
        ESP_LOGW(TAG, "CEDV verification unavailable; standard battery telemetry remains enabled: %s",
                 esp_err_to_name(profile_err));
    }
    s_bq.initialized = true;
    xSemaphoreGive(s_init_lock);
    ESP_LOGI(TAG,
             "ready addr=0x%02x device=0x%04x design_capacity=%u low_threshold=%u",
             address, device_number, s_bq.design_capacity, s_bq.low_threshold);
    return ESP_OK;

fail:
    /* Retain the device handle and scratch storage after an I2C timeout. A
     * later battery-service retry reuses the same handle after its cooldown. */
    s_bq.initialized = false;
    s_bq.profile_verified = false;
    xSemaphoreGive(s_init_lock);
    ESP_LOGE(TAG, "initialization failed: %s", esp_err_to_name(err));
    return err;
}

static esp_err_t read_locked(uint8_t reg, uint16_t *value)
{
    if (!value) return ESP_ERR_INVALID_ARG;
    if (!s_bq.initialized || !s_bq.lock || !s_bq.dev) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_bq.lock, portMAX_DELAY);
    esp_err_t err = read_word(reg, value);
    xSemaphoreGive(s_bq.lock);
    return err;
}

esp_err_t bq27220_get_status(bq27220_battery_status_t *status)
{
    if (!status) return ESP_ERR_INVALID_ARG;
    if (!s_bq.initialized || !s_bq.lock || !s_bq.dev) return ESP_ERR_INVALID_STATE;
    memset(status, 0, sizeof(*status));

    xSemaphoreTake(s_bq.lock, portMAX_DELAY);
    uint16_t voltage = 0, current = 0, soc = 0, remaining = 0, full = 0;
    uint16_t temperature = 0, cycles = 0, flags = 0, operation = 0, design = 0;
    esp_err_t err = read_word(BQ27220_REG_VOLTAGE, &voltage);
    if (err == ESP_OK) err = read_word(BQ27220_REG_CURRENT, &current);
    if (err == ESP_OK) err = read_word(BQ27220_REG_STATE_OF_CHARGE, &soc);
    if (err == ESP_OK) err = read_word(BQ27220_REG_REMAINING_CAPACITY, &remaining);
    if (err == ESP_OK) err = read_word(BQ27220_REG_FULL_CHARGE_CAPACITY, &full);
    if (err == ESP_OK) err = read_word(BQ27220_REG_TEMPERATURE, &temperature);
    if (err == ESP_OK) err = read_word(BQ27220_REG_CYCLE_COUNT, &cycles);
    if (err == ESP_OK) err = read_word(BQ27220_REG_BATTERY_STATUS, &flags);
    if (err == ESP_OK) err = read_word(BQ27220_REG_OPERATION_STATUS, &operation);
    if (err == ESP_OK) err = read_word(BQ27220_REG_DESIGN_CAPACITY, &design);
    xSemaphoreGive(s_bq.lock);
    if (err != ESP_OK) return err;
    if (soc > 100) {
        ESP_LOGE(TAG, "invalid raw state-of-charge value=%u", soc);
        return ESP_ERR_INVALID_RESPONSE;
    }

    status->voltage_mv = voltage;
    status->current_ma = (int16_t)current;
    status->capacity_percent = soc;
    status->soc = (uint8_t)status->capacity_percent;
    status->remaining_mah = remaining;
    status->full_charge_mah = full;
    status->design_capacity_mah = design;
    status->battery_status_flags = flags;
    status->operation_status_flags = operation;
    status->temperature_c = temperature_c_from_raw(temperature);
    status->cycle_count = cycles;
    status->battery_present = (flags & BQ27220_STATUS_BATTPRES) != 0;
    status->initialization_complete = (operation & BQ27220_OPERATION_INITCOMP) != 0;
    status->config_update_mode = (operation & BQ27220_OPERATION_CFGUPDATE) != 0;
    status->profile_verified = s_bq.profile_verified;
    status->is_full = (flags & BQ27220_STATUS_FC) != 0;
    status->is_charging = !status->is_full &&
                          (flags & (BQ27220_STATUS_DSG | BQ27220_STATUS_CHGINH)) == 0 &&
                          status->current_ma > 20;
    status->is_low = status->soc <= s_bq.low_threshold ||
                     (flags & (BQ27220_STATUS_TDA | BQ27220_STATUS_FD)) != 0;
    status->timestamp_ms = (uint64_t)(esp_timer_get_time() / 1000);

    if (!status->battery_present) {
        ESP_LOGW(TAG, "battery status does not assert BATTPRES (flags=0x%04x)", flags);
    }
    return ESP_OK;
}

esp_err_t bq27220_get_capacity_percent(uint8_t *percent)
{
    if (!percent) return ESP_ERR_INVALID_ARG;
    uint16_t value = 0;
    esp_err_t err = read_locked(BQ27220_REG_STATE_OF_CHARGE, &value);
    if (err == ESP_OK) *percent = (uint8_t)(value > 100 ? 100 : value);
    return err;
}

esp_err_t bq27220_get_voltage(uint16_t *voltage_mv)
{
    return read_locked(BQ27220_REG_VOLTAGE, voltage_mv);
}

esp_err_t bq27220_get_current(int16_t *current_ma)
{
    if (!current_ma) return ESP_ERR_INVALID_ARG;
    uint16_t raw = 0;
    esp_err_t err = read_locked(BQ27220_REG_CURRENT, &raw);
    if (err == ESP_OK) *current_ma = (int16_t)raw;
    return err;
}

esp_err_t bq27220_get_temperature(int16_t *temp_c)
{
    if (!temp_c) return ESP_ERR_INVALID_ARG;
    uint16_t raw = 0;
    esp_err_t err = read_locked(BQ27220_REG_TEMPERATURE, &raw);
    if (err == ESP_OK) *temp_c = temperature_c_from_raw(raw);
    return err;
}

bool bq27220_is_low_battery(void)
{
    uint8_t percent = 0;
    return bq27220_get_capacity_percent(&percent) == ESP_OK && percent <= s_bq.low_threshold;
}

bool bq27220_is_present(void)
{
    return s_bq.initialized;
}

esp_err_t bq27220_deinit(void)
{
    if (!s_bq.lock) return ESP_OK;
    xSemaphoreTake(s_bq.lock, portMAX_DELAY);
    /* This is a logical deinit used by battery-service recovery. Keep the
     * device and lock alive so a timeout can never be followed by handle
     * removal/re-add while the I2C ISR may still be retiring. */
    s_bq.initialized = false;
    s_bq.profile_verified = false;
    xSemaphoreGive(s_bq.lock);
    return ESP_OK;
}
