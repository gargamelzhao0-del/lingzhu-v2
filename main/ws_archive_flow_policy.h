#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Failure ceilings, not pacing delays. Real BIO progress may renew the stall
 * deadline, never the absolute frame/heartbeat or foreground-cancel bound. */
#define NUWAX_WS_SD_FRAME_TOTAL_MS 8000U
#define NUWAX_WS_SD_NO_PROGRESS_MS 2500U
#define NUWAX_WS_SD_CANCEL_DRAIN_MS 2500U
#define NUWAX_WS_SD_EVENT_SLICE_MS 20U
#define NUWAX_WS_SD_CONTROL_HEADROOM_BYTES 8192U

typedef struct {
    int64_t last_progress_us, cancel_deadline_us;
    bool initialized, cancelling;
} nuwax_ws_archive_deadline_t;

static inline int64_t nuwax_ws_archive_frame_deadline(
    nuwax_ws_archive_deadline_t *s, int64_t now, int64_t soft, int64_t hard,
    bool unsafe, bool cancel, bool progressed)
{
    if (!s->initialized) { s->initialized = true; s->last_progress_us = now; }
    if (progressed) s->last_progress_us = now;
    if (cancel && !s->cancelling) {
        s->cancelling = true;
        s->cancel_deadline_us = now + (int64_t)NUWAX_WS_SD_CANCEL_DRAIN_MS * 1000;
    }
    int64_t limit = soft;
    if (unsafe) {
        limit = s->last_progress_us + (int64_t)NUWAX_WS_SD_NO_PROGRESS_MS * 1000;
        if (hard < limit) limit = hard;
    }
    if (s->cancelling && s->cancel_deadline_us < limit) limit = s->cancel_deadline_us;
    return limit;
}

/* A global TCP ceiling does not reserve RAM. Limit background queue growth
 * using CURRENT memory and already allocated TCP bytes. Leave control space
 * in the socket and allowance for pbuf/segment overhead. Never shrink or
 * discard TCP-owned data when the budget falls. */
static inline uint32_t nuwax_ws_archive_queue_limit(uint32_t configured,
    size_t internal_free, size_t psram_free, size_t psram_reserve, uint32_t queued)
{
    uint32_t limit = configured > NUWAX_WS_SD_CONTROL_HEADROOM_BYTES
        ? configured - NUWAX_WS_SD_CONTROL_HEADROOM_BYTES : 0U;
    if (internal_free < 12288U && limit > 16384U) limit = 16384U;
    else if (internal_free < 20480U && limit > 32768U) limit = 32768U;
    const uint64_t spare = psram_free > psram_reserve ? psram_free - psram_reserve : 0U;
    const uint64_t affordable = (uint64_t)queued + spare / 4U * 3U;
    return affordable < limit ? (uint32_t)affordable : limit;
}

static inline bool nuwax_ws_archive_queue_admits(uint32_t limit,
                                                uint32_t queued, size_t next)
{
    return queued <= limit && next <= (size_t)(limit - queued);
}
