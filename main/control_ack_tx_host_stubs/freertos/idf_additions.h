#pragma once
#include "FreeRTOS.h"
BaseType_t xTaskCreateWithCaps(TaskFunction_t function, const char *name,
                              uint32_t stack, void *arg, UBaseType_t priority,
                              TaskHandle_t *handle, UBaseType_t caps);
