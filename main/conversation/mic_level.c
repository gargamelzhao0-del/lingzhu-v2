#include "mic_level.h"

#include "conversation/realtime_log.h"
#include "esp_log.h"
#include "sdkconfig.h"

nuwax_mic_level_t nuwax_mic_level_measure(const int16_t *samples, size_t sample_count) {
    nuwax_mic_level_t level = {0};
    uint32_t peak = 0;
    uint32_t clipped = 0;
    uint64_t sum_abs = 0;
    for (size_t i = 0; i < sample_count; i++) {
        int32_t sample = samples[i];
        uint32_t abs_sample = (uint32_t)(sample < 0 ? -sample : sample);
        if (abs_sample > peak) {
            peak = abs_sample;
        }
        if (sample >= 32760 || sample <= -32760) {
            clipped++;
        }
        sum_abs += abs_sample;
    }
    level.avg_abs = sample_count ? (uint32_t)(sum_abs / sample_count) : 0;
    level.peak = peak;
    level.clipped = clipped;
    return level;
}

void nuwax_mic_level_log(const char *tag, nuwax_mic_level_t level, uint32_t seq) {
    ESP_LOGI(tag ? tag : "nuwax_mic_level",
             "mic level seq=%lu avg_abs=%lu peak=%lu clipped=%lu",
             (unsigned long)seq,
             (unsigned long)level.avg_abs,
             (unsigned long)level.peak,
             (unsigned long)level.clipped);
}

bool nuwax_mic_level_should_log(uint32_t seq, bool important) {
    if (important) {
        return true;
    }
#if CONFIG_NUWAX_REALTIME_AUDIO_DIAGNOSTIC_LOG_ENABLE
    if (CONFIG_NUWAX_MIC_LEVEL_LOG_INTERVAL_FRAMES <= 0) {
        return false;
    }
    return seq == 0 || ((seq + 1) % (uint32_t)CONFIG_NUWAX_MIC_LEVEL_LOG_INTERVAL_FRAMES) == 0;
#else
    (void)seq;
    return false;
#endif
}

bool nuwax_mic_level_is_speech(nuwax_mic_level_t level) {
#if CONFIG_NUWAX_VOICE_LOCAL_VAD_ENDPOINT_ENABLE
    const uint32_t medium_avg = (uint32_t)CONFIG_NUWAX_VOICE_LOCAL_VAD_SPEECH_AVG_ABS;
    const uint32_t strong_avg = medium_avg + (medium_avg / 2u);
    const uint32_t peak = (uint32_t)CONFIG_NUWAX_VOICE_LOCAL_VAD_SPEECH_PEAK;
    return level.avg_abs >= strong_avg ||
           (level.avg_abs >= medium_avg && level.peak >= peak);
#else
    (void)level;
    return true;
#endif
}
