#include <assert.h>
#include <string.h>

#include "ws_close_policy.h"

int main(void) {
    assert(!nuwax_ws_close_policy_admits(
        NUWAX_WS_CLOSE_CAUSE_BUSINESS_RECOVERY, true));
    assert(!nuwax_ws_close_policy_admits(
        NUWAX_WS_CLOSE_CAUSE_HEARTBEAT_EXPIRED, true));
    assert(!nuwax_ws_close_policy_admits(
        NUWAX_WS_CLOSE_CAUSE_BUSINESS_STALLED, true));
    assert(nuwax_ws_close_policy_admits(
        NUWAX_WS_CLOSE_CAUSE_BUSINESS_RECOVERY, false));
    assert(nuwax_ws_close_policy_admits(
        NUWAX_WS_CLOSE_CAUSE_HEARTBEAT_EXPIRED, false));

    assert(nuwax_ws_close_policy_admits(
        NUWAX_WS_CLOSE_CAUSE_TRANSPORT_INTEGRITY, true));
    assert(nuwax_ws_close_policy_admits(
        NUWAX_WS_CLOSE_CAUSE_AUTH_REJECTED, true));
    assert(nuwax_ws_close_policy_admits(
        NUWAX_WS_CLOSE_CAUSE_OTA_STATE_REFRESH, true));
    assert(nuwax_ws_close_policy_admits(
        NUWAX_WS_CLOSE_CAUSE_OWNERSHIP_TRANSFER, true));

    assert(strcmp(nuwax_ws_close_cause_name(
                      NUWAX_WS_CLOSE_CAUSE_BUSINESS_RECOVERY),
                  "business_recovery") == 0);
    assert(strcmp(nuwax_ws_close_cause_name(
                      NUWAX_WS_CLOSE_CAUSE_BUSINESS_STALLED),
                  "business_stalled") == 0);
    assert(strcmp(nuwax_ws_close_cause_name(
                      NUWAX_WS_CLOSE_CAUSE_TRANSPORT_INTEGRITY),
                  "transport_integrity") == 0);
    assert(strcmp(nuwax_ws_close_cause_name(
                      NUWAX_WS_CLOSE_CAUSE_OTA_STATE_REFRESH),
                  "ota_state_refresh") == 0);

    assert(nuwax_ws_close_requested_for_epoch(7U, 7U));
    assert(!nuwax_ws_close_requested_for_epoch(7U, 6U));
    assert(!nuwax_ws_close_requested_for_epoch(0U, 0U));

    nuwax_ws_owner_close_recovery_t recovery = {0};
    assert(!nuwax_ws_owner_close_recovery_observe(
        &recovery, false, false, false));
    assert(recovery.consecutive_failures == 1U);
    assert(!nuwax_ws_owner_close_recovery_observe(
        &recovery, false, false, false));
    assert(recovery.consecutive_failures == 2U);
    assert(nuwax_ws_owner_close_recovery_observe(
        &recovery, false, false, false));
    assert(recovery.consecutive_failures ==
           NUWAX_WS_OWNER_CLOSE_RESTART_FAILURES);

    assert(!nuwax_ws_owner_close_recovery_observe(
        &recovery, false, true, false));
    assert(recovery.consecutive_failures == 0U);
    assert(!nuwax_ws_owner_close_recovery_observe(
        &recovery, false, false, true));
    assert(recovery.consecutive_failures == 0U);

    recovery.consecutive_failures = 2U;
    assert(!nuwax_ws_owner_close_recovery_observe(
        &recovery, true, false, false));
    assert(recovery.consecutive_failures == 0U);
    return 0;
}
