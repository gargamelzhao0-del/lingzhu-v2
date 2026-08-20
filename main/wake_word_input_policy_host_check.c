#include <assert.h>
#include <stdint.h>

#include "conversation/wake_word_input_policy.h"

int main(void) {
    assert(nuwax_wake_input_samples_needed(0U, 512U) == 512U);
    assert(nuwax_wake_input_samples_needed(192U, 512U) == 320U);
    assert(nuwax_wake_input_samples_needed(512U, 512U) == 0U);
    assert(nuwax_wake_input_accept_count(400U, 80U, 512U) == 80U);
    assert(nuwax_wake_input_accept_count(400U, 200U, 512U) == 112U);
    assert(!nuwax_wake_input_gap_is_discontinuous(1000U, 1050U, 50U));
    assert(nuwax_wake_input_gap_is_discontinuous(1000U, 1051U, 50U));
    assert(!nuwax_wake_input_gap_is_discontinuous(0U, 9000U, 50U));
    assert(nuwax_wake_input_next_generation(0U) == 1U);
    assert(nuwax_wake_input_next_generation(UINT32_MAX) == 1U);
    assert(!nuwax_wake_input_generation_changed(0U, 7U));
    assert(!nuwax_wake_input_generation_changed(7U, 7U));
    assert(nuwax_wake_input_generation_changed(7U, 8U));
    assert(nuwax_wake_input_capture_exit_expected(true, true, true));
    assert(!nuwax_wake_input_capture_exit_expected(false, true, true));
    assert(!nuwax_wake_input_capture_exit_expected(true, false, true));
    assert(!nuwax_wake_input_capture_exit_expected(true, true, false));
    assert(!nuwax_wake_input_cooperate_due(false, 1000U, 2000U, 1000U));
    assert(!nuwax_wake_input_cooperate_due(true, 1000U, 1999U, 1000U));
    assert(nuwax_wake_input_cooperate_due(true, 1000U, 2000U, 1000U));
    assert(!nuwax_wake_input_cooperate_due(true, 1000U, 2000U, 0U));
    assert(nuwax_wake_input_cooperate_due(
        true, UINT32_MAX - 499U, 500U, 1000U));
    return 0;
}
