#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Single owner: battery task. Never placed on its small task stack. */
#define NUWAX_BATTERY_HISTORY_CAPACITY 64U
#define NUWAX_BATTERY_HISTORY_DUMP_INTERVAL_MS 64000U

typedef struct {
    uint64_t timestamp_ms;
    int32_t error;
    uint32_t sequence;
    uint16_t voltage_mv;
    int16_t current_ma;
    uint16_t remaining_mah;
    uint16_t full_charge_mah;
    uint16_t flags;
    int16_t temperature_c;
    uint8_t percent;
    uint8_t display_percent;
    /* bit 0: raw telemetry valid; 1: raw power/flags valid;
     * 2: published valid; 3: published stale. */
    uint8_t quality;
    /* bit 0: published charging; 1: published full. */
    uint8_t state;
} nuwax_battery_history_sample_t;

typedef struct {
    nuwax_battery_history_sample_t samples[NUWAX_BATTERY_HISTORY_CAPACITY];
    uint64_t last_dump_ms;
    uint32_t next_sequence;
    uint8_t next;
    uint8_t count;
    bool started;
} nuwax_battery_history_t;

static inline void nuwax_battery_history_reset(nuwax_battery_history_t *history)
{
    memset(history, 0, sizeof(*history));
}

static inline void nuwax_battery_history_append(
    nuwax_battery_history_t *history, nuwax_battery_history_sample_t sample)
{
    if (!history->started) {
        history->started = true;
        history->last_dump_ms = sample.timestamp_ms;
    }
    sample.sequence = history->next_sequence++;
    history->samples[history->next] = sample;
    history->next = (history->next + 1U) % NUWAX_BATTERY_HISTORY_CAPACITY;
    if (history->count < NUWAX_BATTERY_HISTORY_CAPACITY) history->count++;
}

static inline const nuwax_battery_history_sample_t *nuwax_battery_history_get(
    const nuwax_battery_history_t *history, unsigned oldest_index)
{
    if (oldest_index >= history->count) return NULL;
    const unsigned start = (history->next + NUWAX_BATTERY_HISTORY_CAPACITY -
                            history->count) % NUWAX_BATTERY_HISTORY_CAPACITY;
    return &history->samples[(start + oldest_index) % NUWAX_BATTERY_HISTORY_CAPACITY];
}

static inline bool nuwax_battery_history_dump_due(
    nuwax_battery_history_t *history, uint64_t now)
{
    if (!history->started) return false;
    if (now < history->last_dump_ms) {
        history->last_dump_ms = now;
        return false;
    }
    if (now - history->last_dump_ms < NUWAX_BATTERY_HISTORY_DUMP_INTERVAL_MS)
        return false;
    history->last_dump_ms = now;
    return true;
}
