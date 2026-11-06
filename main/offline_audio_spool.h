#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "mic_input.h"
#include "ws_transport.h"
#include "offline_capture_health.h"

typedef enum {
    NUWAX_OFFLINE_AUDIO_MODE_MEETING = 0,
    NUWAX_OFFLINE_AUDIO_MODE_MONITORING = 1,
} nuwax_offline_audio_mode_t;

typedef struct {
    uint32_t sequence;
    uint32_t timestamp_ms;
    uint8_t flags;
    const int16_t *samples;
    size_t sample_count;
} nuwax_offline_audio_frame_t;

/* Per-live-stream mirror state owns bounded PSRAM staging. A close worker
 * takes that staging when the MIC producer stops with an unsent tail.
 * Media discovery/recovery belongs to the storage supervisor; frame paths
 * never wait for FILE I/O. Initialize to zero; request_close releases ownership. */
typedef struct {
    bool started;
    bool offline;
    bool close_pending;
    bool fault_latched;
    bool media_deferred; /* No new staging until supervisor reports ready. */
    uint32_t next_service_ms;
    esp_err_t last_error;
    uint32_t accepted_frames;
    uint32_t rejected_frames;
    uint32_t last_drop_log_ms;
    /* Bounded PCM staging survives delayed archive attachment and queue pressure.
     * Ownership transfers to a close worker when the live producer stops. */
    void *staging;
    uint16_t staging_head;
    uint16_t staging_count;
    bool attached;
    bool complete;
    nuwax_offline_audio_mode_t mode;
    char session_id[48];
} nuwax_offline_audio_mirror_t;

#define NUWAX_OFFLINE_AUDIO_RECOVERY_TIMEOUT_MS 1500U

/* Initializes the runtime archive foundation. No-op when
 * CONFIG_NUWAX_OFFLINE_AUDIO_ENABLE is disabled. The caller must not invoke
 * this from an ISR. Mode owners must only call begin/append after the cloud
 * replay protocol and their offline capture state machine are enabled. */
esp_err_t nuwax_offline_audio_init(void);

/* Starts the low-priority media lifecycle owner even when no card is present.
 * It retries an absent/reinserted card with bounded backoff and probes a
 * mounted card only while all archive users are quiescent. */
esp_err_t nuwax_offline_audio_supervisor_start(void);
void nuwax_offline_audio_supervisor_wake(void);

/* Ensures that the SD archive foundation is usable before a new mode owns
 * the microphone. If a runtime card-removal fault was latched, recovery is
 * attempted only after the spool writer, FILE and replay owners are
 * quiescent. A reader may retain bounded PCM during fault recovery; healthy
 * capture defers routine media probes, including silent monitoring. A
 * missing card is returned as an error; callers must keep the
 * online meeting/monitoring path usable without the optional mirror. */
esp_err_t nuwax_offline_audio_recover_if_idle(uint32_t timeout_ms);

/* Starts a bounded microphone capture owner which only feeds the archive
 * queue.  No WebSocket or filesystem operation is performed on the reader
 * path.  The task is intended for an already enabled meeting/monitoring
 * session whose WebSocket has gone offline. */
esp_err_t nuwax_offline_audio_start_capture(nuwax_offline_audio_mode_t mode,
                                            const char *session_id,
                                            uint32_t sample_rate,
                                            uint8_t channels,
                                            uint16_t frame_duration_ms);
esp_err_t nuwax_offline_audio_start_capture_continuation(nuwax_offline_audio_mode_t mode,
    const char *session_id, uint32_t sample_rate, uint8_t channels,
    uint16_t frame_duration_ms, uint32_t first_sequence);
/* Read after capture_running becomes false, before re-admitting online capture. */
uint32_t nuwax_offline_audio_next_capture_sequence(void);
/* Requests capture termination without waiting for SD flush/footer I/O.  This
 * is the privacy-safe path for UI/cloud stop actions on a disconnected device. */
esp_err_t nuwax_offline_audio_request_stop(bool complete);
/* Requests an archive close without waiting for SD/file-system I/O.  This is
 * the only close API allowed from a latency-sensitive online mirror path. */
esp_err_t nuwax_offline_audio_request_end(bool complete);
esp_err_t nuwax_offline_audio_stop_capture(bool complete);
bool nuwax_offline_audio_capture_running(void);
/* Lock-free observation; scheduling a task is not proof of MIC readiness or
 * a usable sink. Historical loss is not cleared by a media recovery. */
nuwax_offline_capture_health_t nuwax_offline_audio_capture_health(void);
bool nuwax_offline_audio_has_ready(void);
bool nuwax_offline_audio_limit_reached(void);

/* Opens one archive session. Only one meeting/monitoring owner may be active. */
esp_err_t nuwax_offline_audio_begin(nuwax_offline_audio_mode_t mode,
                                    const char *session_id,
                                    uint32_t sample_rate,
                                    uint8_t channels,
                                    uint16_t frame_duration_ms);

/* Online mirror: zero-wait admission, file open deferred to the writer, and
 * storage-only replay. SD failures never change the live transport result. */
esp_err_t nuwax_offline_audio_begin_mirror(nuwax_offline_audio_mode_t mode,
                                           const char *session_id,
                                           uint32_t sample_rate,
                                           uint8_t channels,
                                           uint16_t frame_duration_ms);
esp_err_t nuwax_offline_audio_try_append(const nuwax_offline_audio_frame_t *frame);

void nuwax_offline_audio_mirror_reset(nuwax_offline_audio_mirror_t *mirror);
/* Prepares staging and retries archive attachment at frame boundaries. ESP_OK
 * means PCM can be retained, not that a FILE has been opened or persisted.
 * This API never mounts/scans the card or waits for the writer. */
esp_err_t nuwax_offline_audio_mirror_service(
    nuwax_offline_audio_mirror_t *mirror,
    nuwax_offline_audio_mode_t mode,
    const char *session_id,
    uint32_t sample_rate,
    uint8_t channels,
    uint16_t frame_duration_ms,
    uint32_t now_ms);
esp_err_t nuwax_offline_audio_mirror_append(
    nuwax_offline_audio_mirror_t *mirror,
    const nuwax_offline_audio_frame_t *frame,
    uint32_t now_ms);
/* Fences new mirror frames immediately and delegates footer/rename work to
 * the writer. A later storage-owner service pass converges a slow close. */
esp_err_t nuwax_offline_audio_mirror_request_close(
    nuwax_offline_audio_mirror_t *mirror,
    bool complete,
    uint32_t now_ms);
bool nuwax_offline_audio_mirror_started(
    const nuwax_offline_audio_mirror_t *mirror);

/* Copies one PCM16 frame into a bounded queue. This function never performs
 * filesystem I/O and returns ESP_ERR_NOT_FINISHED when the queue is full. */
esp_err_t nuwax_offline_audio_append(const nuwax_offline_audio_frame_t *frame);

/* Waits for queued frames to reach the SD writer. It does not close the file. */
esp_err_t nuwax_offline_audio_flush(uint32_t timeout_ms);

/* Closes the active segment atomically. complete=false marks a disconnect or
 * power-loss boundary; the file remains eligible for later replay. */
esp_err_t nuwax_offline_audio_end(bool complete);

bool nuwax_offline_audio_is_active(void);

/* Owner-side normal shutdown barrier; nonblocking and idempotent. Once
 * requested, no new archive/replay may start until reboot. Existing recording
 * owners must still stop/seal their own sink before this returns ESP_OK. */
esp_err_t nuwax_offline_audio_prepare_shutdown(void);
/* Application owner only, after prepare_shutdown returned ESP_OK. Reopen
 * admission if charging recovered before the terminal power commit. */
esp_err_t nuwax_offline_audio_cancel_shutdown(void);
/* Read-only admission for both replay transports. Recording and pending SD
 * writes have priority; callers must also recheck between filesystem reads. */
bool nuwax_offline_audio_storage_busy(void);
bool nuwax_offline_audio_is_degraded(void);
uint32_t nuwax_offline_audio_dropped_frames(void);

/* Called by the connection owner after a new WebSocket epoch is ready. Ready
 * segments use an offset-resumable begin/chunk/commit transaction; a segment
 * is deleted only after a matching commit ACK. */
esp_err_t nuwax_offline_audio_replay_ready(nuwax_ws_client_t *ws,
                                           uint32_t expected_epoch);

/* Production replay runs on a low-priority worker so the connection owner
 * remains available for conversation, keepalive, OTA, and mode controls. The
 * worker is bound to the supplied WebSocket epoch and leaves .ready files
 * untouched when that epoch is lost. */
esp_err_t nuwax_offline_audio_replay_formal_start(nuwax_ws_client_t *ws,
                                                  uint32_t expected_epoch);
bool nuwax_offline_audio_replay_formal_running(void);
/* Requests a formal replay worker to stop and waits until it no longer
 * references the supplied WebSocket client.  The connection owner must call
 * this before closing or reusing that client after a transport loss. */
esp_err_t nuwax_offline_audio_replay_formal_cancel(uint32_t expected_epoch,
                                                   uint32_t timeout_ms);

/* Test-only asynchronous replay. When enabled by Kconfig, this opens a
 * separate WebSocket to the LAN replay gateway and never uses the primary
 * cloud connection. It is safe to call repeatedly; an active worker is
 * treated as already scheduled. Disabled builds return ESP_OK. */
esp_err_t nuwax_offline_audio_replay_test_start(void);
bool nuwax_offline_audio_replay_test_running(void);
bool nuwax_offline_audio_replay_running(void);
/* Normal background replay yields to a queued OTA. An unreaped transport is
 * a hard admission blocker. prepare must run with the OTA reservation held
 * and join all replay/read/transport workers before any firmware write. */
bool nuwax_offline_audio_replay_blocks_ota(void);
esp_err_t nuwax_offline_audio_replay_prepare_ota(uint32_t timeout_ms);
/* True once a replay worker yielded because recording/storage became busy.
 * The connection owner consumes this edge and schedules an immediate retry
 * after the SD writer has become idle. */
bool nuwax_offline_audio_replay_retry_required(void);
/* Non-consuming form used by the audio owner to yield its wait loop until the
 * connection owner has serviced the retry edge. */
bool nuwax_offline_audio_replay_retry_pending(void);

/* Called from the WebSocket text callback before normal session dispatch.
 * The callback's captured connection epoch is mandatory so a delayed ACK from
 * an older transport cannot complete the current replay transaction. */
bool nuwax_offline_audio_handle_protocol_text(const char *payload, int len,
                                              uint32_t connection_epoch);

/* Transfers the live offline reader at a frame boundary, preserving RX DMA.
 * Called only by the connection owner after cloud authorization. */
esp_err_t nuwax_offline_audio_take_capture_input(nuwax_mic_input_t *mic, uint32_t timeout_ms);
