#pragma once

#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

esp_err_t nuwax_i2c_bus_init(void);
esp_err_t nuwax_i2c_bus_get(i2c_master_bus_handle_t *out_bus);
esp_err_t nuwax_i2c_bus_add_device(uint8_t address,
                                   uint32_t scl_speed_hz,
                                   i2c_master_dev_handle_t *out_dev);
esp_err_t nuwax_i2c_bus_remove_device(i2c_master_dev_handle_t dev);
esp_err_t nuwax_i2c_bus_probe(uint8_t address, int timeout_ms);
/* Serialize application-level transactions with bus recovery. The ESP-IDF
 * device handles serialize their own transfers, but a bus reset must not race
 * a transfer issued by another application driver. */
esp_err_t nuwax_i2c_bus_transaction_begin(uint32_t timeout_ms);
void nuwax_i2c_bus_transaction_end(void);
esp_err_t nuwax_i2c_bus_recover(const char *reason);
void nuwax_i2c_bus_log_probe(const char *name, uint8_t address, int timeout_ms);
void nuwax_i2c_bus_log_full_scan(const char *label, int timeout_ms);
void nuwax_i2c_bus_log_scan_once(void);
