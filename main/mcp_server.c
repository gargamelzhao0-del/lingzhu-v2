#include "mcp_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "json_resource_budget.h"
#include "esp_log.h"

static const char *TAG = "nuwax_mcp";

static const cJSON *get_unique_object_item(const cJSON *root,
                                           const char *name) {
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

static bool trailing_bytes_are_whitespace(const char *cursor,
                                          const char *end) {
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

static esp_err_t send_json(nuwax_ws_client_t *ws, cJSON *root) {
    char *json = cJSON_PrintUnformatted(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = nuwax_ws_send_text(ws, json);
    free(json);
    return err;
}

static void add_id(cJSON *target, cJSON *id) {
    if (cJSON_IsNumber(id)) {
        cJSON_AddNumberToObject(target, "id", id->valuedouble);
    } else if (cJSON_IsString(id)) {
        cJSON_AddStringToObject(target, "id", id->valuestring);
    } else {
        cJSON_AddNullToObject(target, "id");
    }
}

static cJSON *create_response(cJSON *id) {
    cJSON *root = cJSON_CreateObject();
    cJSON *payload = cJSON_CreateObject();
    if (!root || !payload) {
        cJSON_Delete(root);
        cJSON_Delete(payload);
        return NULL;
    }

    cJSON_AddStringToObject(root, "type", "mcp");
    cJSON_AddStringToObject(payload, "jsonrpc", "2.0");
    add_id(payload, id);
    cJSON_AddItemToObject(root, "payload", payload);
    return root;
}

static esp_err_t send_error(nuwax_ws_client_t *ws, cJSON *id, int code, const char *message) {
    cJSON *root = create_response(id);
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON *payload = cJSON_GetObjectItem(root, "payload");
    cJSON *error = cJSON_CreateObject();
    cJSON_AddNumberToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message ? message : "MCP error");
    cJSON_AddItemToObject(payload, "error", error);
    esp_err_t err = send_json(ws, root);
    cJSON_Delete(root);
    return err;
}

static cJSON *create_text_content(const char *text, bool is_error) {
    cJSON *result = cJSON_CreateObject();
    cJSON *content = cJSON_CreateArray();
    cJSON *item = cJSON_CreateObject();
    if (!result || !content || !item) {
        cJSON_Delete(result);
        cJSON_Delete(content);
        cJSON_Delete(item);
        return NULL;
    }
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", text ? text : "");
    cJSON_AddItemToArray(content, item);
    cJSON_AddItemToObject(result, "content", content);
    cJSON_AddBoolToObject(result, "isError", is_error);
    return result;
}

static void add_tool(cJSON *tools, const char *name, const char *description, cJSON *properties, cJSON *required) {
    cJSON *tool = cJSON_CreateObject();
    cJSON *schema = cJSON_CreateObject();
    if (!tool || !schema) {
        cJSON_Delete(tool);
        cJSON_Delete(schema);
        return;
    }
    cJSON_AddStringToObject(tool, "name", name);
    cJSON_AddStringToObject(tool, "description", description);
    cJSON_AddStringToObject(schema, "type", "object");
    cJSON_AddItemToObject(schema, "properties", properties ? properties : cJSON_CreateObject());
    cJSON_AddItemToObject(schema, "required", required ? required : cJSON_CreateArray());
    cJSON_AddItemToObject(tool, "inputSchema", schema);
    cJSON_AddItemToArray(tools, tool);
}

static cJSON *string_property(const char *description) {
    cJSON *property = cJSON_CreateObject();
    cJSON_AddStringToObject(property, "type", "string");
    cJSON_AddStringToObject(property, "description", description);
    return property;
}

static cJSON *integer_property(const char *description, int minimum, int maximum) {
    cJSON *property = cJSON_CreateObject();
    cJSON_AddStringToObject(property, "type", "integer");
    cJSON_AddStringToObject(property, "description", description);
    cJSON_AddNumberToObject(property, "minimum", minimum);
    cJSON_AddNumberToObject(property, "maximum", maximum);
    return property;
}

static cJSON *build_tools_list(const nuwax_mcp_device_status_t *status,
                               const nuwax_mcp_callbacks_t *callbacks) {
    cJSON *result = cJSON_CreateObject();
    cJSON *tools = cJSON_CreateArray();
    if (!result || !tools) {
        cJSON_Delete(result);
        cJSON_Delete(tools);
        return NULL;
    }

    const uint32_t available =
        nuwax_mcp_available_tool_mask(status, callbacks);
    if ((available & NUWAX_MCP_AVAILABLE_DEVICE_STATUS) != 0U) {
        add_tool(tools,
                 "self.get_device_status",
                 "Return current ESP32 device status, Wi-Fi status and heap memory.",
                 NULL,
                 NULL);
    }

    if ((available & NUWAX_MCP_AVAILABLE_SET_EMOTION) != 0U) {
        cJSON *emotion_properties = cJSON_CreateObject();
        cJSON_AddItemToObject(
            emotion_properties,
            "emotion",
            string_property("Emotion name: normal, idle, smile, happy, surprised, sad, error, thinking or listening."));
        cJSON *emotion_required = cJSON_CreateArray();
        cJSON_AddItemToArray(emotion_required, cJSON_CreateString("emotion"));
        add_tool(tools,
                 "self.ui.set_emotion",
                 "Change the round screen face expression.",
                 emotion_properties,
                 emotion_required);
    }

    if ((available & NUWAX_MCP_AVAILABLE_SERVO_HEAD_SHAKE) != 0U) {
        cJSON *servo_properties = cJSON_CreateObject();
        cJSON_AddItemToObject(
            servo_properties,
            "times",
            integer_property("Shake count.", 1, 5));
        add_tool(tools,
                 "self.servo.head_shake",
                 "Shake the head by servo.",
                 servo_properties,
                 NULL);
    }

    cJSON_AddItemToObject(result, "tools", tools);
    cJSON_AddStringToObject(result, "nextCursor", "");
    return result;
}

static cJSON *handle_tool_call(const char *name,
                               cJSON *arguments,
                               const nuwax_mcp_device_status_t *status,
                               const nuwax_mcp_callbacks_t *callbacks) {
    const uint32_t available =
        nuwax_mcp_available_tool_mask(status, callbacks);

    if ((available & NUWAX_MCP_AVAILABLE_DEVICE_STATUS) != 0U &&
        strcmp(name, "self.get_device_status") == 0) {
        char text[192];
        snprintf(text,
                 sizeof(text),
                 "{\"state\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"free_heap\":%lu,\"min_free_heap\":%lu}",
                 status && status->state ? status->state : "",
                 status && status->ssid ? status->ssid : "",
                 status && status->ip ? status->ip : "",
                 status ? (unsigned long)status->free_heap : 0,
                 status ? (unsigned long)status->min_free_heap : 0);
        return create_text_content(text, false);
    }

    if ((available & NUWAX_MCP_AVAILABLE_SET_EMOTION) != 0U &&
        strcmp(name, "self.ui.set_emotion") == 0) {
        const cJSON *emotion = get_unique_object_item(arguments, "emotion");
        if (!cJSON_IsString(emotion) || emotion->valuestring[0] == '\0') {
            return create_text_content("missing emotion", true);
        }
        bool ok = callbacks && callbacks->set_emotion && callbacks->set_emotion(emotion->valuestring, callbacks->ctx);
        return create_text_content(ok ? "true" : "unsupported emotion", !ok);
    }

    if ((available & NUWAX_MCP_AVAILABLE_SERVO_HEAD_SHAKE) != 0U &&
        strcmp(name, "self.servo.head_shake") == 0) {
        const cJSON *times_value = get_unique_object_item(arguments, "times");
        if (times_value &&
            (!cJSON_IsNumber(times_value) ||
             times_value->valuedouble != (double)times_value->valueint ||
             times_value->valueint < 1 || times_value->valueint > 5)) {
            return create_text_content("times must be an integer from 1 to 5", true);
        }
        const int times = times_value ? times_value->valueint : 1;
        bool ok = callbacks && callbacks->servo_head_shake && callbacks->servo_head_shake(times, callbacks->ctx);
        return create_text_content(ok ? "true" : "servo not configured", !ok);
    }

    return create_text_content("unknown tool", true);
}

esp_err_t nuwax_mcp_handle_message(nuwax_ws_client_t *ws,
                                   const char *payload_json,
                                   const nuwax_mcp_device_status_t *status,
                                   const nuwax_mcp_callbacks_t *callbacks) {
    if (!ws || !payload_json) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t payload_len = strlen(payload_json);
    const char *parse_end = NULL;
    cJSON *request = nuwax_json_parse_with_length_opts(payload_json,
                                              payload_len,
                                              &parse_end,
                                              false);
    if (!request || !cJSON_IsObject(request) ||
        !trailing_bytes_are_whitespace(parse_end, payload_json + payload_len)) {
        cJSON_Delete(request);
        return send_error(ws, NULL, -32700, "Parse error");
    }

    const cJSON *jsonrpc = get_unique_object_item(request, "jsonrpc");
    cJSON *id = (cJSON *)get_unique_object_item(request, "id");
    const cJSON *method = get_unique_object_item(request, "method");
    const cJSON *params = get_unique_object_item(request, "params");
    if (!cJSON_IsString(jsonrpc) || !jsonrpc->valuestring ||
        strcmp(jsonrpc->valuestring, "2.0") != 0 ||
        (!cJSON_IsString(id) && !cJSON_IsNumber(id)) ||
        !cJSON_IsString(method) || !method->valuestring ||
        method->valuestring[0] == '\0' || (params && !cJSON_IsObject(params))) {
        esp_err_t err = send_error(ws, id, -32600, "Invalid request");
        cJSON_Delete(request);
        return err;
    }

    cJSON *root = create_response(id);
    if (!root) {
        cJSON_Delete(request);
        return ESP_ERR_NO_MEM;
    }
    cJSON *response_payload = cJSON_GetObjectItem(root, "payload");

    if (strcmp(method->valuestring, "initialize") == 0) {
        cJSON *result = cJSON_CreateObject();
        cJSON *capabilities = cJSON_CreateObject();
        cJSON *server_info = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "protocolVersion", "2024-11-05");
        cJSON_AddItemToObject(capabilities, "tools", cJSON_CreateObject());
        cJSON_AddItemToObject(result, "capabilities", capabilities);
        cJSON_AddStringToObject(server_info, "name", "Nuwax AI Partner ESP32S3");
        cJSON_AddStringToObject(server_info, "version", "7.0");
        cJSON_AddItemToObject(result, "serverInfo", server_info);
        cJSON_AddItemToObject(response_payload, "result", result);
    } else if (strcmp(method->valuestring, "tools/list") == 0) {
        cJSON *result = build_tools_list(status, callbacks);
        if (!result) {
            cJSON_Delete(root);
            cJSON_Delete(request);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToObject(response_payload, "result", result);
    } else if (strcmp(method->valuestring, "tools/call") == 0) {
        const cJSON *name = get_unique_object_item(params, "name");
        cJSON *arguments = (cJSON *)get_unique_object_item(params, "arguments");
        if (!cJSON_IsObject(params) || !cJSON_IsString(name) ||
            !name->valuestring || name->valuestring[0] == '\0' ||
            (arguments && !cJSON_IsObject(arguments))) {
            cJSON_Delete(root);
            esp_err_t err = send_error(ws, id, -32602, "Invalid tool parameters");
            cJSON_Delete(request);
            return err;
        }
        cJSON *result = handle_tool_call(name->valuestring,
                                         cJSON_IsObject(arguments) ? arguments : NULL,
                                         status,
                                         callbacks);
        if (!result) {
            cJSON_Delete(root);
            cJSON_Delete(request);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToObject(response_payload, "result", result);
    } else {
        cJSON_Delete(root);
        esp_err_t err = send_error(ws, id, -32601, "Method not found");
        cJSON_Delete(request);
        return err;
    }

    ESP_LOGI(TAG, "MCP handled method=%s", method->valuestring);
    esp_err_t err = send_json(ws, root);
    cJSON_Delete(root);
    cJSON_Delete(request);
    return err;
}
