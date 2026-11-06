#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "battery_service.h"

#define NUWAX_LOW_BATTERY_SHUTDOWN_PERCENT 1U
#define NUWAX_LOW_BATTERY_WARNING_MS 5000U
#define NUWAX_LOW_BATTERY_FAST_SAMPLE_MS 500U
#define NUWAX_LOW_BATTERY_FRESH_MS 1500U

typedef struct {
    bool warning;
    uint64_t warning_since_ms;
    uint64_t last_sample_ms;
    unsigned samples;
} nuwax_low_battery_shutdown_policy_t;

typedef enum {
    NUWAX_LOW_BATTERY_IDLE,
    NUWAX_LOW_BATTERY_WARNING,
    NUWAX_LOW_BATTERY_COMMIT,
} nuwax_low_battery_shutdown_action_t;

static inline bool nuwax_low_battery_sample_fresh(
    const nuwax_battery_service_snapshot_t *s, uint64_t now)
{
    return s && s->sample_available && s->valid && !s->stale &&
           s->power_valid && !s->power_stale && s->battery_present &&
           s->gauge_ready && s->last_error == ESP_OK &&
           s->sample_timestamp_ms != 0U && now >= s->sample_timestamp_ms &&
           now - s->sample_timestamp_ms <= NUWAX_LOW_BATTERY_FRESH_MS &&
           s->power_timestamp_ms != 0U && now >= s->power_timestamp_ms &&
           now - s->power_timestamp_ms <= NUWAX_LOW_BATTERY_FRESH_MS;
}

static inline bool nuwax_low_battery_shutdown_required(
    const nuwax_battery_service_snapshot_t *s, uint64_t now)
{
    /* Follow the validated percentage actually shown to the user, including
     * best-effort gauge profiles; never interpret unavailable telemetry as 0%.
     * A negative measured current invalidates a UI charging latch retained
     * across unplug. Merely having a USB cable is not charging evidence. */
    return nuwax_low_battery_sample_fresh(s, now) &&
           s->percent <= NUWAX_LOW_BATTERY_SHUTDOWN_PERCENT &&
           s->display_percent <= NUWAX_LOW_BATTERY_SHUTDOWN_PERCENT &&
           !(s->current_ma > 20 || (s->charging && s->current_ma >= 0));
}

static inline nuwax_low_battery_shutdown_action_t nuwax_low_battery_shutdown_update(
    nuwax_low_battery_shutdown_policy_t *state,
    const nuwax_battery_service_snapshot_t *sample, uint64_t now,
    bool protected_operation)
{
    if (!state) return NUWAX_LOW_BATTERY_IDLE;
    if (protected_operation || !nuwax_low_battery_shutdown_required(sample, now)) {
        *state = (nuwax_low_battery_shutdown_policy_t){0};
        return NUWAX_LOW_BATTERY_IDLE;
    }
    if (!state->warning || now < state->warning_since_ms ||
        sample->sample_timestamp_ms < state->last_sample_ms) {
        *state = (nuwax_low_battery_shutdown_policy_t){
            .warning = true, .warning_since_ms = now,
            .last_sample_ms = sample->sample_timestamp_ms, .samples = 1U,
        };
    } else if (sample->sample_timestamp_ms != state->last_sample_ms) {
        state->last_sample_ms = sample->sample_timestamp_ms;
        if (state->samples < 2U) state->samples++;
    }
    /* A new measurement AFTER the reminder deadline is mandatory. Repeated
     * owner polls of one cached 1% sample cannot power off the device. */
    if (state->samples >= 2U && now - state->warning_since_ms >=
            NUWAX_LOW_BATTERY_WARNING_MS && state->last_sample_ms >=
            state->warning_since_ms + NUWAX_LOW_BATTERY_WARNING_MS) {
        return NUWAX_LOW_BATTERY_COMMIT;
    }
    return NUWAX_LOW_BATTERY_WARNING;
}
