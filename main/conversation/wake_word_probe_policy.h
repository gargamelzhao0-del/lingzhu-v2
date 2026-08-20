#pragma once

#include <stdbool.h>
#include <stdint.h>

#define NUWAX_WAKE_PROBE_REQUIRED_SAFE_FRAMES 3U
#define NUWAX_WAKE_PROBE_COOLDOWN_MS 15000U
#define NUWAX_WAKE_PROBE_ATTENUATION_DB 12U
#define NUWAX_WAKE_PROBE_DURATION_MS 80U

typedef struct {
    uint8_t consecutive_safe_frames;
    bool candidate_latched;
    bool has_requested;
    uint32_t last_requested_ms;
} nuwax_wake_probe_policy_t;

static inline void nuwax_wake_probe_policy_reset(
    nuwax_wake_probe_policy_t *policy) {
    if (policy) {
        *policy = (nuwax_wake_probe_policy_t){0};
    }
}

static inline bool nuwax_wake_probe_policy_observe(
    nuwax_wake_probe_policy_t *policy,
    bool nearfield_safe,
    uint32_t now_ms) {
    if (!policy) {
        return false;
    }
    if (!nearfield_safe) {
        policy->consecutive_safe_frames = 0U;
        policy->candidate_latched = false;
        return false;
    }
    if (policy->candidate_latched) {
        return false;
    }
    if (policy->consecutive_safe_frames <
        NUWAX_WAKE_PROBE_REQUIRED_SAFE_FRAMES) {
        policy->consecutive_safe_frames++;
    }
    if (policy->consecutive_safe_frames <
        NUWAX_WAKE_PROBE_REQUIRED_SAFE_FRAMES) {
        return false;
    }

    policy->candidate_latched = true;
    if (policy->has_requested &&
        (uint32_t)(now_ms - policy->last_requested_ms) <
            NUWAX_WAKE_PROBE_COOLDOWN_MS) {
        return false;
    }
    policy->has_requested = true;
    policy->last_requested_ms = now_ms;
    return true;
}
