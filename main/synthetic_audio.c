#include "synthetic_audio.h"

void nuwax_synthetic_audio_init(nuwax_synthetic_audio_t *audio) {
    if (audio) {
        audio->phase = 0;
    }
}

void nuwax_synthetic_audio_fill_pcm16(nuwax_synthetic_audio_t *audio,
                                      int16_t *samples,
                                      size_t sample_count,
                                      uint32_t sample_rate) {
    if (!audio || !samples || sample_rate == 0) {
        return;
    }

    const uint32_t tone_hz = 440;
    const int32_t amplitude = 3600;
    const uint32_t half = sample_rate / 2;
    for (size_t i = 0; i < sample_count; i++) {
        audio->phase = (audio->phase + tone_hz) % sample_rate;
        int32_t value;
        if (audio->phase < half) {
            value = ((int32_t)audio->phase * 2 * amplitude) / (int32_t)half - amplitude;
        } else {
            value = amplitude - (((int32_t)audio->phase - (int32_t)half) * 2 * amplitude) / (int32_t)half;
        }
        samples[i] = (int16_t)value;
    }
}
