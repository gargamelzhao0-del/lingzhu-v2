#pragma once

#include <stdbool.h>
#include <stdint.h>

#define NUWAX_SPEAKER_JITTER_SINGLE_GROWTH_MS 160U
#define NUWAX_SPEAKER_JITTER_REPEAT_GROWTH_MS 320U
#define NUWAX_SPEAKER_JITTER_STABLE_DECAY_PLAYBACKS 2U
#define NUWAX_SPEAKER_JITTER_BASE_TO_MIN_PLAYBACKS 5U
#define NUWAX_SPEAKER_JITTER_STABLE_DECAY_MS 160U
#define NUWAX_SPEAKER_JITTER_ADAPT_REBUFFER_MIN_MS 800U
/* Never let the sole playback consumer wait indefinitely for a full
 * watermark: with some PCM available, a bounded fallback keeps ingress from
 * filling the ring and stalling the WebSocket RX path. */
#define NUWAX_SPEAKER_JITTER_FORCE_RESUME_WAIT_MS 2000U
/* Separate from the 60 s ingress-idle limit: trickle input must not hold an
 * unstarted response forever. Normal adaptive watermarks remain unchanged. */
#define NUWAX_SPEAKER_JITTER_STARTUP_TIMEOUT_MS 5000U
#define NUWAX_SPEAKER_NOTIFICATION_STARTUP_TIMEOUT_MS 10000U

/* Notifications have their own cancellation transaction and a longer startup
 * budget. Ingress may renew idle liveness, never this absolute startup age. */
static inline bool nuwax_speaker_notification_startup_expired(
    bool playback_started, bool ready, uint32_t startup_age_ms) {
    return !playback_started && !ready &&
           startup_age_ms >= NUWAX_SPEAKER_NOTIFICATION_STARTUP_TIMEOUT_MS;
}

static inline bool nuwax_speaker_jitter_startup_expired(
    bool playback_started, bool ready, uint32_t startup_age_ms) {
    return !playback_started && !ready &&
           startup_age_ms >= NUWAX_SPEAKER_JITTER_STARTUP_TIMEOUT_MS;
}

typedef enum {
    NUWAX_SPEAKER_STALL_WAIT = 0,
    NUWAX_SPEAKER_STALL_WARN,
    NUWAX_SPEAKER_STALL_ABORT,
} nuwax_speaker_stall_action_t;

static inline nuwax_speaker_stall_action_t nuwax_speaker_stall_action(
    uint32_t stalled_ms,
    uint32_t warning_ms,
    uint32_t hard_timeout_ms,
    bool warning_emitted) {
    if (hard_timeout_ms != 0U && stalled_ms >= hard_timeout_ms) {
        return NUWAX_SPEAKER_STALL_ABORT;
    }
    if (!warning_emitted && warning_ms != 0U && stalled_ms >= warning_ms) {
        return NUWAX_SPEAKER_STALL_WARN;
    }
    return NUWAX_SPEAKER_STALL_WAIT;
}

static inline uint32_t nuwax_speaker_jitter_audio_ms(
    uint32_t samples,
    uint32_t sample_rate) {
    if (sample_rate == 0U) {
        return 0U;
    }
    const uint64_t duration_ms =
        ((uint64_t)samples * 1000ULL) / sample_rate;
    return duration_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)duration_ms;
}

/* queued_records and queued_samples include a packet already held by the
 * consumer. END is an ingress fact, so a completed short response may drain
 * without reaching the normal watermark. */
static inline bool nuwax_speaker_jitter_ready_ms(bool playback_started,
                                                 bool end_accepted,
                                                 uint32_t queued_records,
                                                 uint32_t queued_samples,
                                                 uint32_t sample_rate,
                                                 uint32_t startup_target_ms,
                                                 uint32_t resume_target_ms) {
    if (queued_records == 0U) {
        return false;
    }
    if (end_accepted) {
        return true;
    }
    const uint32_t required_ms =
        playback_started ? resume_target_ms : startup_target_ms;
    return required_ms == 0U ||
           nuwax_speaker_jitter_audio_ms(queued_samples, sample_rate) >=
               required_ms;
}

static inline bool nuwax_speaker_jitter_force_resume(
    bool playback_started,
    bool rebuffering,
    uint32_t waited_ms,
    uint32_t queued_records,
    uint32_t queued_samples,
    uint32_t max_wait_ms) {
    return playback_started && rebuffering && waited_ms >= max_wait_ms &&
           queued_records > 0U && queued_samples > 0U;
}

static inline bool nuwax_speaker_jitter_should_enter_rebuffer(
    bool playback_active,
    bool already_rebuffering,
    uint32_t queued_records,
    uint32_t empty_wait_ms,
    uint32_t empty_grace_ms) {
    return playback_active && !already_rebuffering && queued_records == 0U &&
           empty_wait_ms >= empty_grace_ms;
}

/* An empty queue is only a candidate underrun. If END arrives without any
 * additional PCM, the wait was the natural stream tail and must not train the
 * next playback to start later. */
static inline bool nuwax_speaker_jitter_rebuffer_is_midstream(
    uint32_t pcm_frames_at_entry,
    uint32_t pcm_frames_at_resume) {
    return pcm_frames_at_resume > pcm_frames_at_entry;
}

static inline uint32_t nuwax_speaker_jitter_resume_target_ms(
    uint32_t confirmed_rebuffer_events,
    uint32_t min_ms,
    uint32_t max_ms) {
    uint32_t target_ms = min_ms;
    if (confirmed_rebuffer_events == 1U) {
        target_ms = min_ms > UINT32_MAX - 160U
                        ? UINT32_MAX
                        : min_ms + 160U;
    } else if (confirmed_rebuffer_events == 2U) {
        target_ms = min_ms > UINT32_MAX - 320U
                        ? UINT32_MAX
                        : min_ms + 320U;
    } else if (confirmed_rebuffer_events >= 3U) {
        target_ms = max_ms;
    }
    if (target_ms < min_ms) {
        return min_ms;
    }
    return target_ms > max_ms ? max_ms : target_ms;
}

/* A measured ingress outage is stronger evidence than a historical tier. The
 * resumed buffer must cover that outage plus a small pacing cushion, bounded
 * by the configured latency ceiling. */
static inline uint32_t nuwax_speaker_jitter_resume_target_with_gap(
    uint32_t confirmed_rebuffer_events,
    uint32_t min_ms,
    uint32_t max_ms,
    uint32_t observed_gap_ms,
    uint32_t cushion_ms) {
    uint32_t target_ms = nuwax_speaker_jitter_resume_target_ms(
        confirmed_rebuffer_events, min_ms, max_ms);
    if (observed_gap_ms == 0U || target_ms >= max_ms) {
        return target_ms;
    }
    const uint32_t gap_target =
        observed_gap_ms > UINT32_MAX - cushion_ms
            ? UINT32_MAX
            : observed_gap_ms + cushion_ms;
    if (gap_target > target_ms) {
        target_ms = gap_target > max_ms ? max_ms : gap_target;
    }
    return target_ms;
}

static inline uint32_t nuwax_speaker_jitter_popcount3(uint8_t history) {
    history &= 0x07U;
    return (uint32_t)(history & 1U) +
           (uint32_t)((history >> 1U) & 1U) +
           (uint32_t)((history >> 2U) & 1U);
}

static inline uint32_t nuwax_speaker_jitter_adapt_startup_ms(
    uint32_t current_ms,
    bool playback_rebuffered,
    uint32_t rebuffer_max_wait_ms,
    uint32_t min_ms,
    uint32_t base_ms,
    uint32_t max_ms,
    uint8_t *recent_instability_history,
    uint8_t *stable_playbacks) {
    if (!recent_instability_history || !stable_playbacks ||
        min_ms > base_ms || base_ms > max_ms) {
        return current_ms;
    }
    /* A long ingress outage cannot be hidden without imposing the same delay
     * on every later response. The measured wait is used only to decide
     * whether the startup tier deserves promotion. */
    uint8_t history =
        (uint8_t)((*recent_instability_history << 1U) & 0x07U);
    /* A short, already-recovered outage must not penalize every subsequent
     * response with a larger startup buffer. Only promote the startup tier
     * after a material rebuffer; longer outages still use the existing
     * adaptive growth and stable-playback decay. */
    const bool playback_unstable =
        playback_rebuffered &&
        rebuffer_max_wait_ms >= NUWAX_SPEAKER_JITTER_ADAPT_REBUFFER_MIN_MS;
    if (playback_unstable) {
        history |= 1U;
    }
    *recent_instability_history = history;

    if (current_ms < min_ms) {
        current_ms = min_ms;
    } else if (current_ms > max_ms) {
        current_ms = max_ms;
    }
    if (playback_unstable) {
        *stable_playbacks = 0U;
        if (current_ms < base_ms) {
            return base_ms;
        }
        const uint32_t increase =
            nuwax_speaker_jitter_popcount3(history) >= 2U
                ? NUWAX_SPEAKER_JITTER_REPEAT_GROWTH_MS
                : NUWAX_SPEAKER_JITTER_SINGLE_GROWTH_MS;
        return increase >= max_ms - current_ms ? max_ms
                                                : current_ms + increase;
    }

    /* A recovered short midstream outage is neutral for buffer growth, but
     * breaks the consecutive clean-playback streak. END-only tail waits are
     * excluded by the caller's confirmed midstream event count. */
    if (playback_rebuffered) {
        *stable_playbacks = 0U;
        return current_ms;
    }
    if (*stable_playbacks < UINT8_MAX) {
        (*stable_playbacks)++;
    }
    if (current_ms > base_ms &&
        *stable_playbacks >= NUWAX_SPEAKER_JITTER_STABLE_DECAY_PLAYBACKS) {
        *stable_playbacks = 0U;
        return current_ms > base_ms + NUWAX_SPEAKER_JITTER_STABLE_DECAY_MS
                   ? current_ms - NUWAX_SPEAKER_JITTER_STABLE_DECAY_MS
                   : base_ms;
    }
    if (current_ms == base_ms && min_ms < base_ms &&
        *stable_playbacks >= NUWAX_SPEAKER_JITTER_BASE_TO_MIN_PLAYBACKS) {
        *stable_playbacks = 0U;
        return min_ms;
    }
    return current_ms;
}
