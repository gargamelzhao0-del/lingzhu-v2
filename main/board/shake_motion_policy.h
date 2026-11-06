#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <math.h>

#define NUWAX_SHAKE_SAMPLE_PERIOD_MS 50U
#define NUWAX_SHAKE_WINDOW_MS 1500U
#define NUWAX_SHAKE_MIN_SPAN_MS 400U
#define NUWAX_SHAKE_MIN_PEAK_INTERVAL_MS 100U
#define NUWAX_SHAKE_MAX_PEAK_INTERVAL_MS 500U
#define NUWAX_SHAKE_MAX_SAMPLE_GAP_MS 150U
#define NUWAX_SHAKE_REQUIRED_PEAKS 4U
#define NUWAX_SHAKE_DYNAMIC_PEAK_MG 380.0f
#define NUWAX_SHAKE_GRAVITY_TAU_MS 400.0f
#define NUWAX_SHAKE_QUIET_MG 150.0f
#define NUWAX_SHAKE_QUIET_GYRO_DPS 45.0f
#define NUWAX_SHAKE_REARM_STILL_MS 300U
#define NUWAX_SHAKE_REARM_WINDOW_MS 1000U

typedef struct {
    float gravity[3];
    float direction[3];
    uint32_t last_sample_ms;
    uint32_t candidate_started_ms;
    uint32_t last_peak_ms;
    uint32_t triggered_ms;
    uint32_t quiet_started_ms;
    uint32_t quiet_valid_ms;
    uint32_t window_resets;
    uint32_t invalid_resets;
    uint32_t last_gesture_ms;
    uint32_t dynamic_peak_mg;
    uint8_t axis;
    uint8_t peaks;
    uint8_t peak_high_water;
    int8_t sign;
    bool initialized;
    bool sample_seen;
    bool candidate;
    bool inhibited;
    bool quiet_seen;
    bool skipped_sample;
} nuwax_shake_motion_t;

static inline void nuwax_shake_candidate_reset(nuwax_shake_motion_t *s) {
    s->candidate = false;
    s->peaks = 0U;
    s->sign = 0;
}

/* Sampling faults never count as motion or quiet time. A short gap retains
 * earlier valid peaks; the next sample only rebuilds the baseline. Absolute
 * sample/window deadlines still expire them. Keep post-trigger inhibition. */
static inline void nuwax_shake_motion_invalidate(nuwax_shake_motion_t *s) {
    s->initialized = false;
    s->quiet_seen = false;
    s->quiet_valid_ms = 0U;
    s->skipped_sample = true;
    s->invalid_resets++;
}

/* A bounded bus-lock miss is not sensor corruption. Preserve the gravity and
 * gesture evidence, but never count the unobserved interval as quiet. The
 * next real sample still enforces the absolute sample/gesture deadlines. */
static inline void nuwax_shake_motion_skip(nuwax_shake_motion_t *s) {
    s->skipped_sample = true;
}

/* Constant-state 20 Hz gesture recognition. gyro_sum is used ONLY for quiet
 * re-arming, never as an independent wake trigger. Signed Schmitt thresholds
 * allow a real zero crossing between samples; no exact zero sample is needed. */
static inline bool nuwax_shake_motion_observe(
    nuwax_shake_motion_t *s, uint32_t now_ms,
    const float accel_mg[3], float gyro_sum_dps, uint32_t cooldown_ms) {
    if (!accel_mg || !isfinite(gyro_sum_dps) || gyro_sum_dps < 0.0f ||
        !isfinite(accel_mg[0]) || !isfinite(accel_mg[1]) ||
        !isfinite(accel_mg[2])) {
        nuwax_shake_motion_invalidate(s);
        return false;
    }
    uint32_t elapsed_ms = now_ms - s->last_sample_ms;
    if (s->sample_seen && elapsed_ms > NUWAX_SHAKE_MAX_SAMPLE_GAP_MS) {
        nuwax_shake_motion_invalidate(s);
        nuwax_shake_candidate_reset(s);
    }
    if (!s->initialized) {
        for (unsigned i = 0; i < 3U; ++i) s->gravity[i] = accel_mg[i];
        s->last_sample_ms = now_ms;
        s->initialized = true;
        s->sample_seen = true;
        return false;
    }
    /* Duplicate/catch-up observations cannot manufacture oscillations. */
    if (elapsed_ms < NUWAX_SHAKE_SAMPLE_PERIOD_MS / 2U) return false;
    const bool quiet_gap = s->skipped_sample ||
        elapsed_ms > NUWAX_SHAKE_SAMPLE_PERIOD_MS + 10U;
    s->skipped_sample = false;
    s->last_sample_ms = now_ms;
    float dynamic[3];
    float maximum = 0.0f;
    float magnitude_squared = 0.0f;
    unsigned axis = 0U;
    const float alpha = (float)elapsed_ms /
                        (NUWAX_SHAKE_GRAVITY_TAU_MS + (float)elapsed_ms);
    for (unsigned i = 0; i < 3U; ++i) {
        s->gravity[i] += alpha * (accel_mg[i] - s->gravity[i]);
        dynamic[i] = accel_mg[i] - s->gravity[i];
        const float magnitude = fabsf(dynamic[i]);
        magnitude_squared += dynamic[i] * dynamic[i];
        if (magnitude > maximum) { maximum = magnitude; axis = i; }
    }
    maximum = sqrtf(magnitude_squared);
    if (maximum > (float)s->dynamic_peak_mg)
        s->dynamic_peak_mg = (uint32_t)maximum;

    if (s->inhibited) {
        const bool quiet = maximum <= NUWAX_SHAKE_QUIET_MG &&
                           gyro_sum_dps <= NUWAX_SHAKE_QUIET_GYRO_DPS;
        if (!quiet || (s->quiet_seen &&
            (uint32_t)(now_ms - s->quiet_started_ms) > NUWAX_SHAKE_REARM_WINDOW_MS)) {
            s->quiet_seen = false;
            s->quiet_valid_ms = 0U;
        }
        if (quiet && !s->quiet_seen) {
            s->quiet_seen = true;
            s->quiet_started_ms = now_ms;
            s->quiet_valid_ms = 0U;
        } else if (quiet && !quiet_gap) {
            const uint32_t valid_ms = elapsed_ms < NUWAX_SHAKE_SAMPLE_PERIOD_MS
                ? elapsed_ms : NUWAX_SHAKE_SAMPLE_PERIOD_MS;
            if (s->quiet_valid_ms < NUWAX_SHAKE_REARM_STILL_MS)
                s->quiet_valid_ms += valid_ms;
        }
        if (s->quiet_seen && s->quiet_valid_ms >=
                NUWAX_SHAKE_REARM_STILL_MS &&
            (uint32_t)(now_ms - s->triggered_ms) >= cooldown_ms) {
            s->inhibited = false;
            s->quiet_seen = false;
            s->quiet_valid_ms = 0U;
            nuwax_shake_candidate_reset(s);
        }
        return false;
    }
    if (s->candidate &&
        ((uint32_t)(now_ms - s->candidate_started_ms) > NUWAX_SHAKE_WINDOW_MS ||
         (uint32_t)(now_ms - s->last_peak_ms) > NUWAX_SHAKE_MAX_PEAK_INTERVAL_MS)) {
        s->window_resets++;
        nuwax_shake_candidate_reset(s);
    }
    if (!s->candidate) {
        if (maximum < NUWAX_SHAKE_DYNAMIC_PEAK_MG) return false;
        s->candidate = true;
        s->axis = (uint8_t)axis;
        /* Lock a unit direction for this gesture, including diagonal holds.
         * Unrelated axes cannot contribute separate alternating peaks. */
        for (unsigned i = 0; i < 3U; ++i) s->direction[i] = dynamic[i] / maximum;
        s->sign = 1;
        s->peaks = 1U;
        s->candidate_started_ms = s->last_peak_ms = now_ms;
    } else {
        const float value = dynamic[0] * s->direction[0] +
                            dynamic[1] * s->direction[1] +
                            dynamic[2] * s->direction[2];
        if (fabsf(value) < NUWAX_SHAKE_DYNAMIC_PEAK_MG) return false;
        const int8_t sign = value > 0.0f ? 1 : -1;
        if (sign == s->sign) return false;
        if ((uint32_t)(now_ms - s->last_peak_ms) < NUWAX_SHAKE_MIN_PEAK_INTERVAL_MS) {
            nuwax_shake_candidate_reset(s);
            return false;
        }
        s->sign = sign;
        s->last_peak_ms = now_ms;
        s->peaks++;
    }
    if (s->peaks > s->peak_high_water) s->peak_high_water = s->peaks;
    const uint32_t span = now_ms - s->candidate_started_ms;
    if (s->peaks < NUWAX_SHAKE_REQUIRED_PEAKS || span < NUWAX_SHAKE_MIN_SPAN_MS)
        return false;
    s->last_gesture_ms = span;
    s->triggered_ms = now_ms;
    s->inhibited = true;
    s->quiet_seen = false;
    s->quiet_valid_ms = 0U;
    nuwax_shake_candidate_reset(s);
    return true;
}
