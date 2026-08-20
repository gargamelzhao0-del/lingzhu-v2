#pragma once

#include <stdbool.h>

typedef enum {
    NUWAX_RT_CAPTURE_WAIT_FRAME_READY = 0,
    NUWAX_RT_CAPTURE_WAIT_CONTROL_POLL_IDLE,
    NUWAX_RT_CAPTURE_WAIT_WORKER_STOPPED,
} nuwax_rt_capture_wait_result_t;

static inline nuwax_rt_capture_wait_result_t
nuwax_rt_capture_wait_classify(bool frame_ready, bool worker_stopped) {
    if (frame_ready) {
        return NUWAX_RT_CAPTURE_WAIT_FRAME_READY;
    }
    return worker_stopped ? NUWAX_RT_CAPTURE_WAIT_WORKER_STOPPED
                          : NUWAX_RT_CAPTURE_WAIT_CONTROL_POLL_IDLE;
}

static inline bool nuwax_rt_capture_wait_counts_mic_timeout(
    nuwax_rt_capture_wait_result_t wait_result,
    bool frame_reports_timeout,
    bool frame_has_samples) {
    return wait_result == NUWAX_RT_CAPTURE_WAIT_FRAME_READY &&
           frame_reports_timeout && !frame_has_samples;
}
