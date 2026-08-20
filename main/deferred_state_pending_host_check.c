#include <stdio.h>

#include "conversation/deferred_state_pending.h"

int main(void) {
    nuwax_deferred_state_pending_t pending = {0};
    nuwax_deferred_state_submit(&pending,
                                NUWAX_APP_STATE_IDLE,
                                7U,
                                11U,
                                3U);
    nuwax_deferred_state_submit(&pending,
                                NUWAX_APP_STATE_LISTENING,
                                7U,
                                11U,
                                3U);
    if (!nuwax_deferred_state_owner_matches(&pending,
                                             NUWAX_APP_STATE_SPEAKING,
                                             7U,
                                             11U,
                                             3U,
                                             3U) ||
        pending.state != NUWAX_APP_STATE_LISTENING) {
        puts("[FAIL] latest deferred state did not retain its bound owner");
        return 1;
    }
    if (nuwax_deferred_state_owner_matches(&pending,
                                            NUWAX_APP_STATE_SPEAKING,
                                            8U,
                                            11U,
                                            3U,
                                            3U) ||
        nuwax_deferred_state_owner_matches(&pending,
                                            NUWAX_APP_STATE_SPEAKING,
                                            7U,
                                            12U,
                                            3U,
                                            3U) ||
        nuwax_deferred_state_owner_matches(&pending,
                                            NUWAX_APP_STATE_SPEAKING,
                                            7U,
                                            11U,
                                            4U,
                                            3U) ||
        nuwax_deferred_state_owner_matches(&pending,
                                            NUWAX_APP_STATE_SPEAKING,
                                            7U,
                                            11U,
                                            3U,
                                            4U)) {
        puts("[FAIL] stale deferred state crossed an ownership generation");
        return 1;
    }
    nuwax_deferred_state_clear(&pending);
    if (pending.pending) {
        puts("[FAIL] deferred state did not clear");
        return 1;
    }
    puts("deferred state generation check: PASS");
    return 0;
}
