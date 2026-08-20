#include "notification_rx_barrier.h"

#include <string.h>

static bool deadline_elapsed(uint32_t now_ms, uint32_t deadline_ms) {
    return deadline_ms != 0U &&
           (int32_t)(now_ms - deadline_ms) >= 0;
}

static bool valid_id(const char *value, size_t capacity) {
    return value && value[0] != '\0' && strlen(value) < capacity;
}

static bool ids_match(const nuwax_notification_rx_barrier_t *barrier,
                      const char *stream_id,
                      const char *playback_id) {
    return barrier && valid_id(stream_id, sizeof(barrier->stream_id)) &&
           valid_id(playback_id, sizeof(barrier->playback_id)) &&
           strcmp(barrier->stream_id, stream_id) == 0 &&
           strcmp(barrier->playback_id, playback_id) == 0;
}

static void bind_stream(nuwax_notification_rx_barrier_t *barrier,
                        nuwax_notification_rx_state_t state,
                        nuwax_notification_rx_owner_t owner,
                        uint32_t ws_epoch,
                        uint32_t deadline_ms,
                        const char *stream_id,
                        const char *playback_id) {
    memset(barrier, 0, sizeof(*barrier));
    barrier->state = state;
    barrier->owner = owner;
    barrier->ws_epoch = ws_epoch;
    barrier->deadline_ms = deadline_ms;
    memcpy(barrier->stream_id, stream_id, strlen(stream_id) + 1U);
    memcpy(barrier->playback_id, playback_id, strlen(playback_id) + 1U);
}

void nuwax_notification_rx_barrier_init(
    nuwax_notification_rx_barrier_t *barrier) {
    if (!barrier) {
        return;
    }
    memset(barrier, 0, sizeof(*barrier));
}

bool nuwax_notification_rx_barrier_arm(
    nuwax_notification_rx_barrier_t *barrier,
    uint32_t ws_epoch,
    uint32_t now_ms,
    uint32_t timeout_ms,
    const char *notify_id,
    const char *playback_id) {
    if (!barrier || ws_epoch == 0U || timeout_ms == 0U ||
        !valid_id(notify_id, sizeof(barrier->stream_id)) ||
        !valid_id(playback_id, sizeof(barrier->playback_id))) {
        return false;
    }
    if (barrier->ws_epoch == ws_epoch &&
        (barrier->state == NUWAX_NOTIFICATION_RX_PENDING ||
         barrier->state == NUWAX_NOTIFICATION_RX_ADMITTED)) {
        if (barrier->owner != NUWAX_NOTIFICATION_RX_OWNER_NOTIFICATION ||
            !ids_match(barrier, notify_id, playback_id)) {
            return false;
        }
        /* Revalidate every duplicate BEGIN in the application owner. This
         * prevents same-ID retries with conflicting totals from using a
         * previously admitted binary path before the queued text is checked. */
        barrier->state = NUWAX_NOTIFICATION_RX_PENDING;
        barrier->deadline_ms = now_ms + timeout_ms;
        return true;
    }
    if (barrier->ws_epoch == ws_epoch &&
        barrier->state == NUWAX_NOTIFICATION_RX_NORMAL_PENDING) {
        /* NORMAL_PENDING means no binary byte has crossed the application
         * ordering barrier yet. A later notification BEGIN is therefore a
         * safe, deterministic cloud selection point: replace the pending
         * normal owner before either stream reaches the speaker. */
        bind_stream(barrier,
                    NUWAX_NOTIFICATION_RX_PENDING,
                    NUWAX_NOTIFICATION_RX_OWNER_NOTIFICATION,
                    ws_epoch,
                    now_ms + timeout_ms,
                    notify_id,
                    playback_id);
        return true;
    }
    if (barrier->ws_epoch == ws_epoch &&
        barrier->state == NUWAX_NOTIFICATION_RX_REJECTED) {
        return false;
    }
    bind_stream(barrier,
                NUWAX_NOTIFICATION_RX_PENDING,
                NUWAX_NOTIFICATION_RX_OWNER_NOTIFICATION,
                ws_epoch,
                now_ms + timeout_ms,
                notify_id,
                playback_id);
    return true;
}

bool nuwax_notification_rx_barrier_admit(
    nuwax_notification_rx_barrier_t *barrier,
    uint32_t ws_epoch,
    const char *notify_id,
    const char *playback_id) {
    if (!barrier || ws_epoch == 0U || barrier->ws_epoch != ws_epoch ||
        barrier->owner != NUWAX_NOTIFICATION_RX_OWNER_NOTIFICATION ||
        !ids_match(barrier, notify_id, playback_id) ||
        (barrier->state != NUWAX_NOTIFICATION_RX_PENDING &&
         barrier->state != NUWAX_NOTIFICATION_RX_ADMITTED)) {
        return false;
    }
    barrier->state = NUWAX_NOTIFICATION_RX_ADMITTED;
    barrier->deadline_ms = 0U;
    return true;
}

bool nuwax_notification_rx_barrier_arm_normal(
    nuwax_notification_rx_barrier_t *barrier,
    uint32_t ws_epoch,
    uint32_t now_ms,
    uint32_t timeout_ms,
    const char *turn_id,
    const char *playback_id) {
    if (!barrier || ws_epoch == 0U || timeout_ms == 0U ||
        !valid_id(turn_id, sizeof(barrier->stream_id)) ||
        !valid_id(playback_id, sizeof(barrier->playback_id))) {
        return false;
    }
    if (barrier->ws_epoch != ws_epoch ||
        barrier->state == NUWAX_NOTIFICATION_RX_IDLE) {
        bind_stream(barrier,
                    NUWAX_NOTIFICATION_RX_NORMAL_PENDING,
                    NUWAX_NOTIFICATION_RX_OWNER_NORMAL,
                    ws_epoch,
                    now_ms + timeout_ms,
                    turn_id,
                    playback_id);
        return true;
    }
    if (barrier->state == NUWAX_NOTIFICATION_RX_NORMAL_PENDING) {
        return barrier->owner == NUWAX_NOTIFICATION_RX_OWNER_NORMAL &&
               ids_match(barrier, turn_id, playback_id);
    }
    return false;
}

bool nuwax_notification_rx_barrier_admit_normal(
    nuwax_notification_rx_barrier_t *barrier,
    uint32_t ws_epoch,
    const char *turn_id,
    const char *playback_id) {
    if (!barrier || ws_epoch == 0U) {
        return false;
    }
    if (barrier->ws_epoch != ws_epoch ||
        barrier->state != NUWAX_NOTIFICATION_RX_NORMAL_PENDING ||
        barrier->owner != NUWAX_NOTIFICATION_RX_OWNER_NORMAL ||
        !ids_match(barrier, turn_id, playback_id)) {
        return false;
    }
    nuwax_notification_rx_barrier_reset(barrier);
    return true;
}

bool nuwax_notification_rx_barrier_reject_normal(
    nuwax_notification_rx_barrier_t *barrier,
    uint32_t ws_epoch,
    uint32_t now_ms,
    uint32_t drain_timeout_ms,
    const char *turn_id,
    const char *playback_id) {
    if (!barrier || ws_epoch == 0U || drain_timeout_ms == 0U ||
        barrier->ws_epoch != ws_epoch ||
        barrier->state != NUWAX_NOTIFICATION_RX_NORMAL_PENDING ||
        barrier->owner != NUWAX_NOTIFICATION_RX_OWNER_NORMAL ||
        !ids_match(barrier, turn_id, playback_id)) {
        return false;
    }
    barrier->state = NUWAX_NOTIFICATION_RX_REJECTED;
    barrier->deadline_ms = now_ms + drain_timeout_ms;
    return true;
}

bool nuwax_notification_rx_barrier_reject(
    nuwax_notification_rx_barrier_t *barrier,
    uint32_t ws_epoch,
    uint32_t now_ms,
    uint32_t drain_timeout_ms) {
    if (!barrier || ws_epoch == 0U || drain_timeout_ms == 0U ||
        barrier->ws_epoch != ws_epoch ||
        barrier->state == NUWAX_NOTIFICATION_RX_IDLE) {
        return false;
    }
    if (barrier->state == NUWAX_NOTIFICATION_RX_REJECTED) {
        /* Repeated rejection must not extend quarantine indefinitely. */
        return true;
    }
    barrier->state = NUWAX_NOTIFICATION_RX_REJECTED;
    barrier->deadline_ms = now_ms + drain_timeout_ms;
    return true;
}

void nuwax_notification_rx_barrier_reset(
    nuwax_notification_rx_barrier_t *barrier) {
    if (!barrier) {
        return;
    }
    memset(barrier, 0, sizeof(*barrier));
}

bool nuwax_notification_rx_barrier_reset_if_epoch(
    nuwax_notification_rx_barrier_t *barrier,
    uint32_t ws_epoch) {
    if (!barrier || ws_epoch == 0U || barrier->ws_epoch != ws_epoch) {
        return false;
    }
    nuwax_notification_rx_barrier_reset(barrier);
    return true;
}

nuwax_notification_rx_state_t nuwax_notification_rx_barrier_state_for_epoch(
    const nuwax_notification_rx_barrier_t *barrier,
    uint32_t ws_epoch) {
    if (!barrier || ws_epoch == 0U || barrier->ws_epoch != ws_epoch) {
        return NUWAX_NOTIFICATION_RX_IDLE;
    }
    return barrier->state;
}

bool nuwax_notification_rx_barrier_deadline_expired(
    const nuwax_notification_rx_barrier_t *barrier,
    uint32_t now_ms) {
    return barrier && barrier->state != NUWAX_NOTIFICATION_RX_IDLE &&
           deadline_elapsed(now_ms, barrier->deadline_ms);
}

bool nuwax_notification_rx_barrier_take_rejected_timeout(
    nuwax_notification_rx_barrier_t *barrier,
    uint32_t ws_epoch,
    uint32_t now_ms) {
    if (!barrier || ws_epoch == 0U || barrier->ws_epoch != ws_epoch ||
        barrier->state != NUWAX_NOTIFICATION_RX_REJECTED ||
        !deadline_elapsed(now_ms, barrier->deadline_ms)) {
        return false;
    }
    /* Zeroing only the deadline makes this notification one-shot while the
     * rejected stream remains quarantined until END or epoch teardown. */
    barrier->deadline_ms = 0U;
    return true;
}
