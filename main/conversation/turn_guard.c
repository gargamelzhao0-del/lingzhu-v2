#include "turn_guard.h"

#include <string.h>

bool nuwax_turn_guard_accepts_rx_rejection(const nuwax_turn_guard_t *guard,
                                          uint32_t awake_generation,
                                          const char *request_id,
                                          const char *turn_id,
                                          const char *playback_id) {
    if (!guard || !guard->active || guard->speaker_generation != 0U ||
        (guard->phase != NUWAX_TURN_PHASE_WAIT_TEXT &&
         guard->phase != NUWAX_TURN_PHASE_WAIT_TTS &&
         guard->phase != NUWAX_TURN_PHASE_WAIT_TTS_RETRY) ||
        !turn_id || !turn_id[0] || !playback_id || !playback_id[0]) {
        return false;
    }
    /* A rejection cannot bootstrap an owner or cancel an unrelated wait.
     * Require an already known identity, beyond permissive BEGIN binding. */
    const bool anchored =
        (guard->request_id[0] && request_id && request_id[0] &&
         strcmp(guard->request_id, request_id) == 0) ||
        (guard->turn_id[0] && strcmp(guard->turn_id, turn_id) == 0);
    return anchored && nuwax_turn_guard_matches(
        guard, awake_generation, request_id, turn_id, playback_id);
}

static void copy_id(char *out, size_t out_size, const char *value) {
    if (!out || out_size == 0U) {
        return;
    }
    if (!value) {
        value = "";
    }
    size_t length = 0U;
    while (length + 1U < out_size && value[length] != '\0') {
        length++;
    }
    memcpy(out, value, length);
    out[length] = '\0';
}

static bool id_present(const char *value) {
    return value && value[0] != '\0';
}

static bool playback_id_retired(const nuwax_turn_guard_t *guard,
                                const char *playback_id) {
    if (!guard || !id_present(playback_id)) {
        return false;
    }
    for (uint8_t i = 0U; i < guard->retired_playback_count; ++i) {
        if (strcmp(guard->retired_playback_ids[i], playback_id) == 0) {
            return true;
        }
    }
    return false;
}

static bool compare_bound_id(const char *bound,
                             const char *incoming,
                             bool *comparable,
                             bool *matched) {
    if (!id_present(bound) || !id_present(incoming)) {
        return true;
    }
    *comparable = true;
    if (strcmp(bound, incoming) != 0) {
        return false;
    }
    *matched = true;
    return true;
}

void nuwax_turn_guard_init(nuwax_turn_guard_t *guard) {
    if (!guard) {
        return;
    }
    memset(guard, 0, sizeof(*guard));
}

void nuwax_turn_guard_clear(nuwax_turn_guard_t *guard) {
    if (!guard) {
        return;
    }
    const uint32_t epoch = guard->epoch;
    memset(guard, 0, sizeof(*guard));
    guard->epoch = epoch;
}

uint32_t nuwax_turn_guard_begin(nuwax_turn_guard_t *guard,
                                uint32_t awake_generation,
                                const char *request_id,
                                const char *turn_id,
                                uint32_t now_ms,
                                uint32_t timeout_ms) {
    if (!guard || awake_generation == 0U || timeout_ms == 0U) {
        return 0U;
    }
    uint32_t epoch = guard->epoch + 1U;
    if (epoch == 0U) {
        epoch = 1U;
    }
    memset(guard, 0, sizeof(*guard));
    guard->active = true;
    guard->epoch = epoch;
    guard->awake_generation = awake_generation;
    guard->phase = NUWAX_TURN_PHASE_WAIT_TEXT;
    guard->phase_started_ms = now_ms;
    guard->deadline_ms = now_ms + timeout_ms;
    copy_id(guard->request_id, sizeof(guard->request_id), request_id);
    copy_id(guard->turn_id, sizeof(guard->turn_id), turn_id);
    return epoch;
}

bool nuwax_turn_guard_matches(const nuwax_turn_guard_t *guard,
                              uint32_t awake_generation,
                              const char *request_id,
                              const char *turn_id,
                              const char *playback_id) {
    if (!guard || !guard->active || guard->awake_generation == 0U ||
        guard->awake_generation != awake_generation) {
        return false;
    }

    bool comparable = false;
    bool matched = false;
    if (playback_id_retired(guard, playback_id)) {
        return false;
    }
    if (!compare_bound_id(guard->request_id, request_id, &comparable, &matched) ||
        !compare_bound_id(guard->turn_id, turn_id, &comparable, &matched) ||
        !compare_bound_id(guard->playback_id, playback_id, &comparable, &matched)) {
        return false;
    }

    const bool guard_has_id = id_present(guard->request_id) ||
                              id_present(guard->turn_id) ||
                              id_present(guard->playback_id);
    return !guard_has_id || (comparable && matched);
}

bool nuwax_turn_guard_accepts_tts_skip(const nuwax_turn_guard_t *guard,
                                       uint32_t awake_generation,
                                       const char *request_id,
                                       const char *turn_id) {
    if (!guard || !guard->active ||
        guard->awake_generation != awake_generation ||
        (guard->phase != NUWAX_TURN_PHASE_WAIT_TEXT &&
         guard->phase != NUWAX_TURN_PHASE_WAIT_TTS &&
         guard->phase != NUWAX_TURN_PHASE_WAIT_TTS_RETRY) ||
        guard->speaker_generation != 0U || id_present(guard->playback_id)) {
        return false;
    }

    if (!id_present(request_id) && !id_present(turn_id)) {
        return id_present(guard->turn_id);
    }
    return nuwax_turn_guard_matches(guard,
                                    awake_generation,
                                    request_id,
                                    turn_id,
                                    NULL);
}

bool nuwax_turn_guard_bind_ids(nuwax_turn_guard_t *guard,
                               const char *request_id,
                               const char *turn_id,
                               const char *playback_id) {
    if (!guard || !guard->active) {
        return false;
    }
    if (playback_id_retired(guard, playback_id)) {
        return false;
    }
    if ((id_present(guard->request_id) && id_present(request_id) &&
         strcmp(guard->request_id, request_id) != 0) ||
        (id_present(guard->turn_id) && id_present(turn_id) &&
         strcmp(guard->turn_id, turn_id) != 0) ||
        (id_present(guard->playback_id) && id_present(playback_id) &&
         strcmp(guard->playback_id, playback_id) != 0)) {
        return false;
    }
    if (!id_present(guard->request_id) && id_present(request_id)) {
        copy_id(guard->request_id, sizeof(guard->request_id), request_id);
    }
    if (!id_present(guard->turn_id) && id_present(turn_id)) {
        copy_id(guard->turn_id, sizeof(guard->turn_id), turn_id);
    }
    if (!id_present(guard->playback_id) && id_present(playback_id)) {
        copy_id(guard->playback_id, sizeof(guard->playback_id), playback_id);
    }
    return true;
}

bool nuwax_turn_guard_retire_metadata_playback(
    nuwax_turn_guard_t *guard,
    uint32_t now_ms,
    uint32_t retry_timeout_ms,
    uint8_t max_retries) {
    if (!guard || !guard->active || retry_timeout_ms == 0U ||
        max_retries == 0U || guard->speaker_generation != 0U ||
        !id_present(guard->turn_id) || !id_present(guard->playback_id) ||
        guard->metadata_retry_count >= max_retries ||
        (guard->phase != NUWAX_TURN_PHASE_WAIT_TEXT &&
         guard->phase != NUWAX_TURN_PHASE_WAIT_TTS &&
         guard->phase != NUWAX_TURN_PHASE_WAIT_TTS_RETRY &&
         guard->phase != NUWAX_TURN_PHASE_PLAYING)) {
        return false;
    }

    const uint8_t slot =
        (uint8_t)(guard->metadata_retry_count %
                  NUWAX_TURN_GUARD_RETIRED_PLAYBACK_SLOTS);
    copy_id(guard->retired_playback_ids[slot],
            sizeof(guard->retired_playback_ids[slot]),
            guard->playback_id);
    if (guard->retired_playback_count <
        NUWAX_TURN_GUARD_RETIRED_PLAYBACK_SLOTS) {
        guard->retired_playback_count++;
    }
    guard->metadata_retry_count++;
    guard->playback_id[0] = '\0';
    guard->phase = NUWAX_TURN_PHASE_WAIT_TTS_RETRY;
    guard->phase_started_ms = now_ms;
    guard->deadline_ms = now_ms + retry_timeout_ms;
    if (guard->deadline_ms == 0U) {
        guard->deadline_ms = 1U;
    }
    return true;
}

bool nuwax_turn_guard_bind_speaker_generation(nuwax_turn_guard_t *guard,
                                              uint32_t speaker_generation) {
    if (!guard || !guard->active || speaker_generation == 0U) {
        return false;
    }
    if (guard->speaker_generation != 0U &&
        guard->speaker_generation != speaker_generation) {
        return false;
    }
    guard->speaker_generation = speaker_generation;
    return true;
}

bool nuwax_turn_guard_speaker_generation_matches(
    const nuwax_turn_guard_t *guard,
    uint32_t speaker_generation) {
    return guard && guard->active && guard->speaker_generation != 0U &&
           guard->speaker_generation == speaker_generation;
}

static bool phase_transition_allowed(nuwax_turn_phase_t current,
                                     nuwax_turn_phase_t next) {
    switch (current) {
        case NUWAX_TURN_PHASE_WAIT_TEXT:
            return next == NUWAX_TURN_PHASE_WAIT_TEXT ||
                   next == NUWAX_TURN_PHASE_WAIT_TTS ||
                   next == NUWAX_TURN_PHASE_PLAYING;
        case NUWAX_TURN_PHASE_WAIT_TTS:
            return next == NUWAX_TURN_PHASE_WAIT_TTS ||
                   next == NUWAX_TURN_PHASE_PLAYING;
        case NUWAX_TURN_PHASE_WAIT_TTS_RETRY:
            return next == NUWAX_TURN_PHASE_WAIT_TTS_RETRY ||
                   next == NUWAX_TURN_PHASE_PLAYING;
        case NUWAX_TURN_PHASE_PLAYING:
            return next == NUWAX_TURN_PHASE_PLAYING ||
                   next == NUWAX_TURN_PHASE_DRAINING;
        case NUWAX_TURN_PHASE_DRAINING:
            return next == NUWAX_TURN_PHASE_DRAINING;
        case NUWAX_TURN_PHASE_INACTIVE:
        default:
            return false;
    }
}

bool nuwax_turn_guard_transition(nuwax_turn_guard_t *guard,
                                 nuwax_turn_phase_t expected_phase,
                                 nuwax_turn_phase_t next_phase,
                                 uint32_t now_ms,
                                 uint32_t timeout_ms) {
    if (!guard || !guard->active || guard->phase != expected_phase ||
        next_phase == NUWAX_TURN_PHASE_INACTIVE || timeout_ms == 0U ||
        !phase_transition_allowed(guard->phase, next_phase)) {
        return false;
    }
    if (guard->phase != next_phase) {
        guard->phase_started_ms = now_ms;
    }
    guard->phase = next_phase;
    guard->deadline_ms = now_ms + timeout_ms;
    return true;
}

bool nuwax_turn_guard_phase_is(const nuwax_turn_guard_t *guard,
                               nuwax_turn_phase_t phase) {
    return guard && guard->active && guard->phase == phase;
}

bool nuwax_turn_guard_deadline_expired(const nuwax_turn_guard_t *guard,
                                       uint32_t now_ms) {
    return guard && guard->active && guard->deadline_ms != 0U &&
           (int32_t)(now_ms - guard->deadline_ms) >= 0;
}

bool nuwax_turn_guard_phase_runtime_expired(
    const nuwax_turn_guard_t *guard,
    nuwax_turn_phase_t phase,
    uint32_t now_ms,
    uint32_t max_runtime_ms) {
    return guard && guard->active && guard->phase == phase &&
           max_runtime_ms != 0U &&
           (uint32_t)(now_ms - guard->phase_started_ms) >= max_runtime_ms;
}

const char *nuwax_turn_phase_name(nuwax_turn_phase_t phase) {
    switch (phase) {
        case NUWAX_TURN_PHASE_WAIT_TEXT:
            return "wait_text";
        case NUWAX_TURN_PHASE_WAIT_TTS:
            return "wait_tts";
        case NUWAX_TURN_PHASE_WAIT_TTS_RETRY:
            return "wait_tts_retry";
        case NUWAX_TURN_PHASE_PLAYING:
            return "playing";
        case NUWAX_TURN_PHASE_DRAINING:
            return "draining";
        case NUWAX_TURN_PHASE_INACTIVE:
        default:
            return "inactive";
    }
}
