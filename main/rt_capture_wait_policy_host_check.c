#include <stdio.h>

#include "conversation/rt_capture_wait_policy.h"

static int failures;

#define CHECK(condition, message)                                      \
    do {                                                               \
        if (!(condition)) {                                            \
            fprintf(stderr, "FAIL: %s\n", (message));                 \
            failures++;                                                \
        }                                                              \
    } while (0)

int main(void) {
    CHECK(nuwax_rt_capture_wait_classify(true, false) ==
              NUWAX_RT_CAPTURE_WAIT_FRAME_READY,
          "an available capture frame must win immediately");
    CHECK(nuwax_rt_capture_wait_classify(false, false) ==
              NUWAX_RT_CAPTURE_WAIT_CONTROL_POLL_IDLE,
          "an empty queue with a live worker is only a control poll");
    CHECK(nuwax_rt_capture_wait_classify(false, true) ==
              NUWAX_RT_CAPTURE_WAIT_WORKER_STOPPED,
          "an empty queue from a stopped worker must fail explicitly");
    CHECK(nuwax_rt_capture_wait_classify(true, true) ==
              NUWAX_RT_CAPTURE_WAIT_FRAME_READY,
          "a final queued frame must be consumed before worker failure");

    CHECK(!nuwax_rt_capture_wait_counts_mic_timeout(
              NUWAX_RT_CAPTURE_WAIT_CONTROL_POLL_IDLE, true, false),
          "queue polling idle must never count as a microphone timeout");
    CHECK(nuwax_rt_capture_wait_counts_mic_timeout(
              NUWAX_RT_CAPTURE_WAIT_FRAME_READY, true, false),
          "an explicit empty timeout frame must count as a microphone timeout");
    CHECK(!nuwax_rt_capture_wait_counts_mic_timeout(
              NUWAX_RT_CAPTURE_WAIT_FRAME_READY, true, true),
          "a partial timeout frame must not count as an empty timeout");
    CHECK(!nuwax_rt_capture_wait_counts_mic_timeout(
              NUWAX_RT_CAPTURE_WAIT_FRAME_READY, false, false),
          "a successful empty frame must not be reclassified as a timeout");

    printf("rt capture wait policy host check: %s (%d failures)\n",
           failures == 0 ? "PASS" : "FAIL",
           failures);
    return failures == 0 ? 0 : 1;
}
