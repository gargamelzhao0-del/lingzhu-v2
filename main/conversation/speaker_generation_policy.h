#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    NUWAX_SPEAKER_STOP_SUPERSEDED = 0,
    NUWAX_SPEAKER_STOP_WAIT_OWNER,
    NUWAX_SPEAKER_STOP_CLAIMED,
} nuwax_speaker_stop_claim_result_t;

static inline bool nuwax_speaker_start_submission_allowed(
    uint32_t io_claim_generation,
    uint32_t completion_claim_generation,
    uint32_t maintenance_claim_generation,
    uint32_t cancel_pending_generation,
    uint32_t output_stop_owner_generation,
    bool runtime_reset_in_progress) {
    return io_claim_generation == 0 &&
           completion_claim_generation == 0 &&
           maintenance_claim_generation == 0 &&
           cancel_pending_generation == 0 &&
           output_stop_owner_generation == 0 &&
           !runtime_reset_in_progress;
}

static inline bool nuwax_speaker_generation_is_current(
    uint32_t submitted_generation,
    uint32_t playback_generation,
    bool drop_until_end,
    uint32_t generation) {
    return generation != 0 &&
           generation == submitted_generation &&
           generation == playback_generation &&
           !drop_until_end;
}

static inline bool nuwax_speaker_owner_loss_matches_turn(
    uint32_t lost_generation,
    bool turn_active,
    uint32_t turn_generation,
    uint32_t pending_start_generation) {
    return lost_generation != 0U && turn_active &&
           (lost_generation == turn_generation ||
            lost_generation == pending_start_generation);
}

static inline bool nuwax_speaker_cancel_expected_matches(
    uint32_t submitted_generation,
    bool drop_until_end,
    uint32_t expected_generation) {
    return expected_generation != 0 &&
           expected_generation == submitted_generation &&
           !drop_until_end;
}

static inline bool nuwax_speaker_natural_drain_preservable(
    uint32_t submitted_generation,
    uint32_t submitted_connection_epoch,
    uint32_t playback_generation,
    uint32_t playback_connection_epoch,
    bool drop_until_end,
    bool stream_active,
    uint32_t stream_generation,
    bool stream_end_accepted,
    bool stream_protocol_faulted,
    bool stop_fault_active,
    bool cancel_in_progress,
    uint32_t expected_connection_epoch,
    uint32_t expected_generation) {
    return !cancel_in_progress && !stream_protocol_faulted &&
           !stop_fault_active && stream_active && stream_end_accepted &&
           expected_connection_epoch != 0U &&
           submitted_connection_epoch == expected_connection_epoch &&
           playback_connection_epoch == expected_connection_epoch &&
           stream_generation == expected_generation &&
           nuwax_speaker_generation_is_current(submitted_generation,
                                               playback_generation,
                                               drop_until_end,
                                               expected_generation);
}

/* Select only a generation backed by live speaker state. A turn guard is an
 * ownership hint, not a current-playback snapshot, so a stale guard must not
 * override a newer generation already admitted by the speaker manager. */
static inline uint32_t nuwax_speaker_live_cancel_generation(
    uint32_t guard_generation,
    bool playback_active,
    bool playback_session_active,
    uint32_t playback_generation,
    bool start_gate_active,
    uint32_t start_gate_generation,
    bool rx_stream_active,
    uint32_t queue_waiting,
    uint32_t submitted_generation) {
    uint32_t live_generation = 0U;
    if ((playback_active || playback_session_active) &&
        playback_generation != 0U) {
        live_generation = playback_generation;
    } else if (start_gate_active && start_gate_generation != 0U) {
        live_generation = start_gate_generation;
    } else if ((rx_stream_active || queue_waiting > 0U) &&
               submitted_generation != 0U) {
        live_generation = submitted_generation;
    }
    return guard_generation != 0U && guard_generation == live_generation
               ? guard_generation
               : live_generation;
}

static inline uint32_t nuwax_speaker_unconditional_cancel_join_target(
    uint32_t completion_claim_generation,
    uint32_t cancel_pending_generation,
    uint32_t output_stop_owner_generation,
    uint32_t cancel_stopped_generation) {
    if (completion_claim_generation != 0U) {
        return completion_claim_generation;
    }
    if (cancel_stopped_generation != 0U &&
        (cancel_pending_generation != 0U ||
         output_stop_owner_generation != 0U)) {
        return cancel_stopped_generation;
    }
    return 0U;
}

static inline bool nuwax_speaker_probe_duck_allowed(
    uint32_t submitted_generation,
    uint32_t playback_generation,
    bool drop_until_end,
    bool output_ready,
    uint32_t expected_generation) {
    return output_ready &&
           nuwax_speaker_generation_is_current(submitted_generation,
                                               playback_generation,
                                               drop_until_end,
                                               expected_generation);
}

static inline uint16_t nuwax_speaker_probe_gain_q15(
    uint8_t attenuation_db) {
    /* 10^(-dB/20), Q15, for 12..18 dB. Invalid inputs fail to the
     * strongest attenuation rather than accidentally increasing output. */
    static const uint16_t gains_q15[] = {
        8231U, 7336U, 6538U, 5827U, 5194U, 4630U, 4125U,
    };
    if (attenuation_db < 12U || attenuation_db > 18U) {
        return gains_q15[6];
    }
    return gains_q15[attenuation_db - 12U];
}

static inline uint8_t nuwax_speaker_probe_output_percent(
    uint8_t logical_volume_percent,
    uint8_t attenuation_db) {
    const uint32_t scaled =
        (uint32_t)logical_volume_percent *
        nuwax_speaker_probe_gain_q15(attenuation_db);
    uint8_t output = (uint8_t)((scaled + 16384U) >> 15U);
    if (logical_volume_percent > 0U && output == 0U) {
        output = 1U;
    }
    return output;
}

static inline bool nuwax_speaker_write_claim_allowed(
    uint32_t submitted_generation,
    uint32_t playback_generation,
    bool drop_until_end,
    uint32_t io_claim_generation,
    uint32_t output_stop_owner_generation,
    uint32_t generation) {
    return nuwax_speaker_generation_is_current(submitted_generation,
                                               playback_generation,
                                               drop_until_end,
                                               generation) &&
           io_claim_generation == 0 &&
           output_stop_owner_generation == 0;
}

static inline bool nuwax_speaker_finish_claim_allowed(
    uint32_t submitted_generation,
    uint32_t playback_generation,
    bool drop_until_end,
    bool playback_active,
    uint32_t completion_claim_generation,
    uint32_t generation) {
    return playback_active && completion_claim_generation == 0 &&
           nuwax_speaker_generation_is_current(submitted_generation,
                                               playback_generation,
                                               drop_until_end,
                                               generation);
}

static inline nuwax_speaker_stop_claim_result_t
nuwax_speaker_stop_claim_decide(uint32_t submitted_generation,
                                uint32_t playback_generation,
                                bool drop_until_end,
                                uint32_t output_stop_owner_generation,
                                uint32_t cancel_generation) {
    if (cancel_generation == 0 ||
        cancel_generation != submitted_generation ||
        cancel_generation != playback_generation ||
        !drop_until_end) {
        return NUWAX_SPEAKER_STOP_SUPERSEDED;
    }
    if (output_stop_owner_generation != 0) {
        return NUWAX_SPEAKER_STOP_WAIT_OWNER;
    }
    return NUWAX_SPEAKER_STOP_CLAIMED;
}

static inline bool nuwax_speaker_stop_recovery_claim_allowed(
    uint32_t submitted_generation,
    uint32_t playback_generation,
    bool drop_until_end,
    uint32_t output_stop_owner_generation,
    uint32_t cancel_generation) {
    return nuwax_speaker_stop_claim_decide(submitted_generation,
                                           playback_generation,
                                           drop_until_end,
                                           output_stop_owner_generation,
                                           cancel_generation) ==
           NUWAX_SPEAKER_STOP_CLAIMED;
}

static inline bool nuwax_speaker_stop_owner_can_release(
    uint32_t output_stop_owner_generation,
    uint32_t cancel_generation) {
    return cancel_generation != 0 &&
           output_stop_owner_generation == cancel_generation;
}

static inline bool nuwax_speaker_completion_owner_can_release(
    uint32_t completion_claim_generation,
    uint32_t generation) {
    return generation != 0 && completion_claim_generation == generation;
}
