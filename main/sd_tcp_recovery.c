#ifndef NUWAX_SD_TCP_RECOVERY_HOST_TEST
#include "sdkconfig.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"
#include "lwip/tcpip.h"
#include "lwip/priv/tcp_priv.h"
#endif
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include "sd_tcp_recovery.h"

#if CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE && !CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE
/* All PCB accesses remain on the TCP/IP task (or under its SDK core lock).
 * Static state survives a queued callback after a file/epoch has ended. */
typedef struct {
    uint32_t local_addr, remote_addr;
    uint16_t local_port, remote_port;
} sd_tcp_endpoint_t;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static atomic_uint s_ready;
static atomic_bool s_enabled;
static esp_timer_handle_t s_timer;
static StaticSemaphore_t s_event_storage;
static SemaphoreHandle_t s_event;
static struct {
    uint32_t token, epoch;
    uintptr_t owner; /* Equality key only; never dereferenced. */
    bool open;
    sd_tcp_endpoint_t endpoint;
    nuwax_sd_tcp_recovery_metrics_t metrics;
    nuwax_sd_tcp_snapshot_t snapshot;
    uint32_t last_ack, last_sent, probe_sequence;
    int64_t probe_started_us;
    bool probe_active;
} s_scope;
static struct {
    uint32_t token;
    unsigned remaining;
    int64_t next_us, cooldown_us;
} s_round;
/* A queue claim is not cleared by begin/end: otherwise an old queued
 * callback could observe an overwritten ticket and operate on a new flow. */
static bool s_queued;
static struct { uint32_t token; sd_tcp_endpoint_t endpoint; } s_ticket;
#define SD_TCP_RETRIES 4U
#define SD_TCP_FIRST_US 5000LL

static void schedule_retry(void);
static void retry_output(void *unused);

static bool endpoint_matches(const sd_tcp_endpoint_t *endpoint, const struct tcp_pcb *pcb)
{
    return pcb && IP_IS_V4_VAL(pcb->local_ip) && IP_IS_V4_VAL(pcb->remote_ip) &&
        pcb->local_port == endpoint->local_port && pcb->remote_port == endpoint->remote_port &&
        ip4_addr_get_u32(ip_2_ip4(&pcb->local_ip)) == endpoint->local_addr &&
        ip4_addr_get_u32(ip_2_ip4(&pcb->remote_ip)) == endpoint->remote_addr;
}

static void retry_poll(void)
{
    const int64_t now = esp_timer_get_time();
    bool claim = false;
    portENTER_CRITICAL(&s_lock);
    if (s_scope.open && s_round.token == s_scope.token && s_round.remaining &&
        !s_queued && now >= s_round.next_us) {
        --s_round.remaining; /* Posting failures consume the same finite budget. */
        s_queued = true;
        s_ticket.token = s_scope.token;
        s_ticket.endpoint = s_scope.endpoint;
        claim = true;
    }
    portEXIT_CRITICAL(&s_lock);
    if (!claim) return;
    /* Never block the high-priority timer task on the TCP/IP mailbox. */
    const err_t posted = tcpip_try_callback(retry_output, NULL);
    if (posted == ERR_OK) return;
    portENTER_CRITICAL(&s_lock);
    if (s_scope.open && s_scope.token == s_ticket.token) {
        ++s_scope.metrics.post_failures;
        if (!s_round.remaining) {
            ++s_scope.metrics.exhausted;
            s_round.cooldown_us = now + 250000LL;
        }
        s_round.next_us = now + (SD_TCP_FIRST_US << (SD_TCP_RETRIES - s_round.remaining));
    }
    s_queued = false;
    portEXIT_CRITICAL(&s_lock);
    schedule_retry();
}

static void retry_timer(void *unused)
{
    (void)unused;
    retry_poll();
    schedule_retry();
}

static void schedule_retry(void)
{
    if (atomic_load_explicit(&s_ready, memory_order_acquire) != 2U) return;
    portENTER_CRITICAL(&s_lock);
    const bool pending = s_scope.open && s_round.token == s_scope.token &&
        s_round.remaining && !s_queued;
    const int64_t due = s_round.next_us;
    portEXIT_CRITICAL(&s_lock);
    if (!pending) return;
    const int64_t delay = due - esp_timer_get_time();
    /* An already armed/selected callback may run; it checks the current
     * generation/deadline and does no output if no longer applicable. */
    (void)esp_timer_start_once(s_timer, delay > 0 ? (uint64_t)delay : 1U);
}

void nuwax_sd_tcp_recovery_prepare(void)
{
    unsigned empty = 0U;
    if (!atomic_compare_exchange_strong_explicit(&s_ready, &empty, 1U,
            memory_order_acq_rel, memory_order_acquire)) return;
    const esp_timer_create_args_t args = {.callback = retry_timer,
        .dispatch_method = ESP_TIMER_TASK, .name = "sd_formal_tcp"};
    if (!s_event) s_event = xSemaphoreCreateBinaryStatic(&s_event_storage);
    const bool ok = s_event && esp_timer_create(&args, &s_timer) == ESP_OK;
    atomic_store_explicit(&s_ready, ok ? 2U : 0U, memory_order_release);
}

uint32_t nuwax_sd_tcp_recovery_begin(uintptr_t owner, uint32_t epoch, int sock)
{
    if (!owner || !epoch || sock < 0 ||
        atomic_load_explicit(&s_ready, memory_order_acquire) != 2U) return 0U;
    struct sockaddr_storage local = {0}, remote = {0};
    socklen_t local_len = sizeof(local), remote_len = sizeof(remote);
    if (getsockname(sock, (struct sockaddr *)&local, &local_len) != 0 ||
        getpeername(sock, (struct sockaddr *)&remote, &remote_len) != 0 ||
        local.ss_family != AF_INET || remote.ss_family != AF_INET) return 0U;
    const struct sockaddr_in *la = (const struct sockaddr_in *)&local;
    const struct sockaddr_in *ra = (const struct sockaddr_in *)&remote;
    portENTER_CRITICAL(&s_lock);
    if (s_scope.open) { portEXIT_CRITICAL(&s_lock); return 0U; }
    uint32_t token = s_scope.token + 1U;
    if (!token) ++token;
    s_scope.token = token; s_scope.epoch = epoch; s_scope.owner = owner;
    s_scope.endpoint = (sd_tcp_endpoint_t){.local_addr = la->sin_addr.s_addr,
        .remote_addr = ra->sin_addr.s_addr, .local_port = ntohs(la->sin_port),
        .remote_port = ntohs(ra->sin_port)};
    s_scope.metrics = (nuwax_sd_tcp_recovery_metrics_t){.send_free_min = UINT32_MAX,
        .cwnd_min = UINT32_MAX, .peer_window_min = UINT32_MAX, .rtt_min_us = UINT32_MAX};
    s_scope.snapshot = (nuwax_sd_tcp_snapshot_t){0};
    s_scope.probe_active = false;
    s_round.token = token; s_round.remaining = 0U; s_round.cooldown_us = 0;
    s_scope.open = true;
    atomic_store_explicit(&s_enabled, true, memory_order_release);
    portEXIT_CRITICAL(&s_lock);
    return token;
}

void nuwax_sd_tcp_recovery_end(uint32_t token, nuwax_sd_tcp_recovery_metrics_t *metrics)
{
    if (metrics) *metrics = (nuwax_sd_tcp_recovery_metrics_t){0};
    if (!token) return;
    bool wake = false;
    portENTER_CRITICAL(&s_lock);
    if (s_scope.token == token) {
        wake = true;
        s_scope.open = false; s_round.remaining = 0U;
        atomic_store_explicit(&s_enabled, false, memory_order_release);
        if (metrics) {
            *metrics = s_scope.metrics;
            if (metrics->send_free_min == UINT32_MAX) metrics->send_free_min = 0U;
            if (metrics->cwnd_min == UINT32_MAX) metrics->cwnd_min = 0U;
            if (metrics->peer_window_min == UINT32_MAX) metrics->peer_window_min = 0U;
            if (metrics->rtt_min_us == UINT32_MAX) metrics->rtt_min_us = 0U;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    if (wake && s_event) xSemaphoreGive(s_event);
    /* A queued/selected callback still owns only its permanent ticket. */
}

void nuwax_sd_tcp_recovery_close(uintptr_t owner, uint32_t epoch)
{
    bool wake = false;
    portENTER_CRITICAL(&s_lock);
    if (s_scope.open && s_scope.owner == owner && (!epoch || s_scope.epoch == epoch)) {
        wake = true;
        s_scope.open = false; s_round.remaining = 0U;
        atomic_store_explicit(&s_enabled, false, memory_order_release);
    }
    portEXIT_CRITICAL(&s_lock);
    if (wake && s_event) xSemaphoreGive(s_event);
}

bool nuwax_sd_tcp_snapshot(uintptr_t owner, uint32_t epoch, nuwax_sd_tcp_snapshot_t *state)
{
    if (!state) return false;
    portENTER_CRITICAL(&s_lock);
    const bool active = s_scope.open && s_scope.owner == owner && s_scope.epoch == epoch;
    if (active) *state = s_scope.snapshot;
    portEXIT_CRITICAL(&s_lock);
    return active;
}

int nuwax_sd_tcp_wait_progress(uintptr_t owner, uint32_t epoch,
                              uint32_t observed_sequence, uint32_t timeout_ms)
{
    if (atomic_load_explicit(&s_ready, memory_order_acquire) != 2U || !s_event) return -1;
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    for (;;) {
        nuwax_sd_tcp_snapshot_t state;
        if (!nuwax_sd_tcp_snapshot(owner, epoch, &state)) return -1;
        if (state.sequence != observed_sequence) return 1;
        const int64_t left = deadline - esp_timer_get_time();
        if (left <= 0) return 0;
        /* Check the generation/sequence again after Take. The permanent
         * semaphore retains an ACK that races between the check and Take;
         * stale end/file events can only cause a harmless recheck. */
        const uint64_t tick_us = (uint64_t)portTICK_PERIOD_MS * 1000;
        const TickType_t ticks = (TickType_t)(((uint64_t)left + tick_us - 1U) / tick_us);
        (void)xSemaphoreTake(s_event, ticks);
    }
}

err_t __real_tcp_output(struct tcp_pcb *pcb);
err_t __wrap_tcp_output(struct tcp_pcb *pcb)
{
    /* Capture the scope BEFORE the SDK call. An old output completing after
     * end/begin on an identical endpoint must not publish into the new file. */
    uint32_t token = 0U;
    if (atomic_load_explicit(&s_enabled, memory_order_acquire)) {
        portENTER_CRITICAL(&s_lock);
        if (s_scope.open && endpoint_matches(&s_scope.endpoint, pcb)) token = s_scope.token;
        portEXIT_CRITICAL(&s_lock);
    }
    const err_t result = __real_tcp_output(pcb);
    const int saved_errno = errno;
    bool armed = false, wake = false;
    if (token && atomic_load_explicit(&s_enabled, memory_order_acquire)) {
        const int64_t now = esp_timer_get_time();
        portENTER_CRITICAL(&s_lock);
        if (s_scope.open && s_scope.token == token && endpoint_matches(&s_scope.endpoint, pcb)) {
            nuwax_sd_tcp_recovery_metrics_t *m = &s_scope.metrics;
            nuwax_sd_tcp_snapshot_t *s = &s_scope.snapshot;
            ++m->outputs;
            m->output_nomem += result == ERR_MEM;
            m->zero_window += pcb->snd_wnd == 0U;
            m->rto_samples += (pcb->flags & TF_RTO) != 0U;
            if (pcb->nrtx > m->nrtx_peak) m->nrtx_peak = pcb->nrtx;
            if (pcb->snd_queuelen > m->queue_peak) m->queue_peak = pcb->snd_queuelen;
            if (pcb->snd_buf < m->send_free_min) m->send_free_min = pcb->snd_buf;
            if (pcb->cwnd < m->cwnd_min) m->cwnd_min = pcb->cwnd;
            if (pcb->cwnd > m->cwnd_max) m->cwnd_max = pcb->cwnd;
            const uint32_t inflight = pcb->snd_nxt - pcb->lastack;
            if (inflight > m->inflight_peak) m->inflight_peak = inflight;
            if (pcb->snd_wnd < m->peer_window_min) m->peer_window_min = pcb->snd_wnd;
            if (pcb->snd_wnd > m->peer_window_max) m->peer_window_max = pcb->snd_wnd;
            const uint32_t queued = pcb->snd_buf <= CONFIG_LWIP_TCP_SND_BUF_DEFAULT
                ? CONFIG_LWIP_TCP_SND_BUF_DEFAULT - pcb->snd_buf : 0U;
            if (queued > m->queued_bytes_peak) m->queued_bytes_peak = queued;
            const uint32_t acked = pcb->lastack - s_scope.last_ack;
            const bool advanced = s->valid && acked && acked < 0x80000000U;
            if (advanced) { ++m->ack_events; m->acked_bytes += acked; }
            if ((pcb->flags & TF_RTO) || pcb->nrtx) s_scope.probe_active = false;
            if (s_scope.probe_active &&
                (uint32_t)(pcb->lastack - s_scope.probe_sequence) < 0x80000000U) {
                const int64_t us = now - s_scope.probe_started_us;
                if (us > 0 && us < UINT32_MAX) {
                    ++m->rtt_samples; m->rtt_total_us += (uint32_t)us;
                    if ((uint32_t)us < m->rtt_min_us) m->rtt_min_us = (uint32_t)us;
                    if ((uint32_t)us > m->rtt_max_us) m->rtt_max_us = (uint32_t)us;
                }
                s_scope.probe_active = false;
            }
            /* High-resolution send-to-ACK sample, excluding retransmitted
             * probes. This includes delayed ACK/network scheduling; it is not
             * a measurement of application commit persistence latency. */
            const uint32_t sent = pcb->snd_nxt - s_scope.last_sent;
            if (!s_scope.probe_active && !pcb->nrtx && !(pcb->flags & TF_RTO) &&
                pcb->snd_nxt != pcb->lastack && s->valid && sent && sent < 0x80000000U) {
                s_scope.probe_sequence = pcb->snd_nxt;
                s_scope.probe_started_us = now; s_scope.probe_active = true;
            }
            wake = !s->valid || advanced || pcb->snd_buf > s->send_free || pcb->snd_wnd > s->peer_window;
            if (wake) ++s->sequence;
            s->valid = true; s->queued_bytes = queued; s->send_free = pcb->snd_buf;
            s->peer_window = pcb->snd_wnd; s->cwnd = pcb->cwnd;
            s_scope.last_ack = pcb->lastack; s_scope.last_sent = pcb->snd_nxt;
            if (result == ERR_MEM && pcb->state == ESTABLISHED && pcb->unsent &&
                (pcb->flags & TF_NAGLEMEMERR) && !s_round.remaining &&
                (!s_queued || s_ticket.token != s_scope.token) &&
                now >= s_round.cooldown_us) {
                s_round.token = s_scope.token; s_round.remaining = SD_TCP_RETRIES;
                s_round.next_us = now + SD_TCP_FIRST_US;
                armed = true;
            }
        }
        portEXIT_CRITICAL(&s_lock);
    }
    if (wake && s_event) xSemaphoreGive(s_event);
    if (armed) schedule_retry();
    errno = saved_errno;
    return result;
}

static void retry_output(void *unused)
{
    (void)unused;
    portENTER_CRITICAL(&s_lock);
    const uint32_t token = s_ticket.token;
    const sd_tcp_endpoint_t endpoint = s_ticket.endpoint;
    bool active = s_scope.open && s_scope.token == token && s_round.token == token;
    portEXIT_CRITICAL(&s_lock);
    struct tcp_pcb *match = NULL, *pcb = active ? tcp_active_pcbs : NULL;
    unsigned visited = 0U;
    for (; pcb && visited < 32U; pcb = pcb->next, ++visited) {
        if (!endpoint_matches(&endpoint, pcb)) continue;
        if (match) { active = false; break; }
        match = pcb;
    }
    if (pcb) active = false; /* A partial scan cannot establish uniqueness. */
    bool attempted = false;
    err_t result = ERR_OK;
    if (active && match && match->state == ESTABLISHED && match->unsent &&
        (match->flags & TF_NAGLEMEMERR)) {
        portENTER_CRITICAL(&s_lock);
        active = s_scope.open && s_scope.token == token;
        if (active) ++s_scope.metrics.retries;
        portEXIT_CRITICAL(&s_lock);
        if (active) {
            /* One original SDK output, using its own window/congestion and
             * unsent-list rules. No payload copy, custom retransmit or waits. */
            attempted = true;
            result = __real_tcp_output(match);
        }
    }
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    if (s_scope.token == token && s_round.token == token) {
        if (!attempted) ++s_scope.metrics.stale;
        if (attempted && result == ERR_OK) ++s_scope.metrics.recovered;
        if (!s_scope.open || !attempted || result != ERR_MEM) s_round.remaining = 0U;
        else if (!s_round.remaining) {
            ++s_scope.metrics.exhausted;
            s_round.cooldown_us = now + 250000LL;
        }
        s_round.next_us = now + (SD_TCP_FIRST_US << (SD_TCP_RETRIES - s_round.remaining));
    }
    s_queued = false;
    portEXIT_CRITICAL(&s_lock);
    schedule_retry();
}
#else
void nuwax_sd_tcp_recovery_prepare(void) {}
uint32_t nuwax_sd_tcp_recovery_begin(uintptr_t owner, uint32_t epoch, int sock)
{ (void)owner; (void)epoch; (void)sock; return 0U; }
void nuwax_sd_tcp_recovery_end(uint32_t token, nuwax_sd_tcp_recovery_metrics_t *metrics)
{ (void)token; if (metrics) *metrics = (nuwax_sd_tcp_recovery_metrics_t){0}; }
void nuwax_sd_tcp_recovery_close(uintptr_t owner, uint32_t epoch) { (void)owner; (void)epoch; }
bool nuwax_sd_tcp_snapshot(uintptr_t owner, uint32_t epoch, nuwax_sd_tcp_snapshot_t *state)
{ (void)owner; (void)epoch; (void)state; return false; }
int nuwax_sd_tcp_wait_progress(uintptr_t owner, uint32_t epoch, uint32_t sequence, uint32_t timeout_ms)
{ (void)owner; (void)epoch; (void)sequence; (void)timeout_ms; return -1; }
#endif
