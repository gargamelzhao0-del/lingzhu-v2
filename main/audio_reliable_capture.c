/*
 * Retired capture prototype.
 *
 * Product capture is owned by mic_input.c and the shared audio-board RX
 * channel. This translation unit is intentionally not linked into firmware.
 * Keep these legacy entry points fail-closed so an out-of-tree caller cannot
 * mistake the old prototype for a working I2S backend.
 */
#include <stdbool.h>
#include <stdint.h>

#include "esp_log.h"

typedef struct {
    int16_t pcm[480];
    uint32_t seq;
    uint32_t ts_ms;
} audio_frame_t;

static const char *TAG = "audio_capture_legacy";

bool audio_capture_pop(audio_frame_t *out)
{
    (void)out;
    return false;
}

int audio_capture_start(void)
{
    ESP_LOGE(TAG, "retired prototype rejected; use mic_input and audio_uplink");
    return -1;
}

void audio_capture_stop(void)
{
}

void audio_capture_on_frame(const int16_t *pcm, int n, uint32_t ts_ms)
{
    (void)pcm;
    (void)n;
    (void)ts_ms;
}
