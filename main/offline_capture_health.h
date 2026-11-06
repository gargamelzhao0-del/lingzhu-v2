#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Device-local state only. No cloud enum or archive format changes. */
typedef enum {
    NUWAX_OFFLINE_CAPTURE_IDLE = 0,
    NUWAX_OFFLINE_CAPTURE_STARTING,
    NUWAX_OFFLINE_CAPTURE_RECORDING,
    NUWAX_OFFLINE_CAPTURE_BUFFERING,
    NUWAX_OFFLINE_CAPTURE_UNAVAILABLE,
} nuwax_offline_capture_health_t;

/* Preserve the already-tested 12s SD stalls within 768 x 20ms staging.
 * The ring-capacity check can stop earlier if backlog already occupies it. */
#define NUWAX_OFFLINE_STORAGE_GRACE_MS 15000U

typedef struct {
    bool buffering;
    uint32_t buffering_since_ms;
} nuwax_offline_capture_watch_t;

/* Capacity belongs to the actual producer staging, not the writer queue that
 * may be inaccessible while storage is recovering. All subtraction is wrap
 * safe for these sub-minute intervals. The caller freezes an absolute expiry. */
static inline uint32_t nuwax_offline_recovery_budget_ms(
    const nuwax_offline_capture_watch_t *watch, uint32_t now_ms,
    uint32_t free_frames, uint32_t frame_ms, uint32_t reserve_ms, uint32_t cap_ms)
{
    const uint32_t elapsed = watch && watch->buffering ?
        (uint32_t)(now_ms - watch->buffering_since_ms) : 0U;
    if (elapsed >= NUWAX_OFFLINE_STORAGE_GRACE_MS) return 0U;
    const uint64_t capacity = (uint64_t)free_frames * frame_ms;
    if (capacity <= reserve_ms) return 0U;
    uint64_t budget = capacity - reserve_ms;
    const uint32_t grace = NUWAX_OFFLINE_STORAGE_GRACE_MS - elapsed;
    if (budget > grace) budget = grace;
    if (budget > cap_ms) budget = cap_ms;
    return (uint32_t)budget;
}

/* A silent monitoring interval needs no FILE progress. Transient pressure
 * is tolerated only while the producer still has a bounded retention path. */
static inline nuwax_offline_capture_health_t nuwax_offline_capture_observe(
    nuwax_offline_capture_watch_t *watch, uint32_t now_ms,
    bool sink_ready, bool retaining, bool lost_current_frame)
{
    if (lost_current_frame || (!sink_ready && !retaining)) {
        return NUWAX_OFFLINE_CAPTURE_UNAVAILABLE;
    }
    if (sink_ready) {
        watch->buffering = false;
        return NUWAX_OFFLINE_CAPTURE_RECORDING;
    }
    if (!watch->buffering) {
        watch->buffering = true;
        watch->buffering_since_ms = now_ms;
    }
    return (uint32_t)(now_ms - watch->buffering_since_ms) >=
                   NUWAX_OFFLINE_STORAGE_GRACE_MS
        ? NUWAX_OFFLINE_CAPTURE_UNAVAILABLE : NUWAX_OFFLINE_CAPTURE_BUFFERING;
}

static inline bool nuwax_offline_capture_health_is_recording(
    nuwax_offline_capture_health_t health)
{
    return health == NUWAX_OFFLINE_CAPTURE_RECORDING ||
           health == NUWAX_OFFLINE_CAPTURE_BUFFERING;
}

/* Queued PCM is not proof that a new card recording has reached storage. */
static inline nuwax_offline_capture_health_t nuwax_offline_capture_admission_health(
    nuwax_offline_capture_health_t health, bool card_session, bool durable_frame)
{
    if (card_session && !durable_frame &&
        (health == NUWAX_OFFLINE_CAPTURE_RECORDING || health == NUWAX_OFFLINE_CAPTURE_BUFFERING))
        return NUWAX_OFFLINE_CAPTURE_STARTING;
    return health;
}

static inline bool nuwax_offline_wait_expired(uint32_t now_ms,
                                              uint32_t since_ms,
                                              uint32_t hours)
{
    return (uint64_t)(uint32_t)(now_ms - since_ms) >=
           (uint64_t)hours * 3600000ULL;
}
