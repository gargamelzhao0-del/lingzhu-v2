#include "notification_playback.h"

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

static bool valid_id(const char *value) {
    return value && value[0] != '\0';
}

static bool token_matches(const nuwax_notification_transaction_t *transaction,
                          uint32_t token) {
    return transaction && transaction->active && token != 0U &&
           transaction->token == token;
}

static bool prepare_matches(
    const nuwax_notification_transaction_t *transaction,
    uint32_t ws_epoch,
    const char *request_id,
    const char *notify_id) {
    return transaction && ws_epoch != 0U &&
           transaction->ws_epoch == ws_epoch && valid_id(request_id) &&
           valid_id(notify_id) &&
           strcmp(transaction->prepare_request_id, request_id) == 0 &&
           strcmp(transaction->notify_id, notify_id) == 0;
}

static bool begin_matches(
    const nuwax_notification_transaction_t *transaction,
    uint32_t ws_epoch,
    const char *request_id,
    const char *notify_id,
    const char *playback_id) {
    return transaction && ws_epoch != 0U &&
           transaction->ws_epoch == ws_epoch && valid_id(request_id) &&
           valid_id(notify_id) && valid_id(playback_id) &&
           strcmp(transaction->begin_request_id, request_id) == 0 &&
           strcmp(transaction->notify_id, notify_id) == 0 &&
           strcmp(transaction->playback_id, playback_id) == 0;
}

static bool end_matches(
    const nuwax_notification_transaction_t *transaction,
    uint32_t ws_epoch,
    const char *request_id,
    const char *notify_id,
    const char *playback_id) {
    return transaction && ws_epoch != 0U &&
           transaction->ws_epoch == ws_epoch && valid_id(request_id) &&
           valid_id(notify_id) && valid_id(playback_id) &&
           strcmp(transaction->end_request_id, request_id) == 0 &&
           strcmp(transaction->notify_id, notify_id) == 0 &&
           strcmp(transaction->playback_id, playback_id) == 0;
}

static void begin_new_transaction(
    nuwax_notification_transaction_t *transaction,
    nuwax_notification_state_t state,
    uint32_t now_ms,
    uint32_t timeout_ms,
    uint32_t ws_epoch,
    uint32_t previous_speaker_generation,
    bool origin_sleeping) {
    const uint32_t token = next_token(transaction->token);
    memset(transaction, 0, sizeof(*transaction));
    transaction->state = state;
    transaction->active = true;
    transaction->origin_sleeping = origin_sleeping;
    transaction->token = token;
    transaction->ws_epoch = ws_epoch;
    transaction->requested_ms = now_ms;
    transaction->deadline_ms = now_ms + timeout_ms;
    transaction->previous_speaker_generation = previous_speaker_generation;
    transaction->previous_speaker_stopped =
        previous_speaker_generation == 0U;
}

void nuwax_notification_init(nuwax_notification_transaction_t *transaction) {
    if (!transaction) {
        return;
    }
    memset(transaction, 0, sizeof(*transaction));
    transaction->state = NUWAX_NOTIFICATION_STATE_IDLE;
}

void nuwax_notification_reset(nuwax_notification_transaction_t *transaction) {
    if (!transaction) {
        return;
    }
    const uint32_t token = next_token(transaction->token);
    memset(transaction, 0, sizeof(*transaction));
    transaction->state = NUWAX_NOTIFICATION_STATE_IDLE;
    transaction->token = token;
}

nuwax_notification_request_result_t nuwax_notification_request_prepare(
    nuwax_notification_transaction_t *transaction,
    uint32_t now_ms,
    uint32_t timeout_ms,
    uint32_t ws_epoch,
    uint32_t previous_speaker_generation,
    const char *request_id,
    const char *notify_id,
    bool origin_sleeping) {
    if (!transaction || timeout_ms == 0U || ws_epoch == 0U ||
        !valid_id(request_id) || !valid_id(notify_id)) {
        return NUWAX_NOTIFICATION_REQUEST_INVALID;
    }
    if (prepare_matches(transaction, ws_epoch, request_id, notify_id)) {
        return NUWAX_NOTIFICATION_REQUEST_DUPLICATE;
    }
    if (transaction->active) {
        return transaction->ws_epoch == ws_epoch &&
                       strcmp(transaction->notify_id, notify_id) == 0
                   ? NUWAX_NOTIFICATION_REQUEST_CONFLICT
                   : NUWAX_NOTIFICATION_REQUEST_BUSY;
    }
    begin_new_transaction(transaction,
                          NUWAX_NOTIFICATION_STATE_PREPARING,
                          now_ms,
                          timeout_ms,
                          ws_epoch,
                          previous_speaker_generation,
                          origin_sleeping);
    copy_string(transaction->prepare_request_id,
                sizeof(transaction->prepare_request_id),
                request_id);
    copy_string(transaction->notify_id,
                sizeof(transaction->notify_id),
                notify_id);
    return NUWAX_NOTIFICATION_REQUEST_STARTED;
}

nuwax_notification_request_result_t nuwax_notification_request_begin(
    nuwax_notification_transaction_t *transaction,
    uint32_t now_ms,
    uint32_t timeout_ms,
    uint32_t ws_epoch,
    uint32_t previous_speaker_generation,
    const char *request_id,
    const char *notify_id,
    const char *playback_id,
    bool local_sleeping) {
    if (!transaction || timeout_ms == 0U || ws_epoch == 0U ||
        !valid_id(request_id) || !valid_id(notify_id) ||
        !valid_id(playback_id)) {
        return NUWAX_NOTIFICATION_REQUEST_INVALID;
    }
    if (begin_matches(transaction,
                      ws_epoch,
                      request_id,
                      notify_id,
                      playback_id)) {
        return NUWAX_NOTIFICATION_REQUEST_DUPLICATE;
    }
    if (transaction->active) {
        if (transaction->ws_epoch != ws_epoch) {
            return NUWAX_NOTIFICATION_REQUEST_BUSY;
        }
        if (strcmp(transaction->notify_id, notify_id) != 0) {
            return NUWAX_NOTIFICATION_REQUEST_CONFLICT;
        }
        if (transaction->state == NUWAX_NOTIFICATION_STATE_PREPARING) {
            return NUWAX_NOTIFICATION_REQUEST_NOT_READY;
        }
        if (transaction->state != NUWAX_NOTIFICATION_STATE_READY ||
            !transaction->prepare_ack_sent) {
            return NUWAX_NOTIFICATION_REQUEST_BUSY;
        }
        transaction->state = NUWAX_NOTIFICATION_STATE_BEGIN_PENDING;
        transaction->deadline_ms = now_ms + timeout_ms;
        copy_string(transaction->begin_request_id,
                    sizeof(transaction->begin_request_id),
                    request_id);
        copy_string(transaction->playback_id,
                    sizeof(transaction->playback_id),
                    playback_id);
        return NUWAX_NOTIFICATION_REQUEST_STARTED;
    }
    if (local_sleeping) {
        return NUWAX_NOTIFICATION_REQUEST_NOT_PREPARED;
    }
    begin_new_transaction(transaction,
                          NUWAX_NOTIFICATION_STATE_BEGIN_PENDING,
                          now_ms,
                          timeout_ms,
                          ws_epoch,
                          previous_speaker_generation,
                          false);
    copy_string(transaction->begin_request_id,
                sizeof(transaction->begin_request_id),
                request_id);
    copy_string(transaction->notify_id,
                sizeof(transaction->notify_id),
                notify_id);
    copy_string(transaction->playback_id,
                sizeof(transaction->playback_id),
                playback_id);
    return NUWAX_NOTIFICATION_REQUEST_STARTED;
}

bool nuwax_notification_mark_capture_released(
    nuwax_notification_transaction_t *transaction,
    uint32_t token) {
    if (!token_matches(transaction, token)) {
        return false;
    }
    transaction->capture_released = true;
    return true;
}

bool nuwax_notification_mark_wake_detector_released(
    nuwax_notification_transaction_t *transaction,
    uint32_t token) {
    if (!token_matches(transaction, token)) {
        return false;
    }
    transaction->wake_detector_released = true;
    return true;
}

bool nuwax_notification_mark_previous_speaker_stopped(
    nuwax_notification_transaction_t *transaction,
    uint32_t token,
    uint32_t ws_epoch,
    uint32_t speaker_generation) {
    if (!token_matches(transaction, token) || ws_epoch == 0U ||
        transaction->ws_epoch != ws_epoch || speaker_generation == 0U ||
        transaction->previous_speaker_generation != speaker_generation) {
        return false;
    }
    transaction->previous_speaker_stopped = true;
    return true;
}

bool nuwax_notification_prepare_ready(
    const nuwax_notification_transaction_t *transaction) {
    return transaction && transaction->active &&
           transaction->state == NUWAX_NOTIFICATION_STATE_PREPARING &&
           transaction->capture_released &&
           transaction->wake_detector_released &&
           transaction->previous_speaker_stopped;
}

bool nuwax_notification_mark_prepare_ready(
    nuwax_notification_transaction_t *transaction,
    uint32_t token) {
    if (!token_matches(transaction, token) ||
        !nuwax_notification_prepare_ready(transaction)) {
        return false;
    }
    transaction->state = NUWAX_NOTIFICATION_STATE_READY;
    return true;
}

bool nuwax_notification_mark_prepare_ack_sent(
    nuwax_notification_transaction_t *transaction,
    uint32_t token,
    uint32_t ws_epoch,
    uint32_t now_ms,
    uint32_t timeout_ms) {
    if (!token_matches(transaction, token) || ws_epoch == 0U ||
        transaction->ws_epoch != ws_epoch ||
        transaction->state != NUWAX_NOTIFICATION_STATE_READY ||
        timeout_ms == 0U) {
        return false;
    }
    transaction->prepare_ack_sent = true;
    transaction->deadline_ms = now_ms + timeout_ms;
    return true;
}

bool nuwax_notification_begin_ready(
    const nuwax_notification_transaction_t *transaction) {
    return transaction && transaction->active &&
           transaction->state == NUWAX_NOTIFICATION_STATE_BEGIN_PENDING &&
           transaction->capture_released &&
           transaction->wake_detector_released &&
           transaction->previous_speaker_stopped &&
           (!transaction->origin_sleeping || transaction->prepare_ack_sent);
}

bool nuwax_notification_mark_streaming(
    nuwax_notification_transaction_t *transaction,
    uint32_t token,
    uint32_t ws_epoch) {
    if (!token_matches(transaction, token) || ws_epoch == 0U ||
        transaction->ws_epoch != ws_epoch ||
        !nuwax_notification_begin_ready(transaction)) {
        return false;
    }
    transaction->state = NUWAX_NOTIFICATION_STATE_STREAMING;
    return true;
}

bool nuwax_notification_mark_begin_ack_sent(
    nuwax_notification_transaction_t *transaction,
    uint32_t token,
    uint32_t ws_epoch) {
    if (!token_matches(transaction, token) || ws_epoch == 0U ||
        transaction->ws_epoch != ws_epoch ||
        transaction->state != NUWAX_NOTIFICATION_STATE_STREAMING) {
        return false;
    }
    transaction->begin_ack_sent = true;
    return true;
}

bool nuwax_notification_refresh_stream_deadline(
    nuwax_notification_transaction_t *transaction,
    uint32_t token,
    uint32_t ws_epoch,
    uint32_t now_ms,
    uint32_t timeout_ms) {
    if (!token_matches(transaction, token) || ws_epoch == 0U ||
        transaction->ws_epoch != ws_epoch || timeout_ms == 0U ||
        (transaction->state != NUWAX_NOTIFICATION_STATE_STREAMING &&
         transaction->state != NUWAX_NOTIFICATION_STATE_DRAINING)) {
        return false;
    }
    transaction->deadline_ms = now_ms + timeout_ms;
    return true;
}

bool nuwax_notification_bind_speaker_generation(
    nuwax_notification_transaction_t *transaction,
    uint32_t token,
    uint32_t ws_epoch,
    uint32_t speaker_generation) {
    if (!token_matches(transaction, token) || ws_epoch == 0U ||
        transaction->ws_epoch != ws_epoch || speaker_generation == 0U ||
        (transaction->state != NUWAX_NOTIFICATION_STATE_STREAMING &&
         transaction->state != NUWAX_NOTIFICATION_STATE_DRAINING &&
         transaction->state != NUWAX_NOTIFICATION_STATE_CANCELLING)) {
        return false;
    }
    if (transaction->speaker_generation != 0U &&
        transaction->speaker_generation != speaker_generation) {
        return false;
    }
    transaction->speaker_generation = speaker_generation;
    return true;
}

nuwax_notification_request_result_t nuwax_notification_request_end(
    nuwax_notification_transaction_t *transaction,
    uint32_t now_ms,
    uint32_t timeout_ms,
    uint32_t ws_epoch,
    const char *request_id,
    const char *notify_id,
    const char *playback_id) {
    if (!transaction || timeout_ms == 0U || ws_epoch == 0U ||
        !valid_id(request_id) || !valid_id(notify_id) ||
        !valid_id(playback_id)) {
        return NUWAX_NOTIFICATION_REQUEST_INVALID;
    }
    if (end_matches(transaction,
                    ws_epoch,
                    request_id,
                    notify_id,
                    playback_id)) {
        return NUWAX_NOTIFICATION_REQUEST_DUPLICATE;
    }
    if (!transaction->active || transaction->ws_epoch != ws_epoch) {
        return NUWAX_NOTIFICATION_REQUEST_NOT_READY;
    }
    if (strcmp(transaction->notify_id, notify_id) != 0 ||
        strcmp(transaction->playback_id, playback_id) != 0) {
        return NUWAX_NOTIFICATION_REQUEST_CONFLICT;
    }
    if (transaction->state != NUWAX_NOTIFICATION_STATE_STREAMING &&
        transaction->state != NUWAX_NOTIFICATION_STATE_DRAINING) {
        return NUWAX_NOTIFICATION_REQUEST_BUSY;
    }
    transaction->state = NUWAX_NOTIFICATION_STATE_DRAINING;
    transaction->logical_end_seen = true;
    transaction->deadline_ms = now_ms + timeout_ms;
    copy_string(transaction->end_request_id,
                sizeof(transaction->end_request_id),
                request_id);
    return NUWAX_NOTIFICATION_REQUEST_STARTED;
}

bool nuwax_notification_mark_binary_end(
    nuwax_notification_transaction_t *transaction,
    uint32_t token,
    uint32_t ws_epoch,
    uint32_t speaker_generation) {
    if (!token_matches(transaction, token) || ws_epoch == 0U ||
        transaction->ws_epoch != ws_epoch || speaker_generation == 0U ||
        transaction->speaker_generation != speaker_generation) {
        return false;
    }
    transaction->binary_end_seen = true;
    return true;
}

bool nuwax_notification_mark_physical_done(
    nuwax_notification_transaction_t *transaction,
    uint32_t token,
    uint32_t ws_epoch,
    uint32_t speaker_generation) {
    if (!token_matches(transaction, token) || ws_epoch == 0U ||
        transaction->ws_epoch != ws_epoch || speaker_generation == 0U ||
        transaction->speaker_generation != speaker_generation) {
        return false;
    }
    transaction->physical_done = true;
    return true;
}

bool nuwax_notification_ready_to_complete(
    const nuwax_notification_transaction_t *transaction) {
    return transaction && transaction->active &&
           transaction->state == NUWAX_NOTIFICATION_STATE_DRAINING &&
           transaction->logical_end_seen && transaction->binary_end_seen &&
           transaction->physical_done;
}

bool nuwax_notification_mark_end_ack_sent(
    nuwax_notification_transaction_t *transaction,
    uint32_t token,
    uint32_t ws_epoch) {
    if (!token_matches(transaction, token) || ws_epoch == 0U ||
        transaction->ws_epoch != ws_epoch ||
        !nuwax_notification_ready_to_complete(transaction)) {
        return false;
    }
    transaction->end_ack_sent = true;
    return true;
}

bool nuwax_notification_request_cancel(
    nuwax_notification_transaction_t *transaction,
    uint32_t token,
    uint32_t ws_epoch,
    uint32_t now_ms,
    uint32_t timeout_ms) {
    if (!token_matches(transaction, token) || ws_epoch == 0U ||
        transaction->ws_epoch != ws_epoch || timeout_ms == 0U) {
        return false;
    }
    transaction->state = NUWAX_NOTIFICATION_STATE_CANCELLING;
    transaction->deadline_ms = now_ms + timeout_ms;
    if (transaction->speaker_generation == 0U) {
        transaction->physical_done = true;
    }
    return true;
}

bool nuwax_notification_cancel_ready(
    const nuwax_notification_transaction_t *transaction) {
    return transaction && transaction->active &&
           transaction->state == NUWAX_NOTIFICATION_STATE_CANCELLING &&
           transaction->capture_released &&
           transaction->wake_detector_released &&
           transaction->physical_done;
}

bool nuwax_notification_mark_cancel_notified(
    nuwax_notification_transaction_t *transaction,
    uint32_t token,
    uint32_t ws_epoch) {
    if (!token_matches(transaction, token) || ws_epoch == 0U ||
        transaction->ws_epoch != ws_epoch ||
        transaction->state != NUWAX_NOTIFICATION_STATE_CANCELLING) {
        return false;
    }
    transaction->cancel_notified = true;
    return true;
}

bool nuwax_notification_complete(
    nuwax_notification_transaction_t *transaction,
    uint32_t token) {
    if (!token_matches(transaction, token)) {
        return false;
    }
    const bool normal_complete =
        nuwax_notification_ready_to_complete(transaction) &&
        transaction->end_ack_sent;
    const bool cancel_complete =
        nuwax_notification_cancel_ready(transaction) &&
        transaction->cancel_notified;
    if (!normal_complete && !cancel_complete) {
        return false;
    }
    transaction->state = NUWAX_NOTIFICATION_STATE_COMPLETE;
    transaction->active = false;
    return true;
}

bool nuwax_notification_deadline_expired(
    const nuwax_notification_transaction_t *transaction,
    uint32_t now_ms) {
    return transaction && transaction->active &&
           (int32_t)(now_ms - transaction->deadline_ms) >= 0;
}

bool nuwax_notification_blocks_conversation(
    const nuwax_notification_transaction_t *transaction) {
    return transaction && transaction->active;
}

bool nuwax_notification_matches(
    const nuwax_notification_transaction_t *transaction,
    uint32_t ws_epoch,
    const char *notify_id,
    const char *playback_id) {
    if (!transaction || !transaction->active || ws_epoch == 0U ||
        transaction->ws_epoch != ws_epoch || !valid_id(notify_id) ||
        strcmp(transaction->notify_id, notify_id) != 0) {
        return false;
    }
    return !playback_id || playback_id[0] == '\0' ||
           strcmp(transaction->playback_id, playback_id) == 0;
}

const char *nuwax_notification_state_name(nuwax_notification_state_t state) {
    switch (state) {
        case NUWAX_NOTIFICATION_STATE_IDLE:
            return "idle";
        case NUWAX_NOTIFICATION_STATE_PREPARING:
            return "preparing";
        case NUWAX_NOTIFICATION_STATE_READY:
            return "ready";
        case NUWAX_NOTIFICATION_STATE_BEGIN_PENDING:
            return "begin_pending";
        case NUWAX_NOTIFICATION_STATE_STREAMING:
            return "streaming";
        case NUWAX_NOTIFICATION_STATE_DRAINING:
            return "draining";
        case NUWAX_NOTIFICATION_STATE_CANCELLING:
            return "cancelling";
        case NUWAX_NOTIFICATION_STATE_COMPLETE:
            return "complete";
        default:
            return "unknown";
    }
}
