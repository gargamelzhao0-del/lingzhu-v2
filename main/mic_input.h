#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "board/audio_board.h"

typedef struct {
    size_t sample_count;
    int32_t first;
    int32_t last;
    int32_t min;
    int32_t max;
    int32_t mean;
    int64_t centered_peak;
    uint32_t zero_count;
    uint32_t positive_count;
    uint32_t negative_count;
    uint32_t selected_shift;
    bool dc_block_enabled;
    bool auto_attenuate_enabled;
} nuwax_mic_raw_stats_t;

typedef enum {
    NUWAX_MIC_OWNER_NONE = 0,
    NUWAX_MIC_OWNER_UNSPECIFIED,
    NUWAX_MIC_OWNER_WAKE_DETECTOR,
    NUWAX_MIC_OWNER_CONVERSATION_CAPTURE,
    NUWAX_MIC_OWNER_MEETING_CAPTURE,
    NUWAX_MIC_OWNER_MONITORING_CAPTURE,
} nuwax_mic_owner_t;

typedef struct {
    void *rx_handle;
    void *raw_samples;
    size_t raw_capacity;
    /* Shared RX keeps unconsumed whole-TDM samples in its own buffer. */
    size_t rx_cached_offset;
    size_t rx_cached_bytes;
    nuwax_mic_raw_stats_t last_raw_stats;
    nuwax_mic_owner_t owner;
    nuwax_audio_board_lease_t board_lease;
    nuwax_audio_board_ref_t board_ref;
    uint32_t rx_overflow_count;
    uint32_t rx_overflow_events;
    uint32_t rx_startup_resyncs;
    uint32_t rx_overflow_last_report_event;
    bool rx_audio_started;
    bool owner_acquired;
    bool initialized;
    bool vocat_shared_rx; /* P2-4: rx_handle is the board's shared RX (release, not delete) */
    bool rx_overflow_recovery_pending;
} nuwax_mic_input_t;

/* Zero-initialize caller storage and serialize its lifecycle. An owner-busy
 * or owner-lock failure preserves the object; successful admission initializes
 * it. Live instances may only be moved by the existing owner-transfer API. */
esp_err_t nuwax_mic_input_init(nuwax_mic_input_t *mic);
esp_err_t nuwax_mic_input_init_with_owner(nuwax_mic_input_t *mic, nuwax_mic_owner_t owner);
esp_err_t nuwax_mic_input_read_pcm16(nuwax_mic_input_t *mic,
                                     int16_t *samples,
                                     size_t sample_count,
                                     uint32_t timeout_ms,
                                     size_t *samples_read);
esp_err_t nuwax_mic_input_read_vocat_mr_pcm16(nuwax_mic_input_t *mic,
                                              int16_t *mic_samples,
                                              int16_t *ref_samples,
                                              size_t sample_count,
                                              uint32_t timeout_ms,
                                              size_t *samples_read);
esp_err_t nuwax_mic_input_read_vocat_mic12_pcm16(nuwax_mic_input_t *mic,
                                                 int16_t *mic1_samples,
                                                 int16_t *mic2_samples,
                                                 size_t sample_count,
                                                 uint32_t timeout_ms,
                                                 size_t *samples_read);
esp_err_t nuwax_mic_input_read_vocat_mic12_ref_pcm16(nuwax_mic_input_t *mic,
                                                     int16_t *mic1_samples,
                                                     int16_t *mic2_samples,
                                                     int16_t *ref_samples,
                                                     size_t sample_count,
                                                     uint32_t timeout_ms,
                                                     size_t *samples_read);
const nuwax_mic_raw_stats_t *nuwax_mic_input_get_last_raw_stats(const nuwax_mic_input_t *mic);
/* Stops use of RX and always relinquishes the process-wide MIC owner. On a
 * shared-board physical teardown error the object remains retryable; callers
 * should retain and retry it when possible, while a later board init can also
 * recover pending teardown. A stale/malformed channel lease cannot be safely
 * overridden and therefore enters controlled restart after owner release. */
esp_err_t nuwax_mic_input_deinit(nuwax_mic_input_t *mic);

/* P2-4 self-test: init mic (conversation-capture owner) -> read N mono frames
 * via read_pcm16 -> report RMS/peak -> deinit. Verifies the mono PCM16 contract
 * end-to-end (on vocat: shared-RX 4-slot TDM downmix). out_rms/out_peak optional. */
esp_err_t nuwax_mic_input_capture_selftest(uint32_t frames, int *out_rms, int *out_peak);
