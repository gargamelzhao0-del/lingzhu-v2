#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t events, queued_on_entry, empty_entry_events;
    uint32_t empty_entry_lag_us, empty_entry_over1ms, empty_entry_over5ms;
    uint32_t receive_wait_us;
} nuwax_sd_event_profile_t;
/* Called only after task-context queue receive, never from the ISR. Lag is
 * enqueue-to-dequeue, not pure hardware latency or guaranteed wakeup latency. */
void nuwax_sd_event_profile_record(uint32_t queued_us, uint32_t received_us,
                                 uint32_t wait_us, bool queued_on_entry);
nuwax_sd_event_profile_t nuwax_sd_event_profile(void);
