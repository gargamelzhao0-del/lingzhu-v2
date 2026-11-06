#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Main audio owner only. UI activity, transport pings and retry scheduling
 * must never extend the meaningful conversation deadline. */
typedef struct {
    uint32_t awake_generation;
    uint32_t last_activity_ms;
    uint32_t timeout_ms;
    uint32_t failure_started_ms;
    uint32_t failures;
    uint32_t stable_frames;
    bool failure_active;
} nuwax_normal_conversation_policy_t;

static inline void nuwax_normal_conversation_begin(
    nuwax_normal_conversation_policy_t *p, uint32_t generation,
    uint32_t now_ms, uint32_t timeout_ms) {
    *p = (nuwax_normal_conversation_policy_t){
        .awake_generation = generation, .last_activity_ms = now_ms,
        .timeout_ms = timeout_ms};
}

static inline void nuwax_normal_conversation_note_activity(
    nuwax_normal_conversation_policy_t *p, uint32_t generation,
    uint32_t now_ms) {
    if (generation != 0U && generation == p->awake_generation) {
        p->last_activity_ms = now_ms;
    }
}

static inline bool nuwax_normal_conversation_expired(
    const nuwax_normal_conversation_policy_t *p, uint32_t generation,
    uint32_t now_ms) {
    return generation != 0U && generation == p->awake_generation &&
           p->timeout_ms != 0U &&
           (uint32_t)(now_ms - p->last_activity_ms) >= p->timeout_ms;
}

/* A successful allocation alone is not recovery: require 500 ms of full
 * 20 ms capture frames before forgiving a resource failure episode. */
static inline void nuwax_normal_conversation_note_capture_frame(
    nuwax_normal_conversation_policy_t *p) {
    if (p->failure_active && ++p->stable_frames >= 25U) {
        p->failure_active = false;
        p->failures = 0U;
        p->stable_frames = 0U;
    }
}

/* Returns bounded backoff, or zero when the five-second recovery budget
 * is exhausted. Called only for an actual audio-source failure. */
static inline uint32_t nuwax_normal_conversation_retry_delay(
    nuwax_normal_conversation_policy_t *p, uint32_t now_ms) {
    if (!p->failure_active) {
        p->failure_active = true;
        p->failure_started_ms = now_ms;
        p->failures = 0U;
    }
    p->stable_frames = 0U;
    const uint32_t elapsed_ms = now_ms - p->failure_started_ms;
    /* Owner notifications can wake a backoff early. Cap attempts as well as
     * elapsed time so notification traffic cannot cause an allocation spin. */
    if (elapsed_ms >= 5000U || p->failures >= 10U) return 0U;
    const uint32_t attempt = p->failures++;
    uint32_t delay = attempt == 0U ? 100U : attempt == 1U ? 250U :
                     attempt == 2U ? 500U : 1000U;
    if (delay > 5000U - elapsed_ms) delay = 5000U - elapsed_ms;
    return delay;
}
