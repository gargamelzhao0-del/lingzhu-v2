#include "barge_in_controller.h"
#include "conversation_latency_contract.h"

#include <stddef.h>
#include <string.h>

#define NUWAX_BARGE_EVIDENCE_ALL                                      \
    (NUWAX_BARGE_EVIDENCE_VOICE_LIKE |                               \
     NUWAX_BARGE_EVIDENCE_DUAL_MIC_IMBALANCED |                      \
     NUWAX_BARGE_EVIDENCE_DUAL_MIC_SYMMETRIC |                       \
     NUWAX_BARGE_EVIDENCE_AEC_NEAREND |                              \
     NUWAX_BARGE_EVIDENCE_LOW_REF_CORRELATION)

static uint8_t saturating_increment_u8(uint8_t value) {
    return value == UINT8_MAX ? value : (uint8_t)(value + 1U);
}

static bool generation_is_newer(uint32_t candidate, uint32_t current) {
    return (int32_t)(candidate - current) > 0;
}

static bool health_valid(nuwax_barge_health_t health) {
    return health >= NUWAX_BARGE_HEALTH_RED &&
           health <= NUWAX_BARGE_HEALTH_GREEN;
}

static bool probe_observation_valid(
    nuwax_barge_probe_observation_t observation) {
    return observation >= NUWAX_BARGE_PROBE_NOT_AVAILABLE &&
           observation <= NUWAX_BARGE_PROBE_INCONCLUSIVE;
}

static nuwax_barge_volume_band_t volume_band(uint8_t volume_percent) {
    if (volume_percent <= 5U) {
        return NUWAX_BARGE_VOLUME_0_TO_5;
    }
    if (volume_percent <= 30U) {
        return NUWAX_BARGE_VOLUME_6_TO_30;
    }
    if (volume_percent <= 75U) {
        return NUWAX_BARGE_VOLUME_31_TO_75;
    }
    return NUWAX_BARGE_VOLUME_76_TO_100;
}

static bool config_valid(const nuwax_barge_controller_config_t *config) {
    if (!config || config->minimum_guard_frames < 4U ||
        config->reference_lock_frames < 3U ||
        config->reference_lock_frames > 5U ||
        config->active_aec_lock_frames < 16U ||
        config->active_aec_lock_frames > 50U ||
        config->reference_lock_deadline_frames < 15U ||
        config->imbalanced_confirm_frames < 2U ||
        config->symmetric_confirm_frames < 3U ||
        config->candidate_timeout_frames <
            config->symmetric_confirm_frames ||
        config->candidate_max_gap_frames > 1U ||
        config->candidate_max_gap_frames >=
            config->candidate_timeout_frames ||
        config->volume_hysteresis_frames < 10U ||
        config->volume_hysteresis_frames > 15U ||
        config->maximum_frame_age_ms == 0U ||
        config->maximum_timestamp_gap_ms < NUWAX_BARGE_CONTROLLER_FRAME_MS) {
        return false;
    }
    if (config->ambiguous_probe_enabled &&
        (config->probe_duration_frames < 3U ||
         config->probe_duration_frames > 5U ||
         config->probe_attenuation_db < 12U ||
         config->probe_attenuation_db > 18U)) {
        return false;
    }
    return true;
}

void nuwax_barge_controller_default_config(
    nuwax_barge_controller_config_t *config) {
    if (!config) {
        return;
    }
    *config = (nuwax_barge_controller_config_t){
        .minimum_guard_frames = 4U,
        .reference_lock_frames = 3U,
        .active_aec_lock_frames = 32U,
        .reference_lock_deadline_frames = 15U,
        .imbalanced_confirm_frames = 2U,
        .symmetric_confirm_frames = 3U,
        .candidate_timeout_frames = 6U,
        .candidate_max_gap_frames = 1U,
        .volume_hysteresis_frames = 10U,
        .ambiguous_probe_enabled = true,
        .probe_duration_frames = 4U,
        .probe_attenuation_db = 15U,
        .maximum_frame_age_ms =
            NUWAX_CONVERSATION_BARGE_END_TO_END_MAX_AGE_MS,
        .maximum_timestamp_gap_ms = 60U,
    };
}

bool nuwax_barge_controller_init(
    nuwax_barge_controller_t *controller,
    const nuwax_barge_controller_config_t *config) {
    if (!controller) {
        return false;
    }

    nuwax_barge_controller_config_t selected = {0};
    if (config) {
        selected = *config;
    } else {
        nuwax_barge_controller_default_config(&selected);
    }
    if (!config_valid(&selected)) {
        return false;
    }

    memset(controller, 0, sizeof(*controller));
    controller->config = selected;
    controller->state = NUWAX_BARGE_CONTROLLER_INACTIVE;
    controller->initialized = true;
    return true;
}

void nuwax_barge_controller_reset(nuwax_barge_controller_t *controller) {
    if (!controller || !controller->initialized) {
        return;
    }
    const nuwax_barge_controller_config_t config = controller->config;
    const uint32_t next_candidate_id = controller->next_candidate_id;
    memset(controller, 0, sizeof(*controller));
    controller->config = config;
    controller->next_candidate_id = next_candidate_id;
    controller->state = NUWAX_BARGE_CONTROLLER_INACTIVE;
    controller->initialized = true;
}

static void bind_playback(nuwax_barge_controller_t *controller,
                          uint32_t playback_generation,
                          uint32_t volume_generation,
                          uint8_t volume_percent,
                          uint64_t timestamp_us) {
    const nuwax_barge_controller_config_t config = controller->config;
    const uint32_t next_candidate_id = controller->next_candidate_id;
    memset(controller, 0, sizeof(*controller));
    controller->config = config;
    controller->next_candidate_id = next_candidate_id;
    controller->state = NUWAX_BARGE_CONTROLLER_PLAYBACK_ARMING;
    controller->active_volume_band = volume_band(volume_percent);
    controller->pending_volume_band = controller->active_volume_band;
    controller->playback_generation = playback_generation;
    controller->volume_generation = volume_generation;
    controller->volume_percent = volume_percent;
    controller->initialized = true;
    controller->playback_active = true;
    controller->timestamps.playback_started_valid = true;
    controller->timestamps.playback_started_timestamp_us = timestamp_us;
}

bool nuwax_barge_controller_begin_playback(
    nuwax_barge_controller_t *controller,
    uint32_t playback_generation,
    uint32_t volume_generation,
    uint8_t volume_percent,
    uint64_t timestamp_us) {
    if (!controller || !controller->initialized ||
        playback_generation == 0U || volume_percent > 100U) {
        return false;
    }
    bind_playback(controller,
                  playback_generation,
                  volume_generation,
                  volume_percent,
                  timestamp_us);
    return true;
}

static void result_begin(const nuwax_barge_controller_t *controller,
                         nuwax_barge_controller_result_t *result) {
    memset(result, 0, sizeof(*result));
    result->state_before = controller->state;
    result->state_after = controller->state;
    result->event_playback_generation = controller->playback_generation;
}

static void result_finish(const nuwax_barge_controller_t *controller,
                          nuwax_barge_controller_result_t *result) {
    result->state_after = controller->state;
}

static void clear_evidence_tracking(nuwax_barge_controller_t *controller) {
    controller->candidate_open = false;
    controller->candidate_generation = 0U;
    controller->candidate_start_sequence = 0U;
    controller->candidate_frames = 0U;
    controller->candidate_gap_frames = 0U;
    controller->imbalanced_run_frames = 0U;
    controller->symmetric_run_frames = 0U;
    controller->probe_frames = 0U;
}

static void clear_interaction_timestamps(
    nuwax_barge_controller_t *controller) {
    controller->timestamps.candidate_started_valid = false;
    controller->timestamps.confirmed_valid = false;
    controller->timestamps.local_stop_started_valid = false;
    controller->timestamps.playback_stopped_valid = false;
    controller->timestamps.uplink_active_valid = false;
    controller->timestamps.candidate_started_timestamp_us = 0U;
    controller->timestamps.confirmed_timestamp_us = 0U;
    controller->timestamps.local_stop_started_timestamp_us = 0U;
    controller->timestamps.playback_stopped_timestamp_us = 0U;
    controller->timestamps.uplink_active_timestamp_us = 0U;
}

static void emit_candidate_event(
    const nuwax_barge_controller_t *controller,
    nuwax_barge_controller_result_t *result,
    nuwax_barge_controller_event_t event,
    nuwax_barge_controller_reason_t reason,
    uint64_t timestamp_us) {
    result->event = event;
    result->reason = reason;
    result->event_playback_generation = controller->candidate_generation;
    result->candidate_id = controller->candidate_id;
    result->candidate_start_sequence = controller->candidate_start_sequence;
    result->candidate_started_timestamp_us =
        controller->timestamps.candidate_started_timestamp_us;
    result->event_timestamp_us = timestamp_us;
}

static void reject_candidate(nuwax_barge_controller_t *controller,
                             nuwax_barge_controller_result_t *result,
                             nuwax_barge_controller_reason_t reason,
                             uint64_t timestamp_us) {
    if (controller->candidate_open) {
        emit_candidate_event(controller,
                             result,
                             NUWAX_BARGE_EVENT_REJECTED,
                             reason,
                             timestamp_us);
    }
    clear_evidence_tracking(controller);
}

static void enter_degraded(nuwax_barge_controller_t *controller,
                           nuwax_barge_controller_result_t *result,
                           nuwax_barge_controller_state_t degraded_state,
                           nuwax_barge_controller_reason_t reason,
                           uint64_t timestamp_us) {
    const nuwax_barge_controller_state_t old_state = controller->state;
    if (controller->candidate_open) {
        reject_candidate(controller, result, reason, timestamp_us);
    } else if (old_state != degraded_state ||
               controller->degraded_reason != reason) {
        result->event = NUWAX_BARGE_EVENT_DEGRADED;
        result->reason = reason;
        result->event_playback_generation = controller->playback_generation;
        result->event_timestamp_us = timestamp_us;
    }
    controller->state = degraded_state;
    controller->degraded_reason = reason;
    controller->frames_since_context_change = 0U;
    controller->reference_stable_frames = 0U;
    controller->active_aec_stable_frames = 0U;
    controller->imbalanced_run_frames = 0U;
    controller->symmetric_run_frames = 0U;
    controller->probe_frames = 0U;
}

static bool state_has_terminal_decision(
    nuwax_barge_controller_state_t state) {
    return state == NUWAX_BARGE_CONTROLLER_STRICT_CONFIRMED ||
           state == NUWAX_BARGE_CONTROLLER_LOCAL_STOPPING ||
           state == NUWAX_BARGE_CONTROLLER_POST_STOP_TAIL ||
           state == NUWAX_BARGE_CONTROLLER_UPLINK_ACTIVE;
}

static bool frame_input_valid(const nuwax_barge_decision_frame_t *frame) {
    return frame && frame->playback_generation != 0U &&
           frame->volume_percent <= 100U &&
           health_valid(frame->aec_health) &&
           health_valid(frame->reference_health) &&
           probe_observation_valid(frame->probe_observation) &&
           (frame->evidence & ~NUWAX_BARGE_EVIDENCE_ALL) == 0U;
}

static bool update_volume_context(
    nuwax_barge_controller_t *controller,
    const nuwax_barge_decision_frame_t *frame,
    nuwax_barge_controller_result_t *result) {
    bool volume_changed = false;
    if (frame->volume_generation == controller->volume_generation) {
        if (frame->volume_percent != controller->volume_percent) {
            result->reason = NUWAX_BARGE_REASON_INVALID_INPUT;
            return false;
        }
    } else if (!generation_is_newer(frame->volume_generation,
                                    controller->volume_generation)) {
        result->reason = NUWAX_BARGE_REASON_STALE_VOLUME_GENERATION;
        return false;
    } else {
        volume_changed = true;
        if (controller->candidate_open) {
            reject_candidate(controller,
                             result,
                             NUWAX_BARGE_REASON_VOLUME_CHANGED,
                             frame->capture_timestamp_us);
        }
        controller->volume_generation = frame->volume_generation;
        controller->volume_percent = frame->volume_percent;
        const nuwax_barge_volume_band_t requested =
            volume_band(frame->volume_percent);
        if (requested >= controller->active_volume_band) {
            controller->active_volume_band = requested;
            controller->pending_volume_band_valid = false;
            controller->pending_volume_frames = 0U;
        } else {
            controller->pending_volume_band = requested;
            controller->pending_volume_band_valid = true;
            controller->pending_volume_frames = 1U;
        }
        controller->state = NUWAX_BARGE_CONTROLLER_PLAYBACK_ARMING;
        controller->degraded_reason = NUWAX_BARGE_REASON_NONE;
        controller->frames_since_context_change = 0U;
        controller->reference_stable_frames = 0U;
        controller->active_aec_stable_frames = 0U;
        controller->last_reference_generation = 0U;
        clear_interaction_timestamps(controller);
        if (result->event == NUWAX_BARGE_EVENT_NONE) {
            result->reason = NUWAX_BARGE_REASON_VOLUME_CHANGED;
        }
        if (result->event != NUWAX_BARGE_EVENT_NONE) {
            return false;
        }
    }

    if (!volume_changed && controller->pending_volume_band_valid &&
        frame->volume_generation == controller->volume_generation) {
        controller->pending_volume_frames =
            saturating_increment_u8(controller->pending_volume_frames);
        if (controller->pending_volume_frames >=
            controller->config.volume_hysteresis_frames) {
            controller->active_volume_band =
                controller->pending_volume_band;
            controller->pending_volume_band_valid = false;
            controller->pending_volume_frames = 0U;
        }
    }
    return true;
}

static bool update_timeline(nuwax_barge_controller_t *controller,
                            const nuwax_barge_decision_frame_t *frame,
                            nuwax_barge_controller_result_t *result) {
    if (frame->frame_age_ms > controller->config.maximum_frame_age_ms) {
        controller->last_sequence = frame->sequence;
        controller->last_capture_timestamp_us = frame->capture_timestamp_us;
        controller->timeline_initialized = true;
        enter_degraded(controller,
                       result,
                       NUWAX_BARGE_CONTROLLER_DEGRADED_REF,
                       NUWAX_BARGE_REASON_FRAME_STALE,
                       frame->capture_timestamp_us);
        return false;
    }

    if (controller->timeline_initialized) {
        const uint32_t sequence_delta = frame->sequence -
                                        controller->last_sequence;
        if (sequence_delta != 1U) {
            if (generation_is_newer(frame->sequence,
                                    controller->last_sequence)) {
                controller->last_sequence = frame->sequence;
                controller->last_capture_timestamp_us =
                    frame->capture_timestamp_us;
            }
            enter_degraded(controller,
                           result,
                           NUWAX_BARGE_CONTROLLER_DEGRADED_REF,
                           NUWAX_BARGE_REASON_SEQUENCE_DISCONTINUITY,
                           frame->capture_timestamp_us);
            return false;
        }
        if (frame->capture_timestamp_us <=
            controller->last_capture_timestamp_us) {
            controller->last_sequence = frame->sequence;
            enter_degraded(controller,
                           result,
                           NUWAX_BARGE_CONTROLLER_DEGRADED_REF,
                           NUWAX_BARGE_REASON_TIMESTAMP_DISCONTINUITY,
                           frame->capture_timestamp_us);
            return false;
        }
        const uint64_t timestamp_gap_us = frame->capture_timestamp_us -
                                          controller->last_capture_timestamp_us;
        if (timestamp_gap_us >
            (uint64_t)controller->config.maximum_timestamp_gap_ms * 1000ULL) {
            controller->last_sequence = frame->sequence;
            controller->last_capture_timestamp_us =
                frame->capture_timestamp_us;
            enter_degraded(controller,
                           result,
                           NUWAX_BARGE_CONTROLLER_DEGRADED_REF,
                           NUWAX_BARGE_REASON_TIMESTAMP_DISCONTINUITY,
                           frame->capture_timestamp_us);
            return false;
        }
    }

    controller->last_sequence = frame->sequence;
    controller->last_capture_timestamp_us = frame->capture_timestamp_us;
    controller->timeline_initialized = true;
    return true;
}

static void update_reference_stability(
    nuwax_barge_controller_t *controller,
    const nuwax_barge_decision_frame_t *frame) {
    const bool generation_matches =
        frame->reference_generation == controller->playback_generation;
    const bool same_reference_generation =
        controller->last_reference_generation == frame->reference_generation;
    if (generation_matches && frame->reference_contiguous &&
        (controller->last_reference_generation == 0U ||
         same_reference_generation)) {
        controller->reference_stable_frames =
            saturating_increment_u8(controller->reference_stable_frames);
    } else {
        controller->reference_stable_frames = 0U;
    }
    controller->last_reference_generation = frame->reference_generation;
}

static bool reference_ready(const nuwax_barge_controller_t *controller,
                            const nuwax_barge_decision_frame_t *frame) {
    if (frame->reference_generation != controller->playback_generation ||
        !frame->reference_contiguous ||
        controller->reference_stable_frames <
            controller->config.reference_lock_frames) {
        return false;
    }

    /* A silent reference cannot be correlated, but its generation must still
     * be continuous. This keeps low-volume human speech reachable. */
    if (!frame->reference_active ||
        controller->active_volume_band == NUWAX_BARGE_VOLUME_0_TO_5) {
        return true;
    }
    return frame->reference_locked;
}

static void update_active_aec_stability(
    nuwax_barge_controller_t *controller,
    const nuwax_barge_decision_frame_t *frame) {
    const bool generation_matches =
        frame->reference_generation == controller->playback_generation;
    if (frame->reference_active && !controller->active_reference_seen) {
        /* Playback metadata and capture can start well before audible TTS.
         * Begin the lock deadline and active-reference history at the first
         * real reference frame, not during the preceding silent wait. */
        controller->active_reference_seen = true;
        controller->frames_since_context_change = 1U;
        controller->reference_stable_frames =
            generation_matches && frame->reference_contiguous ? 1U : 0U;
        controller->active_aec_stable_frames = 0U;
    }
    const bool healthy_active_frame =
        generation_matches && frame->reference_contiguous &&
        frame->reference_active && frame->reference_locked &&
        frame->aec_frame_processed &&
        frame->aec_health == NUWAX_BARGE_HEALTH_GREEN &&
        frame->reference_health == NUWAX_BARGE_HEALTH_GREEN &&
        !frame->microphone_clipped && !frame->reference_clipped;

    if (healthy_active_frame) {
        controller->active_aec_stable_frames =
            saturating_increment_u8(controller->active_aec_stable_frames);
        return;
    }

    /* Preserve an established lock across genuine silence inside one TTS
     * generation. Active reference that bypasses AEC, changes generation or
     * breaks continuity invalidates the lock immediately. */
    if (frame->reference_active || !generation_matches ||
        !frame->reference_contiguous || frame->microphone_clipped ||
        frame->reference_clipped) {
        controller->active_aec_stable_frames = 0U;
    }
}

static bool active_aec_ready(
    const nuwax_barge_controller_t *controller,
    const nuwax_barge_decision_frame_t *frame) {
    if (controller->active_volume_band == NUWAX_BARGE_VOLUME_0_TO_5 &&
        !frame->reference_active) {
        return true;
    }
    return controller->active_aec_stable_frames >=
           controller->config.active_aec_lock_frames;
}

static bool has_human_base(nuwax_barge_evidence_t evidence) {
    const bool voice = (evidence & NUWAX_BARGE_EVIDENCE_VOICE_LIKE) != 0U;
    const bool spatial =
        (evidence & (NUWAX_BARGE_EVIDENCE_DUAL_MIC_IMBALANCED |
                     NUWAX_BARGE_EVIDENCE_DUAL_MIC_SYMMETRIC)) != 0U;
    return voice && spatial;
}

static uint8_t independent_evidence_classes(
    nuwax_barge_evidence_t evidence) {
    uint8_t classes = 0U;
    if ((evidence & (NUWAX_BARGE_EVIDENCE_DUAL_MIC_IMBALANCED |
                     NUWAX_BARGE_EVIDENCE_DUAL_MIC_SYMMETRIC)) != 0U) {
        classes++;
    }
    if ((evidence & NUWAX_BARGE_EVIDENCE_AEC_NEAREND) != 0U) {
        classes++;
    }
    if ((evidence & NUWAX_BARGE_EVIDENCE_LOW_REF_CORRELATION) != 0U) {
        classes++;
    }
    return classes;
}

static void start_candidate(nuwax_barge_controller_t *controller,
                            const nuwax_barge_decision_frame_t *frame,
                            nuwax_barge_controller_result_t *result) {
    controller->next_candidate_id++;
    if (controller->next_candidate_id == 0U) {
        controller->next_candidate_id = 1U;
    }
    controller->candidate_id = controller->next_candidate_id;
    controller->candidate_generation = controller->playback_generation;
    controller->candidate_start_sequence = frame->sequence;
    controller->candidate_frames = 1U;
    controller->candidate_gap_frames = 0U;
    controller->candidate_open = true;
    controller->imbalanced_run_frames =
        (frame->evidence & NUWAX_BARGE_EVIDENCE_DUAL_MIC_IMBALANCED) != 0U
            ? 1U
            : 0U;
    controller->symmetric_run_frames =
        (frame->evidence & NUWAX_BARGE_EVIDENCE_DUAL_MIC_SYMMETRIC) != 0U
            ? 1U
            : 0U;
    controller->state = NUWAX_BARGE_CONTROLLER_HUMAN_CANDIDATE;
    controller->timestamps.candidate_started_valid = true;
    controller->timestamps.candidate_started_timestamp_us =
        frame->capture_timestamp_us;
    emit_candidate_event(controller,
                         result,
                         NUWAX_BARGE_EVENT_CANDIDATE_STARTED,
                         NUWAX_BARGE_REASON_HUMAN_EVIDENCE,
                         frame->capture_timestamp_us);
}

static void update_candidate_runs(nuwax_barge_controller_t *controller,
                                  const nuwax_barge_decision_frame_t *frame) {
    controller->candidate_frames =
        saturating_increment_u8(controller->candidate_frames);
    controller->candidate_gap_frames = 0U;
    controller->imbalanced_run_frames =
        (frame->evidence & NUWAX_BARGE_EVIDENCE_DUAL_MIC_IMBALANCED) != 0U
            ? saturating_increment_u8(controller->imbalanced_run_frames)
            : 0U;
    controller->symmetric_run_frames =
        (frame->evidence & NUWAX_BARGE_EVIDENCE_DUAL_MIC_SYMMETRIC) != 0U
            ? saturating_increment_u8(controller->symmetric_run_frames)
            : 0U;
}

static bool candidate_continuity_ready(
    const nuwax_barge_controller_t *controller) {
    return controller->imbalanced_run_frames >=
               controller->config.imbalanced_confirm_frames ||
           controller->symmetric_run_frames >=
               controller->config.symmetric_confirm_frames;
}

static bool strict_policy_required(
    const nuwax_barge_controller_t *controller,
    const nuwax_barge_decision_frame_t *frame) {
    const bool verified_low_volume_silence =
        controller->active_volume_band == NUWAX_BARGE_VOLUME_0_TO_5 &&
        frame->verified_silent_passthrough && !frame->reference_active &&
        frame->reference_contiguous &&
        frame->reference_health == NUWAX_BARGE_HEALTH_GREEN;
    return controller->active_volume_band ==
               NUWAX_BARGE_VOLUME_76_TO_100 ||
           (frame->aec_health == NUWAX_BARGE_HEALTH_YELLOW &&
            !verified_low_volume_silence) ||
           frame->reference_health == NUWAX_BARGE_HEALTH_YELLOW ||
           frame->microphone_clipped || frame->reference_clipped;
}

static void confirm_candidate(nuwax_barge_controller_t *controller,
                              nuwax_barge_controller_result_t *result,
                              nuwax_barge_controller_reason_t reason,
                              uint64_t timestamp_us) {
    emit_candidate_event(controller,
                         result,
                         NUWAX_BARGE_EVENT_CONFIRMED,
                         reason,
                         timestamp_us);
    controller->candidate_open = false;
    controller->state = NUWAX_BARGE_CONTROLLER_STRICT_CONFIRMED;
    controller->timestamps.confirmed_valid = true;
    controller->timestamps.confirmed_timestamp_us = timestamp_us;
    result->request_local_stop = true;
}

static void request_probe(nuwax_barge_controller_t *controller,
                          nuwax_barge_controller_result_t *result,
                          uint64_t timestamp_us) {
    controller->state = NUWAX_BARGE_CONTROLLER_AMBIGUOUS_PROBE;
    controller->probe_frames = 0U;
    emit_candidate_event(controller,
                         result,
                         NUWAX_BARGE_EVENT_PROBE_REQUESTED,
                         NUWAX_BARGE_REASON_ECHO_CONFLICT,
                         timestamp_us);
    result->request_probe = true;
    result->probe_attenuation_db =
        controller->config.probe_attenuation_db;
    result->probe_duration_ms =
        (uint16_t)controller->config.probe_duration_frames *
        NUWAX_BARGE_CONTROLLER_FRAME_MS;
}

static void process_probe_frame(nuwax_barge_controller_t *controller,
                                const nuwax_barge_decision_frame_t *frame,
                                nuwax_barge_controller_result_t *result) {
    controller->probe_frames =
        saturating_increment_u8(controller->probe_frames);
    if (frame->probe_observation ==
        NUWAX_BARGE_PROBE_ECHO_TRACKS_DUCK) {
        reject_candidate(controller,
                         result,
                         NUWAX_BARGE_REASON_PROBE_ECHO_TRACKS,
                         frame->capture_timestamp_us);
        /* The output gain is about to return from the probe attenuation.
         * Re-arm reference and timing guards before admitting a new
         * candidate so the restore transient cannot self-trigger. */
        controller->state = NUWAX_BARGE_CONTROLLER_PLAYBACK_ARMING;
        controller->frames_since_context_change = 0U;
        controller->reference_stable_frames = 0U;
        controller->active_aec_stable_frames = 0U;
        controller->last_reference_generation = 0U;
        return;
    }
    if (frame->probe_observation == NUWAX_BARGE_PROBE_HUMAN_PERSISTS &&
        has_human_base(frame->evidence) && !frame->echo_candidate) {
        confirm_candidate(controller,
                          result,
                          NUWAX_BARGE_REASON_PROBE_HUMAN_PERSISTS,
                          frame->capture_timestamp_us);
        return;
    }
    if (controller->probe_frames >=
        controller->config.probe_duration_frames) {
        reject_candidate(controller,
                         result,
                         NUWAX_BARGE_REASON_PROBE_TIMEOUT,
                         frame->capture_timestamp_us);
        controller->state = NUWAX_BARGE_CONTROLLER_PLAYBACK_ARMING;
        controller->frames_since_context_change = 0U;
        controller->reference_stable_frames = 0U;
        controller->active_aec_stable_frames = 0U;
        controller->last_reference_generation = 0U;
    }
}

bool nuwax_barge_controller_process_frame(
    nuwax_barge_controller_t *controller,
    const nuwax_barge_decision_frame_t *frame,
    nuwax_barge_controller_result_t *result) {
    if (!controller || !result) {
        return false;
    }
    if (!controller->initialized) {
        memset(result, 0, sizeof(*result));
        result->reason = NUWAX_BARGE_REASON_INVALID_INPUT;
        return false;
    }
    result_begin(controller, result);
    if (!controller->playback_active || !frame_input_valid(frame)) {
        result->reason = NUWAX_BARGE_REASON_INVALID_INPUT;
        return false;
    }

    if (frame->playback_generation != controller->playback_generation) {
        if (!generation_is_newer(frame->playback_generation,
                                 controller->playback_generation)) {
            result->reason = NUWAX_BARGE_REASON_STALE_GENERATION;
            return true;
        }
        if (controller->candidate_open) {
            reject_candidate(controller,
                             result,
                             NUWAX_BARGE_REASON_GENERATION_SUPERSEDED,
                             frame->capture_timestamp_us);
        }
        bind_playback(controller,
                      frame->playback_generation,
                      frame->volume_generation,
                      frame->volume_percent,
                      frame->capture_timestamp_us);
        if (result->event != NUWAX_BARGE_EVENT_NONE) {
            result_finish(controller, result);
            return true;
        }
        result->reason = NUWAX_BARGE_REASON_GENERATION_SUPERSEDED;
    }

    if (!update_volume_context(controller, frame, result)) {
        result_finish(controller, result);
        return result->event != NUWAX_BARGE_EVENT_NONE;
    }

    if (state_has_terminal_decision(controller->state)) {
        result_finish(controller, result);
        return true;
    }

    if (!update_timeline(controller, frame, result)) {
        result_finish(controller, result);
        return true;
    }
    controller->frames_since_context_change =
        saturating_increment_u8(controller->frames_since_context_change);
    update_reference_stability(controller, frame);
    update_active_aec_stability(controller, frame);

    if (frame->aec_health == NUWAX_BARGE_HEALTH_RED) {
        enter_degraded(controller,
                       result,
                       NUWAX_BARGE_CONTROLLER_DEGRADED_AEC,
                       NUWAX_BARGE_REASON_AEC_RED,
                       frame->capture_timestamp_us);
        result_finish(controller, result);
        return true;
    }
    if (frame->reference_health == NUWAX_BARGE_HEALTH_RED) {
        enter_degraded(controller,
                       result,
                       NUWAX_BARGE_CONTROLLER_DEGRADED_REF,
                       NUWAX_BARGE_REASON_REFERENCE_RED,
                       frame->capture_timestamp_us);
        result_finish(controller, result);
        return true;
    }

    const bool ref_ready = reference_ready(controller, frame);
    if (!ref_ready) {
        const nuwax_barge_controller_reason_t reason =
            frame->reference_generation != controller->playback_generation
                ? NUWAX_BARGE_REASON_REFERENCE_GENERATION
                : NUWAX_BARGE_REASON_REFERENCE_UNLOCKED;
        const bool was_armed =
            controller->state != NUWAX_BARGE_CONTROLLER_PLAYBACK_ARMING &&
            controller->state != NUWAX_BARGE_CONTROLLER_DEGRADED_REF &&
            controller->state != NUWAX_BARGE_CONTROLLER_DEGRADED_AEC;
        if (controller->state == NUWAX_BARGE_CONTROLLER_DEGRADED_REF ||
            controller->state == NUWAX_BARGE_CONTROLLER_DEGRADED_AEC) {
            /* Stay fail-closed until both the guard and reference lock recover. */
        } else if (was_armed ||
            controller->frames_since_context_change >=
                controller->config.reference_lock_deadline_frames) {
            enter_degraded(controller,
                           result,
                           NUWAX_BARGE_CONTROLLER_DEGRADED_REF,
                           reason,
                           frame->capture_timestamp_us);
        }
        if (result->reason == NUWAX_BARGE_REASON_NONE) {
            result->reason = reason;
        }
        result_finish(controller, result);
        return true;
    }

    if (controller->frames_since_context_change <
        controller->config.minimum_guard_frames) {
        if (controller->state != NUWAX_BARGE_CONTROLLER_DEGRADED_REF &&
            controller->state != NUWAX_BARGE_CONTROLLER_DEGRADED_AEC) {
            controller->state = NUWAX_BARGE_CONTROLLER_PLAYBACK_ARMING;
        }
        result_finish(controller, result);
        return true;
    }

    if (!active_aec_ready(controller, frame)) {
        if (controller->candidate_open) {
            reject_candidate(controller,
                             result,
                             NUWAX_BARGE_REASON_AEC_STABILIZING,
                             frame->capture_timestamp_us);
        }
        if (controller->state != NUWAX_BARGE_CONTROLLER_DEGRADED_REF &&
            controller->state != NUWAX_BARGE_CONTROLLER_DEGRADED_AEC) {
            controller->state = NUWAX_BARGE_CONTROLLER_PLAYBACK_ARMING;
        }
        if (result->reason == NUWAX_BARGE_REASON_NONE) {
            result->reason = NUWAX_BARGE_REASON_AEC_STABILIZING;
        }
        result_finish(controller, result);
        return true;
    }

    if (controller->state == NUWAX_BARGE_CONTROLLER_PLAYBACK_ARMING ||
        controller->state == NUWAX_BARGE_CONTROLLER_DEGRADED_REF ||
        controller->state == NUWAX_BARGE_CONTROLLER_DEGRADED_AEC) {
        const bool recovered =
            controller->state == NUWAX_BARGE_CONTROLLER_DEGRADED_REF ||
            controller->state == NUWAX_BARGE_CONTROLLER_DEGRADED_AEC;
        controller->state = NUWAX_BARGE_CONTROLLER_ECHO_TRACKING;
        controller->degraded_reason = NUWAX_BARGE_REASON_NONE;
        if (recovered && result->event == NUWAX_BARGE_EVENT_NONE) {
            result->event = NUWAX_BARGE_EVENT_REARMED;
            result->event_playback_generation =
                controller->playback_generation;
            result->event_timestamp_us = frame->capture_timestamp_us;
        }
    } else if (controller->state == NUWAX_BARGE_CONTROLLER_REJECTED) {
        controller->state = NUWAX_BARGE_CONTROLLER_ECHO_TRACKING;
    }

    if (controller->state == NUWAX_BARGE_CONTROLLER_AMBIGUOUS_PROBE) {
        process_probe_frame(controller, frame, result);
        result_finish(controller, result);
        return true;
    }

    const bool human_base = has_human_base(frame->evidence);
    if (controller->state == NUWAX_BARGE_CONTROLLER_ECHO_TRACKING) {
        if (human_base) {
            start_candidate(controller, frame, result);
        }
        result_finish(controller, result);
        return true;
    }

    if (controller->state != NUWAX_BARGE_CONTROLLER_HUMAN_CANDIDATE) {
        result_finish(controller, result);
        return true;
    }
    if (!human_base) {
        controller->candidate_frames =
            saturating_increment_u8(controller->candidate_frames);
        controller->candidate_gap_frames =
            saturating_increment_u8(controller->candidate_gap_frames);
        if (!frame->echo_candidate &&
            controller->candidate_gap_frames <=
                controller->config.candidate_max_gap_frames &&
            controller->candidate_frames <
                controller->config.candidate_timeout_frames) {
            result_finish(controller, result);
            return true;
        }
        reject_candidate(controller,
                         result,
                         controller->candidate_frames >=
                                 controller->config.candidate_timeout_frames
                             ? NUWAX_BARGE_REASON_EVIDENCE_TIMEOUT
                             : NUWAX_BARGE_REASON_EVIDENCE_LOST,
                         frame->capture_timestamp_us);
        controller->state = NUWAX_BARGE_CONTROLLER_REJECTED;
        result_finish(controller, result);
        return true;
    }

    update_candidate_runs(controller, frame);
    if (candidate_continuity_ready(controller)) {
        const bool strict = strict_policy_required(controller, frame);
        /* At 76-100% output, loudspeaker non-linearity can make spatial,
         * AEC-nearend and reference-correlation evidence fail together. They
         * are therefore not independent enough to stop playback directly. */
        const bool high_volume_probe_required =
            controller->active_volume_band ==
            NUWAX_BARGE_VOLUME_76_TO_100;
        const bool independent =
            independent_evidence_classes(frame->evidence) >= 2U;
        if (!frame->echo_candidate && !high_volume_probe_required &&
            (!strict || independent)) {
            confirm_candidate(controller,
                              result,
                              strict
                                  ? NUWAX_BARGE_REASON_STRICT_HUMAN_EVIDENCE
                                  : NUWAX_BARGE_REASON_HUMAN_EVIDENCE,
                              frame->capture_timestamp_us);
            result_finish(controller, result);
            return true;
        }
        if ((strict || high_volume_probe_required) &&
            controller->config.ambiguous_probe_enabled) {
            request_probe(controller, result, frame->capture_timestamp_us);
            result_finish(controller, result);
            return true;
        }
    }

    if (controller->candidate_frames >=
        controller->config.candidate_timeout_frames) {
        reject_candidate(controller,
                         result,
                         frame->echo_candidate
                             ? NUWAX_BARGE_REASON_ECHO_CONFLICT
                             : NUWAX_BARGE_REASON_EVIDENCE_TIMEOUT,
                         frame->capture_timestamp_us);
        controller->state = NUWAX_BARGE_CONTROLLER_REJECTED;
    }
    result_finish(controller, result);
    return true;
}

bool nuwax_barge_controller_mark_local_stop_started(
    nuwax_barge_controller_t *controller,
    uint32_t playback_generation,
    uint32_t candidate_id,
    uint64_t timestamp_us) {
    if (!controller || !controller->initialized ||
        controller->state != NUWAX_BARGE_CONTROLLER_STRICT_CONFIRMED ||
        playback_generation != controller->playback_generation ||
        candidate_id == 0U || candidate_id != controller->candidate_id ||
        !controller->timestamps.confirmed_valid ||
        timestamp_us < controller->timestamps.confirmed_timestamp_us) {
        return false;
    }
    controller->state = NUWAX_BARGE_CONTROLLER_LOCAL_STOPPING;
    controller->timestamps.local_stop_started_valid = true;
    controller->timestamps.local_stop_started_timestamp_us = timestamp_us;
    return true;
}

bool nuwax_barge_controller_mark_playback_stopped(
    nuwax_barge_controller_t *controller,
    uint32_t playback_generation,
    uint32_t candidate_id,
    uint64_t timestamp_us) {
    if (!controller || !controller->initialized ||
        controller->state != NUWAX_BARGE_CONTROLLER_LOCAL_STOPPING ||
        playback_generation != controller->playback_generation ||
        candidate_id == 0U || candidate_id != controller->candidate_id ||
        !controller->timestamps.local_stop_started_valid ||
        timestamp_us <
            controller->timestamps.local_stop_started_timestamp_us) {
        return false;
    }
    controller->state = NUWAX_BARGE_CONTROLLER_POST_STOP_TAIL;
    controller->playback_active = false;
    controller->timestamps.playback_stopped_valid = true;
    controller->timestamps.playback_stopped_timestamp_us = timestamp_us;
    return true;
}

bool nuwax_barge_controller_mark_uplink_active(
    nuwax_barge_controller_t *controller,
    uint32_t playback_generation,
    uint32_t candidate_id,
    uint64_t timestamp_us) {
    if (!controller || !controller->initialized ||
        controller->state != NUWAX_BARGE_CONTROLLER_POST_STOP_TAIL ||
        playback_generation != controller->playback_generation ||
        candidate_id == 0U || candidate_id != controller->candidate_id ||
        !controller->timestamps.playback_stopped_valid ||
        timestamp_us < controller->timestamps.playback_stopped_timestamp_us) {
        return false;
    }
    controller->state = NUWAX_BARGE_CONTROLLER_UPLINK_ACTIVE;
    controller->timestamps.uplink_active_valid = true;
    controller->timestamps.uplink_active_timestamp_us = timestamp_us;
    return true;
}

nuwax_barge_controller_state_t nuwax_barge_controller_state(
    const nuwax_barge_controller_t *controller) {
    return controller && controller->initialized
               ? controller->state
               : NUWAX_BARGE_CONTROLLER_INACTIVE;
}

nuwax_barge_volume_band_t nuwax_barge_controller_volume_band(
    const nuwax_barge_controller_t *controller) {
    return controller && controller->initialized
               ? controller->active_volume_band
               : NUWAX_BARGE_VOLUME_0_TO_5;
}

const nuwax_barge_controller_timestamps_t *nuwax_barge_controller_timestamps(
    const nuwax_barge_controller_t *controller) {
    return controller && controller->initialized
               ? &controller->timestamps
               : NULL;
}
