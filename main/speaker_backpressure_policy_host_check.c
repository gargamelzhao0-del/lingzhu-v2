#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "conversation/speaker_backpressure_policy.h"

int main(void) {
    assert(nuwax_speaker_backpressure_poll_wait_ms(1000U, 4000U, 250U) == 250U);
    assert(nuwax_speaker_backpressure_poll_wait_ms(3900U, 4000U, 250U) == 100U);
    assert(nuwax_speaker_backpressure_poll_wait_ms(4000U, 4000U, 250U) == 0U);
    assert(nuwax_speaker_backpressure_poll_wait_ms(4001U, 4000U, 250U) == 0U);
    assert(!nuwax_speaker_backpressure_deadline_reached(3999U, 4000U));
    assert(nuwax_speaker_backpressure_deadline_reached(4000U, 4000U));

    const uint32_t wrapped_start_ms = UINT32_MAX - 100U;
    const uint32_t wrapped_deadline_ms = wrapped_start_ms + 300U;
    assert(nuwax_speaker_backpressure_poll_wait_ms(wrapped_start_ms,
                                                   wrapped_deadline_ms,
                                                   250U) == 250U);
    assert(nuwax_speaker_backpressure_poll_wait_ms(UINT32_MAX - 10U,
                                                   wrapped_deadline_ms,
                                                   250U) == 210U);
    assert(nuwax_speaker_backpressure_poll_wait_ms(wrapped_deadline_ms,
                                                   wrapped_deadline_ms,
                                                   250U) == 0U);

    puts("speaker backpressure policy host check: PASS");
    return 0;
}
