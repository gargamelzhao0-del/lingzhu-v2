#include "preroll_window.h"

#include <limits.h>
#include <string.h>

uint32_t nuwax_preroll_choose_confirmed_onset(
    bool confirmed_onset_valid,
    uint32_t confirmed_onset_sequence,
    bool candidate_onset_valid,
    uint32_t candidate_onset_sequence,
    uint32_t live_sequence) {
    if (confirmed_onset_valid) {
        return confirmed_onset_sequence;
    }
    return candidate_onset_valid ? candidate_onset_sequence : live_sequence;
}

bool nuwax_preroll_select_targeted_window(uint32_t buffered_frames,
                                          uint32_t onset_sequence,
                                          uint32_t live_sequence,
                                          uint32_t lead_frames,
                                          uint32_t fallback_frames,
                                          nuwax_preroll_sequence_reader_t read_sequence,
                                          void *reader_ctx,
                                          nuwax_preroll_window_t *out_window) {
    if (!read_sequence || !out_window || buffered_frames == 0) {
        return false;
    }
    memset(out_window, 0, sizeof(*out_window));

    /* A targeted window is valid only when the detector's onset is still in
     * the chronological ring. Falling back to an arbitrary tail here can
     * fabricate a sentence start and upload audio that was never associated
     * with the confirmed human event. Keep the legacy fallback parameter in
     * the ABI, but fail closed when the onset is absent. */
    (void)fallback_frames;
    uint32_t onset_offset = UINT32_MAX;
    for (uint32_t i = 0; i < buffered_frames; i++) {
        if (read_sequence(reader_ctx, i) == onset_sequence) {
            onset_offset = i;
            break;
        }
    }

    if (onset_offset == UINT32_MAX) {
        return false;
    }

    const uint32_t nominal_start =
        onset_offset > lead_frames ? onset_offset - lead_frames : 0U;

    /* Retain only lead frames that are contiguous with onset. A gap before
     * onset must not be bridged with unrelated or stale audio. */
    uint32_t start_offset = onset_offset;
    while (start_offset > nominal_start) {
        const uint32_t previous = read_sequence(reader_ctx, start_offset - 1U);
        const uint32_t current = read_sequence(reader_ctx, start_offset);
        if (previous + 1U != current) {
            break;
        }
        start_offset--;
    }

    /* The complete onset-to-live span is one utterance. A gap after onset
     * cannot be hidden by assigning a new session-local wire sequence. */
    uint32_t end_offset = onset_offset;
    while (end_offset < buffered_frames - 1U) {
        const uint32_t current = read_sequence(reader_ctx, end_offset);
        const uint32_t next = read_sequence(reader_ctx, end_offset + 1U);
        if (current + 1U != next) {
            break;
        }
        end_offset++;
    }
    if (end_offset != buffered_frames - 1U ||
        read_sequence(reader_ctx, end_offset) + 1U != live_sequence) {
        return false;
    }

    out_window->start_offset = start_offset;
    out_window->frame_count = end_offset - start_offset + 1U;
    out_window->first_sequence = read_sequence(reader_ctx, start_offset);
    out_window->last_sequence = read_sequence(reader_ctx, end_offset);
    out_window->onset_found = true;
    out_window->onset_included = true;
    out_window->gap_trimmed = start_offset != nominal_start;
    return out_window->frame_count > 0;
}

bool nuwax_preroll_overlap_frame_should_silence(bool playback_active,
                                                 bool trusted_clean_source,
                                                 uint32_t ref_correlation_q15,
                                                 uint32_t max_ref_correlation_q15) {
    if (!playback_active) {
        return false;
    }
    return !trusted_clean_source ||
           ref_correlation_q15 >= max_ref_correlation_q15;
}

bool nuwax_preroll_audio_trust_allows_preserve(
    const nuwax_preroll_audio_trust_t *trust,
    bool repair_succeeded,
    bool human_repaired,
    bool trusted_rescue_source) {
    if (!trust) {
        return false;
    }
    const bool common =
        trust->reference_generation != 0U &&
        trust->playback_generation != 0U &&
        trust->volume_generation != 0U &&
        trust->playback_state_stable &&
        trust->volume_state_stable &&
        trust->reference_snapshot &&
        trust->reference_generation_valid &&
        trust->reference_contiguous &&
        !trust->ref_missing &&
        !trust->processing_error &&
        !trust->microphone_clipped &&
        !trust->reference_clipped &&
        trust->output_underrun_delta == 0U;
    if (!common) {
        return false;
    }

    const bool verified_silence_context =
        trust->verified_silent_passthrough &&
        !trust->reference_active && trust->volume_percent <= 5U;
    const bool trusted_repair =
        repair_succeeded && human_repaired && trusted_rescue_source &&
        ((trust->reference_active && trust->reference_locked) ||
         verified_silence_context);
    const bool trusted_aec =
        trust->aec_processed_ok && trust->reference_active &&
        trust->reference_locked;
    const bool trusted_silence = verified_silence_context;
    return trusted_repair || trusted_aec || trusted_silence;
}

bool nuwax_preroll_confirmed_gap_repair_allowed(
    bool barge_confirmed,
    bool upload_started,
    uint32_t confirmed_sequence,
    uint32_t previous_sequence,
    uint32_t current_sequence,
    uint32_t *gap_frames) {
    if (gap_frames) {
        *gap_frames = 0U;
    }
    if (!barge_confirmed || upload_started) {
        return false;
    }

    /* Unsigned distances preserve the natural uint32_t sequence wrap. Values
     * in the opposite half of the number space are stale/backward input. */
    const uint32_t confirmation_distance =
        previous_sequence - confirmed_sequence;
    if (confirmation_distance >= (UINT32_MAX / 2U + 1U)) {
        return false;
    }
    const uint32_t current_distance = current_sequence - previous_sequence;
    if (current_distance < 2U ||
        current_distance > NUWAX_PREROLL_CONFIRMED_GAP_MAX_FRAMES + 1U) {
        return false;
    }
    if (gap_frames) {
        *gap_frames = current_distance - 1U;
    }
    return true;
}

bool nuwax_preroll_targeted_frame_preserves_confirmation(
    bool confirmation_span_active,
    bool barge_silenced,
    bool forced_silence) {
    return !confirmation_span_active ||
           (!barge_silenced && !forced_silence);
}
