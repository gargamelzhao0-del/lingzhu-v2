#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static inline bool sd_audio_stage_is_integrity_fault(const char *stage)
{
    return stage && (!strcmp(stage, "record_readback_crc") ||
        !strcmp(stage, "seal_prefix_ram") || !strcmp(stage, "seal_tail_ram") ||
        !strcmp(stage, "frame_queue_crc") || !strcmp(stage, "frame_write_crc") ||
        !strcmp(stage, "seal_readback_crc") || !strcmp(stage, "seal_readback_size"));
}

/* Scoped by the storage owner to one card/binding/capture. Repeated reads of
 * the same file are one loss. Only a later original-CRC full verification can
 * clear the window; old/historical successes cannot hide a newer failure. */
typedef struct { bool armed; uint32_t index; } sd_audio_damage_window_t;
static inline bool sd_audio_damage_note(sd_audio_damage_window_t *window, uint32_t index)
{
    if (window->armed) return window->index != index;
    window->armed = true; window->index = index; return false;
}
static inline void sd_audio_damage_healthy(sd_audio_damage_window_t *window, uint32_t index)
{
    if (window->armed && index > window->index) window->armed = false;
}

/* Only the replay owner touches this witness. One failed lookup/read is not
 * proof of a permanently absent file: require the same file to fail again
 * after an ordered media recovery. No ACK or deletion is implied. */
typedef struct {
    char session_id[48];
    uint32_t index, media_generation;
    bool armed;
} sd_audio_file_fault_t;

static inline bool sd_audio_file_fault_confirm(sd_audio_file_fault_t *fault,
    const char *session_id, uint32_t index, uint32_t media_generation)
{
    if (fault->armed && fault->index == index &&
        !strcmp(fault->session_id, session_id) &&
        fault->media_generation != media_generation) return true;
    size_t bytes = 0U;
    while (bytes + 1U < sizeof(fault->session_id) && session_id[bytes]) ++bytes;
    memcpy(fault->session_id, session_id, bytes);
    fault->session_id[bytes] = 0;
    fault->index = index;
    fault->media_generation = media_generation;
    fault->armed = true;
    return false;
}
