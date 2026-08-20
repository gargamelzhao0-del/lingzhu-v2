#pragma once

#include <stdbool.h>

#include "lvgl.h"

bool nuwax_device_qr_view_create(lv_obj_t *parent);
void nuwax_device_qr_view_reset(void);
bool nuwax_device_qr_view_show(const char *payload, const char *device_id);
void nuwax_device_qr_view_hide(void);
bool nuwax_device_qr_view_is_open(void);
