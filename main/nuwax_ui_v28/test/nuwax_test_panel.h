#pragma once

#include "test_controls.h"
#include "lvgl.h"

#if NUWAX_TEST_PANEL_ENABLED
LV_FONT_DECLARE(nuwax_font_test_panel_11);
lv_obj_t *nuwax_test_panel_create(lv_obj_t *parent);
void nuwax_test_panel_reset(void);
void nuwax_test_panel_set_network(bool enabled, bool pending, bool target);
bool nuwax_test_panel_hit_network(uint16_t x, uint16_t y);
#endif
