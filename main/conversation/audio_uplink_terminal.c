#include "conversation/audio_uplink_terminal.h"

#include "audio_frame.h"

esp_err_t nuwax_audio_uplink_ensure_batch_start(uint8_t *batch,
                                                size_t batch_len) {
    if (!batch) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t first_frame_len = 0U;
    esp_err_t err = nuwax_audio_frame_peek(batch,
                                           batch_len,
                                           NULL,
                                           NULL,
                                           &first_frame_len);
    if (err != ESP_OK) {
        return err;
    }
    if (first_frame_len > batch_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    batch[4U] |= NUWAX_AUDIO_FLAG_START;
    return ESP_OK;
}

esp_err_t nuwax_audio_uplink_mark_last_frame_end(uint8_t *batch,
                                                  size_t batch_len,
                                                  size_t last_frame_offset) {
    if (!batch) {
        return ESP_ERR_INVALID_ARG;
    }
    if (last_frame_offset > batch_len ||
        batch_len - last_frame_offset < NUWAX_AUDIO_FRAME_HEADER_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t frame_len = 0;
    esp_err_t err = nuwax_audio_frame_peek(&batch[last_frame_offset],
                                           batch_len - last_frame_offset,
                                           NULL,
                                           NULL,
                                           &frame_len);
    if (err != ESP_OK) {
        return err;
    }
    if (last_frame_offset + frame_len != batch_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    batch[last_frame_offset + 4U] |= NUWAX_AUDIO_FLAG_END;
    return ESP_OK;
}

esp_err_t nuwax_audio_uplink_build_terminal_marker(uint8_t *out,
                                                    size_t out_capacity,
                                                    uint8_t codec,
                                                    uint32_t sequence,
                                                    bool frame_seen,
                                                    uint16_t sample_rate,
                                                    uint8_t channels,
                                                    uint16_t frame_duration_ms,
                                                    size_t *out_len) {
    const uint8_t flags = NUWAX_AUDIO_FLAG_END |
                          (frame_seen ? 0U : NUWAX_AUDIO_FLAG_START);
    return nuwax_audio_frame_build(out,
                                   out_capacity,
                                   codec,
                                   NULL,
                                   0,
                                   sequence,
                                   sequence * (uint32_t)frame_duration_ms,
                                   flags,
                                   sample_rate,
                                   channels,
                                   frame_duration_ms,
                                   out_len);
}
