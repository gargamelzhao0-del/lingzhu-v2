/*
 * Retired playback prototype.
 *
 * Product playback is owned by conversation/speaker_playback.c and
 * speaker_output.c on the shared audio-board TX channel. This translation
 * unit is intentionally not linked into firmware. Its legacy entry points
 * fail closed instead of reporting successful playback without writing I2S.
 */
#include <stdbool.h>
#include <stdint.h>

#include "esp_log.h"

typedef struct {
    int16_t pcm[480];
    uint32_t ts_ms;
} pb_frame_t;

static const char *TAG = "audio_play_legacy";

void audio_playback_push(const int16_t *pcm, int n, uint32_t ts_ms)
{
    (void)pcm;
    (void)n;
    (void)ts_ms;
}

bool audio_playback_pop(pb_frame_t *out)
{
    (void)out;
    return false;
}

uint8_t audio_playback_buffer_pct(void)
{
    return 0;
}

int audio_playback_start(void)
{
    ESP_LOGE(TAG, "retired prototype rejected; use speaker_playback");
    return -1;
}

void audio_playback_stop(void)
{
}
