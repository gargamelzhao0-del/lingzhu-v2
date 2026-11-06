#include "board/sd_event_profile.h"
#include <stdatomic.h>

static atomic_uint_fast32_t events, pending, empty, lag, over1, over5, wait_time;
void nuwax_sd_event_profile_record(uint32_t queued_us, uint32_t received_us,
                                 uint32_t wait_us, bool queued_on_entry)
{
    const uint32_t elapsed = received_us - queued_us; /* timer wrap-safe */
    atomic_fetch_add_explicit(&events, 1U, memory_order_relaxed);
    atomic_fetch_add_explicit(&wait_time, wait_us, memory_order_relaxed);
    if (queued_on_entry) {
        atomic_fetch_add_explicit(&pending, 1U, memory_order_relaxed);
    } else {
        atomic_fetch_add_explicit(&empty, 1U, memory_order_relaxed);
        atomic_fetch_add_explicit(&lag, elapsed, memory_order_relaxed);
        if (elapsed > 1000U) atomic_fetch_add_explicit(&over1, 1U, memory_order_relaxed);
        if (elapsed > 5000U) atomic_fetch_add_explicit(&over5, 1U, memory_order_relaxed);
    }
}
nuwax_sd_event_profile_t nuwax_sd_event_profile(void)
{
    return (nuwax_sd_event_profile_t){
        atomic_load_explicit(&events, memory_order_relaxed),
        atomic_load_explicit(&pending, memory_order_relaxed),
        atomic_load_explicit(&empty, memory_order_relaxed),
        atomic_load_explicit(&lag, memory_order_relaxed),
        atomic_load_explicit(&over1, memory_order_relaxed),
        atomic_load_explicit(&over5, memory_order_relaxed),
        atomic_load_explicit(&wait_time, memory_order_relaxed)};
}
