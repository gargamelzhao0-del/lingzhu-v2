#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
typedef struct { uint32_t f; uint32_t b; uint32_t e; int16_t d; bool en; } nuwax_p3_l3_stats_t;
esp_err_t nuwax_p3_l3_init(void);
bool nuwax_p3_l3_is_enabled(void);
esp_err_t nuwax_p3_l3_set_enabled(bool enabled, bool persist);
esp_err_t nuwax_p3_l3_set_enabled_runtime(bool enabled);
esp_err_t nuwax_p3_l3_process_chunk(int16_t *samples, size_t frames);
void nuwax_p3_l3_get_stats(nuwax_p3_l3_stats_t *out);
void nuwax_p3_l3_reset_stats(void);
