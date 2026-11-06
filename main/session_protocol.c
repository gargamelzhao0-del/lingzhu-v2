#include "session_protocol.h"
#include "cloud_time.h"
#include "nuwax_provision_new/nuwax_device_uid.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "board/audio_board.h"
#include "cJSON.h"
#include "json_resource_budget.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ws_send_failure_policy.h"
#if CONFIG_NUWAX_OTA_ENABLE
#include "esp_ota_ops.h"
#include "nuwax_ota_new/nuwax_ota_main.h"
#include "nuwax_ota_new/nuwax_ota_release_policy.h"
#include "nuwax_ota_new/nuwax_ota_transport.h"
#endif

static const char *TAG = "nuwax_session";

_Static_assert(CONFIG_NUWAX_VOICE_CHANNELS == 1,
               "Nuwax cloud audio protocol currently requires mono uplink");

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
#ifndef CONFIG_NUWAX_VOCAT_MIC3_REFERENCE_PRODUCTION_PROVEN
#define CONFIG_NUWAX_VOCAT_MIC3_REFERENCE_PRODUCTION_PROVEN 0
#endif
#ifndef CONFIG_NUWAX_SPEAKER_ENABLE
#define CONFIG_NUWAX_SPEAKER_ENABLE 0
#endif
#ifndef CONFIG_NUWAX_OTA_ENABLE
#define CONFIG_NUWAX_OTA_ENABLE 0
#endif
#ifndef CONFIG_NUWAX_PROTOCOL_LISTEN_STOP_ENABLE
#define CONFIG_NUWAX_PROTOCOL_LISTEN_STOP_ENABLE 1
#endif

static bool suppress_listen_stop(const char *state,
                                 const char *mode,
                                 const char *audio_session_id)
{
#if !CONFIG_NUWAX_PROTOCOL_LISTEN_STOP_ENABLE
    if (state && strcmp(state, "stop") == 0) {
        ESP_LOGW(TAG,
                 "listen stop suppressed by build config mode=%s audio_session=%s",
                 mode ? mode : "-",
                 audio_session_id && audio_session_id[0] != '\0'
                     ? audio_session_id
                     : "-");
        return true;
    }
#else
    (void)state;
    (void)mode;
    (void)audio_session_id;
#endif
    return false;
}

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

static const char *hardware_ref_health_str(nuwax_hardware_ref_health_t state)
{
    switch (state) {
    case NUWAX_HARDWARE_REF_HEALTH_PASSED:
        return "passed";
    case NUWAX_HARDWARE_REF_HEALTH_FAILED:
        return "failed";
    case NUWAX_HARDWARE_REF_HEALTH_BLOCKED_UNPROVEN:
        return "blocked_unproven";
    case NUWAX_HARDWARE_REF_HEALTH_UNTESTED:
    default:
        return "untested";
    }
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

typedef struct {
    const char *name;
    size_t capacity;
} bounded_json_string_t;

static unsigned json_key_count(const cJSON *object, const char *name) {
    unsigned count = 0U;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, object) {
        if (item->string && strcmp(item->string, name) == 0) {
            count++;
        }
    }
    return count;
}

static unsigned json_alias_count(const cJSON *object,
                                 const char *first,
                                 const char *second) {
    return json_key_count(object, first) + json_key_count(object, second);
}

static bool json_ascii_matches(const char *value,
                               bool (*allowed)(unsigned char)) {
    if (!value || value[0] == '\0' || !allowed) {
        return false;
    }
    for (const unsigned char *cursor = (const unsigned char *)value;
         *cursor != 0U;
         ++cursor) {
        if (!allowed(*cursor)) {
            return false;
        }
    }
    return true;
}

static bool is_unenveloped_set_wake_word(const cJSON *root) {
    const cJSON *command =
        cJSON_GetObjectItemCaseSensitive(root, "command");
    return cJSON_IsObject(root) && json_key_count(root, "type") == 0U &&
           json_key_count(root, "request_id") == 0U &&
           json_key_count(root, "requestId") == 0U &&
           json_key_count(root, "command") == 1U &&
           cJSON_IsString(command) && command->valuestring &&
           strcmp(command->valuestring, "set_wake_word") == 0;
}

static bool command_name_char_allowed(unsigned char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
           ch == '_';
}

static bool request_id_char_allowed(unsigned char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
           (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' ||
           ch == ':' || ch == '-';
}

static bool music_url_is_valid(const char *url) {
    if (!url || strncmp(url, "https://", 8U) != 0 || url[8] == '\0') {
        return false;
    }
    for (const unsigned char *cursor = (const unsigned char *)url;
         *cursor != 0U;
         ++cursor) {
        if (iscntrl(*cursor) || isspace(*cursor) || *cursor == '#' ||
            *cursor == '\\') {
            return false;
        }
    }
    return true;
}

static bool json_parse_end_is_clean(const char *payload,
                                    int len,
                                    const char *parse_end) {
    const char *payload_end = payload + len;
    while (parse_end && parse_end < payload_end &&
           isspace((unsigned char)*parse_end)) {
        parse_end++;
    }
    return parse_end == payload_end;
}

static bool validate_bounded_json_strings(const cJSON *root) {
    const cJSON *message_type =
        cJSON_GetObjectItemCaseSensitive(root, "type");
    const bool is_tts_skip =
        cJSON_IsString(message_type) && message_type->valuestring &&
        strcmp(message_type->valuestring, "tts_skip") == 0;
    static const bounded_json_string_t fields[] = {
        {"type", 48U},
        {"command", 32U},
        {"mode", 24U},
        {"request_id", 40U},
        {"requestId", 40U},
        {"event_id", 48U},
        {"audio_session_id", 48U},
        {"audioSessionId", 48U},
        {"barge_event_id", 48U},
        {"bargeEventId", 48U},
        {"turn_id", 48U},
        {"turnId", 48U},
        {"notify_id", 48U},
        {"new_turn_id", 48U},
        {"playback_id", 48U},
        {"playbackId", 48U},
        {"codec", 16U},
        {"playback_reason", 24U},
        {"provider", 24U},
        {"url", NUWAX_SESSION_MUSIC_URL_MAX_LEN},
        {"format", NUWAX_SESSION_MUSIC_FORMAT_MAX_LEN},
        {"state", 24U},
        {"emotion", 24U},
        {"assistant_emotion", 24U},
        {"assistantEmotion", 24U},
        {"title", 40U},
        {"ssid", 33U},
        {"password", 65U},
        /* Keep this metadata field independent from the 24-byte playback
         * reason buffer. Cloud gateways may include longer diagnostic reason
         * strings; validation remains bounded and dispatch truncates into
         * fixed-size destinations where applicable. */
        {"reason", 64U},
        {"text", 512U},
        {"assistant_text", 512U},
        {"assistantText", 512U},
        {"user_text", 256U},
        {"error", 128U},
    };
    for (size_t i = 0U; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        const unsigned count = json_key_count(root, fields[i].name);
        if (count > 1U) {
            ESP_LOGE(TAG,
                     "inbound JSON rejected duplicate field=%s count=%u",
                     fields[i].name,
                     count);
            return false;
        }
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(
            root, fields[i].name);
        if (!item) {
            continue;
        }
        /* Optional protocol strings are occasionally serialized as JSON null
         * by the formal gateway. Treat null exactly like an omitted optional
         * field; message-specific validators below still reject null for
         * required fields such as device_command.request_id. */
        if (cJSON_IsNull(item)) {
            continue;
        }
        if (!cJSON_IsString(item) || !item->valuestring) {
            ESP_LOGE(TAG,
                     "inbound JSON rejected non-string field=%s",
                     fields[i].name);
            return false;
        }
        const size_t bytes = strlen(item->valuestring);
        const size_t capacity =
            is_tts_skip && strcmp(fields[i].name, "reason") == 0
                ? sizeof(((nuwax_session_command_t *)0)->error)
                : fields[i].capacity;
        if (bytes >= capacity) {
            ESP_LOGE(TAG,
                     "inbound JSON rejected overlong field=%s bytes=%u max_bytes=%u",
                     fields[i].name,
                     (unsigned)bytes,
                     (unsigned)(capacity - 1U));
            return false;
        }
    }

    static const struct {
        const char *first;
        const char *second;
    } aliases[] = {
        {"request_id", "requestId"},
        {"audio_session_id", "audioSessionId"},
        {"barge_event_id", "bargeEventId"},
        {"turn_id", "turnId"},
        {"playback_id", "playbackId"},
        {"assistant_emotion", "assistantEmotion"},
    };
    for (size_t i = 0U; i < sizeof(aliases) / sizeof(aliases[0]); ++i) {
        if (json_alias_count(root, aliases[i].first, aliases[i].second) >
            1U) {
            ESP_LOGE(TAG,
                     "inbound JSON rejected conflicting aliases=%s/%s",
                     aliases[i].first,
                     aliases[i].second);
            return false;
        }
    }

    const cJSON *lines = cJSON_GetObjectItemCaseSensitive(root, "lines");
    if (lines) {
        if (!cJSON_IsArray(lines) ||
            cJSON_GetArraySize(lines) > NUWAX_SESSION_STATUS_LINES) {
            ESP_LOGE(TAG, "inbound JSON rejected invalid status lines");
            return false;
        }
        const cJSON *line = NULL;
        cJSON_ArrayForEach(line, lines) {
            if (!cJSON_IsString(line) || !line->valuestring ||
                strlen(line->valuestring) >=
                    sizeof(((nuwax_session_command_t *)0)->lines[0])) {
                ESP_LOGE(TAG,
                         "inbound JSON rejected invalid/overlong status line");
                return false;
            }
        }
    }
    return true;
}

static bool validate_device_command_json(const cJSON *root) {
    static const char *const unique_fields[] = {
        "type", "command", "mode", "request_id", "requestId",
        "audio_session_id", "audioSessionId", "barge_event_id",
        "bargeEventId", "turn_id", "turnId", "playback_id",
        "playbackId", "notify_id", "ssid", "password", "wake_word", "wakeWord",
        "state", "emotion", "assistant_emotion", "assistantEmotion",
        "title", "lines", "text", "volume_db", "volumeDb",
        "url", "format",
        "volume_percent", "volumePercent", "persist", "volume_persist",
        "volumePersist", "wake_word_persist", "wakeWordPersist",
        "duration_ms", "durationMs", "delay_samples", "delaySamples",
        "delay_ms", "delayMs", "run_ref_probe", "runRefProbe",
        "enabled", "audio_cleaner_enabled", "audioCleanerEnabled",
        "interrupt_enabled", "interruptEnabled", "interrupt_current_turn",
        "interruptCurrentTurn",
    };
    for (size_t i = 0U;
         i < sizeof(unique_fields) / sizeof(unique_fields[0]);
         ++i) {
        if (json_key_count(root, unique_fields[i]) > 1U) {
            ESP_LOGE(TAG,
                     "device_command rejected duplicate field=%s",
                     unique_fields[i]);
            return false;
        }
    }

    static const struct {
        const char *names[3];
        size_t count;
    } alias_groups[] = {
        {{"request_id", "requestId", NULL}, 2U},
        {{"audio_session_id", "audioSessionId", NULL}, 2U},
        {{"barge_event_id", "bargeEventId", NULL}, 2U},
        {{"turn_id", "turnId", NULL}, 2U},
        {{"playback_id", "playbackId", NULL}, 2U},
        {{"wake_word", "wakeWord", NULL}, 2U},
        {{"volume_db", "volumeDb", NULL}, 2U},
        {{"volume_percent", "volumePercent", NULL}, 2U},
        {{"persist", "volume_persist", "volumePersist"}, 3U},
        {{"persist", "wake_word_persist", "wakeWordPersist"}, 3U},
        {{"duration_ms", "durationMs", NULL}, 2U},
        {{"delay_samples", "delaySamples", NULL}, 2U},
        {{"delay_ms", "delayMs", NULL}, 2U},
        {{"run_ref_probe", "runRefProbe", NULL}, 2U},
        {{"enabled", "audio_cleaner_enabled", "audioCleanerEnabled"}, 3U},
        {{"interrupt_enabled", "interruptEnabled", NULL}, 2U},
        {{"interrupt_current_turn", "interruptCurrentTurn", NULL}, 2U},
    };
    for (size_t i = 0U;
         i < sizeof(alias_groups) / sizeof(alias_groups[0]);
         ++i) {
        unsigned count = 0U;
        for (size_t j = 0U; j < alias_groups[i].count; ++j) {
            count += json_key_count(root, alias_groups[i].names[j]);
        }
        if (count > 1U) {
            ESP_LOGE(TAG,
                     "device_command rejected conflicting alias group first=%s count=%u",
                     alias_groups[i].names[0],
                     count);
            return false;
        }
    }

    const cJSON *command = cJSON_GetObjectItemCaseSensitive(root, "command");
    const cJSON *request_id =
        cJSON_GetObjectItemCaseSensitive(root, "request_id");
    const bool unenveloped_wake_word =
        is_unenveloped_set_wake_word(root);
    if (json_key_count(root, "command") != 1U ||
        !cJSON_IsString(command) || !command->valuestring ||
        command->valuestring[0] == '\0' ||
        (!unenveloped_wake_word &&
         (json_key_count(root, "request_id") != 1U ||
          !cJSON_IsString(request_id) || !request_id->valuestring ||
          request_id->valuestring[0] == '\0'))) {
        ESP_LOGE(TAG,
                 "device_command rejected: command/request_id must be unique non-empty strings (request_id may be omitted only by unenveloped set_wake_word)");
        return false;
    }
    if (!json_ascii_matches(command->valuestring,
                            command_name_char_allowed) ||
        (!unenveloped_wake_word &&
         !json_ascii_matches(request_id->valuestring,
                             request_id_char_allowed))) {
        ESP_LOGE(TAG,
                 "device_command rejected: invalid command/request_id characters");
        return false;
    }

    if (strcmp(command->valuestring, "stop_playback") == 0) {
        const cJSON *turn_id =
            cJSON_GetObjectItemCaseSensitive(root, "turn_id");
        const cJSON *playback_id =
            cJSON_GetObjectItemCaseSensitive(root, "playback_id");
        if (!cJSON_IsString(turn_id) || !turn_id->valuestring ||
            turn_id->valuestring[0] == '\0' ||
            !cJSON_IsString(playback_id) || !playback_id->valuestring ||
            playback_id->valuestring[0] == '\0') {
            ESP_LOGE(TAG,
                     "stop_playback rejected: turn_id and playback_id are required");
            return false;
        }
    } else if (strcmp(command->valuestring,
                      "prepare_notify_playback") == 0) {
        const cJSON *notify_id =
            cJSON_GetObjectItemCaseSensitive(root, "notify_id");
        if (json_key_count(root, "notify_id") != 1U ||
            !cJSON_IsString(notify_id) || !notify_id->valuestring ||
            !json_ascii_matches(notify_id->valuestring,
                                request_id_char_allowed)) {
            ESP_LOGE(TAG,
                     "prepare_notify_playback rejected: one ASCII-safe notify_id is required");
            return false;
        }
    } else if (strcmp(command->valuestring, "wifi_connect") == 0) {
        const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
        if (!cJSON_IsString(ssid) || !ssid->valuestring ||
            ssid->valuestring[0] == '\0') {
            ESP_LOGE(TAG, "wifi_connect rejected: non-empty ssid required");
            return false;
        }
    } else if (strcmp(command->valuestring, "set_state") == 0) {
        const cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
        if (!cJSON_IsString(state) || !state->valuestring ||
            state->valuestring[0] == '\0') {
            ESP_LOGE(TAG, "set_state rejected: non-empty state required");
            return false;
        }
        if (strcmp(state->valuestring, "sleeping") != 0 &&
            strcmp(state->valuestring, "wake_only") != 0) {
            ESP_LOGE(TAG,
                     "set_state rejected: device lifecycle owns state=%s",
                     state->valuestring);
            return false;
        }
    } else if (strcmp(command->valuestring, "play_music") == 0) {
        const cJSON *url = cJSON_GetObjectItemCaseSensitive(root, "url");
        const cJSON *format =
            cJSON_GetObjectItemCaseSensitive(root, "format");
        if (json_key_count(root, "url") != 1U ||
            !cJSON_IsString(url) || !url->valuestring ||
            !music_url_is_valid(url->valuestring) ||
            json_key_count(root, "format") != 1U ||
            !cJSON_IsString(format) || !format->valuestring ||
            strcmp(format->valuestring, "mp3") != 0) {
            ESP_LOGE(TAG,
                     "play_music rejected: HTTPS url and format=mp3 are required");
            return false;
        }
    }
    return true;
}

static bool validate_nonnegative_integer(const cJSON *item) {
    return !item || (cJSON_IsNumber(item) && item->valuedouble >= 0.0 &&
                     (double)item->valueint == item->valuedouble);
}

static bool validate_notification_event_json(const cJSON *root,
                                             bool is_begin) {
    static const char *const unique_fields[] = {
        "type", "request_id", "notify_id", "playback_id", "codec",
        "sample_rate", "channels", "frame_duration_ms", "frames",
        "bytes", "reason",
    };
    for (size_t i = 0U;
         i < sizeof(unique_fields) / sizeof(unique_fields[0]);
         ++i) {
        if (json_key_count(root, unique_fields[i]) > 1U) {
            ESP_LOGE(TAG,
                     "notification playback rejected duplicate field=%s",
                     unique_fields[i]);
            return false;
        }
    }

    const cJSON *request_id =
        cJSON_GetObjectItemCaseSensitive(root, "request_id");
    const cJSON *notify_id =
        cJSON_GetObjectItemCaseSensitive(root, "notify_id");
    const cJSON *playback_id =
        cJSON_GetObjectItemCaseSensitive(root, "playback_id");
    if (json_key_count(root, "request_id") != 1U ||
        json_key_count(root, "notify_id") != 1U ||
        json_key_count(root, "playback_id") != 1U ||
        !cJSON_IsString(request_id) || !request_id->valuestring ||
        !cJSON_IsString(notify_id) || !notify_id->valuestring ||
        !cJSON_IsString(playback_id) || !playback_id->valuestring ||
        !json_ascii_matches(request_id->valuestring,
                            request_id_char_allowed) ||
        !json_ascii_matches(notify_id->valuestring,
                            request_id_char_allowed) ||
        !json_ascii_matches(playback_id->valuestring,
                            request_id_char_allowed)) {
        ESP_LOGE(TAG,
                 "notification playback rejected: request_id/notify_id/playback_id must be unique ASCII-safe strings");
        return false;
    }

    const cJSON *codec = cJSON_GetObjectItemCaseSensitive(root, "codec");
    const cJSON *sample_rate =
        cJSON_GetObjectItemCaseSensitive(root, "sample_rate");
    const cJSON *channels =
        cJSON_GetObjectItemCaseSensitive(root, "channels");
    const cJSON *frame_duration_ms =
        cJSON_GetObjectItemCaseSensitive(root, "frame_duration_ms");
    const cJSON *frames = cJSON_GetObjectItemCaseSensitive(root, "frames");
    const cJSON *bytes = cJSON_GetObjectItemCaseSensitive(root, "bytes");
    if ((codec && (!cJSON_IsString(codec) ||
                   strcmp(codec->valuestring, "pcm16") != 0)) ||
        (sample_rate && (!cJSON_IsNumber(sample_rate) ||
                         sample_rate->valueint != 16000)) ||
        (channels && (!cJSON_IsNumber(channels) ||
                      channels->valueint != 1)) ||
        !validate_nonnegative_integer(frame_duration_ms) ||
        !validate_nonnegative_integer(frames) ||
        !validate_nonnegative_integer(bytes)) {
        ESP_LOGE(TAG,
                 "notification playback rejected: audio must be pcm16/16000Hz/mono with nonnegative integer totals");
        return false;
    }
    if (!is_begin) {
        const cJSON *reason =
            cJSON_GetObjectItemCaseSensitive(root, "reason");
        if (reason && (!cJSON_IsString(reason) || !reason->valuestring)) {
            ESP_LOGE(TAG,
                     "notify_playback_end rejected: reason must be a string");
            return false;
        }
    }
    return true;
}

static bool validate_tts_skip_json(const cJSON *root) {
    const cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    const cJSON *request_id =
        cJSON_GetObjectItemCaseSensitive(root, "request_id");
    if (!request_id) {
        request_id = cJSON_GetObjectItemCaseSensitive(root, "requestId");
    }
    const cJSON *turn_id =
        cJSON_GetObjectItemCaseSensitive(root, "turn_id");
    if (!turn_id) {
        turn_id = cJSON_GetObjectItemCaseSensitive(root, "turnId");
    }
    const cJSON *reason = cJSON_GetObjectItemCaseSensitive(root, "reason");
    const unsigned request_id_count =
        json_alias_count(root, "request_id", "requestId");
    const unsigned turn_id_count =
        json_alias_count(root, "turn_id", "turnId");
    const bool request_id_valid =
        !request_id || cJSON_IsNull(request_id) ||
        (cJSON_IsString(request_id) && request_id->valuestring);
    const bool turn_id_valid =
        !turn_id || cJSON_IsNull(turn_id) ||
        (cJSON_IsString(turn_id) && turn_id->valuestring);
    if (json_key_count(root, "ok") != 1U ||
        request_id_count > 1U || turn_id_count > 1U ||
        json_key_count(root, "reason") != 1U ||
        !cJSON_IsTrue(ok) || !request_id_valid || !turn_id_valid ||
        !cJSON_IsString(reason) ||
        !reason->valuestring ||
        strcmp(reason->valuestring, "voice_broadcast_disabled") != 0) {
        ESP_LOGE(TAG,
                 "tts_skip rejected ok_true=%d request_id_kind=%s turn_id_kind=%s reason_valid=%d",
                 cJSON_IsTrue(ok) ? 1 : 0,
                 !request_id ? "missing"
                             : cJSON_IsNull(request_id) ? "null"
                             : cJSON_IsString(request_id) ? "string"
                                                          : "invalid",
                 !turn_id ? "missing"
                          : cJSON_IsNull(turn_id) ? "null"
                          : cJSON_IsString(turn_id) ? "string" : "invalid",
                 cJSON_IsString(reason) && reason->valuestring &&
                         strcmp(reason->valuestring,
                                "voice_broadcast_disabled") == 0
                     ? 1
                     : 0);
        return false;
    }
    return true;
}

static bool validate_mcp_json(const cJSON *root) {
    if (json_key_count(root, "payload") != 1U) {
        ESP_LOGE(TAG, "MCP envelope rejected: one payload object required");
        return false;
    }
    const cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    if (!cJSON_IsObject(payload) ||
        json_key_count(payload, "jsonrpc") != 1U ||
        json_key_count(payload, "id") != 1U ||
        json_key_count(payload, "method") != 1U ||
        json_key_count(payload, "params") > 1U) {
        ESP_LOGE(TAG, "MCP envelope rejected: invalid or duplicate fields");
        return false;
    }
    const cJSON *jsonrpc =
        cJSON_GetObjectItemCaseSensitive(payload, "jsonrpc");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(payload, "id");
    const cJSON *method = cJSON_GetObjectItemCaseSensitive(payload, "method");
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(payload, "params");
    if (!cJSON_IsString(jsonrpc) ||
        strcmp(jsonrpc->valuestring, "2.0") != 0 ||
        (!cJSON_IsString(id) && !cJSON_IsNumber(id)) ||
        !cJSON_IsString(method) || !method->valuestring ||
        method->valuestring[0] == '\0' || (params && !cJSON_IsObject(params))) {
        ESP_LOGE(TAG, "MCP envelope rejected: invalid JSON-RPC contract");
        return false;
    }
    char *serialized = cJSON_PrintUnformatted(payload);
    if (!serialized) {
        return false;
    }
    const size_t payload_bytes = strlen(serialized);
    free(serialized);
    if (payload_bytes >= sizeof(((nuwax_session_command_t *)0)->mcp_payload)) {
        ESP_LOGE(TAG,
                 "MCP envelope rejected: payload_bytes=%u max_bytes=%u",
                 (unsigned)payload_bytes,
                 (unsigned)(sizeof(((nuwax_session_command_t *)0)->mcp_payload) -
                            1U));
        return false;
    }
    return true;
}

bool nuwax_session_validate_text_frame(const char *payload, int len) {
    if (!payload || len <= 0) {
        return false;
    }
    const char *parse_end = NULL;
    cJSON *root = nuwax_json_parse_with_length_opts(payload,
                                            (size_t)len,
                                            &parse_end,
                                            false);
    if (!root || !cJSON_IsObject(root) ||
        !json_parse_end_is_clean(payload, len, parse_end)) {
        ESP_LOGE(TAG, "inbound text rejected: incomplete or invalid JSON object");
        cJSON_Delete(root);
        return false;
    }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const bool unenveloped_wake_word =
        is_unenveloped_set_wake_word(root);
    const bool typed_frame = json_key_count(root, "type") == 1U &&
                             cJSON_IsString(type) && type->valuestring &&
                             type->valuestring[0] != '\0';
    bool valid = (typed_frame || unenveloped_wake_word) &&
                 validate_bounded_json_strings(root);
    if (valid &&
        ((typed_frame && strcmp(type->valuestring, "device_command") == 0) ||
         unenveloped_wake_word)) {
        valid = validate_device_command_json(root);
    } else if (valid && strcmp(type->valuestring, "mcp") == 0) {
        valid = validate_mcp_json(root);
    } else if (valid &&
               strcmp(type->valuestring, "notify_playback_begin") == 0) {
        valid = validate_notification_event_json(root, true);
    } else if (valid &&
               strcmp(type->valuestring, "notify_playback_end") == 0) {
        valid = validate_notification_event_json(root, false);
    } else if (valid && strcmp(type->valuestring, "tts_skip") == 0) {
        valid = validate_tts_skip_json(root);
    }
    cJSON_Delete(root);
    return valid;
}

static void log_text_preview(const char *label, const char *text) {
    /* Keep event/length diagnostics without persisting a child's speech or
     * cloud response in serial logs. The protocol payload is not changed. */
    ESP_LOGI(TAG, "%s len=%u content=redacted", label,
             (unsigned)(text ? strlen(text) : 0U));
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

static esp_err_t send_json_object_until(nuwax_ws_client_t *ws,
                                        cJSON *root,
                                        int64_t deadline_us) {
    if (!ws || !root || deadline_us <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    char *json = cJSON_PrintUnformatted(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = nuwax_ws_send_text_until(ws, json, deadline_us);
    free(json);
    return err;
}

static esp_err_t send_json_object_until_if_epoch(
    nuwax_ws_client_t *ws,
    cJSON *root,
    int64_t deadline_us,
    uint32_t expected_epoch) {
    if (!ws || !root || deadline_us <= 0 || expected_epoch == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    char *json = cJSON_PrintUnformatted(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = nuwax_ws_send_text_until_if_epoch(
        ws, json, deadline_us, expected_epoch);
    free(json);
    return err;
}

static void secure_zero_memory(void *memory, size_t length) {
    volatile unsigned char *cursor = (volatile unsigned char *)memory;
    while (length-- > 0U) {
        *cursor++ = 0U;
    }
}

static esp_err_t send_sensitive_json_object(nuwax_ws_client_t *ws, cJSON *root) {
    char *json = cJSON_PrintUnformatted(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    const size_t length = strlen(json);
    esp_err_t err = nuwax_ws_send_text(ws, json);
    secure_zero_memory(json, length);
    cJSON_free(json);
    return err;
}

esp_err_t nuwax_session_send_hello(nuwax_ws_client_t *ws,
                                   const char *device_secret, bool has_sd_card) {
    if (!ws || !device_secret || !device_secret[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ESP_ERR_NO_MEM;
    cJSON *root = cJSON_CreateObject();
    cJSON *features = cJSON_CreateObject();
    cJSON *audio = cJSON_CreateObject();
    cJSON *firmware = NULL;
    if (!root || !features || !audio) goto done;

    const nuwax_audio_board_caps_t *caps = nuwax_audio_board_get_caps();
    const bool software_ref_enabled =
        CONFIG_NUWAX_P3_SOFTWARE_REF_ENABLE && CONFIG_NUWAX_SPEAKER_ENABLE;
    const bool hardware_ref_candidate =
        CONFIG_NUWAX_VOCAT_SLOT2_AEC_REF_ENABLE &&
        CONFIG_NUWAX_VOCAT_MIC3_REFERENCE_PRODUCTION_PROVEN &&
        caps->has_hardware_ref_candidate;
    const nuwax_hardware_ref_health_t hardware_ref_health =
        nuwax_audio_board_get_hardware_ref_health();
    const bool hardware_ref_enabled =
        hardware_ref_candidate &&
        hardware_ref_health == NUWAX_HARDWARE_REF_HEALTH_PASSED;
    const bool runtime_ref_available = software_ref_enabled || hardware_ref_enabled;
    const bool live_aec_enabled =
        CONFIG_NUWAX_LIVE_AEC_ENABLE && software_ref_enabled;
    const nuwax_afe_input_format_t runtime_afe_input_format =
        runtime_ref_available ? caps->input_format : NUWAX_AFE_INPUT_MM;

    if (!cJSON_AddStringToObject(root, "type", "hello")) goto done;
    if (!cJSON_AddNumberToObject(root, "version", 1)) goto done;
    if (!cJSON_AddStringToObject(root, "device_id", nuwax_device_uid_string()[0]
                                                     ? nuwax_device_uid_string()
                                                     : CONFIG_NUWAX_DEVICE_ID)) goto done;
    if (!cJSON_AddStringToObject(root, "device_secret", device_secret)) goto done;
    if (!cJSON_AddStringToObject(root, "transport", "websocket")) goto done;
    if (!cJSON_AddBoolToObject(root, "has_sd_card", has_sd_card)) goto done;
    if (!cJSON_AddBoolToObject(features, "audio_upload", true)) goto done;
    /* Device-side meeting mode is negotiated independently from normal
     * conversation capture.  This advertises capability only; it does not
     * mean that a meeting session is currently active. */
    if (!cJSON_AddBoolToObject(features, "meeting_mode_v1", true)) goto done;
    if (!cJSON_AddBoolToObject(features, "monitoring_mode_v1", true)) goto done;
    if (!cJSON_AddBoolToObject(features, "monitoring_pause_resume_v1", true)) goto done;
    if (!cJSON_AddBoolToObject(features, "remote_ble_reprovision", true)) goto done;
    if (!cJSON_AddBoolToObject(features, "volume_changed_v1", true)) goto done;
#if CONFIG_NUWAX_WAKE_WORD_ENABLE && CONFIG_NUWAX_WAKE_WORD_BACKEND_MULTINET
    if (!cJSON_AddBoolToObject(features, "remote_wake_word_config", true)) goto done;
#else
    if (!cJSON_AddBoolToObject(features, "remote_wake_word_config", false)) goto done;
#endif
#if CONFIG_NUWAX_OTA_ENABLE
    if (!cJSON_AddBoolToObject(features, "ota_v1", true)) goto done;
#endif
#if CONFIG_NUWAX_SPEAKER_ENABLE
    if (!cJSON_AddBoolToObject(features, "audio_playback", true)) goto done;
    if (!cJSON_AddBoolToObject(features, "notify_playback_v1", true)) goto done;
#else
    if (!cJSON_AddBoolToObject(features, "audio_playback", false)) goto done;
    if (!cJSON_AddBoolToObject(features, "notify_playback_v1", false)) goto done;
#endif
#if defined(CONFIG_NUWAX_MUSIC_PLAYBACK_ENABLE) && \
    CONFIG_NUWAX_MUSIC_PLAYBACK_ENABLE && \
    defined(CONFIG_NUWAX_MUSIC_MP3_ENABLE) && \
    CONFIG_NUWAX_MUSIC_MP3_ENABLE
    if (!cJSON_AddBoolToObject(features, "music_playback_mp3", true)) goto done;
#else
    if (!cJSON_AddBoolToObject(features, "music_playback_mp3", false)) goto done;
#endif
    if (!cJSON_AddBoolToObject(features, "mcp", true)) goto done;
    if (!cJSON_AddItemToObject(root, "features", features)) goto done;
    features = NULL; /* Ownership transferred to root. */

#if CONFIG_NUWAX_OTA_ENABLE
    firmware = cJSON_CreateObject();
    if (!firmware) goto done;
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    bool pending_verify = false;
    uint64_t release_sequence = NUWAX_OTA_FIRMWARE_RELEASE_SEQUENCE;
    (void)nuwax_ota_main_running_image_pending_verify(&pending_verify);
    (void)nuwax_ota_main_get_release_sequence(&release_sequence);
    nuwax_ota_running_identity_t running_identity = {0};
    const esp_err_t identity_err =
        nuwax_ota_main_peek_running_identity(&running_identity);
    if (identity_err == ESP_OK) {
        pending_verify = running_identity.pending_verify;
        release_sequence = running_identity.release_sequence;
    }
    if (!cJSON_AddStringToObject(firmware, "project_name",
                            app ? app->project_name : "NuwaxAIPartner")) goto done;
    if (!cJSON_AddStringToObject(firmware, "version", app ? app->version : "unknown")) goto done;
    if (!cJSON_AddNumberToObject(firmware, "release_sequence", (double)release_sequence)) goto done;
    if (!cJSON_AddStringToObject(firmware, "target_board", NUWAX_OTA_TARGET_BOARD)) goto done;
    if (!cJSON_AddStringToObject(firmware, "target_chip", NUWAX_OTA_TARGET_CHIP)) goto done;
    if (!cJSON_AddNumberToObject(firmware, "partition_schema", NUWAX_OTA_PARTITION_SCHEMA)) goto done;
    if (!cJSON_AddStringToObject(firmware, "build_flavor", NUWAX_OTA_BUILD_FLAVOR)) goto done;
    if (!cJSON_AddStringToObject(firmware, "running_partition",
                            running ? running->label : "unknown")) goto done;
    if (!cJSON_AddStringToObject(firmware, "ota_state",
                            pending_verify ? "pending_verify" : "valid")) goto done;
    if (identity_err == ESP_OK) {
        if (!cJSON_AddStringToObject(firmware, "image_sha256",
                                running_identity.image_sha256)) goto done;
    }
    if (!cJSON_AddItemToObject(root, "firmware", firmware)) goto done;
    firmware = NULL; /* Ownership transferred to root. */
#endif

#if CONFIG_NUWAX_VOICE_UPLINK_CODEC_OPUS
    if (!cJSON_AddStringToObject(audio, "input_format", "opus")) goto done;
    if (!cJSON_AddStringToObject(audio, "codec", "opus")) goto done;
    if (!cJSON_AddBoolToObject(audio, "opus", true)) goto done;
#else
    if (!cJSON_AddStringToObject(audio, "input_format", "pcm16")) goto done;
    if (!cJSON_AddStringToObject(audio, "codec", "pcm16")) goto done;
    if (!cJSON_AddBoolToObject(audio, "opus", false)) goto done;
#endif
    if (!cJSON_AddStringToObject(audio, "output_format", "pcm16")) goto done;
    if (!cJSON_AddNumberToObject(audio, "sample_rate", CONFIG_NUWAX_VOICE_SAMPLE_RATE)) goto done;
    if (!cJSON_AddNumberToObject(audio, "output_sample_rate", CONFIG_NUWAX_VOICE_OUTPUT_SAMPLE_RATE)) goto done;
    if (!cJSON_AddNumberToObject(audio, "channels", CONFIG_NUWAX_VOICE_CHANNELS)) goto done;
    /* Cloud media is mono even though the ES7210 hardware capture uses four
     * TDM slots. read_pcm16() selects the primary slot before framing. */
    if (!cJSON_AddNumberToObject(audio, "mic_channels", CONFIG_NUWAX_VOICE_CHANNELS)) goto done;
    if (!cJSON_AddNumberToObject(audio, "frame_duration_ms", CONFIG_NUWAX_VOICE_FRAME_DURATION_MS)) goto done;
#if CONFIG_NUWAX_SPEAKER_ENABLE
    if (!cJSON_AddBoolToObject(audio, "speaker", true)) goto done;
#else
    if (!cJSON_AddBoolToObject(audio, "speaker", false)) goto done;
#endif
    if (!cJSON_AddBoolToObject(audio, "wake_word", CONFIG_NUWAX_WAKE_WORD_ENABLE ? true : false)) goto done;
    if (!cJSON_AddBoolToObject(audio, "vad", CONFIG_NUWAX_VOICE_LOCAL_VAD_ENDPOINT_ENABLE ? true : false)) goto done;
    if (!cJSON_AddBoolToObject(audio, "full_duplex", caps->full_duplex)) goto done;
    if (!cJSON_AddBoolToObject(audio, "shared_i2s", caps->shared_i2s)) goto done;
    if (!cJSON_AddStringToObject(audio, "hardware_profile", caps->hardware_profile ? caps->hardware_profile : "legacy")) goto done;

    /* A routed MIC3 is not advertised as usable until its runtime test passes. */
    if (!cJSON_AddBoolToObject(audio, "aec", live_aec_enabled)) goto done;
    if (!cJSON_AddStringToObject(audio,
                           "aec_reference",
                           aec_reference_kind_str(hardware_ref_enabled, software_ref_enabled))) goto done;
    if (!cJSON_AddBoolToObject(audio, "has_ref", runtime_ref_available)) goto done;
    if (!cJSON_AddBoolToObject(audio, "software_ref", software_ref_enabled)) goto done;
    if (!cJSON_AddBoolToObject(audio, "hardware_ref_candidate", hardware_ref_candidate)) goto done;
    if (!cJSON_AddStringToObject(audio, "hardware_ref_health",
                           hardware_ref_health_str(hardware_ref_health))) goto done;
    if (!cJSON_AddBoolToObject(audio, "hardware_ref", hardware_ref_enabled)) goto done;
    if (!cJSON_AddNumberToObject(audio, "ref_slot", hardware_ref_enabled ? caps->ref_slot : -1)) goto done;
    if (!cJSON_AddStringToObject(audio, "afe_input_format",
                           afe_input_format_str(runtime_afe_input_format))) goto done;

    if (!cJSON_AddItemToObject(root, "audio_params", audio)) goto done;
    audio = NULL; /* Ownership transferred to root. */

    err = send_sensitive_json_object(ws, root);
done:
    cJSON_Delete(features);
    cJSON_Delete(audio);
    cJSON_Delete(firmware);
    cJSON *secret_item = cJSON_GetObjectItemCaseSensitive(root, "device_secret");
    if (cJSON_IsString(secret_item) && secret_item->valuestring) {
        secure_zero_memory(secret_item->valuestring,
                           strlen(secret_item->valuestring));
    }
    cJSON_Delete(root);
    return err;
}

nuwax_cloud_message_kind_t nuwax_session_classify_cloud_message(
    const char *payload,
    int len)
{
    const char *expected_device_id = nuwax_device_uid_string()[0]
                                          ? nuwax_device_uid_string()
                                          : CONFIG_NUWAX_DEVICE_ID;
    return nuwax_cloud_classify_message(payload, len, expected_device_id);
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

esp_err_t nuwax_session_send_ping_until_if_epoch(nuwax_ws_client_t *ws,
                                                 int64_t now_ms,
                                                 int64_t deadline_us,
                                                 uint32_t expected_epoch) {
    if (!ws || deadline_us <= 0 || expected_epoch == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Same JSON fields and integer value; avoid heap/JSON work inside the
     * short admission deadline. Maximum signed 64-bit time fits this buffer. */
    char json[64];
    const int length = snprintf(json, sizeof(json),
        "{\"type\":\"ping\",\"now_ms\":%lld}", (long long)now_ms);
    if (length < 0 || (size_t)length >= sizeof(json)) return ESP_ERR_INVALID_SIZE;
    return nuwax_ws_send_keepalive_until_if_epoch(ws, json, deadline_us, expected_epoch);
}

esp_err_t nuwax_session_send_listen(nuwax_ws_client_t *ws, const char *state, const char *mode) {
    return nuwax_session_send_listen_with_audio_session(ws, state, mode, NULL, NULL);
}

esp_err_t nuwax_session_send_listen_with_audio_session(nuwax_ws_client_t *ws,
                                                       const char *state,
                                                       const char *mode,
                                                       const char *audio_session_id,
                                                       const char *request_id) {
    if (suppress_listen_stop(state, mode, audio_session_id)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
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

esp_err_t nuwax_session_send_listen_with_audio_session_until(
    nuwax_ws_client_t *ws,
    const char *state,
    const char *mode,
    const char *audio_session_id,
    const char *request_id,
    int64_t deadline_us) {
    if (!ws || !state || !mode || deadline_us <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (suppress_listen_stop(state, mode, audio_session_id)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
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
    esp_err_t err = send_json_object_until(ws, root, deadline_us);
    cJSON_Delete(root);
    return err;
}

esp_err_t nuwax_session_send_listen_with_audio_session_until_if_epoch(
    nuwax_ws_client_t *ws,
    const char *state,
    const char *mode,
    const char *audio_session_id,
    const char *request_id,
    int64_t deadline_us,
    uint32_t expected_epoch) {
    if (!ws || !state || !mode || deadline_us <= 0 ||
        expected_epoch == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (suppress_listen_stop(state, mode, audio_session_id)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
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
    esp_err_t err = send_json_object_until_if_epoch(
        ws, root, deadline_us, expected_epoch);
    cJSON_Delete(root);
    return err;
}

esp_err_t nuwax_session_send_listen_stop_retry_if_epoch(
    nuwax_ws_client_t *ws,
    const char *mode,
    const char *audio_session_id,
    const char *request_id,
    uint32_t attempt_timeout_ms,
    uint32_t expected_epoch) {
    if (!ws || !mode || attempt_timeout_ms == 0U ||
        expected_epoch == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_FAIL;
    for (uint32_t attempt = 1U;
         attempt <= NUWAX_SESSION_TERMINAL_RETRY_ATTEMPTS;
         ++attempt) {
        const int64_t deadline_us =
            esp_timer_get_time() + (int64_t)attempt_timeout_ms * 1000LL;
        err = nuwax_session_send_listen_with_audio_session_until_if_epoch(
            ws,
            "stop",
            mode,
            audio_session_id,
            request_id,
            deadline_us,
            expected_epoch);
        const bool safely_unsent_timeout =
            nuwax_ws_safe_unsent_retry_allowed(
                err == ESP_ERR_TIMEOUT,
                nuwax_ws_is_connected(ws),
                nuwax_ws_connection_epoch(ws) == expected_epoch,
                nuwax_ws_heartbeat_healthy_if_epoch(ws, expected_epoch));
        if (!safely_unsent_timeout ||
            attempt >= NUWAX_SESSION_TERMINAL_RETRY_ATTEMPTS) {
            break;
        }
        ESP_LOGW(TAG,
                 "listen stop safely unsent; retrying mode=%s audio_session=%s epoch=%lu attempt=%lu/%u",
                 mode,
                 audio_session_id && audio_session_id[0]
                     ? audio_session_id
                     : "-",
                 (unsigned long)expected_epoch,
                 (unsigned long)(attempt + 1U),
                 (unsigned)NUWAX_SESSION_TERMINAL_RETRY_ATTEMPTS);
        vTaskDelay(pdMS_TO_TICKS(NUWAX_SESSION_TERMINAL_RETRY_DELAY_MS));
    }
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

esp_err_t nuwax_session_send_text_request_with_timeout_if_epoch(
    nuwax_ws_client_t *ws,
    const char *request_id,
    const char *text,
    uint32_t timeout_ms,
    uint32_t expected_epoch) {
    if (!ws || timeout_ms == 0U || expected_epoch == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "type", "text_request");
    cJSON_AddStringToObject(root, "request_id", request_id ? request_id : "");
    cJSON_AddStringToObject(root, "text", text ? text : "");
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t err = nuwax_ws_send_text_with_timeout_if_epoch(
        ws, json, timeout_ms, expected_epoch);
    free(json);
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

esp_err_t nuwax_session_send_cancel_turn_until_if_epoch(
    nuwax_ws_client_t *ws,
    const char *request_id,
    const char *turn_id,
    const char *reason,
    int64_t deadline_us,
    uint32_t expected_epoch) {
    if (!ws || deadline_us <= 0 || expected_epoch == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "type", "cancel_turn");
    cJSON_AddStringToObject(root, "request_id", request_id ? request_id : "");
    cJSON_AddStringToObject(root, "turn_id", turn_id ? turn_id : "");
    cJSON_AddStringToObject(root, "reason", reason ? reason : "device_cancel");
    esp_err_t err = send_json_object_until_if_epoch(
        ws, root, deadline_us, expected_epoch);
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

esp_err_t nuwax_session_send_barge_in_until(
    nuwax_ws_client_t *ws,
    const char *state,
    const char *event_id,
    const char *parent_turn_id,
    const char *playback_id,
    const char *audio_session_id,
    uint32_t onset_sequence,
    uint32_t current_sequence,
    uint32_t confidence_q15,
    int64_t deadline_us) {
    if (!ws || !state || !event_id || event_id[0] == '\0' ||
        deadline_us <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "type", "barge_in");
    cJSON_AddStringToObject(root, "state", state);
    cJSON_AddStringToObject(root, "event_id", event_id);
    cJSON_AddStringToObject(root, "parent_turn_id",
                            parent_turn_id ? parent_turn_id : "");
    cJSON_AddStringToObject(root, "playback_id",
                            playback_id ? playback_id : "");
    cJSON_AddStringToObject(root, "audio_session_id",
                            audio_session_id ? audio_session_id : "");
    cJSON_AddNumberToObject(root, "onset_sequence", onset_sequence);
    cJSON_AddNumberToObject(root, "current_sequence", current_sequence);
    if (strcmp(state, "confirmed") == 0 || strcmp(state, "confirm") == 0) {
        cJSON_AddNumberToObject(root, "confirmed_sequence", current_sequence);
    }
    cJSON_AddNumberToObject(root, "confidence_q15",
                            confidence_q15 > 32768U ? 32768U : confidence_q15);
    esp_err_t err = send_json_object_until(ws, root, deadline_us);
    cJSON_Delete(root);
    return err;
}

esp_err_t nuwax_session_send_barge_in_until_if_epoch(
    nuwax_ws_client_t *ws,
    const char *state,
    const char *event_id,
    const char *parent_turn_id,
    const char *playback_id,
    const char *audio_session_id,
    uint32_t onset_sequence,
    uint32_t current_sequence,
    uint32_t confidence_q15,
    int64_t deadline_us,
    uint32_t expected_epoch) {
    if (!ws || !state || !event_id || event_id[0] == '\0' ||
        deadline_us <= 0 || expected_epoch == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "type", "barge_in");
    cJSON_AddStringToObject(root, "state", state);
    cJSON_AddStringToObject(root, "event_id", event_id);
    cJSON_AddStringToObject(root, "parent_turn_id",
                            parent_turn_id ? parent_turn_id : "");
    cJSON_AddStringToObject(root, "playback_id",
                            playback_id ? playback_id : "");
    cJSON_AddStringToObject(root, "audio_session_id",
                            audio_session_id ? audio_session_id : "");
    cJSON_AddNumberToObject(root, "onset_sequence", onset_sequence);
    cJSON_AddNumberToObject(root, "current_sequence", current_sequence);
    if (strcmp(state, "confirmed") == 0 || strcmp(state, "confirm") == 0) {
        cJSON_AddNumberToObject(root, "confirmed_sequence", current_sequence);
    }
    cJSON_AddNumberToObject(root, "confidence_q15",
                            confidence_q15 > 32768U ? 32768U : confidence_q15);
    esp_err_t err = send_json_object_until_if_epoch(
        ws, root, deadline_us, expected_epoch);
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

static esp_err_t send_wifi_scan_result(nuwax_ws_client_t *ws,
                                       const char *request_id,
                                       bool ok,
                                       const char *message,
                                       const char ssids[][33],
                                       const int8_t *rssis,
                                       const uint8_t *authmodes,
                                       int count,
                                       int64_t deadline_us,
                                       uint32_t expected_epoch) {
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
    if (request_id && request_id[0] != '\0') {
        cJSON_AddStringToObject(root, "request_id", request_id);
    }
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

    esp_err_t err = expected_epoch != 0U
                        ? send_json_object_until_if_epoch(
                              ws, root, deadline_us, expected_epoch)
                        : send_json_object(ws, root);
    cJSON_Delete(root);
    return err;
}

esp_err_t nuwax_session_send_wifi_scan_result(nuwax_ws_client_t *ws,
                                              const char *request_id,
                                              bool ok,
                                              const char *message,
                                              const char ssids[][33],
                                              const int8_t *rssis,
                                              const uint8_t *authmodes,
                                              int count) {
    return send_wifi_scan_result(ws,
                                 request_id,
                                 ok,
                                 message,
                                 ssids,
                                 rssis,
                                 authmodes,
                                 count,
                                 0,
                                 0U);
}

esp_err_t nuwax_session_send_wifi_scan_result_until_if_epoch(
    nuwax_ws_client_t *ws,
    const char *request_id,
    bool ok,
    const char *message,
    const char ssids[][33],
    const int8_t *rssis,
    const uint8_t *authmodes,
    int count,
    int64_t deadline_us,
    uint32_t expected_epoch) {
    if (!ws || deadline_us <= 0 || expected_epoch == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    return send_wifi_scan_result(ws,
                                 request_id,
                                 ok,
                                 message,
                                 ssids,
                                 rssis,
                                 authmodes,
                                 count,
                                 deadline_us,
                                 expected_epoch);
}

esp_err_t nuwax_session_send_wifi_status(nuwax_ws_client_t *ws,
                                         const char *request_id,
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
    if (request_id && request_id[0] != '\0') {
        cJSON_AddStringToObject(root, "request_id", request_id);
    }
    cJSON_AddStringToObject(root, "phase", phase ? phase : "");
    cJSON_AddStringToObject(root, "ssid", ssid ? ssid : "");
    cJSON_AddStringToObject(root, "ip", ip ? ip : "");
    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddStringToObject(root, "message", message ? message : "");
    esp_err_t err = send_json_object(ws, root);
    cJSON_Delete(root);
    return err;
}

esp_err_t nuwax_session_send_volume_changed_until_if_epoch(
    nuwax_ws_client_t *ws,
    const char *event_id,
    int volume_percent,
    const char *source,
    int64_t deadline_us,
    uint32_t expected_epoch) {
    if (!ws || !event_id || strnlen(event_id, 40U) >= 40U ||
        !json_ascii_matches(event_id, request_id_char_allowed) ||
        volume_percent < 0 || volume_percent > 100 || !source ||
        (strcmp(source, "device_ui") != 0 &&
         strcmp(source, "device_sync") != 0) ||
        deadline_us <= 0 || expected_epoch == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "type", "volume_changed");
    cJSON_AddStringToObject(root, "event_id", event_id);
    cJSON_AddNumberToObject(root, "volume_percent", volume_percent);
    cJSON_AddStringToObject(root, "source", source);
    const esp_err_t err = send_json_object_until_if_epoch(
        ws, root, deadline_us, expected_epoch);
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
    if (request_id && request_id[0] != '\0') {
        cJSON_AddStringToObject(root, "request_id", request_id);
    }
    cJSON_AddStringToObject(root, "command", command ? command : "");
    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddStringToObject(root, "message", message ? message : "");
    esp_err_t err = send_json_object(ws, root);
    cJSON_Delete(root);
    return err;
}

esp_err_t nuwax_session_send_command_ack_until_if_epoch(
    nuwax_ws_client_t *ws,
    const char *request_id,
    const char *command,
    bool ok,
    const char *message,
    int64_t deadline_us,
    uint32_t expected_epoch) {
    if (!ws || deadline_us <= 0 || expected_epoch == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "type", "device_command_ack");
    if (request_id && request_id[0] != '\0') {
        cJSON_AddStringToObject(root, "request_id", request_id);
    }
    cJSON_AddStringToObject(root, "command", command ? command : "");
    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddStringToObject(root, "message", message ? message : "");
    esp_err_t err = send_json_object_until_if_epoch(
        ws, root, deadline_us, expected_epoch);
    cJSON_Delete(root);
    return err;
}

esp_err_t nuwax_session_send_music_playback_status_until_if_epoch(
    nuwax_ws_client_t *ws,
    const char *request_id,
    const char *state,
    const char *reason,
    int64_t deadline_us,
    uint32_t expected_epoch) {
    if (!ws || !request_id || request_id[0] == '\0' || !state ||
        state[0] == '\0' || deadline_us <= 0 || expected_epoch == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(state, "playing") != 0 &&
        strcmp(state, "completed") != 0 &&
        strcmp(state, "failed") != 0 &&
        strcmp(state, "cancelled") != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "type", "music_playback_status");
    cJSON_AddStringToObject(root, "request_id", request_id);
    cJSON_AddStringToObject(root, "state", state);
    if (reason && reason[0] != '\0') {
        cJSON_AddStringToObject(root, "reason", reason);
    }
    const esp_err_t err = send_json_object_until_if_epoch(
        ws, root, deadline_us, expected_epoch);
    cJSON_Delete(root);
    return err;
}

esp_err_t nuwax_session_send_notify_playback_ack_until_if_epoch(
    nuwax_ws_client_t *ws,
    const char *ack_type,
    const char *request_id,
    const char *notify_id,
    const char *playback_id,
    bool ok,
    const char *message,
    int64_t deadline_us,
    uint32_t expected_epoch) {
    if (!ws || !ack_type ||
        (strcmp(ack_type, "notify_playback_begin_ack") != 0 &&
         strcmp(ack_type, "notify_playback_end_ack") != 0) ||
        !request_id || !request_id[0] || !notify_id || !notify_id[0] ||
        !playback_id || !playback_id[0] || deadline_us <= 0 ||
        expected_epoch == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "type", ack_type);
    cJSON_AddStringToObject(root, "request_id", request_id);
    cJSON_AddStringToObject(root, "notify_id", notify_id);
    cJSON_AddStringToObject(root, "playback_id", playback_id);
    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddStringToObject(root, "message", message ? message : "");
    const esp_err_t err = send_json_object_until_if_epoch(
        ws, root, deadline_us, expected_epoch);
    cJSON_Delete(root);
    return err;
}

esp_err_t nuwax_session_send_notify_playback_cancelled_until_if_epoch(
    nuwax_ws_client_t *ws,
    const char *request_id,
    const char *notify_id,
    const char *playback_id,
    const char *reason,
    int64_t deadline_us,
    uint32_t expected_epoch) {
    if (!ws || !request_id || !request_id[0] || !notify_id ||
        !notify_id[0] || !playback_id || !playback_id[0] ||
        deadline_us <= 0 || expected_epoch == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "type", "notify_playback_cancelled");
    cJSON_AddStringToObject(root, "request_id", request_id);
    cJSON_AddStringToObject(root, "notify_id", notify_id);
    cJSON_AddStringToObject(root, "playback_id", playback_id);
    cJSON_AddStringToObject(root, "reason", reason ? reason : "cancelled");
    const esp_err_t err = send_json_object_until_if_epoch(
        ws, root, deadline_us, expected_epoch);
    cJSON_Delete(root);
    return err;
}

void nuwax_session_handle_text(const char *payload, int len, nuwax_session_command_t *out_command) {
    if (out_command) {
        memset(out_command, 0, sizeof(*out_command));
    }
    ESP_LOGD(TAG, "RX text frame len=%d", len);

    cJSON *root = nuwax_json_parse_with_length(payload, len);
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

    if (cJSON_IsString(type) &&
        (strcmp(type->valuestring, "error") == 0 ||
         strcmp(type->valuestring, "hello") == 0)) {
        const cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
        const cJSON *authenticated =
            cJSON_GetObjectItemCaseSensitive(root, "authenticated");
        const bool rejected_hello = strcmp(type->valuestring, "hello") == 0 &&
                                    (!cJSON_IsTrue(ok) ||
                                     (authenticated != NULL &&
                                      !cJSON_IsTrue(authenticated)));
        if (strcmp(type->valuestring, "error") == 0 || rejected_hello) {
            const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
            const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
            if (!cJSON_IsString(error)) {
                error = cJSON_GetObjectItemCaseSensitive(root, "message");
            }
            ESP_LOGE(TAG, "Cloud application rejection type=%s code=%s",
                     type->valuestring,
                     cJSON_IsString(code) ? code->valuestring : "-");
            if (cJSON_IsString(error)) {
                log_text_preview("Cloud rejection detail", error->valuestring);
            }
        }
    }

    if (out_command && cJSON_IsString(type) && strcmp(type->valuestring, "pong") == 0) {
        out_command->has_pong = true;
        const cJSON *server = cJSON_GetObjectItemCaseSensitive(root, "server_ms");
        if (cJSON_IsNumber(server))
            (void)nuwax_cloud_time_number(server->valuedouble, &out_command->pong_server_ms);
    }

    if (out_command &&
        ((cJSON_IsString(type) &&
          strcmp(type->valuestring, "device_command") == 0) ||
         is_unenveloped_set_wake_word(root))) {
        cJSON *command = cJSON_GetObjectItem(root, "command");
        cJSON *mode = cJSON_GetObjectItem(root, "mode");
        cJSON *request_id = cJSON_GetObjectItem(root, "request_id");
        cJSON *audio_session_id = cJSON_GetObjectItem(root, "audio_session_id");
        if (!audio_session_id) {
            audio_session_id = cJSON_GetObjectItem(root, "audioSessionId");
        }
        cJSON *turn_id = cJSON_GetObjectItem(root, "turn_id");
        cJSON *notify_id = cJSON_GetObjectItem(root, "notify_id");
        cJSON *playback_id = cJSON_GetObjectItem(root, "playback_id");
        cJSON *barge_event_id = cJSON_GetObjectItem(root, "barge_event_id");
        cJSON *ssid = cJSON_GetObjectItem(root, "ssid");
        cJSON *password = cJSON_GetObjectItem(root, "password");
        cJSON *wake_word = cJSON_GetObjectItem(root, "wake_word");
        if (!wake_word) {
            wake_word = cJSON_GetObjectItem(root, "wakeWord");
        }
        cJSON *state = cJSON_GetObjectItem(root, "state");
        cJSON *emotion = cJSON_GetObjectItem(root, "emotion");
        if (!cJSON_IsString(emotion) || emotion->valuestring[0] == '\0') {
            emotion = cJSON_GetObjectItem(root, "assistant_emotion");
        }
        if (!cJSON_IsString(emotion) || emotion->valuestring[0] == '\0') {
            emotion = cJSON_GetObjectItem(root, "assistantEmotion");
        }
        cJSON *title = cJSON_GetObjectItem(root, "title");
        cJSON *lines = cJSON_GetObjectItem(root, "lines");
        cJSON *text = cJSON_GetObjectItem(root, "text");
        cJSON *url = cJSON_GetObjectItem(root, "url");
        cJSON *format = cJSON_GetObjectItem(root, "format");
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
        cJSON *wake_word_persist = cJSON_GetObjectItem(root, "persist");
        if (!wake_word_persist) {
            wake_word_persist = cJSON_GetObjectItem(root, "wake_word_persist");
        }
        if (!wake_word_persist) {
            wake_word_persist = cJSON_GetObjectItem(root, "wakeWordPersist");
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
        utf8_copy_trunc(out_command->notify_id,
                        sizeof(out_command->notify_id),
                        cJSON_IsString(notify_id) ? notify_id->valuestring : "");
        utf8_copy_trunc(out_command->playback_id,
                        sizeof(out_command->playback_id),
                        cJSON_IsString(playback_id) ? playback_id->valuestring : "");
        utf8_copy_trunc(out_command->url,
                        sizeof(out_command->url),
                        cJSON_IsString(url) ? url->valuestring : "");
        utf8_copy_trunc(out_command->format,
                        sizeof(out_command->format),
                        cJSON_IsString(format) ? format->valuestring : "");
        utf8_copy_trunc(out_command->barge_event_id,
                        sizeof(out_command->barge_event_id),
                        cJSON_IsString(barge_event_id) ? barge_event_id->valuestring : "");
        utf8_copy_trunc(out_command->ssid, sizeof(out_command->ssid), cJSON_IsString(ssid) ? ssid->valuestring : "");
        utf8_copy_trunc(out_command->password,
                        sizeof(out_command->password),
                        cJSON_IsString(password) ? password->valuestring : "");
        if (cJSON_IsString(wake_word)) {
            if (strlen(wake_word->valuestring) <
                sizeof(out_command->wake_word_config.variants[0])) {
                out_command->wake_word_config.count = 1U;
                utf8_copy_trunc(out_command->wake_word_config.variants[0],
                                sizeof(out_command->wake_word_config.variants[0]),
                                wake_word->valuestring);
            } else {
                out_command->wake_word_invalid = true;
            }
        } else if (cJSON_IsArray(wake_word)) {
            out_command->wake_word_is_array = true;
            const int wake_word_count = cJSON_GetArraySize(wake_word);
            if (wake_word_count < 1) {
                out_command->wake_word_invalid = true;
            } else {
                const int retained_count =
                    wake_word_count > (int)NUWAX_WAKE_WORD_MAX_VARIANTS
                        ? (int)NUWAX_WAKE_WORD_MAX_VARIANTS
                        : wake_word_count;
                out_command->wake_word_truncated =
                    retained_count != wake_word_count;
                out_command->wake_word_config.count =
                    (uint8_t)retained_count;
                for (int i = 0; i < retained_count; ++i) {
                    const cJSON *variant = cJSON_GetArrayItem(wake_word, i);
                    if (!cJSON_IsString(variant) ||
                        strlen(variant->valuestring) >
                            NUWAX_WAKE_WORD_MAX_COMMAND_LEN) {
                        out_command->wake_word_invalid = true;
                        out_command->wake_word_config.count = 0U;
                        break;
                    }
                    utf8_copy_trunc(
                        out_command->wake_word_config.variants[i],
                        sizeof(out_command->wake_word_config.variants[i]),
                        variant->valuestring);
                }
            }
        } else if (wake_word != NULL) {
            out_command->wake_word_invalid = true;
        }
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
        if (cJSON_IsBool(wake_word_persist)) {
            out_command->has_wake_word_persist = true;
            out_command->wake_word_persist = cJSON_IsTrue(wake_word_persist);
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
        if (strcmp(out_command->command, "set_wake_word") == 0) {
            ESP_LOGI(TAG,
                     "Wake word command format=%s count=%u value=%s truncated=%d invalid=%d persist=%d",
                     out_command->wake_word_is_array ? "array" : "string",
                     (unsigned)out_command->wake_word_config.count,
                     out_command->wake_word_is_array
                         ? (out_command->wake_word_config.count > 0U
                                ? out_command->wake_word_config.variants[0]
                                : "-")
                          : (out_command->wake_word_config.count > 0U
                                 ? out_command->wake_word_config.variants[0]
                                 : "-"),
                     out_command->wake_word_truncated ? 1 : 0,
                     out_command->wake_word_invalid ? 1 : 0,
                     out_command->has_wake_word_persist
                         ? (out_command->wake_word_persist ? 1 : 0)
                         : 1);
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
    if (out_command && cJSON_IsString(type) &&
        strcmp(type->valuestring, "audio_stats") == 0 &&
        cJSON_IsNumber(frames)) {
        cJSON *audio_session = cJSON_GetObjectItem(root, "audio_session");
        cJSON *audio_session_id = cJSON_IsObject(audio_session)
                                      ? cJSON_GetObjectItem(audio_session,
                                                            "audio_session_id")
                                      : NULL;
        if (cJSON_IsString(audio_session_id) &&
            audio_session_id->valuestring[0] != '\0') {
            out_command->has_event = true;
            out_command->event = NUWAX_SESSION_EVENT_AUDIO_PROGRESS;
            out_command->sequence = frames->valueint;
            utf8_copy_trunc(out_command->audio_session_id,
                            sizeof(out_command->audio_session_id),
                            audio_session_id->valuestring);
        }
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
        if (!cJSON_IsString(emotion) || emotion->valuestring[0] == '\0') {
            emotion = cJSON_GetObjectItem(root, "assistant_emotion");
        }
        if (!cJSON_IsString(emotion) || emotion->valuestring[0] == '\0') {
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

    if (cJSON_IsString(type) &&
        (strcmp(type->valuestring, "notify_playback_begin") == 0 ||
         strcmp(type->valuestring, "notify_playback_end") == 0)) {
        const bool is_begin =
            strcmp(type->valuestring, "notify_playback_begin") == 0;
        cJSON *request_id = cJSON_GetObjectItem(root, "request_id");
        cJSON *notify_id = cJSON_GetObjectItem(root, "notify_id");
        cJSON *playback_id = cJSON_GetObjectItem(root, "playback_id");
        cJSON *codec = cJSON_GetObjectItem(root, "codec");
        cJSON *sample_rate = cJSON_GetObjectItem(root, "sample_rate");
        cJSON *channels = cJSON_GetObjectItem(root, "channels");
        cJSON *frame_duration_ms =
            cJSON_GetObjectItem(root, "frame_duration_ms");
        cJSON *frames_count = cJSON_GetObjectItem(root, "frames");
        cJSON *bytes = cJSON_GetObjectItem(root, "bytes");
        cJSON *reason = cJSON_GetObjectItem(root, "reason");
        if (out_command) {
            out_command->has_event = true;
            out_command->event =
                is_begin ? NUWAX_SESSION_EVENT_NOTIFY_PLAYBACK_BEGIN
                         : NUWAX_SESSION_EVENT_NOTIFY_PLAYBACK_END;
            out_command->tts_frames =
                cJSON_IsNumber(frames_count) ? frames_count->valueint : 0;
            out_command->playback_bytes =
                cJSON_IsNumber(bytes) ? bytes->valueint : 0;
            out_command->playback_sample_rate =
                cJSON_IsNumber(sample_rate) ? sample_rate->valueint : 16000;
            out_command->playback_channels =
                cJSON_IsNumber(channels) ? channels->valueint : 1;
            out_command->playback_frame_duration_ms =
                cJSON_IsNumber(frame_duration_ms)
                    ? frame_duration_ms->valueint
                    : 0;
            utf8_copy_trunc(out_command->request_id,
                            sizeof(out_command->request_id),
                            cJSON_IsString(request_id)
                                ? request_id->valuestring
                                : "");
            utf8_copy_trunc(out_command->notify_id,
                            sizeof(out_command->notify_id),
                            cJSON_IsString(notify_id)
                                ? notify_id->valuestring
                                : "");
            utf8_copy_trunc(out_command->playback_id,
                            sizeof(out_command->playback_id),
                            cJSON_IsString(playback_id)
                                ? playback_id->valuestring
                                : "");
            utf8_copy_trunc(out_command->playback_codec,
                            sizeof(out_command->playback_codec),
                            cJSON_IsString(codec) ? codec->valuestring
                                                  : "pcm16");
            utf8_copy_trunc(out_command->playback_reason,
                            sizeof(out_command->playback_reason),
                            cJSON_IsString(reason) ? reason->valuestring : "");
        }
        ESP_LOGI(TAG,
                 "Notification playback %s notify=%s playback=%s request=%s frames=%d bytes=%d reason=%s",
                 is_begin ? "begin" : "end",
                 cJSON_IsString(notify_id) ? notify_id->valuestring : "-",
                 cJSON_IsString(playback_id) ? playback_id->valuestring : "-",
                 cJSON_IsString(request_id) ? request_id->valuestring : "-",
                 cJSON_IsNumber(frames_count) ? frames_count->valueint : 0,
                 cJSON_IsNumber(bytes) ? bytes->valueint : 0,
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
    if (cJSON_IsString(type) && strcmp(type->valuestring, "tts_skip") == 0) {
        cJSON *ok = cJSON_GetObjectItem(root, "ok");
        cJSON *request_id = cJSON_GetObjectItem(root, "request_id");
        if (!request_id) {
            request_id = cJSON_GetObjectItem(root, "requestId");
        }
        cJSON *turn_id = cJSON_GetObjectItem(root, "turn_id");
        if (!turn_id) {
            turn_id = cJSON_GetObjectItem(root, "turnId");
        }
        cJSON *reason = cJSON_GetObjectItem(root, "reason");
        if (out_command) {
            out_command->has_event = true;
            out_command->event = NUWAX_SESSION_EVENT_TTS_SKIP;
            out_command->ok = cJSON_IsBool(ok) ? cJSON_IsTrue(ok) : false;
            utf8_copy_trunc(out_command->request_id,
                            sizeof(out_command->request_id),
                            cJSON_IsString(request_id)
                                ? request_id->valuestring
                                : "");
            utf8_copy_trunc(out_command->turn_id,
                            sizeof(out_command->turn_id),
                            cJSON_IsString(turn_id) ? turn_id->valuestring
                                                    : "");
            utf8_copy_trunc(out_command->error,
                            sizeof(out_command->error),
                            cJSON_IsString(reason) ? reason->valuestring : "");
        }
        ESP_LOGI(TAG,
                 "TTS skip ok=%d request=%s turn=%s reason=%s",
                 cJSON_IsBool(ok) ? cJSON_IsTrue(ok) : 0,
                 cJSON_IsString(request_id) && request_id->valuestring[0]
                     ? request_id->valuestring
                     : "-",
                 cJSON_IsString(turn_id) && turn_id->valuestring[0]
                     ? turn_id->valuestring
                     : "-",
                 cJSON_IsString(reason) ? reason->valuestring : "-");
    }
    if (cJSON_IsString(type) && strcmp(type->valuestring, "cancel_turn_ack") == 0) {
        cJSON *ok = cJSON_GetObjectItem(root, "ok");
        cJSON *request_id = cJSON_GetObjectItem(root, "request_id");
        cJSON *turn_id = cJSON_GetObjectItem(root, "turn_id");
        cJSON *playback_id = cJSON_GetObjectItem(root, "playback_id");
        cJSON *new_turn_id = cJSON_GetObjectItem(root, "new_turn_id");
        cJSON *reason = cJSON_GetObjectItem(root, "reason");
        if (out_command) {
            out_command->has_event = true;
            out_command->event = NUWAX_SESSION_EVENT_CANCEL_TURN_ACK;
            out_command->ok = cJSON_IsBool(ok) ? cJSON_IsTrue(ok) : false;
            utf8_copy_trunc(out_command->request_id,
                            sizeof(out_command->request_id),
                            cJSON_IsString(request_id)
                                ? request_id->valuestring
                                : "");
            utf8_copy_trunc(out_command->turn_id,
                            sizeof(out_command->turn_id),
                            cJSON_IsString(turn_id) ? turn_id->valuestring : "");
            utf8_copy_trunc(out_command->playback_id,
                            sizeof(out_command->playback_id),
                            cJSON_IsString(playback_id)
                                ? playback_id->valuestring
                                : "");
            utf8_copy_trunc(out_command->playback_reason,
                            sizeof(out_command->playback_reason),
                            cJSON_IsString(reason) ? reason->valuestring : "");
        }
        ESP_LOGI(TAG,
                 "Cancel turn ack ok=%d request=%s turn=%s playback=%s new_turn=%s reason=%s",
                 cJSON_IsBool(ok) ? cJSON_IsTrue(ok) : 0,
                 cJSON_IsString(request_id) ? request_id->valuestring : "-",
                 cJSON_IsString(turn_id) ? turn_id->valuestring : "-",
                 cJSON_IsString(playback_id) ? playback_id->valuestring : "-",
                 cJSON_IsString(new_turn_id) ? new_turn_id->valuestring : "-",
                 cJSON_IsString(reason) ? reason->valuestring : "");
    }
    cJSON_Delete(root);
}
