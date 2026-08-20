#include <stdio.h>
#include <string.h>

#include "conversation/emotion_ui_pending.h"

int main(void) {
    nuwax_emotion_ui_pending_t pending = {0};
    const uint32_t first =
        nuwax_emotion_ui_submit(&pending, "happy", "device_command");
    const uint32_t second =
        nuwax_emotion_ui_submit(&pending, "tired", "text_response");

    if (first == 0 || second == 0 || second == first) {
        puts("[FAIL] revision did not advance");
        return 1;
    }
    if (nuwax_emotion_ui_complete(&pending, first)) {
        puts("[FAIL] stale completion cleared the latest emotion");
        return 1;
    }
    if (!nuwax_emotion_ui_matches(&pending, second) ||
        strcmp(pending.emotion, "tired") != 0 ||
        strcmp(pending.source, "text_response") != 0) {
        puts("[FAIL] latest emotion was not retained");
        return 1;
    }
    if (!nuwax_emotion_ui_complete(&pending, second) || pending.pending) {
        puts("[FAIL] latest emotion did not complete");
        return 1;
    }

    const uint32_t displayed_revision = pending.revision;
    nuwax_emotion_ui_invalidate(&pending);
    if (pending.pending || pending.revision == displayed_revision) {
        puts("[FAIL] listening boundary did not invalidate displayed emotion");
        return 1;
    }

    char oversized[64];
    memset(oversized, 'x', sizeof(oversized));
    oversized[sizeof(oversized) - 1] = '\0';
    (void)nuwax_emotion_ui_submit(&pending, oversized, oversized);
    if (pending.emotion[sizeof(pending.emotion) - 1] != '\0' ||
        pending.source[sizeof(pending.source) - 1] != '\0') {
        puts("[FAIL] bounded strings were not terminated");
        return 1;
    }

    const uint32_t leased_revision = nuwax_emotion_ui_submit_leased(
        &pending, "thinking", "device_command_sleep", 3000U, true);
    const nuwax_emotion_ui_pending_t leased_snapshot = pending;
    const uint32_t replacement_revision = nuwax_emotion_ui_submit_leased(
        &pending, "playful", "device_command_sleep", 5000U, true);
    if (leased_revision == 0U || replacement_revision == leased_revision ||
        !leased_snapshot.finite_lease ||
        leased_snapshot.duration_ms != 3000U ||
        !pending.finite_lease || pending.duration_ms != 5000U ||
        strcmp(pending.emotion, "playful") != 0) {
        puts("[FAIL] leased emotion metadata did not follow latest revision");
        return 1;
    }

    const uint32_t turn_revision =
        nuwax_emotion_ui_submit(&pending, "happy", "filtered_text_response");
    if (!nuwax_emotion_ui_cancel_turn_scoped(&pending) || pending.pending ||
        pending.revision == turn_revision || pending.emotion[0] != '\0' ||
        pending.source[0] != '\0') {
        puts("[FAIL] turn-scoped emotion was not invalidated");
        return 1;
    }

    (void)nuwax_emotion_ui_submit(&pending, "focused", "device_command");
    if (nuwax_emotion_ui_cancel_turn_scoped(&pending) || !pending.pending ||
        strcmp(pending.emotion, "focused") != 0) {
        puts("[FAIL] persistent command emotion was cancelled");
        return 1;
    }

    const uint32_t listening_boundary_revision = pending.revision;
    nuwax_emotion_ui_invalidate(&pending);
    if (pending.pending || pending.emotion[0] != '\0' ||
        pending.source[0] != '\0' ||
        pending.revision == listening_boundary_revision) {
        puts("[FAIL] listening boundary did not invalidate persistent emotion");
        return 1;
    }

    const uint32_t shake_revision =
        nuwax_emotion_ui_submit(&pending, "dizziness", "shake_agent");
    const nuwax_emotion_ui_pending_t shake_snapshot = pending;
    if (nuwax_emotion_ui_cancel_source(&pending, "head_touch_agent") ||
        !pending.pending || pending.revision != shake_revision) {
        puts("[FAIL] unrelated source cancelled shake emotion");
        return 1;
    }
    if (!nuwax_emotion_ui_cancel_source(&pending, "shake_agent") ||
        pending.pending || pending.revision == shake_revision ||
        nuwax_emotion_ui_matches(&pending, shake_snapshot.revision)) {
        puts("[FAIL] shake source cancellation did not invalidate snapshot");
        return 1;
    }

    puts("emotion UI pending latest-wins check: PASS");
    return 0;
}
