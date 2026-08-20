#include "playback_terminal_guard.h"

#include <string.h>

static bool id_present(const char *value) {
    return value && value[0] != '\0';
}

static void copy_id(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0U) {
        return;
    }
    strncpy(dst, src ? src : "", dst_size - 1U);
    dst[dst_size - 1U] = '\0';
}

static bool source_matches(const nuwax_playback_terminal_guard_t *guard,
                           uint32_t connection_epoch,
                           uint32_t speaker_generation) {
    return guard && guard->active && connection_epoch != 0U &&
           speaker_generation != 0U &&
           guard->connection_epoch == connection_epoch &&
           guard->speaker_generation == speaker_generation;
}

static void arm_deadline(nuwax_playback_terminal_guard_t *guard,
                         uint32_t now_ms,
                         uint32_t timeout_ms) {
    if (guard->deadline_ms == 0U && timeout_ms != 0U) {
        guard->deadline_ms = now_ms + timeout_ms;
        if (guard->deadline_ms == 0U) {
            guard->deadline_ms = 1U;
        }
    }
}

static void replace_deadline(nuwax_playback_terminal_guard_t *guard,
                             uint32_t now_ms,
                             uint32_t timeout_ms) {
    if (!guard || timeout_ms == 0U) {
        return;
    }
    guard->deadline_ms = now_ms + timeout_ms;
    if (guard->deadline_ms == 0U) {
        guard->deadline_ms = 1U;
    }
}

void nuwax_playback_terminal_guard_clear(
    nuwax_playback_terminal_guard_t *guard) {
    if (guard) {
        memset(guard, 0, sizeof(*guard));
    }
}

bool nuwax_playback_terminal_guard_arm(
    nuwax_playback_terminal_guard_t *guard,
    uint32_t connection_epoch,
    uint32_t speaker_generation,
    const char *turn_id,
    const char *playback_id,
    uint32_t now_ms,
    uint32_t liveness_timeout_ms) {
    if (!guard || connection_epoch == 0U || speaker_generation == 0U ||
        !id_present(turn_id) || !id_present(playback_id) ||
        liveness_timeout_ms == 0U) {
        return false;
    }
    if (source_matches(guard, connection_epoch, speaker_generation)) {
        const bool ids_match = strcmp(guard->turn_id, turn_id) == 0 &&
                               strcmp(guard->playback_id, playback_id) == 0;
        if (ids_match && !guard->faulted && !guard->physical_done) {
            replace_deadline(guard, now_ms, liveness_timeout_ms);
        }
        return ids_match;
    }
    if (guard->active) {
        return false;
    }

    nuwax_playback_terminal_guard_clear(guard);
    guard->active = true;
    guard->connection_epoch = connection_epoch;
    guard->speaker_generation = speaker_generation;
    copy_id(guard->turn_id, sizeof(guard->turn_id), turn_id);
    copy_id(guard->playback_id, sizeof(guard->playback_id), playback_id);
    replace_deadline(guard, now_ms, liveness_timeout_ms);
    return true;
}

bool nuwax_playback_terminal_guard_refresh_progress(
    nuwax_playback_terminal_guard_t *guard,
    uint32_t connection_epoch,
    uint32_t speaker_generation,
    uint32_t now_ms,
    uint32_t liveness_timeout_ms) {
    if (!source_matches(guard, connection_epoch, speaker_generation) ||
        guard->faulted || guard->physical_done ||
        liveness_timeout_ms == 0U) {
        return false;
    }
    replace_deadline(guard, now_ms, liveness_timeout_ms);
    return true;
}

bool nuwax_playback_terminal_guard_mark_fault(
    nuwax_playback_terminal_guard_t *guard,
    uint32_t connection_epoch,
    uint32_t speaker_generation,
    uint32_t now_ms,
    uint32_t timeout_ms) {
    if (!source_matches(guard, connection_epoch, speaker_generation)) {
        return false;
    }
    const bool terminal_wait_started = guard->faulted || guard->physical_done;
    guard->faulted = true;
    if (terminal_wait_started) {
        arm_deadline(guard, now_ms, timeout_ms);
    } else {
        replace_deadline(guard, now_ms, timeout_ms);
    }
    return true;
}

bool nuwax_playback_terminal_guard_mark_physical_done(
    nuwax_playback_terminal_guard_t *guard,
    uint32_t connection_epoch,
    uint32_t speaker_generation,
    bool stream_end_seen,
    uint32_t now_ms,
    uint32_t timeout_ms) {
    if (!source_matches(guard, connection_epoch, speaker_generation)) {
        return false;
    }
    const bool terminal_wait_started = guard->faulted || guard->physical_done;
    guard->physical_done = true;
    guard->stream_end_seen |= stream_end_seen;
    if (terminal_wait_started) {
        arm_deadline(guard, now_ms, timeout_ms);
    } else {
        replace_deadline(guard, now_ms, timeout_ms);
    }
    return true;
}

bool nuwax_playback_terminal_guard_mark_stream_end(
    nuwax_playback_terminal_guard_t *guard,
    uint32_t connection_epoch,
    uint32_t speaker_generation) {
    if (!source_matches(guard, connection_epoch, speaker_generation)) {
        return false;
    }
    guard->stream_end_seen = true;
    return true;
}

bool nuwax_playback_terminal_guard_mark_logical_end(
    nuwax_playback_terminal_guard_t *guard,
    uint32_t connection_epoch,
    const char *turn_id,
    const char *playback_id,
    bool completed) {
    if (!guard || !guard->active || connection_epoch == 0U ||
        guard->connection_epoch != connection_epoch || !id_present(turn_id) ||
        !id_present(playback_id) || strcmp(guard->turn_id, turn_id) != 0 ||
        strcmp(guard->playback_id, playback_id) != 0) {
        return false;
    }
    if (!guard->logical_end_seen) {
        guard->logical_end_seen = true;
        guard->logical_end_completed = completed;
    }
    return true;
}

bool nuwax_playback_terminal_guard_natural_drain_matches(
    const nuwax_playback_terminal_guard_t *guard,
    uint32_t connection_epoch,
    uint32_t speaker_generation,
    const char *turn_id,
    const char *playback_id) {
    return source_matches(guard, connection_epoch, speaker_generation) &&
           !guard->faulted && guard->logical_end_seen &&
           guard->logical_end_completed && id_present(turn_id) &&
           id_present(playback_id) && strcmp(guard->turn_id, turn_id) == 0 &&
           strcmp(guard->playback_id, playback_id) == 0;
}

bool nuwax_playback_terminal_guard_mark_cancel_sent(
    nuwax_playback_terminal_guard_t *guard,
    uint32_t connection_epoch,
    uint32_t speaker_generation,
    const char *request_id,
    uint32_t now_ms,
    uint32_t timeout_ms) {
    if (!source_matches(guard, connection_epoch, speaker_generation) ||
        !guard->faulted || !id_present(request_id)) {
        return false;
    }
    if (guard->cancel_sent) {
        return strcmp(guard->cancel_request_id, request_id) == 0;
    }
    copy_id(guard->cancel_request_id,
            sizeof(guard->cancel_request_id),
            request_id);
    guard->cancel_sent = true;
    replace_deadline(guard, now_ms, timeout_ms);
    return true;
}

nuwax_playback_cancel_ack_result_t
nuwax_playback_terminal_guard_apply_cancel_ack(
    nuwax_playback_terminal_guard_t *guard,
    uint32_t connection_epoch,
    const char *request_id,
    const char *turn_id,
    const char *playback_id,
    bool ok) {
    if (!guard || !guard->active || !guard->faulted || !guard->cancel_sent ||
        connection_epoch == 0U || guard->connection_epoch != connection_epoch ||
        !id_present(turn_id) || strcmp(guard->turn_id, turn_id) != 0) {
        return NUWAX_PLAYBACK_CANCEL_ACK_UNMATCHED;
    }
    if (id_present(request_id) &&
        strcmp(guard->cancel_request_id, request_id) != 0) {
        return NUWAX_PLAYBACK_CANCEL_ACK_UNMATCHED;
    }
    if (id_present(playback_id) &&
        strcmp(guard->playback_id, playback_id) != 0) {
        return NUWAX_PLAYBACK_CANCEL_ACK_UNMATCHED;
    }
    if (!ok) {
        return NUWAX_PLAYBACK_CANCEL_ACK_REJECTED;
    }
    guard->cancel_ack_seen = true;
    return NUWAX_PLAYBACK_CANCEL_ACK_ACCEPTED;
}

bool nuwax_playback_terminal_guard_mark_committed_stop(
    nuwax_playback_terminal_guard_t *guard,
    uint32_t connection_epoch,
    uint32_t speaker_generation,
    const char *turn_id,
    const char *playback_id) {
    if (!source_matches(guard, connection_epoch, speaker_generation) ||
        !id_present(turn_id) || !id_present(playback_id) ||
        strcmp(guard->turn_id, turn_id) != 0 ||
        strcmp(guard->playback_id, playback_id) != 0) {
        return false;
    }
    guard->stream_end_seen = true;
    guard->logical_end_seen = true;
    guard->logical_end_completed = false;
    return true;
}

bool nuwax_playback_terminal_guard_commit_local_recovery(
    nuwax_playback_terminal_guard_t *guard,
    uint32_t connection_epoch,
    uint32_t speaker_generation) {
    if (!source_matches(guard, connection_epoch, speaker_generation) ||
        !guard->physical_done) {
        return false;
    }
    guard->faulted = true;
    guard->local_recovery_committed = true;
    return true;
}

bool nuwax_playback_terminal_guard_release_committed_interrupt(
    nuwax_playback_terminal_guard_t *guard,
    uint32_t connection_epoch,
    uint32_t speaker_generation,
    const char *turn_id,
    const char *playback_id) {
    if (!source_matches(guard, connection_epoch, speaker_generation) ||
        !id_present(turn_id) || !id_present(playback_id) ||
        strcmp(guard->turn_id, turn_id) != 0 ||
        strcmp(guard->playback_id, playback_id) != 0) {
        return false;
    }

    nuwax_playback_terminal_guard_clear(guard);
    return true;
}

bool nuwax_playback_terminal_guard_ready(
    const nuwax_playback_terminal_guard_t *guard) {
    if (!guard || !guard->active || !guard->physical_done) {
        return false;
    }
    if (guard->faulted) {
        return guard->stream_end_seen || guard->logical_end_seen ||
               guard->cancel_ack_seen || guard->local_recovery_committed;
    }
    return guard->stream_end_seen && guard->logical_end_seen;
}

bool nuwax_playback_terminal_guard_waiting(
    const nuwax_playback_terminal_guard_t *guard) {
    return guard && guard->active;
}

bool nuwax_playback_terminal_guard_deadline_expired(
    const nuwax_playback_terminal_guard_t *guard,
    uint32_t now_ms) {
    return guard && guard->active && guard->deadline_ms != 0U &&
           (int32_t)(now_ms - guard->deadline_ms) >= 0;
}

bool nuwax_playback_terminal_guard_mark_reconnect_requested(
    nuwax_playback_terminal_guard_t *guard) {
    if (!guard || !guard->active || guard->reconnect_requested) {
        return false;
    }
    guard->reconnect_requested = true;
    return true;
}
