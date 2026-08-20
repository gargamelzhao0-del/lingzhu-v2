#ifndef NUWAX_BOARD_BOARD_DRIVERS_H
#define NUWAX_BOARD_BOARD_DRIVERS_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int16_t accel_x;
    int16_t accel_y;
    int16_t accel_z;
    int16_t gyro_x;
    int16_t gyro_y;
    int16_t gyro_z;
    float accel_scale;
    float gyro_scale;
    uint64_t timestamp_us;
} qmi8658a_sensor_data_t;

typedef enum {
    QMI8658A_ACCEL_RANGE_2G = 0,
    QMI8658A_ACCEL_RANGE_4G = 1,
    QMI8658A_ACCEL_RANGE_8G = 2,
    QMI8658A_ACCEL_RANGE_16G = 3,
} qmi8658a_accel_range_t;

typedef enum {
    QMI8658A_GYRO_RANGE_16DPS = 0,
    QMI8658A_GYRO_RANGE_32DPS = 1,
    QMI8658A_GYRO_RANGE_64DPS = 2,
    QMI8658A_GYRO_RANGE_128DPS = 3,
    QMI8658A_GYRO_RANGE_256DPS = 4,
    QMI8658A_GYRO_RANGE_512DPS = 5,
    QMI8658A_GYRO_RANGE_1024DPS = 6,
    QMI8658A_GYRO_RANGE_2048DPS = 7,
} qmi8658a_gyro_range_t;

typedef struct {
    uint8_t i2c_address;
    uint8_t accel_range;
    uint8_t gyro_range;
    uint16_t accel_odr;
    uint16_t gyro_odr;
    bool use_interrupt;
    int int_gpio;
} qmi8658a_config_t;

esp_err_t qmi8658a_init(const qmi8658a_config_t *config);
esp_err_t qmi8658a_read_data(qmi8658a_sensor_data_t *data);
bool qmi8658a_is_present(void);
esp_err_t qmi8658a_get_device_id(uint8_t *id);
esp_err_t qmi8658a_set_accel_range(uint8_t range);
esp_err_t qmi8658a_set_gyro_range(uint8_t range);
esp_err_t qmi8658a_enter_low_power(void);
esp_err_t qmi8658a_wakeup(void);
esp_err_t qmi8658a_deinit(void);

typedef struct {
    uint16_t voltage_mv;
    int16_t current_ma;
    uint16_t capacity_percent;
    uint16_t remaining_mah;
    uint16_t full_charge_mah;
    uint16_t design_capacity_mah;
    uint16_t battery_status_flags;
    uint16_t operation_status_flags;
    int16_t temperature_c;
    uint16_t cycle_count;
    uint8_t soc;
    bool battery_present;
    bool initialization_complete;
    bool config_update_mode;
    bool profile_verified;
    bool is_charging;
    bool is_full;
    bool is_low;
    uint64_t timestamp_ms;
} bq27220_battery_status_t;

typedef struct {
    uint8_t i2c_address;
    uint16_t design_capacity;
    uint8_t low_battery_threshold;
} bq27220_config_t;

esp_err_t bq27220_init(const bq27220_config_t *config);
esp_err_t bq27220_get_status(bq27220_battery_status_t *status);
esp_err_t bq27220_get_capacity_percent(uint8_t *percent);
esp_err_t bq27220_get_voltage(uint16_t *voltage_mv);
esp_err_t bq27220_get_current(int16_t *current_ma);
esp_err_t bq27220_get_temperature(int16_t *temp_c);
bool bq27220_is_low_battery(void);
bool bq27220_is_present(void);
esp_err_t bq27220_deinit(void);

typedef enum {
    LED_STATE_OFF = 0,
    LED_STATE_ON,
    LED_STATE_BLINK_SLOW,
    LED_STATE_BLINK_FAST,
    LED_STATE_BREATH,
} led_state_t;

typedef enum {
    LED_COLOR_GREEN = 0,
    LED_COLOR_COUNT,
} led_color_t;

esp_err_t nuwax_led_init(void);
esp_err_t nuwax_led_set_state(led_color_t color, led_state_t state);
esp_err_t nuwax_led_on(led_color_t color);
esp_err_t nuwax_led_off(led_color_t color);
esp_err_t nuwax_led_toggle(led_color_t color);
esp_err_t nuwax_led_start_breath(led_color_t color, uint32_t period_ms);
esp_err_t nuwax_led_stop_breath(led_color_t color);
esp_err_t nuwax_led_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* NUWAX_BOARD_BOARD_DRIVERS_H */
