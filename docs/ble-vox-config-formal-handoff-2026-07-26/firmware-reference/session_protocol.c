#include "session_protocol.h"
#include "nuwax_provision_new/nuwax_device_uid.h"

#include <stdlib.h>
#include <string.h>

#include "board/audio_board.h"
#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "nuwax_session";

#ifndef CONFIG_NUWAX_WAKE_WORD_ENABLE
#define CONFIG_NUWAX_WAKE_WORD_ENABLE 0
#endif
#ifndef CONFIG_NUWAX_VOICE_LOCAL_VAD_ENDPOINT_ENABLE
#define CONFIG_NUWAX_VOICE_LOCAL_VAD_ENDPOINT_ENABLE 0
#endif
#ifndef CONFIG_NUWAX_LIVE_AEC_ENABLE
#define CONFIG_NUWAX_LIVE_AEC_ENABLE 0
#endif
#ifndef CONFIG_NUWAX_P3_SOFTWARE_REF_ENABLE
#define CONFIG_NUWAX_P3_SOFTWARE_REF_ENABLE 0
#endif
#ifndef CONFIG_NUWAX_VOCAT_SLOT2_AEC_REF_ENABLE
#define CONFIG_NUWAX_VOCAT_SLOT2_AEC_REF_ENABLE 0
#endif
#ifndef CONFIG_NUWAX_SPEAKER_ENABLE
#define CONFIG_NUWAX_SPEAKER_ENABLE 0
#endif

/* Values are part of the device/cloud hello contract. */
static const char *aec_reference_kind_str(bool hardware_ref_enabled, bool software_ref_enabled)
{
    if (hardware_ref_enabled && software_ref_enabled) {
        return "es7210_slot2_software_fallback";
    }
    if (hardware_ref_enabled) {
        return "es7210_slot2";
    }
    return software_ref_enabled ? "software" : "none";
}

static const char *afe_input_format_str(nuwax_afe_input_format_t fmt)
{
    switch (fmt) {
    case NUWAX_AFE_INPUT_MR:
        return "MR";
    case NUWAX_AFE_INPUT_MMR:
        return "MMR";
    case NUWAX_AFE_INPUT_MM:
    default:
        return "MM";
    }
}

static size_t utf8_safe_prefix_len(const char *text, size_t max_bytes) {
    if (!text || max_bytes == 0) {
        return 0;
    }

    size_t len = strnlen(text, max_bytes);
    if (len < max_bytes || text[len] == '\0') {
        return len;
    }

    size_t start = len;
    while (start > 0 && (((unsigned char)text[start - 1] & 0xc0) == 0x80)) {
        start--;
    }
    if (start == 0) {
        return 0;
    }

    unsigned char lead = (unsigned char)text[start - 1];
    size_t needed = 1;
    if ((lead & 0x80) == 0x00) {
        needed = 1;
    } else if ((lead & 0xE0) == 0xC0) {
        needed = 2;
    } else if ((lead & 0xF0) == 0xE0) {
        needed = 3;
    } else if ((lead & 0xF8) == 0xF0) {
        needed = 4;
    } else {
        return start - 1;
    }

    size_t have = len - (start - 1);
    if (have < needed) {
        return start - 1;
    }
    return len;
}

static void utf8_copy_trunc(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }

    size_t copy_len = utf8_safe_prefix_len(src, dst_size - 1);
    memcpy(dst, src, copy_len);
    dst[copy_len] = '\0';
}

static void log_text_preview(const char *label, const char *text) {
    const char *value = text ? text : "";
    size_t len = strlen(value);
    if (len == 0) {
        ESP_LOGI(TAG, "%s len=0 text=", label);
        return;
    }

    char preview[97];
    size_t preview_len = utf8_safe_prefix_len(value, sizeof(preview) - 1);
    memcpy(preview, value, preview_len);
    preview[preview_len] = '\0';
    ESP_LOGI(TAG, "%s len=%u preview=%s", label, (unsigned)len, preview);
}

static esp_err_t send_json_object(nuwax_ws_client_t *ws, cJSON *root) {
    char *json = cJSON_PrintUnformatted(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = nuwax_ws_send_text(ws, json);
    free(json);
    return err;
}

static esp_err_t send_sensitive_json_object(nuwax_ws_client_t *ws, cJSON *root) {
    char *json = cJSON_PrintUnformatted(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    const size_t length = strlen(json);
    esp_err_t err = nuwax_ws_send_text(ws, json);
    memset(json, 0, length);
    free(json);
    return err;
}

esp_err_t nuwax_session_send_hello(nuwax_ws_client_t *ws,
                                   const char *device_secret) {
    if (!ws || !device_secret || !device_secret[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *features = cJSON_CreateObject();
    cJSON *audio = cJSON_CreateObject();
    if (!root || !features || !audio) {
        cJSON_Delete(root);
        cJSON_Delete(features);
        cJSON_Delete(audio);
        return ESP_ERR_NO_MEM;
    }

    const nuwax_audio_board_caps_t *caps = nuwax_audio_board_get_caps();
    const bool live_aec_enabled =
        CONFIG_NUWAX_LIVE_AEC_ENABLE && CONFIG_NUWAX_P3_SOFTWARE_REF_ENABLE;
    const bool software_ref_enabled =
        CONFIG_NUWAX_P3_SOFTWARE_REF_ENABLE && CONFIG_NUWAX_SPEAKER_ENABLE;
    const bool hardware_ref_enabled =
        CONFIG_NUWAX_VOCAT_SLOT2_AEC_REF_ENABLE && caps->has_ref;
    const bool runtime_ref_available = software_ref_enabled || hardware_ref_enabled;

    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddStringToObject(root, "device_id", nuwax_device_uid_string()[0]
                                                     ? nuwax_device_uid_string()
                                                     : CONFIG_NUWAX_DEVICE_ID);
    cJSON_AddStringToObject(root, "device_secret", device_secret);
    cJSON_AddStringToObject(root, "transport", "websocket");
    cJSON_AddBoolToObject(features, "audio_upload", true);
    /* Device-side meeting mode is negotiated independently from normal
     * conversation capture.  This advertises capability only; it does not
     * mean that a meeting session is currently active. */
    cJSON_AddBoolToObject(features, "meeting_mode_v1", true);
    cJSON_AddBoolToObject(features, "monitoring_mode_v1", true);
    cJSON_AddBoolToObject(features, "remote_ble_reprovision", true);
#if CONFIG_NUWAX_SPEAKER_ENABLE
    cJSON_AddBoolToObject(features, "audio_playback", true);
#else
    cJSON_AddBoolToObject(features, "audio_playback", false);
#endif
    cJSON_AddBoolToObject(features, "mcp", true);
    cJSON_AddItemToObject(root, "features", features);

#if CONFIG_NUWAX_VOICE_UPLINK_CODEC_OPUS
    cJSON_AddStringToObject(audio, "input_format", "opus");
    cJSON_AddStringToObject(audio, "codec", "opus");
    cJSON_AddBoolToObject(audio, "opus", true);
#else
    cJSON_AddStringToObject(audio, "input_format", "pcm16");
    cJSON_AddStringToObject(audio, "codec", "pcm16");
    cJSON_AddBoolToObject(audio, "opus", false);
#endif
    cJSON_AddStringToObject(audio, "output_format", "pcm16");
    cJSON_AddNumberToObject(audio, "sample_rate", CONFIG_NUWAX_VOICE_SAMPLE_RATE);
    cJSON_AddNumberToObject(audio, "output_sample_rate", CONFIG_NUWAX_VOICE_OUTPUT_SAMPLE_RATE);
    cJSON_AddNumberToObject(audio, "channels", CONFIG_NUWAX_VOICE_CHANNELS);
    cJSON_AddNumberToObject(audio, "mic_channels", caps->mic_channels);
    cJSON_AddNumberToObject(audio, "frame_duration_ms", CONFIG_NUWAX_VOICE_FRAME_DURATION_MS);
#if CONFIG_NUWAX_SPEAKER_ENABLE
    cJSON_AddBoolToObject(audio, "speaker", true);
#else
    cJSON_AddBoolToObject(audio, "speaker", false);
#endif
    cJSON_AddBoolToObject(audio, "wake_word", CONFIG_NUWAX_WAKE_WORD_ENABLE ? true : false);
    cJSON_AddBoolToObject(audio, "vad", CONFIG_NUWAX_VOICE_LOCAL_VAD_ENDPOINT_ENABLE ? true : false);
    cJSON_AddBoolToObject(audio, "full_duplex", caps->full_duplex);
    cJSON_AddBoolToObject(audio, "shared_i2s", caps->shared_i2s);
    cJSON_AddStringToObject(audio, "hardware_profile", caps->hardware_profile ? caps->hardware_profile : "legacy");

    /* Advertise only reference paths enabled in this firmware configuration. */
    cJSON_AddBoolToObject(audio, "aec", live_aec_enabled);
    cJSON_AddStringToObject(audio,
                           "aec_reference",
                           aec_reference_kind_str(hardware_ref_enabled, software_ref_enabled));
    cJSON_AddBoolToObject(audio, "has_ref", runtime_ref_available);
    cJSON_AddBoolToObject(audio, "software_ref", software_ref_enabled);
    cJSON_AddBoolToObject(audio, "hardware_ref", hardware_ref_enabled);
    cJSON_AddNumberToObject(audio, "ref_slot", hardware_ref_enabled ? caps->ref_slot : -1);
    cJSON_AddStringToObject(audio, "afe_input_format", afe_input_format_str(caps->input_format));

    cJSON_AddItemToObject(root, "audio_params", audio);

    esp_err_t err = send_sensitive_json_object(ws, root);
    cJSON *secret_item = cJSON_GetObjectItemCaseSensitive(root, "device_secret");
    if (cJSON_IsString(secret_item) && secret_item->valuestring) {
        memset(secret_item->valuestring, 0, strlen(secret_item->valuestring));
    }
    cJSON_Delete(root);
    return err;
}

esp_err_t nuwax_session_send_ping(nuwax_ws_client_t *ws, int64_t now_ms) {
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "type", "ping");
    cJSON_AddNumberToObject(root, "now_ms", (double)now_ms);
    esp_err_t err = send_json_object(ws, root);
    cJSON_Delete(root);
    return err;
}

esp_err_t nuwax_session_send_listen(nuwax_ws_client_t *ws, const char *state, const char *mode) {
    return nuwax_session_send_listen_with_audio_session(ws, state, mode, NULL, NULL);
}

esp_err_t nuwax_session_send_listen_with_audio_session(nuwax_ws_client_t *ws,
                                                       const char *state,
                                                       const char *mode,
                                                       const char *audio_session_id,
                                                       const char *request_id) {
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "type", "listen");
    cJSON_AddStringToObject(root, "state", state);
    cJSON_AddStringToObject(root, "mode", mode);
    if (audio_session_id && audio_session_id[0] != '\0') {
        cJSON_AddStringToObject(root, "audio_session_id", audio_session_id);
    }
    if (request_id && request_id[0] != '\0') {
        cJSON_AddStringToObject(root, "request_id", request_id);
    }
    esp_err_t err = send_json_object(ws, root);
    cJSON_Delete(root);
    return err;
}

esp_err_t nuwax_session_send_text_request(nuwax_ws_client_t *ws, const char *request_id, const char *text) {
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "type", "text_request");
    cJSON_AddStringToObject(root, "request_id", request_id ? request_id : "");
    cJSON_AddStringToObject(root, "text", text ? text : "");
    esp_err_t err = send_json_object(ws, root);
    cJSON_Delete(root);
    return err;
}

esp_err_t nuwax_session_send_cancel_turn(nuwax_ws_client_t *ws,
                                         const char *request_id,
                                         const char *turn_id,
                                         const char *reason) {
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "type", "cancel_turn");
    cJSON_AddStringToObject(root, "request_id", request_id ? request_id : "");
    cJSON_AddStringToObject(root, "turn_id", turn_id ? turn_id : "");
    cJSON_AddStringToObject(root, "reason", reason ? reason : "device_cancel");
    esp_err_t err = send_json_object(ws, root);
    cJSON_Delete(root);
    return err;
}

esp_err_t nuwax_session_send_barge_in(nuwax_ws_client_t *ws,
                                      const char *state,
                                      const char *event_id,
                                      const char *parent_turn_id,
                                      const char *playback_id,
                                      const char *audio_session_id,
                                      uint32_t onset_sequence,
                                      uint32_t current_sequence,
                                      uint32_t confidence_q15) {
    if (!ws || !state || !event_id || event_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "type", "barge_in");
    cJSON_AddStringToObject(root, "state", state);
    cJSON_AddStringToObject(root, "event_id", event_id);
    cJSON_AddStringToObject(root, "parent_turn_id", parent_turn_id ? parent_turn_id : "");
    cJSON_AddStringToObject(root, "playback_id", playback_id ? playback_id : "");
    cJSON_AddStringToObject(root, "audio_session_id", audio_session_id ? audio_session_id : "");
    cJSON_AddNumberToObject(root, "onset_sequence", onset_sequence);
    cJSON_AddNumberToObject(root, "current_sequence", current_sequence);
    if (strcmp(state, "confirmed") == 0 || strcmp(state, "confirm") == 0) {
        cJSON_AddNumberToObject(root, "confirmed_sequence", current_sequence);
    }
    cJSON_AddNumberToObject(root, "confidence_q15", confidence_q15 > 32768U ? 32768U : confidence_q15);
    esp_err_t err = send_json_object(ws, root);
    cJSON_Delete(root);
    return err;
}

esp_err_t nuwax_session_send_pc_status_request(nuwax_ws_client_t *ws, const char *request_id) {
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "type", "pc_status_request");
    cJSON_AddStringToObject(root, "request_id", request_id ? request_id : "");
    esp_err_t err = send_json_object(ws, root);
    cJSON_Delete(root);
    return err;
}

esp_err_t nuwax_session_send_status_app_request(nuwax_ws_client_t *ws, const char *request_id, const char *app) {
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "type", "status_app_request");
    cJSON_AddStringToObject(root, "request_id", request_id ? request_id : "");
    cJSON_AddStringToObject(root, "app", app ? app : "");
    esp_err_t err = send_json_object(ws, root);
    cJSON_Delete(root);
    return err;
}

esp_err_t nuwax_session_send_touch_event(nuwax_ws_client_t *ws, const nuwax_touch_point_t *point, const char *action) {
    if (!point) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *touch = cJSON_CreateObject();
    if (!root || !touch) {
        cJSON_Delete(root);
        cJSON_Delete(touch);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "type", "input_event");
    cJSON_AddStringToObject(root, "source", "touch");
    cJSON_AddStringToObject(root, "action", action ? action : "press");
    cJSON_AddNumberToObject(touch, "x", point->x);
    cJSON_AddNumberToObject(touch, "y", point->y);
    cJSON_AddNumberToObject(touch, "gesture", point->gesture);
    cJSON_AddNumberToObject(touch, "points", point->points);
    cJSON_AddNumberToObject(touch, "event", point->event);
    cJSON_AddItemToObject(root, "touch", touch);

    esp_err_t err = send_json_object(ws, root);
    cJSON_Delete(root);
    return err;
}

esp_err_t nuwax_session_send_wifi_scan_result(nuwax_ws_client_t *ws,
                                              bool ok,
                                              const char *message,
                                              const char ssids[][33],
                                              const int8_t *rssis,
                                              const uint8_t *authmodes,
                                              int count) {
    if (count < 0) {
        count = 0;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *networks = cJSON_CreateArray();
    if (!root || !networks) {
        cJSON_Delete(root);
        cJSON_Delete(networks);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "type", "wifi_scan_result");
    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddStringToObject(root, "message", message ? message : "");
    cJSON_AddNumberToObject(root, "count", count);
    for (int i = 0; i < count; i++) {
        cJSON *network = cJSON_CreateObject();
        if (!network) {
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(network, "ssid", ssids ? ssids[i] : "");
        cJSON_AddNumberToObject(network, "rssi", rssis ? rssis[i] : 0);
        cJSON_AddNumberToObject(network, "authmode", authmodes ? authmodes[i] : 0);
        cJSON_AddBoolToObject(network, "secure", authmodes ? authmodes[i] != 0 : false);
        cJSON_AddItemToArray(networks, network);
    }
    cJSON_AddItemToObject(root, "networks", networks);

    esp_err_t err = send_json_object(ws, root);
    cJSON_Delete(root);
    return err;
}

esp_err_t nuwax_session_send_wifi_status(nuwax_ws_client_t *ws,
                                         const char *phase,
                                         const char *ssid,
                                         const char *ip,
                                         bool ok,
                                         const char *message) {
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "type", "wifi_status");
    cJSON_AddStringToObject(root, "phase", phase ? phase : "");
    cJSON_AddStringToObject(root, "ssid", ssid ? ssid : "");
    cJSON_AddStringToObject(root, "ip", ip ? ip : "");
    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddStringToObject(root, "message", message ? message : "");
    esp_err_t err = send_json_object(ws, root);
    cJSON_Delete(root);
    return err;
}

esp_err_t nuwax_session_send_command_ack(nuwax_ws_client_t *ws,
                                         const char *request_id,
                                         const char *command,
                                         bool ok,
                                         const char *message) {
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "type", "device_command_ack");
    cJSON_AddStringToObject(root, "request_id", request_id ? request_id : "");
    cJSON_AddStringToObject(root, "command", command ? command : "");
    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddStringToObject(root, "message", message ? message : "");
    esp_err_t err = send_json_object(ws, root);
    cJSON_Delete(root);
    return err;
}

void nuwax_session_handle_text(const char *payload, int len, nuwax_session_command_t *out_command) {
    if (out_command) {
        memset(out_command, 0, sizeof(*out_command));
    }
    ESP_LOGD(TAG, "RX text frame len=%d", len);

    cJSON *root = cJSON_ParseWithLength(payload, len);
    if (!root) {
        ESP_LOGW(TAG, "Received non-JSON text frame len=%d", len);
        return;
    }

    cJSON *type = cJSON_GetObjectItem(root, "type");
    cJSON *session_id = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(type)) {
        ESP_LOGI(TAG, "RX type=%s session=%s", type->valuestring,
                 cJSON_IsString(session_id) ? session_id->valuestring : "-");
    }

    if (out_command && cJSON_IsString(type) && strcmp(type->valuestring, "pong") == 0) {
        out_command->has_pong = true;
    }

    if (out_command && cJSON_IsString(type) && strcmp(type->valuestring, "device_command") == 0) {
        cJSON *command = cJSON_GetObjectItem(root, "command");
        cJSON *mode = cJSON_GetObjectItem(root, "mode");
        cJSON *request_id = cJSON_GetObjectItem(root, "request_id");
        cJSON *audio_session_id = cJSON_GetObjectItem(root, "audio_session_id");
        if (!audio_session_id) {
            audio_session_id = cJSON_GetObjectItem(root, "audioSessionId");
        }
        cJSON *turn_id = cJSON_GetObjectItem(root, "turn_id");
        cJSON *playback_id = cJSON_GetObjectItem(root, "playback_id");
        cJSON *barge_event_id = cJSON_GetObjectItem(root, "barge_event_id");
        cJSON *ssid = cJSON_GetObjectItem(root, "ssid");
        cJSON *password = cJSON_GetObjectItem(root, "password");
        cJSON *state = cJSON_GetObjectItem(root, "state");
        cJSON *emotion = cJSON_GetObjectItem(root, "emotion");
        if (!emotion) {
            emotion = cJSON_GetObjectItem(root, "assistant_emotion");
        }
        if (!emotion) {
            emotion = cJSON_GetObjectItem(root, "assistantEmotion");
        }
        cJSON *title = cJSON_GetObjectItem(root, "title");
        cJSON *lines = cJSON_GetObjectItem(root, "lines");
        cJSON *text = cJSON_GetObjectItem(root, "text");
        cJSON *volume_db = cJSON_GetObjectItem(root, "volume_db");
        if (!volume_db) {
            volume_db = cJSON_GetObjectItem(root, "volumeDb");
        }
        cJSON *volume_percent = cJSON_GetObjectItem(root, "volume_percent");
        if (!volume_percent) {
            volume_percent = cJSON_GetObjectItem(root, "volumePercent");
        }
        cJSON *volume_persist = cJSON_GetObjectItem(root, "persist");
        if (!volume_persist) {
            volume_persist = cJSON_GetObjectItem(root, "volume_persist");
        }
        if (!volume_persist) {
            volume_persist = cJSON_GetObjectItem(root, "volumePersist");
        }
        cJSON *duration_ms = cJSON_GetObjectItem(root, "duration_ms");
        if (!duration_ms) {
            duration_ms = cJSON_GetObjectItem(root, "durationMs");
        }
        cJSON *delay_samples = cJSON_GetObjectItem(root, "delay_samples");
        if (!delay_samples) {
            delay_samples = cJSON_GetObjectItem(root, "delaySamples");
        }
        cJSON *delay_ms = cJSON_GetObjectItem(root, "delay_ms");
        if (!delay_ms) {
            delay_ms = cJSON_GetObjectItem(root, "delayMs");
        }
        cJSON *run_ref_probe = cJSON_GetObjectItem(root, "run_ref_probe");
        if (!run_ref_probe) {
            run_ref_probe = cJSON_GetObjectItem(root, "runRefProbe");
        }
        cJSON *enabled = cJSON_GetObjectItem(root, "enabled");
        if (!enabled) {
            enabled = cJSON_GetObjectItem(root, "audio_cleaner_enabled");
        }
        if (!enabled) {
            enabled = cJSON_GetObjectItem(root, "audioCleanerEnabled");
        }
        out_command->has_command = true;
        utf8_copy_trunc(out_command->command, sizeof(out_command->command), cJSON_IsString(command) ? command->valuestring : "");
        utf8_copy_trunc(out_command->mode, sizeof(out_command->mode), cJSON_IsString(mode) ? mode->valuestring : "");
        utf8_copy_trunc(out_command->request_id,
                        sizeof(out_command->request_id),
                        cJSON_IsString(request_id) ? request_id->valuestring : "");
        utf8_copy_trunc(out_command->audio_session_id,
                        sizeof(out_command->audio_session_id),
                        cJSON_IsString(audio_session_id) ? audio_session_id->valuestring : "");
        utf8_copy_trunc(out_command->turn_id,
                        sizeof(out_command->turn_id),
                        cJSON_IsString(turn_id) ? turn_id->valuestring : "");
        utf8_copy_trunc(out_command->playback_id,
                        sizeof(out_command->playback_id),
                        cJSON_IsString(playback_id) ? playback_id->valuestring : "");
        utf8_copy_trunc(out_command->barge_event_id,
                        sizeof(out_command->barge_event_id),
                        cJSON_IsString(barge_event_id) ? barge_event_id->valuestring : "");
        utf8_copy_trunc(out_command->ssid, sizeof(out_command->ssid), cJSON_IsString(ssid) ? ssid->valuestring : "");
        utf8_copy_trunc(out_command->password,
                        sizeof(out_command->password),
                        cJSON_IsString(password) ? password->valuestring : "");
        utf8_copy_trunc(out_command->state, sizeof(out_command->state), cJSON_IsString(state) ? state->valuestring : "");
        utf8_copy_trunc(out_command->emotion,
                        sizeof(out_command->emotion),
                        cJSON_IsString(emotion) ? emotion->valuestring : "");
        utf8_copy_trunc(out_command->title, sizeof(out_command->title), cJSON_IsString(title) ? title->valuestring : "");
        utf8_copy_trunc(out_command->text, sizeof(out_command->text), cJSON_IsString(text) ? text->valuestring : "");
        if (cJSON_IsNumber(volume_db)) {
            out_command->has_volume_db = true;
            out_command->volume_db = (float)volume_db->valuedouble;
        }
        if (cJSON_IsNumber(volume_percent)) {
            out_command->has_volume_percent = true;
            out_command->volume_percent = volume_percent->valueint;
        }
        if (cJSON_IsBool(volume_persist)) {
            out_command->has_volume_persist = true;
            out_command->volume_persist = cJSON_IsTrue(volume_persist);
        }
        if (cJSON_IsNumber(duration_ms) && duration_ms->valueint > 0) {
            out_command->has_duration_ms = true;
            out_command->duration_ms = (uint32_t)duration_ms->valueint;
        }
        if (cJSON_IsNumber(delay_samples) && delay_samples->valueint >= 0) {
            out_command->has_delay_samples = true;
            out_command->delay_samples = (uint32_t)delay_samples->valueint;
        }
        if (cJSON_IsNumber(delay_ms) && delay_ms->valueint >= 0) {
            out_command->has_delay_ms = true;
            out_command->delay_ms = (uint32_t)delay_ms->valueint;
        }
        if (cJSON_IsBool(enabled)) {
            out_command->has_audio_cleaner_enabled = true;
            out_command->audio_cleaner_enabled = cJSON_IsTrue(enabled);
        }
        cJSON *interrupt_enabled = cJSON_GetObjectItem(root, "interrupt_enabled");
        if (!interrupt_enabled) {
            interrupt_enabled = cJSON_GetObjectItem(root, "interruptEnabled");
        }
        if (cJSON_IsBool(interrupt_enabled)) {
            out_command->has_interrupt_enabled = true;
            out_command->interrupt_enabled = cJSON_IsTrue(interrupt_enabled);
        }
        cJSON *interrupt_persist = cJSON_GetObjectItem(root, "persist");
        if (cJSON_IsBool(interrupt_persist)) {
            out_command->has_interrupt_persist = true;
            out_command->interrupt_persist = cJSON_IsTrue(interrupt_persist);
        }
        out_command->run_ref_probe = cJSON_IsTrue(run_ref_probe);
        ESP_LOGI(TAG,
                 "Device command detail command=%s mode=%s emotion=%s title=%s text_len=%u volume_db=%.1f volume_percent=%d persist=%d duration_ms=%lu delay_samples=%lu delay_ms=%lu cleaner_enabled=%d run_ref_probe=%d",
                 out_command->command,
                 out_command->mode[0] ? out_command->mode : "-",
                 out_command->emotion[0] ? out_command->emotion : "-",
                 out_command->title,
                 (unsigned)strlen(out_command->text),
                 out_command->has_volume_db ? (double)out_command->volume_db : -999.0,
                 out_command->has_volume_percent ? out_command->volume_percent : -1,
                 out_command->has_volume_persist && out_command->volume_persist ? 1 : 0,
                 (unsigned long)(out_command->has_duration_ms ? out_command->duration_ms : 0),
                 (unsigned long)(out_command->has_delay_samples ? out_command->delay_samples : 0),
                 (unsigned long)(out_command->has_delay_ms ? out_command->delay_ms : 0),
                 out_command->has_audio_cleaner_enabled ? (out_command->audio_cleaner_enabled ? 1 : 0) : -1,
                 out_command->run_ref_probe ? 1 : 0);
        if (out_command->text[0] != '\0') {
            log_text_preview("Device command text", out_command->text);
        }
        if (cJSON_IsArray(lines)) {
            for (int i = 0; i < NUWAX_SESSION_STATUS_LINES; i++) {
                cJSON *line = cJSON_GetArrayItem(lines, i);
                utf8_copy_trunc(out_command->lines[i],
                                sizeof(out_command->lines[i]),
                                cJSON_IsString(line) ? line->valuestring : "");
                if (out_command->lines[i][0] != '\0') {
                    ESP_LOGI(TAG, "Device command line[%d]=%s", i, out_command->lines[i]);
                }
            }
        }
    }

    if (out_command && cJSON_IsString(type) && strcmp(type->valuestring, "mcp") == 0) {
        cJSON *payload = cJSON_GetObjectItem(root, "payload");
        if (cJSON_IsObject(payload)) {
            char *payload_json = cJSON_PrintUnformatted(payload);
            if (payload_json) {
                utf8_copy_trunc(out_command->mcp_payload, sizeof(out_command->mcp_payload), payload_json);
                free(payload_json);
            }
            out_command->has_mcp = true;
            ESP_LOGI(TAG, "MCP message received payload_len=%u", (unsigned)strlen(out_command->mcp_payload));
        }
    }

    if (out_command && cJSON_IsString(type) &&
        (strcmp(type->valuestring, "asr_partial") == 0 || strcmp(type->valuestring, "asr_final") == 0)) {
        cJSON *provider = cJSON_GetObjectItem(root, "provider");
        cJSON *audio_session_id = cJSON_GetObjectItem(root, "audio_session_id");
        cJSON *turn_id = cJSON_GetObjectItem(root, "turn_id");
        cJSON *text = cJSON_GetObjectItem(root, "text");
        cJSON *definite = cJSON_GetObjectItem(root, "definite");
        cJSON *sequence = cJSON_GetObjectItem(root, "sequence");
        cJSON *at_ms = cJSON_GetObjectItem(root, "at_ms");
        out_command->has_event = true;
        out_command->event = strcmp(type->valuestring, "asr_final") == 0
                                 ? NUWAX_SESSION_EVENT_ASR_FINAL
                                 : NUWAX_SESSION_EVENT_ASR_PARTIAL;
        out_command->definite = cJSON_IsBool(definite) ? cJSON_IsTrue(definite) : out_command->event == NUWAX_SESSION_EVENT_ASR_FINAL;
        out_command->sequence = cJSON_IsNumber(sequence) ? sequence->valueint : 0;
        out_command->at_ms = cJSON_IsNumber(at_ms) ? at_ms->valueint : 0;
        utf8_copy_trunc(out_command->provider,
                        sizeof(out_command->provider),
                        cJSON_IsString(provider) ? provider->valuestring : "");
        utf8_copy_trunc(out_command->audio_session_id,
                        sizeof(out_command->audio_session_id),
                        cJSON_IsString(audio_session_id) ? audio_session_id->valuestring : "");
        utf8_copy_trunc(out_command->turn_id,
                        sizeof(out_command->turn_id),
                        cJSON_IsString(turn_id) ? turn_id->valuestring : "");
        utf8_copy_trunc(out_command->text, sizeof(out_command->text), cJSON_IsString(text) ? text->valuestring : "");
        ESP_LOGI(TAG,
                 "%s seq=%d definite=%d at_ms=%d provider=%s text_len=%u",
                 out_command->event == NUWAX_SESSION_EVENT_ASR_FINAL ? "ASR final" : "ASR partial",
                 out_command->sequence,
                 out_command->definite ? 1 : 0,
                 out_command->at_ms,
                 out_command->provider[0] ? out_command->provider : "-",
                 (unsigned)strlen(out_command->text));
        if (out_command->text[0] != '\0') {
            log_text_preview(out_command->event == NUWAX_SESSION_EVENT_ASR_FINAL ? "ASR final text" : "ASR partial text",
                             out_command->text);
        }
    }

    cJSON *frames = cJSON_GetObjectItem(root, "frames_received");
    cJSON *rms = cJSON_GetObjectItem(root, "last_rms");
    if (cJSON_IsNumber(frames)) {
        ESP_LOGI(TAG, "Audio stats frames=%d rms=%d", frames->valueint, cJSON_IsNumber(rms) ? rms->valueint : 0);
    }

    if (cJSON_IsString(type) && strcmp(type->valuestring, "input_event_ack") == 0) {
        cJSON *source = cJSON_GetObjectItem(root, "source");
        cJSON *action = cJSON_GetObjectItem(root, "action");
        cJSON *events = cJSON_GetObjectItem(root, "events_received");
        ESP_LOGI(TAG,
                 "Input event ack source=%s action=%s count=%d",
                 cJSON_IsString(source) ? source->valuestring : "-",
                 cJSON_IsString(action) ? action->valuestring : "-",
                 cJSON_IsNumber(events) ? events->valueint : 0);
    }

    cJSON *error = cJSON_GetObjectItem(root, "error");
    if (cJSON_IsString(error) && error->valuestring[0] != '\0') {
        ESP_LOGW(TAG, "Bridge error: %s", error->valuestring);
    }

    if (cJSON_IsString(type) && strcmp(type->valuestring, "audio_session") == 0) {
        cJSON *state = cJSON_GetObjectItem(root, "state");
        cJSON *audio_session_id = cJSON_GetObjectItem(root, "audio_session_id");
        cJSON *frames_count = cJSON_GetObjectItem(root, "frames");
        cJSON *duration_ms = cJSON_GetObjectItem(root, "duration_ms");
        cJSON *audio_rms = cJSON_GetObjectItem(root, "rms");
        ESP_LOGI(TAG,
                 "Audio session state=%s id=%s frames=%d duration_ms=%d rms=%d",
                 cJSON_IsString(state) ? state->valuestring : "-",
                 cJSON_IsString(audio_session_id) ? audio_session_id->valuestring : "-",
                 cJSON_IsNumber(frames_count) ? frames_count->valueint : 0,
                 cJSON_IsNumber(duration_ms) ? duration_ms->valueint : 0,
                 cJSON_IsNumber(audio_rms) ? audio_rms->valueint : 0);
    }

    if (cJSON_IsString(type) && strcmp(type->valuestring, "asr_result") == 0) {
        cJSON *ok = cJSON_GetObjectItem(root, "ok");
        cJSON *provider = cJSON_GetObjectItem(root, "provider");
        cJSON *audio_session_id = cJSON_GetObjectItem(root, "audio_session_id");
        cJSON *turn_id = cJSON_GetObjectItem(root, "turn_id");
        cJSON *text = cJSON_GetObjectItem(root, "text");
        cJSON *asr_error = cJSON_GetObjectItem(root, "error");
        if (out_command) {
            out_command->has_event = true;
            out_command->event = NUWAX_SESSION_EVENT_ASR_RESULT;
            out_command->ok = cJSON_IsBool(ok) ? cJSON_IsTrue(ok) : false;
            utf8_copy_trunc(out_command->provider,
                            sizeof(out_command->provider),
                            cJSON_IsString(provider) ? provider->valuestring : "");
            utf8_copy_trunc(out_command->audio_session_id,
                            sizeof(out_command->audio_session_id),
                            cJSON_IsString(audio_session_id) ? audio_session_id->valuestring : "");
            utf8_copy_trunc(out_command->turn_id,
                            sizeof(out_command->turn_id),
                            cJSON_IsString(turn_id) ? turn_id->valuestring : "");
            utf8_copy_trunc(out_command->text, sizeof(out_command->text), cJSON_IsString(text) ? text->valuestring : "");
            utf8_copy_trunc(out_command->error,
                            sizeof(out_command->error),
                            cJSON_IsString(asr_error) ? asr_error->valuestring : "");
        }
        ESP_LOGI(TAG,
                 "ASR result ok=%d provider=%s text_len=%u",
                 cJSON_IsBool(ok) ? cJSON_IsTrue(ok) : 0,
                 cJSON_IsString(provider) ? provider->valuestring : "-",
                 cJSON_IsString(text) ? (unsigned)strlen(text->valuestring) : 0);
        if (cJSON_IsString(text)) {
            log_text_preview("ASR text", text->valuestring);
        }
        if (cJSON_IsString(asr_error) && asr_error->valuestring[0] != '\0') {
            ESP_LOGW(TAG, "ASR error: %s", asr_error->valuestring);
        }
    }

    if (cJSON_IsString(type) && strcmp(type->valuestring, "text_response") == 0) {
        cJSON *ok = cJSON_GetObjectItem(root, "ok");
        cJSON *request_id = cJSON_GetObjectItem(root, "request_id");
        cJSON *turn_id = cJSON_GetObjectItem(root, "turn_id");
        cJSON *user = cJSON_GetObjectItem(root, "user_text");
        cJSON *assistant = cJSON_GetObjectItem(root, "assistant_text");
        cJSON *emotion = cJSON_GetObjectItem(root, "emotion");
        if (!emotion) {
            emotion = cJSON_GetObjectItem(root, "assistant_emotion");
        }
        if (!emotion) {
            emotion = cJSON_GetObjectItem(root, "assistantEmotion");
        }
        cJSON *tts_pending = cJSON_GetObjectItem(root, "tts_pending");
        cJSON *response_error = cJSON_GetObjectItem(root, "error");
        if (out_command) {
            out_command->has_event = true;
            out_command->event = NUWAX_SESSION_EVENT_TEXT_RESPONSE;
            out_command->ok = cJSON_IsBool(ok) ? cJSON_IsTrue(ok) : false;
            out_command->tts_pending = cJSON_IsBool(tts_pending) ? cJSON_IsTrue(tts_pending) : false;
            utf8_copy_trunc(out_command->request_id,
                            sizeof(out_command->request_id),
                            cJSON_IsString(request_id) ? request_id->valuestring : "");
            utf8_copy_trunc(out_command->turn_id,
                            sizeof(out_command->turn_id),
                            cJSON_IsString(turn_id) ? turn_id->valuestring : "");
            utf8_copy_trunc(out_command->user_text,
                            sizeof(out_command->user_text),
                            cJSON_IsString(user) ? user->valuestring : "");
            utf8_copy_trunc(out_command->text,
                            sizeof(out_command->text),
                            cJSON_IsString(assistant) ? assistant->valuestring : "");
            utf8_copy_trunc(out_command->emotion,
                            sizeof(out_command->emotion),
                            cJSON_IsString(emotion) ? emotion->valuestring : "");
            utf8_copy_trunc(out_command->error,
                            sizeof(out_command->error),
                            cJSON_IsString(response_error) ? response_error->valuestring : "");
        }
        ESP_LOGI(TAG,
                 "Text response ok=%d assistant_len=%u emotion=%s tts_pending=%d",
                 cJSON_IsBool(ok) ? cJSON_IsTrue(ok) : 0,
                 cJSON_IsString(assistant) ? (unsigned)strlen(assistant->valuestring) : 0,
                 cJSON_IsString(emotion) ? emotion->valuestring : "-",
                 cJSON_IsBool(tts_pending) && cJSON_IsTrue(tts_pending) ? 1 : 0);
        if (cJSON_IsString(user)) {
            log_text_preview("Text response user", user->valuestring);
        }
        if (cJSON_IsString(assistant)) {
            log_text_preview("Text response assistant", assistant->valuestring);
        }
        if (cJSON_IsString(response_error) && response_error->valuestring[0] != '\0') {
            ESP_LOGW(TAG, "Text response error: %s", response_error->valuestring);
        }
    }

    if (cJSON_IsString(type) &&
        (strcmp(type->valuestring, "playback_begin") == 0 || strcmp(type->valuestring, "playback_end") == 0)) {
        const bool is_begin = strcmp(type->valuestring, "playback_begin") == 0;
        cJSON *request_id = cJSON_GetObjectItem(root, "request_id");
        cJSON *turn_id = cJSON_GetObjectItem(root, "turn_id");
        cJSON *playback_id = cJSON_GetObjectItem(root, "playback_id");
        cJSON *provider = cJSON_GetObjectItem(root, "provider");
        cJSON *sample_rate = cJSON_GetObjectItem(root, "sample_rate");
        cJSON *channels = cJSON_GetObjectItem(root, "channels");
        cJSON *frame_duration_ms = cJSON_GetObjectItem(root, "frame_duration_ms");
        cJSON *frames_count = cJSON_GetObjectItem(root, "frames");
        cJSON *bytes = cJSON_GetObjectItem(root, "bytes");
        cJSON *reason = cJSON_GetObjectItem(root, "reason");
        if (out_command) {
            out_command->has_event = true;
            out_command->event = is_begin ? NUWAX_SESSION_EVENT_PLAYBACK_BEGIN
                                          : NUWAX_SESSION_EVENT_PLAYBACK_END;
            out_command->tts_frames = cJSON_IsNumber(frames_count) ? frames_count->valueint : 0;
            out_command->playback_sample_rate = cJSON_IsNumber(sample_rate) ? sample_rate->valueint : 0;
            out_command->playback_channels = cJSON_IsNumber(channels) ? channels->valueint : 0;
            out_command->playback_frame_duration_ms =
                cJSON_IsNumber(frame_duration_ms) ? frame_duration_ms->valueint : 0;
            out_command->playback_bytes = cJSON_IsNumber(bytes) ? bytes->valueint : 0;
            utf8_copy_trunc(out_command->request_id,
                            sizeof(out_command->request_id),
                            cJSON_IsString(request_id) ? request_id->valuestring : "");
            utf8_copy_trunc(out_command->turn_id,
                            sizeof(out_command->turn_id),
                            cJSON_IsString(turn_id) ? turn_id->valuestring : "");
            utf8_copy_trunc(out_command->playback_id,
                            sizeof(out_command->playback_id),
                            cJSON_IsString(playback_id) ? playback_id->valuestring : "");
            utf8_copy_trunc(out_command->provider,
                            sizeof(out_command->provider),
                            cJSON_IsString(provider) ? provider->valuestring : "");
            utf8_copy_trunc(out_command->playback_reason,
                            sizeof(out_command->playback_reason),
                            cJSON_IsString(reason) ? reason->valuestring : "");
        }
        ESP_LOGI(TAG,
                 "Playback %s turn=%s playback=%s frames=%d bytes=%d sample_rate=%d channels=%d reason=%s",
                 is_begin ? "begin" : "end",
                 cJSON_IsString(turn_id) ? turn_id->valuestring : "-",
                 cJSON_IsString(playback_id) ? playback_id->valuestring : "-",
                 cJSON_IsNumber(frames_count) ? frames_count->valueint : 0,
                 cJSON_IsNumber(bytes) ? bytes->valueint : 0,
                 cJSON_IsNumber(sample_rate) ? sample_rate->valueint : 0,
                 cJSON_IsNumber(channels) ? channels->valueint : 0,
                 cJSON_IsString(reason) ? reason->valuestring : "-");
    }

    if (cJSON_IsString(type) && strcmp(type->valuestring, "tts_result") == 0) {
        cJSON *ok = cJSON_GetObjectItem(root, "ok");
        cJSON *request_id = cJSON_GetObjectItem(root, "request_id");
        cJSON *turn_id = cJSON_GetObjectItem(root, "turn_id");
        cJSON *provider = cJSON_GetObjectItem(root, "provider");
        cJSON *playback_id = cJSON_GetObjectItem(root, "playback_id");
        cJSON *frames_count = cJSON_GetObjectItem(root, "frames");
        cJSON *duration_ms = cJSON_GetObjectItem(root, "duration_ms");
        cJSON *sample_rate = cJSON_GetObjectItem(root, "sample_rate");
        cJSON *channels = cJSON_GetObjectItem(root, "channels");
        cJSON *playback_start = cJSON_GetObjectItem(root, "playback_start");
        cJSON *playback_end = cJSON_GetObjectItem(root, "playback_end");
        cJSON *tts_error = cJSON_GetObjectItem(root, "error");
        const bool tts_ok = cJSON_IsBool(ok) ? cJSON_IsTrue(ok) : false;
        if (out_command) {
            out_command->has_event = true;
            out_command->event = NUWAX_SESSION_EVENT_TTS_RESULT;
            out_command->ok = tts_ok;
            out_command->playback_start = cJSON_IsBool(playback_start) ? cJSON_IsTrue(playback_start) : false;
            out_command->playback_end = cJSON_IsBool(playback_end) ? cJSON_IsTrue(playback_end) : false;
            out_command->tts_frames = cJSON_IsNumber(frames_count) ? frames_count->valueint : 0;
            utf8_copy_trunc(out_command->request_id,
                            sizeof(out_command->request_id),
                            cJSON_IsString(request_id) ? request_id->valuestring : "");
            utf8_copy_trunc(out_command->turn_id,
                            sizeof(out_command->turn_id),
                            cJSON_IsString(turn_id) ? turn_id->valuestring : "");
            utf8_copy_trunc(out_command->playback_id,
                            sizeof(out_command->playback_id),
                            cJSON_IsString(playback_id) ? playback_id->valuestring : "");
            utf8_copy_trunc(out_command->provider,
                            sizeof(out_command->provider),
                            cJSON_IsString(provider) ? provider->valuestring : "");
            utf8_copy_trunc(out_command->error,
                            sizeof(out_command->error),
                            cJSON_IsString(tts_error) ? tts_error->valuestring : "");
        }
        ESP_LOGI(TAG,
                 "TTS result ok=%d provider=%s playback=%s frames=%d duration_ms=%d sample_rate=%d channels=%d error=%s",
                 cJSON_IsBool(ok) ? cJSON_IsTrue(ok) : 0,
                 cJSON_IsString(provider) ? provider->valuestring : "-",
                 cJSON_IsString(playback_id) ? playback_id->valuestring : "-",
                 cJSON_IsNumber(frames_count) ? frames_count->valueint : 0,
                 cJSON_IsNumber(duration_ms) ? duration_ms->valueint : 0,
                 cJSON_IsNumber(sample_rate) ? sample_rate->valueint : 0,
                 cJSON_IsNumber(channels) ? channels->valueint : 0,
                 cJSON_IsString(tts_error) ? tts_error->valuestring : "");
    }
    if (cJSON_IsString(type) && strcmp(type->valuestring, "cancel_turn_ack") == 0) {
        cJSON *ok = cJSON_GetObjectItem(root, "ok");
        cJSON *turn_id = cJSON_GetObjectItem(root, "turn_id");
        cJSON *new_turn_id = cJSON_GetObjectItem(root, "new_turn_id");
        cJSON *reason = cJSON_GetObjectItem(root, "reason");
        ESP_LOGI(TAG,
                 "Cancel turn ack ok=%d turn=%s new_turn=%s reason=%s",
                 cJSON_IsBool(ok) ? cJSON_IsTrue(ok) : 0,
                 cJSON_IsString(turn_id) ? turn_id->valuestring : "-",
                 cJSON_IsString(new_turn_id) ? new_turn_id->valuestring : "-",
                 cJSON_IsString(reason) ? reason->valuestring : "");
    }
    cJSON_Delete(root);
}
