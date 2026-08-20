#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_state.h"
#include "esp_err.h"
#include "session_protocol.h"
#include "ws_transport.h"

typedef struct {
    void *ctx;
    void (*set_active_turn_id)(void *ctx, const char *turn_id);
    bool (*queue_wifi_connect)(void *ctx,
                               const char *ssid,
                               const char *password,
                               const char *request_id);
    void (*request_wifi_scan)(void *ctx);
    bool (*request_ble_reprovision)(void *ctx);
    void (*cancel_auto_followup_capture)(void *ctx, const char *reason);
    bool (*speaker_has_pending)(void *ctx);
    bool (*speaker_playback_matches)(void *ctx,
                                     const char *turn_id,
                                     const char *playback_id);
    void (*speaker_cancel_now)(void *ctx, const char *reason);
    void (*speaker_request_fade_cancel)(void *ctx, const char *reason, uint32_t fade_ms);
    bool (*preserve_capture_on_stop_playback)(void *ctx);
    void (*enter_sleeping_conversation_state)(void *ctx, const char *reason);
    bool (*local_blocks_remote_idle)(void *ctx);
    void (*set_state)(void *ctx, nuwax_app_state_t state);
    bool (*set_emotion)(void *ctx, const char *emotion);
    void (*set_conversation_status)(void *ctx,
                                    const char *title,
                                    const char *phase,
                                    const char *text,
                                    bool show_now);
    void (*return_to_listening_after_cancel)(void *ctx, const char *reason);
    void (*show_status)(void *ctx,
                        const char *title,
                        const char lines[NUWAX_SESSION_STATUS_LINES][48],
                        const char *text);
    void (*apply_volume_percent)(void *ctx, int volume_percent, bool persist);
    esp_err_t (*set_audio_cleaner_enabled)(void *ctx, bool enabled);
    esp_err_t (*run_p3_ref_health)(void *ctx,
                                   float volume_db,
                                   uint32_t duration_ms,
                                   bool run_ref_probe,
                                   bool *out_passed);
    esp_err_t (*run_p3_softref_ab)(void *ctx,
                                   uint32_t duration_ms,
                                   uint32_t delay_samples,
                                   bool *out_ready);
    esp_err_t (*run_p3_hardref_ab)(void *ctx,
                                   uint32_t duration_ms,
                                   bool *out_ready);
    esp_err_t (*run_p3_nearend_candidate)(void *ctx,
                                          uint32_t duration_ms,
                                          uint32_t delay_samples,
                                          bool *out_candidate);
    esp_err_t (*run_p3_interrupt_timing)(void *ctx,
                                         nuwax_ws_client_t *ws,
                                         const char *mode,
                                         const char *request_id,
                                         uint32_t duration_ms,
                                         uint32_t delay_samples,
                                         bool *out_candidate,
                                         bool *out_cancel_sent);
    esp_err_t (*run_p5_fullduplex_overlap_probe)(void *ctx,
                                                 nuwax_ws_client_t *ws,
                                                 const char *request_id,
                                                 const char *audio_session_id,
                                                 uint32_t duration_ms,
                                                 bool *out_passed);
    esp_err_t (*run_p5_during_playback_candidate)(void *ctx,
                                                  nuwax_ws_client_t *ws,
                                                  const char *mode,
                                                  const char *request_id,
                                                  const char *audio_session_id,
                                                  uint32_t duration_ms,
                                                  uint32_t delay_samples,
                                                  bool *out_candidate,
                                                  bool *out_cancel_sent,
                                                  bool *out_passed);
    esp_err_t (*set_l3_interrupt_enabled)(void *ctx, bool enabled, bool persist);
} nuwax_session_dispatch_callbacks_t;

void nuwax_session_dispatch_command(nuwax_ws_client_t *ws,
                                    const nuwax_session_command_t *command,
                                    const nuwax_session_dispatch_callbacks_t *callbacks);
