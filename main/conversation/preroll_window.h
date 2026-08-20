#pragma once

#include <stdbool.h>
#include <stdint.h>

#define NUWAX_PREROLL_CONFIRMED_GAP_MAX_FRAMES 8U

typedef uint32_t (*nuwax_preroll_sequence_reader_t)(void *ctx, uint32_t chronological_offset);

typedef struct {
    uint32_t start_offset;
    uint32_t frame_count;
    uint32_t first_sequence;
    uint32_t last_sequence;
    bool onset_found;
    bool onset_included;
    /* True when a historical gap trims optional lead frames before onset.
     * Any gap from onset through the live boundary rejects the window. */
    bool gap_trimmed;
} nuwax_preroll_window_t;

uint32_t nuwax_preroll_choose_confirmed_onset(
    bool confirmed_onset_valid,
    uint32_t confirmed_onset_sequence,
    bool candidate_onset_valid,
    uint32_t candidate_onset_sequence,
    uint32_t live_sequence);

typedef struct {
    uint32_t reference_generation;
    uint32_t playback_generation;
    uint32_t volume_generation;
    uint32_t output_underrun_delta;
    uint8_t volume_percent;
    bool playback_state_stable;
    bool volume_state_stable;
    bool reference_snapshot;
    bool reference_generation_valid;
    bool reference_contiguous;
    bool reference_active;
    bool reference_locked;
    bool aec_processed_ok;
    bool ref_missing;
    bool processing_error;
    bool microphone_clipped;
    bool reference_clipped;
    bool verified_silent_passthrough;
} nuwax_preroll_audio_trust_t;

/* Select a window with optional contiguous lead and an unbroken onset-to-live
 * span. fallback_frames is retained for call-site compatibility, but a
 * missing onset or any post-onset gap fails closed. */
bool nuwax_preroll_select_targeted_window(uint32_t buffered_frames,
                                          uint32_t onset_sequence,
                                          uint32_t live_sequence,
                                          uint32_t lead_frames,
                                          uint32_t fallback_frames,
                                          nuwax_preroll_sequence_reader_t read_sequence,
                                          void *reader_ctx,
                                          nuwax_preroll_window_t *out_window);

bool nuwax_preroll_overlap_frame_should_silence(bool playback_active,
                                                 bool trusted_clean_source,
                                                 uint32_t ref_correlation_q15,
                                                 uint32_t max_ref_correlation_q15);

bool nuwax_preroll_audio_trust_allows_preserve(
    const nuwax_preroll_audio_trust_t *trust,
    bool repair_succeeded,
    bool human_repaired,
    bool trusted_rescue_source);

/* A confirmed barge-in may bridge a short capture loss only after the
 * confirmation frame and before any audio is committed to the uplink. */
bool nuwax_preroll_confirmed_gap_repair_allowed(
    bool barge_confirmed,
    bool upload_started,
    uint32_t confirmed_sequence,
    uint32_t previous_sequence,
    uint32_t current_sequence,
    uint32_t *gap_frames);

/* Real audio is mandatory from onset through confirmation. Forced silence is
 * structural padding and is admissible only after that span is complete. */
bool nuwax_preroll_targeted_frame_preserves_confirmation(
    bool confirmation_span_active,
    bool barge_silenced,
    bool forced_silence);
