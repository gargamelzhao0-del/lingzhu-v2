#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "p3_nearend_candidate.h"
#include "ws_transport.h"

typedef struct {
    char playback_id[48];
    char audio_session_id[48];
    uint32_t capture_frames;
    uint32_t upload_frames;
    uint32_t queued_frames;
    uint32_t queue_drops;
    uint32_t write_failures;
    uint32_t read_timeouts;
    float ref_rms;
    float mic_rms;
    float mic2_rms;
    float dual_mic_corr;
    float dual_mic_abs_corr;
    float dual_mic_level_delta_db;
    float dual_mic_diff_rms;
    float dual_mic_diff_ratio;
    size_t heap_internal_min;
    size_t dma_largest;
    uint32_t overlap_ms;
    bool dual_mic_ready;
    bool dual_mic_voice_ok;
    bool live_cancel_candidate;
    uint32_t live_candidate_elapsed_ms;
    uint32_t live_candidate_ts;
    nuwax_p3_nearend_candidate_result_t live_candidate;
    bool passed;
    char result[32];
} nuwax_p5_fullduplex_overlap_probe_result_t;

esp_err_t nuwax_p5_fullduplex_overlap_probe_run(nuwax_ws_client_t *ws,
                                                const char *request_id,
                                                const char *audio_session_id,
                                                uint32_t duration_ms,
                                                nuwax_p5_fullduplex_overlap_probe_result_t *out_result);

esp_err_t nuwax_p5_fullduplex_overlap_probe_run_ex(nuwax_ws_client_t *ws,
                                                   const char *request_id,
                                                   const char *audio_session_id,
                                                   uint32_t duration_ms,
                                                   uint32_t delay_samples,
                                                   bool enable_live_cancel_candidate,
                                                   nuwax_p5_fullduplex_overlap_probe_result_t *out_result);

esp_err_t nuwax_p5_fullduplex_overlap_probe_run_ex_if_epoch(
    nuwax_ws_client_t *ws,
    uint32_t expected_ws_epoch,
    const char *request_id,
    const char *audio_session_id,
    uint32_t duration_ms,
    uint32_t delay_samples,
    bool enable_live_cancel_candidate,
    nuwax_p5_fullduplex_overlap_probe_result_t *out_result);
