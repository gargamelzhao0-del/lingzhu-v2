#include "replay_ui_schedule.h"
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_freertos_hooks.h"
#include "esp_timer.h"

static atomic_uint s_ui_epoch;
static atomic_uint s_help_state;
static atomic_uint s_help_generation;
static atomic_uint s_help_sequence;
static atomic_uint s_help_until_us;
static atomic_uint s_dma_loan_sequence, s_dma_loan_bytes, s_dma_loan_started_us;
/* Each core observes its own IDLE task. In particular, CPU0 display progress
 * cannot waive CPU1's fairness guard while TLS competes with wake inference.
 * Permanent storage avoids retaining any replay task pointer in an idle hook. */
typedef struct {
    atomic_uint epoch;
    /* 0: not attempted, 1: registering, 2: ready, 3: safe fallback. */
    atomic_uint state;
    /* 0: no waiter, 1: armed, 2: signal issued. */
    atomic_uint wait_state;
    StaticEventGroup_t storage;
    EventGroupHandle_t event;
} replay_idle_observer_t;
#if CONFIG_FREERTOS_UNICORE
#define REPLAY_OBSERVER_CORES 1U
#else
#define REPLAY_OBSERVER_CORES 2U
#endif
static replay_idle_observer_t s_idle_observers[REPLAY_OBSERVER_CORES];
#define REPLAY_IDLE_PROGRESS_BIT (1U << 0)
#define REPLAY_UI_HELP_CHANGED_BIT (1U << 1)

void nuwax_replay_dma_loan_begin(uint32_t bytes, uint32_t now_us)
{
    atomic_fetch_add_explicit(&s_dma_loan_sequence, 1U, memory_order_seq_cst);
    atomic_store_explicit(&s_dma_loan_started_us, now_us, memory_order_seq_cst);
    atomic_store_explicit(&s_dma_loan_bytes, bytes, memory_order_seq_cst);
    atomic_fetch_add_explicit(&s_dma_loan_sequence, 1U, memory_order_seq_cst);
}

void nuwax_replay_dma_loan_end(void)
{
    nuwax_replay_dma_loan_begin(0U, 0U);
}

uint32_t nuwax_replay_dma_loan_bytes(uint32_t now_us)
{
    const unsigned sequence = atomic_load_explicit(&s_dma_loan_sequence, memory_order_seq_cst);
    const unsigned bytes = atomic_load_explicit(&s_dma_loan_bytes, memory_order_seq_cst);
    const uint32_t started = atomic_load_explicit(&s_dma_loan_started_us, memory_order_seq_cst);
    /* No spin/retry in the high-priority UI. A torn/stale hint falls back to
     * the original pressure protection, including after the timer wraps. */
    if ((sequence & 1U) || sequence != atomic_load_explicit(&s_dma_loan_sequence, memory_order_seq_cst) ||
        bytes > 8192U || (uint32_t)(now_us - started) >= 50000U) return 0U;
    return bytes;
}

static bool replay_idle_hook(void)
{
    const unsigned core = (unsigned)xPortGetCoreID();
    if (core >= REPLAY_OBSERVER_CORES) return true;
    replay_idle_observer_t *observer = &s_idle_observers[core];
    atomic_fetch_add_explicit(&observer->epoch, 1U, memory_order_relaxed);
    unsigned armed = 1U;
    if (atomic_compare_exchange_strong_explicit(&observer->wait_state, &armed, 2U,
            memory_order_acq_rel, memory_order_acquire)) {
        /* Nonblocking task-context API. This can reschedule the sender, but
         * never waits, allocates, logs or acquires an application lock. */
        xEventGroupSetBits(observer->event, REPLAY_IDLE_PROGRESS_BIT);
        return false;
    }
    return true;
}

void nuwax_replay_schedule_init(void)
{
    for (unsigned core = 0U; core < REPLAY_OBSERVER_CORES; ++core) {
        replay_idle_observer_t *observer = &s_idle_observers[core];
        unsigned expected = 0U;
        if (atomic_compare_exchange_strong_explicit(&observer->state, &expected, 1U,
                memory_order_acq_rel, memory_order_acquire)) {
            observer->event = xEventGroupCreateStatic(&observer->storage);
            const bool ok = observer->event &&
                esp_register_freertos_idle_hook_for_cpu(replay_idle_hook, (int)core) == ESP_OK;
            atomic_store_explicit(&observer->state, ok ? 2U : 3U, memory_order_release);
        }
    }
}

bool nuwax_replay_schedule_yield(void)
{
    /* The existing one-tick fairness delay remains the fallback. Only the
     * serialized sender on this core waits after releasing transport locks.
     * A frame/UI completion is NOT sufficient to end this wait early. */
    const unsigned core = (unsigned)xPortGetCoreID();
    if (core >= REPLAY_OBSERVER_CORES || uxTaskPriorityGet(NULL) != 5U) {
        vTaskDelay(1U);
        return false;
    }
    replay_idle_observer_t *observer = &s_idle_observers[core];
    if (atomic_load_explicit(&observer->state, memory_order_acquire) != 2U || !observer->event) {
        vTaskDelay(1U);
        return false;
    }
    const TickType_t started = xTaskGetTickCount();
    const unsigned before = atomic_load_explicit(&observer->epoch, memory_order_acquire);
    xEventGroupClearBits(observer->event, REPLAY_IDLE_PROGRESS_BIT);
    unsigned vacant = 0U;
    if (!atomic_compare_exchange_strong_explicit(&observer->wait_state, &vacant, 1U,
            memory_order_acq_rel, memory_order_acquire)) {
        vTaskDelay(1U);
        return false;
    }
    /* Recheck after arming: IDLE may have run before the signal was armed.
     * A signal after this check is retained by the static event group. */
    if (before == atomic_load_explicit(&observer->epoch, memory_order_acquire))
        (void)xEventGroupWaitBits(observer->event, REPLAY_IDLE_PROGRESS_BIT,
                                 pdTRUE, pdFALSE, 1U);
    atomic_store_explicit(&observer->wait_state, 0U, memory_order_release);
    if (before != atomic_load_explicit(&observer->epoch, memory_order_acquire)) return true;
    /* A stale/spurious hint cannot forgive the fairness delay. Tick arithmetic
     * also bounds this fallback across the FreeRTOS tick counter wrap. */
    if ((TickType_t)(xTaskGetTickCount() - started) == 0U) vTaskDelay(1U);
    return false;
}

bool nuwax_replay_schedule_snapshot(uint32_t *idle_epoch, uint32_t *ui_epoch)
{
    const unsigned core = (unsigned)xPortGetCoreID();
    if (!idle_epoch || !ui_epoch || core >= REPLAY_OBSERVER_CORES) return false;
    replay_idle_observer_t *observer = &s_idle_observers[core];
    if (atomic_load_explicit(&observer->state, memory_order_acquire) != 2U) return false;
    *idle_epoch = atomic_load_explicit(&observer->epoch, memory_order_acquire);
    *ui_epoch = core == 0U ? atomic_load_explicit(&s_ui_epoch, memory_order_acquire) : 0U;
    return true;
}

void nuwax_replay_ui_set_help_state(nuwax_replay_ui_help_state_t state, uint32_t until_us)
{
    /* The UI owner serializes grants. Publish the new identity before help. */
    atomic_fetch_add_explicit(&s_help_sequence, 1U, memory_order_seq_cst);
    if (state == NUWAX_REPLAY_UI_HELP_REQUESTED &&
        atomic_load_explicit(&s_help_state, memory_order_acquire) != NUWAX_REPLAY_UI_HELP_REQUESTED) {
        atomic_fetch_add_explicit(&s_help_generation, 1U, memory_order_relaxed);
        atomic_store_explicit(&s_help_until_us, until_us, memory_order_seq_cst);
    }
    atomic_store_explicit(&s_help_state, state, memory_order_release);
    atomic_fetch_add_explicit(&s_help_sequence, 1U, memory_order_seq_cst);
    /* Permanent storage, never a retiring sender handle. The coherent state
     * is authoritative; this bit only wakes a blocked helper to recheck it. */
    replay_idle_observer_t *observer = &s_idle_observers[0];
    if (atomic_load_explicit(&observer->state, memory_order_acquire) == 2U &&
        observer->event && state != NUWAX_REPLAY_UI_HELP_REQUESTED)
        xEventGroupSetBits(observer->event, REPLAY_UI_HELP_CHANGED_BIT);
}

nuwax_replay_ui_help_state_t nuwax_replay_ui_help_snapshot(uint32_t *generation, uint32_t *until_us)
{
    if (!generation) return NUWAX_REPLAY_UI_HELP_NONE;
    const unsigned sequence = atomic_load_explicit(&s_help_sequence, memory_order_seq_cst);
    const unsigned before = atomic_load_explicit(&s_help_generation, memory_order_acquire);
    const unsigned state = atomic_load_explicit(&s_help_state, memory_order_acquire);
    const uint32_t deadline = atomic_load_explicit(&s_help_until_us, memory_order_seq_cst);
    *generation = before;
    if (until_us) *until_us = deadline;
    /* No spinning in a sender. An interrupted snapshot is unknown, never
     * counted as completion or used to waive the byte/IDLE guard. */
    if ((sequence & 1U) || sequence != atomic_load_explicit(&s_help_sequence, memory_order_seq_cst))
        return NUWAX_REPLAY_UI_HELP_NONE;
    return (nuwax_replay_ui_help_state_t)state;
}

void nuwax_replay_ui_frame_done(void)
{
    atomic_fetch_add_explicit(&s_ui_epoch, 1U, memory_order_release);
}

bool nuwax_replay_ui_handoff(uint32_t *served_generation)
{
    if (!served_generation || xPortGetCoreID() != 0 || uxTaskPriorityGet(NULL) != 5U) return false;
    uint32_t generation, until_us;
    if (nuwax_replay_ui_help_snapshot(&generation, &until_us) != NUWAX_REPLAY_UI_HELP_REQUESTED) return false;
    if (*served_generation == generation) return false;
    /* The lease is short relative to a 32-bit timer half-period. Preserve its
     * absolute deadline across batches and timer wrap; never start a new lease
     * from the sender's observation time or from a one-tick timeout. */
    if ((int32_t)(until_us - (uint32_t)esp_timer_get_time()) <= 0) {
        *served_generation = generation;
        return false;
    }
    /* Block, rather than remain runnable at the decoder's BASE priority.
     * Completion/cancellation wakes us early; no missing owner notification
     * can hold the sender beyond this one-tick wait. Keep the SD reader at 6.
     * A stale event never proves completion and cannot renew the timeout. */
    const TickType_t started = xTaskGetTickCount();
    EventGroupHandle_t event = atomic_load_explicit(&s_idle_observers[0].state, memory_order_acquire) == 2U
        ? s_idle_observers[0].event : NULL;
    if (!event) vTaskDelay(1U);
    else for (;;) {
        xEventGroupClearBits(event, REPLAY_UI_HELP_CHANGED_BIT);
        uint32_t current_generation;
        if (nuwax_replay_ui_help_snapshot(&current_generation, NULL) != NUWAX_REPLAY_UI_HELP_REQUESTED ||
            current_generation != generation) break;
        if ((int32_t)(until_us - (uint32_t)esp_timer_get_time()) <= 0) break;
        if ((TickType_t)(xTaskGetTickCount() - started) >= 1U) break;
        (void)xEventGroupWaitBits(event, REPLAY_UI_HELP_CHANGED_BIT,
                                 pdTRUE, pdFALSE, 1U);
    }
    uint32_t current_generation;
    const nuwax_replay_ui_help_state_t state = nuwax_replay_ui_help_snapshot(&current_generation, NULL);
    /* Unknown/torn snapshots and replacement grants never count as done.
     * A partial wait leaves this grant eligible on a later batch only while
     * its original deadline is still in the future. */
    if ((int32_t)(until_us - (uint32_t)esp_timer_get_time()) <= 0 ||
        (current_generation == generation && state != NUWAX_REPLAY_UI_HELP_NONE &&
         state != NUWAX_REPLAY_UI_HELP_REQUESTED))
        *served_generation = generation;
    return true;
}
