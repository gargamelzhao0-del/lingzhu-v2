#include "offline_audio_voice.h"

#include <string.h>
#include "audio_frame.h"
#include "esp_heap_caps.h"

#define VOICE_PREROLL_MS 500U
#define VOICE_CONFIRM_MS 60U
#define VOICE_TAIL_MS 1000U

esp_err_t nuwax_offline_voice_init(nuwax_offline_voice_t *v,
                                  const char *session_id,
                                  uint32_t sample_rate, uint16_t frame_ms)
{
    const nuwax_offline_voice_policy_t policy = {
        .silence_ms = VOICE_TAIL_MS,
        .resume_confirm_frames = VOICE_CONFIRM_MS / 20U,
        .activity_confirm_frames = 1U,
        .start_recording = false,
    };
    return nuwax_offline_voice_init_with_policy(v, session_id, sample_rate,
                                                frame_ms, &policy);
}

esp_err_t nuwax_offline_voice_init_with_policy(nuwax_offline_voice_t *v,
                                  const char *session_id,
                                  uint32_t sample_rate, uint16_t frame_ms,
                                  const nuwax_offline_voice_policy_t *policy)
{
    if (!v || !session_id || strlen(session_id) >= sizeof(v->session_id) ||
        sample_rate != 16000U || frame_ms != 20U || !policy ||
        !policy->silence_ms || !policy->resume_confirm_frames ||
        !policy->activity_confirm_frames) return ESP_ERR_INVALID_ARG;
    memset(v, 0, sizeof(*v));
    v->policy = *policy;
    v->silence_limit_frames = (uint32_t)(((uint64_t)policy->silence_ms +
                                        frame_ms - 1U) / frame_ms);
    v->recording = policy->start_recording;
    v->frame_samples = sample_rate * frame_ms / 1000U;
    v->capacity = VOICE_PREROLL_MS / frame_ms;
    v->sample_rate = sample_rate;
    v->frame_ms = frame_ms;
    memcpy(v->session_id, session_id, strlen(session_id) + 1U);
    v->pcm = heap_caps_calloc(v->capacity * v->frame_samples, sizeof(int16_t),
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    v->timestamps = heap_caps_calloc(v->capacity, sizeof(uint32_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    v->sequences = heap_caps_calloc(v->capacity, sizeof(uint32_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    v->speech = heap_caps_calloc(v->capacity, sizeof(uint8_t),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!v->pcm || !v->timestamps || !v->sequences || !v->speech) {
        nuwax_offline_voice_deinit(v, false);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void append(nuwax_offline_voice_t *v, const int16_t *samples,
                   bool speech, uint32_t sequence, uint32_t timestamp)
{
    const nuwax_offline_audio_frame_t frame = {
        .sequence = sequence, .timestamp_ms = timestamp,
        .flags = speech ? NUWAX_AUDIO_FLAG_VAD_SPEECH : 0U,
        .samples = samples, .sample_count = v->frame_samples,
    };
    (void)nuwax_offline_audio_mirror_append(&v->mirror, &frame, timestamp);
}

void nuwax_offline_voice_process(nuwax_offline_voice_t *v,
                                 const int16_t *samples, size_t count,
                                 bool speech, uint32_t sequence, uint32_t timestamp)
{
    if (!v || !v->pcm || !samples || count != v->frame_samples) return;
    v->speech_frames = speech ? v->speech_frames + (v->speech_frames < UINT32_MAX) : 0U;
    /* Brief candidates do not restart the 120-second inactivity timer. The
     * counter must not retain the legacy 1000-frame (20-second) saturation. */
    const bool activity = v->speech_frames >= v->policy.activity_confirm_frames;
    v->quiet_frames = activity ? 0U : v->quiet_frames + (v->quiet_frames < UINT32_MAX);
    if (v->speech_frames >= v->policy.resume_confirm_frames) v->recording = true;
    if (!v->mirror.started) {
        const size_t i = v->cursor;
        memcpy(v->pcm + i * count, samples, count * sizeof(*samples));
        v->timestamps[i] = timestamp;
        v->sequences[i] = sequence;
        v->speech[i] = speech;
        v->cursor = (i + 1U) % v->capacity;
        if (v->count < v->capacity) ++v->count;
        if (v->recording && nuwax_offline_audio_mirror_service(
                &v->mirror, NUWAX_OFFLINE_AUDIO_MODE_MONITORING, v->session_id,
                v->sample_rate, 1U, v->frame_ms, timestamp) == ESP_OK) {
            const size_t first = (v->cursor + v->capacity - v->count) % v->capacity;
            for (size_t n = 0; n < v->count; ++n) {
                const size_t j = (first + n) % v->capacity;
                append(v, v->pcm + j * count, v->speech[j] != 0U,
                       v->sequences[j], v->timestamps[j]);
            }
            v->count = 0U;
        }
    } else {
        append(v, samples, speech, sequence, timestamp);
    }
    if (v->recording && v->quiet_frames >= v->silence_limit_frames) {
        (void)nuwax_offline_audio_mirror_request_close(&v->mirror, true, timestamp);
        v->recording = false;
        v->count = 0U;
    }
}

void nuwax_offline_voice_reset(nuwax_offline_voice_t *v)
{
    if (!v) return;
    const bool keep_recording = v->policy.start_recording && v->recording;
    (void)nuwax_offline_audio_mirror_request_close(&v->mirror, false, 0U);
    /* A MIC discontinuity invalidates its old tail, not the user's active
     * monitoring intent. A paused recorder remains paused. */
    v->recording = keep_recording;
    v->speech_frames = v->quiet_frames = 0U;
    v->count = v->cursor = 0U;
}

void nuwax_offline_voice_deinit(nuwax_offline_voice_t *v, bool complete)
{
    if (!v) return;
    (void)nuwax_offline_audio_mirror_request_close(&v->mirror, complete, 0U);
    heap_caps_free(v->pcm);
    heap_caps_free(v->timestamps);
    heap_caps_free(v->sequences);
    heap_caps_free(v->speech);
    memset(v, 0, sizeof(*v));
}
