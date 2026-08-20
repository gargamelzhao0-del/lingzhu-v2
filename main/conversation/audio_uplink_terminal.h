#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t nuwax_audio_uplink_mark_last_frame_end(uint8_t *batch,
                                                  size_t batch_len,
                                                  size_t last_frame_offset);

esp_err_t nuwax_audio_uplink_ensure_batch_start(uint8_t *batch,
                                                size_t batch_len);

esp_err_t nuwax_audio_uplink_build_terminal_marker(uint8_t *out,
                                                    size_t out_capacity,
                                                    uint8_t codec,
                                                    uint32_t sequence,
                                                    bool frame_seen,
                                                    uint16_t sample_rate,
                                                    uint8_t channels,
                                                    uint16_t frame_duration_ms,
                                                    size_t *out_len);
