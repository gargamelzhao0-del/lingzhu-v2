#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Capacity is filesystem capacity, not the nominal size printed on the card.
 * Reserve 1%, bounded to 16..128 MiB, plus a complete next file and metadata.
 * Reclaim ahead of the writer, with three files of hysteresis. */
static inline uint64_t sd_monitor_reserve_bytes(uint64_t total)
{
    const uint64_t mib = 1024ULL * 1024ULL;
    uint64_t reserve = total / 100U;
    if (reserve < 16U * mib) reserve = 16U * mib;
    if (reserve > 128U * mib) reserve = 128U * mib;
    return reserve;
}

static inline uint64_t sd_monitor_required_bytes(uint64_t total, uint32_t file_bytes, bool ahead)
{
    return sd_monitor_reserve_bytes(total) + (uint64_t)file_bytes * (ahead ? 4U : 1U) + 65536U;
}

/* Meetings and the legacy/no-card route keep their existing duration policy. */
static inline bool sd_monitor_unlimited(bool card_session, bool monitoring)
{
    return card_session && monitoring;
}
