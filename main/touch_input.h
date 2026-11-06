#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool touched;
    bool cancelled;
    uint8_t gesture;
    uint8_t points;
    uint8_t event;
    uint16_t x;
    uint16_t y;
    /* Local producer identity/timestamp, never part of a cloud message.
     * A new contact remains distinguishable if queued motion drops a release. */
    uint32_t contact_id;
    uint32_t sample_ms;
} nuwax_touch_point_t;

typedef void (*nuwax_touch_event_cb_t)(const nuwax_touch_point_t *point, void *user_ctx);

typedef struct {
    bool initialized;
    bool controller_ready;
    bool event_producer_ready;
    uint8_t i2c_address;
} nuwax_touch_status_t;

esp_err_t nuwax_touch_init(void);
esp_err_t nuwax_touch_get_status(nuwax_touch_status_t *status);
esp_err_t nuwax_touch_read(nuwax_touch_point_t *point);
esp_err_t nuwax_touch_set_event_callback(nuwax_touch_event_cb_t callback, void *user_ctx);
esp_err_t nuwax_touch_poll_event(nuwax_touch_point_t *point, uint32_t timeout_ms);
esp_err_t nuwax_touch_start_log_task(void);
esp_err_t nuwax_touch_start_p1_diag_task(void);
