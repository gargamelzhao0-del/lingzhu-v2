#include <assert.h>
#include <stdio.h>
#include "touch_read_policy.h"

int main(void)
{
    assert(!nuwax_touch_read_should_cancel(false, false, 200, 0));
    assert(!nuwax_touch_read_should_cancel(true, true, 110, 100));
    assert(!nuwax_touch_read_should_cancel(true, true, 199, 100));
    assert(nuwax_touch_read_should_cancel(true, true, 200, 100));
    assert(nuwax_touch_read_should_cancel(true, true, 201, 100));
    assert(nuwax_touch_read_should_cancel(true, false, 101, 100));
    // A valid sample renews the grace period; skipped reads must not renew it.
    assert(!nuwax_touch_read_should_cancel(true, true, 201, 190));
    assert(!nuwax_touch_read_should_cancel(true, true, 20, UINT32_MAX-50));
    assert(nuwax_touch_read_should_cancel(true, true, 49, UINT32_MAX-50));
    puts("PASS: transient bus contention retains gesture, bounded expiry, hard failure, idle and clock wrap");
}
