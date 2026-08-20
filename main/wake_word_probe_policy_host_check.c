#include <assert.h>
#include <stdint.h>

#include "conversation/wake_word_probe_policy.h"

int main(void) {
    nuwax_wake_probe_policy_t policy = {0};

    assert(!nuwax_wake_probe_policy_observe(&policy, true, 100U));
    assert(!nuwax_wake_probe_policy_observe(&policy, true, 132U));
    assert(nuwax_wake_probe_policy_observe(&policy, true, 164U));
    assert(!nuwax_wake_probe_policy_observe(&policy, true, 196U));

    assert(!nuwax_wake_probe_policy_observe(&policy, false, 228U));
    assert(!nuwax_wake_probe_policy_observe(&policy, true, 260U));
    assert(!nuwax_wake_probe_policy_observe(&policy, true, 292U));
    assert(!nuwax_wake_probe_policy_observe(&policy, true, 324U));

    assert(!nuwax_wake_probe_policy_observe(&policy, false, 15163U));
    assert(!nuwax_wake_probe_policy_observe(&policy, true, 15164U));
    assert(!nuwax_wake_probe_policy_observe(&policy, true, 15196U));
    assert(nuwax_wake_probe_policy_observe(&policy, true, 15228U));

    nuwax_wake_probe_policy_reset(&policy);
    assert(!policy.has_requested);
    assert(!nuwax_wake_probe_policy_observe(&policy, true, 0U));
    assert(!nuwax_wake_probe_policy_observe(&policy, true, 32U));
    assert(nuwax_wake_probe_policy_observe(&policy, true, 64U));
    return 0;
}
