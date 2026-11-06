#include "sleep_transaction.h"

#include <stddef.h>
#include <string.h>

static void copy_string(char *destination,
                        size_t destination_size,
                        const char *source) {
    if (!destination || destination_size == 0U) {
        return;
    }
    if (!source) {
        source = "";
    }
    size_t length = strlen(source);
    if (length >= destination_size) {
        length = destination_size - 1U;
    }
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static uint32_t next_token(uint32_t token) {
    token++;
    return token == 0U ? 1U : token;
}

static bool request_matches(const nuwax_sleep_transaction_t *transaction,
                            uint32_t ws_epoch,
                            const char *request_id) {
    return transaction && ws_epoch != 0U &&
           transaction->ws_epoch == ws_epoch && request_id &&
           request_id[0] != '\0' &&
           strcmp(transaction->request_id, request_id) == 0;
}

void nuwax_sleep_transaction_init(nuwax_sleep_transaction_t *transaction) {
    if (!transaction) {
        return;
    }
    memset(transaction, 0, sizeof(*transaction));
    transaction->state = NUWAX_SLEEP_TRANSACTION_IDLE;
}

nuwax_sleep_request_result_t nuwax_sleep_transaction_request(
    nuwax_sleep_transaction_t *transaction,
    uint32_t now_ms,
    uint32_t timeout_ms,
    uint32_t ws_epoch,
    uint32_t speaker_generation,
    const char *request_id,
    const char *command,
    const char *turn_id,
    bool local_sleeping) {
    if (!transaction || ws_epoch == 0U || timeout_ms == 0U ||
        !request_id || request_id[0] == '\0' ||
        !command || command[0] == '\0') {
        return NUWAX_SLEEP_REQUEST_INVALID;
    }

    if (request_matches(transaction, ws_epoch, request_id)) {
        return transaction->state == NUWAX_SLEEP_TRANSACTION_SLEEPING
                   ? NUWAX_SLEEP_REQUEST_DUPLICATE_COMMITTED
                   : NUWAX_SLEEP_REQUEST_DUPLICATE_ACTIVE;
    }
    if (local_sleeping ||
        transaction->state == NUWAX_SLEEP_TRANSACTION_SLEEPING) {
        return NUWAX_SLEEP_REQUEST_ALREADY_SLEEPING;
    }
    if (transaction->active ||
        transaction->state != NUWAX_SLEEP_TRANSACTION_IDLE) {
        return NUWAX_SLEEP_REQUEST_BUSY;
    }

    const uint32_t token = next_token(transaction->token);
    memset(transaction, 0, sizeof(*transaction));
    transaction->state = NUWAX_SLEEP_TRANSACTION_REQUESTED;
    transaction->active = true;
    transaction->token = token;
    transaction->ws_epoch = ws_epoch;
    transaction->speaker_generation = speaker_generation;
    transaction->requested_ms = now_ms;
    transaction->deadline_ms = now_ms + timeout_ms;
    copy_string(transaction->request_id,
                sizeof(transaction->request_id),
                request_id);
    copy_string(transaction->command,
                sizeof(transaction->command),
                command);
    copy_string(transaction->turn_id,
                sizeof(transaction->turn_id),
                turn_id);
    transaction->speaker_stopped = speaker_generation == 0U;
    transaction->local_request = false;
    return NUWAX_SLEEP_REQUEST_STARTED;
}

nuwax_sleep_request_result_t nuwax_sleep_transaction_request_local(
    nuwax_sleep_transaction_t *transaction,
    uint32_t now_ms,
    uint32_t timeout_ms,
    uint32_t speaker_generation,
    bool local_sleeping) {
    if (!transaction || timeout_ms == 0U) {
        return NUWAX_SLEEP_REQUEST_INVALID;
    }
    if (local_sleeping ||
        transaction->state == NUWAX_SLEEP_TRANSACTION_SLEEPING) {
        return NUWAX_SLEEP_REQUEST_ALREADY_SLEEPING;
    }
    if (transaction->active ||
        transaction->state != NUWAX_SLEEP_TRANSACTION_IDLE) {
        return NUWAX_SLEEP_REQUEST_BUSY;
    }

    const uint32_t token = next_token(transaction->token);
    memset(transaction, 0, sizeof(*transaction));
    transaction->state = NUWAX_SLEEP_TRANSACTION_REQUESTED;
    transaction->active = true;
    transaction->token = token;
    transaction->speaker_generation = speaker_generation;
    transaction->requested_ms = now_ms;
    transaction->deadline_ms = now_ms + timeout_ms;
    copy_string(transaction->request_id,
                sizeof(transaction->request_id),
                "local_power_key");
    copy_string(transaction->command,
                sizeof(transaction->command),
                "local_sleep");
    transaction->speaker_stopped = speaker_generation == 0U;
    transaction->local_request = true;
    return NUWAX_SLEEP_REQUEST_STARTED;
}

bool nuwax_sleep_transaction_begin_quiescing(
    nuwax_sleep_transaction_t *transaction,
    uint32_t token) {
    if (!transaction || !transaction->active || token == 0U ||
        transaction->token != token ||
        transaction->state != NUWAX_SLEEP_TRANSACTION_REQUESTED) {
        return false;
    }
    transaction->state = NUWAX_SLEEP_TRANSACTION_QUIESCING;
    return true;
}

bool nuwax_sleep_transaction_mark_capture_released(
    nuwax_sleep_transaction_t *transaction,
    uint32_t token) {
    if (!transaction || !transaction->active || token == 0U ||
        transaction->token != token ||
        (transaction->state != NUWAX_SLEEP_TRANSACTION_REQUESTED &&
         transaction->state != NUWAX_SLEEP_TRANSACTION_QUIESCING &&
         transaction->state != NUWAX_SLEEP_TRANSACTION_RECOVERING)) {
        return false;
    }
    transaction->capture_released = true;
    return true;
}

bool nuwax_sleep_transaction_mark_protocol_closed(
    nuwax_sleep_transaction_t *transaction,
    uint32_t token,
    uint32_t ws_epoch) {
    if (!transaction || !transaction->active || token == 0U ||
        transaction->token != token ||
        ((transaction->ws_epoch != 0U || ws_epoch != 0U) &&
         transaction->ws_epoch != ws_epoch) ||
        (transaction->state != NUWAX_SLEEP_TRANSACTION_REQUESTED &&
         transaction->state != NUWAX_SLEEP_TRANSACTION_QUIESCING &&
         transaction->state != NUWAX_SLEEP_TRANSACTION_RECOVERING)) {
        return false;
    }
    transaction->protocol_closed = true;
    return true;
}

bool nuwax_sleep_transaction_mark_speaker_stopped(
    nuwax_sleep_transaction_t *transaction,
    uint32_t token,
    uint32_t ws_epoch,
    uint32_t speaker_generation) {
    if (!transaction || !transaction->active || token == 0U ||
        transaction->token != token || speaker_generation == 0U ||
        ((transaction->ws_epoch != 0U || ws_epoch != 0U) &&
         transaction->ws_epoch != ws_epoch) ||
        transaction->speaker_generation != speaker_generation ||
        (transaction->state != NUWAX_SLEEP_TRANSACTION_REQUESTED &&
         transaction->state != NUWAX_SLEEP_TRANSACTION_QUIESCING &&
         transaction->state != NUWAX_SLEEP_TRANSACTION_RECOVERING)) {
        return false;
    }
    transaction->speaker_stopped = true;
    return true;
}

bool nuwax_sleep_transaction_ready_to_commit(
    const nuwax_sleep_transaction_t *transaction) {
    return transaction && transaction->active &&
           (transaction->state == NUWAX_SLEEP_TRANSACTION_QUIESCING ||
            transaction->state == NUWAX_SLEEP_TRANSACTION_RECOVERING) &&
           transaction->capture_released && transaction->speaker_stopped &&
           transaction->protocol_closed;
}

bool nuwax_sleep_transaction_begin_committing(
    nuwax_sleep_transaction_t *transaction,
    uint32_t token) {
    if (!transaction || token == 0U || transaction->token != token ||
        !nuwax_sleep_transaction_ready_to_commit(transaction)) {
        return false;
    }
    transaction->state = NUWAX_SLEEP_TRANSACTION_COMMITTING;
    return true;
}

bool nuwax_sleep_transaction_commit_sleeping(
    nuwax_sleep_transaction_t *transaction,
    uint32_t token) {
    if (!transaction || !transaction->active || token == 0U ||
        transaction->token != token ||
        transaction->state != NUWAX_SLEEP_TRANSACTION_COMMITTING) {
        return false;
    }
    transaction->state = NUWAX_SLEEP_TRANSACTION_SLEEPING;
    transaction->active = false;
    return true;
}

bool nuwax_sleep_transaction_abort(
    nuwax_sleep_transaction_t *transaction,
    uint32_t token) {
    if (!transaction || !transaction->active || token == 0U ||
        transaction->token != token ||
        transaction->state == NUWAX_SLEEP_TRANSACTION_IDLE ||
        transaction->state == NUWAX_SLEEP_TRANSACTION_SLEEPING) {
        return false;
    }
    const uint32_t invalidated_token = next_token(transaction->token);
    memset(transaction, 0, sizeof(*transaction));
    transaction->state = NUWAX_SLEEP_TRANSACTION_IDLE;
    transaction->token = invalidated_token;
    return true;
}

bool nuwax_sleep_transaction_mark_ack_sent(
    nuwax_sleep_transaction_t *transaction,
    uint32_t token,
    uint32_t ws_epoch) {
    if (!transaction || token == 0U || transaction->token != token ||
        transaction->state != NUWAX_SLEEP_TRANSACTION_SLEEPING ||
        transaction->ws_epoch != ws_epoch) {
        return false;
    }
    transaction->ack_sent = true;
    return true;
}

bool nuwax_sleep_transaction_deadline_expired(
    const nuwax_sleep_transaction_t *transaction,
    uint32_t now_ms) {
    return transaction && transaction->active &&
           (int32_t)(now_ms - transaction->deadline_ms) >= 0;
}

bool nuwax_sleep_transaction_begin_recovery_after_timeout(
    nuwax_sleep_transaction_t *transaction,
    uint32_t token,
    uint32_t now_ms,
    uint32_t recovery_timeout_ms) {
    if (!transaction || !transaction->active || token == 0U ||
        transaction->token != token ||
        transaction->state != NUWAX_SLEEP_TRANSACTION_QUIESCING ||
        recovery_timeout_ms == 0U ||
        !nuwax_sleep_transaction_deadline_expired(transaction, now_ms)) {
        return false;
    }
    transaction->state = NUWAX_SLEEP_TRANSACTION_RECOVERING;
    transaction->deadline_ms = now_ms + recovery_timeout_ms;
    return true;
}

bool nuwax_sleep_transaction_blocks_audio(
    const nuwax_sleep_transaction_t *transaction) {
    return transaction &&
           (transaction->active ||
            transaction->state == NUWAX_SLEEP_TRANSACTION_SLEEPING);
}

bool nuwax_sleep_transaction_requires_local_sleep_reconcile(
    const nuwax_sleep_transaction_t *transaction,
    bool local_sleeping) {
    return transaction &&
           transaction->state == NUWAX_SLEEP_TRANSACTION_SLEEPING &&
           !local_sleeping;
}

void nuwax_sleep_transaction_reset_for_wake(
    nuwax_sleep_transaction_t *transaction) {
    if (!transaction) {
        return;
    }
    const uint32_t token = next_token(transaction->token);
    memset(transaction, 0, sizeof(*transaction));
    transaction->state = NUWAX_SLEEP_TRANSACTION_IDLE;
    transaction->token = token;
}

const char *nuwax_sleep_transaction_state_name(
    nuwax_sleep_transaction_state_t state) {
    switch (state) {
        case NUWAX_SLEEP_TRANSACTION_IDLE:
            return "idle";
        case NUWAX_SLEEP_TRANSACTION_REQUESTED:
            return "requested";
        case NUWAX_SLEEP_TRANSACTION_QUIESCING:
            return "quiescing";
        case NUWAX_SLEEP_TRANSACTION_RECOVERING:
            return "recovering";
        case NUWAX_SLEEP_TRANSACTION_COMMITTING:
            return "committing";
        case NUWAX_SLEEP_TRANSACTION_SLEEPING:
            return "sleeping";
        default:
            return "unknown";
    }
}

void nuwax_deferred_sleep_intent_init(
    nuwax_deferred_sleep_intent_t *intent) {
    if (!intent) {
        return;
    }
    memset(intent, 0, sizeof(*intent));
}

nuwax_sleep_request_result_t nuwax_deferred_sleep_intent_request(
    nuwax_deferred_sleep_intent_t *intent,
    uint32_t now_ms,
    uint32_t timeout_ms,
    uint32_t ws_epoch,
    const char *request_id,
    const char *command,
    const char *turn_id,
    bool local_sleeping) {
    if (!intent || timeout_ms == 0U || ws_epoch == 0U ||
        !request_id || request_id[0] == '\0' ||
        !command || command[0] == '\0') {
        return NUWAX_SLEEP_REQUEST_INVALID;
    }
    if (intent->active) {
        return intent->ws_epoch == ws_epoch &&
                       strcmp(intent->request_id, request_id) == 0
                   ? NUWAX_SLEEP_REQUEST_DUPLICATE_ACTIVE
                   : NUWAX_SLEEP_REQUEST_BUSY;
    }
    if (local_sleeping) {
        return NUWAX_SLEEP_REQUEST_ALREADY_SLEEPING;
    }

    const uint32_t token = next_token(intent->token);
    memset(intent, 0, sizeof(*intent));
    intent->active = true;
    intent->token = token;
    intent->ws_epoch = ws_epoch;
    intent->requested_ms = now_ms;
    intent->deadline_ms = now_ms + timeout_ms;
    copy_string(intent->request_id,
                sizeof(intent->request_id),
                request_id);
    copy_string(intent->command, sizeof(intent->command), command);
    copy_string(intent->turn_id, sizeof(intent->turn_id), turn_id);
    return NUWAX_SLEEP_REQUEST_STARTED;
}

bool nuwax_deferred_sleep_intent_ready_to_execute(
    const nuwax_deferred_sleep_intent_t *intent,
    uint32_t now_ms,
    bool turn_active,
    bool speaker_pending,
    bool playback_terminal_active) {
    if (!intent || !intent->active) {
        return false;
    }
    const bool deadline_expired =
        (int32_t)(now_ms - intent->deadline_ms) >= 0;
    return deadline_expired ||
           (!turn_active && !speaker_pending && !playback_terminal_active);
}

bool nuwax_deferred_sleep_turn_owner_active(
    bool turn_guard_active,
    nuwax_app_state_t app_state,
    bool capture_busy,
    bool prelisten_capture_active) {
    /* SPEAKING is a presentation state, not playback ownership. Physical
     * playback and its protocol terminal are gated independently by the
     * caller, so a stale UI state must not hold a completed reply open. */
    return turn_guard_active || app_state == NUWAX_APP_STATE_THINKING ||
           capture_busy || prelisten_capture_active;
}

bool nuwax_deferred_sleep_intent_extend(
    nuwax_deferred_sleep_intent_t *intent,
    uint32_t token,
    uint32_t now_ms,
    uint32_t timeout_ms) {
    if (!intent || !intent->active || token == 0U ||
        intent->token != token || timeout_ms == 0U) {
        return false;
    }
    intent->deadline_ms = now_ms + timeout_ms;
    return true;
}

bool nuwax_deferred_sleep_intent_consume(
    nuwax_deferred_sleep_intent_t *intent,
    uint32_t token) {
    if (!intent || !intent->active || token == 0U ||
        intent->token != token) {
        return false;
    }
    memset(intent, 0, sizeof(*intent));
    intent->token = token;
    return true;
}
