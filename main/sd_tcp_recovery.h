#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Optional, file-scoped recovery of SDK output rejected with ERR_MEM.
 * Retains TCP-owned packets; never re-enqueues application payload. */
typedef struct {
    uint32_t outputs, output_nomem, retries, recovered, exhausted;
    uint32_t post_failures, stale, zero_window, rto_samples, nrtx_peak;
    uint32_t queue_peak, send_free_min, cwnd_min;
    uint32_t peer_window_min, peer_window_max, queued_bytes_peak, ack_events;
    uint32_t cwnd_max, inflight_peak;
    uint32_t rtt_samples, rtt_min_us, rtt_max_us;
    uint64_t rtt_total_us, acked_bytes;
} nuwax_sd_tcp_recovery_metrics_t;
typedef struct {
    bool valid;
    uint32_t sequence, queued_bytes, send_free, peer_window, cwnd;
} nuwax_sd_tcp_snapshot_t;
void nuwax_sd_tcp_recovery_prepare(void);
/* Caller holds the transport's I/O lock and has checked its connection epoch.
 * Endpoints and counters are copied; no socket/client/PCB pointer retained. */
uint32_t nuwax_sd_tcp_recovery_begin(uintptr_t owner, uint32_t epoch, int sock);
void nuwax_sd_tcp_recovery_end(uint32_t token, nuwax_sd_tcp_recovery_metrics_t *metrics);
void nuwax_sd_tcp_recovery_close(uintptr_t owner, uint32_t epoch);
/* Snapshot copies only. The static wake semaphore never borrows task/client,
 * PCB or stack storage. A stale/spurious wake cannot establish progress. */
bool nuwax_sd_tcp_snapshot(uintptr_t owner, uint32_t epoch, nuwax_sd_tcp_snapshot_t *state);
/* 1 = event sequence changed; 0 = bounded timeout; -1 = inactive/fallback. */
int nuwax_sd_tcp_wait_progress(uintptr_t owner, uint32_t epoch,
                              uint32_t observed_sequence, uint32_t timeout_ms);
