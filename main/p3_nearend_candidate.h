#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "p3_softref_aec_diag.h"

typedef struct {
    uint32_t duration_ms;
    uint32_t delay_samples;
    float active_out_to_ref_db;
    float active_out_to_mic_db;
    bool ready;
    bool gate_ok;
    bool quality_gate_ok;
    bool strong_nearend_ok;
    bool mic_ok;
    bool energy_ok;
    bool nearend_observed;
    bool p6_low_ref_nearend_ok;
    bool p6_low_ref_normal_voice_ok;
    bool p6_high_ref_voice_ok;
    bool p6_high_ref_strong_voice_ok;
    bool p6_mid_ref_strong_voice_ok;
    bool p6_normal_voice_ok;
    bool p6_mid_ref_high_erle_voice_ok;
    bool p6_single_mic_voice_shape_ok;
    bool p6_cancel_discriminator_ok;
    bool p6_record_candidate;
    bool p6_cancel_candidate;
    bool candidate;
    nuwax_p3_softref_aec_ab_result_t aec;
} nuwax_p3_nearend_candidate_result_t;

esp_err_t nuwax_p3_nearend_candidate_run(uint32_t duration_ms,
                                         uint32_t delay_samples,
                                         nuwax_p3_nearend_candidate_result_t *out_result);

esp_err_t nuwax_p3_nearend_candidate_evaluate(uint32_t duration_ms,
                                              uint32_t delay_samples,
                                              const nuwax_p3_softref_aec_ab_result_t *aec,
                                              nuwax_p3_nearend_candidate_result_t *out_result);
