#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "conversation/touch_cancel_recovery.h"

int main(void) {
    nuwax_touch_cancel_recovery_t recovery = {0};
    assert(!nuwax_touch_cancel_recovery_arm(
        &recovery, 0U, 2U, 8U, 100U, "cancel_1", "turn_1", "pb_1", false, false));
    assert(nuwax_touch_cancel_recovery_arm(
        &recovery, 1U, 3U, 8U, 100U, "cancel_1", "turn_1", "pb_1", false, false));
    assert(!nuwax_touch_cancel_recovery_stop_matches(&recovery, 1U, 9U));
    assert(nuwax_touch_cancel_recovery_stop_matches(&recovery, 1U, 8U));
    assert(!recovery.physical_stop_done);
    assert(nuwax_touch_cancel_recovery_mark_physical_stop(&recovery, 1U, 8U));
    assert(recovery.physical_stop_done);
    assert(nuwax_touch_cancel_recovery_mark_detector_released(&recovery));
    assert(nuwax_touch_cancel_recovery_owner_matches(&recovery, 1U, 3U));
    assert(!nuwax_touch_cancel_recovery_owner_matches(&recovery, 2U, 3U));
    assert(!nuwax_touch_cancel_recovery_ack_matches(
        &recovery, 1U, "cancel_2", "turn_1", "pb_1"));
    assert(nuwax_touch_cancel_recovery_mark_ack(
        &recovery, 1U, "", "turn_1", "", true));
    assert(recovery.cancel_ack_received && recovery.cancel_ack_ok);
    assert(!nuwax_touch_cancel_recovery_deadline_expired(&recovery, 99U));
    assert(nuwax_touch_cancel_recovery_deadline_expired(&recovery, 100U));
    assert(!nuwax_touch_cancel_recovery_arm(
        &recovery, 1U, 3U, 9U, 200U, "cancel_2", "turn_2", "pb_2", false, false));
    nuwax_touch_cancel_recovery_clear(&recovery);
    assert(!recovery.active);

    /* Unsigned millisecond wrap must still honor a pending deadline. */
    assert(nuwax_touch_cancel_recovery_arm(
        &recovery, 7U, 4U, 11U, 5U, NULL, "turn_wrap", "pb_wrap", false, true));
    assert(!nuwax_touch_cancel_recovery_deadline_expired(&recovery, UINT32_MAX));
    assert(nuwax_touch_cancel_recovery_deadline_expired(&recovery, 5U));
    return 0;
}
