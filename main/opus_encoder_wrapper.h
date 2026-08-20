#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    void *handle;
    size_t frame_samples;
    size_t output_bytes;
    uint32_t sample_rate;
    uint8_t channels;
    uint16_t frame_duration_ms;
} nuwax_opus_encoder_t;

esp_err_t nuwax_opus_encoder_init(nuwax_opus_encoder_t *encoder,
                                  uint32_t sample_rate,
                                  uint8_t channels,
                                  uint16_t frame_duration_ms);

esp_err_t nuwax_opus_encoder_encode(nuwax_opus_encoder_t *encoder,
                                    const int16_t *samples,
                                    size_t sample_count,
                                    uint8_t *out,
                                    size_t out_capacity,
                                    size_t *out_len);

void nuwax_opus_encoder_deinit(nuwax_opus_encoder_t *encoder);

size_t nuwax_opus_encoder_frame_samples(const nuwax_opus_encoder_t *encoder);
size_t nuwax_opus_encoder_output_bytes(const nuwax_opus_encoder_t *encoder);
