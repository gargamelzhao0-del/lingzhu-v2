#include <stdio.h>

#include "conversation/ui_sync_pending.h"

int main(void) {
    nuwax_ui_sync_pending_t sync = {0};
    nuwax_ui_sync_submit(&sync, NUWAX_APP_STATE_LISTENING, 11);
    nuwax_ui_sync_submit(&sync, NUWAX_APP_STATE_SPEAKING, 12);

    if (nuwax_ui_sync_complete(&sync, NUWAX_APP_STATE_LISTENING, 11)) {
        puts("[FAIL] stale completion cleared a newer UI update");
        return 1;
    }
    if (!nuwax_ui_sync_matches(&sync, NUWAX_APP_STATE_SPEAKING, 12)) {
        puts("[FAIL] newer UI update was not preserved");
        return 1;
    }
    if (!nuwax_ui_sync_complete(&sync, NUWAX_APP_STATE_SPEAKING, 12) ||
        sync.pending) {
        puts("[FAIL] current UI update did not complete");
        return 1;
    }
    puts("ui sync pending generation check: PASS");
    return 0;
}
