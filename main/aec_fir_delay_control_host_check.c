#include <stdio.h>

#include "conversation/aec_fir_delay_control.h"

static int check_plan_and_commit(void) {
    nuwax_aec_fir_delay_control_t control;
    nuwax_aec_fir_delay_control_init(&control, 1216U);
    nuwax_aec_fir_delay_request_t request = {0};
    const bool planned = nuwax_aec_fir_delay_plan(&control, 1444U, 120U, 0U, 3072U,
                                                   7U, 3U, 100U, 28000U, &request);
    nuwax_aec_fir_delay_transition_t transition = {0};
    const bool stale_rejected =
        !nuwax_aec_fir_delay_commit(&control, &request, 8U, 3U, &transition);
    const bool committed =
        nuwax_aec_fir_delay_commit(&control, &request, 7U, 3U, &transition);
    const bool pass = planned && request.target_delay_samples == 1324U &&
                      request.revision == 2U && stale_rejected && committed &&
                      transition.changed && transition.old_delay_samples == 1216U &&
                      transition.new_delay_samples == 1324U &&
                      control.active_revision == 2U;
    printf("[%s] plan_commit target=%lu revision=%lu active=%lu\n",
           pass ? "PASS" : "FAIL",
           (unsigned long)request.target_delay_samples,
           (unsigned long)request.revision,
           (unsigned long)control.active_delay_samples);
    return pass ? 0 : 1;
}

static int check_one_request_per_generation(void) {
    nuwax_aec_fir_delay_control_t control;
    nuwax_aec_fir_delay_control_init(&control, 1216U);
    nuwax_aec_fir_delay_request_t first = {0};
    nuwax_aec_fir_delay_request_t second = {0};
    const bool planned =
        nuwax_aec_fir_delay_plan(&control, 1544U, 120U, 0U, 3072U,
                                 11U, 9U, 200U, 30000U, &first);
    const bool duplicate_rejected =
        !nuwax_aec_fir_delay_plan(&control, 1538U, 120U, 0U, 3072U,
                                  11U, 9U, 220U, 30000U, &second);
    const bool next_generation =
        nuwax_aec_fir_delay_plan(&control, 1538U, 120U, 0U, 3072U,
                                 12U, 10U, 240U, 30000U, &second);
    const bool pass = planned && duplicate_rejected && next_generation &&
                      first.revision == 2U && second.revision == 3U;
    printf("[%s] generation_gate revisions=%lu,%lu\n",
           pass ? "PASS" : "FAIL",
           (unsigned long)first.revision,
           (unsigned long)second.revision);
    return pass ? 0 : 1;
}

static int check_same_target_is_noop(void) {
    nuwax_aec_fir_delay_control_t control;
    nuwax_aec_fir_delay_control_init(&control, 1216U);
    nuwax_aec_fir_delay_request_t request = {0};
    const bool planned =
        nuwax_aec_fir_delay_plan(&control, 1336U, 120U, 0U, 3072U,
                                 5U, 2U, 50U, 30000U, &request);
    nuwax_aec_fir_delay_transition_t transition = {0};
    const bool committed =
        nuwax_aec_fir_delay_commit(&control, &request, 5U, 2U, &transition);
    const bool pass = planned && committed && !transition.changed &&
                      control.active_delay_samples == 1216U &&
                      control.active_revision == 1U &&
                      control.active_generation == 5U;
    printf("[%s] same_target_noop active=%lu revision=%lu\n",
           pass ? "PASS" : "FAIL",
           (unsigned long)control.active_delay_samples,
           (unsigned long)control.active_revision);
    return pass ? 0 : 1;
}

static int check_invalid_mapping(void) {
    nuwax_aec_fir_delay_control_t control;
    nuwax_aec_fir_delay_control_init(&control, 1216U);
    nuwax_aec_fir_delay_request_t request = {0};
    const bool underflow_rejected =
        !nuwax_aec_fir_delay_plan(&control, 120U, 120U, 0U, 3072U,
                                  1U, 1U, 0U, 0U, &request);
    const bool range_rejected =
        !nuwax_aec_fir_delay_plan(&control, 3200U, 120U, 0U, 3000U,
                                  2U, 1U, 0U, 0U, &request);
    const bool pass = underflow_rejected && range_rejected;
    printf("[%s] invalid_mapping\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}

int main(void) {
    int failures = 0;
    failures += check_plan_and_commit();
    failures += check_one_request_per_generation();
    failures += check_same_target_is_noop();
    failures += check_invalid_mapping();
    printf("AEC FIR delay control host check: %s (%d failures)\n",
           failures == 0 ? "PASS" : "FAIL",
           failures);
    return failures == 0 ? 0 : 1;
}
