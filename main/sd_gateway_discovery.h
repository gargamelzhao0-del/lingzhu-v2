#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
/* Called only by the dedicated replay worker. Never blocks the main task. */
esp_err_t nuwax_sd_gateway_discover(char *url, size_t capacity,
                                  bool (*cancelled)(void *), void *ctx);
