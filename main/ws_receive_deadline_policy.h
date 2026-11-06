#pragma once

#include <stdint.h>

typedef enum {
    NUWAX_WS_RECEIVE_TIMEOUT_RETRY = 0,
    NUWAX_WS_RECEIVE_TIMEOUT_SEND_HEARTBEAT,
    NUWAX_WS_RECEIVE_TIMEOUT_DISCONNECT_IDLE,
} nuwax_ws_receive_timeout_action_t;

/* RX and TX share the transport I/O lock, so a timed wait may expire slightly
 * before the requested wall-clock deadline. Only the actual monotonic
 * deadlines may decide whether a timeout sends a heartbeat or closes the
 * connection. The RX idle deadline wins when both deadlines have elapsed. */
static inline nuwax_ws_receive_timeout_action_t
nuwax_ws_receive_timeout_action(int64_t now_us,
                                int64_t heartbeat_deadline_us,
                                int64_t rx_idle_deadline_us) {
    if (now_us >= rx_idle_deadline_us) {
        return NUWAX_WS_RECEIVE_TIMEOUT_DISCONNECT_IDLE;
    }
    if (now_us >= heartbeat_deadline_us) {
        return NUWAX_WS_RECEIVE_TIMEOUT_SEND_HEARTBEAT;
    }
    return NUWAX_WS_RECEIVE_TIMEOUT_RETRY;
}
