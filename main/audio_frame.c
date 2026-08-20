#include "audio_frame.h"

#include <string.h>

#include "esp_err.h"

static void write_u16le(uint8_t *dst, uint16_t value) {
    dst[0] = (uint8_t)(value & 0xff);
    dst[1] = (uint8_t)((value >> 8) & 0xff);
}

static void write_u32le(uint8_t *dst, uint32_t value) {
    dst[0] = (uint8_t)(value & 0xff);
    dst[1] = (uint8_t)((value >> 8) & 0xff);
    dst[2] = (uint8_t)((value >> 16) & 0xff);
    dst[3] = (uint8_t)((value >> 24) & 0xff);
}

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
                                  size_t *out_len) {
    if (!out || !out_len || (payload_bytes > 0U && !payload)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (codec != NUWAX_AUDIO_CODEC_PCM16 && codec != NUWAX_AUDIO_CODEC_OPUS) {
        return ESP_ERR_INVALID_ARG;
    }
    if (codec == NUWAX_AUDIO_CODEC_PCM16 && (payload_bytes % sizeof(int16_t)) != 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (payload_bytes > UINT16_MAX || out_capacity < NUWAX_AUDIO_FRAME_HEADER_BYTES + payload_bytes) {
        return ESP_ERR_NO_MEM;
    }

    out[0] = 'N';
    out[1] = 'A';
    out[2] = 1;
    out[3] = codec;
    out[4] = flags;
    out[5] = channels;
    write_u16le(&out[6], sample_rate);
    write_u16le(&out[8], frame_duration_ms);
    write_u32le(&out[10], sequence);
    write_u32le(&out[14], timestamp_ms);
    write_u16le(&out[18], (uint16_t)payload_bytes);
    write_u16le(&out[20], 0);
    if (payload_bytes > 0U) {
        memcpy(&out[NUWAX_AUDIO_FRAME_HEADER_BYTES], payload, payload_bytes);
    }
    *out_len = NUWAX_AUDIO_FRAME_HEADER_BYTES + payload_bytes;
    return ESP_OK;
}

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
                                        size_t *out_len) {
    if (sample_count > SIZE_MAX / sizeof(int16_t)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return nuwax_audio_frame_build(out,
                                   out_capacity,
                                   NUWAX_AUDIO_CODEC_PCM16,
                                   (const uint8_t *)samples,
                                   sample_count * sizeof(int16_t),
                                   sequence,
                                   timestamp_ms,
                                   flags,
                                   sample_rate,
                                   channels,
                                   frame_duration_ms,
                                   out_len);
}

static uint16_t read_u16le(const uint8_t *src) {
    return (uint16_t)src[0] | ((uint16_t)src[1] << 8);
}

static uint32_t read_u32le(const uint8_t *src) {
    return (uint32_t)src[0] | ((uint32_t)src[1] << 8) | ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
}

esp_err_t nuwax_audio_frame_peek(const uint8_t *frame,
                                 size_t available_len,
                                 uint8_t *codec,
                                 uint8_t *flags,
                                 size_t *frame_len) {
    if (!frame || !frame_len) {
        return ESP_ERR_INVALID_ARG;
    }
    if (available_len < NUWAX_AUDIO_FRAME_HEADER_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (frame[0] != 'N' || frame[1] != 'A' || frame[2] != 1) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint16_t payload_bytes = read_u16le(&frame[18]);
    const size_t total_len = NUWAX_AUDIO_FRAME_HEADER_BYTES + (size_t)payload_bytes;
    if (total_len > available_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (codec) {
        *codec = frame[3];
    }
    if (flags) {
        *flags = frame[4];
    }
    *frame_len = total_len;
    return ESP_OK;
}

esp_err_t nuwax_audio_frame_parse_pcm16(const uint8_t *frame,
                                        size_t frame_len,
                                        const int16_t **samples,
                                        size_t *sample_count,
                                        uint32_t *sequence,
                                        uint16_t *sample_rate,
                                        uint8_t *channels) {
    if (!frame || !samples || !sample_count || frame_len < NUWAX_AUDIO_FRAME_HEADER_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    if (frame[0] != 'N' || frame[1] != 'A' || frame[2] != 1U ||
        frame[3] != NUWAX_AUDIO_CODEC_PCM16) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint16_t payload_bytes = read_u16le(&frame[18]);
    if ((size_t)NUWAX_AUDIO_FRAME_HEADER_BYTES + (size_t)payload_bytes > frame_len ||
        (payload_bytes % sizeof(int16_t)) != 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    const uint8_t *payload = &frame[NUWAX_AUDIO_FRAME_HEADER_BYTES];
    if (((uintptr_t)payload % _Alignof(int16_t)) != 0U) {
        return ESP_ERR_INVALID_SIZE;
    }
    *samples = (const int16_t *)payload;
    *sample_count = payload_bytes / sizeof(int16_t);
    if (sequence) {
        *sequence = read_u32le(&frame[10]);
    }
    if (sample_rate) {
        *sample_rate = read_u16le(&frame[6]);
    }
    if (channels) {
        *channels = frame[5];
    }
    return ESP_OK;
}
