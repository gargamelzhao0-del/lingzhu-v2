#pragma once

#include <stdbool.h>

#include "lvgl.h"

bool nuwax_device_qr_view_create(lv_obj_t *parent);
void nuwax_device_qr_view_reset(void);
bool nuwax_device_qr_view_show(const char *payload, const char *device_id);
void nuwax_device_qr_view_hide(void);
bool nuwax_device_qr_view_is_open(void);
/* LVGL owner only: opaque integer translation and press feedback. */
lv_obj_t *nuwax_device_qr_view_root(void);
lv_obj_t *nuwax_device_qr_view_back_button(void);
