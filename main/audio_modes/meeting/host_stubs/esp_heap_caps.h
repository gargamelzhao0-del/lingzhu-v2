#pragma once

#include <stddef.h>
#include <stdlib.h>

#define MALLOC_CAP_SPIRAM 0x01U
#define MALLOC_CAP_8BIT 0x02U

static inline void *heap_caps_malloc(size_t bytes, unsigned int caps) {
    (void)caps;
    return malloc(bytes);
}

static inline void heap_caps_free(void *ptr) {
    free(ptr);
}

