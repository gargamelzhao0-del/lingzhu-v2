#pragma once

#include <stdbool.h>
#include <stdint.h>

#define NUWAX_PLAYBACK_TERMINAL_ID_MAX_LEN 48
#define NUWAX_PLAYBACK_CANCEL_REQUEST_ID_MAX_LEN 40

typedef enum {
    NUWAX_PLAYBACK_CANCEL_ACK_UNMATCHED = 0,
    NUWAX_PLAYBACK_CANCEL_ACK_ACCEPTED,
    NUWAX_PLAYBACK_CANCEL_ACK_REJECTED,
} nuwax_playback_cancel_ack_result_t;

typedef struct {
    bool active;
    bool faulted;
    bool physical_done;
    bool stream_end_seen;
    bool logical_end_seen;
    bool logical_end_completed;
    bool cancel_sent;
    bool cancel_ack_seen;
    bool local_recovery_committed;
    bool reconnect_requested;
    uint32_t connection_epoch;
    uint32_t speaker_generation;
    uint32_t deadline_ms;
    char turn_id[NUWAX_PLAYBACK_TERMINAL_ID_MAX_LEN];
    char playback_id[NUWAX_PLAYBACK_TERMINAL_ID_MAX_LEN];
    char cancel_request_id[NUWAX_PLAYBACK_CANCEL_REQUEST_ID_MAX_LEN];
} nuwax_playback_terminal_guard_t;

void nuwax_playback_terminal_guard_clear(
    nuwax_playback_terminal_guard_t *guard);
bool nuwax_playback_terminal_guard_arm(
    nuwax_playback_terminal_guard_t *guard,
    uint32_t connection_epoch,
    uint32_t speaker_generation,
    const char *turn_id,
    const char *playback_id,
    uint32_t now_ms,
    uint32_t liveness_timeout_ms);
/* Accepted PCM keeps a live playback generation from being mistaken for an
 * orphan. Progress stops extending the deadline after a fault or physical
 * completion, when the shorter terminal wait owns convergence instead. */
bool nuwax_playback_terminal_guard_refresh_progress(
    nuwax_playback_terminal_guard_t *guard,
    uint32_t connection_epoch,
    uint32_t speaker_generation,
    uint32_t now_ms,
    uint32_t liveness_timeout_ms);
bool nuwax_playback_terminal_guard_mark_fault(
    nuwax_playback_terminal_guard_t *guard,
    uint32_t connection_epoch,
    uint32_t speaker_generation,
    uint32_t now_ms,
    uint32_t timeout_ms);
bool nuwax_playback_terminal_guard_mark_physical_done(
    nuwax_playback_terminal_guard_t *guard,
    uint32_t connection_epoch,
    uint32_t speaker_generation,
    bool stream_end_seen,
    uint32_t now_ms,
    uint32_t timeout_ms);
bool nuwax_playback_terminal_guard_mark_stream_end(
    nuwax_playback_terminal_guard_t *guard,
    uint32_t connection_epoch,
    uint32_t speaker_generation);
bool nuwax_playback_terminal_guard_mark_logical_end(
    nuwax_playback_terminal_guard_t *guard,
    uint32_t connection_epoch,
    const char *turn_id,
    const char *playback_id,
    bool completed);
bool nuwax_playback_terminal_guard_natural_drain_matches(
    const nuwax_playback_terminal_guard_t *guard,
    uint32_t connection_epoch,
    uint32_t speaker_generation,
    const char *turn_id,
    const char *playback_id);
bool nuwax_playback_terminal_guard_mark_cancel_sent(
    nuwax_playback_terminal_guard_t *guard,
    uint32_t connection_epoch,
    uint32_t speaker_generation,
    const char *request_id,
    uint32_t now_ms,
    uint32_t timeout_ms);
nuwax_playback_cancel_ack_result_t
nuwax_playback_terminal_guard_apply_cancel_ack(
    nuwax_playback_terminal_guard_t *guard,
    uint32_t connection_epoch,
    const char *request_id,
    const char *turn_id,
    const char *playback_id,
    bool ok);
/* A scoped remote stop replaces both missing cloud terminal markers, but it
 * still waits for the same generation's physical stop before becoming ready. */
bool nuwax_playback_terminal_guard_mark_committed_stop(
    nuwax_playback_terminal_guard_t *guard,
    uint32_t connection_epoch,
    uint32_t speaker_generation,
    const char *turn_id,
    const char *playback_id);
/* Once the matching speaker generation is physically stopped, a missing or
 * rejected cloud terminal response may be quarantined locally without
 * destroying an otherwise healthy WebSocket. */
bool nuwax_playback_terminal_guard_commit_local_recovery(
    nuwax_playback_terminal_guard_t *guard,
    uint32_t connection_epoch,
    uint32_t speaker_generation);
bool nuwax_playback_terminal_guard_release_committed_interrupt(
    nuwax_playback_terminal_guard_t *guard,
    uint32_t connection_epoch,
    uint32_t speaker_generation,
    const char *turn_id,
    const char *playback_id);
bool nuwax_playback_terminal_guard_ready(
    const nuwax_playback_terminal_guard_t *guard);
bool nuwax_playback_terminal_guard_waiting(
    const nuwax_playback_terminal_guard_t *guard);
bool nuwax_playback_terminal_guard_deadline_expired(
    const nuwax_playback_terminal_guard_t *guard,
    uint32_t now_ms);
bool nuwax_playback_terminal_guard_mark_reconnect_requested(
    nuwax_playback_terminal_guard_t *guard);
