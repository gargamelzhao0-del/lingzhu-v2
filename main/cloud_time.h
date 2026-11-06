#pragma once
#include <stdbool.h>
#include <stdint.h>

/* UTC Unix milliseconds, never a server/device uptime or a 32-bit value. */
#define NUWAX_CLOUD_TIME_MIN_MS UINT64_C(1704067200000)
#define NUWAX_CLOUD_TIME_MAX_MS UINT64_C(253402300799999)
typedef struct {
    uint64_t utc_ms;
    uint64_t uptime_ms;
} nuwax_cloud_time_anchor_t;

static inline bool nuwax_cloud_time_valid(uint64_t utc_ms)
{
    return utc_ms >= NUWAX_CLOUD_TIME_MIN_MS && utc_ms <= NUWAX_CLOUD_TIME_MAX_MS;
}
static inline bool nuwax_cloud_time_number(double value, uint64_t *out)
{
    if (!(value >= (double)NUWAX_CLOUD_TIME_MIN_MS && value <= (double)NUWAX_CLOUD_TIME_MAX_MS) ||
        value != (double)(uint64_t)value) return false;
    *out = (uint64_t)value;
    return true;
}
/* Only compare uptimes from the SAME boot. Callers persist/verify boot identity. */
static inline bool nuwax_cloud_time_project(const nuwax_cloud_time_anchor_t *anchor,
    uint64_t uptime_ms, uint64_t *utc_ms)
{
    if (!anchor || !utc_ms || !nuwax_cloud_time_valid(anchor->utc_ms)) return false;
    uint64_t value;
    if (uptime_ms >= anchor->uptime_ms) {
        const uint64_t delta = uptime_ms - anchor->uptime_ms;
        if (delta > NUWAX_CLOUD_TIME_MAX_MS - anchor->utc_ms) return false;
        value = anchor->utc_ms + delta;
    } else {
        const uint64_t delta = anchor->uptime_ms - uptime_ms;
        if (delta > anchor->utc_ms - NUWAX_CLOUD_TIME_MIN_MS) return false;
        value = anchor->utc_ms - delta;
    }
    *utc_ms = value;
    return true;
}
/* RX publishes RAM only. The SD storage owner persists snapshots off RX. */
bool nuwax_cloud_time_observe(uint64_t server_ms, uint64_t received_uptime_ms);
bool nuwax_cloud_time_snapshot(nuwax_cloud_time_anchor_t *anchor);
