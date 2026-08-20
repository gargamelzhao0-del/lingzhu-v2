#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "conversation/prelisten_state.h"

static int s_failures;

static void check(bool condition, const char *name) {
    if (condition) {
        printf("[PASS] %s\n", name);
        return;
    }
    printf("[FAIL] %s\n", name);
    s_failures++;
}

int main(void) {
    nuwax_prelisten_state_t state = {0};

    nuwax_prelisten_state_start_window(&state, "awake_session", 1000U,
                                       1000U, 50U);
    const uint32_t remaining =
        nuwax_prelisten_state_activate_pending(&state, 1100U);
    check(remaining == 900U && state.capture_active && !state.pending &&
              state.capture_deadline_ms == 2000U &&
              strcmp(state.mode, "awake_session") == 0,
          "pending window activates with exact remaining budget");

    nuwax_prelisten_state_start_window(&state, "equal_deadline", 2000U,
                                       100U, 0U);
    check(nuwax_prelisten_state_activate_pending(&state, 2100U) == 0U &&
              !state.pending && !state.capture_active && state.mode[0] == '\0',
          "deadline equality expires instead of creating a wrapped window");

    nuwax_prelisten_state_start_window(&state, "expired", 3000U, 100U, 0U);
    check(nuwax_prelisten_state_activate_pending(&state, 3101U) == 0U &&
              !state.pending && !state.capture_active,
          "expired pending window is cleared");

    const uint32_t wrap_start = UINT32_MAX - 100U;
    nuwax_prelisten_state_start_window(&state, "wrap", wrap_start, 200U, 0U);
    const uint32_t wrap_remaining =
        nuwax_prelisten_state_activate_pending(&state, UINT32_MAX - 50U);
    check(wrap_remaining == 150U && state.capture_active &&
              state.capture_deadline_ms == 99U,
          "pending activation is wrap safe before uint32 rollover");

    nuwax_prelisten_state_start_window(&state, "wrap_after", wrap_start,
                                       200U, 0U);
    check(nuwax_prelisten_state_activate_pending(&state, 20U) == 79U &&
              state.capture_deadline_ms == 99U,
          "pending activation is wrap safe after uint32 rollover");

    nuwax_prelisten_state_start_capture(&state, "cancel", 4000U, 500U,
                                        false);
    nuwax_prelisten_state_request_cancel(&state);
    check(atomic_load_explicit(&state.cancel_capture,
                               memory_order_acquire) &&
              !state.pending && !state.capture_active,
          "cancel request atomically survives capture-state cleanup");
    nuwax_prelisten_state_clear_cancel(&state);
    check(!atomic_load_explicit(&state.cancel_capture,
                                memory_order_acquire),
          "cancel acknowledgement atomically clears the request");

    printf("prelisten state host check: %s (%d failures)\n",
           s_failures == 0 ? "PASS" : "FAIL", s_failures);
    return s_failures == 0 ? 0 : 1;
}
