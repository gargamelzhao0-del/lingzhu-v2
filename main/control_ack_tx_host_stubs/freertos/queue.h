#pragma once
#include "FreeRTOS.h"
QueueHandle_t xQueueCreateStatic(UBaseType_t length, UBaseType_t item_bytes,
                                uint8_t *storage, StaticQueue_t *control);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait);
void vQueueDelete(QueueHandle_t queue);
