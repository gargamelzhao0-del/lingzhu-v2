#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "board/audio_board.h"

typedef struct {
    void *tx_handle;
    void *lock;
    int16_t *stereo_buffer;
    size_t stereo_capacity;
    uint8_t volume_percent;
    /* Accepted frames of the startup envelope. Leading digital silence does
     * not consume it; only a successful physical stop resets it. */
    uint32_t startup_fade_position;
    nuwax_audio_board_lease_t board_lease;
    nuwax_audio_board_ref_t board_ref;
    bool initialized;
    bool muted;
    /* True only after physical PA mute AND the complete DMA-silence settling
     * sequence. Shared ES8311 TX keeps clocking silence while PA is off;
     * muted alone does not prove DMA contents are safe for the next start. */
    bool stop_settled;
    bool vocat_shared_tx; /* P2: tx_handle is audio_board's shared TX (release, not delete) */
    bool software_ref_required;
    bool software_ref_ready;
    bool shared_tx_recovery_required;
} nuwax_speaker_output_t;

esp_err_t nuwax_speaker_output_init(nuwax_speaker_output_t *speaker);
esp_err_t nuwax_speaker_output_set_volume_percent(nuwax_speaker_output_t *speaker,
                                                  uint8_t volume_percent);
uint8_t nuwax_speaker_output_get_volume_percent(const nuwax_speaker_output_t *speaker);
bool nuwax_speaker_output_software_ref_ready(
    const nuwax_speaker_output_t *speaker);
esp_err_t nuwax_speaker_output_write_pcm16(nuwax_speaker_output_t *speaker,
                                           const int16_t *samples,
                                           size_t sample_count,
                                           uint32_t timeout_ms);
esp_err_t nuwax_speaker_output_stop(nuwax_speaker_output_t *speaker);
esp_err_t nuwax_speaker_output_drain_and_stop(
    nuwax_speaker_output_t *speaker,
    uint32_t timeout_ms);
esp_err_t nuwax_speaker_output_deinit(nuwax_speaker_output_t *speaker);
