#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Plain TCP reports exactly how many bytes send() committed. TLS may retain a
 * pending record after WANT_WRITE and requires the same input buffer on the
 * next call. This predicate identifies the point after which a caller must
 * either complete the current frame with the same buffer or reconnect. */
static inline bool nuwax_ws_send_has_unsafe_transport_progress(
    size_t frame_bytes_written,
    bool transport_write_attempted,
    bool secure_transport) {
    return frame_bytes_written > 0U ||
           (secure_transport && transport_write_attempted);
}

static inline bool nuwax_ws_send_should_drain_inflight(
    bool inflight_drain_enabled,
    size_t frame_bytes_written,
    bool transport_write_attempted,
    bool secure_transport) {
    return inflight_drain_enabled &&
           nuwax_ws_send_has_unsafe_transport_progress(
               frame_bytes_written,
               transport_write_attempted,
               secure_transport);
}

/* An unsent frame may be abandoned without changing the WebSocket byte
 * stream. Once transport progress is unsafe, abandoning the frame requires a
 * reconnect if the bounded in-flight drain cannot complete it. */
static inline bool nuwax_ws_send_failure_requires_disconnect(
    bool timed_out,
    size_t frame_bytes_written,
    bool transport_write_attempted,
    bool secure_transport,
    bool preserve_unsent_timeout) {
    return !timed_out ||
           nuwax_ws_send_has_unsafe_transport_progress(
               frame_bytes_written,
               transport_write_attempted,
               secure_transport) ||
           !preserve_unsent_timeout;
}

static inline bool nuwax_ws_reliable_send_failure_requires_disconnect(
    bool timed_out,
    size_t frame_bytes_written,
    bool transport_write_attempted,
    bool secure_transport) {
    return nuwax_ws_send_failure_requires_disconnect(
        timed_out,
        frame_bytes_written,
        transport_write_attempted,
        secure_transport,
        true);
}

/* A higher-level transaction may retry the whole frame only when the
 * transport has already proven that the timed-out attempt made no unsafe
 * progress. The send layer communicates that proof by retaining connected on
 * the same epoch. Requiring a recent heartbeat avoids extending a transaction
 * on a connection whose liveness is already uncertain. */
static inline bool nuwax_ws_safe_unsent_retry_allowed(
    bool timed_out,
    bool connected,
    bool epoch_matches,
    bool heartbeat_healthy) {
    return timed_out && connected && epoch_matches && heartbeat_healthy;
}

/* A bounded application ping may lose its send slot to active audio or
 * control traffic. Keep the current owner alive only when the transport has
 * independently proved that the same connection is still healthy. */
static inline bool nuwax_ws_keepalive_timeout_is_transient(
    bool timed_out,
    bool connected,
    bool epoch_matches,
    bool heartbeat_healthy) {
    return timed_out && connected && epoch_matches && heartbeat_healthy;
}

static inline uint32_t nuwax_ws_inflight_drain_budget_ms(
    uint32_t minimum_drain_ms,
    uint32_t heartbeat_remaining_ms) {
    return heartbeat_remaining_ms > minimum_drain_ms
               ? heartbeat_remaining_ms
               : minimum_drain_ms;
}

/* A readiness probe never owns frame bytes. No readiness, an interrupted
 * probe, or a positive result for another descriptor can therefore defer the
 * realtime frame without changing the WebSocket byte stream. A non-retryable
 * probe failure still reports a transport fault to the caller. */
static inline bool nuwax_ws_write_probe_should_defer(
    int select_result,
    bool socket_writable,
    bool retryable_error) {
    return select_result == 0 ||
           (select_result > 0 && !socket_writable) ||
           (select_result < 0 && retryable_error);
}
