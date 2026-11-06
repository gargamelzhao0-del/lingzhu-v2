#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Distinct source/destination buffers. Use word transfers only when both are
 * aligned; memcpy retains C aliasing rules, including uint8_t array callers.
 * Constructing the key as bytes makes the operation endian independent.
 * IDF builds with -fno-builtin-memcpy. Explicit builtins below are only for
 * these bounded, aligned transfers: ordinary memcpy here produces
 * two ROM calls per word (524288 calls per MiB). Do not change global memcpy
 * flags or use pointer casts that violate C aliasing rules. */
#if defined(__GNUC__)
__attribute__((always_inline))
#endif
static inline void nuwax_ws_mask_copy(uint8_t *dst, const uint8_t *src, size_t len,
                                      const uint8_t mask[4], size_t offset)
{
    const size_t phase = offset & 3U;
    if ((((uintptr_t)dst | (uintptr_t)src) & 3U) == 0U && len >= 4U) {
        const uint8_t rotated[4] = {mask[phase], mask[(phase + 1U) & 3U],
                                   mask[(phase + 2U) & 3U], mask[(phase + 3U) & 3U]};
        uint32_t key;
#if defined(__GNUC__)
        __builtin_memcpy(&key, rotated, sizeof(key));
#else
        memcpy(&key, rotated, sizeof(key));
#endif
#if defined(__GNUC__)
        dst = __builtin_assume_aligned(dst, 4U);
        src = __builtin_assume_aligned(src, 4U);
#endif
        /* Four words per iteration reduce pointer/loop overhead in the hot
         * PSRAM-to-PSRAM archive encoder. Each access is still an aligned,
         * alias-safe fixed-size copy; never read beyond len or modify src. */
        while (len >= 16U) {
            uint32_t words[4];
#if defined(__GNUC__)
            __builtin_memcpy(words, src, sizeof(words));
#else
            memcpy(words, src, sizeof(words));
#endif
            words[0] ^= key; words[1] ^= key;
            words[2] ^= key; words[3] ^= key;
#if defined(__GNUC__)
            __builtin_memcpy(dst, words, sizeof(words));
#else
            memcpy(dst, words, sizeof(words));
#endif
            dst += 16U; src += 16U; len -= 16U;
        }
        while (len >= 4U) {
            uint32_t word;
#if defined(__GNUC__)
            __builtin_memcpy(&word, src, sizeof(word));
#else
            memcpy(&word, src, sizeof(word));
#endif
            word ^= key;
#if defined(__GNUC__)
            __builtin_memcpy(dst, &word, sizeof(word));
#else
            memcpy(dst, &word, sizeof(word));
#endif
            dst += 4U; src += 4U; len -= 4U;
        }
    }
    for (size_t i = 0U; i < len; ++i) dst[i] = src[i] ^ mask[(phase + i) & 3U];
}

/* Encode a subrange of the logical concatenation without materializing it.
 * Caller validates offset + bytes within prefix_bytes + tail_bytes. */
#if defined(__GNUC__)
__attribute__((always_inline))
#endif
static inline void nuwax_ws_mask_copy_parts(uint8_t *dst, const uint8_t *prefix,
    size_t prefix_bytes, const uint8_t *tail, size_t offset, size_t bytes,
    const uint8_t mask[4])
{
    if (offset < prefix_bytes) {
        size_t n = prefix_bytes - offset;
        if (n > bytes) n = bytes;
        nuwax_ws_mask_copy(dst, prefix + offset, n, mask, offset);
        dst += n; offset += n; bytes -= n;
    }
    if (bytes) nuwax_ws_mask_copy(dst, tail + offset - prefix_bytes, bytes, mask, offset);
}
