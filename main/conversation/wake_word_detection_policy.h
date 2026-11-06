#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Conservative score-only tuning; pronunciation and acoustic gates unchanged. */
#define NUWAX_WAKE_WEAK_THRESHOLD_Q1000 90U
#define NUWAX_WAKE_STRONG_THRESHOLD_Q1000 90U
#define NUWAX_WAKE_FAST_MIN_VOICE_SPAN_MS 160U
#define NUWAX_WAKE_FAST_MIN_SNR_RATIO_Q8 640U
#define NUWAX_WAKE_WEAK_MIN_VOICE_SPAN_MS 250U
#define NUWAX_WAKE_WEAK_MAX_VOICE_SPAN_MS 3500U
#define NUWAX_WAKE_WEAK_MIN_SNR_RATIO_Q8 512U
#define NUWAX_WAKE_WEAK_MAX_CLIP_PPM 10000U

#define NUWAX_WAKE_VOICE_NOISE_MULTIPLIER 2U
#define NUWAX_WAKE_VOICE_NOISE_MARGIN 60U
#define NUWAX_WAKE_VOICE_MIN_PEAK 400U
#define NUWAX_WAKE_VOICE_TAIL_HOLD_MS 900U

/* Audio evidence outlives a model TIMEOUT, but never a capture gap, candidate
 * consumption or task re-arm. The oldest bucket is discarded conservatively.
 * Observe is O(1); only a model candidate scans this fixed, stack-local window. */
#define NUWAX_WAKE_EVIDENCE_BUCKET_MS 128U
#define NUWAX_WAKE_EVIDENCE_BUCKETS 32U
typedef struct {
    uint32_t start_ms;
    uint32_t total_samples;
    uint32_t clipped_samples;
    uint32_t voice_samples;
    uint32_t signal_max;
    uint32_t last_voice_ms;
    uint32_t voice_signal_sum;
} nuwax_wake_evidence_bucket_t;

typedef struct {
    nuwax_wake_evidence_bucket_t buckets[NUWAX_WAKE_EVIDENCE_BUCKETS];
    uint32_t valid;
} nuwax_wake_evidence_t;

typedef struct {
    uint32_t voiced_ms;
    uint32_t signal_max;
    uint32_t clipped_samples;
    uint32_t total_samples;
    uint32_t age_ms;
    uint32_t local_voice_ms;
    uint32_t local_total_ms;
    uint32_t local_signal_avg;
} nuwax_wake_evidence_snapshot_t;

static inline void nuwax_wake_evidence_reset(nuwax_wake_evidence_t *e) {
    e->valid = 0U;
}

static inline void nuwax_wake_evidence_observe(
    nuwax_wake_evidence_t *e, uint32_t now_ms, uint32_t samples,
    uint32_t clipped, uint32_t signal, bool voice_like) {
    if (samples == 0U || clipped > samples) {
        nuwax_wake_evidence_reset(e);
        return;
    }
    const uint32_t tick = now_ms / NUWAX_WAKE_EVIDENCE_BUCKET_MS;
    const uint32_t index = tick % NUWAX_WAKE_EVIDENCE_BUCKETS;
    const uint32_t mask = 1U << index;
    const uint32_t start = tick * NUWAX_WAKE_EVIDENCE_BUCKET_MS;
    nuwax_wake_evidence_bucket_t *b = &e->buckets[index];
    if (!(e->valid & mask) || b->start_ms != start) {
        *b = (nuwax_wake_evidence_bucket_t){.start_ms = start};
        e->valid |= mask;
    }
    if (samples > UINT32_MAX - b->total_samples) {
        nuwax_wake_evidence_reset(e);
        return;
    }
    b->total_samples += samples;
    b->clipped_samples += clipped;
    if (voice_like) {
        b->voice_samples += samples;
        /* A bucket contains 128 ms of PCM16; keep shadow statistics bounded. */
        const uint64_t sum = (uint64_t)b->voice_signal_sum +
                             (uint64_t)signal * samples;
        b->voice_signal_sum = sum > UINT32_MAX ? UINT32_MAX : (uint32_t)sum;
        b->last_voice_ms = now_ms;
        if (signal > b->signal_max) b->signal_max = signal;
    }
}

static inline nuwax_wake_evidence_snapshot_t nuwax_wake_evidence_snapshot(
    const nuwax_wake_evidence_t *e, uint32_t now_ms, uint32_t sample_rate) {
    nuwax_wake_evidence_snapshot_t out = {0};
    uint64_t total = 0U, clipped = 0U, voice = 0U;
    uint32_t youngest_voice = UINT32_MAX;
    uint64_t local_voice = 0U, local_total = 0U, local_signal = 0U;
    if (sample_rate == 0U) return out;
    for (uint32_t i = 0U; i < NUWAX_WAKE_EVIDENCE_BUCKETS; ++i) {
        const nuwax_wake_evidence_bucket_t *b = &e->buckets[i];
        if (!(e->valid & (1U << i)) ||
            (uint32_t)(now_ms - b->start_ms) >
                NUWAX_WAKE_WEAK_MAX_VOICE_SPAN_MS - NUWAX_WAKE_EVIDENCE_BUCKET_MS) continue;
        total += b->total_samples;
        clipped += b->clipped_samples;
        voice += b->voice_samples;
        if ((uint32_t)(now_ms - b->start_ms) <= 384U) {
            local_voice += b->voice_samples;
            local_total += b->total_samples;
            local_signal += b->voice_signal_sum;
        }
        if (b->voice_samples) {
            const uint32_t age = now_ms - b->last_voice_ms;
            if (age < youngest_voice) youngest_voice = age;
            if (b->signal_max > out.signal_max) out.signal_max = b->signal_max;
        }
    }
    /* Silence, expired voice and malformed counters can never lend evidence. */
    const uint64_t voiced_ms = voice * 1000ULL / sample_rate;
    if (youngest_voice > NUWAX_WAKE_VOICE_TAIL_HOLD_MS ||
        total > UINT32_MAX || clipped > total || voiced_ms > UINT32_MAX) {
        return (nuwax_wake_evidence_snapshot_t){0};
    }
    out.voiced_ms = (uint32_t)voiced_ms;
    out.total_samples = (uint32_t)total;
    out.clipped_samples = (uint32_t)clipped;
    out.age_ms = youngest_voice;
    out.local_voice_ms = (uint32_t)(local_voice * 1000ULL / sample_rate);
    out.local_total_ms = (uint32_t)(local_total * 1000ULL / sample_rate);
    out.local_signal_avg = local_voice ? (uint32_t)(local_signal / local_voice) : 0U;
    return out;
}

static inline uint32_t nuwax_wake_voice_threshold(uint32_t noise_avg_abs) {
    const uint32_t scaled =
        noise_avg_abs * NUWAX_WAKE_VOICE_NOISE_MULTIPLIER;
    const uint32_t margin = noise_avg_abs + NUWAX_WAKE_VOICE_NOISE_MARGIN;
    return scaled > margin ? scaled : margin;
}

static inline bool nuwax_wake_voice_frame_is_voice_like(
    uint32_t frame_avg_abs,
    uint32_t frame_peak,
    uint32_t noise_avg_abs) {
    return frame_avg_abs >= nuwax_wake_voice_threshold(noise_avg_abs) &&
           frame_peak >= NUWAX_WAKE_VOICE_MIN_PEAK;
}

static inline bool nuwax_wake_voice_tail_expired(
    uint32_t now_ms,
    uint32_t last_voice_ms) {
    return last_voice_ms != 0U &&
           (uint32_t)(now_ms - last_voice_ms) >
               NUWAX_WAKE_VOICE_TAIL_HOLD_MS;
}

static inline bool nuwax_wake_detection_accept(
    uint32_t probability_q1000,
    uint32_t noise_avg_abs,
    uint32_t signal_avg_abs,
    uint32_t voice_span_ms,
    uint32_t clipped_samples,
    uint32_t total_samples) {
    /* A model score is not independent acoustic evidence. Even the strong
     * score tier must satisfy the same recent voice/SNR/clipping checks. */
    if (probability_q1000 < NUWAX_WAKE_WEAK_THRESHOLD_Q1000 ||
        noise_avg_abs == 0U || total_samples == 0U ||
        voice_span_ms < NUWAX_WAKE_FAST_MIN_VOICE_SPAN_MS ||
        voice_span_ms > NUWAX_WAKE_WEAK_MAX_VOICE_SPAN_MS) {
        return false;
    }
    const uint64_t snr_ratio_q8 =
        ((uint64_t)signal_avg_abs << 8U) / noise_avg_abs;
    const uint64_t clip_ppm =
        ((uint64_t)clipped_samples * 1000000ULL) / total_samples;
    if (clip_ppm > NUWAX_WAKE_WEAK_MAX_CLIP_PPM) {
        return false;
    }
    if (voice_span_ms < NUWAX_WAKE_WEAK_MIN_VOICE_SPAN_MS) {
        return snr_ratio_q8 >= NUWAX_WAKE_FAST_MIN_SNR_RATIO_Q8;
    }
    return snr_ratio_q8 >= NUWAX_WAKE_WEAK_MIN_SNR_RATIO_Q8;
}

/* Diagnostic only. This deliberately stricter recent-window check never
 * authorizes or suppresses a wake until recordings validate its tradeoffs. */
static inline const char *nuwax_wake_shadow_reason(
    uint32_t score, uint32_t noise, const nuwax_wake_evidence_snapshot_t *e) {
    if (score < NUWAX_WAKE_WEAK_THRESHOLD_Q1000) return "score";
    if (!noise || !e->total_samples) return "no_evidence";
    if (e->local_voice_ms < 160U) return "recent_voice_short";
    if ((uint64_t)e->local_voice_ms * 2U < e->local_total_ms) return "recent_voice_sparse";
    if ((uint64_t)e->local_signal_avg < (uint64_t)noise * 2U) return "recent_signal_weak";
    if ((uint64_t)e->clipped_samples * 1000000U >
        (uint64_t)e->total_samples * NUWAX_WAKE_WEAK_MAX_CLIP_PPM) return "clipping";
    return "pass";
}
