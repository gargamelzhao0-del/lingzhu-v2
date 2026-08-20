#ifndef NUWAX_AEC_FIR_DELAY_CONTROL_H
#define NUWAX_AEC_FIR_DELAY_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t acoustic_delay_samples;
    uint32_t target_delay_samples;
    uint32_t target_tap;
    uint32_t revision;
    uint32_t generation;
    uint64_t anchor_events;
    uint32_t decision_sequence;
    uint32_t confidence_q15;
} nuwax_aec_fir_delay_request_t;

typedef struct {
    uint32_t baseline_delay_samples;
    uint32_t active_delay_samples;
    uint32_t active_revision;
    uint32_t active_generation;
    uint32_t next_revision;
    uint32_t requested_generation;
} nuwax_aec_fir_delay_control_t;

typedef struct {
    bool changed;
    uint32_t old_delay_samples;
    uint32_t new_delay_samples;
    uint32_t old_revision;
    uint32_t new_revision;
    uint32_t generation;
} nuwax_aec_fir_delay_transition_t;

void nuwax_aec_fir_delay_control_init(nuwax_aec_fir_delay_control_t *control,
                                      uint32_t baseline_delay_samples);

bool nuwax_aec_fir_delay_plan(nuwax_aec_fir_delay_control_t *control,
                              uint32_t acoustic_delay_samples,
                              uint32_t target_tap,
                              uint32_t min_delay_samples,
                              uint32_t max_delay_samples,
                              uint32_t generation,
                              uint64_t anchor_events,
                              uint32_t decision_sequence,
                              uint32_t confidence_q15,
                              nuwax_aec_fir_delay_request_t *request);

bool nuwax_aec_fir_delay_commit(nuwax_aec_fir_delay_control_t *control,
                                const nuwax_aec_fir_delay_request_t *request,
                                uint32_t current_generation,
                                uint64_t current_anchor_events,
                                nuwax_aec_fir_delay_transition_t *transition);

#ifdef __cplusplus
}
#endif

#endif
