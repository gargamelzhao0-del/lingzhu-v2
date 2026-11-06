#include <assert.h>
#include <stdio.h>
#include "board/shake_motion_policy.h"

static bool observe(nuwax_shake_motion_t *s, uint32_t ms, float x, float y, float z, float gyro) {
    const float a[3] = {x, y, z};
    return nuwax_shake_motion_observe(s, ms, a, gyro, 2000U);
}

static unsigned wave(nuwax_shake_motion_t *s, uint32_t start, unsigned duration,
                     float hz, float amplitude, unsigned axis, float phase) {
    unsigned hits = 0;
    for (unsigned t = 0; t <= duration; t += 50U) {
        float value = amplitude * sinf(6.2831853f * hz * (float)t / 1000.0f + phase);
        float a[3] = {0, 0, 1000};
        if (axis < 3) a[axis] += value;
        else for (unsigned i=0;i<3;i++) a[i] += value * 0.57735027f;
        hits += nuwax_shake_motion_observe(s, start+t, a, 0.0f, 2000U);
    }
    return hits;
}

int main(void) {
    for (unsigned axis=0; axis<4; ++axis) {
        for (unsigned speed=0; speed<3; ++speed) {
            const float hz[] = {1.25f, 2.0f, 3.0f};
            for (unsigned phase=0; phase<4; ++phase) {
                nuwax_shake_motion_t s={0};
                assert(wave(&s,0,4000,hz[speed],650.0f,axis,phase*1.5707963f)==1);
                assert(s.last_gesture_ms>=400 && s.last_gesture_ms<=1500);
            }
        }
    }
    puts("PASS: slow/normal/fast, all axes/diagonal, four start phases, one event per gesture");

    nuwax_shake_motion_t s={0};
    for (unsigned t=0;t<=10000;t+=50)
        assert(!observe(&s,t,0,0,1000,160)); /* Old gyro-only false trigger. */
    s=(nuwax_shake_motion_t){0};
    for (unsigned t=0;t<=10000;t+=50)
        assert(!observe(&s,t,t%250==0?1500:0,0,1000,0)); /* Sparse same-sign bumps. */
    s=(nuwax_shake_motion_t){0};
    for (unsigned t=0;t<=4000;t+=50) {
        float angle = t<1000 ? (float)t*0.0015707963f : 1.5707963f;
        assert(!observe(&s,t,1000*sinf(angle),0,1000*cosf(angle),t<1000?90:0));
    }
    s=(nuwax_shake_motion_t){0};
    assert(wave(&s,0,8000,2.0f,200.0f,3,0)==0); /* Mild walking/jitter. */
    s=(nuwax_shake_motion_t){0};
    assert(wave(&s,0,8000,0.4f,800.0f,0,0)==0); /* Slow carrying sway. */
    s=(nuwax_shake_motion_t){0};
    for (unsigned t=0;t<=2000;t+=50) {
        float x=t==500?1600:t==550?-1000:t==600?400:0;
        assert(!observe(&s,t,x,0,1000,0)); /* One impact with rapid rebound. */
    }
    puts("PASS: rotation, sparse carry bumps, pickup tilt, mild walking, slow sway, impact rebound rejected");
    s=(nuwax_shake_motion_t){0};
    assert(wave(&s,0,650,2.0f,650.0f,0,0)==0); /* Only three lobes. */

    /* Exact window and peak spacing boundaries; the state records only one
     * candidate direction, so orthogonal movement cannot finish its gesture. */
    s=(nuwax_shake_motion_t){0};
    observe(&s,0,0,0,1000,0);
    observe(&s,50,1000,0,1000,0);
    assert(s.peaks==1);
    observe(&s,100,0,-2000,1000,0);
    assert(s.peaks==1);
    assert(!observe(&s,150,1000,0,1000,0));
    assert(s.peaks==1);
    for(unsigned t=200;t<=1650;t+=50) observe(&s,t,0,0,1000,0);
    assert(s.peaks==0 && !s.candidate);

    /* Inject a candidate just below completion to isolate deadline handling. */
    for(unsigned expired=0;expired<2;expired++) {
        s=(nuwax_shake_motion_t){.initialized=true,.last_sample_ms=1450,
            .candidate=true,.candidate_started_ms=0,.last_peak_ms=1250,
            .peaks=3,.sign=1,.direction={1,0,0},.gravity={0,0,1000}};
        assert(observe(&s,1500+expired,-1000,0,1000,0)==!expired);
    }
    s=(nuwax_shake_motion_t){.initialized=true,.last_sample_ms=550,
        .candidate=true,.candidate_started_ms=0,.last_peak_ms=100,
        .peaks=3,.sign=1,.direction={1,0,0},.gravity={0,0,1000}};
    assert(!observe(&s,601,-1000,0,1000,0)); /* Peak interval expiry. */
    assert(s.peaks==1);

    s=(nuwax_shake_motion_t){.initialized=true,.last_sample_ms=550,
        .candidate=true,.candidate_started_ms=0,.last_peak_ms=550,
        .peaks=3,.sign=1,.direction={1,0,0},.gravity={0,0,1000}};
    assert(!observe(&s,600,-1000,0,1000,0)); /* Opposite peak only 50ms later. */
    assert(!s.candidate);

    s=(nuwax_shake_motion_t){0};
    assert(wave(&s,UINT32_MAX-1000U,4000,2.0f,650.0f,0,0)==1);
    assert(s.last_gesture_ms>=400 && s.last_gesture_ms<=1500);
    s=(nuwax_shake_motion_t){0};
    observe(&s,0,0,0,1000,0);
    observe(&s,50,1000,0,1000,0);
    for(unsigned i=0;i<20;i++) assert(!observe(&s,50,(i&1)?-1000:1000,0,1000,0));
    assert(s.peaks==1);
    assert(!observe(&s,500,-1000,0,1000,0));
    assert(!s.candidate && s.invalid_resets>0);
    assert(!observe(&s,550,NAN,0,1000,0));
    assert(!s.initialized);
    assert(wave(&s,1000,3000,2.0f,650.0f,0,0)==1);
    s=(nuwax_shake_motion_t){.initialized=true,.sample_seen=true,.last_sample_ms=500,
        .candidate=true,.candidate_started_ms=0,.last_peak_ms=500,
        .peaks=3,.sign=1,.direction={1,0,0},.gravity={0,0,1000}};
    nuwax_shake_motion_invalidate(&s);
    assert(!observe(&s,550,0,0,1000,0) && s.peaks==3); /* Rebase, no invented peak. */
    assert(observe(&s,600,-1000,0,1000,0)); /* Real fourth lobe after short gap. */
    puts("PASS: direction lock, window/spacing deadlines, timestamp wrap, duplicates, gaps and fault recovery");

    s=(nuwax_shake_motion_t){0};
    assert(wave(&s,0,12000,2.0f,650.0f,0,0)==1); /* Continuous motion never re-arms. */
    assert(s.inhibited);
    nuwax_shake_motion_invalidate(&s);
    assert(s.inhibited && !s.quiet_seen);
    assert(wave(&s,13000,3000,2.0f,650.0f,0,0)==0);
    for(unsigned t=16050;t<=18000;t+=50) observe(&s,t,0,0,1000,100);
    assert(s.inhibited); /* Rotation is not quiet, but cannot trigger either. */
    for(unsigned t=18050;t<=19000;t+=50) assert(!observe(&s,t,0,0,1000,0));
    assert(!s.inhibited && !s.candidate);
    assert(wave(&s,19050,2500,2.0f,650.0f,0,0)==1);
    puts("PASS: 2s cooldown, sustained-motion suppression, quiet re-arm, sensor-gap inhibition retention");
    s=(nuwax_shake_motion_t){.inhibited=true,.triggered_ms=0};
    for(unsigned t=0;t<2000;t+=50) {
        assert(!observe(&s,t,0,0,1000,0));
        assert(s.inhibited);
    }
    assert(!observe(&s,2000,0,0,1000,0) && !s.inhibited);
    /* Bus contention keeps a real baseline: every fifth sample missing must
     * not destroy an otherwise supported deliberate gesture. */
    for (unsigned speed=0;speed<3;speed++) for(unsigned phase=0;phase<4;phase++)
    for(unsigned skip=0;skip<5;skip++) {
        const float hz[]={1.25f,2.0f,3.0f};
        s=(nuwax_shake_motion_t){0}; unsigned hits=0;
        for(unsigned t=0;t<=4000;t+=50) {
            if((t/50)%5==skip) { nuwax_shake_motion_skip(&s); continue; }
            hits+=observe(&s,t,650*sinf(6.2831853f*hz[speed]*t/1000.0f+phase*1.5707963f),0,1000,0);
        }
        assert(hits==1);
    }
    /* Only adjacent valid quiet intervals count. Missing observations and
     * excessively sparse data must not invent the 300 ms quiet requirement. */
    for(unsigned drop=0;drop<3;drop++) {
        s=(nuwax_shake_motion_t){0};
        assert(wave(&s,0,2000,2,650,0,0)==1);
        for(unsigned t=2050;t<=12000;t+=50) {
            if(drop && (t/50)%5==0) {
                if(drop==1) nuwax_shake_motion_skip(&s);
                else nuwax_shake_motion_invalidate(&s);
                continue;
            }
            observe(&s,t,0,0,1000,0);
        }
        assert(s.inhibited==(drop==2));
        assert(wave(&s,12050,4000,2,650,0,0)==(drop==2 ? 0U : 1U));
    }
    s=(nuwax_shake_motion_t){.inhibited=true,.triggered_ms=0};
    for(unsigned t=0;t<=12000;t+=50) {
        if((t/50)%2) nuwax_shake_motion_skip(&s);
        else observe(&s,t,0,0,1000,0);
    }
    assert(s.inhibited && s.quiet_valid_ms==0); /* No adjacent valid pair. */
    s=(nuwax_shake_motion_t){.inhibited=true,.triggered_ms=0,
        .initialized=true,.sample_seen=true,.last_sample_ms=4950,
        .gravity={0,0,1000},.quiet_valid_ms=300};
    assert(!observe(&s,5000,0,0,1000,0) && s.inhibited && s.quiet_valid_ms==0);
    for(unsigned t=5050;t<5300;t+=50) {
        observe(&s,t,0,0,1000,0); assert(s.inhibited);
    }
    observe(&s,5300,0,0,1000,0); assert(!s.inhibited);
    s=(nuwax_shake_motion_t){.inhibited=true,.triggered_ms=0,
        .initialized=true,.sample_seen=true,.last_sample_ms=5950,
        .gravity={0,0,1000},.quiet_seen=true,.quiet_started_ms=4900,.quiet_valid_ms=250};
    observe(&s,6000,0,0,1000,0);
    assert(s.inhibited && s.quiet_valid_ms==0); /* Do not sum sparse evidence forever. */
    s=(nuwax_shake_motion_t){.inhibited=true,.triggered_ms=0};
    for(unsigned t=0;t<=12000;t+=50) {
        if((t/50)%5==0) { nuwax_shake_motion_skip(&s); continue; }
        observe(&s,t,(t/50)%5==2?1800:0,0,1000,0);
    }
    assert(s.inhibited); /* Repeated real movement clears quiet evidence. */
    for(unsigned kind=0;kind<3;kind++) for(unsigned skip=0;skip<5;skip++) {
        s=(nuwax_shake_motion_t){0};
        for(unsigned t=0;t<=10000;t+=50) {
            if((t/50)%5==skip) { nuwax_shake_motion_skip(&s); continue; }
            float x=kind==0?200*sinf(6.2831853f*2*t/1000.0f):
                kind==1?800*sinf(6.2831853f*0.4f*t/1000.0f):0;
            assert(!observe(&s,t,x,0,1000,kind==2?160:0));
        }
    }
    puts("PASS: 60 periodic-gap gestures, quiet re-arm with busy gaps, real faults and motion retain inhibition");
    return 0;
}
