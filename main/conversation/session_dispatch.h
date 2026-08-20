#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_state.h"
#include "esp_err.h"
#include "session_protocol.h"
#include "sleep_transaction.h"
#include "ws_transport.h"

typedef enum {
    NUWAX_SESSION_PLAYBACK_STOP_REJECTED = 0,
    NUWAX_SESSION_PLAYBACK_STOP_CANCEL_ARMED,
    NUWAX_SESSION_PLAYBACK_STOP_NATURAL_DRAINING,
} nuwax_session_playback_stop_result_t;

typedef struct {
    void *ctx;
    void (*set_active_turn_id)(void *ctx, const char *turn_id);
    bool (*queue_wifi_connect)(void *ctx,
                               const char *ssid,
                               const char *password,
                               const char *request_id);
    bool (*request_wifi_scan)(void *ctx,
                              uint32_t ws_epoch,
                              const char *request_id);
    bool (*request_ble_reprovision)(void *ctx);
    nuwax_sleep_request_result_t (*request_sleep_transaction)(
        void *ctx,
        nuwax_ws_client_t *ws,
        const char *request_id,
        const char *command,
        const char *turn_id);
    void (*cancel_auto_followup_capture)(void *ctx, const char *reason);
    bool (*speaker_has_pending)(void *ctx);
    bool (*speaker_playback_matches)(void *ctx,
                                     const char *turn_id,
                                     const char *playback_id);
    bool (*active_turn_matches_playback)(void *ctx,
                                         const char *turn_id,
                                         const char *playback_id);
    nuwax_session_playback_stop_result_t (*speaker_cancel_matching_playback)(
        void *ctx,
        const char *turn_id,
        const char *playback_id,
        const char *reason);
    void (*speaker_cancel_now)(void *ctx, const char *reason);
    void (*speaker_request_fade_cancel)(void *ctx, const char *reason, uint32_t fade_ms);
    bool (*preserve_capture_on_stop_playback)(void *ctx);
    bool (*local_blocks_remote_idle)(void *ctx);
    bool (*set_state)(void *ctx, nuwax_app_state_t state);
    bool (*set_emotion)(void *ctx,
                        const char *emotion,
                        bool has_duration_ms,
                        uint32_t duration_ms);
    /* Optional identity-aware variant.  Device commands may arrive in a
     * different order within one cloud turn; this lets the owner bind a
     * set_emotion to the same request/turn without changing the wire format. */
    bool (*set_emotion_with_context)(void *ctx,
                                     const char *emotion,
                                     bool has_duration_ms,
                                     uint32_t duration_ms,
                                     const char *request_id,
                                     const char *turn_id);
    /* Queried immediately after set_emotion returns false so the protocol
     * ACK can distinguish an unsupported name from lifecycle rejection. */
    const char *(*set_emotion_failure_reason)(void *ctx);
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
    esp_err_t (*set_wake_word)(void *ctx,
                               const char *wake_word,
                               bool persist);
    esp_err_t (*set_wake_words)(void *ctx,
                                const nuwax_wake_word_config_t *config,
                                bool persist);
    esp_err_t (*reset_wake_word)(void *ctx);
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
