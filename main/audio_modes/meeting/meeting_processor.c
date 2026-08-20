#include "meeting_processor.h"

#include <limits.h>
#include <string.h>

static uint32_t abs_i16(int16_t value) {
    int32_t v = value;
    return (uint32_t)(v < 0 ? -v : v);
}

static int16_t clamp_i32(int32_t value) {
    if (value > INT16_MAX) {
        return INT16_MAX;
    }
    if (value < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)value;
}

static uint32_t max_u32(uint32_t a, uint32_t b) {
    return a > b ? a : b;
}

static uint32_t saturating_add_u32(uint32_t value, uint32_t increment) {
    return increment > UINT32_MAX - value ? UINT32_MAX : value + increment;
}

void nuwax_meeting_inactivity_reset(nuwax_meeting_inactivity_tracker_t *tracker) {
    if (tracker) {
        memset(tracker, 0, sizeof(*tracker));
    }
}

bool nuwax_meeting_inactivity_update(nuwax_meeting_inactivity_tracker_t *tracker,
                                     const nuwax_meeting_frame_features_t *features,
                                     uint32_t frame_duration_ms,
                                     uint32_t inactive_timeout_ms) {
    if (!tracker || !features || frame_duration_ms == 0U) {
        return false;
    }
    if (!features->capture_valid) {
        tracker->speech_activity_ms = 0U;
        tracker->sound_activity_ms = 0U;
        return false;
    }

    tracker->speech_activity_ms = features->speech
                                      ? saturating_add_u32(tracker->speech_activity_ms,
                                                           frame_duration_ms)
                                      : 0U;
    tracker->sound_activity_ms = !features->environment_quiet
                                     ? saturating_add_u32(tracker->sound_activity_ms,
                                                          frame_duration_ms)
                                     : 0U;

    if (features->speech) {
        tracker->inactive_ms = 0U;
    } else {
        tracker->inactive_ms = saturating_add_u32(tracker->inactive_ms,
                                                  frame_duration_ms);
    }
    return inactive_timeout_ms > 0U && tracker->inactive_ms >= inactive_timeout_ms;
}

void nuwax_meeting_processor_default_config(nuwax_meeting_processor_config_t *config) {
    if (!config) {
        return;
    }
    *config = (nuwax_meeting_processor_config_t){
        .speech_min_avg_abs = 280U,
        .speech_min_peak = 900U,
        .quiet_margin_abs = 110U,
        .quiet_peak = 520U,
        .voice_target_avg_abs = 1500U,
        .voice_gain_q15 = 34816U,     /* +0.54 dB initial gain step */
        .voice_max_gain_q15 = 39322U, /* +1.58 dB hard ceiling */
    };
}

void nuwax_meeting_processor_init(nuwax_meeting_processor_t *processor,
                                  const nuwax_meeting_processor_config_t *config) {
    if (!processor) {
        return;
    }
    memset(processor, 0, sizeof(*processor));
    if (config) {
        processor->config = *config;
    } else {
        nuwax_meeting_processor_default_config(&processor->config);
    }
    processor->gain_q15 = 32768U;
    processor->initialized = true;
}

void nuwax_meeting_processor_reset(nuwax_meeting_processor_t *processor) {
    if (!processor) {
        return;
    }
    const nuwax_meeting_processor_config_t config = processor->config;
    nuwax_meeting_processor_init(processor, &config);
}

void nuwax_meeting_processor_process(nuwax_meeting_processor_t *processor,
                                     int16_t *samples,
                                     size_t sample_count,
                                     bool capture_valid,
                                     nuwax_meeting_frame_features_t *features) {
    if (!processor || !samples || sample_count == 0 || !features) {
        if (features) {
            memset(features, 0, sizeof(*features));
            features->capture_valid = capture_valid;
        }
        return;
    }

    memset(features, 0, sizeof(*features));
    features->capture_valid = capture_valid;

    uint64_t sum_abs = 0;
    uint32_t peak = 0;
    int64_t sum = 0;
    for (size_t i = 0; i < sample_count; i++) {
        sum += samples[i];
        const uint32_t abs_value = abs_i16(samples[i]);
        sum_abs += abs_value;
        if (abs_value > peak) {
            peak = abs_value;
        }
    }
    const int32_t mean = (int32_t)(sum / (int64_t)sample_count);
    const uint32_t avg_abs = (uint32_t)(sum_abs / sample_count);
    features->avg_abs = avg_abs;
    features->peak = peak;

    /* A slow DC tracker removes bias without introducing a frame boundary jump. */
    for (size_t i = 0; i < sample_count; i++) {
        const int32_t input = samples[i];
        const int32_t input_q15 = input * 32768;
        processor->dc_estimate_q15 += (input_q15 - processor->dc_estimate_q15) >> 8;
        const int32_t centered = input - (processor->dc_estimate_q15 >> 15);
        samples[i] = clamp_i32(centered);
    }

    if (capture_valid) {
        if (!processor->noise_floor_initialized) {
            /* Do not seed the floor with an entire first speech burst.  A bounded
             * seed keeps the first syllable eligible for speech detection while
             * still allowing a genuinely quiet first frame to establish zero. */
            const uint32_t seed_limit = processor->config.speech_min_avg_abs / 2U;
            processor->noise_floor_abs = avg_abs < seed_limit ? avg_abs : seed_limit;
            processor->noise_floor_initialized = true;
        } else if (avg_abs < processor->noise_floor_abs) {
            /* Fast enough to follow a newly quiet room, still smoothed over frames. */
            processor->noise_floor_abs =
                (processor->noise_floor_abs * 15U + avg_abs) / 16U;
        } else {
            /* Rise slowly so a speech burst does not redefine the noise floor. */
            processor->noise_floor_abs =
                (processor->noise_floor_abs * 63U + avg_abs) / 64U;
        }
    }
    features->noise_floor_abs = processor->noise_floor_abs;

    const uint32_t adaptive_threshold =
        max_u32(processor->config.speech_min_avg_abs,
                processor->noise_floor_abs + processor->config.quiet_margin_abs + 40U);
    const bool speech_energy = avg_abs >= adaptive_threshold &&
                               peak >= processor->config.speech_min_peak;
    const bool strong_peak = peak >= processor->config.speech_min_peak * 2U;
    const bool speech = speech_energy || (strong_peak && avg_abs >= processor->config.speech_min_avg_abs / 2U);
    if (speech) {
        processor->speech_hangover_frames = 8U;
    } else if (processor->speech_hangover_frames > 0U) {
        processor->speech_hangover_frames--;
    }
    features->speech = speech || processor->speech_hangover_frames > 0U;

    const uint32_t quiet_limit = processor->noise_floor_abs + processor->config.quiet_margin_abs;
    features->environment_quiet = capture_valid && !features->speech &&
                                   avg_abs <= quiet_limit && peak <= processor->config.quiet_peak;

    /* Apply only a bounded, slow voice lift.  Silence and noise are untouched. */
    uint32_t target_gain = 32768U;
    if (features->speech && avg_abs < processor->config.voice_target_avg_abs) {
        target_gain = processor->config.voice_gain_q15;
        if (target_gain > processor->config.voice_max_gain_q15) {
            target_gain = processor->config.voice_max_gain_q15;
        }
    }
    if (target_gain > processor->gain_q15) {
        processor->gain_q15 = (processor->gain_q15 * 7U + target_gain) / 8U;
    } else {
        processor->gain_q15 = (processor->gain_q15 * 31U + target_gain) / 32U;
    }
    if (processor->gain_q15 > 32768U) {
        features->gain_applied = true;
        for (size_t i = 0; i < sample_count; i++) {
            const int32_t amplified = (int32_t)(((int64_t)samples[i] * processor->gain_q15) >> 15);
            samples[i] = clamp_i32(amplified);
        }
    }

    uint32_t confidence = 0U;
    if (adaptive_threshold > 0U && avg_abs > 0U) {
        const uint64_t ratio = ((uint64_t)avg_abs * 32768ULL) / adaptive_threshold;
        confidence = ratio > 32768ULL ? 32768U : (uint32_t)ratio;
    }
    if (!features->speech) {
        confidence = confidence / 2U;
    }
    features->speech_confidence_q15 = (uint16_t)confidence;
    (void)mean; /* Kept for diagnostics/debugger inspection without changing the path. */
}
