#pragma once

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>

#define NUWAX_PRELISTEN_MODE_MAX_LEN 24

typedef struct {
    uint32_t ready_ms;
    uint32_t deadline_ms;
    uint32_t capture_deadline_ms;
    bool pending;
    bool capture_active;
    _Atomic bool cancel_capture;
    bool mic_release_stops_capture;
    char mode[NUWAX_PRELISTEN_MODE_MAX_LEN];
} nuwax_prelisten_state_t;

void nuwax_prelisten_state_clear(nuwax_prelisten_state_t *state);
void nuwax_prelisten_state_clear_cancel(nuwax_prelisten_state_t *state);
void nuwax_prelisten_state_request_cancel(nuwax_prelisten_state_t *state);
void nuwax_prelisten_state_start_window(nuwax_prelisten_state_t *state,
                                        const char *mode,
                                        uint32_t now_ms,
                                        uint32_t window_ms,
                                        uint32_t ready_delay_ms);
void nuwax_prelisten_state_start_capture(nuwax_prelisten_state_t *state,
                                         const char *mode,
                                         uint32_t now_ms,
                                         uint32_t window_ms,
                                         bool mic_release_stops_capture);
uint32_t nuwax_prelisten_state_activate_pending(nuwax_prelisten_state_t *state, uint32_t now_ms);
void nuwax_prelisten_state_finish_capture(nuwax_prelisten_state_t *state);
const char *nuwax_prelisten_state_mode_or(const nuwax_prelisten_state_t *state, const char *fallback);
bool nuwax_prelisten_state_mode_is(const nuwax_prelisten_state_t *state, const char *mode);
