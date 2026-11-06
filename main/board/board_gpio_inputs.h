#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    NUWAX_BOARD_INPUT_HEAD_TOUCH = 0,
    NUWAX_BOARD_INPUT_BOOT_BUTTON,
} nuwax_board_input_t;

typedef enum {
    NUWAX_HEAD_TOUCH_EVENT_PRESS = 0,
    NUWAX_HEAD_TOUCH_EVENT_LONG_PRESS,
    NUWAX_HEAD_TOUCH_EVENT_RELEASE,
    /* The active level lasted beyond the product's valid-touch window. */
    NUWAX_HEAD_TOUCH_EVENT_EXPIRED,
} nuwax_head_touch_event_type_t;

typedef struct {
    nuwax_head_touch_event_type_t type;
    uint64_t duration_ms;
    bool active;
} nuwax_head_touch_event_t;

typedef struct {
    uint32_t long_press_ms;
    uint32_t gap_tolerance_ms; /* Deprecated: continuous-contact policy ignores this. */
    uint32_t max_valid_duration_ms;
} nuwax_head_touch_config_t;

typedef void (*nuwax_head_touch_event_cb_t)(
    const nuwax_head_touch_event_t *event,
    void *user_ctx);

typedef void (*nuwax_board_input_callback_t)(nuwax_board_input_t input,
                                              bool active,
                                              void *user_ctx);

esp_err_t nuwax_board_gpio_inputs_init(nuwax_board_input_callback_t callback,
                                       void *user_ctx);
esp_err_t nuwax_board_gpio_input_read(nuwax_board_input_t input, bool *active);
esp_err_t nuwax_board_gpio_inputs_deinit(void);

/* Optional product-facing head-touch event path. It is independent of the
 * legacy input callback used by hardware acceptance tests. Registering this
 * callback does not select a wake or expression policy; the application owns
 * that decision. The callback runs in the input polling task and must only
 * perform bounded, non-blocking work such as posting to an application queue. */
esp_err_t nuwax_head_touch_set_event_callback(nuwax_head_touch_event_cb_t callback,
                                              void *user_ctx);
esp_err_t nuwax_head_touch_set_config(const nuwax_head_touch_config_t *config);
