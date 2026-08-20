/* Host-only sanity check.  This file is intentionally not part of the ESP-IDF
 * component; it exercises the meeting-mode policy without hardware or a
 * WebSocket connection. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "meeting_processor.h"

static void fill_constant(int16_t *samples, size_t count, int16_t value) {
    for (size_t i = 0; i < count; ++i) {
        samples[i] = value;
    }
}

static void fill_alternating(int16_t *samples, size_t count, int16_t amplitude) {
    for (size_t i = 0; i < count; ++i) {
        samples[i] = (i & 1U) == 0U ? amplitude : (int16_t)-amplitude;
    }
}

static uint64_t sum_abs_samples(const int16_t *samples, size_t count) {
    uint64_t sum = 0U;
    for (size_t i = 0; i < count; ++i) {
        const int32_t value = samples[i];
        sum += (uint32_t)(value < 0 ? -value : value);
    }
    return sum;
}

int main(void) {
    nuwax_meeting_processor_t processor;
    nuwax_meeting_processor_config_t config;
    nuwax_meeting_processor_default_config(&config);
    nuwax_meeting_processor_init(&processor, &config);

    int16_t samples[320];
    nuwax_meeting_frame_features_t features;

    /* Quiet frames remain present and are classified only as metadata. */
    fill_constant(samples, 320, 0);
    nuwax_meeting_processor_process(&processor, samples, 320, true, &features);
    assert(features.capture_valid);
    assert(features.environment_quiet);
    for (size_t i = 0; i < 320; ++i) {
        assert(samples[i] == 0);
    }

    /* A weak voice is retained and may receive only the bounded lift. */
    fill_alternating(samples, 320, 1000);
    const uint64_t weak_voice_energy_before = sum_abs_samples(samples, 320);
    nuwax_meeting_processor_process(&processor, samples, 320, true, &features);
    assert(features.speech);
    assert(features.gain_applied);
    assert(processor.gain_q15 <= config.voice_max_gain_q15);
    assert(sum_abs_samples(samples, 320) > weak_voice_energy_before);

    /* The simple filter rejects DC bias; it is not a broadband denoiser. */
    nuwax_meeting_processor_reset(&processor);
    for (unsigned int frame = 0; frame < 20U; ++frame) {
        fill_constant(samples, 320, 300);
        nuwax_meeting_processor_process(&processor, samples, 320, true, &features);
    }
    assert(samples[319] > -10 && samples[319] < 10);

    /* Invalid reads must never feed the no-speech auto-stop timer. */
    fill_constant(samples, 320, 0);
    nuwax_meeting_processor_process(&processor, samples, 320, false, &features);
    assert(!features.capture_valid);
    assert(!features.environment_quiet);

    nuwax_meeting_inactivity_tracker_t inactivity;
    nuwax_meeting_inactivity_reset(&inactivity);

    const nuwax_meeting_frame_features_t quiet = {
        .capture_valid = true,
        .environment_quiet = true,
    };
    for (unsigned int i = 0; i < 9U; ++i) {
        assert(!nuwax_meeting_inactivity_update(&inactivity,
                                                &quiet,
                                                16U,
                                                160U));
    }
    assert(nuwax_meeting_inactivity_update(&inactivity,
                                           &quiet,
                                           16U,
                                           160U));
    assert(inactivity.inactive_ms == 160U);

    const nuwax_meeting_frame_features_t speech_activity = {
        .speech = true,
        .capture_valid = true,
    };
    const uint32_t no_speech_timeout_ms = 30U * 60U * 1000U;
    inactivity.inactive_ms = no_speech_timeout_ms - 16U;
    assert(!nuwax_meeting_inactivity_update(&inactivity,
                                            &speech_activity,
                                            16U,
                                            no_speech_timeout_ms));
    assert(inactivity.inactive_ms == 0U);
    assert(inactivity.speech_activity_ms == 16U);

    nuwax_meeting_inactivity_reset(&inactivity);
    const nuwax_meeting_frame_features_t background_sound = {
        .capture_valid = true,
    };
    inactivity.inactive_ms = no_speech_timeout_ms - 16U;
    assert(nuwax_meeting_inactivity_update(&inactivity,
                                           &background_sound,
                                           16U,
                                           no_speech_timeout_ms));
    assert(inactivity.inactive_ms == no_speech_timeout_ms);
    assert(inactivity.sound_activity_ms == 16U);

    const nuwax_meeting_frame_features_t invalid = {0};
    inactivity.inactive_ms = 1234U;
    (void)nuwax_meeting_inactivity_update(&inactivity,
                                          &invalid,
                                          16U,
                                          no_speech_timeout_ms);
    assert(inactivity.inactive_ms == 1234U);
    assert(inactivity.speech_activity_ms == 0U);
    assert(inactivity.sound_activity_ms == 0U);

    puts("meeting_processor_host_check: PASS");
    return 0;
}
