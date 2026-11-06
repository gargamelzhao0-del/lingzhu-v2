#pragma once

#include "conversation_visual_round.h"
#include "turn_guard.h"

/* A heartbeat cannot resolve a greeting which never acquired a playback
 * generation. Retire only this bound transaction; never apply this rule to
 * normal turns, physical playback, or a newer awake/connection owner. */
static inline bool nuwax_wake_greeting_needs_epoch_recovery(
    const nuwax_conversation_visual_round_t *visual,
    const nuwax_turn_guard_t *guard,
    uint32_t ws_epoch,
    uint32_t awake_generation)
{
    return visual && guard && guard->active && guard->epoch != 0U &&
           guard->speaker_generation == 0U && !visual->first_pcm_seen &&
           visual->speaker_generation == 0U && ws_epoch != 0U &&
           awake_generation != 0U &&
           visual->kind == NUWAX_VISUAL_ROUND_WAKE_GREETING &&
           visual->ws_epoch == ws_epoch &&
           visual->awake_generation == awake_generation &&
           guard->awake_generation == awake_generation &&
           guard->request_id[0] != '\0' &&
           strcmp(visual->request_id, guard->request_id) == 0 &&
           (guard->phase == NUWAX_TURN_PHASE_WAIT_TEXT ||
            guard->phase == NUWAX_TURN_PHASE_WAIT_TTS ||
            guard->phase == NUWAX_TURN_PHASE_WAIT_TTS_RETRY ||
            guard->phase == NUWAX_TURN_PHASE_PLAYING);
}
