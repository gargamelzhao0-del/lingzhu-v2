#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Caller-owned totals, accessed synchronously by its sending task only.
 * Timings include scheduling delays inside each measured operation. */
typedef struct {
    uint32_t write_calls, short_writes, retries;
    uint32_t write_bytes, batch_calls, batch_frames, batch_max_frames;
    uint64_t admission_us, io_lock_us, write_us, writable_wait_us, encode_us;
    uint64_t write_max_us;
    uint32_t write_over_20ms;
    /* Subsets of synchronous TLS write wall time, including task preemption.
     * AEAD and socket BIO are disjoint; neither is a CPU-utilization counter. */
    uint64_t tls_aead_us, tls_bio_us, tls_aead_max_us, tls_bio_max_us;
    uint32_t tls_aead_calls, tls_bio_calls, tls_aead_bytes, tls_bio_bytes;
    uint32_t tls_profiled_calls, tls_profile_skipped;
    uint32_t tls_hw_calls, tls_hw_bytes, tls_hw_fallbacks, tls_hw_failures, tls_hw_verified;
    uint32_t tls_hw_key_setups, tls_hw_key_reuses;
    uint32_t tls_hw_staged_calls, tls_hw_staged_bytes;
    uint64_t tls_gcm_ctr_us, tls_gcm_hash_us, tls_gcm_sdk_update_us;
    uint32_t tls_gcm_fast_updates;
    /* Subsets of encode_us, including preemption. Never infer CPU-only cost.
     * Byte counts expose scalar fallbacks without logging in the hot loop. */
    uint64_t mask_us, mask_random_us;
    uint32_t mask_word_bytes, mask_scalar_bytes;
    uint32_t writable_wait_calls, writable_timeouts;
    uint64_t writable_wait_max_us;
    uint64_t writable_timeout_overrun_us, writable_timeout_overrun_max_us;
    uint64_t operation_wait_max_us;
    /* Opt-in for the isolated plain-TCP archive owner, never cloud realtime.
     * The callback must be nonblocking and cannot perform transport I/O. */
    bool bounded_progress;
    /* Formal SD Binary only. Complete an admitted TLS frame within a bounded
     * drain budget and wait for the requested socket direction on WANT_*. */
    bool shared_tls_archive;
    /* Per TLS write, independent from WS frame length; zero keeps defaults. */
    size_t tls_plaintext_limit;
    /* Optional formal sender queue admission. Zero leaves other callers alone. */
    uint32_t tcp_queue_ceiling_bytes;
    size_t tcp_psram_reserve_bytes;
    uint32_t tcp_admission_waits, tcp_event_waits, tcp_event_wakes, tcp_event_timeouts;
    uint32_t tls_record_splits;
    bool (*cancelled)(void);
    uint32_t tls_read_waits, tls_write_waits, tls_retry_yields;
    uint32_t precise_wait_calls, precise_wait_timeouts, precise_wait_fallbacks;
    uint32_t tls_timeout_bio_progress;
    uint32_t progress_recoveries;
    uint64_t no_progress_max_us;
    /* Outcome of the first nonblocking write AFTER a timed-out select.
     * Accepted bytes prove socket progress at retry time, not at timeout time.
     * Kept separate from transport retries/retransmissions. No policy input. */
    uint32_t timeout_next_progress, timeout_next_blocked, timeout_next_error;
    uint64_t timeout_next_bytes, timeout_resume_max_us;
} nuwax_ws_send_metrics_t;

static inline void nuwax_ws_metrics_mask_span(nuwax_ws_send_metrics_t *m,
    const uint8_t *dst, const uint8_t *src, size_t bytes)
{
    if (!m) return;
    const size_t words = (((uintptr_t)dst | (uintptr_t)src) & 3U) == 0U
        ? bytes & ~(size_t)3U : 0U;
    m->mask_word_bytes += (uint32_t)words;
    m->mask_scalar_bytes += (uint32_t)(bytes - words);
}

static inline void nuwax_ws_metrics_mask_parts(nuwax_ws_send_metrics_t *m,
    const uint8_t *dst, const uint8_t *prefix, size_t prefix_bytes,
    const uint8_t *tail, size_t offset, size_t bytes)
{
    if (!m) return;
    if (offset < prefix_bytes) {
        const size_t n = bytes < prefix_bytes - offset ? bytes : prefix_bytes - offset;
        nuwax_ws_metrics_mask_span(m, dst, prefix + offset, n);
        dst += n; offset += n; bytes -= n;
    }
    if (bytes) nuwax_ws_metrics_mask_span(m, dst, tail + offset - prefix_bytes, bytes);
}

static inline void nuwax_ws_metrics_after_timeout(nuwax_ws_send_metrics_t *m,
    int64_t result, bool retryable, uint64_t resume_us)
{
    if (!m) return;
    if (result > 0) { m->timeout_next_progress++; m->timeout_next_bytes += (uint64_t)result; }
    else if (retryable) m->timeout_next_blocked++;
    else m->timeout_next_error++;
    if (resume_us > m->timeout_resume_max_us) m->timeout_resume_max_us = resume_us;
}

static inline void nuwax_ws_metrics_writable_wait(nuwax_ws_send_metrics_t *metrics,
                                                 uint64_t elapsed_us, int timed_out)
{
    if (!metrics) return;
    metrics->writable_wait_calls++;
    metrics->writable_timeouts += timed_out ? 1U : 0U;
    metrics->writable_wait_us += elapsed_us;
    if (elapsed_us > metrics->writable_wait_max_us) metrics->writable_wait_max_us = elapsed_us;
    if (elapsed_us > metrics->operation_wait_max_us) metrics->operation_wait_max_us = elapsed_us;
}

/* Only a timed-out select has a known requested sleep duration. Excess wall
 * time includes RTOS tick rounding, scheduling and call overhead; it is NOT
 * a measurement of radio RTT or proof of a specific task starving TX. */
static inline void nuwax_ws_metrics_timeout_overrun(nuwax_ws_send_metrics_t *metrics,
                                                   int64_t finished, int64_t deadline)
{
    if (!metrics || finished <= deadline) return;
    const uint64_t excess = (uint64_t)(finished - deadline);
    metrics->writable_timeout_overrun_us += excess;
    if (excess > metrics->writable_timeout_overrun_max_us)
        metrics->writable_timeout_overrun_max_us = excess;
}
