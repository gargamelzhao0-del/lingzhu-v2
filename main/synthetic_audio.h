#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t phase;
} nuwax_synthetic_audio_t;

void nuwax_synthetic_audio_init(nuwax_synthetic_audio_t *audio);
void nuwax_synthetic_audio_fill_pcm16(nuwax_synthetic_audio_t *audio,
                                      int16_t *samples,
                                      size_t sample_count,
                                      uint32_t sample_rate);
