#pragma once

#include <stdbool.h>
#include <stdint.h>

/* A skipped bus transaction is not a finger release. Bound the grace period
 * since the last valid sample so persistent contention cannot latch a drag. */
static inline bool nuwax_touch_read_should_cancel(bool active, bool transient,
                                                 uint32_t now_ms,
                                                 uint32_t last_valid_ms)
{
    return active && (!transient || (uint32_t)(now_ms - last_valid_ms) >= 100U);
}
