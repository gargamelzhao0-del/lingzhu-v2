#pragma once
#include "sd_audio_protocol.h"
#include "sd_audio_time.h"
#include "cJSON.h"

static inline bool sd_audio_add_session_time(cJSON *root, const sd_audio_session_time_t *time)
{
    cJSON *object = cJSON_AddObjectToObject(root, "session_time");
    if (!object) return false;
    return ((time && (time->valid & SD_AUDIO_TIME_STARTED))
        ? cJSON_AddNumberToObject(object, "started_at_ms", (double)time->started_at_ms)
        : cJSON_AddNullToObject(object, "started_at_ms")) &&
        ((time && (time->valid & SD_AUDIO_TIME_DURATION))
        ? cJSON_AddNumberToObject(object, "duration_ms", (double)time->duration_ms)
        : cJSON_AddNullToObject(object, "duration_ms"));
}

/* A partial control object is not a valid request. In particular, never turn
 * transient allocation pressure into the peer's permanent invalid_argument. */
static inline cJSON *sd_audio_commit_json(unsigned route, const char *request,
    const char *segment, const char *session, uint32_t index, uint32_t bytes,
    uint32_t crc, bool compact)
{
    cJSON *json = cJSON_CreateObject();
    if (!json) return NULL;
    bool built = cJSON_AddStringToObject(json, "type", sd_audio_type(route, SD_COMMIT)) &&
        cJSON_AddStringToObject(json, "request_id", request);
    if (route == SD_UPLOAD_LEGACY) {
        built = built && cJSON_AddStringToObject(json, "mode", "archive_only") &&
            cJSON_AddStringToObject(json, "segment_id", segment);
    } else {
        built = built && cJSON_AddStringToObject(json, "audio_session_id", session) &&
            cJSON_AddNumberToObject(json, "segment_index", index);
    }
    if (!compact) built = built && cJSON_AddNumberToObject(json, "total_bytes", bytes) &&
        cJSON_AddNumberToObject(json, "crc32", crc);
    if (!built) { cJSON_Delete(json); return NULL; }
    return json;
}

static inline cJSON *sd_audio_end_json(bool monitoring, const char *request,
    const char *session, uint32_t count, bool complete, const sd_audio_session_time_t *time)
{
    cJSON *json = cJSON_CreateObject();
    if (!json) return NULL;
    const bool built = cJSON_AddStringToObject(json, "type", monitoring ?
            "monitoring_audio_session_end" : "meeting_audio_session_end") &&
        cJSON_AddStringToObject(json, "request_id", request) &&
        cJSON_AddStringToObject(json, "audio_session_id", session) &&
        cJSON_AddNumberToObject(json, "segment_count", count) &&
        cJSON_AddBoolToObject(json, "recording_complete", complete) &&
        sd_audio_add_session_time(json, time);
    if (!built) { cJSON_Delete(json); return NULL; }
    return json;
}

static inline bool sd_scene_number(const cJSON *item, uint32_t max, uint32_t *out)
{
    if (!cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble > max ||
        item->valuedouble != (double)(uint32_t)item->valuedouble) return false;
    *out = (uint32_t)item->valuedouble;
    return true;
}
/* Validate the entire reply before publishing any offset to the sender. */
static inline bool sd_scene_parse_resume(const cJSON *root, sd_scene_ack_t *ack)
{
    const cJSON *array = cJSON_GetObjectItemCaseSensitive(root, "resume");
    if (!ack->count) return !array || (cJSON_IsArray(array) && !cJSON_GetArraySize(array));
    if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) != (int)ack->count) return false;
    uint32_t offsets[SD_SCENE_RESUME_MAX];
    for (unsigned i = 0; i < ack->count; ++i) {
        const cJSON *item = cJSON_GetArrayItem(array, (int)i);
        uint32_t index;
        if (!sd_scene_number(cJSON_GetObjectItemCaseSensitive(item, "segment_index"), 999999, &index) ||
            index != ack->items[i].index ||
            !sd_scene_number(cJSON_GetObjectItemCaseSensitive(item, "next_offset"), ack->items[i].total, &offsets[i]))
            return false;
    }
    for (unsigned i = 0; i < ack->count; ++i) ack->items[i].offset = offsets[i];
    return true;
}
static inline unsigned sd_scene_error(const cJSON *root)
{
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "error_code");
    if (!cJSON_IsString(code)) return SD_SCENE_RETRY;
    if (!strcmp(code->valuestring, "invalid_argument") || !strcmp(code->valuestring, "conflict")) return SD_SCENE_PERMANENT;
    return !strcmp(code->valuestring, "crc_mismatch") ? SD_SCENE_CRC : SD_SCENE_RETRY;
}
static inline void sd_scene_packet(uint8_t *header, const char *session, uint32_t index,
                                    uint32_t offset, uint32_t length, bool final)
{
    memset(header, 0, 64);
    memcpy(header, "NWXCH1", 6);
    uint32_t value = 3;
    memcpy(header + 8, &value, 4); memcpy(header + 12, &offset, 4); memcpy(header + 16, &length, 4);
    memcpy(header + 20, session, strlen(session)); memcpy(header + 56, &index, 4);
    header[60] = final ? 1 : 0;
}
