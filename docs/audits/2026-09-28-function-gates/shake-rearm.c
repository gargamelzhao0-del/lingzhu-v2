#include <assert.h>
#include <stdio.h>
#include "board/shake_motion_policy.h"

static void run_case(int periodic_drop) {
    nuwax_shake_motion_t s = {0};
    unsigned triggers = 0;
    for (unsigned t = 0; t <= 2000; t += 50) {
        float a[] = {650.0f * sinf(6.2831853f * 2.0f * t / 1000.0f), 0, 1000};
        triggers += nuwax_shake_motion_observe(&s, t, a, 0, 4000);
    }
    assert(triggers == 1 && s.inhibited);
    for (unsigned t = 2050; t <= 12000; t += 50) {
        if (periodic_drop && (t / 50) % 5 == 0) {
            nuwax_shake_motion_invalidate(&s);
            continue;
        }
        float still[] = {0, 0, 1000};
        (void)nuwax_shake_motion_observe(&s, t, still, 0, 4000);
    }
    printf("quiet10s periodic20percent=%d inhibited=%d\n", periodic_drop, s.inhibited);
    assert(s.inhibited == (periodic_drop != 0));
    unsigned again = 0;
    for (unsigned t = 12050; t <= 16000; t += 50) {
        float a[] = {650.0f * sinf(6.2831853f * 2.0f * t / 1000.0f), 0, 1000};
        again += nuwax_shake_motion_observe(&s, t, a, 0, 4000);
    }
    printf("next_gesture periodic20percent=%d events=%u\n", periodic_drop, again);
    assert(periodic_drop ? again == 0 : again > 0);
}
int main(void) { run_case(0); run_case(1); return 0; }
