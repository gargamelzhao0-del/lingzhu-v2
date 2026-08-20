#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The meeting processor deliberately stays conservative.  It never drops or
 * zeros a frame; the classifier result is metadata used by the inactivity
 * tracker and by the uplink frame flags.
 */
typedef struct {
    uint32_t speech_min_avg_abs;
    uint32_t speech_min_peak;
    uint32_t quiet_margin_abs;
    uint32_t quiet_peak;
    uint32_t voice_target_avg_abs;
    uint16_t voice_gain_q15;
    uint16_t voice_max_gain_q15;
} nuwax_meeting_processor_config_t;

typedef struct {
    uint32_t avg_abs;
    uint32_t peak;
    uint32_t noise_floor_abs;
    uint16_t speech_confidence_q15;
    bool speech;
    bool environment_quiet;
    bool capture_valid;
    bool gain_applied;
} nuwax_meeting_frame_features_t;

typedef struct {
    uint32_t inactive_ms;
    uint32_t speech_activity_ms;
    uint32_t sound_activity_ms;
} nuwax_meeting_inactivity_tracker_t;

typedef struct {
    nuwax_meeting_processor_config_t config;
    int32_t dc_estimate_q15;
    uint32_t noise_floor_abs;
    bool noise_floor_initialized;
    uint32_t speech_hangover_frames;
    uint32_t gain_q15;
    bool initialized;
} nuwax_meeting_processor_t;

void nuwax_meeting_processor_default_config(nuwax_meeting_processor_config_t *config);
void nuwax_meeting_processor_init(nuwax_meeting_processor_t *processor,
                                  const nuwax_meeting_processor_config_t *config);
void nuwax_meeting_processor_reset(nuwax_meeting_processor_t *processor);

void nuwax_meeting_inactivity_reset(nuwax_meeting_inactivity_tracker_t *tracker);

/*
 * Advance the continuous no-speech timer. A frame classified as speech resets
 * the timer; every other valid frame advances it, including background sound.
 * Invalid capture frames neither advance nor reset inactivity.
 */
bool nuwax_meeting_inactivity_update(nuwax_meeting_inactivity_tracker_t *tracker,
                                     const nuwax_meeting_frame_features_t *features,
                                     uint32_t frame_duration_ms,
                                     uint32_t inactive_timeout_ms);

/* Process one PCM16 frame in place and return classification metadata. */
void nuwax_meeting_processor_process(nuwax_meeting_processor_t *processor,
                                     int16_t *samples,
                                     size_t sample_count,
                                     bool capture_valid,
                                     nuwax_meeting_frame_features_t *features);
