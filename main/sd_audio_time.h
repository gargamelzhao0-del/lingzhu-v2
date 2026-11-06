#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SD_AUDIO_TIME_STARTED 1U
#define SD_AUDIO_TIME_DURATION 2U
typedef struct {
    uint64_t started_at_ms;
    uint64_t duration_ms;
    uint32_t valid;
} sd_audio_session_time_t;

/* Capture/preroll/write queues are bounded to seconds. Expand a frame's low
 * 32 bits near the current uptime, NOT relative to a possibly months-old
 * connection. The wire timestamp remains unchanged. */
static inline bool sd_audio_frame_after_online(uint32_t timestamp_ms,
    uint64_t now_ms, uint64_t online_since_ms)
{
    const int64_t delta = (int32_t)(timestamp_ms - (uint32_t)now_ms);
    if (delta < 0 && now_ms < (uint64_t)-delta) return false;
    const uint64_t captured_ms = delta < 0 ? now_ms - (uint64_t)-delta : now_ms + (uint64_t)delta;
    return captured_ms >= online_since_ms;
}
