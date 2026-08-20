#pragma once

#include "lvgl.h"
#include <stdbool.h>

/**
 * Check if animations should be skipped (low power mode).
 */
bool nuwax_anim_should_skip(void);

/**
 * Fade an object's opacity from current to target.
 */
void nuwax_anim_fade(lv_obj_t *obj, lv_opa_t target, uint32_t duration_ms);
