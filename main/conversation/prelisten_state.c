#include "prelisten_state.h"

#include <string.h>

static uint32_t normalize_window_ms(uint32_t window_ms) {
    return window_ms == 0 ? 1 : window_ms;
}

static void copy_mode(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0U) {
        return;
    }
    src = src ? src : "prelisten";
    size_t length = 0U;
    while (length + 1U < dst_size && src[length] != '\0') {
        dst[length] = src[length];
        length++;
    }
    dst[length] = '\0';
}

void nuwax_prelisten_state_clear(nuwax_prelisten_state_t *state) {
    if (!state) {
        return;
    }
    state->pending = false;
    state->capture_active = false;
    state->ready_ms = 0;
    state->deadline_ms = 0;
    state->capture_deadline_ms = 0;
    state->mic_release_stops_capture = false;
    state->mode[0] = '\0';
}

void nuwax_prelisten_state_clear_cancel(nuwax_prelisten_state_t *state) {
    if (!state) {
        return;
    }
    atomic_store_explicit(&state->cancel_capture, false,
                          memory_order_release);
}

void nuwax_prelisten_state_request_cancel(nuwax_prelisten_state_t *state) {
    if (!state) {
        return;
    }
    atomic_store_explicit(&state->cancel_capture, true,
                          memory_order_release);
    nuwax_prelisten_state_clear(state);
}

void nuwax_prelisten_state_start_window(nuwax_prelisten_state_t *state,
                                        const char *mode,
                                        uint32_t now_ms,
                                        uint32_t window_ms,
                                        uint32_t ready_delay_ms) {
    if (!state) {
        return;
    }
    window_ms = normalize_window_ms(window_ms);
    atomic_store_explicit(&state->cancel_capture, false,
                          memory_order_release);
    state->pending = true;
    state->capture_active = false;
    state->ready_ms = now_ms + ready_delay_ms;
    state->deadline_ms = now_ms + window_ms;
    state->capture_deadline_ms = 0;
    state->mic_release_stops_capture = false;
    copy_mode(state->mode, sizeof(state->mode), mode);
}

void nuwax_prelisten_state_start_capture(nuwax_prelisten_state_t *state,
                                         const char *mode,
                                         uint32_t now_ms,
                                         uint32_t window_ms,
                                         bool mic_release_stops_capture) {
    if (!state) {
        return;
    }
    window_ms = normalize_window_ms(window_ms);
    nuwax_prelisten_state_clear(state);
    atomic_store_explicit(&state->cancel_capture, false,
                          memory_order_release);
    state->capture_active = true;
    state->capture_deadline_ms = now_ms + window_ms;
    state->mic_release_stops_capture = mic_release_stops_capture;
    copy_mode(state->mode, sizeof(state->mode), mode);
}

uint32_t nuwax_prelisten_state_activate_pending(nuwax_prelisten_state_t *state, uint32_t now_ms) {
    if (!state) {
        return 0;
    }
    if (!state->pending ||
        (int32_t)(state->deadline_ms - now_ms) <= 0) {
        nuwax_prelisten_state_clear(state);
        return 0;
    }
    const uint32_t remaining_ms = state->deadline_ms - now_ms;
    char mode[sizeof(state->mode)] = {0};
    copy_mode(mode, sizeof(mode),
              nuwax_prelisten_state_mode_or(state, "prelisten"));
    nuwax_prelisten_state_clear(state);
    atomic_store_explicit(&state->cancel_capture, false,
                          memory_order_release);
    state->capture_active = true;
    state->capture_deadline_ms = now_ms + remaining_ms;
    state->mic_release_stops_capture = false;
    copy_mode(state->mode, sizeof(state->mode), mode);
    return remaining_ms;
}

void nuwax_prelisten_state_finish_capture(nuwax_prelisten_state_t *state) {
    if (!state || !state->capture_active) {
        return;
    }
    state->capture_active = false;
    state->capture_deadline_ms = 0;
    state->mic_release_stops_capture = false;
    state->mode[0] = '\0';
}

const char *nuwax_prelisten_state_mode_or(const nuwax_prelisten_state_t *state, const char *fallback) {
    if (state && state->mode[0] != '\0') {
        return state->mode;
    }
    return fallback ? fallback : "";
}

bool nuwax_prelisten_state_mode_is(const nuwax_prelisten_state_t *state, const char *mode) {
    if (!state || !mode) {
        return false;
    }
    return strcmp(state->mode, mode) == 0;
}
