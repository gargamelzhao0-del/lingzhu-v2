#pragma once

#include <stdint.h>

/* Map an animation deadline to its logical frame. Delayed LVGL callbacks skip
 * expired poses so rendering pressure cannot turn a bounded transition into
 * slow motion. */
static inline uint32_t nuwax_control_center_timeline_step(
    uint32_t total_steps,
    uint32_t duration_ms,
    int64_t started_us,
    int64_t now_us)
{
    if (total_steps == 0U) return 0U;
    if (duration_ms == 0U) return total_steps;
    if (now_us <= started_us) return 0U;

    const uint64_t elapsed_us = (uint64_t)(now_us - started_us);
    const uint64_t duration_us = (uint64_t)duration_ms * 1000ULL;
    if (elapsed_us >= duration_us) return total_steps;

    uint64_t step =
        (elapsed_us * total_steps + duration_us - 1ULL) / duration_us;
    if (step == 0U) step = 1U;
    if (step > total_steps) step = total_steps;
    return (uint32_t)step;
}
