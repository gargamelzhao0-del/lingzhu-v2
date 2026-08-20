#pragma once

#include "lvgl.h"

typedef enum {
    NUWAX_CC_ICON_WIFI_0 = 0,
    NUWAX_CC_ICON_WIFI_1 = 1,
    NUWAX_CC_ICON_WIFI_2 = 2,
    NUWAX_CC_ICON_WIFI_3 = 3,
    NUWAX_CC_ICON_CLOUD = 4,
    NUWAX_CC_ICON_BATTERY = 5,
    NUWAX_CC_ICON_CONVERSATION = 6,
    NUWAX_CC_ICON_SLEEPING = 7,
    NUWAX_CC_ICON_MEETING_STATE = 8,
    NUWAX_CC_ICON_MONITORING = 9,
    NUWAX_CC_ICON_MEETING = 10,
    NUWAX_CC_ICON_PROVISIONING = 11,
    NUWAX_CC_ICON_QR = 12,
    NUWAX_CC_ICON_DEVICE_INFO = 13,
    NUWAX_CC_ICON_VOLUME = 14,
    NUWAX_CC_ICON_COUNT
} nuwax_control_center_icon_id_t;

const lv_image_dsc_t *nuwax_control_center_icon_get(nuwax_control_center_icon_id_t id);
