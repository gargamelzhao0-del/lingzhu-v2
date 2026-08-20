#pragma once

#include <stdbool.h>

#include "app_state.h"
#include "esp_err.h"

typedef nuwax_app_state_t (*nuwax_head_touch_state_cb_t)(void *user_ctx);
typedef bool (*nuwax_head_touch_allowed_cb_t)(void *user_ctx);

/* Connects the independent active-high head touch input to the product
 * conversation action queue. The GPIO polling callback remains non-blocking;
 * all conversation state changes are owned by the normal application loop. */
esp_err_t nuwax_head_touch_control_init(nuwax_head_touch_state_cb_t get_state,
                                        nuwax_head_touch_allowed_cb_t interaction_allowed,
                                        void *user_ctx);
