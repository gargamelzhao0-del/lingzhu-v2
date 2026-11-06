#pragma once

#include "offline_audio_spool.h"

typedef struct {
    uint32_t silence_ms;
    uint32_t resume_confirm_frames;
    uint32_t activity_confirm_frames;
    bool start_recording;
} nuwax_offline_voice_policy_t;

/* One owner, one physical PCM frame per call. Cloud pause/resume has no
 * influence on this recorder. Ring entries retain original timestamps. */
typedef struct {
    nuwax_offline_audio_mirror_t mirror;
    int16_t *pcm;
    uint32_t *timestamps;
    uint32_t *sequences;
    uint8_t *speech;
    size_t frame_samples;
    size_t capacity;
    size_t count;
    size_t cursor;
    uint32_t speech_frames;
    uint32_t quiet_frames;
    uint32_t silence_limit_frames;
    nuwax_offline_voice_policy_t policy;
    uint32_t sample_rate;
    uint16_t frame_ms;
    bool recording;
    char session_id[48];
} nuwax_offline_voice_t;

esp_err_t nuwax_offline_voice_init(nuwax_offline_voice_t *voice,
                                  const char *session_id,
                                  uint32_t sample_rate, uint16_t frame_ms);
/* SD monitoring starts immediately and pauses only after sustained silence.
 * The default initializer retains the legacy utterance recorder policy. */
esp_err_t nuwax_offline_voice_init_with_policy(nuwax_offline_voice_t *voice,
                                  const char *session_id,
                                  uint32_t sample_rate, uint16_t frame_ms,
                                  const nuwax_offline_voice_policy_t *policy);
void nuwax_offline_voice_process(nuwax_offline_voice_t *voice,
                                 const int16_t *samples, size_t count,
                                 bool speech, uint32_t sequence, uint32_t timestamp);
void nuwax_offline_voice_reset(nuwax_offline_voice_t *voice);
void nuwax_offline_voice_deinit(nuwax_offline_voice_t *voice, bool complete);
