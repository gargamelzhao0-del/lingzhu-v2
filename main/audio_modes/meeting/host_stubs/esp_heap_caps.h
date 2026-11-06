#pragma once

#include <stdbool.h>
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

static inline void *heap_caps_calloc(size_t count, size_t bytes, unsigned int caps) {
    (void)caps;
#if defined(NUWAX_HOST_HEAP_CAPS_FAULT_INJECT)
    extern bool nuwax_host_heap_caps_fail_calloc;
    if (nuwax_host_heap_caps_fail_calloc) {
        return NULL;
    }
#endif
    return calloc(count, bytes);
}
