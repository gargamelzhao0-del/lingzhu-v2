#include <assert.h>
#include <string.h>
#include "conversation/wake_noise_policy.h"

int main(void) {
    nuwax_wake_noise_t n = {0};
    uint32_t now = 0, floor = 0;
    for (unsigned i = 0; i < 64; ++i, now += 32)
        floor = nuwax_wake_noise_observe(&n, now, 50, 150);
    assert(floor == 50);
    /* Quiet speech must not train its own rejection threshold. */
    nuwax_wake_evidence_t e = {0};
    for (unsigned i = 0; i < 32; ++i, now += 32) {
        floor = nuwax_wake_noise_observe(&n, now, 120, 500);
        assert(floor == 50 && n.rise_held);
        const bool voice = nuwax_wake_voice_frame_is_voice_like(120, 500, floor);
        assert(voice);
        nuwax_wake_evidence_observe(&e, now, 512, 0, 120, voice);
    }
    nuwax_wake_evidence_snapshot_t ev = nuwax_wake_evidence_snapshot(&e, now - 32, 16000);
    assert(nuwax_wake_detection_accept(117, floor, ev.signal_max, ev.voiced_ms,
                                      ev.clipped_samples, ev.total_samples));
    assert(strcmp(nuwax_wake_shadow_reason(117, floor, &ev), "pass") == 0);
    /* A sustained louder background is learned after the bounded hold. */
    for (unsigned i = 0; i < 500; ++i, now += 32)
        floor = nuwax_wake_noise_observe(&n, now, 300, 900);
    assert(floor >= 290 && floor <= 300);
    for (unsigned i = 0; i < 100; ++i, now += 32)
        floor = nuwax_wake_noise_observe(&n, now, 30, 100);
    assert(floor <= 35);
    const uint32_t preserved = n.floor_q8;
    nuwax_wake_noise_reset(&n);
    assert(n.floor_q8 == preserved && !n.voice_active && !n.initialized);
    assert(nuwax_wake_noise_observe(&n, now, 1000, 3000) < 40);

    /* Time wrap, startup silence and impulse do not corrupt the floor. */
    n = (nuwax_wake_noise_t){0};
    now = UINT32_MAX - 1023U;
    for (unsigned i = 0; i < 200; ++i, now += 32)
        assert(nuwax_wake_noise_observe(&n, now, 0, 0) == 1);
    for (unsigned i = 0; i < 100; ++i, now += 32)
        floor = nuwax_wake_noise_observe(&n, now, 50, 150);
    assert(floor >= 45);
    floor = nuwax_wake_noise_observe(&n, now, 20000, 32000);
    assert(floor <= 50);

    /* Speech elsewhere in the broad evidence window does not count locally. */
    nuwax_wake_evidence_reset(&e);
    for (unsigned i = 0; i < 10; ++i)
        nuwax_wake_evidence_observe(&e, i * 32, 512, 0, 300, true);
    for (unsigned i = 10; i < 27; ++i)
        nuwax_wake_evidence_observe(&e, i * 32, 512, 0, 20, false);
    ev = nuwax_wake_evidence_snapshot(&e, 26 * 32, 16000);
    assert(ev.voiced_ms == 320 && ev.local_voice_ms == 0);
    assert(nuwax_wake_detection_accept(117, 50, ev.signal_max, ev.voiced_ms,
                                      ev.clipped_samples, ev.total_samples));
    assert(strcmp(nuwax_wake_shadow_reason(117, 50, &ev), "recent_voice_short") == 0);
    /* Shadow does not promote model rejects or become an admission gate. */
    assert(strcmp(nuwax_wake_shadow_reason(89, 50, &ev), "score") == 0);
    return 0;
}
