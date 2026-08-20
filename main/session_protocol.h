#pragma once

#include "esp_err.h"
#include "cloud_protocol_compat.h"
#include "conversation/wake_word_config.h"
#include "touch_input.h"
#include "ws_transport.h"
#include <stdbool.h>
#include <stdint.h>

#define NUWAX_SESSION_STATUS_LINES 6
#define NUWAX_SESSION_TERMINAL_RETRY_ATTEMPTS 3U
#define NUWAX_SESSION_TERMINAL_RETRY_DELAY_MS 20U

typedef enum {
    NUWAX_SESSION_EVENT_NONE = 0,
    NUWAX_SESSION_EVENT_ASR_PARTIAL,
    NUWAX_SESSION_EVENT_ASR_FINAL,
    NUWAX_SESSION_EVENT_ASR_RESULT,
    NUWAX_SESSION_EVENT_TEXT_RESPONSE,
    NUWAX_SESSION_EVENT_TTS_RESULT,
    NUWAX_SESSION_EVENT_TTS_SKIP,
    NUWAX_SESSION_EVENT_PLAYBACK_BEGIN,
    NUWAX_SESSION_EVENT_PLAYBACK_END,
    NUWAX_SESSION_EVENT_NOTIFY_PLAYBACK_BEGIN,
    NUWAX_SESSION_EVENT_NOTIFY_PLAYBACK_END,
    NUWAX_SESSION_EVENT_CANCEL_TURN_ACK,
    NUWAX_SESSION_EVENT_AUDIO_PROGRESS,
} nuwax_session_event_t;

typedef struct {
    bool has_command;
    bool has_mcp;
    bool has_event;
    bool has_pong;
    nuwax_session_event_t event;
    bool ok;
    bool definite;
    bool tts_pending;
    bool playback_start;
    bool playback_end;
    int sequence;
    int at_ms;
    int tts_frames;
    int playback_sample_rate;
    int playback_channels;
    int playback_frame_duration_ms;
    int playback_bytes;
    bool has_volume_db;
    bool has_volume_percent;
    bool has_volume_persist;
    bool has_wake_word_persist;
    bool wake_word_is_array;
    bool wake_word_truncated;
    bool wake_word_invalid;
    bool has_duration_ms;
    bool has_delay_samples;
    bool has_delay_ms;
    bool has_interrupt_enabled;
    bool has_interrupt_persist;
    bool has_audio_cleaner_enabled;
    bool run_ref_probe;
    float volume_db;
    int volume_percent;
    bool volume_persist;
    bool wake_word_persist;
    uint32_t duration_ms;
    uint32_t delay_samples;
    uint32_t delay_ms;
    bool interrupt_enabled;
    bool interrupt_persist;
    bool audio_cleaner_enabled;
    char command[32];
    char mode[24];
    char request_id[40];
    char audio_session_id[48];
    char barge_event_id[48];
    char turn_id[48];
    char notify_id[48];
    char playback_id[48];
    char playback_codec[16];
    char playback_reason[24];
    char provider[24];
    char ssid[33];
    char password[65];
    char state[24];
    char emotion[24];
    char title[40];
    char lines[NUWAX_SESSION_STATUS_LINES][48];
    char text[512];
    char user_text[256];
    char error[128];
    /* A frame is either device_command or mcp, never both. Reusing the MCP
     * payload storage keeps the 32-deep session queue size unchanged. */
    union {
        char mcp_payload[512];
        nuwax_wake_word_config_t wake_word_config;
    };
} nuwax_session_command_t;

esp_err_t nuwax_session_send_hello(nuwax_ws_client_t *ws,
                                   const char *device_secret);

/* Accepts the strict test-gateway hello ACK and the formal cloud's optional
 * legacy hello/direct-command behavior. */
nuwax_cloud_message_kind_t nuwax_session_classify_cloud_message(
    const char *payload,
    int len);
/* Validates one complete inbound JSON frame before any handler is allowed to
 * refresh liveness or produce side effects. Known fixed-buffer fields are
 * bounded in UTF-8 bytes, not Unicode code points. */
bool nuwax_session_validate_text_frame(const char *payload, int len);
esp_err_t nuwax_session_send_ping(nuwax_ws_client_t *ws, int64_t now_ms);
esp_err_t nuwax_session_send_ping_until_if_epoch(nuwax_ws_client_t *ws,
                                                 int64_t now_ms,
                                                 int64_t deadline_us,
                                                 uint32_t expected_epoch);
esp_err_t nuwax_session_send_listen(nuwax_ws_client_t *ws, const char *state, const char *mode);
esp_err_t nuwax_session_send_listen_with_audio_session(nuwax_ws_client_t *ws,
                                                       const char *state,
                                                       const char *mode,
                                                       const char *audio_session_id,
                                                       const char *request_id);
esp_err_t nuwax_session_send_listen_with_audio_session_until(
    nuwax_ws_client_t *ws,
    const char *state,
    const char *mode,
    const char *audio_session_id,
    const char *request_id,
    int64_t deadline_us);
/* Delayed send variant bound to the connection generation that created the
 * work item. It fails instead of crossing a reconnect boundary. */
esp_err_t nuwax_session_send_listen_with_audio_session_until_if_epoch(
    nuwax_ws_client_t *ws,
    const char *state,
    const char *mode,
    const char *audio_session_id,
    const char *request_id,
    int64_t deadline_us,
    uint32_t expected_epoch);
/* Sends the terminal listen stop with a fresh bounded deadline on each
 * attempt. A retry is allowed only when the previous timeout left the
 * WebSocket connected on the same epoch with a healthy heartbeat. The
 * transport marks partial TCP/TLS frame progress disconnected, so this never
 * duplicates a frame whose delivery became uncertain. */
esp_err_t nuwax_session_send_listen_stop_retry_if_epoch(
    nuwax_ws_client_t *ws,
    const char *mode,
    const char *audio_session_id,
    const char *request_id,
    uint32_t attempt_timeout_ms,
    uint32_t expected_epoch);
esp_err_t nuwax_session_send_text_request(nuwax_ws_client_t *ws, const char *request_id, const char *text);
esp_err_t nuwax_session_send_text_request_with_timeout_if_epoch(
    nuwax_ws_client_t *ws,
    const char *request_id,
    const char *text,
    uint32_t timeout_ms,
    uint32_t expected_epoch);
esp_err_t nuwax_session_send_cancel_turn(nuwax_ws_client_t *ws,
                                         const char *request_id,
                                         const char *turn_id,
                                         const char *reason);
esp_err_t nuwax_session_send_cancel_turn_until_if_epoch(
    nuwax_ws_client_t *ws,
    const char *request_id,
    const char *turn_id,
    const char *reason,
    int64_t deadline_us,
    uint32_t expected_epoch);
esp_err_t nuwax_session_send_barge_in(nuwax_ws_client_t *ws,
                                      const char *state,
                                      const char *event_id,
                                      const char *parent_turn_id,
                                      const char *playback_id,
                                      const char *audio_session_id,
                                      uint32_t onset_sequence,
                                      uint32_t current_sequence,
                                      uint32_t confidence_q15);
esp_err_t nuwax_session_send_barge_in_until(nuwax_ws_client_t *ws,
                                            const char *state,
                                            const char *event_id,
                                            const char *parent_turn_id,
                                            const char *playback_id,
                                            const char *audio_session_id,
                                            uint32_t onset_sequence,
                                            uint32_t current_sequence,
                                            uint32_t confidence_q15,
                                            int64_t deadline_us);
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
    uint32_t expected_epoch);
esp_err_t nuwax_session_send_pc_status_request(nuwax_ws_client_t *ws, const char *request_id);
esp_err_t nuwax_session_send_status_app_request(nuwax_ws_client_t *ws, const char *request_id, const char *app);
esp_err_t nuwax_session_send_touch_event(nuwax_ws_client_t *ws, const nuwax_touch_point_t *point, const char *action);
esp_err_t nuwax_session_send_wifi_scan_result(nuwax_ws_client_t *ws,
                                              const char *request_id,
                                              bool ok,
                                              const char *message,
                                              const char ssids[][33],
                                              const int8_t *rssis,
                                              const uint8_t *authmodes,
                                              int count);
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
    uint32_t expected_epoch);
esp_err_t nuwax_session_send_wifi_status(nuwax_ws_client_t *ws,
                                         const char *request_id,
                                         const char *phase,
                                         const char *ssid,
                                         const char *ip,
                                         bool ok,
                                         const char *message);
esp_err_t nuwax_session_send_volume_changed_until_if_epoch(
    nuwax_ws_client_t *ws,
    const char *event_id,
    int volume_percent,
    const char *source,
    int64_t deadline_us,
    uint32_t expected_epoch);
esp_err_t nuwax_session_send_command_ack(nuwax_ws_client_t *ws,
                                         const char *request_id,
                                         const char *command,
                                         bool ok,
                                         const char *message);
esp_err_t nuwax_session_send_command_ack_until_if_epoch(
    nuwax_ws_client_t *ws,
    const char *request_id,
    const char *command,
    bool ok,
    const char *message,
    int64_t deadline_us,
    uint32_t expected_epoch);
esp_err_t nuwax_session_send_notify_playback_ack_until_if_epoch(
    nuwax_ws_client_t *ws,
    const char *ack_type,
    const char *request_id,
    const char *notify_id,
    const char *playback_id,
    bool ok,
    const char *message,
    int64_t deadline_us,
    uint32_t expected_epoch);
esp_err_t nuwax_session_send_notify_playback_cancelled_until_if_epoch(
    nuwax_ws_client_t *ws,
    const char *request_id,
    const char *notify_id,
    const char *playback_id,
    const char *reason,
    int64_t deadline_us,
    uint32_t expected_epoch);
void nuwax_session_handle_text(const char *payload, int len, nuwax_session_command_t *out_command);
