#include "meeting_protocol.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

static const cJSON *get_unique_object_item(const cJSON *root, const char *name) {
    const cJSON *match = NULL;
    for (const cJSON *item = root ? root->child : NULL; item; item = item->next) {
        if (!item->string || strcmp(item->string, name) != 0) {
            continue;
        }
        if (match) {
            return NULL;
        }
        match = item;
    }
    return match;
}

static bool is_request_id_char(char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
           (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.';
}

static bool copy_request_id(char *dst, size_t dst_size, const cJSON *item) {
    if (!dst || dst_size == 0U) {
        return false;
    }
    dst[0] = '\0';
    if (!item || !cJSON_IsString(item) || !item->valuestring) {
        return false;
    }
    const size_t length = strlen(item->valuestring);
    if (length == 0U || length >= dst_size) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        if (!is_request_id_char(item->valuestring[i])) {
            return false;
        }
    }
    memcpy(dst, item->valuestring, length + 1U);
    return true;
}

static bool copy_optional_json_string(char *dst,
                                      size_t dst_size,
                                      const cJSON *item) {
    if (!dst || dst_size == 0) {
        return false;
    }
    dst[0] = '\0';
    if (!item) {
        return true; /* Optional field omitted. */
    }
    if (!cJSON_IsString(item) || !item->valuestring) {
        return false;
    }
    const size_t length = strlen(item->valuestring);
    if (length >= dst_size) {
        /* IDs and enum strings are fixed-width protocol fields.  Reject an
         * overlong value instead of silently aliasing it to a truncated ID. */
        return false;
    }
    memcpy(dst, item->valuestring, length);
    dst[length] = '\0';
    return true;
}

static bool trailing_bytes_are_whitespace(const char *cursor, const char *end) {
    if (!cursor || !end || cursor > end) {
        return false;
    }
    while (cursor < end) {
        const char ch = *cursor++;
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
            return false;
        }
    }
    return true;
}

static bool json_string_equals(const cJSON *item, const char *value) {
    return item && cJSON_IsString(item) && item->valuestring && value &&
           strcmp(item->valuestring, value) == 0;
}

static esp_err_t send_json_object(nuwax_ws_client_t *ws, cJSON *root) {
    if (!ws || !root) {
        return ESP_ERR_INVALID_ARG;
    }
    char *json = cJSON_PrintUnformatted(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = nuwax_ws_send_text(ws, json);
    free(json);
    return err;
}

bool nuwax_meeting_protocol_parse(const char *payload,
                                  int len,
                                  nuwax_meeting_protocol_event_t *event) {
    if (event) {
        memset(event, 0, sizeof(*event));
    }
    if (!payload || len <= 0 || !event) {
        return false;
    }

    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(payload,
                                           (size_t)len,
                                           &parse_end,
                                           false);
    if (!root || !cJSON_IsObject(root) ||
        !trailing_bytes_are_whitespace(parse_end, payload + len)) {
        cJSON_Delete(root);
        return false;
    }
    const cJSON *type = get_unique_object_item(root, "type");
    const cJSON *mode = get_unique_object_item(root, "mode");
    const cJSON *request_id = get_unique_object_item(root, "request_id");
    const cJSON *state = get_unique_object_item(root, "state");
    bool recognized = false;

    if (json_string_equals(type, "audio_mode_request_ack")) {
        if (!json_string_equals(mode, "meeting")) {
            cJSON_Delete(root);
            return false;
        }
        event->type = NUWAX_MEETING_PROTOCOL_EVENT_REQUEST_ACK;
        const cJSON *ok = get_unique_object_item(root, "ok");
        const cJSON *current_turn_cancelled =
            get_unique_object_item(root, "current_turn_cancelled");
        const bool optional_bool_valid =
            !current_turn_cancelled || cJSON_IsBool(current_turn_cancelled);
        bool fields_valid = copy_request_id(event->request_id,
                                            sizeof(event->request_id),
                                            request_id) &&
                            copy_optional_json_string(event->mode,
                                                      sizeof(event->mode),
                                                      mode) &&
                            copy_optional_json_string(event->state,
                                                      sizeof(event->state),
                                                      state) &&
                            copy_optional_json_string(
                                event->audio_session_id,
                                sizeof(event->audio_session_id),
                                get_unique_object_item(root, "audio_session_id")) &&
                            copy_optional_json_string(
                                event->reason,
                                sizeof(event->reason),
                                get_unique_object_item(root, "reason"));
        event->ok = cJSON_IsTrue(ok);
        event->current_turn_cancelled = cJSON_IsTrue(current_turn_cancelled);
        recognized = fields_valid && cJSON_IsBool(ok) && optional_bool_valid &&
                    cJSON_IsString(state) &&
                    (strcmp(event->state, "enabled") == 0 ||
                     strcmp(event->state, "disabled") == 0);
    } else if (json_string_equals(type, "device_command") &&
               json_string_equals(get_unique_object_item(root, "command"),
                                  "set_audio_mode") &&
               json_string_equals(mode, "meeting")) {
        event->type = NUWAX_MEETING_PROTOCOL_EVENT_SET_COMMAND;
        event->ok = true;
        const cJSON *interrupt_current_turn =
            get_unique_object_item(root, "interrupt_current_turn");
        const bool optional_bool_valid =
            !interrupt_current_turn || cJSON_IsBool(interrupt_current_turn);
        bool fields_valid = copy_request_id(event->request_id,
                                            sizeof(event->request_id),
                                            request_id) &&
                            copy_optional_json_string(event->mode,
                                                      sizeof(event->mode),
                                                      mode) &&
                            copy_optional_json_string(event->state,
                                                      sizeof(event->state),
                                                      state) &&
                            copy_optional_json_string(
                                event->audio_session_id,
                                sizeof(event->audio_session_id),
                                get_unique_object_item(root, "audio_session_id"));
        event->interrupt_current_turn = cJSON_IsTrue(interrupt_current_turn);
        recognized = fields_valid && optional_bool_valid && cJSON_IsString(state) &&
                    (strcmp(event->state, "enabled") == 0 ||
                     strcmp(event->state, "disabled") == 0);
    }

    if (!recognized) {
        memset(event, 0, sizeof(*event));
    }
    cJSON_Delete(root);
    return recognized;
}

esp_err_t nuwax_meeting_protocol_send_request(nuwax_ws_client_t *ws,
                                              const char *request_id,
                                              bool enabled,
                                              bool interrupt_current_turn,
                                              const char *reason) {
    cJSON request_id_item = {0};
    request_id_item.type = cJSON_String;
    request_id_item.valuestring = (char *)request_id;
    char validated_request_id[NUWAX_MEETING_REQUEST_ID_BYTES];
    if (!ws || !copy_request_id(validated_request_id,
                                sizeof(validated_request_id),
                                &request_id_item)) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "type", "audio_mode_request");
    cJSON_AddStringToObject(root, "request_id", validated_request_id);
    cJSON_AddStringToObject(root, "mode", "meeting");
    cJSON_AddStringToObject(root, "state", enabled ? "enabled" : "disabled");
    if (interrupt_current_turn) {
        cJSON_AddTrueToObject(root, "interrupt_current_turn");
    }
    if (reason && reason[0] != '\0') {
        cJSON_AddStringToObject(root, "reason", reason);
    }
    esp_err_t err = send_json_object(ws, root);
    cJSON_Delete(root);
    return err;
}

esp_err_t nuwax_meeting_protocol_send_command_ack(nuwax_ws_client_t *ws,
                                                  const char *request_id,
                                                  bool enabled,
                                                  bool ok,
                                                  const char *message) {
    cJSON request_id_item = {0};
    request_id_item.type = cJSON_String;
    request_id_item.valuestring = (char *)request_id;
    char validated_request_id[NUWAX_MEETING_REQUEST_ID_BYTES];
    if (!ws || !copy_request_id(validated_request_id,
                                sizeof(validated_request_id),
                                &request_id_item)) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "type", "device_command_ack");
    cJSON_AddStringToObject(root, "request_id", validated_request_id);
    cJSON_AddStringToObject(root, "command", "set_audio_mode");
    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddStringToObject(root, "mode", "meeting");
    cJSON_AddStringToObject(root, "state", enabled ? "enabled" : "disabled");
    cJSON_AddStringToObject(root, "message", message ? message : "");
    esp_err_t err = send_json_object(ws, root);
    cJSON_Delete(root);
    return err;
}
