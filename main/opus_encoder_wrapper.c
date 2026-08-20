#include "opus_encoder_wrapper.h"

#include <stdbool.h>
#include <string.h>

#include "sdkconfig.h"

#if CONFIG_NUWAX_VOICE_UPLINK_CODEC_OPUS
#include "esp_audio_enc.h"
#include "esp_audio_enc_def.h"
#include "esp_log.h"
#include "esp_opus_enc.h"
#endif

#if CONFIG_NUWAX_VOICE_UPLINK_CODEC_OPUS
static const char *TAG = "nuwax_opus";

static bool map_frame_duration(uint16_t frame_duration_ms, esp_opus_enc_frame_duration_t *out) {
    if (!out) {
        return false;
    }
    switch (frame_duration_ms) {
        case 5:
            *out = ESP_OPUS_ENC_FRAME_DURATION_5_MS;
            return true;
        case 10:
            *out = ESP_OPUS_ENC_FRAME_DURATION_10_MS;
            return true;
        case 20:
            *out = ESP_OPUS_ENC_FRAME_DURATION_20_MS;
            return true;
        case 40:
            *out = ESP_OPUS_ENC_FRAME_DURATION_40_MS;
            return true;
        case 60:
            *out = ESP_OPUS_ENC_FRAME_DURATION_60_MS;
            return true;
        default:
            return false;
    }
}
#endif

esp_err_t nuwax_opus_encoder_init(nuwax_opus_encoder_t *encoder,
                                  uint32_t sample_rate,
                                  uint8_t channels,
                                  uint16_t frame_duration_ms) {
    if (!encoder) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(encoder, 0, sizeof(*encoder));

#if CONFIG_NUWAX_VOICE_UPLINK_CODEC_OPUS
    if (sample_rate != 16000 || channels != 1) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_opus_enc_frame_duration_t duration = 0;
    if (!map_frame_duration(frame_duration_ms, &duration)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_opus_enc_config_t config = {
        .sample_rate = ESP_AUDIO_SAMPLE_RATE_16K,
        .channel = ESP_AUDIO_MONO,
        .bit_per_sample = ESP_AUDIO_BIT16,
        .bitrate = 32000,
        .frame_duration = duration,
        .application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO,
        .complexity = 0,
        .enable_fec = false,
        .enable_dtx = true,
    };

    void *handle = NULL;
    int ret = esp_opus_enc_open(&config, sizeof(config), &handle);
    if (ret != ESP_AUDIO_ERR_OK || !handle) {
        return ESP_FAIL;
    }

    int frame_bytes = 0;
    int out_bytes = 0;
    ret = esp_opus_enc_get_frame_size(handle, &frame_bytes, &out_bytes);
    if (ret != ESP_AUDIO_ERR_OK || frame_bytes <= 0 || out_bytes <= 0) {
        esp_opus_enc_close(handle);
        return ESP_FAIL;
    }

    encoder->handle = handle;
    encoder->frame_samples = (size_t)frame_bytes / sizeof(int16_t);
    encoder->output_bytes = (size_t)out_bytes;
    encoder->sample_rate = sample_rate;
    encoder->channels = channels;
    encoder->frame_duration_ms = frame_duration_ms;
    ESP_LOGI(TAG,
             "Opus encoder ready sample_rate=%lu channels=%u frame_ms=%u frame_samples=%u output_bytes=%u",
             (unsigned long)sample_rate,
             (unsigned)channels,
             (unsigned)frame_duration_ms,
             (unsigned)encoder->frame_samples,
             (unsigned)encoder->output_bytes);
    return ESP_OK;
#else
    (void)sample_rate;
    (void)channels;
    (void)frame_duration_ms;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t nuwax_opus_encoder_encode(nuwax_opus_encoder_t *encoder,
                                    const int16_t *samples,
                                    size_t sample_count,
                                    uint8_t *out,
                                    size_t out_capacity,
                                    size_t *out_len) {
    if (!encoder || !samples || !out || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_len = 0;

#if CONFIG_NUWAX_VOICE_UPLINK_CODEC_OPUS
    if (!encoder->handle || sample_count != encoder->frame_samples || out_capacity < encoder->output_bytes) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_audio_enc_in_frame_t in = {
        .buffer = (uint8_t *)samples,
        .len = (uint32_t)(sample_count * sizeof(int16_t)),
    };
    esp_audio_enc_out_frame_t encoded = {
        .buffer = out,
        .len = (uint32_t)out_capacity,
        .encoded_bytes = 0,
    };

    int ret = esp_opus_enc_process(encoder->handle, &in, &encoded);
    if (ret != ESP_AUDIO_ERR_OK) {
        return ESP_FAIL;
    }
    if (encoded.encoded_bytes <= 0 || (size_t)encoded.encoded_bytes > out_capacity) {
        return ESP_FAIL;
    }
    *out_len = (size_t)encoded.encoded_bytes;
    return ESP_OK;
#else
    (void)sample_count;
    (void)out_capacity;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

void nuwax_opus_encoder_deinit(nuwax_opus_encoder_t *encoder) {
    if (!encoder) {
        return;
    }
#if CONFIG_NUWAX_VOICE_UPLINK_CODEC_OPUS
    if (encoder->handle) {
        esp_opus_enc_close(encoder->handle);
    }
#endif
    memset(encoder, 0, sizeof(*encoder));
}

size_t nuwax_opus_encoder_frame_samples(const nuwax_opus_encoder_t *encoder) {
    return encoder ? encoder->frame_samples : 0;
}

size_t nuwax_opus_encoder_output_bytes(const nuwax_opus_encoder_t *encoder) {
    return encoder ? encoder->output_bytes : 0;
}
