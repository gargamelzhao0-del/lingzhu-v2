#pragma once

#include <stdbool.h>
#include <stdint.h>

/* 8h worst case at 16kHz mono PCM16, 20ms: 1,440,000 records of
 * (640 PCM + 22 NA + 4 length) bytes, plus segment metadata. Keep room
 * for backlog and filesystem reserve; old saved sdkconfigs are upgraded. */
#define NUWAX_OFFLINE_AUDIO_MIN_QUOTA_BYTES (1536ULL * 1024ULL * 1024ULL)
#define NUWAX_OFFLINE_AUDIO_FREE_RESERVE_BYTES (16ULL * 1024ULL * 1024ULL)
/* Directory mutations from successful writes/deletes require only metadata
 * refresh. A new or uncertain media epoch requires full prefix validation. */
static inline bool nuwax_offline_recovery_scan_required(bool media_was_ready,
    bool degraded, bool writer_error, bool previous_error)
{
    return !media_was_ready || degraded || writer_error || previous_error;
}
static inline uint64_t nuwax_offline_audio_quota(uint64_t configured)
{
    return configured < NUWAX_OFFLINE_AUDIO_MIN_QUOTA_BYTES
        ? NUWAX_OFFLINE_AUDIO_MIN_QUOTA_BYTES : configured;
}

/* SD scenes use actual filesystem free space plus the existing reserve.
 * The fixed legacy spool quota must not cut a second long meeting short on
 * a mostly empty card. Old firmware profiles retain their quota behavior. */
static inline bool nuwax_offline_quota_blocked(bool sd_scenes, uint64_t used,
                                              uint64_t quota, uint64_t next)
{
    return !sd_scenes && (used > quota || quota - used < next);
}

static inline bool nuwax_offline_limit_elapsed(uint64_t now_us,
                                              uint64_t start_us,
                                              uint32_t hours)
{
    return now_us >= start_us && now_us - start_us >=
           (uint64_t)hours * 3600000000ULL;
}

/* Only actual offline MIC ownership consumes the session budget. */
static inline uint64_t nuwax_offline_budget_used(uint64_t previous_us,
                                                uint64_t started_us,
                                                uint64_t now_us)
{
    const uint64_t interval = now_us >= started_us ? now_us - started_us : 0U;
    return UINT64_MAX - previous_us < interval ? UINT64_MAX : previous_us + interval;
}
