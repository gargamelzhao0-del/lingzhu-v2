#pragma once

#include <stddef.h>
#include <stdint.h>

typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;
typedef void *TaskHandle_t;

#define pdFALSE 0
#define pdTRUE 1
#define pdPASS pdTRUE
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

