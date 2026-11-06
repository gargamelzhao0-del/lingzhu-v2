#include <assert.h>
#include <stdio.h>
#include "board/sd_event_profile.h"

int main(void)
{
    nuwax_sd_event_profile_t p = nuwax_sd_event_profile();
    assert(p.events == 0);
    nuwax_sd_event_profile_record(10, 90010, 2, true);
    nuwax_sd_event_profile_record(100, 1100, 3000, false);
    nuwax_sd_event_profile_record(100, 1101, 3001, false);
    nuwax_sd_event_profile_record(100, 5100, 7000, false);
    nuwax_sd_event_profile_record(100, 5101, 7001, false);
    nuwax_sd_event_profile_record(UINT32_MAX - 99U, 100, 500, false);
    p = nuwax_sd_event_profile();
    assert(p.events == 6 && p.queued_on_entry == 1 && p.empty_entry_events == 5);
    assert(p.empty_entry_lag_us == 12202 && p.empty_entry_over1ms == 3 && p.empty_entry_over5ms == 1);
    assert(p.receive_wait_us == 20504);
    const nuwax_sd_event_profile_t next = nuwax_sd_event_profile();
    assert(next.events == p.events && next.empty_entry_lag_us == p.empty_entry_lag_us);
    puts("PASS: SD event counters exclude pending events from lag, threshold edges, timer wrap, cumulative snapshots");
    return 0;
}
