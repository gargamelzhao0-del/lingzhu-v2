#pragma once

#include <stdbool.h>
#include <stdint.h>

#define NUWAX_WS_OWNER_CLOSE_RESTART_FAILURES 3U

typedef enum {
    NUWAX_WS_CLOSE_CAUSE_BUSINESS_RECOVERY = 0,
    NUWAX_WS_CLOSE_CAUSE_BUSINESS_STALLED,
    NUWAX_WS_CLOSE_CAUSE_HEARTBEAT_EXPIRED,
    NUWAX_WS_CLOSE_CAUSE_TRANSPORT_INTEGRITY,
    NUWAX_WS_CLOSE_CAUSE_AUTH_REJECTED,
    NUWAX_WS_CLOSE_CAUSE_OTA_STATE_REFRESH,
    NUWAX_WS_CLOSE_CAUSE_OWNERSHIP_TRANSFER,
    NUWAX_WS_CLOSE_CAUSE_WAKE_GREETING_TIMEOUT,
} nuwax_ws_close_cause_t;

static inline bool nuwax_ws_close_cause_bypasses_heartbeat(
    nuwax_ws_close_cause_t cause) {
    return cause == NUWAX_WS_CLOSE_CAUSE_TRANSPORT_INTEGRITY ||
           cause == NUWAX_WS_CLOSE_CAUSE_AUTH_REJECTED ||
           cause == NUWAX_WS_CLOSE_CAUSE_OTA_STATE_REFRESH ||
           cause == NUWAX_WS_CLOSE_CAUSE_OWNERSHIP_TRANSFER ||
           cause == NUWAX_WS_CLOSE_CAUSE_WAKE_GREETING_TIMEOUT;
}

static inline bool nuwax_ws_close_policy_admits(
    nuwax_ws_close_cause_t cause,
    bool heartbeat_healthy) {
    return !heartbeat_healthy ||
           nuwax_ws_close_cause_bypasses_heartbeat(cause);
}

static inline bool nuwax_ws_close_requested_for_epoch(
    uint32_t connection_epoch,
    uint32_t requested_epoch) {
    return connection_epoch != 0U && requested_epoch == connection_epoch;
}

static inline const char *nuwax_ws_close_cause_name(
    nuwax_ws_close_cause_t cause) {
    switch (cause) {
        case NUWAX_WS_CLOSE_CAUSE_BUSINESS_RECOVERY:
            return "business_recovery";
        case NUWAX_WS_CLOSE_CAUSE_BUSINESS_STALLED:
            return "business_stalled";
        case NUWAX_WS_CLOSE_CAUSE_HEARTBEAT_EXPIRED:
            return "heartbeat_expired";
        case NUWAX_WS_CLOSE_CAUSE_TRANSPORT_INTEGRITY:
            return "transport_integrity";
        case NUWAX_WS_CLOSE_CAUSE_AUTH_REJECTED:
            return "auth_rejected";
        case NUWAX_WS_CLOSE_CAUSE_OTA_STATE_REFRESH:
            return "ota_state_refresh";
        case NUWAX_WS_CLOSE_CAUSE_OWNERSHIP_TRANSFER:
            return "ownership_transfer";
        case NUWAX_WS_CLOSE_CAUSE_WAKE_GREETING_TIMEOUT:
            return "wake_greeting_timeout";
        default:
            return "invalid";
    }
}

typedef struct {
    uint32_t consecutive_failures;
} nuwax_ws_owner_close_recovery_t;

static inline void nuwax_ws_owner_close_recovery_reset(
    nuwax_ws_owner_close_recovery_t *state) {
    if (state) {
        state->consecutive_failures = 0U;
    }
}

/* An active or heartbeat-healthy transport is never a cleanup failure. Only
 * repeated failure to release an already inactive transport can request the
 * product's final controlled-restart fallback. */
static inline bool nuwax_ws_owner_close_recovery_observe(
    nuwax_ws_owner_close_recovery_t *state,
    bool resources_released,
    bool transport_connected,
    bool heartbeat_healthy) {
    if (!state) {
        return false;
    }
    if (resources_released || transport_connected || heartbeat_healthy) {
        nuwax_ws_owner_close_recovery_reset(state);
        return false;
    }
    if (state->consecutive_failures < UINT32_MAX) {
        state->consecutive_failures++;
    }
    return state->consecutive_failures >=
           NUWAX_WS_OWNER_CLOSE_RESTART_FAILURES;
}
