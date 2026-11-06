#pragma once
#include <stdbool.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include "sd_audio_identity.h"

#define SD_SCENE_RESUME_MAX 16U
enum { SD_SCENE_RETRY, SD_SCENE_PERMANENT, SD_SCENE_CRC };
typedef struct { uint32_t index, total, crc, offset; } sd_scene_resume_t;
typedef struct {
    unsigned count, error;
    sd_scene_resume_t items[SD_SCENE_RESUME_MAX];
} sd_scene_ack_t;

/* Local/logging key only. On the wire the session and index are separate. */
static inline bool sd_audio_file_key(char key[48], const char *session, uint32_t index)
{
    if (!sd_audio_identity_valid(session) || index >= 1000000U) return false;
    return snprintf(key, 48U, "sd_%s_%08lx", session, (unsigned long)index) < 48;
}
enum { SD_UPLOAD_LEGACY, SD_UPLOAD_MEETING, SD_UPLOAD_LIVE, SD_UPLOAD_HISTORY };
enum { SD_BEGIN, SD_BEGIN_ACK, SD_CHUNK_ACK, SD_COMMIT, SD_COMMIT_ACK };
static inline const char *sd_audio_type(unsigned route, unsigned step)
{
    static const char *const types[4][5] = {
        {"offline_audio_begin", "offline_audio_begin_ack", "offline_audio_chunk_ack", "offline_audio_commit", "offline_audio_commit_ack"},
        {"meeting_audio_begin", "meeting_audio_begin_ack", "meeting_audio_chunk_ack", "meeting_audio_commit", "meeting_audio_commit_ack"},
        {"monitoring_audio_begin", "monitoring_audio_begin_ack", "monitoring_audio_realtime_chunk_ack", "monitoring_audio_realtime_commit", "monitoring_audio_realtime_commit_ack"},
        {"monitoring_audio_begin", "monitoring_audio_begin_ack", "monitoring_audio_history_chunk_ack", "monitoring_audio_history_commit", "monitoring_audio_history_commit_ack"}
    };
    return types[route < 4U ? route : 0U][step < 5U ? step : 0U];
}
static inline bool sd_audio_type_is(const char *type, unsigned step)
{
    for (unsigned route = 0; route < 4U; ++route)
        if (!strcmp(type, sd_audio_type(route, step))) return true;
    return false;
}
static inline bool sd_audio_end_ack(const char *type)
{
    return !strcmp(type, "meeting_audio_session_end_ack") ||
           !strcmp(type, "monitoring_audio_session_end_ack");
}
