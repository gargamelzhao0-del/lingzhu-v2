#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "ws_transport.h"

/* This recovery is deliberately single-session.  The product audio owner
 * serializes microphone sessions, so retaining one terminal packet is enough
 * and avoids a queue/task in the failure path. */
#define NUWAX_AUDIO_TERMINAL_RECOVERY_MODE_MAX 32U
#define NUWAX_AUDIO_TERMINAL_RECOVERY_SESSION_MAX 48U
#define NUWAX_AUDIO_TERMINAL_RECOVERY_REQUEST_MAX 48U
#define NUWAX_AUDIO_TERMINAL_RECOVERY_DEFAULT_ATTEMPT_TIMEOUT_MS 1000U

typedef struct {
    bool valid;
    bool success;
    bool quarantined;
    uint32_t ws_epoch;
    uint32_t failures;
    uint32_t age_ms;
    char mode[NUWAX_AUDIO_TERMINAL_RECOVERY_MODE_MAX];
    char audio_session_id[NUWAX_AUDIO_TERMINAL_RECOVERY_SESSION_MAX];
} nuwax_audio_terminal_recovery_completion_t;

/* Arm an END + listen-stop transaction.  When end_committed is true the
 * binary END is already known to have reached the transport and packet may be
 * NULL; otherwise packet must contain the exact safely-unsent terminal frame.
 * The caller must finish writing packet before this function publishes the
 * pending state. */
esp_err_t nuwax_audio_terminal_recovery_arm(
    nuwax_ws_client_t *ws,
    uint32_t ws_epoch,
    const uint8_t *packet,
    size_t packet_len,
    bool end_committed,
    const char *mode,
    const char *audio_session_id,
    const char *request_id,
    uint32_t attempt_timeout_ms);

/* Service at a main-owner frame/control boundary.  ESP_ERR_NOT_FINISHED
 * means the transaction remains pending and the caller should return to its
 * normal loop; it is not a transport-disconnect indication. */
esp_err_t nuwax_audio_terminal_recovery_service(nuwax_ws_client_t *ws);

/* Includes a completion that still has to be consumed by the main owner.
 * New audio transactions must wait until this becomes false. */
bool nuwax_audio_terminal_recovery_pending(void);

/* Returns a bounded delay until recovery needs service again. A zero return
 * means service is due now. The caller should still cap the argument so
 * control-plane work is serviced regularly while the audio transaction is
 * serialized. */
uint32_t nuwax_audio_terminal_recovery_next_service_ms(
    uint32_t max_wait_ms);

/* Resolve a transaction when a late in-flight END and its sender-task
 * follow-up both complete after the owner had already armed recovery. */
bool nuwax_audio_terminal_recovery_complete_if_matches(
    nuwax_ws_client_t *ws,
    uint32_t ws_epoch,
    const char *mode,
    const char *audio_session_id);

/* Completion is latched until the owner consumes it. */
bool nuwax_audio_terminal_recovery_take_completion(
    nuwax_audio_terminal_recovery_completion_t *out);
