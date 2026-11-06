#pragma once
#include <stddef.h>
#include <stdint.h>
#define MALLOC_CAP_SPIRAM 1U
#define MALLOC_CAP_8BIT 2U
void *heap_caps_malloc(size_t size, uint32_t caps);
void heap_caps_free(void *memory);
