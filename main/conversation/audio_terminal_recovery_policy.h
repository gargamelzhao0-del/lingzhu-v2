#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    NUWAX_AUDIO_TERMINAL_RECOVERY_ACTION_IDLE = 0,
    NUWAX_AUDIO_TERMINAL_RECOVERY_ACTION_RETRY_END,
    NUWAX_AUDIO_TERMINAL_RECOVERY_ACTION_RETRY_STOP,
    NUWAX_AUDIO_TERMINAL_RECOVERY_ACTION_COMPLETE,
} nuwax_audio_terminal_recovery_action_t;

#define NUWAX_AUDIO_TERMINAL_RECOVERY_MAX_FAILURES 8U
#define NUWAX_AUDIO_TERMINAL_RECOVERY_MAX_AGE_MS 30000U

static inline nuwax_audio_terminal_recovery_action_t
nuwax_audio_terminal_recovery_action(bool pending,
                                     bool epoch_matches,
                                     bool connected,
                                     bool attempt_due,
                                     bool end_committed) {
    if (!pending) {
        return NUWAX_AUDIO_TERMINAL_RECOVERY_ACTION_IDLE;
    }
    if (!epoch_matches || !connected) {
        return NUWAX_AUDIO_TERMINAL_RECOVERY_ACTION_COMPLETE;
    }
    if (!attempt_due) {
        return NUWAX_AUDIO_TERMINAL_RECOVERY_ACTION_IDLE;
    }
    return end_committed
               ? NUWAX_AUDIO_TERMINAL_RECOVERY_ACTION_RETRY_STOP
               : NUWAX_AUDIO_TERMINAL_RECOVERY_ACTION_RETRY_END;
}

static inline uint32_t nuwax_audio_terminal_recovery_backoff_ms(
    uint32_t failures) {
    if (failures >= 5U) {
        return 2000U;
    }
    const uint32_t value = 100U << failures;
    return value > 2000U ? 2000U : value;
}

static inline bool nuwax_audio_terminal_recovery_should_quarantine(
    uint32_t failures,
    uint32_t age_ms) {
    return failures >= NUWAX_AUDIO_TERMINAL_RECOVERY_MAX_FAILURES ||
           age_ms >= NUWAX_AUDIO_TERMINAL_RECOVERY_MAX_AGE_MS;
}

static inline uint32_t nuwax_audio_terminal_recovery_wait_ms(
    int64_t next_attempt_us,
    int64_t deadline_us,
    int64_t now_us,
    uint32_t max_wait_ms) {
    if (max_wait_ms == 0U || deadline_us <= now_us ||
        next_attempt_us <= now_us) {
        return 0U;
    }
    int64_t wait_us = next_attempt_us - now_us;
    const int64_t deadline_wait_us = deadline_us - now_us;
    if (deadline_wait_us < wait_us) {
        wait_us = deadline_wait_us;
    }
    uint64_t wait_ms = ((uint64_t)wait_us + 999ULL) / 1000ULL;
    if (wait_ms > max_wait_ms) {
        wait_ms = max_wait_ms;
    }
    return (uint32_t)wait_ms;
}
