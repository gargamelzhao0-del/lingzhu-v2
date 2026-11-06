#pragma once

#include <stddef.h>

#define NUWAX_AUDIO_RUNTIME_MIN_INTERNAL_FREE_BYTES 16384U
#define NUWAX_AUDIO_RUNTIME_MIN_INTERNAL_LARGEST_BYTES 10240U
#define NUWAX_AUDIO_RUNTIME_MIN_DMA_LARGEST_BYTES 8192U

#define NUWAX_AUDIO_RUNTIME_CRITICAL_INTERNAL_FREE_BYTES 4096U
#define NUWAX_AUDIO_RUNTIME_CRITICAL_INTERNAL_LARGEST_BYTES 2048U
#define NUWAX_AUDIO_RUNTIME_CRITICAL_DMA_LARGEST_BYTES 2048U

typedef struct {
    size_t internal_free;
    size_t internal_largest;
    size_t dma_largest;
} nuwax_audio_runtime_memory_t;

typedef enum {
    NUWAX_AUDIO_RUNTIME_MEMORY_READY = 0,
    NUWAX_AUDIO_RUNTIME_MEMORY_DEFER,
    NUWAX_AUDIO_RUNTIME_MEMORY_CRITICAL,
} nuwax_audio_runtime_memory_state_t;

/* Exclusive capture creates internal task metadata and I2S/DMA resources.
 * Refuse the transition while either total reserve or contiguous reserve is
 * below the established runtime heap safety watermark. */
static inline nuwax_audio_runtime_memory_state_t
nuwax_audio_runtime_memory_classify(nuwax_audio_runtime_memory_t memory) {
    if (memory.internal_free >= NUWAX_AUDIO_RUNTIME_MIN_INTERNAL_FREE_BYTES &&
        memory.internal_largest >=
            NUWAX_AUDIO_RUNTIME_MIN_INTERNAL_LARGEST_BYTES &&
        memory.dma_largest >= NUWAX_AUDIO_RUNTIME_MIN_DMA_LARGEST_BYTES) {
        return NUWAX_AUDIO_RUNTIME_MEMORY_READY;
    }
    if (memory.internal_free <
            NUWAX_AUDIO_RUNTIME_CRITICAL_INTERNAL_FREE_BYTES ||
        memory.internal_largest <
            NUWAX_AUDIO_RUNTIME_CRITICAL_INTERNAL_LARGEST_BYTES ||
        memory.dma_largest <
            NUWAX_AUDIO_RUNTIME_CRITICAL_DMA_LARGEST_BYTES) {
        return NUWAX_AUDIO_RUNTIME_MEMORY_CRITICAL;
    }
    return NUWAX_AUDIO_RUNTIME_MEMORY_DEFER;
}
