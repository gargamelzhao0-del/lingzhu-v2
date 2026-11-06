#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_state.h"
#include "esp_err.h"

#if (defined(CONFIG_NUWAX_MUSIC_URL_TEST_ENABLE) && \
     CONFIG_NUWAX_MUSIC_URL_TEST_ENABLE) || \
    (defined(CONFIG_NUWAX_MUSIC_PLAYBACK_ENABLE) && \
     CONFIG_NUWAX_MUSIC_PLAYBACK_ENABLE)
#define NUWAX_SPEAKER_URL_STREAM_ENABLED 1
#else
#define NUWAX_SPEAKER_URL_STREAM_ENABLED 0
#endif

typedef enum {
    NUWAX_SPEAKER_PLAYBACK_OWNER_NONE = 0,
    NUWAX_SPEAKER_PLAYBACK_OWNER_CONVERSATION,
    NUWAX_SPEAKER_PLAYBACK_OWNER_NOTIFICATION,
#if NUWAX_SPEAKER_URL_STREAM_ENABLED
    /* Development-only local PCM stream. It uses the shared output manager
     * but never publishes conversation state or follow-up callbacks. */
    NUWAX_SPEAKER_PLAYBACK_OWNER_URL_TEST,
#endif
} nuwax_speaker_playback_owner_t;

typedef struct {
    nuwax_app_state_t (*get_state)(void);
    void (*set_state)(uint32_t connection_epoch,
                      uint32_t generation,
                      nuwax_app_state_t state);
    void (*set_state_fast)(uint32_t connection_epoch,
                           uint32_t generation,
                           nuwax_app_state_t state);
    /* Published after a START generation is admitted but before any I2S
     * activation. The callback must not block. */
    void (*on_start_pending)(uint32_t connection_epoch,
                             uint32_t generation);
    void (*on_tts_start)(uint32_t connection_epoch,
                         uint32_t generation);
    /* First successful physical PCM write for the START generation. */
    void (*on_first_pcm_written)(uint32_t connection_epoch,
                                 uint32_t generation,
                                 int64_t timestamp_us);
    /* Called from the speaker task when a START frame waited for metadata
     * longer than the bounded authorization window.  The callback must be
     * non-blocking and only publish an owner event. */
    void (*on_start_gate_timeout)(uint32_t connection_epoch,
                                  uint32_t generation);
    /* Called for a generation-bound output/stop failure. The callback must be
     * non-blocking and only publish an event to the conversation owner. */
    void (*on_playback_fault)(uint32_t connection_epoch,
                              uint32_t generation,
                              esp_err_t result);
    /* Optional progress edge for an owner that renews a PLAYING deadline.
     * It is rate-limited and must not block. */
    void (*on_playback_progress)(uint32_t connection_epoch,
                                 uint32_t generation,
                                 uint32_t played_frames,
                                 uint64_t played_bytes,
                                 uint32_t progress_ms);
    /* PCM ingress is also turn progress. This edge prevents a bounded
     * no-progress timeout from firing while the jitter buffer is still
     * receiving a slow or bursty stream before the next I2S write. */
    void (*on_ingress_progress)(uint32_t connection_epoch,
                                uint32_t generation);
    void (*on_playback_done)(uint32_t connection_epoch,
                              uint32_t generation,
                              uint32_t played_frames,
                              uint32_t write_failures);
    /* Published when the binary END for a generation is consumed, including
     * an END received while a cancelled stream is being discarded. */
    void (*on_stream_end)(uint32_t connection_epoch,
                          uint32_t generation);
    bool (*schedule_auto_followup_after_playback)(uint32_t connection_epoch,
                                                  uint32_t generation);
    void (*return_to_listening_after_cancel)(uint32_t connection_epoch,
                                             uint32_t generation,
                                             const char *reason);
    void (*on_volume_changed)(uint8_t volume_percent);
} nuwax_speaker_playback_callbacks_t;

typedef struct {
    bool playback_active;
    bool playback_session_active;
    bool pending;
    bool stop_in_progress;
    bool stop_fault_active;
    bool fade_cancel_active;
    uint32_t submitted_generation;
    uint32_t submitted_connection_epoch;
    uint32_t playback_generation;
    uint32_t playback_connection_epoch;
    uint32_t fade_cancel_generation;
    uint32_t volume_generation;
    uint8_t volume_percent;
    bool probe_duck_active;
    bool start_gate_pending;
    bool start_gate_authorized;
    uint32_t start_gate_generation;
    uint32_t start_gate_connection_epoch;
    uint32_t start_gate_deadline_ms;
    uint32_t probe_duck_generation;
    uint32_t queued_frames;
    uint32_t played_frames;
    uint32_t queue_dropped_frames;
    uint32_t write_failures;
    uint64_t played_bytes;
    uint32_t playback_last_progress_ms;
    bool jitter_rebuffering;
    uint32_t startup_target_ms;
    uint32_t resume_target_ms;
    uint32_t startup_buffer_wait_ms;
    uint32_t low_watermark_ms;
    uint32_t rebuffer_events;
    uint32_t rebuffer_wait_ms;
    uint32_t rebuffer_max_wait_ms;
    uint32_t tail_wait_events;
    uint32_t tail_wait_ms;
    uint32_t max_ingress_gap_ms;
    uint32_t stream_stall_faults;
    uint32_t stream_protocol_faults;
    bool rx_stream_active;
    bool rx_end_accepted;
    uint32_t rx_next_sequence;
    uint32_t rx_pcm_frames;
    uint64_t rx_pcm_bytes;
    uint32_t rx_expected_frames;
    uint32_t rx_expected_bytes;
    bool aec_reference_required;
    bool aec_reference_ready;
    uint32_t queue_waiting;
    uint32_t completed_stop_generation;
    uint32_t completed_stop_connection_epoch;
    int64_t completed_stop_timestamp_us;
    esp_err_t completed_stop_result;
    uint32_t failed_stop_generation;
    uint32_t failed_stop_connection_epoch;
    int64_t failed_stop_timestamp_us;
    esp_err_t failed_stop_result;
    nuwax_speaker_playback_owner_t active_owner;
    char active_turn_id[48];
    char active_notify_id[48];
    char active_playback_id[48];
} nuwax_speaker_playback_stats_t;

typedef enum {
    NUWAX_SPEAKER_CANCEL_REQUEST_REJECTED = 0,
    NUWAX_SPEAKER_CANCEL_REQUEST_ARMED,
    NUWAX_SPEAKER_CANCEL_REQUEST_STOPPING,
    NUWAX_SPEAKER_CANCEL_REQUEST_NATURAL_DRAINING,
    NUWAX_SPEAKER_CANCEL_REQUEST_COMPLETED,
    NUWAX_SPEAKER_CANCEL_REQUEST_FAILED,
} nuwax_speaker_cancel_request_status_t;

typedef struct {
    nuwax_speaker_cancel_request_status_t status;
    uint32_t connection_epoch;
    uint32_t playback_generation;
    /* Linearization time of the first cancel request for this generation. */
    int64_t request_timestamp_us;
    /* Valid when status is COMPLETED; may predate a late cancel request. */
    int64_t completed_stop_timestamp_us;
    esp_err_t completed_stop_result;
    int64_t failed_stop_timestamp_us;
    esp_err_t failed_stop_result;
} nuwax_speaker_cancel_request_result_t;

esp_err_t nuwax_speaker_playback_init(const nuwax_speaker_playback_callbacks_t *callbacks);
void nuwax_speaker_playback_task(void *arg);

bool nuwax_speaker_playback_has_pending(void);
bool nuwax_speaker_playback_session_active(void);
bool nuwax_speaker_playback_output_ready(void);
void nuwax_speaker_playback_get_stats(nuwax_speaker_playback_stats_t *stats);
void nuwax_speaker_playback_cancel_now(const char *source);
bool nuwax_speaker_playback_request_cancel_generation(uint32_t expected_generation,
                                                      const char *source,
                                                      uint32_t fade_ms);
bool nuwax_speaker_playback_request_cancel_generation_ex(
    uint32_t expected_generation,
    const char *source,
    uint32_t fade_ms,
    nuwax_speaker_cancel_request_result_t *result);
bool nuwax_speaker_playback_request_scoped_stop_generation_ex(
    uint32_t expected_connection_epoch,
    uint32_t expected_generation,
    bool preserve_natural_drain,
    const char *source,
    uint32_t fade_ms,
    nuwax_speaker_cancel_request_result_t *result);
bool nuwax_speaker_playback_request_probe_duck_generation(
    uint32_t expected_generation,
    uint8_t attenuation_db,
    uint16_t duration_ms);
void nuwax_speaker_playback_request_fade_cancel(const char *source, uint32_t fade_ms);
void nuwax_speaker_playback_fade_cancel(const char *source, uint32_t fade_ms);
void nuwax_speaker_playback_prepare_for_tts(const char *reason);
void nuwax_speaker_playback_release_idle_output(const char *reason);
esp_err_t nuwax_speaker_playback_reset_runtime(void);
void nuwax_speaker_playback_reset_jitter_learning(void);
/* Releases only the physical speaker/audio-board lease. The playback task,
 * protocol state, volume and PSRAM jitter buffer remain available so output
 * can be rebuilt lazily after a temporary exclusive-memory operation. */
esp_err_t nuwax_speaker_playback_deinit_output(void);
void nuwax_speaker_playback_note_playback_begin(const char *turn_id,
                                                const char *playback_id,
                                                uint32_t frames,
                                                uint32_t bytes);
bool nuwax_speaker_playback_abort_metadata_only_session(
    const char *turn_id,
    const char *playback_id,
    const char *reason);
/* Atomically retire an expired metadata-only owner and fence late RX. A
 * concurrent START, real output claim or different IDs cause a no-op. */
bool nuwax_speaker_playback_expire_metadata_only_session(
    const char *turn_id, const char *playback_id, uint32_t timeout_ms);
void nuwax_speaker_playback_note_playback_end(const char *turn_id,
                                              const char *playback_id,
                                              const char *reason);
void nuwax_speaker_playback_note_notification_begin(const char *notify_id,
                                                    const char *playback_id,
                                                    uint32_t frames,
                                                    uint32_t bytes);
bool nuwax_speaker_playback_abort_notification_metadata_only_session(
    const char *notify_id,
    const char *playback_id,
    const char *reason);
void nuwax_speaker_playback_note_notification_end(const char *notify_id,
                                                  const char *playback_id,
                                                  const char *reason);

void nuwax_speaker_playback_apply_volume_percent(int volume_percent, bool persist);
/* A queued local release keeps the sequence returned by its final preview.
 * Returns false for superseded/duplicate tokens with no report or persistence.
 * Zero is the compatibility path: apply as a new authoritative command. */
bool nuwax_speaker_playback_apply_volume_commit_percent(int volume_percent,
                                                       bool persist,
                                                       uint32_t preview_sequence);
/* Nonblocking registration AFTER the local action entered its owner queue.
 * Captures the accepted rollback target even before the owner processes it. */
void nuwax_speaker_playback_note_volume_commit_queued(int volume_percent,
                                                     uint32_t preview_sequence);
/* Publish only. PCM output consumes the latest preview under its owner lock. */
uint32_t nuwax_speaker_playback_preview_volume_percent_ordered(int volume_percent);
/* Roll back only this gesture to the newest admitted value. */
void nuwax_speaker_playback_cancel_volume_preview(uint32_t preview_sequence);
/* Application owner only, after audio/OTA and UI idle admission. Force bypasses
 * debounce for an already quiesced orderly shutdown, never the Flash fence. */
bool nuwax_speaker_playback_service_volume_persistence(bool force);
void nuwax_speaker_playback_preview_volume_percent(int volume_percent);
void nuwax_speaker_playback_apply_current_volume(void);
uint8_t nuwax_speaker_playback_get_volume_percent(void);
/* Read the newest admitted setting and its order without consuming a preview. */
uint8_t nuwax_speaker_playback_get_confirmed_volume_percent(uint32_t *sequence);

// v15-v2: 设备声纹 mask 模块需要 TTS ID 过滤字典子集
// TTS ID 映射 (与 tools/device_voiceprint_trainer/train.py TTS_ID_MAP 严格一致)
esp_err_t nuwax_speaker_playback_handle_audio_frame(uint32_t sequence,
                                                    uint8_t flags,
                                                    uint16_t sample_rate,
                                                    uint8_t channels,
                                                    const int16_t *samples,
                                                    size_t sample_count);
/* A normal downlink frame carries both its immutable WebSocket connection
 * epoch and the conversation owner's admission epoch. The speaker manager
 * binds the source connection to the new generation and rechecks both values
 * while holding its own mutex. */
esp_err_t nuwax_speaker_playback_handle_audio_frame_admitted(
    uint32_t connection_epoch,
    uint32_t admission_epoch,
    uint32_t sequence,
    uint8_t flags,
    uint16_t sample_rate,
    uint8_t channels,
    const int16_t *samples,
    size_t sample_count);
/* Batch-aware variant. The timeout is additionally capped by the speaker's
 * per-frame RX admission limit, so a WebSocket callback can share one total
 * budget across all frames without weakening generation or epoch checks. */
esp_err_t nuwax_speaker_playback_handle_audio_frame_admitted_with_timeout(
    uint32_t connection_epoch,
    uint32_t admission_epoch,
    uint32_t sequence,
    uint8_t flags,
    uint16_t sample_rate,
    uint8_t channels,
    const int16_t *samples,
    size_t sample_count,
    uint32_t admission_timeout_ms);
void nuwax_speaker_playback_set_downlink_admission(uint32_t epoch,
                                                  bool allowed);

#if NUWAX_SPEAKER_URL_STREAM_ENABLED
/* Development URL-test ingress. The caller supplies exact PCM16/16 kHz/mono
 * totals and sequential chunks. This path is intentionally separate from the
 * cloud protocol: it never creates a turn, changes app state, or schedules a
 * listen follow-up. A formal cloud START preempts it. */
esp_err_t nuwax_speaker_playback_url_test_begin(
    uint32_t total_bytes,
    bool end,
    const int16_t *samples,
    size_t sample_count,
    uint32_t *generation_out);
esp_err_t nuwax_speaker_playback_url_test_write(
    uint32_t generation,
    uint32_t sequence,
    bool end,
    const int16_t *samples,
    size_t sample_count,
    uint32_t timeout_ms);
bool nuwax_speaker_playback_url_test_stop(uint32_t generation,
                                          const char *reason);
bool nuwax_speaker_playback_url_test_active(uint32_t generation);
#endif

/* A binary START is not physically playable until the conversation owner has
 * validated the matching JSON turn/playback metadata. */
esp_err_t nuwax_speaker_playback_authorize_generation(
    uint32_t generation,
    const char *turn_id,
    const char *playback_id);
esp_err_t nuwax_speaker_playback_authorize_notification_generation(
    uint32_t generation,
    const char *notify_id,
    const char *playback_id);
bool nuwax_speaker_playback_reject_start_generation(uint32_t expected_generation,
                                                    const char *source);
