#include <assert.h>
#include <stdio.h>

#include "conversation/speaker_jitter_policy.h"

int main(void) {
    assert(nuwax_speaker_jitter_audio_ms(320U, 16000U) == 20U);
    assert(nuwax_speaker_jitter_audio_ms(7680U, 16000U) == 480U);
    assert(nuwax_speaker_jitter_audio_ms(160U, 8000U) == 20U);
    assert(nuwax_speaker_jitter_audio_ms(960U, 48000U) == 20U);
    assert(nuwax_speaker_jitter_audio_ms(320U, 0U) == 0U);

    assert(nuwax_speaker_stall_action(11999U, 12000U, 60000U, false) ==
           NUWAX_SPEAKER_STALL_WAIT);
    assert(nuwax_speaker_stall_action(12000U, 12000U, 60000U, false) ==
           NUWAX_SPEAKER_STALL_WARN);
    assert(nuwax_speaker_stall_action(22700U, 12000U, 60000U, true) ==
           NUWAX_SPEAKER_STALL_WAIT);
    assert(nuwax_speaker_stall_action(59999U, 12000U, 60000U, true) ==
           NUWAX_SPEAKER_STALL_WAIT);
    assert(nuwax_speaker_stall_action(60000U, 12000U, 60000U, true) ==
           NUWAX_SPEAKER_STALL_ABORT);

    assert(!nuwax_speaker_jitter_ready_ms(
        false, false, 23U, 7360U, 16000U, 480U, 240U));
    assert(nuwax_speaker_jitter_ready_ms(
        false, false, 24U, 7680U, 16000U, 480U, 240U));
    assert(nuwax_speaker_jitter_ready_ms(
        false, false, 8U, 7680U, 16000U, 480U, 240U));
    assert(nuwax_speaker_jitter_ready_ms(
        false, false, 48U, 7680U, 16000U, 480U, 240U));
    assert(!nuwax_speaker_jitter_ready_ms(
        true, false, 11U, 3520U, 16000U, 480U, 240U));
    assert(nuwax_speaker_jitter_ready_ms(
        true, false, 12U, 3840U, 16000U, 480U, 240U));

    assert(!nuwax_speaker_jitter_ready_ms(
        false, true, 0U, 0U, 16000U, 480U, 240U));
    assert(nuwax_speaker_jitter_ready_ms(
        false, true, 1U, 320U, 16000U, 480U, 240U));
    assert(nuwax_speaker_jitter_ready_ms(
        true, true, 1U, 0U, 16000U, 480U, 240U));

    assert(!nuwax_speaker_jitter_force_resume(
        true, true, 1999U, 1U, 320U, 2000U));
    assert(nuwax_speaker_jitter_force_resume(
        true, true, 2000U, 1U, 320U, 2000U));
    assert(!nuwax_speaker_jitter_force_resume(
        true, true, 2500U, 0U, 0U, 2000U));
    assert(!nuwax_speaker_jitter_force_resume(
        false, true, 2500U, 1U, 320U, 2000U));

    assert(!nuwax_speaker_jitter_should_enter_rebuffer(
        true, false, 0U, 79U, 80U));
    assert(nuwax_speaker_jitter_should_enter_rebuffer(
        true, false, 0U, 80U, 80U));
    assert(!nuwax_speaker_jitter_should_enter_rebuffer(
        true, true, 0U, 80U, 80U));
    assert(!nuwax_speaker_jitter_should_enter_rebuffer(
        true, false, 1U, 80U, 80U));
    assert(!nuwax_speaker_jitter_should_enter_rebuffer(
        false, false, 0U, 80U, 80U));
    assert(nuwax_speaker_jitter_rebuffer_is_midstream(100U, 101U));
    assert(!nuwax_speaker_jitter_rebuffer_is_midstream(100U, 100U));

    assert(nuwax_speaker_jitter_resume_target_ms(0U, 800U, 1200U) ==
           800U);
    assert(nuwax_speaker_jitter_resume_target_ms(1U, 800U, 1200U) ==
           960U);
    assert(nuwax_speaker_jitter_resume_target_ms(2U, 800U, 1200U) ==
           1120U);
    assert(nuwax_speaker_jitter_resume_target_ms(3U, 800U, 1200U) ==
           1200U);
    assert(nuwax_speaker_jitter_resume_target_ms(20U, 800U, 960U) ==
           960U);
    assert(nuwax_speaker_jitter_resume_target_with_gap(
               1U, 800U, 1200U, 0U, 240U) == 960U);
    assert(nuwax_speaker_jitter_resume_target_with_gap(
               0U, 800U, 1200U, 799U, 240U) == 1039U);
    assert(nuwax_speaker_jitter_resume_target_with_gap(
               1U, 800U, 1200U, 799U, 240U) == 1039U);
    assert(nuwax_speaker_jitter_resume_target_with_gap(
               2U, 800U, 1200U, 2000U, 240U) == 1200U);

    uint8_t history = 0U;
    uint8_t stable = 0U;
    uint32_t startup = 960U;
    startup = nuwax_speaker_jitter_adapt_startup_ms(
        startup, true, 800U, 800U, 960U, 1600U, &history, &stable);
    assert(startup == 1120U);
    startup = nuwax_speaker_jitter_adapt_startup_ms(
        startup, false, 0U, 800U, 960U, 1600U, &history, &stable);
    startup = nuwax_speaker_jitter_adapt_startup_ms(
        startup, true, 800U, 800U, 960U, 1600U, &history, &stable);
    assert(startup == 1440U);
    for (int i = 0; i < 2; ++i) {
        startup = nuwax_speaker_jitter_adapt_startup_ms(
            startup, false, 0U, 800U, 960U, 1600U, &history, &stable);
    }
    assert(startup == 1280U);

    history = 0U;
    stable = 0U;
    startup = 960U;
    startup = nuwax_speaker_jitter_adapt_startup_ms(
        startup, true, 5000U, 800U, 960U, 1600U, &history, &stable);
    assert(startup == 1120U);

    /* A short recovered outage must not raise the next startup watermark. */
    history = 0U;
    stable = 0U;
    startup = 960U;
    startup = nuwax_speaker_jitter_adapt_startup_ms(
        startup, true, 706U, 800U, 960U, 1600U, &history, &stable);
    assert(startup == 960U);

    /* Production recovery is intentionally lower than the generic vectors
     * above: it reduces audible silence while retaining a substantial buffer
     * over the measured ingress gap. */
    assert(nuwax_speaker_jitter_resume_target_ms(0U, 640U, 960U) == 640U);
    assert(nuwax_speaker_jitter_resume_target_ms(1U, 640U, 960U) == 800U);
    assert(nuwax_speaker_jitter_resume_target_ms(2U, 640U, 960U) == 960U);

    /* A low queue or END-only tail wait is diagnostic. Without a confirmed
     * midstream rebuffer it must not make the next response start later. */
    history = 0U;
    stable = 0U;
    startup = 800U;
    startup = nuwax_speaker_jitter_adapt_startup_ms(
        startup, false, 5000U, 800U, 960U, 1600U, &history, &stable);
    assert(startup == 800U);

    history = 0U;
    stable = 0U;
    startup = 960U;
    for (int i = 0; i < 5; ++i) {
        startup = nuwax_speaker_jitter_adapt_startup_ms(
            startup, false, 0U, 800U, 960U, 1600U, &history, &stable);
    }
    assert(startup == 800U);

    history = 0U;
    stable = 0U;
    startup = 800U;
    for (int i = 0; i < 5; ++i) {
        startup = nuwax_speaker_jitter_adapt_startup_ms(
            startup, false, 0U, 480U, 800U, 1600U, &history, &stable);
    }
    assert(startup == 480U);
    startup = nuwax_speaker_jitter_adapt_startup_ms(
        startup, true, 1000U, 480U, 800U, 1600U, &history, &stable);
    assert(startup == 800U);

    /* Exact field sequence: four clean completions, then two confirmed
     * midstream rebuffers whose maximum wait is 684 ms. No downshift. */
    history = stable = 0U;
    startup = 800U;
    for (int i = 0; i < 4; ++i)
        startup = nuwax_speaker_jitter_adapt_startup_ms(
            startup, false, 0, 480, 800, 1600, &history, &stable);
    assert(stable == 4 && startup == 800);
    startup = nuwax_speaker_jitter_adapt_startup_ms(
        startup, true, 684, 480, 800, 1600, &history, &stable);
    assert(startup == 800 && stable == 0);
    for (int i = 0; i < 4; ++i) {
        startup = nuwax_speaker_jitter_adapt_startup_ms(
            startup, false, 0, 480, 800, 1600, &history, &stable);
        assert(startup == 800);
    }
    startup = nuwax_speaker_jitter_adapt_startup_ms(
        startup, false, 0, 480, 800, 1600, &history, &stable);
    assert(startup == 480);

    /* Every short-wait boundary breaks the clean streak at every valid tier;
     * a short outage never inflates startup latency or trains a downshift. */
    const unsigned tiers[] = {480, 800, 960, 1120, 1440, 1600};
    for (unsigned t = 0; t < sizeof(tiers)/sizeof(tiers[0]); ++t) {
        for (unsigned wait = 0; wait < 800; ++wait) {
            history = 5;
            stable = 255;
            startup = nuwax_speaker_jitter_adapt_startup_ms(
                tiers[t], true, wait, 480, 800, 1600, &history, &stable);
            assert(startup == tiers[t] && stable == 0 && history == 2);
        }
    }
    /* Above-base decay also requires clean completions after the outage. */
    history = 0; stable = 1;
    startup = nuwax_speaker_jitter_adapt_startup_ms(
        1120, true, 799, 480, 800, 1600, &history, &stable);
    assert(startup == 1120 && stable == 0);
    startup = nuwax_speaker_jitter_adapt_startup_ms(
        startup, false, 0, 480, 800, 1600, &history, &stable);
    assert(startup == 1120 && stable == 1);
    startup = nuwax_speaker_jitter_adapt_startup_ms(
        startup, false, 0, 480, 800, 1600, &history, &stable);
    assert(startup == 960 && stable == 0);
    /* Material-outage promotion and upper bound remain unchanged. */
    history = 0; stable = 4;
    startup = nuwax_speaker_jitter_adapt_startup_ms(
        800, true, 800, 480, 800, 1600, &history, &stable);
    assert(startup == 960 && stable == 0);
    startup = nuwax_speaker_jitter_adapt_startup_ms(
        1600, true, 5000, 480, 800, 1600, &history, &stable);
    assert(startup == 1600 && stable == 0);
    /* END-only tail wait is not a confirmed midstream rebuffer. */
    history = 0; stable = 4;
    startup = nuwax_speaker_jitter_adapt_startup_ms(
        800, false, 5000, 480, 800, 1600, &history, &stable);
    assert(startup == 480 && stable == 0);
    puts("speaker jitter policy host check: PASS");
    return 0;
}
