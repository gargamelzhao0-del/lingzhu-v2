#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Boot/main owner only. PWM changes immediately; Flash writes wait for idle. */
void nuwax_display_brightness_load(void);
uint8_t nuwax_display_brightness_percent(void);
esp_err_t nuwax_display_brightness_apply(uint8_t percent);
void nuwax_display_brightness_save(bool force);

static inline bool nuwax_display_brightness_valid(unsigned percent)
{
    return percent == 40U || percent == 70U || percent == 100U;
}
