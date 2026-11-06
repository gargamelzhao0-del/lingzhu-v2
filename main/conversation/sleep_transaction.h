#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_state.h"

#define NUWAX_SLEEP_REQUEST_ID_MAX_LEN 40U
#define NUWAX_SLEEP_COMMAND_MAX_LEN 32U
#define NUWAX_SLEEP_TURN_ID_MAX_LEN 48U

typedef enum {
    NUWAX_SLEEP_TRANSACTION_IDLE = 0,
    NUWAX_SLEEP_TRANSACTION_REQUESTED,
    NUWAX_SLEEP_TRANSACTION_QUIESCING,
    NUWAX_SLEEP_TRANSACTION_RECOVERING,
    NUWAX_SLEEP_TRANSACTION_COMMITTING,
    NUWAX_SLEEP_TRANSACTION_SLEEPING,
} nuwax_sleep_transaction_state_t;

typedef enum {
    NUWAX_SLEEP_REQUEST_INVALID = 0,
    NUWAX_SLEEP_REQUEST_STARTED,
    NUWAX_SLEEP_REQUEST_DUPLICATE_ACTIVE,
    NUWAX_SLEEP_REQUEST_DUPLICATE_COMMITTED,
    NUWAX_SLEEP_REQUEST_ALREADY_SLEEPING,
    NUWAX_SLEEP_REQUEST_BUSY,
    NUWAX_SLEEP_REQUEST_STALE_SCOPE,
    NUWAX_SLEEP_REQUEST_BUSY_EXCLUSIVE,
} nuwax_sleep_request_result_t;

typedef struct {
    nuwax_sleep_transaction_state_t state;
    bool active;
    uint32_t token;
    uint32_t ws_epoch;
    uint32_t speaker_generation;
    uint32_t requested_ms;
    uint32_t deadline_ms;
    char request_id[NUWAX_SLEEP_REQUEST_ID_MAX_LEN];
    char command[NUWAX_SLEEP_COMMAND_MAX_LEN];
    char turn_id[NUWAX_SLEEP_TURN_ID_MAX_LEN];
    bool capture_released;
    bool speaker_stopped;
    bool protocol_closed;
    bool ack_sent;
    bool local_request;
} nuwax_sleep_transaction_t;

typedef struct {
    bool active;
    uint32_t token;
    uint32_t ws_epoch;
    uint32_t requested_ms;
    uint32_t deadline_ms;
    char request_id[NUWAX_SLEEP_REQUEST_ID_MAX_LEN];
    char command[NUWAX_SLEEP_COMMAND_MAX_LEN];
    char turn_id[NUWAX_SLEEP_TURN_ID_MAX_LEN];
} nuwax_deferred_sleep_intent_t;

void nuwax_sleep_transaction_init(nuwax_sleep_transaction_t *transaction);
nuwax_sleep_request_result_t nuwax_sleep_transaction_request(
    nuwax_sleep_transaction_t *transaction,
    uint32_t now_ms,
    uint32_t timeout_ms,
    uint32_t ws_epoch,
    uint32_t speaker_generation,
    const char *request_id,
    const char *command,
    const char *turn_id,
    bool local_sleeping);

/* Local controls such as the physical power key do not have a cloud request
 * id or a WebSocket epoch. They still use the same token-bound quiescence
 * barriers and therefore cannot bypass capture/speaker ownership. */
nuwax_sleep_request_result_t nuwax_sleep_transaction_request_local(
    nuwax_sleep_transaction_t *transaction,
    uint32_t now_ms,
    uint32_t timeout_ms,
    uint32_t speaker_generation,
    bool local_sleeping);
bool nuwax_sleep_transaction_begin_quiescing(
    nuwax_sleep_transaction_t *transaction,
    uint32_t token);
bool nuwax_sleep_transaction_mark_capture_released(
    nuwax_sleep_transaction_t *transaction,
    uint32_t token);
bool nuwax_sleep_transaction_mark_protocol_closed(
    nuwax_sleep_transaction_t *transaction,
    uint32_t token,
    uint32_t ws_epoch);
bool nuwax_sleep_transaction_mark_speaker_stopped(
    nuwax_sleep_transaction_t *transaction,
    uint32_t token,
    uint32_t ws_epoch,
    uint32_t speaker_generation);
bool nuwax_sleep_transaction_ready_to_commit(
    const nuwax_sleep_transaction_t *transaction);
bool nuwax_sleep_transaction_begin_committing(
    nuwax_sleep_transaction_t *transaction,
    uint32_t token);
bool nuwax_sleep_transaction_commit_sleeping(
    nuwax_sleep_transaction_t *transaction,
    uint32_t token);
bool nuwax_sleep_transaction_abort(
    nuwax_sleep_transaction_t *transaction,
    uint32_t token);
bool nuwax_sleep_transaction_mark_ack_sent(
    nuwax_sleep_transaction_t *transaction,
    uint32_t token,
    uint32_t ws_epoch);
bool nuwax_sleep_transaction_deadline_expired(
    const nuwax_sleep_transaction_t *transaction,
    uint32_t now_ms);
bool nuwax_sleep_transaction_begin_recovery_after_timeout(
    nuwax_sleep_transaction_t *transaction,
    uint32_t token,
    uint32_t now_ms,
    uint32_t recovery_timeout_ms);
bool nuwax_sleep_transaction_blocks_audio(
    const nuwax_sleep_transaction_t *transaction);
/* A committed transaction survives a WebSocket reconnect. The transport
 * owner must republish local SLEEPING when its app-state snapshot drifted. */
bool nuwax_sleep_transaction_requires_local_sleep_reconcile(
    const nuwax_sleep_transaction_t *transaction,
    bool local_sleeping);
void nuwax_sleep_transaction_reset_for_wake(
    nuwax_sleep_transaction_t *transaction);
const char *nuwax_sleep_transaction_state_name(
    nuwax_sleep_transaction_state_t state);

void nuwax_deferred_sleep_intent_init(
    nuwax_deferred_sleep_intent_t *intent);
nuwax_sleep_request_result_t nuwax_deferred_sleep_intent_request(
    nuwax_deferred_sleep_intent_t *intent,
    uint32_t now_ms,
    uint32_t timeout_ms,
    uint32_t ws_epoch,
    const char *request_id,
    const char *command,
    const char *turn_id,
    bool local_sleeping);
bool nuwax_deferred_sleep_intent_ready_to_execute(
    const nuwax_deferred_sleep_intent_t *intent,
    uint32_t now_ms,
    bool turn_active,
    bool speaker_pending,
    bool playback_terminal_active);
bool nuwax_deferred_sleep_turn_owner_active(
    bool turn_guard_active,
    nuwax_app_state_t app_state,
    bool capture_busy,
    bool prelisten_capture_active);
bool nuwax_deferred_sleep_intent_extend(
    nuwax_deferred_sleep_intent_t *intent,
    uint32_t token,
    uint32_t now_ms,
    uint32_t timeout_ms);
bool nuwax_deferred_sleep_intent_consume(
    nuwax_deferred_sleep_intent_t *intent,
    uint32_t token);
