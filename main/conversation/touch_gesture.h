#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef void (*nuwax_touch_gesture_volume_preview_cb_t)(int volume_percent);

void nuwax_touch_gesture_set_action_queue(QueueHandle_t queue);
void nuwax_touch_gesture_set_volume_preview_callback(nuwax_touch_gesture_volume_preview_cb_t callback);
void nuwax_touch_gesture_task(void *arg);
