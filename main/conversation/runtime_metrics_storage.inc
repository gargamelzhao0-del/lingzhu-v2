/* Optional, task-only diagnostics. Do not consume DMA-capable internal RAM
 * for the 6 KiB histogram. The pointer stays stable for the runtime lifetime. */
static bool metrics_runtime_prepare(uint64_t round, uint64_t now_us)
{
    if (!s_conversation_runtime_metrics) {
        s_conversation_runtime_metrics = heap_caps_calloc(
            1U, sizeof(*s_conversation_runtime_metrics), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_conversation_runtime_metrics) return false;
    }
    return s_conversation_runtime_metrics_ready
        ? nuwax_conversation_metrics_reset(s_conversation_runtime_metrics, round, now_us)
        : nuwax_conversation_metrics_init(s_conversation_runtime_metrics, round, now_us);
}
