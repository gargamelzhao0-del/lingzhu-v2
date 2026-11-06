#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "audio_command_deadline.h"

/* Owner-task state only. RX stamps events with an independent monotonic order;
 * ACK queues may drain before commands, so dequeue order is not wire order.
 * No new wire fields: use the existing audio_mode_request / acknowledgement.
 * Reconciliation only publishes actual enabled/disabled state. Cloud set_audio_mode
 * is a desired-state command, even without a session ID or a sync reply. */
#define NUWAX_SD_CONTROL_SYNC_MS 5000U
#define NUWAX_SD_CONTROL_SYNC_RETRY_MS 1000U
#define NUWAX_SD_CONTROL_SYNC_ATTEMPTS 3U
#define NUWAX_SD_CONTROL_ENABLED_IDS 4U

typedef struct {
    uint32_t epoch;
    uint32_t started_ms;
    uint32_t started_order;
    uint32_t next_send_ms;
    uint32_t confirmed_order;
    bool confirmed;
    bool reply_confirmed;
    bool timeout_reported;
    /* A cloud command is not proof that this epoch's device report was sent. */
    bool announced;
    unsigned attempts;
    unsigned enabled_next;
    unsigned identity_attempts;
    bool identity_ready;
    bool reported_enabled;
    uint8_t send_failures;
    char request_id[48];
    /* Keep the original control alias for legacy App commands. A correlated
     * reconnect reply has a separate, replaceable alias. Neither changes the
     * identity stored in the audio archive. */
    char cloud_session_id[48];
    char reply_session_id[48];
    char enabled_ids[NUWAX_SD_CONTROL_ENABLED_IDS][48];
} nuwax_sd_control_sync_t;

static inline void nuwax_sd_control_sync_connect(
    nuwax_sd_control_sync_t *s, uint32_t epoch, uint32_t now, uint32_t received_order)
{
    if (!epoch || s->epoch == epoch) return;
    s->epoch = epoch;
    s->started_ms = s->next_send_ms = now;
    s->started_order = received_order;
    s->confirmed = s->reply_confirmed = s->timeout_reported = false;
    s->announced = false;
    s->send_failures = 0U;
    s->confirmed_order = s->attempts = 0;
    s->request_id[0] = '\0';
    /* Retain aliases and accepted requests across reconnects of this scene.
     * A fresh correlated reply replaces only the latest reply alias. */
}

static inline bool nuwax_sd_control_sync_due(
    const nuwax_sd_control_sync_t *s, uint32_t now)
{
    if (!s->epoch || (int32_t)(now - s->next_send_ms) < 0) return false;
    /* Keep trying, at bounded frequency, until at least one complete report
     * has left the device. Afterwards ACK retries have a finite budget. */
    return !s->announced || (!s->confirmed &&
        s->attempts < NUWAX_SD_CONTROL_SYNC_ATTEMPTS &&
        (uint32_t)(now - s->started_ms) < NUWAX_SD_CONTROL_SYNC_MS);
}

static inline void nuwax_sd_control_sync_note_send(
    nuwax_sd_control_sync_t *s, uint32_t now, bool success)
{
    if (success) {
        if (!s->announced) {
            s->announced = true;
            s->started_ms = now;
            s->timeout_reported = false;
            s->attempts = 0U;
        }
        if (s->attempts < NUWAX_SD_CONTROL_SYNC_ATTEMPTS) ++s->attempts;
        s->send_failures = 0U;
        s->next_send_ms = now + NUWAX_SD_CONTROL_SYNC_RETRY_MS;
    } else {
        if (s->send_failures < 5U) ++s->send_failures;
        uint32_t delay = NUWAX_SD_CONTROL_SYNC_RETRY_MS << (s->send_failures - 1U);
        if (delay > 10000U) delay = 10000U;
        s->next_send_ms = now + delay;
    }
}

static inline void nuwax_sd_control_sync_confirm(
    nuwax_sd_control_sync_t *s, uint32_t order, const char *cloud_session)
{
    if (s->confirmed) return; /* Duplicate ACK must not move the barrier. */
    s->confirmed = true;
    s->confirmed_order = order;
    /* Keep a legacy control alias across reconnect. Echoing the local SD ID
     * must not replace the original cloud ID still used by an App stop. */
    if (!s->cloud_session_id[0] && cloud_session && cloud_session[0] && strlen(cloud_session) < 48U)
        memcpy(s->cloud_session_id, cloud_session, strlen(cloud_session) + 1U);
}

/* Call only after matching the successful enabled reply to the current
 * request and connection epoch. A matching cloud enable may have confirmed
 * the ordering barrier already; it must not discard this later reply's ID.
 * Duplicate replies cannot change either the binding or that barrier. */
static inline void nuwax_sd_control_sync_confirm_reply(
    nuwax_sd_control_sync_t *s, uint32_t order, const char *cloud_session)
{
    if (s->reply_confirmed) return;
    s->reply_confirmed = true;
    nuwax_sd_control_sync_confirm(s, order, cloud_session);
    if (cloud_session && cloud_session[0] && strlen(cloud_session) < sizeof(s->reply_session_id))
        memcpy(s->reply_session_id, cloud_session, strlen(cloud_session) + 1U);
}

static inline bool nuwax_sd_control_session_matches(
    const nuwax_sd_control_sync_t *s, const char *local_session, const char *target_session)
{
    return target_session[0] &&
        ((local_session[0] && strcmp(target_session, local_session) == 0) ||
         (s->cloud_session_id[0] && strcmp(target_session, s->cloud_session_id) == 0) ||
         (s->reply_session_id[0] && strcmp(target_session, s->reply_session_id) == 0));
}

static inline const char *nuwax_sd_control_disable_rejection(
    nuwax_sd_control_sync_t *s, uint32_t order, uint32_t received_ms,
    const char *request, const char *target_session, const char *local_session)
{
    (void)s; (void)order; (void)received_ms; (void)request;
    (void)target_session; (void)local_session;
    /* Epoch, local-stop generation, expiry and request retirement are checked
     * by the controller. An audio identity is never permission to switch off. */
    return NULL;
}

static inline void nuwax_sd_control_track_enabled(nuwax_sd_control_sync_t *s, const char *request)
{
    if (!request || !request[0] || strlen(request) >= 48U) return;
    for (unsigned i = 0; i < NUWAX_SD_CONTROL_ENABLED_IDS; ++i)
        if (!strcmp(s->enabled_ids[i], request)) return;
    char *slot = s->enabled_ids[s->enabled_next++ % NUWAX_SD_CONTROL_ENABLED_IDS];
    memcpy(slot, request, strlen(request) + 1U);
}

/* Already-active commands are successful state assignments, but must not
 * resurrect this capture after a later stop. Reuse the existing bounded
 * retired-request cache rather than inventing durable control identities. */
static inline void nuwax_sd_control_retire_enabled(nuwax_sd_control_sync_t *s,
    nuwax_audio_command_deadline_t *command, int64_t now)
{
    for (unsigned i = 0; i < NUWAX_SD_CONTROL_ENABLED_IDS; ++i) {
        if (s->enabled_ids[i][0] && !nuwax_audio_command_failed(command, s->enabled_ids[i])) {
            nuwax_audio_command_failure_t *entry = &command->failed[command->failed_next++ % NUWAX_AUDIO_COMMAND_FAILURE_CACHE];
            memcpy(entry->request_id, s->enabled_ids[i], sizeof(entry->request_id));
            entry->received_us = now;
        }
        s->enabled_ids[i][0] = '\0';
    }
}
