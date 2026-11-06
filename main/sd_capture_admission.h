#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

/* One owner-task lease per controller. Releasing a meeting cannot release a
 * monitoring lease, or the separate OTA/shutdown gates. No transport pointer. */
typedef struct {
    atomic_uint mask;
    atomic_uint until_ms[2];
} sd_capture_admission_t;

static inline bool sd_capture_admission_held(const sd_capture_admission_t *s, uint32_t now)
{
    const unsigned mask = atomic_load_explicit(&s->mask, memory_order_acquire);
    for (unsigned i = 0U; i < 2U; ++i)
        if ((mask & (1U << i)) &&
            (int32_t)(now - atomic_load_explicit(&s->until_ms[i], memory_order_acquire)) < 0)
            return true;
    return false;
}

static inline void sd_capture_admission_hold(sd_capture_admission_t *s, unsigned mode, uint32_t until)
{
    atomic_store_explicit(&s->until_ms[mode], until, memory_order_release);
    atomic_fetch_or_explicit(&s->mask, 1U << mode, memory_order_release);
}

static inline void sd_capture_admission_release(sd_capture_admission_t *s, unsigned mode)
{
    if (mode < 2U) atomic_fetch_and_explicit(&s->mask, ~(1U << mode), memory_order_release);
}
