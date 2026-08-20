#include "aec_fir_delay_control.h"

#include <string.h>

void nuwax_aec_fir_delay_control_init(nuwax_aec_fir_delay_control_t *control,
                                      uint32_t baseline_delay_samples) {
    if (!control) {
        return;
    }
    memset(control, 0, sizeof(*control));
    control->baseline_delay_samples = baseline_delay_samples;
    control->active_delay_samples = baseline_delay_samples;
    control->active_revision = 1U;
    control->next_revision = 1U;
}

bool nuwax_aec_fir_delay_plan(nuwax_aec_fir_delay_control_t *control,
                              uint32_t acoustic_delay_samples,
                              uint32_t target_tap,
                              uint32_t min_delay_samples,
                              uint32_t max_delay_samples,
                              uint32_t generation,
                              uint64_t anchor_events,
                              uint32_t decision_sequence,
                              uint32_t confidence_q15,
                              nuwax_aec_fir_delay_request_t *request) {
    if (!control || !request || generation == 0U || anchor_events == 0U ||
        min_delay_samples > max_delay_samples || acoustic_delay_samples <= target_tap ||
        control->requested_generation == generation) {
        return false;
    }
    const uint32_t target_delay = acoustic_delay_samples - target_tap;
    if (target_delay < min_delay_samples || target_delay > max_delay_samples) {
        return false;
    }

    uint32_t revision = control->next_revision + 1U;
    if (revision == 0U) {
        revision = 1U;
    }
    control->next_revision = revision;
    control->requested_generation = generation;
    *request = (nuwax_aec_fir_delay_request_t){
        .acoustic_delay_samples = acoustic_delay_samples,
        .target_delay_samples = target_delay,
        .target_tap = target_tap,
        .revision = revision,
        .generation = generation,
        .anchor_events = anchor_events,
        .decision_sequence = decision_sequence,
        .confidence_q15 = confidence_q15,
    };
    return true;
}

bool nuwax_aec_fir_delay_commit(nuwax_aec_fir_delay_control_t *control,
                                const nuwax_aec_fir_delay_request_t *request,
                                uint32_t current_generation,
                                uint64_t current_anchor_events,
                                nuwax_aec_fir_delay_transition_t *transition) {
    if (transition) {
        memset(transition, 0, sizeof(*transition));
    }
    if (!control || !request || !transition || request->revision == 0U ||
        request->generation == 0U || request->generation != current_generation ||
        request->anchor_events == 0U || request->anchor_events != current_anchor_events) {
        return false;
    }

    transition->old_delay_samples = control->active_delay_samples;
    transition->new_delay_samples = request->target_delay_samples;
    transition->old_revision = control->active_revision;
    transition->new_revision = control->active_revision;
    transition->generation = current_generation;
    if (request->target_delay_samples == control->active_delay_samples) {
        control->active_generation = current_generation;
        return true;
    }

    control->active_delay_samples = request->target_delay_samples;
    control->active_revision = request->revision;
    control->active_generation = current_generation;
    transition->changed = true;
    transition->new_revision = request->revision;
    return true;
}
