#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "conversation/live_aec_output_provenance.h"

typedef enum {
    NUWAX_LIVE_AEC_RESCUE_SOURCE_RAW = 0,
    NUWAX_LIVE_AEC_RESCUE_SOURCE_SCALAR = 1,
    NUWAX_LIVE_AEC_RESCUE_SOURCE_FIR = 2,
} nuwax_live_aec_rescue_source_kind_t;

#define NUWAX_LIVE_AEC_RESCUE_SEGMENT_CAPACITY 8U

typedef struct {
    nuwax_live_aec_rescue_source_kind_t kind;
    size_t sample_count;
    uint32_t mix_q15;
} nuwax_live_aec_rescue_segment_t;

typedef struct {
    nuwax_live_aec_rescue_segment_t
        segments[NUWAX_LIVE_AEC_RESCUE_SEGMENT_CAPACITY];
    size_t segment_count;
    size_t sample_count;
} nuwax_live_aec_rescue_provenance_t;

typedef struct {
    bool enabled;
    bool initialized;
    bool output_started;
    bool pending_external_ref;
    bool internal_software_ref_enabled;
    bool nearend_preserve_hint;
    bool strong_nearend_preserve_hint;
    bool provisional_human_protect;
    bool fast_human_protect;
    bool confirmed_human_protect;
    bool human_rescue_ref_gain_valid;
    bool human_rescue_fir_available;
    bool human_rescue_fir_initialized;
    bool human_rescue_fir_ready;
    bool human_rescue_fir_validating;
    bool human_rescue_fir_frozen;
    bool human_rescue_fir_ready_health_hint_seen;
    bool human_rescue_source_valid;
    bool human_rescue_source_fir;
    nuwax_live_aec_rescue_source_kind_t human_rescue_source_kind;
    void *aec;
    int chunk_samples;
    uint32_t delay_samples;
    uint32_t adaptive_ref_nominal_delay_samples;
    int16_t *pending_mic;
    int16_t *pending_mic2;
    int16_t *pending_ref;
    int16_t *pending_fir_ref;
    size_t pending_count;
    bool pending_dual_mic;
    int16_t *ref_chunk;
    int16_t *probe_ref_chunk;
    int16_t *best_ref_chunk;
    int16_t *mr_chunk;
    int16_t *out_chunk;
    int16_t *human_rescue_source_chunk;
    int16_t *human_rescue_fifo;
    int16_t *human_rescue_output_frame;
    float *human_rescue_fir_coeffs;
    float *human_rescue_fir_ref_history;
    int16_t *output_fifo;
    size_t output_capacity;
    size_t output_read;
    size_t output_count;
    size_t human_rescue_read;
    size_t human_rescue_count;
    nuwax_live_aec_rescue_provenance_t human_rescue_queued;
    nuwax_live_aec_rescue_provenance_t last_human_rescue_output;
    size_t human_rescue_output_count;
    bool last_output_emitted;
    nuwax_live_aec_provenance_queue_t output_provenance;
    nuwax_live_aec_output_stats_t last_output;
    uint32_t processed_chunks;
    uint32_t bypass_chunks;
    uint32_t external_ref_chunks;
    uint32_t external_ref_inactive_chunks;
    uint32_t ref_missing_chunks;
    uint32_t ref_missing_attenuated_chunks;
    uint32_t ref_inactive_preserved_chunks;
    uint32_t ref_inactive_attenuated_chunks;
    uint32_t internal_ref_disabled_chunks;
    uint32_t adaptive_ref_chunks;
    uint32_t adaptive_ref_scans;
    uint32_t adaptive_ref_probe_attempts;
    uint32_t adaptive_ref_active_candidates;
    uint32_t adaptive_ref_probe_copied_low;
    uint32_t adaptive_ref_probe_ref_inactive;
    uint32_t adaptive_ref_probe_no_improve;
    uint32_t adaptive_ref_last_base_corr_q15;
    uint32_t adaptive_ref_last_best_corr_q15;
    uint32_t adaptive_ref_corr_max_q15;
    uint32_t adaptive_ref_offset_abs_sum;
    int32_t adaptive_ref_last_offset_samples;
    uint32_t output_underrun_frames;
    uint32_t clipped_samples;
    uint32_t output_gain_q15;
    uint32_t human_rescue_mix_q15;
    uint32_t nearend_blend_chunks;
    uint32_t strong_nearend_blend_chunks;
    uint32_t residual_cancel_chunks;
    uint32_t residual_nearend_skip_chunks;
    uint32_t post_blend_residual_cancel_chunks;
    uint32_t nearend_blend_ref_guard_chunks;
    uint32_t nearend_blend_output_guard_chunks;
    uint32_t nearend_blend_dual_guard_chunks;
    uint32_t nearend_hint_chunks;
    uint32_t strong_nearend_hint_chunks;
    uint32_t nearend_hint_output_guard_bypass_chunks;
    uint32_t ref_missing_hint_preserved_chunks;
    uint32_t dual_restore_chunks;
    uint32_t dual_restore_hint_chunks;
    uint32_t dual_restore_guard_chunks;
    uint32_t dual_restore_spatial_guard_chunks;
    uint32_t dual_restore_corr_guard_chunks;
    uint32_t dual_restore_no_ref_guard_chunks;
    uint32_t dual_restore_post_skip_chunks;
    uint32_t dual_restore_post_residual_chunks;
    uint32_t dual_restore_score_max_q15;
    uint32_t dual_restore_gain_sum_q15;
    uint32_t dual_restore_diff_avg_max;
    uint32_t dual_restore_diff_peak_max;
    uint32_t dual_restore_balance_min_q15;
    uint32_t human_rescue_chunks;
    uint32_t human_rescue_provisional_chunks;
    uint32_t human_rescue_fast_chunks;
    uint32_t human_rescue_confirmed_chunks;
    uint32_t human_rescue_release_chunks;
    uint32_t human_rescue_mix_sum_q15;
    uint32_t human_rescue_mix_max_q15;
    uint32_t human_rescue_ref_corr_max_q15;
    uint32_t human_rescue_ref_gain_updates;
    uint32_t human_rescue_ref_gain_rejects;
    uint32_t human_rescue_ref_gain_latches;
    uint32_t human_rescue_no_ref_gain_chunks;
    int32_t human_rescue_ref_gain_q15;
    int32_t last_human_rescue_ref_gain_q15;
    size_t human_rescue_source_count;
    uint32_t human_rescue_fir_history_pos;
    float human_rescue_fir_ref_energy;
    uint32_t human_rescue_fir_warmup_samples;
    uint32_t human_rescue_fir_session_training_samples;
    uint32_t human_rescue_fir_training_samples;
    uint32_t human_rescue_fir_training_chunks;
    uint32_t human_rescue_fir_freeze_events;
    uint32_t human_rescue_fir_frozen_chunks;
    uint32_t human_rescue_fir_ready_events;
    uint32_t human_rescue_fir_ready_lost_events;
    uint32_t human_rescue_fir_nearend_events;
    uint32_t human_rescue_fir_nearend_hold_chunks;
    uint32_t human_rescue_fir_health_boundary_resets;
    uint32_t human_rescue_fir_validation_starts;
    uint32_t human_rescue_fir_validation_passes;
    uint32_t human_rescue_fir_validation_failures;
    uint32_t human_rescue_fir_reference_epoch_resets;
    uint32_t human_rescue_fir_reference_reanchors;
    uint32_t human_rescue_fir_reference_retrain_reanchors;
    uint32_t human_rescue_fir_validation_remaining;
    int32_t human_rescue_fir_validation_scalar_gain_q15;
    uint32_t human_rescue_fir_rescue_chunks;
    uint32_t human_rescue_fir_fallback_chunks;
    uint32_t human_rescue_fir_divergences;
    uint32_t human_rescue_fir_last_residual_raw_q15;
    uint32_t human_rescue_fir_last_residual_scalar_q15;
    uint32_t human_rescue_fir_residual_raw_ema_q15;
    uint32_t human_rescue_fir_residual_scalar_ema_q15;
    uint32_t human_rescue_fir_prediction_corr_q15;
    uint32_t human_rescue_fir_last_ref_corr_q15;
    uint32_t human_rescue_fir_last_gate_corr_q15;
    uint32_t human_rescue_fir_last_validation_raw_q15;
    uint32_t human_rescue_fir_last_validation_scalar_q15;
    uint32_t human_rescue_fir_last_validation_prediction_corr_q15;
    uint32_t human_rescue_fir_coeff_peak_q15;
    uint32_t human_rescue_fir_coeff_peak_tap;
    uint32_t human_rescue_fir_edge_energy_q15;
    uint32_t human_rescue_fir_bad_health_windows;
    uint32_t human_rescue_fir_ready_bad_windows;
    uint32_t human_rescue_fir_ready_health_chunks;
    uint32_t human_rescue_fir_ready_health_windows;
    uint32_t human_rescue_fir_ready_health_failures;
    uint32_t human_rescue_fir_last_ready_health_raw_q15;
    uint32_t human_rescue_fir_last_ready_health_scalar_q15;
    uint64_t human_rescue_fir_training_raw_energy;
    uint64_t human_rescue_fir_training_residual_energy;
    uint64_t human_rescue_fir_training_scalar_energy;
    uint64_t human_rescue_fir_training_prediction_energy;
    int64_t human_rescue_fir_training_prediction_mic_dot;
    uint64_t human_rescue_fir_validation_raw_energy;
    uint64_t human_rescue_fir_validation_residual_energy;
    uint64_t human_rescue_fir_validation_scalar_energy;
    uint64_t human_rescue_fir_validation_prediction_energy;
    int64_t human_rescue_fir_validation_prediction_mic_dot;
    uint64_t human_rescue_fir_ready_health_raw_energy;
    uint64_t human_rescue_fir_ready_health_residual_energy;
    uint64_t human_rescue_fir_ready_health_scalar_energy;
    uint32_t human_rescue_fir_dsp_chunks;
    uint64_t human_rescue_fir_dsp_us_sum;
    uint32_t human_rescue_fir_dsp_us_max;
    bool human_rescue_fir_last_session_ready;
    uint32_t human_rescue_fir_last_session_training_samples;
    uint32_t human_rescue_fir_last_session_raw_q15;
    uint32_t human_rescue_fir_last_session_scalar_q15;
    uint32_t human_rescue_fir_last_session_prediction_corr_q15;
    uint32_t human_rescue_fir_last_session_coeff_peak_q15;
    uint32_t human_rescue_fir_last_session_coeff_peak_tap;
    uint32_t human_rescue_fir_last_session_edge_energy_q15;
    bool human_rescue_fir_last_validation_passed;
    uint32_t nearend_floor_restore_chunks;
    uint32_t nearend_floor_guard_chunks;
    uint32_t nearend_floor_ref_guard_chunks;
    uint32_t nearend_floor_gain_sum_q15;
    uint32_t nearend_floor_diff_avg_max;
    uint32_t nearend_floor_balance_min_q15;
    uint32_t last_residual_corr_q15;
    int32_t last_residual_gain_q15;
    uint32_t last_post_blend_residual_corr_q15;
    int32_t last_post_blend_residual_gain_q15;
    uint32_t last_mic_ref_corr_q15;
    int32_t last_mic_ref_gain_q15;
    uint32_t last_mic_avg_abs;
    uint32_t last_mic_peak;
    uint32_t last_ref_avg_abs;
    uint32_t last_ref_peak;
    uint32_t last_out_avg_abs;
    uint32_t last_out_peak;
} nuwax_live_aec_t;

void nuwax_live_aec_init(nuwax_live_aec_t *aec);
esp_err_t nuwax_live_aec_prepare(nuwax_live_aec_t *aec, size_t frame_samples);
esp_err_t nuwax_live_aec_begin_session(nuwax_live_aec_t *aec, size_t frame_samples);
void nuwax_live_aec_deinit(nuwax_live_aec_t *aec);
bool nuwax_live_aec_is_enabled(const nuwax_live_aec_t *aec);
void nuwax_live_aec_set_nearend_hint(nuwax_live_aec_t *aec, bool nearend_hint, bool strong_hint);
void nuwax_live_aec_set_human_protection(nuwax_live_aec_t *aec,
                                         bool provisional_human,
                                         bool fast_human,
                                         bool confirmed_human);
void nuwax_live_aec_set_internal_software_ref_enabled(nuwax_live_aec_t *aec, bool enabled);
void nuwax_live_aec_reset_reference_epoch(nuwax_live_aec_t *aec);
bool nuwax_live_aec_reanchor_reference(nuwax_live_aec_t *aec);
bool nuwax_live_aec_copy_human_rescue_source(const nuwax_live_aec_t *aec,
                                             int16_t *out,
                                             size_t sample_count,
                                             nuwax_live_aec_rescue_source_kind_t *source_kind);
bool nuwax_live_aec_copy_human_rescue_frame(
    const nuwax_live_aec_t *aec,
    int16_t *out,
    size_t sample_count,
    nuwax_live_aec_rescue_provenance_t *provenance);
bool nuwax_live_aec_rescue_provenance_kind_at(
    const nuwax_live_aec_rescue_provenance_t *provenance,
    size_t sample_index,
    nuwax_live_aec_rescue_source_kind_t *kind);
bool nuwax_live_aec_rescue_provenance_sample_at(
    const nuwax_live_aec_rescue_provenance_t *provenance,
    size_t sample_index,
    nuwax_live_aec_rescue_source_kind_t *kind,
    uint32_t *mix_q15);
bool nuwax_live_aec_output_emitted(const nuwax_live_aec_t *aec);

/* Discard an incomplete adapter tail at a stream boundary. The return value
 * is the number of not-yet-emitted timeline samples that were removed. */
size_t nuwax_live_aec_flush_pending(nuwax_live_aec_t *aec);
esp_err_t nuwax_live_aec_process(nuwax_live_aec_t *aec,
                                 int16_t *samples,
                                 size_t sample_count,
                                 bool playback_active);
esp_err_t nuwax_live_aec_process_with_dual_mic(nuwax_live_aec_t *aec,
                                               int16_t *samples,
                                               const int16_t *mic2_samples,
                                               size_t sample_count,
                                               bool playback_active);
esp_err_t nuwax_live_aec_process_with_ref(nuwax_live_aec_t *aec,
                                          int16_t *samples,
                                          const int16_t *ref_samples,
                                          size_t sample_count,
                                          bool playback_active);
esp_err_t nuwax_live_aec_process_with_ref_and_dual_mic(nuwax_live_aec_t *aec,
                                                       int16_t *samples,
                                                       const int16_t *mic2_samples,
                                                       const int16_t *ref_samples,
                                                       size_t sample_count,
                                                       bool playback_active);
esp_err_t nuwax_live_aec_process_with_refs_and_dual_mic(nuwax_live_aec_t *aec,
                                                        int16_t *samples,
                                                        const int16_t *mic2_samples,
                                                        const int16_t *aec_ref_samples,
                                                        const int16_t *fir_ref_samples,
                                                        size_t sample_count,
                                                        bool playback_active);
void nuwax_live_aec_log_summary(const nuwax_live_aec_t *aec);
