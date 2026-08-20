#include <stdio.h>

#include "conversation/capture_phase_handoff.h"

static int failures;

#define CHECK(condition, message)                                      \
    do {                                                               \
        if (!(condition)) {                                            \
            fprintf(stderr, "FAIL: %s\n", (message));                 \
            failures++;                                                \
        }                                                              \
    } while (0)

int main(void) {
    nuwax_capture_phase_handoff_t state = {0};
    CHECK(nuwax_capture_phase_resolve_early_playback_complete(
              false, false, false, false, false) ==
              NUWAX_CAPTURE_PLAYBACK_COMPLETE_NO_HANDOFF,
          "playback completion without a handoff must use normal scheduling");
    CHECK(nuwax_capture_phase_resolve_early_playback_complete(
              true, true, true, false, true) ==
              NUWAX_CAPTURE_PLAYBACK_COMPLETE_KEEP_AWAKE_OWNER,
          "verified active awake owner may survive an empty playback");
    CHECK(nuwax_capture_phase_resolve_early_playback_complete(
              true, false, false, true, false) ==
              NUWAX_CAPTURE_PLAYBACK_COMPLETE_DEFER_CANCEL_REARM,
          "cancelled non-awake owner must defer rearm until cleanup");
    CHECK(nuwax_capture_phase_resolve_early_playback_complete(
              true, true, false, false, true) ==
              NUWAX_CAPTURE_PLAYBACK_COMPLETE_DEFER_CANCEL_REARM,
          "seamless intent without a live capture owner must not be trusted");
    CHECK(nuwax_capture_phase_resolve_early_playback_complete(
              true, true, true, true, true) ==
              NUWAX_CAPTURE_PLAYBACK_COMPLETE_DEFER_CANCEL_REARM,
          "cancelled capture must never be retained as seamless");
    CHECK(nuwax_capture_phase_cancel_recovery_needs_resume(
              true, true, false) &&
              nuwax_capture_phase_cancel_recovery_needs_resume(
                  true, false, true) &&
              !nuwax_capture_phase_cancel_recovery_needs_resume(
                  false, true, false) &&
              !nuwax_capture_phase_cancel_recovery_needs_resume(
                  true, false, false),
          "cancel recovery must resume only active playback/barge capture");
    CHECK(nuwax_capture_phase_preserve_stale_transition(true, true) &&
              !nuwax_capture_phase_preserve_stale_transition(true, false) &&
              !nuwax_capture_phase_preserve_stale_transition(false, true),
          "only a pending persistent awake-to-playback transition may retain a stale frame");
    CHECK(nuwax_capture_phase_may_discard_uncommitted_candidate(
              true, false) &&
              !nuwax_capture_phase_may_discard_uncommitted_candidate(
                  false, false) &&
              !nuwax_capture_phase_may_discard_uncommitted_candidate(
                  true, true),
          "local confirmation must not make uncommitted stale preroll fatal");
    CHECK(nuwax_capture_phase_stop_before_next_frame(true, true, false) &&
              !nuwax_capture_phase_stop_before_next_frame(false, true, false) &&
              !nuwax_capture_phase_stop_before_next_frame(true, false, false) &&
              !nuwax_capture_phase_stop_before_next_frame(true, true, true),
          "explicit capture stop must precede the next frame unless an old session is draining");

    nuwax_capture_phase_handoff_begin_awake_prelisten(&state);
    CHECK(state.phase == NUWAX_CAPTURE_PHASE_AWAKE_PRELISTEN,
          "awake local prelisten has explicit phase");
    CHECK(nuwax_capture_phase_handoff_request_playback_without_session(
              &state, 1959U),
          "awake local prelisten transitions directly to playback prelisten");
    CHECK(state.phase == NUWAX_CAPTURE_PHASE_DURING_PLAYBACK_PRELISTEN,
          "local prelisten handoff owns no draining transaction");
    nuwax_capture_phase_handoff_begin_awake_prelisten(&state);
    CHECK(nuwax_capture_phase_handoff_promote_awake_uplink(&state),
          "confirmed speech promotes local prelisten to awake uplink");
    nuwax_capture_phase_handoff_begin_awake(&state);
    CHECK(state.phase == NUWAX_CAPTURE_PHASE_AWAKE_UPLINK,
          "awake uplink must be the initial active phase");
    CHECK(!nuwax_capture_phase_routes_to_preroll(&state),
          "awake frames must not enter playback preroll");

    CHECK(nuwax_capture_phase_handoff_request(&state, 1960U, 1949U),
          "first frame-boundary handoff must succeed");
    CHECK(state.transition_capture_sequence == 1960U &&
              state.frozen_old_wire_frames == 1949U,
          "capture and old wire timelines must remain distinct");
    CHECK(nuwax_capture_phase_routes_to_preroll(&state),
          "the transition frame must immediately enter preroll");
    CHECK(!nuwax_capture_phase_allows_new_session(&state),
          "new listen must wait for old END and listen-stop");
    CHECK(!nuwax_capture_phase_handoff_request(&state, 1961U, 1950U),
          "duplicate handoff must not move the frozen old boundary");
    CHECK(state.transition_capture_sequence == 1960U &&
              state.frozen_old_wire_frames == 1949U,
          "duplicate request must be idempotent");

    nuwax_capture_phase_handoff_mark_old_session_closed(&state);
    CHECK(nuwax_capture_phase_routes_to_preroll(&state),
          "closing the old session must not stop capture");
    CHECK(nuwax_capture_phase_allows_new_session(&state),
          "barge listen may start only after old session closure");

    CHECK(nuwax_capture_phase_handoff_request_awake(&state, 2048U),
          "natural playback completion must return to awake in place");
    CHECK(state.phase == NUWAX_CAPTURE_PHASE_AWAKE_PRELISTEN &&
              state.transition_capture_sequence == 2048U,
          "reverse handoff must preserve the device capture timeline");
    CHECK(nuwax_capture_phase_handoff_promote_awake_uplink(&state),
          "preserved onset promotes resumed prelisten to awake uplink");
    CHECK(!nuwax_capture_phase_routes_to_preroll(&state),
          "post-tail awake frames must route to the new uplink");

    nuwax_capture_phase_handoff_begin_during_playback(&state);
    CHECK(nuwax_capture_phase_allows_new_session(&state),
          "a stream born during playback has no old session to drain");
    CHECK(nuwax_capture_phase_handoff_request_awake(&state, 9U),
          "initial during-playback capture must support in-place resume");

    nuwax_capture_phase_handoff_begin_during_playback(&state);
    CHECK(nuwax_capture_phase_handoff_request_playback_session_close(
              &state, 88U, 17U),
          "an active barge listen must drain before awake resume");
    CHECK(!nuwax_capture_phase_allows_new_session(&state),
          "barge END without listen-stop must still block awake listen");
    nuwax_capture_phase_handoff_mark_old_session_closed(&state);
    CHECK(nuwax_capture_phase_allows_new_session(&state),
          "barge listen-stop completion returns to playback preroll ownership");
    CHECK(nuwax_capture_phase_handoff_request_awake(&state, 89U),
          "closed barge listen may resume awake on the next frame");

    nuwax_capture_phase_handoff_begin_awake(&state);
    CHECK(nuwax_capture_phase_handoff_request(&state, UINT32_MAX, 77U),
          "capture sequence wrap boundary is a valid handoff point");
    nuwax_capture_phase_handoff_fail(&state);
    CHECK(!nuwax_capture_phase_routes_to_preroll(&state) &&
              !nuwax_capture_phase_allows_new_session(&state),
          "failed old-session closure must fail closed");

    printf("capture phase handoff host check: %s (%d failures)\n",
           failures == 0 ? "PASS" : "FAIL",
           failures);
    return failures == 0 ? 0 : 1;
}
