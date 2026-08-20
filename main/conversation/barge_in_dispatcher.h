#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "ws_transport.h"

typedef struct {
    const char *event_id;
    const char *parent_turn_id;
    const char *playback_id;
    const char *audio_session_id;
    const char *listen_mode;
    uint32_t onset_sequence;
    uint32_t confirmed_sequence;
    uint32_t confidence_q15;
} nuwax_barge_confirmed_event_t;

typedef enum {
    NUWAX_BARGE_DISPATCH_UNKNOWN = 0,
    /* The listen send has definitely not started and may still be cancelled. */
    NUWAX_BARGE_DISPATCH_PENDING,
    /*
     * The listen-start send function has been entered.  Until it reaches a
     * terminal state, the peer may already have committed the listen session.
     */
    NUWAX_BARGE_DISPATCH_LISTEN_ATTEMPTED,
    /* Compatibility alias for callers being migrated to LISTEN_ATTEMPTED. */
    NUWAX_BARGE_DISPATCH_LISTEN_SENDING =
        NUWAX_BARGE_DISPATCH_LISTEN_ATTEMPTED,
    NUWAX_BARGE_DISPATCH_DELIVERED,
    /* The listen-start send function was never entered. */
    NUWAX_BARGE_DISPATCH_FAILED,
    /*
     * The listen-start send was attempted and returned an error.  The peer
     * may nevertheless have committed it, so the caller must send a matching
     * stop on the same live binding or close that binding.
     */
    NUWAX_BARGE_DISPATCH_COMMIT_UNCERTAIN,
} nuwax_barge_dispatch_status_t;

static inline bool nuwax_barge_dispatch_listen_may_be_committed(
    nuwax_barge_dispatch_status_t status) {
    return status == NUWAX_BARGE_DISPATCH_LISTEN_ATTEMPTED ||
           status == NUWAX_BARGE_DISPATCH_DELIVERED ||
           status == NUWAX_BARGE_DISPATCH_COMMIT_UNCERTAIN;
}

esp_err_t nuwax_barge_dispatcher_init(void);

/* Binding prevents events queued for a closed connection from leaking into a
 * later WebSocket session that happens to reuse the same stack address. */
uint32_t nuwax_barge_dispatcher_bind(nuwax_ws_client_t *ws);
uint32_t nuwax_barge_dispatcher_current_binding(nuwax_ws_client_t *ws);
void nuwax_barge_dispatcher_unbind(nuwax_ws_client_t *ws,
                                   uint32_t binding_generation);

/* Enqueues one ordered transaction: candidate, confirmed, then listen start. */
esp_err_t nuwax_barge_dispatcher_enqueue_confirmed(
    uint32_t binding_generation,
    const nuwax_barge_confirmed_event_t *event);

bool nuwax_barge_dispatcher_get_status(
    uint32_t binding_generation,
    const char *event_id,
    nuwax_barge_dispatch_status_t *status,
    esp_err_t *result);

bool nuwax_barge_dispatcher_cancel(uint32_t binding_generation,
                                   const char *event_id);
