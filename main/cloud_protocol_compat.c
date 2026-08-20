#include "cloud_protocol_compat.h"

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "cJSON.h"

static bool json_object_is_complete(const char *payload,
                                    int len,
                                    const char *parse_end)
{
    const char *payload_end = payload + len;
    while (parse_end != NULL && parse_end < payload_end &&
           isspace((unsigned char)*parse_end)) {
        parse_end++;
    }
    return parse_end == payload_end;
}

static bool optional_device_id_matches(const cJSON *item,
                                       const char *expected_device_id)
{
    if (item == NULL) {
        return true;
    }
    return cJSON_IsString(item) && item->valuestring != NULL &&
           expected_device_id != NULL && expected_device_id[0] != '\0' &&
           strcmp(item->valuestring, expected_device_id) == 0;
}

static unsigned object_key_count(const cJSON *object, const char *name)
{
    unsigned count = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, object) {
        if (item->string != NULL && strcmp(item->string, name) == 0) {
            count++;
        }
    }
    return count;
}

static bool bounded_nonempty_string(const cJSON *item, size_t capacity)
{
    if (!cJSON_IsString(item) || item->valuestring == NULL ||
        item->valuestring[0] == '\0') {
        return false;
    }
    return strlen(item->valuestring) < capacity;
}

static bool is_auth_rejection_value(const cJSON *item)
{
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return false;
    }
    return strcmp(item->valuestring, "DEVICE_AUTH_FAILED") == 0 ||
           strcmp(item->valuestring, "DEVICE_SECRET_INVALID") == 0 ||
           strcmp(item->valuestring, "AUTH_FAILED") == 0 ||
           strcmp(item->valuestring, "UNAUTHORIZED") == 0;
}

static bool is_provision_required_code(const cJSON *item)
{
    return cJSON_IsString(item) && item->valuestring != NULL &&
           strcmp(item->valuestring, "DEVICE_NOT_BOUND") == 0;
}

nuwax_cloud_message_kind_t nuwax_cloud_classify_message(
    const char *payload,
    int len,
    const char *expected_device_id)
{
    if (payload == NULL || len <= 0) {
        return NUWAX_CLOUD_MESSAGE_NONE;
    }

    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(payload, (size_t)len,
                                             &parse_end, false);
    if (root == NULL || !cJSON_IsObject(root) ||
        !json_object_is_complete(payload, len, parse_end)) {
        cJSON_Delete(root);
        return NUWAX_CLOUD_MESSAGE_NONE;
    }

    nuwax_cloud_message_kind_t kind = NUWAX_CLOUD_MESSAGE_NONE;
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const cJSON *command =
        cJSON_GetObjectItemCaseSensitive(root, "command");
    const bool unenveloped_wake_word =
        object_key_count(root, "type") == 0U &&
        object_key_count(root, "request_id") == 0U &&
        object_key_count(root, "requestId") == 0U &&
        object_key_count(root, "command") == 1U &&
        cJSON_IsString(command) && command->valuestring != NULL &&
        strcmp(command->valuestring, "set_wake_word") == 0;
    if (unenveloped_wake_word) {
        cJSON_Delete(root);
        return NUWAX_CLOUD_MESSAGE_DEVICE_COMMAND;
    }
    if (object_key_count(root, "type") != 1U ||
        !cJSON_IsString(type) || type->valuestring == NULL) {
        cJSON_Delete(root);
        return NUWAX_CLOUD_MESSAGE_NONE;
    }

    if (strcmp(type->valuestring, "hello") == 0) {
        const cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
        const cJSON *authenticated =
            cJSON_GetObjectItemCaseSensitive(root, "authenticated");
        const cJSON *device_id =
            cJSON_GetObjectItemCaseSensitive(root, "device_id");
        const bool unique_fields = object_key_count(root, "ok") == 1U &&
                                   object_key_count(root, "authenticated") <= 1U &&
                                   object_key_count(root, "device_id") <= 1U;
        const bool strict_identity_matches =
            authenticated != NULL && cJSON_IsTrue(authenticated) &&
            device_id != NULL &&
            optional_device_id_matches(device_id, expected_device_id);
        const bool legacy_identity_matches =
            authenticated == NULL &&
            optional_device_id_matches(device_id, expected_device_id);
        if (unique_fields && cJSON_IsTrue(ok) &&
            (strict_identity_matches || legacy_identity_matches)) {
            kind = NUWAX_CLOUD_MESSAGE_HELLO_OK;
        } else {
            kind = NUWAX_CLOUD_MESSAGE_HELLO_REJECTED;
        }
    } else if (strcmp(type->valuestring, "pong") == 0) {
        kind = NUWAX_CLOUD_MESSAGE_PONG;
    } else if (strcmp(type->valuestring, "error") == 0) {
        const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
        const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
        if (object_key_count(root, "code") <= 1U &&
            object_key_count(root, "error") <= 1U) {
            if (is_provision_required_code(code)) {
                kind = NUWAX_CLOUD_MESSAGE_PROVISION_REQUIRED;
            } else if (is_auth_rejection_value(code) ||
                       is_auth_rejection_value(error)) {
                kind = NUWAX_CLOUD_MESSAGE_AUTH_REJECTED;
            }
        }
    } else if (strcmp(type->valuestring, "device_command") == 0) {
        const cJSON *request_id =
            cJSON_GetObjectItemCaseSensitive(root, "request_id");
        if (object_key_count(root, "command") == 1U &&
            object_key_count(root, "request_id") == 1U &&
            bounded_nonempty_string(command, 32U) &&
            bounded_nonempty_string(request_id, 40U)) {
            kind = NUWAX_CLOUD_MESSAGE_DEVICE_COMMAND;
        }
    }

    cJSON_Delete(root);
    return kind;
}
