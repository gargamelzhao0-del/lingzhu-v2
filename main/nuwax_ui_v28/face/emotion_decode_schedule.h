#pragma once

#include <stdbool.h>
#include <stdint.h>

/* One frame of help per display period. The LVGL owner alone changes the
 * priority; the worker only reads a hint and stops its batch after one frame.
 * Expiry is observed on an owner tick, not a hard real-time timer guarantee. */
#define EMOTION_DECODE_HELP_PRIORITY 5U
/* Make the next-frame request eligible across a full 40 FPS period. Waiting
 * another 5 ms after consuming the last ready frame only grows an empty queue.
 * Keep the same 12 ms lease and one-frame/period limit. */
#define EMOTION_DECODE_HELP_LEAD_US 25000U
#define EMOTION_DECODE_HELP_LEASE_US 12000U

typedef enum {
    EMOTION_DECODE_HELP_NONE = 0,
    EMOTION_DECODE_HELP_GRANT,
    EMOTION_DECODE_HELP_READY,
    EMOTION_DECODE_HELP_EXPIRED,
    EMOTION_DECODE_HELP_CANCEL,
} emotion_decode_help_action_t;

typedef struct {
    bool active;
    uint32_t generation;
    int64_t started_us, until_us, next_grant_us;
} emotion_decode_help_t;

static inline emotion_decode_help_action_t emotion_decode_help_update(
    emotion_decode_help_t *p, int64_t now_us, int64_t frame_due_us,
    uint32_t period_us, uint32_t generation, bool eligible,
    bool worker_busy, uint32_t ready_frames)
{
    if (p->active) {
        emotion_decode_help_action_t result = EMOTION_DECODE_HELP_NONE;
        if (!eligible || generation != p->generation)
            result = EMOTION_DECODE_HELP_CANCEL;
        else if (ready_frames != 0U)
            result = EMOTION_DECODE_HELP_READY;
        else if (!worker_busy)
            result = EMOTION_DECODE_HELP_CANCEL;
        else if (now_us >= p->until_us)
            result = EMOTION_DECODE_HELP_EXPIRED;
        if (result != EMOTION_DECODE_HELP_NONE) p->active = false;
        return result;
    }
    if (!eligible || !worker_busy || ready_frames != 0U || period_us == 0U ||
        now_us < p->next_grant_us ||
        frame_due_us - now_us > EMOTION_DECODE_HELP_LEAD_US)
        return EMOTION_DECODE_HELP_NONE;
    p->active = true;
    p->generation = generation;
    p->started_us = now_us;
    p->until_us = now_us + EMOTION_DECODE_HELP_LEASE_US;
    p->next_grant_us = now_us + period_us;
    return EMOTION_DECODE_HELP_GRANT;
}
