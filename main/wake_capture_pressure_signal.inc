/* Permanent static event storage: never expose a capture task/context to
 * replay and never delete a signal that a retiring reader may still await. */
static StaticEventGroup_t s_pressure_event_storage;
static EventGroupHandle_t s_pressure_event;
static atomic_bool s_pressure_event_ready;
#define WAKE_CAPTURE_DRAINED_BIT (1U << 0)

static void wake_capture_pressure_signal_init(void)
{
    portENTER_CRITICAL(&s_wake_capture_pressure_lock);
    if (!atomic_load_explicit(&s_pressure_event_ready, memory_order_relaxed)) {
        s_pressure_event=xEventGroupCreateStatic(&s_pressure_event_storage);
        atomic_store_explicit(&s_pressure_event_ready, s_pressure_event != NULL, memory_order_release);
    }
    portEXIT_CRITICAL(&s_wake_capture_pressure_lock);
}

static void wake_capture_signal_drained(void)
{
    if (atomic_load_explicit(&s_pressure_event_ready, memory_order_acquire))
        xEventGroupSetBits(s_pressure_event, WAKE_CAPTURE_DRAINED_BIT);
}

void nuwax_wake_word_wait_capture_headroom(uint32_t timeout_ms)
{
    TickType_t ticks=pdMS_TO_TICKS(timeout_ms);
    if (!ticks) ticks=1U;
    if (!atomic_load_explicit(&s_pressure_event_ready, memory_order_acquire)) {
        vTaskDelay(ticks); return;
    }
    /* Clear BEFORE rechecking the predicate. A drain after the check remains
     * signalled until wait; a drain before it is visible in the atomic age.
     * Multiple replay waiters can consume hints, so retain the old one-tick
     * maximum poll as a lost-hint/cancel bound. Only the caller admits work. */
    xEventGroupClearBits(s_pressure_event, WAKE_CAPTURE_DRAINED_BIT);
    if (nuwax_wake_word_capture_backlog_ms() <= 30U) return;
    (void)xEventGroupWaitBits(s_pressure_event, WAKE_CAPTURE_DRAINED_BIT,
                             pdTRUE, pdFALSE, ticks);
}
