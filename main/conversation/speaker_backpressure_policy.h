#pragma once

#include <stdbool.h>
#include <stdint.h>

static inline bool nuwax_speaker_backpressure_deadline_reached(uint32_t now_ms,
                                                               uint32_t deadline_ms) {
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static inline uint32_t nuwax_speaker_backpressure_poll_wait_ms(uint32_t now_ms,
                                                               uint32_t deadline_ms,
                                                               uint32_t max_poll_ms) {
    if (nuwax_speaker_backpressure_deadline_reached(now_ms, deadline_ms)) {
        return 0;
    }

    const uint32_t remaining_ms = deadline_ms - now_ms;
    return remaining_ms < max_poll_ms ? remaining_ms : max_poll_ms;
}
