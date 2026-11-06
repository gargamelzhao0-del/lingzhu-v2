#pragma once
#include <stdbool.h>
#include <stddef.h>

/* One identity fits the existing 36-byte Binary field and 48-byte archive
 * field. Never truncate an archive identity. Temporary legacy control-ID
 * translation is isolated in sd_audio_control_compat.h. */
static inline bool sd_audio_identity_valid(const char *id)
{
    if (!id || !id[0]) return false;
    for (size_t i = 0; i <= 35U; ++i) {
        const char c = id[i];
        if (!c) return true;
        if (i == 35U || !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return false;
}
