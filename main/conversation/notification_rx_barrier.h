#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    NUWAX_NOTIFICATION_RX_IDLE = 0,
    NUWAX_NOTIFICATION_RX_PENDING,
    NUWAX_NOTIFICATION_RX_ADMITTED,
    NUWAX_NOTIFICATION_RX_REJECTED,
    NUWAX_NOTIFICATION_RX_NORMAL_PENDING,
} nuwax_notification_rx_state_t;

typedef enum {
    NUWAX_NOTIFICATION_RX_OWNER_NONE = 0,
    NUWAX_NOTIFICATION_RX_OWNER_NOTIFICATION,
    NUWAX_NOTIFICATION_RX_OWNER_NORMAL,
} nuwax_notification_rx_owner_t;

#define NUWAX_NOTIFICATION_RX_STREAM_ID_MAX_LEN 48U
#define NUWAX_NOTIFICATION_RX_PLAYBACK_ID_MAX_LEN 48U

typedef struct {
    nuwax_notification_rx_state_t state;
    nuwax_notification_rx_owner_t owner;
    uint32_t ws_epoch;
    uint32_t deadline_ms;
    char stream_id[NUWAX_NOTIFICATION_RX_STREAM_ID_MAX_LEN];
    char playback_id[NUWAX_NOTIFICATION_RX_PLAYBACK_ID_MAX_LEN];
} nuwax_notification_rx_barrier_t;

void nuwax_notification_rx_barrier_init(
    nuwax_notification_rx_barrier_t *barrier);
bool nuwax_notification_rx_barrier_arm(
    nuwax_notification_rx_barrier_t *barrier,
    uint32_t ws_epoch,
    uint32_t now_ms,
    uint32_t timeout_ms,
    const char *notify_id,
    const char *playback_id);
bool nuwax_notification_rx_barrier_admit(
    nuwax_notification_rx_barrier_t *barrier,
    uint32_t ws_epoch,
    const char *notify_id,
    const char *playback_id);
bool nuwax_notification_rx_barrier_arm_normal(
    nuwax_notification_rx_barrier_t *barrier,
    uint32_t ws_epoch,
    uint32_t now_ms,
    uint32_t timeout_ms,
    const char *turn_id,
    const char *playback_id);
bool nuwax_notification_rx_barrier_admit_normal(
    nuwax_notification_rx_barrier_t *barrier,
    uint32_t ws_epoch,
    const char *turn_id,
    const char *playback_id);
bool nuwax_notification_rx_barrier_reject_normal(
    nuwax_notification_rx_barrier_t *barrier,
    uint32_t ws_epoch,
    uint32_t now_ms,
    uint32_t drain_timeout_ms,
    const char *turn_id,
    const char *playback_id);
bool nuwax_notification_rx_barrier_reject(
    nuwax_notification_rx_barrier_t *barrier,
    uint32_t ws_epoch,
    uint32_t now_ms,
    uint32_t drain_timeout_ms);
void nuwax_notification_rx_barrier_reset(
    nuwax_notification_rx_barrier_t *barrier);
bool nuwax_notification_rx_barrier_reset_if_epoch(
    nuwax_notification_rx_barrier_t *barrier,
    uint32_t ws_epoch);
nuwax_notification_rx_state_t nuwax_notification_rx_barrier_state_for_epoch(
    const nuwax_notification_rx_barrier_t *barrier,
    uint32_t ws_epoch);
bool nuwax_notification_rx_barrier_deadline_expired(
    const nuwax_notification_rx_barrier_t *barrier,
    uint32_t now_ms);
/* Converts an expired rejected-stream drain deadline into a one-shot request
 * to rebuild the bound transport. The barrier deliberately remains REJECTED;
 * only binary END or a WebSocket epoch boundary may release quarantine. */
bool nuwax_notification_rx_barrier_take_rejected_timeout(
    nuwax_notification_rx_barrier_t *barrier,
    uint32_t ws_epoch,
    uint32_t now_ms);
