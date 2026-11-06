#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Owned by the mode controller. Survives transport cleanup, not reboot or
 * an explicit mode stop. The SD capture cursor spans reconnects; wire sequence
 * belongs to each listen transport and is never stored in this identity. */
typedef struct {
    char session_id[48];
    bool resuming;
    uint32_t next_capture_sequence;
} nuwax_audio_mode_identity_t;

static inline void nuwax_audio_mode_identity_clear(nuwax_audio_mode_identity_t *id)
{
    memset(id, 0, sizeof(*id));
}

static inline void nuwax_audio_mode_identity_remember(
    nuwax_audio_mode_identity_t *id, const char *session)
{
    if (session && session[0] && !id->session_id[0] &&
        strlen(session) < sizeof(id->session_id)) {
        memcpy(id->session_id, session, strlen(session) + 1U);
    }
}
