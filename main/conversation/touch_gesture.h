#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

void nuwax_touch_gesture_set_action_queue(QueueHandle_t queue);
void nuwax_touch_gesture_task(void *arg);
