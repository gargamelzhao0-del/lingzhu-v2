#pragma once

#include <stdbool.h>
#include <stdint.h>

/* A local touch cancel is independent from the cloud turn guard.  The cloud
 * turn may be quarantined immediately, while this transaction remains bound
 * to the physical speaker generation until the I2S stop is observed. */
typedef struct {
    bool active;
    bool physical_stop_done;
    bool detector_released;
    bool cancel_ack_received;
    bool cancel_ack_ok;
    bool head_touch_agent_handoff;
    bool cancel_ack_grace_started;
    uint8_t reset_attempts;
    uint32_t ws_epoch;
    uint32_t awake_generation;
    uint32_t speaker_generation;
    uint32_t deadline_ms;
    char request_id[40];
    char turn_id[48];
    char playback_id[48];
} nuwax_touch_cancel_recovery_t;

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
    bool detector_released);

void nuwax_touch_cancel_recovery_clear(
    nuwax_touch_cancel_recovery_t *recovery);

bool nuwax_touch_cancel_recovery_owner_matches(
    const nuwax_touch_cancel_recovery_t *recovery,
    uint32_t ws_epoch,
    uint32_t awake_generation);

bool nuwax_touch_cancel_recovery_stop_matches(
    const nuwax_touch_cancel_recovery_t *recovery,
    uint32_t ws_epoch,
    uint32_t speaker_generation);

bool nuwax_touch_cancel_recovery_mark_physical_stop(
    nuwax_touch_cancel_recovery_t *recovery,
    uint32_t ws_epoch,
    uint32_t speaker_generation);

bool nuwax_touch_cancel_recovery_mark_detector_released(
    nuwax_touch_cancel_recovery_t *recovery);

bool nuwax_touch_cancel_recovery_deadline_expired(
    const nuwax_touch_cancel_recovery_t *recovery,
    uint32_t now_ms);

/* ACK fields are optional in the formal protocol.  Match every field that is
 * present and require at least one identity field, so an omitted ACK payload
 * can never complete an unrelated touch transaction. */
bool nuwax_touch_cancel_recovery_ack_matches(
    const nuwax_touch_cancel_recovery_t *recovery,
    uint32_t ws_epoch,
    const char *request_id,
    const char *turn_id,
    const char *playback_id);

bool nuwax_touch_cancel_recovery_mark_ack(
    nuwax_touch_cancel_recovery_t *recovery,
    uint32_t ws_epoch,
    const char *request_id,
    const char *turn_id,
    const char *playback_id,
    bool ok);
