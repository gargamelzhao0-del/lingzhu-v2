#include "monitoring_protocol.h"
#include "sd_audio_identity.h"
#include "audio_command_deadline.h"
#include "esp_timer.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "json_resource_budget.h"

static bool json_string_equals(const cJSON *item, const char *value) {
    return item && cJSON_IsString(item) && item->valuestring && value &&
           strcmp(item->valuestring, value) == 0;
}

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
    if (!dst || dst_size == 0) {
        return false;
    }
    dst[0] = '\0';
    if (!item || !cJSON_IsString(item) || !item->valuestring) {
        return false;
    }

    const size_t length = strlen(item->valuestring);
    if (length == 0 || length >= dst_size) {
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

static bool copy_optional_text(char *dst,
                               size_t dst_size,
                               const cJSON *item) {
    if (!dst || dst_size == 0U) {
        return false;
    }
    dst[0] = '\0';
    if (!item) {
        return true;
    }
    if (!cJSON_IsString(item) || !item->valuestring) {
        return false;
    }
    const size_t length = strlen(item->valuestring);
    if (length >= dst_size) {
        return false;
    }
    memcpy(dst, item->valuestring, length + 1U);
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

static esp_err_t send_json_object(nuwax_ws_client_t *ws, cJSON *root) {
    if (!ws || !root) {
        return ESP_ERR_INVALID_ARG;
    }
    char *json = cJSON_PrintUnformatted(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t err = nuwax_ws_send_text_until_if_epoch(ws, json,
        esp_timer_get_time() + NUWAX_AUDIO_COMMAND_SEND_SLICE_US, nuwax_ws_connection_epoch(ws));
    cJSON_free(json);
    return err;
}

static esp_err_t send_mode_request(nuwax_ws_client_t *ws,
                                                  const char *request_id,
                                                  bool enabled,
                                                  const char *reason,
                                                  const char *audio_session_id) {
    if (!ws || !request_id) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON request_id_item = {0};
    request_id_item.type = cJSON_String;
    request_id_item.valuestring = (char *)request_id;
    char validated_request_id[NUWAX_MONITORING_REQUEST_ID_BYTES];
    if (!copy_request_id(validated_request_id,
                         sizeof(validated_request_id),
                         &request_id_item)) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    bool created = cJSON_AddStringToObject(root, "type", "audio_mode_request") &&
                   cJSON_AddStringToObject(root, "request_id",
                                           validated_request_id) &&
                   cJSON_AddStringToObject(root, "mode", "monitoring") &&
                   cJSON_AddStringToObject(root, "state",
                                           enabled ? "enabled" : "disabled");
    if (created && reason && reason[0] != '\0') {
        created = cJSON_AddStringToObject(root, "reason", reason);
    }
    if (created && audio_session_id && audio_session_id[0]) {
        created = cJSON_AddStringToObject(root, "audio_session_id", audio_session_id);
    }
    const esp_err_t err = created ? send_json_object(ws, root) : ESP_ERR_NO_MEM;
    cJSON_Delete(root);
    return err;
}

esp_err_t nuwax_monitoring_protocol_send_request(nuwax_ws_client_t *ws,
    const char *request_id, bool enabled, const char *reason)
{
    return send_mode_request(ws, request_id, enabled, reason, NULL);
}

esp_err_t nuwax_monitoring_protocol_send_resume_request(nuwax_ws_client_t *ws,
    const char *request_id, const char *audio_session_id)
{
    return send_mode_request(ws, request_id, true, "offline_audio_recovered", audio_session_id);
}

bool nuwax_monitoring_protocol_parse(const char *payload,
                                     int len,
                                     nuwax_monitoring_protocol_event_t *event) {
    if (event) {
        memset(event, 0, sizeof(*event));
    }
    if (!payload || len <= 0 || !event) {
        return false;
    }

    const char *parse_end = NULL;
    cJSON *root = nuwax_json_parse_with_length_opts(payload,
                                           (size_t)len,
                                           &parse_end,
                                           false);
    if (!root || !cJSON_IsObject(root) ||
        !trailing_bytes_are_whitespace(parse_end, payload + len)) {
        cJSON_Delete(root);
        return false;
    }

    const cJSON *type = get_unique_object_item(root, "type");
    const cJSON *request_id = get_unique_object_item(root, "request_id");
    const cJSON *mode = get_unique_object_item(root, "mode");
    const cJSON *state = get_unique_object_item(root, "state");

    bool recognized = false;
    if (json_string_equals(type, "audio_mode_request_ack")) {
        const cJSON *ok = get_unique_object_item(root, "ok");
        recognized = json_string_equals(mode, "monitoring") && cJSON_IsBool(ok) &&
            (json_string_equals(state, "enabled") || json_string_equals(state, "disabled")) &&
            copy_request_id(event->request_id, sizeof(event->request_id), request_id) &&
            copy_optional_text(event->audio_session_id, sizeof(event->audio_session_id),
                               get_unique_object_item(root, "audio_session_id")) &&
            copy_optional_text(event->reason, sizeof(event->reason),
                               get_unique_object_item(root, "reason"));
        if (recognized) {
            event->type = NUWAX_MONITORING_PROTOCOL_EVENT_REQUEST_ACK;
            event->ok = cJSON_IsTrue(ok);
            event->enabled = json_string_equals(state, "enabled");
            memcpy(event->mode, "monitoring", sizeof("monitoring"));
            strcpy(event->state, event->enabled ? "enabled" : "disabled");
        }
    } else if (json_string_equals(type, "monitoring_capture_control_ack")) {
        const bool pause = json_string_equals(state, "pause");
        const bool resume = json_string_equals(state, "resume");
        const cJSON *ok = get_unique_object_item(root, "ok");
        const cJSON *audio_session_id =
            get_unique_object_item(root, "audio_session_id");
        recognized = json_string_equals(mode, "monitoring") &&
                     (pause || resume) && cJSON_IsBool(ok) &&
                     copy_request_id(event->request_id,
                                     sizeof(event->request_id),
                                     request_id) &&
                     copy_request_id(event->audio_session_id,
                                     sizeof(event->audio_session_id),
                                     audio_session_id) &&
                     copy_optional_text(event->reason,
                                        sizeof(event->reason),
                                        get_unique_object_item(root, "reason"));
        if (recognized) {
            event->type =
                NUWAX_MONITORING_PROTOCOL_EVENT_CAPTURE_CONTROL_ACK;
            memcpy(event->mode, "monitoring", sizeof("monitoring"));
            if (pause) {
                memcpy(event->state, "pause", sizeof("pause"));
            } else {
                memcpy(event->state, "resume", sizeof("resume"));
            }
            event->ok = cJSON_IsTrue(ok);
        }
    } else {
        const cJSON *command = get_unique_object_item(root, "command");
        const bool enabled = json_string_equals(state, "enabled");
        const bool disabled = json_string_equals(state, "disabled");
        recognized = json_string_equals(type, "device_command") &&
                     json_string_equals(command, "set_audio_mode") &&
                     json_string_equals(mode, "monitoring") &&
                     (enabled || disabled) &&
                     copy_request_id(event->request_id,
                                     sizeof(event->request_id),
                                     request_id) &&
                      copy_optional_text(event->audio_session_id, sizeof(event->audio_session_id),
                                         get_unique_object_item(root, "audio_session_id"));
        if (recognized) {
        event->type = NUWAX_MONITORING_PROTOCOL_EVENT_SET_COMMAND;
        memcpy(event->mode, "monitoring", sizeof("monitoring"));
        if (enabled) {
            memcpy(event->state, "enabled", sizeof("enabled"));
        } else {
            memcpy(event->state, "disabled", sizeof("disabled"));
        }
        event->enabled = enabled;
        }
    }
    if (!recognized) {
        memset(event, 0, sizeof(*event));
    }

    cJSON_Delete(root);
    return recognized;
}

esp_err_t nuwax_monitoring_protocol_send_capture_control(
    nuwax_ws_client_t *ws,
    const char *request_id,
    bool pause,
    const char *audio_session_id,
    const char *reason,
    uint32_t silence_ms) {
    if (!ws || !request_id || !audio_session_id) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON request_id_item = {0};
    request_id_item.type = cJSON_String;
    request_id_item.valuestring = (char *)request_id;
    char validated_request_id[NUWAX_MONITORING_REQUEST_ID_BYTES];
    if (!copy_request_id(validated_request_id,
                         sizeof(validated_request_id),
                         &request_id_item)) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON audio_session_id_item = {0};
    audio_session_id_item.type = cJSON_String;
    audio_session_id_item.valuestring = (char *)audio_session_id;
    char validated_audio_session_id[NUWAX_MONITORING_AUDIO_SESSION_ID_BYTES];
    if (!copy_request_id(validated_audio_session_id,
                         sizeof(validated_audio_session_id),
                         &audio_session_id_item)) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    const bool created =
        cJSON_AddStringToObject(root, "type", "monitoring_capture_control") &&
        cJSON_AddStringToObject(root, "request_id", validated_request_id) &&
        cJSON_AddStringToObject(root, "mode", "monitoring") &&
        cJSON_AddStringToObject(root, "state", pause ? "pause" : "resume") &&
        cJSON_AddStringToObject(root,
                               "audio_session_id",
                               validated_audio_session_id) &&
        cJSON_AddStringToObject(root, "reason", reason ? reason : "");
    if (!created ||
        (pause && !cJSON_AddNumberToObject(root, "silence_ms", silence_ms))) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t err = send_json_object(ws, root);
    cJSON_Delete(root);
    return err;
}

esp_err_t nuwax_monitoring_protocol_send_command_ack(nuwax_ws_client_t *ws,
                                                      const char *request_id,
                                                      bool enabled,
                                                      bool ok,
                                                      const char *message) {
    return nuwax_monitoring_protocol_send_command_ack_with_session(
        ws, request_id, enabled, ok, message, NULL);
}

esp_err_t nuwax_monitoring_protocol_send_command_ack_with_session(nuwax_ws_client_t *ws,
    const char *request_id, bool enabled, bool ok, const char *message,
    const char *audio_session_id) {
    return nuwax_monitoring_protocol_send_command_ack_until(ws, request_id, enabled, ok,
        message, audio_session_id, 0, 0);
}

esp_err_t nuwax_monitoring_protocol_send_command_ack_until(nuwax_ws_client_t *ws,
    const char *request_id, bool enabled, bool ok, const char *message,
    const char *audio_session_id, int64_t deadline_us, uint32_t expected_epoch) {
    if (audio_session_id && !sd_audio_identity_valid(audio_session_id)) return ESP_ERR_INVALID_ARG;
    if (!ws || !request_id) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON request_id_item = {0};
    request_id_item.type = cJSON_String;
    request_id_item.valuestring = (char *)request_id;
    char validated_request_id[NUWAX_MONITORING_REQUEST_ID_BYTES];
    if (!copy_request_id(validated_request_id,
                         sizeof(validated_request_id),
                         &request_id_item)) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    const bool created = cJSON_AddStringToObject(root, "type", "device_command_ack") &&
                         cJSON_AddStringToObject(root,
                                                "request_id",
                                                validated_request_id) &&
                         cJSON_AddStringToObject(root, "command", "set_audio_mode") &&
                         cJSON_AddBoolToObject(root, "ok", ok) &&
                         cJSON_AddStringToObject(root, "mode", "monitoring") &&
                         cJSON_AddStringToObject(root,
                                                "state",
                                                enabled ? "enabled" : "disabled") &&
                         cJSON_AddStringToObject(root,
                                                "message",
                                                message ? message : "") &&
                         (!audio_session_id || cJSON_AddStringToObject(root, "audio_session_id", audio_session_id));
    if (!created) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err;
    if (deadline_us > 0) {
        char *json = cJSON_PrintUnformatted(root);
        err = json ? nuwax_ws_send_text_until_if_epoch(ws, json, deadline_us, expected_epoch)
                   : ESP_ERR_NO_MEM;
        cJSON_free(json);
    } else {
        err = send_json_object(ws, root);
    }
    cJSON_Delete(root);
    return err;
}
