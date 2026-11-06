#pragma once

#include "wake_word_detection_policy.h"

/* Eight 128 ms minima approximate the quiet part of the last second. Speech
 * can hold a rising estimate for at most 3.5 s; sustained new background must
 * eventually be learned. Fixed storage, no allocation or audio retention. */
typedef struct {
    uint32_t minima[8];
    uint32_t floor_q8;
    uint32_t bucket_ms;
    uint32_t voice_start_ms;
    uint32_t last_voice_ms;
    uint8_t index;
    uint8_t count;
    bool initialized;
    bool voice_active;
    bool rise_held;
} nuwax_wake_noise_t;

static inline void nuwax_wake_noise_reset(nuwax_wake_noise_t *s) {
    const uint32_t floor = s->floor_q8;
    *s = (nuwax_wake_noise_t){.floor_q8 = floor};
}

static inline uint32_t nuwax_wake_noise_observe(
    nuwax_wake_noise_t *s, uint32_t now_ms, uint32_t average, uint32_t peak) {
    /* PCM16 absolute amplitudes, including abs(INT16_MIN). */
    if (average > 32768U) average = 32768U;
    const uint32_t level = average ? average : 1U;
    if (!s->initialized) {
        s->initialized = true;
        if (!s->floor_q8) s->floor_q8 = level << 8U;
        s->minima[0] = level;
        s->count = 1U;
        s->bucket_ms = now_ms;
    }
    uint32_t floor = s->floor_q8 >> 8U;
    if (!floor) floor = 1U;
    if (s->voice_active &&
        (uint32_t)(now_ms - s->last_voice_ms) > NUWAX_WAKE_VOICE_TAIL_HOLD_MS) {
        s->voice_active = false;
    }
    if (nuwax_wake_voice_frame_is_voice_like(average, peak, floor)) {
        if (!s->voice_active) s->voice_start_ms = now_ms;
        s->voice_active = true;
        s->last_voice_ms = now_ms;
    }
    s->rise_held = s->voice_active &&
        (uint32_t)(now_ms - s->voice_start_ms) < 3500U;
    if ((uint32_t)(now_ms - s->bucket_ms) >= 128U) {
        uint32_t sorted[8];
        for (unsigned i = 0; i < s->count; ++i) {
            unsigned j = i;
            const uint32_t v = s->minima[i];
            while (j && sorted[j - 1U] > v) {
                sorted[j] = sorted[j - 1U];
                --j;
            }
            sorted[j] = v;
        }
        const uint32_t target = sorted[(s->count - 1U) / 4U] << 8U;
        if (target < s->floor_q8) {
            s->floor_q8 -= (s->floor_q8 - target + 3U) / 4U;
        } else if (!s->rise_held) {
            s->floor_q8 += (target - s->floor_q8 + 7U) / 8U;
        }
        s->index = (s->index + 1U) % 8U;
        if (s->count < 8U) ++s->count;
        s->minima[s->index] = level;
        s->bucket_ms = now_ms;
    } else if (level < s->minima[s->index]) {
        s->minima[s->index] = level;
    }
    floor = s->floor_q8 >> 8U;
    return floor ? floor : 1U;
}
