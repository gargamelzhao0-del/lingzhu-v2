#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "ws_transport.h"
#include "music_status.h"

esp_err_t nuwax_music_playback_init(void);
bool nuwax_music_playback_start(nuwax_ws_client_t *ws,
                                const char *request_id,
                                const char *url,
                                const char *format);
bool nuwax_music_playback_stop(const char *request_id);
bool nuwax_music_playback_active(void);
bool nuwax_music_playback_audio_active(void);
bool nuwax_music_playback_output_quiesced(void);
typedef struct {
    char request_id[80];
    char state[16];
    char reason[48];
    uint32_t sample_rate;
} nuwax_music_snapshot_t;
void nuwax_music_playback_snapshot(nuwax_music_snapshot_t *out);
void nuwax_music_playback_cancel(const char *reason);
bool nuwax_music_playback_start_test(const char *id, const char *url, const char *format);
