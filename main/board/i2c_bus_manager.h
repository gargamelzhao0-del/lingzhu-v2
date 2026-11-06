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
/* Optional sampler: timeout=0 tries once and never waits behind touch. */
/* Mark immediately after a synchronous driver call, while owning the bus.
 * Reporting is deferred until after the transaction; no log in the hot path. */
void nuwax_i2c_bus_sample_transfer_done(esp_err_t result);
typedef enum {
    NUWAX_I2C_CLIENT_OTHER,
    NUWAX_I2C_CLIENT_TOUCH,
    NUWAX_I2C_CLIENT_BATTERY,
    NUWAX_I2C_CLIENT_QMI,
    NUWAX_I2C_CLIENT_COUNT
} nuwax_i2c_client_t;
/* Call while holding the transaction lock, immediately after driver return. */
void nuwax_i2c_bus_transfer_done(nuwax_i2c_client_t client, esp_err_t result);
void nuwax_i2c_bus_transaction_end(void);
esp_err_t nuwax_i2c_bus_recover(const char *reason);
void nuwax_i2c_bus_log_probe(const char *name, uint8_t address, int timeout_ms);
void nuwax_i2c_bus_log_full_scan(const char *label, int timeout_ms);
void nuwax_i2c_bus_log_scan_once(void);

typedef struct {
    uint32_t calls, errors, long_holds;
    uint32_t hold_max_us, driver_max_us, release_max_us;
} nuwax_i2c_client_window_t;

typedef struct {
    uint32_t transactions, lock_timeouts, recoveries;
    uint32_t max_wait_us, max_hold_us;
    uint64_t total_wait_us, total_hold_us;
    uint32_t long_holds;
    uint32_t try_skips, sample_calls, sample_errors;
    uint32_t sample_driver_max_us, sample_release_max_us;
    uint32_t sample_window_driver_max_us, sample_window_release_max_us;
    char max_wait_task[24], max_hold_task[24], last_timeout_task[24];
    nuwax_i2c_client_window_t clients[NUWAX_I2C_CLIENT_COUNT];
} nuwax_i2c_bus_stats_t;
void nuwax_i2c_bus_get_stats(nuwax_i2c_bus_stats_t *stats);
/* get_stats consumes sample window maxima and client windows; lifetime totals persist.
 * OTHER includes uninstrumented transactions: driver/release timing unavailable. */
