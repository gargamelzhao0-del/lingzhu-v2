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
        startup, true, 0U, 800U, 960U, 1600U, &history, &stable);
    assert(startup == 1120U);
    startup = nuwax_speaker_jitter_adapt_startup_ms(
        startup, false, 0U, 800U, 960U, 1600U, &history, &stable);
    startup = nuwax_speaker_jitter_adapt_startup_ms(
        startup, true, 0U, 800U, 960U, 1600U, &history, &stable);
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

    puts("speaker jitter policy host check: PASS");
    return 0;
}
