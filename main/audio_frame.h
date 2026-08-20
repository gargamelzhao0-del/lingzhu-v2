#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define NUWAX_AUDIO_FRAME_HEADER_BYTES 22
#define NUWAX_AUDIO_CODEC_PCM16 1
#define NUWAX_AUDIO_CODEC_OPUS 2
#define NUWAX_AUDIO_FLAG_START 0x01
#define NUWAX_AUDIO_FLAG_END 0x02
#define NUWAX_AUDIO_FLAG_VAD_SPEECH 0x04

esp_err_t nuwax_audio_frame_build(uint8_t *out,
                                  size_t out_capacity,
                                  uint8_t codec,
                                  const uint8_t *payload,
                                  size_t payload_bytes,
                                  uint32_t sequence,
                                  uint32_t timestamp_ms,
                                  uint8_t flags,
                                  uint16_t sample_rate,
                                  uint8_t channels,
                                  uint16_t frame_duration_ms,
                                  size_t *out_len);

esp_err_t nuwax_audio_frame_build_pcm16(uint8_t *out,
                                        size_t out_capacity,
                                        const int16_t *samples,
                                        size_t sample_count,
                                        uint32_t sequence,
                                        uint32_t timestamp_ms,
                                        uint8_t flags,
                                        uint16_t sample_rate,
                                        uint8_t channels,
                                        uint16_t frame_duration_ms,
                                        size_t *out_len);

esp_err_t nuwax_audio_frame_peek(const uint8_t *frame,
                                 size_t available_len,
                                 uint8_t *codec,
                                 uint8_t *flags,
                                 size_t *frame_len);

/* Returns a borrowed native PCM16 view. The parser rejects a payload that is
 * not aligned for int16_t access; the frame storage must outlive the view. */
esp_err_t nuwax_audio_frame_parse_pcm16(const uint8_t *frame,
                                        size_t frame_len,
                                        const int16_t **samples,
                                        size_t *sample_count,
                                        uint32_t *sequence,
                                        uint16_t *sample_rate,
                                        uint8_t *channels);
