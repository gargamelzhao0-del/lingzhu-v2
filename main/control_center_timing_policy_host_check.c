#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "nuwax_ui_v28/control_center_timing_policy.h"

int main(void)
{
    assert(nuwax_control_center_timeline_step(0U, 1300U, 1000, 2000) ==
           0U);
    assert(nuwax_control_center_timeline_step(52U, 0U, 1000, 1000) ==
           52U);
    assert(nuwax_control_center_timeline_step(52U, 1300U, 1000, 999) ==
           0U);
    assert(nuwax_control_center_timeline_step(52U, 1300U, 1000, 1000) ==
           0U);
    assert(nuwax_control_center_timeline_step(52U, 1300U, 1000,
                                              1000 + 25000) == 1U);
    assert(nuwax_control_center_timeline_step(52U, 1300U, 1000,
                                              1000 + 160000) == 7U);
    assert(nuwax_control_center_timeline_step(52U, 1300U, 1000,
                                              1000 + 1299999) == 52U);
    assert(nuwax_control_center_timeline_step(52U, 1300U, 1000,
                                              1000 + 1300000) == 52U);
    assert(nuwax_control_center_timeline_step(26U, 650U, 5000,
                                              5000 + 325000) == 13U);
    puts("control center timing policy: PASS");
    return 0;
}
