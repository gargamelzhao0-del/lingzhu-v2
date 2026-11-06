#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Device-local limits; no additional fields are sent to the cloud. */
#define NUWAX_AUDIO_COMMAND_EXECUTE_MS 20000U
#define NUWAX_AUDIO_COMMAND_ACK_MS 25000U
#define NUWAX_AUDIO_COMMAND_SEND_SLICE_US 250000LL
#define NUWAX_AUDIO_COMMAND_RETRY_MS 500U
#define NUWAX_AUDIO_COMMAND_FAILURE_CACHE 8U

typedef struct {
    char request_id[48];
    int64_t received_us;
} nuwax_audio_command_failure_t;

typedef struct {
    char request_id[48];
    char session_id[48];
    char message[32];
    int64_t received_us;
    uint32_t epoch;
    uint32_t local_stop_generation;
    uint32_t retry_at_ms;
    unsigned attempts;
    bool tracked;
    bool prepared;
    bool decided;
    bool enabled;
    bool ok;
    bool ack_pending;
    bool retired;
    /* Independent of capture teardown and connection epochs. Failed or retired
     * starts must not be revived by queued/retransmitted copies. */
    nuwax_audio_command_failure_t failed[NUWAX_AUDIO_COMMAND_FAILURE_CACHE];
    unsigned failed_next;
} nuwax_audio_command_deadline_t;

static inline bool nuwax_audio_command_matches(const nuwax_audio_command_deadline_t *c,
                                               const char *request)
{
    return c->tracked && request && request[0] && !strcmp(c->request_id, request);
}

static inline bool nuwax_audio_command_failed(const nuwax_audio_command_deadline_t *c,
                                              const char *request)
{
    if (!request || !request[0]) return false;
    for (unsigned i = 0; i < NUWAX_AUDIO_COMMAND_FAILURE_CACHE; ++i)
        if (!strcmp(c->failed[i].request_id, request)) return true;
    return false;
}

static inline int64_t nuwax_audio_command_execution_deadline(const nuwax_audio_command_deadline_t *c)
{
    return c->tracked ? c->received_us + (int64_t)NUWAX_AUDIO_COMMAND_EXECUTE_MS * 1000 : 0;
}

static inline bool nuwax_audio_command_expired(const nuwax_audio_command_deadline_t *c, int64_t now)
{
    return c->tracked && !c->decided && now >= nuwax_audio_command_execution_deadline(c);
}

static inline void nuwax_audio_command_begin(nuwax_audio_command_deadline_t *c,
    const char *request, uint32_t received_ms, uint32_t epoch, uint32_t stop_generation,
    int64_t now)
{
    if (nuwax_audio_command_matches(c, request)) return;
    /* received_ms is the low 32 bits of the monotonic RX time, not dequeue time. */
    c->received_us = (now / 1000 - (uint32_t)((uint32_t)(now / 1000) - received_ms)) * 1000;
    snprintf(c->request_id, sizeof(c->request_id), "%s", request ? request : "");
    c->session_id[0] = c->message[0] = '\0';
    c->epoch = epoch;
    c->local_stop_generation = stop_generation;
    c->tracked = request && request[0];
    c->prepared = c->decided = c->ok = c->ack_pending = false;
    c->retired = false;
    c->enabled = true;
    c->attempts = 0;
    c->retry_at_ms = 0;
}

static inline void nuwax_audio_command_decide(nuwax_audio_command_deadline_t *c,
    bool enabled, bool ok, const char *message, const char *session_id)
{
    if (!c->tracked || c->decided) return;
    c->decided = true;
    c->enabled = enabled;
    c->ok = ok;
    c->ack_pending = true;
    c->attempts = 0;
    c->retry_at_ms = 0;
    snprintf(c->message, sizeof(c->message), "%s", message ? message : "capture_failed");
    snprintf(c->session_id, sizeof(c->session_id), "%s", session_id ? session_id : "");
    if (!ok) {
        nuwax_audio_command_failure_t *f = &c->failed[c->failed_next++ % NUWAX_AUDIO_COMMAND_FAILURE_CACHE];
        memmove(f->request_id, c->request_id, sizeof(f->request_id));
        f->received_us = c->received_us;
    }
}

static inline int64_t nuwax_audio_command_send_deadline(const nuwax_audio_command_deadline_t *c,
                                                       int64_t now)
{
    const int64_t limit = c->received_us + (int64_t)NUWAX_AUDIO_COMMAND_ACK_MS * 1000;
    if (!c->tracked || now >= limit) return 0;
    return limit - now < NUWAX_AUDIO_COMMAND_SEND_SLICE_US
        ? limit : now + NUWAX_AUDIO_COMMAND_SEND_SLICE_US;
}

/* A successful opening result describes a live capture. Once that capture
 * ends it must never be replayed as current state or reopen the old command. */
static inline void nuwax_audio_command_retire_active(nuwax_audio_command_deadline_t *c)
{
    if (!c->tracked || !c->decided || !c->ok || !c->enabled || c->retired) return;
    c->retired = true;
    c->ack_pending = false;
    nuwax_audio_command_failure_t *f = &c->failed[c->failed_next++ % NUWAX_AUDIO_COMMAND_FAILURE_CACHE];
    memmove(f->request_id, c->request_id, sizeof(f->request_id));
    f->received_us = c->received_us;
}
