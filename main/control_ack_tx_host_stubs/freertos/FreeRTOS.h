#pragma once
#include <stdint.h>
#include <pthread.h>
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
typedef void (*TaskFunction_t)(void *);
typedef void *TaskHandle_t;
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t ready;
    uint8_t *storage;
    size_t item_bytes;
    unsigned capacity, count, head, tail;
} StaticQueue_t;
typedef StaticQueue_t *QueueHandle_t;
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
