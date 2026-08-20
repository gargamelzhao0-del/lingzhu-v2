#include "conversation/touch_cancel_recovery.h"

#include <stddef.h>
#include <string.h>

static bool nonempty(const char *value) {
    return value && value[0] != '\0';
}

static bool equal_optional(const char *expected, const char *actual, bool *seen) {
    if (!nonempty(actual)) {
        return true;
    }
    if (seen) {
        *seen = true;
    }
    return nonempty(expected) && strcmp(expected, actual) == 0;
}

bool nuwax_touch_cancel_recovery_arm(
    nuwax_touch_cancel_recovery_t *recovery,
    uint32_t ws_epoch,
    uint32_t awake_generation,
    uint32_t speaker_generation,
    uint32_t deadline_ms,
    const char *request_id,
    const char *turn_id,
    const char *playback_id,
    bool physical_stop_done,
    bool detector_released) {
    if (!recovery || ws_epoch == 0U || awake_generation == 0U ||
        speaker_generation == 0U || deadline_ms == 0U ||
        !nonempty(turn_id) || !nonempty(playback_id)) {
        return false;
    }
    if (recovery->active) {
        return false;
    }
    memset(recovery, 0, sizeof(*recovery));
    recovery->active = true;
    recovery->physical_stop_done = physical_stop_done;
    recovery->detector_released = detector_released;
    recovery->ws_epoch = ws_epoch;
    recovery->awake_generation = awake_generation;
    recovery->speaker_generation = speaker_generation;
    recovery->deadline_ms = deadline_ms;
    if (nonempty(request_id)) {
        (void)strncpy(recovery->request_id,
                      request_id,
                      sizeof(recovery->request_id) - 1U);
    }
    (void)strncpy(recovery->turn_id,
                  turn_id,
                  sizeof(recovery->turn_id) - 1U);
    (void)strncpy(recovery->playback_id,
                  playback_id,
                  sizeof(recovery->playback_id) - 1U);
    return true;
}

void nuwax_touch_cancel_recovery_clear(
    nuwax_touch_cancel_recovery_t *recovery) {
    if (recovery) {
        memset(recovery, 0, sizeof(*recovery));
    }
}

bool nuwax_touch_cancel_recovery_owner_matches(
    const nuwax_touch_cancel_recovery_t *recovery,
    uint32_t ws_epoch,
    uint32_t awake_generation) {
    return recovery && recovery->active && recovery->ws_epoch == ws_epoch &&
           recovery->awake_generation == awake_generation;
}

bool nuwax_touch_cancel_recovery_stop_matches(
    const nuwax_touch_cancel_recovery_t *recovery,
    uint32_t ws_epoch,
    uint32_t speaker_generation) {
    return recovery && recovery->active && recovery->ws_epoch == ws_epoch &&
           recovery->speaker_generation == speaker_generation;
}

bool nuwax_touch_cancel_recovery_mark_physical_stop(
    nuwax_touch_cancel_recovery_t *recovery,
    uint32_t ws_epoch,
    uint32_t speaker_generation) {
    if (!nuwax_touch_cancel_recovery_stop_matches(
            recovery, ws_epoch, speaker_generation)) {
        return false;
    }
    recovery->physical_stop_done = true;
    return true;
}

bool nuwax_touch_cancel_recovery_mark_detector_released(
    nuwax_touch_cancel_recovery_t *recovery) {
    if (!recovery || !recovery->active) {
        return false;
    }
    recovery->detector_released = true;
    return true;
}

bool nuwax_touch_cancel_recovery_deadline_expired(
    const nuwax_touch_cancel_recovery_t *recovery,
    uint32_t now_ms) {
    return recovery && recovery->active &&
           (int32_t)(now_ms - recovery->deadline_ms) >= 0;
}

bool nuwax_touch_cancel_recovery_ack_matches(
    const nuwax_touch_cancel_recovery_t *recovery,
    uint32_t ws_epoch,
    const char *request_id,
    const char *turn_id,
    const char *playback_id) {
    if (!recovery || !recovery->active || recovery->ws_epoch != ws_epoch) {
        return false;
    }
    bool seen = false;
    return (equal_optional(recovery->request_id, request_id, &seen) &&
            equal_optional(recovery->turn_id, turn_id, &seen) &&
            equal_optional(recovery->playback_id, playback_id, &seen) &&
            seen);
}

bool nuwax_touch_cancel_recovery_mark_ack(
    nuwax_touch_cancel_recovery_t *recovery,
    uint32_t ws_epoch,
    const char *request_id,
    const char *turn_id,
    const char *playback_id,
    bool ok) {
    if (!nuwax_touch_cancel_recovery_ack_matches(
            recovery, ws_epoch, request_id, turn_id, playback_id)) {
        return false;
    }
    recovery->cancel_ack_received = true;
    recovery->cancel_ack_ok = ok;
    return true;
}
