#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NUWAX_TURN_GUARD_REQUEST_ID_LEN 48
#define NUWAX_TURN_GUARD_TURN_ID_LEN 48
#define NUWAX_TURN_GUARD_PLAYBACK_ID_LEN 48
#define NUWAX_TURN_GUARD_RETIRED_PLAYBACK_SLOTS 2

typedef enum {
    NUWAX_TURN_PHASE_INACTIVE = 0,
    NUWAX_TURN_PHASE_WAIT_TEXT,
    NUWAX_TURN_PHASE_WAIT_TTS,
    NUWAX_TURN_PHASE_WAIT_TTS_RETRY,
    NUWAX_TURN_PHASE_PLAYING,
    NUWAX_TURN_PHASE_DRAINING,
} nuwax_turn_phase_t;

typedef struct {
    bool active;
    uint32_t epoch;
    uint32_t awake_generation;
    uint32_t speaker_generation;
    uint32_t phase_started_ms;
    uint32_t deadline_ms;
    nuwax_turn_phase_t phase;
    uint8_t metadata_retry_count;
    uint8_t retired_playback_count;
    char request_id[NUWAX_TURN_GUARD_REQUEST_ID_LEN];
    char turn_id[NUWAX_TURN_GUARD_TURN_ID_LEN];
    char playback_id[NUWAX_TURN_GUARD_PLAYBACK_ID_LEN];
    char retired_playback_ids[NUWAX_TURN_GUARD_RETIRED_PLAYBACK_SLOTS]
                             [NUWAX_TURN_GUARD_PLAYBACK_ID_LEN];
} nuwax_turn_guard_t;

void nuwax_turn_guard_init(nuwax_turn_guard_t *guard);
void nuwax_turn_guard_clear(nuwax_turn_guard_t *guard);
bool nuwax_turn_guard_accepts_rx_rejection(const nuwax_turn_guard_t *guard,
                                          uint32_t awake_generation,
                                          const char *request_id,
                                          const char *turn_id,
                                          const char *playback_id);
uint32_t nuwax_turn_guard_begin(nuwax_turn_guard_t *guard,
                                uint32_t awake_generation,
                                const char *request_id,
                                const char *turn_id,
                                uint32_t now_ms,
                                uint32_t timeout_ms);
bool nuwax_turn_guard_matches(const nuwax_turn_guard_t *guard,
                              uint32_t awake_generation,
                              const char *request_id,
                              const char *turn_id,
                              const char *playback_id);
bool nuwax_turn_guard_accepts_tts_skip(const nuwax_turn_guard_t *guard,
                                       uint32_t awake_generation,
                                       const char *request_id,
                                       const char *turn_id);
bool nuwax_turn_guard_bind_ids(nuwax_turn_guard_t *guard,
                               const char *request_id,
                               const char *turn_id,
                               const char *playback_id);
bool nuwax_turn_guard_retire_metadata_playback(
    nuwax_turn_guard_t *guard,
    uint32_t now_ms,
    uint32_t retry_timeout_ms,
    uint8_t max_retries);
bool nuwax_turn_guard_bind_speaker_generation(nuwax_turn_guard_t *guard,
                                              uint32_t speaker_generation);
bool nuwax_turn_guard_speaker_generation_matches(
    const nuwax_turn_guard_t *guard,
    uint32_t speaker_generation);
bool nuwax_turn_guard_transition(nuwax_turn_guard_t *guard,
                                 nuwax_turn_phase_t expected_phase,
                                 nuwax_turn_phase_t next_phase,
                                 uint32_t now_ms,
                                 uint32_t timeout_ms);
bool nuwax_turn_guard_phase_is(const nuwax_turn_guard_t *guard,
                               nuwax_turn_phase_t phase);
bool nuwax_turn_guard_deadline_expired(const nuwax_turn_guard_t *guard,
                                       uint32_t now_ms);
bool nuwax_turn_guard_phase_runtime_expired(
    const nuwax_turn_guard_t *guard,
    nuwax_turn_phase_t phase,
    uint32_t now_ms,
    uint32_t max_runtime_ms);
const char *nuwax_turn_phase_name(nuwax_turn_phase_t phase);
