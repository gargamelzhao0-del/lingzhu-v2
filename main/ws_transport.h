#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>

#include "esp_err.h"
#include "ws_close_policy.h"

typedef struct esp_tls esp_tls_t;

typedef void (*nuwax_ws_text_handler_t)(const char *payload, int len, void *user_ctx);
typedef void (*nuwax_ws_binary_handler_t)(const uint8_t *payload, int len, void *user_ctx);
typedef void (*nuwax_ws_activity_handler_t)(void *user_ctx);
typedef void (*nuwax_ws_handshake_handler_t)(void *user_ctx);

typedef enum {
    NUWAX_WS_LIFECYCLE_IDLE = 0,
    NUWAX_WS_LIFECYCLE_CONNECTING,
    NUWAX_WS_LIFECYCLE_OPEN,
    NUWAX_WS_LIFECYCLE_CLOSE_REQUESTED,
    NUWAX_WS_LIFECYCLE_RX_EXITED,
    NUWAX_WS_LIFECYCLE_FAULTED,
} nuwax_ws_lifecycle_state_t;

typedef enum {
    NUWAX_WS_CLOSE_FAILURE_NONE = 0,
    NUWAX_WS_CLOSE_FAILURE_RX_JOIN,
    NUWAX_WS_CLOSE_FAILURE_TRANSPORT_LOCK,
} nuwax_ws_close_failure_stage_t;

typedef enum {
    NUWAX_WS_TX_STAGE_IDLE = 0,
    NUWAX_WS_TX_STAGE_WAIT_LIFECYCLE,
    NUWAX_WS_TX_STAGE_WAIT_IO,
    NUWAX_WS_TX_STAGE_WRITING,
    NUWAX_WS_TX_STAGE_RETRYING,
} nuwax_ws_tx_stage_t;

typedef struct {
    nuwax_ws_tx_stage_t stage;
    uint32_t epoch;
    uint32_t stage_age_ms;
} nuwax_ws_tx_diagnostics_t;

typedef struct {
    const char *url;
    const char *token;
    nuwax_ws_text_handler_t on_text;
    nuwax_ws_binary_handler_t on_binary;
    nuwax_ws_activity_handler_t on_rx_activity;
    /* Called synchronously after a validated 101 response and before the RX
     * task starts. It must not perform WebSocket I/O or block. */
    nuwax_ws_handshake_handler_t on_handshake_verified;
    void *user_ctx;
} nuwax_ws_config_t;

typedef struct {
    int sock;
    esp_tls_t *tls;
    bool use_tls;
    atomic_bool connected;
    int last_http_status;
    nuwax_ws_text_handler_t on_text;
    nuwax_ws_binary_handler_t on_binary;
    nuwax_ws_activity_handler_t on_rx_activity;
    void *user_ctx;
    void *rx_task;
    void *io_lock;
    uint8_t *rx_buffer;
    size_t rx_buffer_capacity;
    atomic_bool rx_task_running;
    atomic_int lifecycle_state;
    atomic_int close_failure_stage;
    atomic_int last_close_result;
    atomic_uint_fast32_t connection_epoch;
    /* A recent protocol or application Pong protects an otherwise healthy
     * connection from business/state-machine close requests. */
    atomic_uint_fast32_t last_heartbeat_epoch;
    atomic_uint_fast32_t last_heartbeat_ms;
    /* Lock-free epoch mailbox for cross-task close requests. It prevents a
     * short lifecycle-lock timeout from silently losing a required close. */
    atomic_uint_fast32_t close_requested_epoch;
    atomic_int tx_stage;
    atomic_uint_fast32_t tx_stage_epoch;
    atomic_uint_fast32_t tx_stage_since_ms;
} nuwax_ws_client_t;

void nuwax_ws_init(nuwax_ws_client_t *client);
/* RX payload pointers are borrowed connection storage and remain valid only
 * for the duration of their callback. A callback must copy data it retains. */
esp_err_t nuwax_ws_connect(nuwax_ws_client_t *client, const nuwax_ws_config_t *config);
esp_err_t nuwax_ws_send_text(nuwax_ws_client_t *client, const char *payload);
/* Sends one text frame without extending the caller's absolute monotonic
 * deadline.  deadline_us uses esp_timer_get_time() units (microseconds). */
esp_err_t nuwax_ws_send_text_until(nuwax_ws_client_t *client,
                                   const char *payload,
                                   int64_t deadline_us);
esp_err_t nuwax_ws_send_text_until_if_epoch(nuwax_ws_client_t *client,
                                            const char *payload,
                                            int64_t deadline_us,
                                            uint32_t expected_epoch);
/* Convenience wrapper for a bounded text send. */
esp_err_t nuwax_ws_send_text_with_timeout(nuwax_ws_client_t *client,
                                          const char *payload,
                                          uint32_t timeout_ms);
esp_err_t nuwax_ws_send_text_with_timeout_if_epoch(
    nuwax_ws_client_t *client,
    const char *payload,
    uint32_t timeout_ms,
    uint32_t expected_epoch);
esp_err_t nuwax_ws_send_binary(nuwax_ws_client_t *client, const uint8_t *payload, size_t len);
esp_err_t nuwax_ws_send_binary_if_epoch(nuwax_ws_client_t *client,
                                        const uint8_t *payload,
                                        size_t len,
                                        uint32_t expected_epoch);
esp_err_t nuwax_ws_send_binary_with_timeout(nuwax_ws_client_t *client,
                                            const uint8_t *payload,
                                            size_t len,
                                            uint32_t timeout_ms);
esp_err_t nuwax_ws_send_binary_with_timeout_if_epoch(
    nuwax_ws_client_t *client,
    const uint8_t *payload,
    size_t len,
    uint32_t timeout_ms,
    uint32_t expected_epoch);
esp_err_t nuwax_ws_send_ping(nuwax_ws_client_t *client);
/* Records a validated Pong for the specified connection generation. Control
 * Pong frames call this internally; the application protocol must call it for
 * its JSON Pong. */
void nuwax_ws_note_heartbeat_if_epoch(nuwax_ws_client_t *client,
                                      uint32_t expected_epoch);
bool nuwax_ws_heartbeat_healthy_if_epoch(const nuwax_ws_client_t *client,
                                         uint32_t expected_epoch);
/* Remaining authority of the most recent validated Pong. Zero means no
 * heartbeat protection for this epoch. */
uint32_t nuwax_ws_heartbeat_remaining_ms_if_epoch(
    const nuwax_ws_client_t *client,
    uint32_t expected_epoch);
/* Non-blocking advisory disconnect request. A recent Pong suppresses this
 * class of local-processing recovery. The connection owner must call
 * nuwax_ws_owner_close() only after the transport is actually down. */
void nuwax_ws_request_close(nuwax_ws_client_t *client);
/* Epoch-bound advisory close for explicitly best-effort local recovery. A
 * healthy heartbeat may veto it. */
bool nuwax_ws_advisory_close_if_epoch(nuwax_ws_client_t *client,
                                      uint32_t expected_epoch);
/* Compatibility entry point for business/state-machine recovery. A healthy
 * heartbeat protects a stream-integrity-clean connection from this request. */
bool nuwax_ws_request_close_if_epoch(nuwax_ws_client_t *client,
                                     uint32_t expected_epoch);
/* Explicit close admission for transport integrity, authentication, liveness,
 * and ownership events. Routine business recovery must remain heartbeat
 * protected; BUSINESS_STALLED is reserved for a separately bounded business
 * response watchdog that has expired repeatedly. */
bool nuwax_ws_close_if_epoch(nuwax_ws_client_t *client,
                             uint32_t expected_epoch,
                             nuwax_ws_close_cause_t cause);
/* Deliberately bypasses heartbeat protection. Product code reserves this for
 * entering BLE provisioning, where the network stack changes ownership. */
bool nuwax_ws_force_close_if_epoch(nuwax_ws_client_t *client,
                                   uint32_t expected_epoch);
/* A nonzero value identifies one connect attempt and is never reused by the
 * next reconnect on the same client object. */
uint32_t nuwax_ws_connection_epoch(const nuwax_ws_client_t *client);

/* connect() and owner_close() must be serialized by one persistent owner;
 * request_close() is the cross-task operation. The client object, including
 * its lazily-created I/O mutex, must remain alive across reconnect attempts.
 *
 * Owner-only, retryable close. ESP_ERR_TIMEOUT identifies an RX join timeout;
 * ESP_ERR_NOT_FINISHED identifies a transport-lock timeout. Other failures can
 * be disambiguated with nuwax_ws_owner_close_failure_stage(). The client stays
 * FAULTED and must remain alive until this function succeeds. */
esp_err_t nuwax_ws_owner_close(nuwax_ws_client_t *client);

/* Compatibility wrapper. New owner code must use nuwax_ws_owner_close() and
 * check its result before releasing or reusing the client object. */
void nuwax_ws_close(nuwax_ws_client_t *client);

/* These owner-side queries support a persistent client object across reconnect
 * attempts. A client is reusable only after owner_close() returns ESP_OK and
 * this function reports true. */
bool nuwax_ws_owner_resources_released(const nuwax_ws_client_t *client);
nuwax_ws_tx_diagnostics_t nuwax_ws_tx_diagnostics(
    const nuwax_ws_client_t *client);
nuwax_ws_close_failure_stage_t nuwax_ws_owner_close_failure_stage(
    const nuwax_ws_client_t *client);
esp_err_t nuwax_ws_owner_last_close_result(const nuwax_ws_client_t *client);

bool nuwax_ws_is_connected(const nuwax_ws_client_t *client);
int nuwax_ws_last_http_status(const nuwax_ws_client_t *client);
nuwax_ws_lifecycle_state_t nuwax_ws_lifecycle_state(
    const nuwax_ws_client_t *client);
