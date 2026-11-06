#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef ESP_PLATFORM
#include "replay_ui_schedule.h"
#endif

/* Below display flush (8), LVGL (7), wake (10), capture (12), Wi-Fi/lwIP.
 * Bounded slices let the priority-4 animation decoder and idle tasks run. */
#define OFFLINE_IO_TASK_PRIORITY 5U
/* Equal priority with the reader; writer-first SD admission prevents reader
 * starvation of capture writes. Both remain below LVGL (7) / flush (8). */
#define OFFLINE_WRITER_TASK_PRIORITY 6U
/* The dedicated reader must outrank the runnable sender on core 0, so an SD
 * completion can preempt encoding instead of waiting for a time slice. Keep
 * this priority through validate/copy/publish; a per-read 6 -> 5 transition
 * schedules the sender before publishing the data it needs. A bounded UI
 * handoff belongs to the sender, after its transport locks are released.
 * Reader publication proceeds straight to the next SD command. Below display flush (8), LVGL (7),
 * wake (10), capture (12), Wi-Fi/lwIP. It still blocks on
 * SD, retained ACK slots and capture pressure, and obeys the work budget. */
#define OFFLINE_REPLAY_READER_PRIORITY 6U

/* Select once per worker. Active capture leaves CPU1 free of wake inference;
 * idle replay uses CPU0 below UI/flush and reader, leaving wake on CPU1.
 * A phase change is handled between files by retiring/relaunching the owner. */
static inline unsigned offline_replay_formal_core(bool capture_running)
{
#if CONFIG_FREERTOS_UNICORE
    (void)capture_running; return 0U;
#else
    return capture_running ? 1U : 0U;
#endif
}
typedef struct { size_t bytes; int64_t since_us; } offline_io_budget_t;
static inline bool offline_io_budget_due(offline_io_budget_t *b, size_t bytes, int64_t now)
{
    b->bytes += bytes;
    return b->bytes >= 32768U || now - b->since_us >= 8000;
}
static inline void offline_io_budget_reset(offline_io_budget_t *b, int64_t now)
{
    b->bytes = 0U; b->since_us = now;
}

/* work_us is a wall-clock guard, NOT a CPU counter. UI progress clears that
 * guard on CPU0, but cannot erase the 64 KiB byte bound. Only same-core IDLE progress
 * proves that lower-priority ready tasks also got a chance to run. */
typedef struct {
    size_t bytes;
    uint64_t work_us;
    uint32_t yields, idle_epoch, ui_epoch, idle_credits, ui_credits;
    bool observed;
} offline_replay_budget_t;
/* A signalled wait can return immediately after its predicate check. Such a
 * short handoff must not erase accumulated CPU work and starve lower priorities. */
static inline bool offline_replay_wait_relaxes(uint64_t elapsed_us, uint32_t tick_us)
{
    return tick_us != 0U && elapsed_us >= tick_us;
}
static inline void offline_replay_budget_relax(offline_replay_budget_t *b)
{
    b->bytes = 0U; b->work_us = 0U;
}
static inline bool offline_replay_budget_account(offline_replay_budget_t *b,
    size_t bytes, uint64_t work_us, bool observed, uint32_t idle_epoch, uint32_t ui_epoch)
{
    bool ui_progress = false, idle_progress = false;
    if (observed) {
        if (b->observed && b->idle_epoch != idle_epoch) {
            offline_replay_budget_relax(b);
            b->idle_credits++;
            idle_progress = true;
        }
        ui_progress = b->observed && b->ui_epoch != ui_epoch;
        if (ui_progress) { b->work_us = 0U; b->ui_credits++; }
        b->idle_epoch = idle_epoch;
        b->ui_epoch = ui_epoch;
    }
    b->observed = observed;
    b->bytes += bytes;
    /* The just-finished operation includes that scheduler progress. Charging
     * its entire wall time again would immediately undo the idle credit.
     * Count its bytes, and retain the bounded guard for the next operation. */
    if (!ui_progress && !idle_progress) b->work_us += work_us;
    if (b->bytes < 65536U && b->work_us < 20000U) return false;
    offline_replay_budget_relax(b);
    b->yields++;
    return true;
}

static inline bool offline_replay_budget_due(offline_replay_budget_t *b, size_t bytes, uint64_t work_us)
{
    uint32_t idle_epoch = 0U, ui_epoch = 0U;
    bool observed = false;
#ifdef ESP_PLATFORM
    observed = nuwax_replay_schedule_snapshot(&idle_epoch, &ui_epoch);
#endif
    return offline_replay_budget_account(b, bytes, work_us, observed, idle_epoch, ui_epoch);
}
