#include "conversation_audio_health.h"

#include <limits.h>
#include <string.h>

#define NUWAX_AUDIO_HEALTH_DEFAULT_STABLE_FRAMES 3U
#define NUWAX_AUDIO_HEALTH_DEFAULT_ANOMALY_FRAMES 3U
#define NUWAX_AUDIO_HEALTH_DEFAULT_MAXIMUM_FRAME_AGE_US 60000U

static uint16_t saturating_increment_u16(uint16_t value) {
    return value == UINT16_MAX ? value : (uint16_t)(value + 1U);
}

static bool config_valid(
    const nuwax_conversation_audio_health_config_t *config) {
    return config && config->stable_frames_for_green > 0U &&
           config->stable_frames_for_green <= 50U &&
           config->anomaly_frames_for_red > 0U &&
           config->anomaly_frames_for_red <= 50U &&
           config->maximum_frame_age_us > 0U;
}

void nuwax_conversation_audio_health_default_config(
    nuwax_conversation_audio_health_config_t *config) {
    if (!config) {
        return;
    }
    *config = (nuwax_conversation_audio_health_config_t){
        .stable_frames_for_green =
            NUWAX_AUDIO_HEALTH_DEFAULT_STABLE_FRAMES,
        .anomaly_frames_for_red =
            NUWAX_AUDIO_HEALTH_DEFAULT_ANOMALY_FRAMES,
        .maximum_frame_age_us =
            NUWAX_AUDIO_HEALTH_DEFAULT_MAXIMUM_FRAME_AGE_US,
    };
}

bool nuwax_conversation_audio_health_init(
    nuwax_conversation_audio_health_t *health,
    const nuwax_conversation_audio_health_config_t *config) {
    if (!health) {
        return false;
    }
    nuwax_conversation_audio_health_config_t selected = {0};
    if (config) {
        selected = *config;
    } else {
        nuwax_conversation_audio_health_default_config(&selected);
    }
    if (!config_valid(&selected)) {
        return false;
    }
    memset(health, 0, sizeof(*health));
    health->config = selected;
    health->initialized = true;
    return true;
}

void nuwax_conversation_audio_health_reset(
    nuwax_conversation_audio_health_t *health) {
    if (!health || !health->initialized) {
        return;
    }
    const nuwax_conversation_audio_health_config_t config = health->config;
    memset(health, 0, sizeof(*health));
    health->config = config;
    health->initialized = true;
}

static void reset_streaks(nuwax_conversation_audio_health_t *health) {
    health->aec_stable_frames = 0U;
    health->aec_anomaly_frames = 0U;
    health->reference_stable_frames = 0U;
    health->reference_anomaly_frames = 0U;
}

static nuwax_barge_health_t update_health_streak(
    bool anomaly,
    bool hard_failure,
    uint16_t *stable_frames,
    uint16_t *anomaly_frames,
    const nuwax_conversation_audio_health_config_t *config) {
    if (anomaly || hard_failure) {
        *stable_frames = 0U;
        *anomaly_frames = saturating_increment_u16(*anomaly_frames);
        if (hard_failure ||
            *anomaly_frames >= config->anomaly_frames_for_red) {
            return NUWAX_BARGE_HEALTH_RED;
        }
        return NUWAX_BARGE_HEALTH_YELLOW;
    }

    *anomaly_frames = 0U;
    *stable_frames = saturating_increment_u16(*stable_frames);
    return *stable_frames >= config->stable_frames_for_green
               ? NUWAX_BARGE_HEALTH_GREEN
               : NUWAX_BARGE_HEALTH_YELLOW;
}

static nuwax_aec_runtime_reason_bits_t classify_reasons(
    const nuwax_conversation_audio_health_t *health,
    const nuwax_conversation_audio_health_frame_t *frame) {
    nuwax_aec_runtime_reason_bits_t reasons = NUWAX_AEC_REASON_NONE;
    if (!frame->enabled) {
        reasons |= NUWAX_AEC_REASON_CONFIG_DISABLED;
    }
    if (!frame->initialized) {
        reasons |= NUWAX_AEC_REASON_NOT_PREPARED;
    }
    if (frame->playback_active && !frame->reference_generation_matches) {
        reasons |= NUWAX_AEC_REASON_REFERENCE_GENERATION_MISMATCH;
    }
    if (frame->playback_active && !frame->reference_contiguous) {
        reasons |= NUWAX_AEC_REASON_REFERENCE_GAP;
    }
    if (frame->playback_active && frame->reference_active &&
        !frame->reference_locked) {
        reasons |= NUWAX_AEC_REASON_DELAY_UNLOCKED;
    }
    if (frame->playback_active && !frame->reference_active) {
        reasons |= NUWAX_AEC_REASON_REFERENCE_INACTIVE;
    }
    if (frame->playback_active && frame->ref_missing) {
        reasons |= NUWAX_AEC_REASON_REFERENCE_MISSING;
    }
    if (frame->playback_active && !frame->aec_frame_processed) {
        reasons |= NUWAX_AEC_REASON_RAW_FALLBACK;
    }
    if (frame->processing_error) {
        reasons |= NUWAX_AEC_REASON_PROCESSING_ERROR;
    }
    if (frame->microphone_clipped) {
        reasons |= NUWAX_AEC_REASON_MIC_CLIPPING;
    }
    if (frame->reference_clipped) {
        reasons |= NUWAX_AEC_REASON_REFERENCE_CLIPPING;
    }
    if (frame->output_underrun_delta > 0U) {
        reasons |= NUWAX_AEC_REASON_OUTPUT_UNDERRUN;
    }
    if (frame->deadline_missed ||
        frame->frame_age_us > health->config.maximum_frame_age_us) {
        reasons |= NUWAX_AEC_REASON_DSP_DEADLINE_MISS;
    }
    return reasons;
}

static nuwax_aec_runtime_state_t classify_runtime_state(
    const nuwax_conversation_audio_health_frame_t *frame,
    const nuwax_conversation_audio_health_result_t *result,
    bool current_anomaly) {
    if (!frame->enabled) {
        return NUWAX_AEC_RUNTIME_DISABLED;
    }
    if (!frame->initialized) {
        return NUWAX_AEC_RUNTIME_INITIALIZING;
    }
    if (frame->processing_error) {
        return NUWAX_AEC_RUNTIME_FAILED;
    }
    if (frame->playback_active && !frame->aec_frame_processed &&
        result->aec_health == NUWAX_BARGE_HEALTH_RED) {
        return NUWAX_AEC_RUNTIME_BYPASSED;
    }
    if (current_anomaly || result->aec_health == NUWAX_BARGE_HEALTH_RED ||
        result->reference_health == NUWAX_BARGE_HEALTH_RED) {
        return NUWAX_AEC_RUNTIME_DEGRADED;
    }
    if (result->aec_health == NUWAX_BARGE_HEALTH_YELLOW ||
        result->reference_health == NUWAX_BARGE_HEALTH_YELLOW) {
        return NUWAX_AEC_RUNTIME_INITIALIZING;
    }
    return NUWAX_AEC_RUNTIME_HEALTHY;
}

bool nuwax_conversation_audio_health_process(
    nuwax_conversation_audio_health_t *health,
    const nuwax_conversation_audio_health_frame_t *frame,
    nuwax_conversation_audio_health_result_t *result) {
    if (result) {
        memset(result, 0, sizeof(*result));
    }
    if (!health || !health->initialized || !frame || !result) {
        return false;
    }

    if (frame->playback_active != health->previous_playback_active) {
        reset_streaks(health);
    }
    health->previous_playback_active = frame->playback_active;

    const bool stale_or_late =
        frame->deadline_missed ||
        frame->frame_age_us > health->config.maximum_frame_age_us;
    const bool reference_timeline_ok =
        frame->reference_generation_matches &&
        frame->reference_contiguous && !frame->ref_missing;
    const bool reference_signal_ok =
        !frame->reference_active || frame->reference_locked;
    const bool safe_silent_passthrough =
        frame->playback_active && reference_timeline_ok &&
        !frame->reference_active && !frame->aec_frame_processed &&
        !frame->processing_error && !frame->microphone_clipped &&
        !frame->reference_clipped && frame->output_underrun_delta == 0U &&
        !stale_or_late;
    result->verified_silent_passthrough = safe_silent_passthrough;
    const bool reference_anomaly =
        frame->playback_active &&
        (!reference_timeline_ok || !reference_signal_ok ||
         frame->reference_clipped || stale_or_late);
    const bool aec_anomaly =
        frame->playback_active &&
        ((!frame->aec_frame_processed && !safe_silent_passthrough) ||
         !reference_timeline_ok ||
         !reference_signal_ok || frame->processing_error ||
         frame->microphone_clipped || frame->reference_clipped ||
         frame->output_underrun_delta > 0U || stale_or_late);
    const bool aec_hard_failure =
        !frame->enabled || !frame->initialized || frame->processing_error;

    if (!frame->playback_active) {
        health->reference_stable_frames = 0U;
        health->reference_anomaly_frames = 0U;
        result->reference_health = NUWAX_BARGE_HEALTH_GREEN;
        result->reference_locked = false;
    } else {
        result->reference_health = update_health_streak(
            reference_anomaly,
            false,
            &health->reference_stable_frames,
            &health->reference_anomaly_frames,
            &health->config);
        result->reference_locked =
            result->reference_health == NUWAX_BARGE_HEALTH_GREEN &&
            reference_timeline_ok && reference_signal_ok;
    }

    if (safe_silent_passthrough && frame->enabled && frame->initialized) {
        /* No playback energy exists to cancel. Keep barge-in conservative,
         * but do not turn an intentional silent pass-through into RED. */
        health->aec_stable_frames = 0U;
        health->aec_anomaly_frames = 0U;
        result->aec_health = NUWAX_BARGE_HEALTH_YELLOW;
    } else if (!frame->playback_active && frame->enabled && frame->initialized &&
        !frame->processing_error) {
        health->aec_anomaly_frames = 0U;
        health->aec_stable_frames = saturating_increment_u16(
            health->aec_stable_frames);
        result->aec_health = NUWAX_BARGE_HEALTH_GREEN;
    } else {
        result->aec_health = update_health_streak(
            aec_anomaly || aec_hard_failure,
            aec_hard_failure,
            &health->aec_stable_frames,
            &health->aec_anomaly_frames,
            &health->config);
    }

    result->aec_stable_frames = health->aec_stable_frames;
    result->aec_anomaly_frames = health->aec_anomaly_frames;
    result->reference_stable_frames = health->reference_stable_frames;
    result->reference_anomaly_frames = health->reference_anomaly_frames;
    result->metrics_reason_bits = classify_reasons(health, frame);
    result->metrics_runtime_state = classify_runtime_state(
        frame,
        result,
        aec_anomaly || reference_anomaly || safe_silent_passthrough);
    result->fail_closed =
        result->aec_health == NUWAX_BARGE_HEALTH_RED ||
        result->reference_health == NUWAX_BARGE_HEALTH_RED;
    return true;
}
