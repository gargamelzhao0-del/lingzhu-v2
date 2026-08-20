#include "turn_guard.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition, message)                                              \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "FAIL: %s\n", message);                          \
            return 1;                                                          \
        }                                                                      \
    } while (0)

int main(void) {
    nuwax_turn_guard_t guard;
    nuwax_turn_guard_init(&guard);

    CHECK(nuwax_turn_guard_begin(&guard, 7U, "req-1", NULL, 100U, 500U) == 1U,
          "request turn did not start");
    CHECK(nuwax_turn_guard_matches(&guard, 7U, "req-1", "turn-1", NULL),
          "matching request was rejected");
    CHECK(!nuwax_turn_guard_matches(&guard, 7U, "old-req", "turn-1", NULL),
          "stale request was accepted");
    CHECK(!nuwax_turn_guard_matches(&guard, 8U, "req-1", "turn-1", NULL),
          "wrong awake generation was accepted");
    CHECK(nuwax_turn_guard_bind_ids(&guard, "req-1", "turn-1", NULL),
          "turn binding failed");
    CHECK(nuwax_turn_guard_bind_speaker_generation(&guard, 12U),
          "speaker generation binding failed");
    CHECK(nuwax_turn_guard_speaker_generation_matches(&guard, 12U),
          "speaker generation did not match");
    CHECK(!nuwax_turn_guard_bind_speaker_generation(&guard, 13U),
          "conflicting speaker generation was accepted");
    CHECK(nuwax_turn_guard_transition(&guard,
                                      NUWAX_TURN_PHASE_WAIT_TEXT,
                                      NUWAX_TURN_PHASE_WAIT_TTS,
                                      200U,
                                      300U),
          "text-to-TTS transition failed");
    CHECK(!nuwax_turn_guard_transition(&guard,
                                       NUWAX_TURN_PHASE_WAIT_TTS,
                                       NUWAX_TURN_PHASE_WAIT_TEXT,
                                       200U,
                                       300U),
          "backward TTS-to-text transition was accepted");
    CHECK(nuwax_turn_guard_transition(&guard,
                                      NUWAX_TURN_PHASE_WAIT_TTS,
                                      NUWAX_TURN_PHASE_PLAYING,
                                      220U,
                                      300U),
          "TTS-to-playing transition failed");
    CHECK(guard.phase_started_ms == 220U,
          "playing phase start was not recorded");
    CHECK(nuwax_turn_guard_transition(&guard,
                                      NUWAX_TURN_PHASE_PLAYING,
                                      NUWAX_TURN_PHASE_PLAYING,
                                      400U,
                                      300U),
          "playing progress refresh failed");
    CHECK(guard.phase_started_ms == 220U,
          "playing progress incorrectly reset absolute phase start");
    CHECK(!nuwax_turn_guard_phase_runtime_expired(
              &guard, NUWAX_TURN_PHASE_PLAYING, 519U, 300U),
          "absolute playing limit expired early");
    CHECK(nuwax_turn_guard_phase_runtime_expired(
              &guard, NUWAX_TURN_PHASE_PLAYING, 520U, 300U),
          "absolute playing limit was extended by progress");
    CHECK(!nuwax_turn_guard_deadline_expired(&guard, 699U),
          "refreshed activity deadline expired early");
    CHECK(nuwax_turn_guard_deadline_expired(&guard, 700U),
          "refreshed activity deadline did not expire");
    CHECK(!nuwax_turn_guard_transition(&guard,
                                       NUWAX_TURN_PHASE_PLAYING,
                                       NUWAX_TURN_PHASE_WAIT_TTS,
                                       220U,
                                       300U),
          "backward playing-to-TTS transition was accepted");
    CHECK(!nuwax_turn_guard_transition(&guard,
                                       NUWAX_TURN_PHASE_WAIT_TEXT,
                                       NUWAX_TURN_PHASE_PLAYING,
                                       200U,
                                       300U),
          "invalid phase transition was accepted");
    const uint32_t previous_epoch = guard.epoch;
    nuwax_turn_guard_clear(&guard);
    CHECK(!guard.active && guard.epoch == previous_epoch,
          "clear did not preserve monotonic epoch");
    CHECK(nuwax_turn_guard_begin(&guard, 9U, NULL, "turn-2", UINT32_MAX - 9U, 20U) ==
              previous_epoch + 1U,
          "second turn did not advance epoch");
    CHECK(!nuwax_turn_guard_deadline_expired(&guard, 5U),
          "wrap-safe deadline expired early");
    CHECK(nuwax_turn_guard_deadline_expired(&guard, 10U),
          "wrap-safe deadline did not expire");
    CHECK(!nuwax_turn_guard_phase_runtime_expired(
              &guard, NUWAX_TURN_PHASE_WAIT_TEXT, 4U, 15U),
          "wrap-safe phase runtime expired early");
    CHECK(nuwax_turn_guard_phase_runtime_expired(
              &guard, NUWAX_TURN_PHASE_WAIT_TEXT, 5U, 15U),
          "wrap-safe phase runtime did not expire");

    nuwax_turn_guard_clear(&guard);
    CHECK(nuwax_turn_guard_begin(&guard,
                                 10U,
                                 "req-retry",
                                 "turn-retry",
                                 1000U,
                                 30000U) != 0U,
          "metadata retry turn did not start");
    CHECK(nuwax_turn_guard_bind_ids(&guard,
                                    "req-retry",
                                    "turn-retry",
                                    "pb-a"),
          "initial metadata playback did not bind");
    CHECK(nuwax_turn_guard_transition(&guard,
                                      NUWAX_TURN_PHASE_WAIT_TEXT,
                                      NUWAX_TURN_PHASE_PLAYING,
                                      1010U,
                                      60000U),
          "initial metadata playback did not enter playing");
    CHECK(nuwax_turn_guard_retire_metadata_playback(
              &guard, 1020U, 3000U, 2U),
          "first metadata-only playback was not retired");
    CHECK(guard.phase == NUWAX_TURN_PHASE_WAIT_TTS_RETRY &&
              guard.playback_id[0] == '\0' &&
              guard.metadata_retry_count == 1U &&
              guard.deadline_ms == 4020U,
          "first metadata retry state was not bounded");
    CHECK(!nuwax_turn_guard_matches(&guard,
                                    10U,
                                    "req-retry",
                                    "turn-retry",
                                    "pb-a"),
          "retired playback was accepted again");
    CHECK(!nuwax_turn_guard_matches(&guard,
                                    10U,
                                    "req-retry",
                                    "turn-other",
                                    "pb-b"),
          "cross-turn retry playback was accepted");
    CHECK(nuwax_turn_guard_matches(&guard,
                                   10U,
                                   "req-retry",
                                   "turn-retry",
                                   "pb-b") &&
              nuwax_turn_guard_bind_ids(&guard,
                                        "req-retry",
                                        "turn-retry",
                                        "pb-b"),
          "same-turn retry playback did not bind");
    CHECK(nuwax_turn_guard_transition(&guard,
                                      NUWAX_TURN_PHASE_WAIT_TTS_RETRY,
                                      NUWAX_TURN_PHASE_PLAYING,
                                      1030U,
                                      60000U),
          "retry playback did not enter playing");
    CHECK(nuwax_turn_guard_retire_metadata_playback(
              &guard, 1040U, 3000U, 2U),
          "second metadata-only playback was not retired");
    CHECK(!nuwax_turn_guard_matches(&guard,
                                    10U,
                                    "req-retry",
                                    "turn-retry",
                                    "pb-b"),
          "second retired playback was accepted again");
    CHECK(nuwax_turn_guard_matches(&guard,
                                   10U,
                                   "req-retry",
                                   "turn-retry",
                                   "pb-c") &&
              nuwax_turn_guard_bind_ids(&guard,
                                        "req-retry",
                                        "turn-retry",
                                        "pb-c") &&
              nuwax_turn_guard_transition(
                  &guard,
                  NUWAX_TURN_PHASE_WAIT_TTS_RETRY,
                  NUWAX_TURN_PHASE_PLAYING,
                  1050U,
                  60000U),
          "final allowed retry playback did not start");
    CHECK(!nuwax_turn_guard_retire_metadata_playback(
              &guard, 1060U, 3000U, 2U),
          "metadata retry limit was not enforced");
    CHECK(guard.phase == NUWAX_TURN_PHASE_PLAYING &&
              strcmp(guard.playback_id, "pb-c") == 0,
          "retry exhaustion corrupted the active playback owner");

    nuwax_turn_guard_clear(&guard);
    CHECK(nuwax_turn_guard_begin(&guard,
                                 11U,
                                 NULL,
                                 "turn-physical",
                                 2000U,
                                 30000U) != 0U &&
              nuwax_turn_guard_bind_ids(&guard,
                                        NULL,
                                        "turn-physical",
                                        "pb-physical") &&
              nuwax_turn_guard_bind_speaker_generation(&guard, 42U),
          "physical playback guard setup failed");
    CHECK(!nuwax_turn_guard_retire_metadata_playback(
              &guard, 2010U, 3000U, 2U),
          "physical playback was incorrectly treated as metadata-only");

    nuwax_turn_guard_clear(&guard);
    CHECK(nuwax_turn_guard_begin(&guard,
                                 12U,
                                 "req-skip",
                                 "turn-skip",
                                 3000U,
                                 30000U) != 0U,
          "TTS skip guard setup failed");
    CHECK(nuwax_turn_guard_accepts_tts_skip(
              &guard, 12U, "req-skip", "turn-skip"),
          "owned WAIT_TEXT turn rejected a terminal TTS skip");
    CHECK(nuwax_turn_guard_transition(&guard,
                                      NUWAX_TURN_PHASE_WAIT_TEXT,
                                      NUWAX_TURN_PHASE_WAIT_TTS,
                                      3010U,
                                      30000U),
          "TTS skip guard did not enter WAIT_TTS");
    CHECK(nuwax_turn_guard_accepts_tts_skip(
              &guard, 12U, "req-skip", "turn-skip"),
          "matching TTS skip was rejected");
    CHECK(nuwax_turn_guard_accepts_tts_skip(&guard, 12U, "", ""),
          "empty-ID TTS skip was rejected for an owned WAIT_TTS turn");
    CHECK(!nuwax_turn_guard_accepts_tts_skip(
              &guard, 12U, "req-other", "turn-skip"),
          "mismatched TTS skip request was accepted");
    CHECK(!nuwax_turn_guard_accepts_tts_skip(
              &guard, 13U, "req-skip", "turn-skip"),
          "cross-generation TTS skip was accepted");
    CHECK(nuwax_turn_guard_transition(&guard,
                                      NUWAX_TURN_PHASE_WAIT_TTS,
                                      NUWAX_TURN_PHASE_PLAYING,
                                      3020U,
                                      60000U),
          "TTS skip guard did not enter PLAYING");
    CHECK(!nuwax_turn_guard_accepts_tts_skip(
              &guard, 12U, "req-skip", "turn-skip"),
          "TTS skip interrupted an active playback");

    nuwax_turn_guard_clear(&guard);
    CHECK(nuwax_turn_guard_begin(&guard,
                                 13U,
                                 NULL,
                                 NULL,
                                 4000U,
                                 30000U) != 0U &&
              nuwax_turn_guard_transition(&guard,
                                          NUWAX_TURN_PHASE_WAIT_TEXT,
                                          NUWAX_TURN_PHASE_WAIT_TTS,
                                          4010U,
                                          30000U),
          "unowned TTS skip guard setup failed");
    CHECK(!nuwax_turn_guard_accepts_tts_skip(&guard, 13U, "", ""),
          "empty-ID TTS skip was accepted without an owned turn");

    puts("turn_guard_host_check: PASS");
    return 0;
}
