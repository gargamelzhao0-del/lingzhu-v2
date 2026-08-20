#include <assert.h>
#include <stdbool.h>

#include "ws_send_failure_policy.h"

int main(void) {
    assert(!nuwax_ws_send_has_unsafe_transport_progress(
        0U, false, false));
    assert(!nuwax_ws_send_has_unsafe_transport_progress(
        0U, true, false));
    assert(nuwax_ws_send_has_unsafe_transport_progress(
        0U, true, true));
    assert(nuwax_ws_send_has_unsafe_transport_progress(
        1U, false, false));

    assert(!nuwax_ws_send_should_drain_inflight(
        false, 1U, true, true));
    assert(!nuwax_ws_send_should_drain_inflight(
        true, 0U, true, false));
    assert(nuwax_ws_send_should_drain_inflight(
        true, 0U, true, true));
    assert(nuwax_ws_send_should_drain_inflight(
        true, 1U, true, false));

    /* The deadline expired before entering either transport. */
    assert(!nuwax_ws_send_failure_requires_disconnect(
        true, 0U, false, true, true));
    /* Plain send() made no progress (EAGAIN/EWOULDBLOCK). */
    assert(!nuwax_ws_send_failure_requires_disconnect(
        true, 0U, true, false, true));
    /* TLS WANT_WRITE may retain the caller's input even at zero progress. */
    assert(nuwax_ws_send_failure_requires_disconnect(
        true, 0U, true, true, true));
    assert(nuwax_ws_send_failure_requires_disconnect(
        true, 1U, true, false, true));
    assert(nuwax_ws_send_failure_requires_disconnect(
        false, 0U, false, false, true));

    assert(nuwax_ws_inflight_drain_budget_ms(5000U, 0U) == 5000U);
    assert(nuwax_ws_inflight_drain_budget_ms(5000U, 3000U) == 5000U);
    assert(nuwax_ws_inflight_drain_budget_ms(5000U, 42000U) == 42000U);
    assert(!nuwax_ws_write_probe_should_defer(1, true, false));
    assert(nuwax_ws_write_probe_should_defer(0, false, false));
    assert(nuwax_ws_write_probe_should_defer(1, false, false));
    assert(nuwax_ws_write_probe_should_defer(-1, false, true));
    assert(!nuwax_ws_write_probe_should_defer(-1, false, false));
    assert(nuwax_ws_send_failure_requires_disconnect(
        true, 0U, false, false, false));

    /* Reliable control sends retain the connection only when no WebSocket or
     * TLS stream progress was made. The caller can retry the whole frame. */
    assert(!nuwax_ws_reliable_send_failure_requires_disconnect(
        true, 0U, false, true));
    assert(!nuwax_ws_reliable_send_failure_requires_disconnect(
        true, 0U, true, false));
    assert(nuwax_ws_reliable_send_failure_requires_disconnect(
        true, 0U, true, true));
    assert(nuwax_ws_reliable_send_failure_requires_disconnect(
        true, 1U, true, false));
    assert(nuwax_ws_reliable_send_failure_requires_disconnect(
        false, 0U, false, false));

    assert(nuwax_ws_safe_unsent_retry_allowed(true, true, true, true));
    assert(!nuwax_ws_safe_unsent_retry_allowed(false, true, true, true));
    assert(!nuwax_ws_safe_unsent_retry_allowed(true, false, true, true));
    assert(!nuwax_ws_safe_unsent_retry_allowed(true, true, false, true));
    assert(!nuwax_ws_safe_unsent_retry_allowed(true, true, true, false));

    assert(nuwax_ws_keepalive_timeout_is_transient(
        true, true, true, true));
    assert(!nuwax_ws_keepalive_timeout_is_transient(
        false, true, true, true));
    assert(!nuwax_ws_keepalive_timeout_is_transient(
        true, false, true, true));
    assert(!nuwax_ws_keepalive_timeout_is_transient(
        true, true, false, true));
    assert(!nuwax_ws_keepalive_timeout_is_transient(
        true, true, true, false));
    return 0;
}
