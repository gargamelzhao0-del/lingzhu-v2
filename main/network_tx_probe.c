#ifndef NUWAX_NETWORK_PROBE_HOST_TEST
#include "sdkconfig.h"
#endif

#if CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE
#ifndef NUWAX_NETWORK_PROBE_HOST_TEST
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/tcp.h"
#include "lwip/tcpip.h"
#include "lwip/priv/tcp_priv.h"
#endif
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "network_tx_probe.h"

/* Only scalar copies leave TCP/IP context. No PCB, socket or pbuf is retained.
 * The netif counters include cloud/ARP traffic; TCP samples select port 8810.
 * Linker wrapping observes cross-object calls; internal same-object calls can
 * bypass it, so these are observed events, not a packet retransmission count. */
static atomic_uint s_netif_calls, s_netif_nomem, s_netif_other;
static atomic_uint s_tcp_calls, s_tcp_errors, s_zero_window, s_rto_samples;
static atomic_uint s_tcp_mem, s_tcp_route, s_tcp_other;
static atomic_uint s_cwnd, s_peer_window, s_send_free, s_queue, s_nrtx_peak;
static atomic_uint s_retry_budget, s_retry_next_ms;
static atomic_bool s_retry_queued;
/* These three values form one state transition. Individual atomics are not
 * enough: a timer callback can complete between a caller's check and claim.
 * No timer/lwIP/allocation/logging operation is allowed under this lock. */
static portMUX_TYPE s_retry_lock = portMUX_INITIALIZER_UNLOCKED;
static atomic_uint s_retry_attempts, s_retry_recovered, s_retry_exhausted;
static atomic_uint s_retry_post_failures, s_retry_stale;
static atomic_uint s_timer_state, s_timer_failures, s_timer_fires;
static atomic_uint s_timer_init_next_ms, s_timer_init_attempts;
static atomic_uint s_tcp_core0, s_tcp_core1;
static atomic_uint s_tcp_inputs, s_tcp_input_max_us, s_tcp_input_ambiguous;
static esp_timer_handle_t s_retry_timer;
/* One writer (TCP/IP), scalar atomic publication, bounded lock-free readers.
 * A connection number labels observed TCP endpoints, not a WebSocket epoch.
 * No PCB/segment pointer survives a callback. Samples can be stale: wrapped
 * tcp_output does not observe every ACK or internal same-object SDK call.
 * ACK bytes count observed TCP sequence-space advance (including SYN/FIN),
 * not audio payload and not the receiver's durable application ACK. */
#define REPLAY_TCP_SAMPLE_FIELDS(X) \
    X(connection) X(local_port) X(state) X(sample_ms) X(first_ms) \
    X(lastack) X(ack_observed_bytes) X(ack_advances) X(ack_progress_ms) \
    X(snd_nxt) X(snd_lbb) X(cwnd) X(peer_window) X(send_free) X(queue) \
    X(rto_ms) X(rtime_ms) X(timer_active) X(nrtx) X(in_rto) \
    X(queue_sample_ms) X(unacked_segments) X(unacked_bytes) \
    X(unsent_segments) X(unsent_bytes) X(queue_truncated) \
    X(ack_gap_peak_ms) X(ack_gap_at_ms) X(ack_gap_cwnd) X(ack_gap_nrtx) \
    X(ack_gap_in_rto) X(rto_entries) X(dupacks_peak)
#define SAMPLE_SCALAR(name) uint32_t name;
typedef struct { REPLAY_TCP_SAMPLE_FIELDS(SAMPLE_SCALAR) } replay_tcp_sample_t;
#undef SAMPLE_SCALAR
#define SAMPLE_ATOMIC(name) atomic_uint name;
static struct { atomic_uint version; REPLAY_TCP_SAMPLE_FIELDS(SAMPLE_ATOMIC) } s_sample;
#undef SAMPLE_ATOMIC
static replay_tcp_sample_t s_tcp_previous; /* TCP/IP context only. */
#define REPLAY_QUEUE_SAMPLE_INTERVAL_MS 100U
#define REPLAY_QUEUE_SCAN_LIMIT 128U
/* Only TCP/IP context accesses this flag. Never retain a PCB across calls. */
static bool s_in_retry;
#define REPLAY_OUTPUT_RETRIES 4U
#define REPLAY_OUTPUT_FIRST_RETRY_MS 5U
#define REPLAY_TIMER_INIT_RETRY_MS 1000U

static uint32_t replay_retry_now_ms(void)
{
    /* SDK sys_now() has 10 ms resolution at 100 Hz. Use the monotonic timer
     * for this 5 ms recovery only; leave lwIP's own clocks/timers unchanged. */
    return (uint32_t)((uint64_t)esp_timer_get_time() / 1000U);
}

static void replay_sample_queue(const struct tcp_seg *segment,
                               uint32_t *count, uint32_t *bytes, uint32_t *truncated)
{
    *count = *bytes = 0U;
    while (segment && *count < REPLAY_QUEUE_SCAN_LIMIT) {
        ++*count;
        *bytes += segment->len; /* TCP payload only; excludes SYN/FIN. */
        segment = segment->next;
    }
    if (segment) *truncated = 1U;
}

static void replay_sample_tcp(const struct tcp_pcb *pcb)
{
    const uint32_t now_ms = replay_retry_now_ms();
    replay_tcp_sample_t next = s_tcp_previous;
    /* Serial-number subtraction accepts normal 32-bit sequence wrap. This
     * observed identity may miss a same-port reuse with no intervening SYN;
     * local_port/lastack/sample time remain in the log for packet correlation. */
    const bool fresh = !next.connection || next.local_port != pcb->local_port ||
        ((pcb->state == SYN_SENT || pcb->state == SYN_RCVD) &&
         next.state != (uint32_t)pcb->state) ||
        (int32_t)(pcb->lastack - next.lastack) < 0;
    if (fresh) {
        const uint32_t connection = next.connection + 1U;
        next = (replay_tcp_sample_t){.connection = connection ? connection : 1U,
            .local_port = pcb->local_port, .first_ms = now_ms,
            .ack_progress_ms = now_ms};
    } else if ((int32_t)(pcb->lastack - next.lastack) > 0) {
        /* Only count a gap while data was outstanding. A quiet connection
         * between files is not a network stall. Snapshots below are the last
         * observed pre-ACK state, not a radio RTT estimate. */
        const uint32_t gap = now_ms - next.ack_progress_ms;
        if (next.snd_lbb != next.lastack && gap > next.ack_gap_peak_ms) {
            next.ack_gap_peak_ms = gap;
            next.ack_gap_at_ms = now_ms;
            next.ack_gap_cwnd = next.cwnd;
            next.ack_gap_nrtx = next.nrtx;
            next.ack_gap_in_rto = next.in_rto;
        }
        next.ack_observed_bytes += pcb->lastack - next.lastack;
        ++next.ack_advances;
        next.ack_progress_ms = now_ms;
    } else if (next.snd_lbb == next.lastack) {
        next.ack_progress_ms = now_ms;
    }
    const bool in_rto = (pcb->flags & TF_RTO) != 0U;
    if (in_rto && !next.in_rto) ++next.rto_entries;
    if (pcb->dupacks > next.dupacks_peak) next.dupacks_peak = pcb->dupacks;
    if (fresh || (in_rto && !next.in_rto) ||
        (uint32_t)(now_ms - next.queue_sample_ms) >= REPLAY_QUEUE_SAMPLE_INTERVAL_MS) {
        next.queue_truncated = 0U;
        replay_sample_queue(pcb->unacked, &next.unacked_segments, &next.unacked_bytes,
                            &next.queue_truncated);
        replay_sample_queue(pcb->unsent, &next.unsent_segments, &next.unsent_bytes,
                            &next.queue_truncated);
        next.queue_sample_ms = now_ms;
    }
    next.state = (uint32_t)pcb->state;
    next.sample_ms = now_ms;
    next.lastack = pcb->lastack;
    next.snd_nxt = pcb->snd_nxt;
    next.snd_lbb = pcb->snd_lbb;
    next.cwnd = pcb->cwnd;
    next.peer_window = pcb->snd_wnd;
    next.send_free = pcb->snd_buf;
    next.queue = pcb->snd_queuelen;
    next.rto_ms = pcb->rto > 0 ? (uint32_t)pcb->rto * TCP_SLOW_INTERVAL : 0U;
    next.timer_active = pcb->rtime >= 0;
    next.rtime_ms = next.timer_active ? (uint32_t)pcb->rtime * TCP_SLOW_INTERVAL : 0U;
    next.nrtx = pcb->nrtx;
    next.in_rto = in_rto;
    s_tcp_previous = next;
    atomic_fetch_add_explicit(&s_sample.version, 1U, memory_order_acq_rel);
#define PUBLISH_SAMPLE(name) atomic_store_explicit(&s_sample.name, next.name, memory_order_relaxed);
    REPLAY_TCP_SAMPLE_FIELDS(PUBLISH_SAMPLE)
#undef PUBLISH_SAMPLE
    atomic_fetch_add_explicit(&s_sample.version, 1U, memory_order_release);
}

static bool replay_sample_read(replay_tcp_sample_t *sample)
{
    for (unsigned attempt = 0U; attempt < 3U; ++attempt) {
        const unsigned version = atomic_load_explicit(&s_sample.version, memory_order_acquire);
        if (!version || (version & 1U)) continue;
#define READ_SAMPLE(name) sample->name = atomic_load_explicit(&s_sample.name, memory_order_relaxed);
        REPLAY_TCP_SAMPLE_FIELDS(READ_SAMPLE)
#undef READ_SAMPLE
        atomic_thread_fence(memory_order_acquire);
        if (version == atomic_load_explicit(&s_sample.version, memory_order_relaxed)) return true;
    }
    return false; /* No busy wait if TCP/IP is preempted while publishing. */
}

/* One permanent timer object; only pending recovery arms it. It carries no
 * socket/PCB/payload and never calls lwIP outside TCP/IP context. An already
 * armed one-shot is allowed to fire early, recheck the deadline and re-arm.
 * No periodic idle wakeups or per-firing timer allocation. The nonblocking
 * TCP/IP message allocation can fail and consumes the same retry budget. */
static void replay_retry_schedule(void)
{
    if (atomic_load_explicit(&s_timer_state, memory_order_acquire) != 2U) return;
    portENTER_CRITICAL(&s_retry_lock);
    const bool pending = atomic_load_explicit(&s_retry_budget, memory_order_relaxed) &&
        !atomic_load_explicit(&s_retry_queued, memory_order_relaxed);
    const uint32_t next_ms = atomic_load_explicit(&s_retry_next_ms, memory_order_relaxed);
    portEXIT_CRITICAL(&s_retry_lock);
    if (!pending) return;
    const int32_t remaining = (int32_t)(next_ms - replay_retry_now_ms());
    const uint64_t delay_us = (remaining > 0 ? (uint32_t)remaining : 1U) * 1000ULL;
    const esp_err_t err = esp_timer_start_once(s_retry_timer, delay_us);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        atomic_fetch_add_explicit(&s_timer_failures, 1U, memory_order_relaxed);
}

static void replay_retry_timer(void *unused)
{
    (void)unused;
    atomic_fetch_add_explicit(&s_timer_fires, 1U, memory_order_relaxed);
    nuwax_network_tx_retry_poll();
    replay_retry_schedule();
}

void nuwax_network_tx_probe_init(void)
{
    unsigned expected = atomic_load_explicit(&s_timer_state, memory_order_acquire);
    if (expected != 0U && expected != 3U) return;
    if (expected == 3U && (int32_t)(replay_retry_now_ms() -
        atomic_load_explicit(&s_timer_init_next_ms, memory_order_relaxed)) < 0) return;
    if (!atomic_compare_exchange_strong_explicit(&s_timer_state, &expected, 1U,
            memory_order_acq_rel, memory_order_acquire)) return;
    /* Another failed initializer may have completed before our CAS (3->1->3).
     * Recheck its new cooldown while we exclusively own initialization. */
    if (expected == 3U && (int32_t)(replay_retry_now_ms() -
        atomic_load_explicit(&s_timer_init_next_ms, memory_order_relaxed)) < 0) {
        atomic_store_explicit(&s_timer_state, 3U, memory_order_release);
        return;
    }
    const esp_timer_create_args_t args = {
        .callback = replay_retry_timer, .dispatch_method = ESP_TIMER_TASK,
        .name = "sd_tcp_retry",
    };
    atomic_fetch_add_explicit(&s_timer_init_attempts, 1U, memory_order_relaxed);
    const esp_err_t err = esp_timer_create(&args, &s_retry_timer);
    if (err != ESP_OK) {
        atomic_store_explicit(&s_timer_init_next_ms,
            replay_retry_now_ms() + REPLAY_TIMER_INIT_RETRY_MS, memory_order_relaxed);
        atomic_store_explicit(&s_timer_state, 3U, memory_order_release);
        atomic_fetch_add_explicit(&s_timer_failures, 1U, memory_order_relaxed);
        ESP_LOGW("net_tx_probe", "retry timer unavailable err=%d; polling retained, init retry after %ums on activity",
                 err, REPLAY_TIMER_INIT_RETRY_MS);
    } else {
        atomic_store_explicit(&s_timer_state, 2U, memory_order_release);
    }
    replay_retry_schedule();
}

esp_err_t __real_esp_netif_transmit_wrap(esp_netif_t *, void *, size_t, void *);
err_t __real_tcp_output(struct tcp_pcb *);
void __real_tcp_input(struct pbuf *, struct netif *);

/* Observe AFTER SDK validation/processing, never trust a raw ACK number.
 * tcp_input may free the pbuf and PCB: copy only the ports before calling it,
 * and find a currently active, unambiguous endpoint afterwards. No payload,
 * PCB or netif pointer is retained; no allocation, log or wait in TCP/IP.
 * This closes the outgoing-only probe's blind spot while TX is blocked. */
void __wrap_tcp_input(struct pbuf *p, struct netif *inp)
{
    uint16_t local_port = 0U;
    if (p && p->len >= 20U) {
        const uint8_t *header = p->payload;
        if ((((unsigned)header[0] << 8) | header[1]) == 8810U)
            local_port = (uint16_t)(((unsigned)header[2] << 8) | header[3]);
    }
    const int64_t started = local_port ? esp_timer_get_time() : 0;
    __real_tcp_input(p, inp);
    if (!local_port) return;
    const uint32_t elapsed = (uint32_t)(esp_timer_get_time() - started);
    atomic_fetch_add_explicit(&s_tcp_inputs, 1U, memory_order_relaxed);
    if (elapsed > atomic_load_explicit(&s_tcp_input_max_us, memory_order_relaxed))
        atomic_store_explicit(&s_tcp_input_max_us, elapsed, memory_order_relaxed);
    struct tcp_pcb *match = NULL, *pcb = tcp_active_pcbs;
    unsigned visited = 0U;
    for (; pcb && visited < 32U; pcb = pcb->next, ++visited) {
        if (pcb->remote_port != 8810U || pcb->local_port != local_port) continue;
        if (match) {
            atomic_fetch_add_explicit(&s_tcp_input_ambiguous, 1U, memory_order_relaxed);
            return;
        }
        match = pcb;
    }
    if (pcb) { /* A partial search cannot establish endpoint uniqueness. */
        atomic_fetch_add_explicit(&s_tcp_input_ambiguous, 1U, memory_order_relaxed);
        return;
    }
    if (match) replay_sample_tcp(match);
}

esp_err_t __wrap_esp_netif_transmit_wrap(esp_netif_t *netif, void *data,
                                      size_t len, void *netstack_buf)
{
    const esp_err_t err = __real_esp_netif_transmit_wrap(netif, data, len, netstack_buf);
    atomic_fetch_add_explicit(&s_netif_calls, 1U, memory_order_relaxed);
    if (err == ESP_ERR_NO_MEM)
        atomic_fetch_add_explicit(&s_netif_nomem, 1U, memory_order_relaxed);
    else if (err != ESP_OK)
        atomic_fetch_add_explicit(&s_netif_other, 1U, memory_order_relaxed);
    return err;
}

err_t __wrap_tcp_output(struct tcp_pcb *pcb)
{
    const err_t err = __real_tcp_output(pcb);
    /* tcp_output does not free the PCB; caller still owns TCP/IP context. */
    if (pcb && pcb->remote_port == 8810U) {
        atomic_fetch_add_explicit(xPortGetCoreID() == 0 ? &s_tcp_core0 : &s_tcp_core1,
                                  1U, memory_order_relaxed);
        atomic_fetch_add_explicit(&s_tcp_calls, 1U, memory_order_relaxed);
        if (err != ERR_OK) {
            atomic_fetch_add_explicit(&s_tcp_errors, 1U, memory_order_relaxed);
            atomic_fetch_add_explicit(err == ERR_MEM ? &s_tcp_mem :
                err == ERR_RTE ? &s_tcp_route : &s_tcp_other, 1U, memory_order_relaxed);
        }
        if (err == ERR_MEM && !s_in_retry) {
            const uint32_t next_ms = replay_retry_now_ms() + REPLAY_OUTPUT_FIRST_RETRY_MS;
            bool armed = false;
            portENTER_CRITICAL(&s_retry_lock);
            if (!atomic_load_explicit(&s_retry_budget, memory_order_relaxed) &&
                !atomic_load_explicit(&s_retry_queued, memory_order_relaxed)) {
                atomic_store_explicit(&s_retry_next_ms, next_ms, memory_order_relaxed);
                atomic_store_explicit(&s_retry_budget, REPLAY_OUTPUT_RETRIES, memory_order_release);
                armed = true;
            }
            portEXIT_CRITICAL(&s_retry_lock);
            if (armed) replay_retry_schedule();
        }
        if (pcb->snd_wnd == 0U) atomic_fetch_add_explicit(&s_zero_window, 1U, memory_order_relaxed);
        if (pcb->flags & TF_RTO) atomic_fetch_add_explicit(&s_rto_samples, 1U, memory_order_relaxed);
        atomic_store_explicit(&s_cwnd, pcb->cwnd, memory_order_relaxed);
        atomic_store_explicit(&s_peer_window, pcb->snd_wnd, memory_order_relaxed);
        atomic_store_explicit(&s_send_free, pcb->snd_buf, memory_order_relaxed);
        atomic_store_explicit(&s_queue, pcb->snd_queuelen, memory_order_relaxed);
        unsigned peak = atomic_load_explicit(&s_nrtx_peak, memory_order_relaxed);
        while (peak < pcb->nrtx && !atomic_compare_exchange_weak_explicit(
                   &s_nrtx_peak, &peak, pcb->nrtx, memory_order_relaxed, memory_order_relaxed)) {}
        replay_sample_tcp(pcb);
    }
    return err;
}

/* A failed link output remains on lwIP's unsent list with TF_NAGLEMEMERR.
 * Retry it in TCP/IP context after the driver has had scheduling time. This
 * uses the same condition as SDK tcp_txnow(), restricted to the PC gateway.
 * No payload is re-enqueued, no sequence/cwnd/RTO state is changed, and a
 * closed PCB cannot survive in our state. At most one callback is queued. */
static void replay_output_retry(void *unused)
{
    (void)unused;
    /* The queued claim stays owned until every field of this round is
     * updated. New ERR_MEM events cannot arm a round during its completion. */
    portENTER_CRITICAL(&s_retry_lock);
    unsigned remaining = atomic_load_explicit(&s_retry_budget, memory_order_acquire);
    portEXIT_CRITICAL(&s_retry_lock);
    bool attempted = false;
    if (remaining) {
        --remaining;
        s_in_retry = true;
        for (struct tcp_pcb *pcb = tcp_active_pcbs; pcb; pcb = pcb->next) {
            if (pcb->remote_port != 8810U || pcb->state != ESTABLISHED ||
                !(pcb->flags & TF_NAGLEMEMERR) || !pcb->unsent) continue;
            attempted = true;
            atomic_fetch_add_explicit(&s_retry_attempts, 1U, memory_order_relaxed);
            const err_t err = __wrap_tcp_output(pcb);
            if (err != ERR_MEM) {
                if (err == ERR_OK)
                    atomic_fetch_add_explicit(&s_retry_recovered, 1U, memory_order_relaxed);
                remaining = 0U;
            } else if (!remaining) {
                atomic_fetch_add_explicit(&s_retry_exhausted, 1U, memory_order_relaxed);
            }
            break; /* One bounded output call per callback, no busy retry. */
        }
        s_in_retry = false;
    }
    if (!attempted) {
        remaining = 0U;
        atomic_fetch_add_explicit(&s_retry_stale, 1U, memory_order_relaxed);
    }
    const uint32_t next_ms = replay_retry_now_ms() +
        (remaining ? REPLAY_OUTPUT_FIRST_RETRY_MS << (REPLAY_OUTPUT_RETRIES - remaining) : REPLAY_OUTPUT_FIRST_RETRY_MS);
    portENTER_CRITICAL(&s_retry_lock);
    atomic_store_explicit(&s_retry_next_ms, next_ms, memory_order_relaxed);
    atomic_store_explicit(&s_retry_budget, remaining, memory_order_release);
    atomic_store_explicit(&s_retry_queued, false, memory_order_release);
    portEXIT_CRITICAL(&s_retry_lock);
    replay_retry_schedule();
}

void nuwax_network_tx_retry_poll(void)
{
    /* A failed timer allocation is retried only on connection/upload activity,
     * at most once per second. No allocation from TCP/IP output callbacks. */
    if (atomic_load_explicit(&s_timer_state, memory_order_acquire) == 3U)
        nuwax_network_tx_probe_init();
    const uint32_t now_ms = replay_retry_now_ms();
    bool claimed = false;
    portENTER_CRITICAL(&s_retry_lock);
    if (atomic_load_explicit(&s_retry_budget, memory_order_relaxed) &&
        !atomic_load_explicit(&s_retry_queued, memory_order_relaxed) &&
        (int32_t)(now_ms - atomic_load_explicit(&s_retry_next_ms, memory_order_relaxed)) >= 0) {
        atomic_store_explicit(&s_retry_queued, true, memory_order_release);
        claimed = true;
    }
    portEXIT_CRITICAL(&s_retry_lock);
    if (!claimed) return;
    /* Nonblocking: a full mailbox/allocation failure keeps the original
     * TCP timer recovery. No stack pointer or transport lock enters lwIP. */
    if (tcpip_try_callback(replay_output_retry, NULL) != ERR_OK) {
        atomic_fetch_add_explicit(&s_retry_post_failures, 1U, memory_order_relaxed);
        /* A permanently full mailbox must not leave an autonomous timer
         * waking forever after the uploader has already stopped. No core
         * callback owns this round when posting fails. */
        const uint32_t next_ms = replay_retry_now_ms() + 100U;
        portENTER_CRITICAL(&s_retry_lock);
        const unsigned remaining = atomic_load_explicit(&s_retry_budget, memory_order_relaxed);
        atomic_store_explicit(&s_retry_budget, remaining ? remaining - 1U : 0U, memory_order_release);
        atomic_store_explicit(&s_retry_next_ms, next_ms, memory_order_relaxed);
        atomic_store_explicit(&s_retry_queued, false, memory_order_release);
        portEXIT_CRITICAL(&s_retry_lock);
        replay_retry_schedule();
    }
}

void nuwax_network_tx_probe_log(void)
{
#define READ(field) ((unsigned long)atomic_load_explicit(&(field), memory_order_relaxed))
    ESP_LOGI("net_tx_probe", "cumulative netif_calls=%lu netif_nomem=%lu netif_other=%lu replay_output_calls=%lu replay_output_errors=%lu zero_window_samples=%lu rto_samples=%lu nrtx_peak=%lu last_cwnd=%lu last_peer_window=%lu last_send_free=%lu last_queue=%lu",
             READ(s_netif_calls), READ(s_netif_nomem), READ(s_netif_other),
             READ(s_tcp_calls), READ(s_tcp_errors), READ(s_zero_window),
             READ(s_rto_samples), READ(s_nrtx_peak), READ(s_cwnd),
             READ(s_peer_window), READ(s_send_free), READ(s_queue));
    ESP_LOGI("net_tx_probe", "recovery tcp_mem=%lu tcp_route=%lu tcp_other=%lu retry_attempts=%lu retry_recovered=%lu retry_exhausted=%lu retry_post_failures=%lu retry_stale=%lu retry_remaining=%lu",
             READ(s_tcp_mem), READ(s_tcp_route), READ(s_tcp_other),
             READ(s_retry_attempts), READ(s_retry_recovered), READ(s_retry_exhausted),
             READ(s_retry_post_failures), READ(s_retry_stale), READ(s_retry_budget));
    ESP_LOGI("net_tx_probe", "scheduler core0=%lu core1=%lu timer_ready=%lu timer_fires=%lu timer_failures=%lu timer_init_attempts=%lu retry_first_ms=%u state=locked",
             READ(s_tcp_core0), READ(s_tcp_core1),
             READ(s_timer_state) == 2U ? 1UL : 0UL, READ(s_timer_fires),
             READ(s_timer_failures), READ(s_timer_init_attempts), REPLAY_OUTPUT_FIRST_RETRY_MS);
    replay_tcp_sample_t sample;
    if (replay_sample_read(&sample)) {
        const uint32_t now_ms = replay_retry_now_ms();
        ESP_LOGI("net_tx_probe", "tcp sample_ms=%lu age_ms=%lu observed_conn=%lu local_port=%lu state=%lu first_ms=%lu lastack=%lu ack_observed_bytes=%lu ack_advances=%lu ack_age_ms=%lu snd_nxt=%lu snd_lbb=%lu",
            (unsigned long)sample.sample_ms, (unsigned long)(now_ms - sample.sample_ms),
            (unsigned long)sample.connection, (unsigned long)sample.local_port,
            (unsigned long)sample.state, (unsigned long)sample.first_ms,
            (unsigned long)sample.lastack, (unsigned long)sample.ack_observed_bytes,
            (unsigned long)sample.ack_advances, (unsigned long)(now_ms - sample.ack_progress_ms),
            (unsigned long)sample.snd_nxt, (unsigned long)sample.snd_lbb);
        ESP_LOGI("net_tx_probe", "tcp queues observed_conn=%lu queue_age_ms=%lu unacked_segments=%lu unacked_bytes=%lu unsent_segments=%lu unsent_bytes=%lu truncated=%lu rto_ms=%lu rtime_ms=%lu timer_active=%lu nrtx=%lu in_rto=%lu cwnd=%lu peer_window=%lu send_free=%lu queue=%lu",
            (unsigned long)sample.connection, (unsigned long)(now_ms - sample.queue_sample_ms),
            (unsigned long)sample.unacked_segments, (unsigned long)sample.unacked_bytes,
            (unsigned long)sample.unsent_segments, (unsigned long)sample.unsent_bytes,
            (unsigned long)sample.queue_truncated, (unsigned long)sample.rto_ms,
            (unsigned long)sample.rtime_ms, (unsigned long)sample.timer_active,
            (unsigned long)sample.nrtx, (unsigned long)sample.in_rto,
            (unsigned long)sample.cwnd, (unsigned long)sample.peer_window,
            (unsigned long)sample.send_free, (unsigned long)sample.queue);
        ESP_LOGI("net_tx_probe", "ack trace revision=148 conn=%lu port=%lu inputs=%lu input_max_us=%lu ambiguous=%lu gap_peak_ms=%lu gap_at_ms=%lu prior_cwnd=%lu prior_nrtx=%lu prior_rto=%lu rto_entries=%lu dupacks_peak=%lu",
            (unsigned long)sample.connection, (unsigned long)sample.local_port,
            READ(s_tcp_inputs), READ(s_tcp_input_max_us), READ(s_tcp_input_ambiguous),
            (unsigned long)sample.ack_gap_peak_ms, (unsigned long)sample.ack_gap_at_ms,
            (unsigned long)sample.ack_gap_cwnd, (unsigned long)sample.ack_gap_nrtx,
            (unsigned long)sample.ack_gap_in_rto, (unsigned long)sample.rto_entries,
            (unsigned long)sample.dupacks_peak);
    } else ESP_LOGI("net_tx_probe", "tcp snapshot unavailable=no_sample_or_publish_busy");
    /* Ordinary sender context, after payload I/O. Queries are observational,
     * neither change Wi-Fi policy nor report SSID/BSSID/credentials. */
    wifi_ap_record_t ap = {0};
    wifi_ps_type_t ps = WIFI_PS_NONE;
    const esp_err_t ap_err = esp_wifi_sta_get_ap_info(&ap);
    const esp_err_t ps_err = esp_wifi_get_ps(&ps);
    ESP_LOGI("net_tx_probe", "radio sample_ms=%lu valid=%u rssi=%d channel=%u ps_valid=%u ps=%u",
        (unsigned long)replay_retry_now_ms(), ap_err == ESP_OK, ap.rssi,
        (unsigned)ap.primary, ps_err == ESP_OK, (unsigned)ps);
#undef READ
}
#endif
