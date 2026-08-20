#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t avg_abs;
    uint32_t peak;
    uint32_t clipped;
} nuwax_mic_level_t;

nuwax_mic_level_t nuwax_mic_level_measure(const int16_t *samples, size_t sample_count);
void nuwax_mic_level_log(const char *tag, nuwax_mic_level_t level, uint32_t seq);
bool nuwax_mic_level_should_log(uint32_t seq, bool important);
bool nuwax_mic_level_is_speech(nuwax_mic_level_t level);
