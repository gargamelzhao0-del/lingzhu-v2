#pragma once

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_random.h"
#include "sd_audio_identity.h"
#include "sd_audio_control_sync.h"

/* Control requests assign device state. Optional control IDs are metadata;
 * archive identity constraints still apply to the actual recording. */
static inline const char *nuwax_sd_control_enable_rejection(
    bool existing_scene, const char *local_id, const nuwax_sd_control_sync_t *sync,
    const char *requested)
{
    (void)existing_scene; (void)local_id; (void)sync; (void)requested;
    return NULL;
}

static inline void nuwax_sd_control_generate_id(char local_id[48], const char *prefix)
{
    const unsigned long high = (unsigned long)esp_random();
    const unsigned long low = (unsigned long)esp_random();
    snprintf(local_id, 48, "%s_%08lx%08lx", prefix, high, low);
}

/* Owner task, once at SD admission and before any catalog check/handoff.
 * The parser has already bounded control IDs to 47 bytes. Do not call for
 * active recordings or no-card streams. Repeated calls retain the same ID. */
static inline bool nuwax_sd_control_prepare_legacy_id(
    nuwax_sd_control_sync_t *sync, char local_id[48], const char *prefix)
{
    if (sd_audio_identity_valid(local_id)) return false;
    if (local_id[0] && !sync->cloud_session_id[0])
        memcpy(sync->cloud_session_id, local_id, strlen(local_id) + 1U);
    nuwax_sd_control_generate_id(local_id, prefix);
    return true;
}

static inline bool nuwax_sd_control_rotate_used_id(nuwax_sd_control_sync_t *sync,
    char local_id[48], const char *prefix)
{
    if (sync->identity_attempts >= 4U) return false;
    ++sync->identity_attempts;
    sync->identity_ready = false;
    nuwax_sd_control_generate_id(local_id, prefix);
    return true;
}
