#pragma once

#include "esp_err.h"
#include "touch_input.h"
#include "ws_transport.h"
#include <stdbool.h>
#include <stdint.h>

#define NUWAX_SESSION_STATUS_LINES 6

typedef enum {
    NUWAX_SESSION_EVENT_NONE = 0,
    NUWAX_SESSION_EVENT_ASR_PARTIAL,
    NUWAX_SESSION_EVENT_ASR_FINAL,
    NUWAX_SESSION_EVENT_ASR_RESULT,
    NUWAX_SESSION_EVENT_TEXT_RESPONSE,
    NUWAX_SESSION_EVENT_TTS_RESULT,
    NUWAX_SESSION_EVENT_PLAYBACK_BEGIN,
    NUWAX_SESSION_EVENT_PLAYBACK_END,
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
    char playback_id[48];
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
    char mcp_payload[512];
} nuwax_session_command_t;

esp_err_t nuwax_session_send_hello(nuwax_ws_client_t *ws,
                                   const char *device_secret);
esp_err_t nuwax_session_send_ping(nuwax_ws_client_t *ws, int64_t now_ms);
esp_err_t nuwax_session_send_listen(nuwax_ws_client_t *ws, const char *state, const char *mode);
esp_err_t nuwax_session_send_listen_with_audio_session(nuwax_ws_client_t *ws,
                                                       const char *state,
                                                       const char *mode,
                                                       const char *audio_session_id,
                                                       const char *request_id);
esp_err_t nuwax_session_send_text_request(nuwax_ws_client_t *ws, const char *request_id, const char *text);
esp_err_t nuwax_session_send_cancel_turn(nuwax_ws_client_t *ws,
                                         const char *request_id,
                                         const char *turn_id,
                                         const char *reason);
esp_err_t nuwax_session_send_barge_in(nuwax_ws_client_t *ws,
                                      const char *state,
                                      const char *event_id,
                                      const char *parent_turn_id,
                                      const char *playback_id,
                                      const char *audio_session_id,
                                      uint32_t onset_sequence,
                                      uint32_t current_sequence,
                                      uint32_t confidence_q15);
esp_err_t nuwax_session_send_pc_status_request(nuwax_ws_client_t *ws, const char *request_id);
esp_err_t nuwax_session_send_status_app_request(nuwax_ws_client_t *ws, const char *request_id, const char *app);
esp_err_t nuwax_session_send_touch_event(nuwax_ws_client_t *ws, const nuwax_touch_point_t *point, const char *action);
esp_err_t nuwax_session_send_wifi_scan_result(nuwax_ws_client_t *ws,
                                              bool ok,
                                              const char *message,
                                              const char ssids[][33],
                                              const int8_t *rssis,
                                              const uint8_t *authmodes,
                                              int count);
esp_err_t nuwax_session_send_wifi_status(nuwax_ws_client_t *ws,
                                         const char *phase,
                                         const char *ssid,
                                         const char *ip,
                                         bool ok,
                                         const char *message);
esp_err_t nuwax_session_send_command_ack(nuwax_ws_client_t *ws,
                                         const char *request_id,
                                         const char *command,
                                         bool ok,
                                         const char *message);
void nuwax_session_handle_text(const char *payload, int len, nuwax_session_command_t *out_command);
