#pragma once

#include <stdbool.h>
#include <stdint.h>

#define NUWAX_NOTIFICATION_REQUEST_ID_MAX_LEN 40U
#define NUWAX_NOTIFICATION_ID_MAX_LEN 48U
#define NUWAX_NOTIFICATION_PLAYBACK_ID_MAX_LEN 48U

typedef enum {
    NUWAX_NOTIFICATION_STATE_IDLE = 0,
    NUWAX_NOTIFICATION_STATE_PREPARING,
    NUWAX_NOTIFICATION_STATE_READY,
    NUWAX_NOTIFICATION_STATE_BEGIN_PENDING,
    NUWAX_NOTIFICATION_STATE_STREAMING,
    NUWAX_NOTIFICATION_STATE_DRAINING,
    NUWAX_NOTIFICATION_STATE_CANCELLING,
    NUWAX_NOTIFICATION_STATE_COMPLETE,
} nuwax_notification_state_t;

typedef enum {
    NUWAX_NOTIFICATION_REQUEST_INVALID = 0,
    NUWAX_NOTIFICATION_REQUEST_STARTED,
    NUWAX_NOTIFICATION_REQUEST_DUPLICATE,
    NUWAX_NOTIFICATION_REQUEST_BUSY,
    NUWAX_NOTIFICATION_REQUEST_CONFLICT,
    NUWAX_NOTIFICATION_REQUEST_NOT_PREPARED,
    NUWAX_NOTIFICATION_REQUEST_NOT_READY,
} nuwax_notification_request_result_t;

typedef struct {
    nuwax_notification_state_t state;
    bool active;
    bool origin_sleeping;
    uint32_t token;
    uint32_t ws_epoch;
    uint32_t requested_ms;
    uint32_t deadline_ms;
    uint32_t previous_speaker_generation;
    uint32_t speaker_generation;
    char prepare_request_id[NUWAX_NOTIFICATION_REQUEST_ID_MAX_LEN];
    char begin_request_id[NUWAX_NOTIFICATION_REQUEST_ID_MAX_LEN];
    char end_request_id[NUWAX_NOTIFICATION_REQUEST_ID_MAX_LEN];
    char notify_id[NUWAX_NOTIFICATION_ID_MAX_LEN];
    char playback_id[NUWAX_NOTIFICATION_PLAYBACK_ID_MAX_LEN];
    bool capture_released;
    bool wake_detector_released;
    bool previous_speaker_stopped;
    bool prepare_ack_sent;
    bool begin_ack_sent;
    bool binary_end_seen;
    bool logical_end_seen;
    bool physical_done;
    bool end_ack_sent;
    bool cancel_notified;
} nuwax_notification_transaction_t;

void nuwax_notification_init(nuwax_notification_transaction_t *transaction);
void nuwax_notification_reset(nuwax_notification_transaction_t *transaction);

nuwax_notification_request_result_t nuwax_notification_request_prepare(
    nuwax_notification_transaction_t *transaction,
    uint32_t now_ms,
    uint32_t timeout_ms,
    uint32_t ws_epoch,
    uint32_t previous_speaker_generation,
    const char *request_id,
    const char *notify_id,
    bool origin_sleeping);

nuwax_notification_request_result_t nuwax_notification_request_begin(
    nuwax_notification_transaction_t *transaction,
    uint32_t now_ms,
    uint32_t timeout_ms,
    uint32_t ws_epoch,
    uint32_t previous_speaker_generation,
    const char *request_id,
    const char *notify_id,
    const char *playback_id,
    bool local_sleeping);

bool nuwax_notification_mark_capture_released(
    nuwax_notification_transaction_t *transaction,
    uint32_t token);
bool nuwax_notification_mark_wake_detector_released(
    nuwax_notification_transaction_t *transaction,
    uint32_t token);
bool nuwax_notification_mark_previous_speaker_stopped(
    nuwax_notification_transaction_t *transaction,
    uint32_t token,
    uint32_t ws_epoch,
    uint32_t speaker_generation);
bool nuwax_notification_prepare_ready(
    const nuwax_notification_transaction_t *transaction);
bool nuwax_notification_mark_prepare_ready(
    nuwax_notification_transaction_t *transaction,
    uint32_t token);
bool nuwax_notification_mark_prepare_ack_sent(
    nuwax_notification_transaction_t *transaction,
    uint32_t token,
    uint32_t ws_epoch,
    uint32_t now_ms,
    uint32_t timeout_ms);

bool nuwax_notification_begin_ready(
    const nuwax_notification_transaction_t *transaction);
bool nuwax_notification_mark_streaming(
    nuwax_notification_transaction_t *transaction,
    uint32_t token,
    uint32_t ws_epoch);
bool nuwax_notification_mark_begin_ack_sent(
    nuwax_notification_transaction_t *transaction,
    uint32_t token,
    uint32_t ws_epoch);
bool nuwax_notification_refresh_stream_deadline(
    nuwax_notification_transaction_t *transaction,
    uint32_t token,
    uint32_t ws_epoch,
    uint32_t now_ms,
    uint32_t timeout_ms);
bool nuwax_notification_bind_speaker_generation(
    nuwax_notification_transaction_t *transaction,
    uint32_t token,
    uint32_t ws_epoch,
    uint32_t speaker_generation);

nuwax_notification_request_result_t nuwax_notification_request_end(
    nuwax_notification_transaction_t *transaction,
    uint32_t now_ms,
    uint32_t timeout_ms,
    uint32_t ws_epoch,
    const char *request_id,
    const char *notify_id,
    const char *playback_id);
bool nuwax_notification_mark_binary_end(
    nuwax_notification_transaction_t *transaction,
    uint32_t token,
    uint32_t ws_epoch,
    uint32_t speaker_generation);
bool nuwax_notification_mark_physical_done(
    nuwax_notification_transaction_t *transaction,
    uint32_t token,
    uint32_t ws_epoch,
    uint32_t speaker_generation);
bool nuwax_notification_ready_to_complete(
    const nuwax_notification_transaction_t *transaction);
bool nuwax_notification_mark_end_ack_sent(
    nuwax_notification_transaction_t *transaction,
    uint32_t token,
    uint32_t ws_epoch);

bool nuwax_notification_request_cancel(
    nuwax_notification_transaction_t *transaction,
    uint32_t token,
    uint32_t ws_epoch,
    uint32_t now_ms,
    uint32_t timeout_ms);
bool nuwax_notification_cancel_ready(
    const nuwax_notification_transaction_t *transaction);
bool nuwax_notification_mark_cancel_notified(
    nuwax_notification_transaction_t *transaction,
    uint32_t token,
    uint32_t ws_epoch);
bool nuwax_notification_complete(
    nuwax_notification_transaction_t *transaction,
    uint32_t token);

bool nuwax_notification_deadline_expired(
    const nuwax_notification_transaction_t *transaction,
    uint32_t now_ms);
bool nuwax_notification_blocks_conversation(
    const nuwax_notification_transaction_t *transaction);
bool nuwax_notification_matches(
    const nuwax_notification_transaction_t *transaction,
    uint32_t ws_epoch,
    const char *notify_id,
    const char *playback_id);
const char *nuwax_notification_state_name(nuwax_notification_state_t state);
