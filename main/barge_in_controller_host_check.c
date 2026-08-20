#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "conversation/barge_in_controller.h"
#include "conversation/conversation_latency_contract.h"

#define CHECK(condition, message)                                      \
    do {                                                               \
        if (!(condition)) {                                            \
            printf("[FAIL] %s:%d: %s\n", __func__, __LINE__, message); \
            return 1;                                                  \
        }                                                              \
    } while (0)

#define HUMAN_IMBALANCED                                                \
    (NUWAX_BARGE_EVIDENCE_VOICE_LIKE |                                 \
     NUWAX_BARGE_EVIDENCE_DUAL_MIC_IMBALANCED)

#define HUMAN_SYMMETRIC                                                 \
    (NUWAX_BARGE_EVIDENCE_VOICE_LIKE |                                 \
     NUWAX_BARGE_EVIDENCE_DUAL_MIC_SYMMETRIC)

#define HUMAN_STRICT                                                    \
    (HUMAN_IMBALANCED | NUWAX_BARGE_EVIDENCE_LOW_REF_CORRELATION)

static nuwax_barge_decision_frame_t healthy_frame(uint32_t generation,
                                                   uint32_t volume_generation,
                                                   uint8_t volume,
                                                   uint32_t sequence,
                                                   uint64_t timestamp_us) {
    return (nuwax_barge_decision_frame_t){
        .playback_generation = generation,
        .volume_generation = volume_generation,
        .reference_generation = generation,
        .sequence = sequence,
        .capture_timestamp_us = timestamp_us,
        .frame_age_ms = 0U,
        .volume_percent = volume,
        .aec_health = NUWAX_BARGE_HEALTH_GREEN,
        .reference_health = NUWAX_BARGE_HEALTH_GREEN,
        .reference_active = volume > 0U,
        .reference_locked = volume > 0U,
        .reference_contiguous = true,
        .aec_frame_processed = true,
        .probe_observation = NUWAX_BARGE_PROBE_NOT_AVAILABLE,
    };
}

static bool begin_controller(nuwax_barge_controller_t *controller,
                             uint32_t generation,
                             uint32_t volume_generation,
                             uint8_t volume) {
    return nuwax_barge_controller_init(controller, NULL) &&
           nuwax_barge_controller_begin_playback(controller,
                                                 generation,
                                                 volume_generation,
                                                 volume,
                                                 0U);
}

static int arm_controller(nuwax_barge_controller_t *controller,
                          uint32_t generation,
                          uint32_t volume_generation,
                          uint8_t volume,
                          uint32_t *sequence,
                          uint64_t *timestamp_us) {
    nuwax_barge_controller_result_t result = {0};
    const uint32_t required_frames =
        controller->config.active_aec_lock_frames >
                controller->config.minimum_guard_frames
            ? controller->config.active_aec_lock_frames
            : controller->config.minimum_guard_frames;
    for (uint32_t i = 0; i < required_frames; i++) {
        (*sequence)++;
        *timestamp_us += 20000U;
        nuwax_barge_decision_frame_t frame =
            healthy_frame(generation,
                          volume_generation,
                          volume,
                          *sequence,
                          *timestamp_us);
        CHECK(nuwax_barge_controller_process_frame(controller,
                                                   &frame,
                                                   &result),
              "healthy arming frame was rejected");
        CHECK(!result.request_local_stop,
              "minimum arming guard requested a local stop");
        if (i + 1U < required_frames) {
            CHECK(nuwax_barge_controller_state(controller) ==
                      NUWAX_BARGE_CONTROLLER_PLAYBACK_ARMING,
                  "arming guard opened before active AEC stabilization");
        }
    }
    CHECK(nuwax_barge_controller_state(controller) ==
              NUWAX_BARGE_CONTROLLER_ECHO_TRACKING,
          "controller did not enter echo tracking after lock and guard");
    return 0;
}

static int check_default_config_and_validation(void) {
    nuwax_barge_controller_config_t config = {0};
    nuwax_barge_controller_default_config(&config);
    CHECK(config.minimum_guard_frames == 4U,
          "default minimum guard is not 80 ms");
    CHECK(config.reference_lock_frames == 3U,
          "default reference lock is not three frames");
    CHECK(config.active_aec_lock_frames == 32U,
          "default active AEC stabilization is not 32 frames");
    CHECK(config.reference_lock_deadline_frames == 15U,
          "default reference lock deadline is not 300 ms");
    CHECK(config.candidate_max_gap_frames == 1U,
          "default candidate gap tolerance is not one frame");
    CHECK(config.maximum_frame_age_ms ==
              NUWAX_CONVERSATION_BARGE_END_TO_END_MAX_AGE_MS,
          "default post-DSP decision age is not 100 ms");
    CHECK(config.volume_hysteresis_frames == 10U,
          "default volume hysteresis is not 200 ms");
    CHECK(config.probe_duration_frames == 4U &&
              config.probe_attenuation_db == 15U,
          "default duck probe is not 80 ms / 15 dB");

    nuwax_barge_controller_t controller = {0};
    config.minimum_guard_frames = 3U;
    CHECK(!nuwax_barge_controller_init(&controller, &config),
          "unsafe sub-80-ms guard was accepted");
    nuwax_barge_controller_default_config(&config);
    config.active_aec_lock_frames = 15U;
    CHECK(!nuwax_barge_controller_init(&controller, &config),
          "unsafe active AEC stabilization was accepted");
    nuwax_barge_controller_default_config(&config);
    config.probe_attenuation_db = 11U;
    CHECK(!nuwax_barge_controller_init(&controller, &config),
          "probe attenuation outside 12-18 dB was accepted");
    nuwax_barge_controller_default_config(&config);
    config.candidate_max_gap_frames = 2U;
    CHECK(!nuwax_barge_controller_init(&controller, &config),
          "unsafe multi-frame evidence gap was accepted");
    return 0;
}

static int check_medium_confirmation_and_execution_chain(void) {
    nuwax_barge_controller_t controller = {0};
    nuwax_barge_controller_result_t result = {0};
    uint32_t sequence = 0U;
    uint64_t timestamp_us = 0U;
    CHECK(begin_controller(&controller, 7U, 11U, 50U),
          "controller did not start");
    CHECK(arm_controller(&controller,
                         7U,
                         11U,
                         50U,
                         &sequence,
                         &timestamp_us) == 0,
          "controller did not arm");

    sequence++;
    timestamp_us += 20000U;
    nuwax_barge_decision_frame_t frame =
        healthy_frame(7U, 11U, 50U, sequence, timestamp_us);
    frame.evidence = HUMAN_IMBALANCED;
    CHECK(nuwax_barge_controller_process_frame(&controller, &frame, &result),
          "candidate frame failed");
    CHECK(result.event == NUWAX_BARGE_EVENT_CANDIDATE_STARTED &&
              !result.request_local_stop,
          "candidate altered playback before confirmation");
    const uint32_t candidate_id = result.candidate_id;
    const uint32_t candidate_start_sequence = sequence;
    const uint64_t candidate_started_timestamp_us = timestamp_us;
    CHECK(candidate_id != 0U &&
              result.candidate_started_timestamp_us == timestamp_us,
          "candidate identity or onset timestamp was not retained");

    sequence++;
    timestamp_us += 20000U;
    frame = healthy_frame(7U, 11U, 50U, sequence, timestamp_us);
    frame.evidence = HUMAN_IMBALANCED;
    CHECK(nuwax_barge_controller_process_frame(&controller, &frame, &result),
          "confirmation frame failed");
    CHECK(result.event == NUWAX_BARGE_EVENT_CONFIRMED &&
              result.reason == NUWAX_BARGE_REASON_HUMAN_EVIDENCE &&
              result.request_local_stop &&
              result.candidate_id == candidate_id &&
              result.candidate_start_sequence == candidate_start_sequence,
          "two-frame medium-volume confirmation was incorrect");
    CHECK(nuwax_barge_controller_state(&controller) ==
              NUWAX_BARGE_CONTROLLER_STRICT_CONFIRMED,
          "confirmed state was not retained for the executor");

    sequence++;
    timestamp_us += 20000U;
    frame = healthy_frame(7U, 11U, 50U, sequence, timestamp_us);
    frame.aec_health = NUWAX_BARGE_HEALTH_RED;
    CHECK(nuwax_barge_controller_process_frame(&controller, &frame, &result),
          "post-confirmation frame failed");
    CHECK(result.event == NUWAX_BARGE_EVENT_NONE &&
              nuwax_barge_controller_state(&controller) ==
                  NUWAX_BARGE_CONTROLLER_STRICT_CONFIRMED,
          "confirmed candidate later emitted a contradictory rejection");

    CHECK(!nuwax_barge_controller_mark_local_stop_started(
              &controller, 8U, candidate_id, timestamp_us + 1000U),
          "wrong playback generation claimed local stop");
    CHECK(!nuwax_barge_controller_mark_local_stop_started(
              &controller, 7U, candidate_id + 1U, timestamp_us + 1000U),
          "wrong candidate claimed local stop");
    CHECK(nuwax_barge_controller_mark_local_stop_started(
              &controller, 7U, candidate_id, timestamp_us + 1000U),
          "current generation could not start local stop");
    CHECK(nuwax_barge_controller_state(&controller) ==
              NUWAX_BARGE_CONTROLLER_LOCAL_STOPPING,
          "LOCAL_STOPPING was not entered");
    CHECK(nuwax_barge_controller_mark_playback_stopped(
              &controller, 7U, candidate_id, timestamp_us + 4000U),
          "playback stop boundary was not accepted");
    CHECK(nuwax_barge_controller_state(&controller) ==
              NUWAX_BARGE_CONTROLLER_POST_STOP_TAIL,
          "POST_STOP_TAIL was not entered");
    CHECK(nuwax_barge_controller_mark_uplink_active(
              &controller, 7U, candidate_id, timestamp_us + 5000U),
          "uplink boundary was not accepted");
    CHECK(nuwax_barge_controller_state(&controller) ==
              NUWAX_BARGE_CONTROLLER_UPLINK_ACTIVE,
          "UPLINK_ACTIVE was not entered");

    const nuwax_barge_controller_timestamps_t *timestamps =
        nuwax_barge_controller_timestamps(&controller);
    CHECK(timestamps && timestamps->candidate_started_valid &&
              timestamps->confirmed_valid &&
              timestamps->local_stop_started_valid &&
              timestamps->playback_stopped_valid &&
              timestamps->uplink_active_valid,
          "execution timestamps are incomplete");
    CHECK(timestamps->candidate_started_timestamp_us ==
                  candidate_started_timestamp_us &&
              timestamps->confirmed_timestamp_us ==
                  candidate_started_timestamp_us + 20000U &&
              timestamps->local_stop_started_timestamp_us ==
                  candidate_started_timestamp_us + 41000U &&
              timestamps->playback_stopped_timestamp_us ==
                  candidate_started_timestamp_us + 44000U &&
              timestamps->uplink_active_timestamp_us ==
                  candidate_started_timestamp_us + 45000U,
          "execution timestamps changed or lost ordering");
    return 0;
}

static int check_single_frame_candidate_gap_tolerance(void) {
    nuwax_barge_controller_t controller = {0};
    nuwax_barge_controller_result_t result = {0};
    uint32_t sequence = 0U;
    uint64_t timestamp_us = 0U;
    CHECK(begin_controller(&controller, 8U, 1U, 50U),
          "gap-tolerant controller did not start");
    CHECK(arm_controller(&controller,
                         8U,
                         1U,
                         50U,
                         &sequence,
                         &timestamp_us) == 0,
          "gap-tolerant controller did not arm");

    sequence++;
    timestamp_us += 20000U;
    nuwax_barge_decision_frame_t frame =
        healthy_frame(8U, 1U, 50U, sequence, timestamp_us);
    frame.evidence = HUMAN_IMBALANCED;
    CHECK(nuwax_barge_controller_process_frame(&controller, &frame, &result) &&
              result.event == NUWAX_BARGE_EVENT_CANDIDATE_STARTED,
          "candidate did not start before the evidence gap");

    sequence++;
    timestamp_us += 20000U;
    frame = healthy_frame(8U, 1U, 50U, sequence, timestamp_us);
    CHECK(nuwax_barge_controller_process_frame(&controller, &frame, &result) &&
              result.event == NUWAX_BARGE_EVENT_NONE &&
              nuwax_barge_controller_state(&controller) ==
                  NUWAX_BARGE_CONTROLLER_HUMAN_CANDIDATE,
          "one clean 20-ms evidence gap rejected the human candidate");

    sequence++;
    timestamp_us += 20000U;
    frame = healthy_frame(8U, 1U, 50U, sequence, timestamp_us);
    frame.evidence = HUMAN_IMBALANCED;
    CHECK(nuwax_barge_controller_process_frame(&controller, &frame, &result) &&
              result.event == NUWAX_BARGE_EVENT_CONFIRMED &&
              result.request_local_stop,
          "human evidence did not confirm after one clean gap");

    CHECK(begin_controller(&controller, 9U, 1U, 50U),
          "echo-gap controller did not start");
    sequence = 0U;
    timestamp_us = 0U;
    CHECK(arm_controller(&controller,
                         9U,
                         1U,
                         50U,
                         &sequence,
                         &timestamp_us) == 0,
          "echo-gap controller did not arm");
    sequence++;
    timestamp_us += 20000U;
    frame = healthy_frame(9U, 1U, 50U, sequence, timestamp_us);
    frame.evidence = HUMAN_IMBALANCED;
    CHECK(nuwax_barge_controller_process_frame(&controller, &frame, &result),
          "echo-gap candidate did not start");
    sequence++;
    timestamp_us += 20000U;
    frame = healthy_frame(9U, 1U, 50U, sequence, timestamp_us);
    frame.echo_candidate = true;
    CHECK(nuwax_barge_controller_process_frame(&controller, &frame, &result) &&
              result.event == NUWAX_BARGE_EVENT_REJECTED &&
              result.reason == NUWAX_BARGE_REASON_EVIDENCE_LOST,
          "echo-correlated gap was incorrectly tolerated");
    return 0;
}

static int check_low_volume_without_active_reference(void) {
    nuwax_barge_controller_t controller = {0};
    nuwax_barge_controller_result_t result = {0};
    CHECK(begin_controller(&controller, 20U, 1U, 3U),
          "low-volume controller did not start");

    for (uint32_t sequence = 1U; sequence <= 4U; sequence++) {
        nuwax_barge_decision_frame_t frame =
            healthy_frame(20U, 1U, 3U, sequence, sequence * 20000ULL);
        frame.reference_active = false;
        frame.reference_locked = false;
        CHECK(nuwax_barge_controller_process_frame(&controller,
                                                   &frame,
                                                   &result),
              "inactive-reference arming frame failed");
    }
    CHECK(nuwax_barge_controller_state(&controller) ==
              NUWAX_BARGE_CONTROLLER_ECHO_TRACKING,
          "0-5% policy still hard-required an active reference");

    for (uint32_t sequence = 5U; sequence <= 7U; sequence++) {
        nuwax_barge_decision_frame_t frame =
            healthy_frame(20U, 1U, 3U, sequence, sequence * 20000ULL);
        frame.reference_active = false;
        frame.reference_locked = false;
        frame.aec_health = NUWAX_BARGE_HEALTH_YELLOW;
        frame.verified_silent_passthrough = true;
        frame.evidence = HUMAN_SYMMETRIC;
        CHECK(nuwax_barge_controller_process_frame(&controller,
                                                   &frame,
                                                   &result),
              "symmetric low-volume frame failed");
    }
    CHECK(result.event == NUWAX_BARGE_EVENT_CONFIRMED &&
              result.request_local_stop,
          "three-frame symmetric low-volume human was not confirmed");

    CHECK(begin_controller(&controller, 21U, 1U, 3U),
          "unverified low-volume controller did not start");
    for (uint32_t sequence = 1U; sequence <= 4U; sequence++) {
        nuwax_barge_decision_frame_t frame =
            healthy_frame(21U, 1U, 3U, sequence, sequence * 20000ULL);
        frame.reference_active = false;
        frame.reference_locked = false;
        CHECK(nuwax_barge_controller_process_frame(&controller,
                                                   &frame,
                                                   &result),
              "unverified low-volume arming frame failed");
    }
    for (uint32_t sequence = 5U; sequence <= 7U; sequence++) {
        nuwax_barge_decision_frame_t frame =
            healthy_frame(21U, 1U, 3U, sequence, sequence * 20000ULL);
        frame.reference_active = false;
        frame.reference_locked = false;
        frame.aec_health = NUWAX_BARGE_HEALTH_YELLOW;
        frame.evidence = HUMAN_SYMMETRIC;
        CHECK(nuwax_barge_controller_process_frame(&controller,
                                                   &frame,
                                                   &result),
              "unverified low-volume candidate frame failed");
    }
    CHECK(result.event == NUWAX_BARGE_EVENT_PROBE_REQUESTED &&
              !result.request_local_stop,
          "unverified low-volume AEC bypassed the strict path");
    return 0;
}

static int check_high_volume_strict_and_probe_paths(void) {
    nuwax_barge_controller_t controller = {0};
    nuwax_barge_controller_result_t result = {0};
    uint32_t sequence = 0U;
    uint64_t timestamp_us = 0U;
    CHECK(begin_controller(&controller, 30U, 1U, 90U),
          "high-volume controller did not start");
    CHECK(arm_controller(&controller,
                         30U,
                         1U,
                         90U,
                         &sequence,
                         &timestamp_us) == 0,
          "high-volume controller did not arm");

    for (uint32_t i = 0U; i < 2U; i++) {
        sequence++;
        timestamp_us += 20000U;
        nuwax_barge_decision_frame_t frame =
            healthy_frame(30U, 1U, 90U, sequence, timestamp_us);
        frame.evidence = HUMAN_IMBALANCED;
        CHECK(nuwax_barge_controller_process_frame(&controller,
                                                   &frame,
                                                   &result),
              "high-volume candidate frame failed");
    }
    CHECK(result.event == NUWAX_BARGE_EVENT_PROBE_REQUESTED &&
              result.request_probe && !result.request_local_stop &&
              result.probe_attenuation_db == 15U &&
              result.probe_duration_ms == 80U,
          "single-class high-volume evidence bypassed the duck probe");

    sequence++;
    timestamp_us += 20000U;
    nuwax_barge_decision_frame_t frame =
        healthy_frame(30U, 1U, 90U, sequence, timestamp_us);
    frame.evidence = HUMAN_IMBALANCED;
    frame.probe_observation = NUWAX_BARGE_PROBE_HUMAN_PERSISTS;
    CHECK(nuwax_barge_controller_process_frame(&controller, &frame, &result),
          "human probe result failed");
    CHECK(result.event == NUWAX_BARGE_EVENT_CONFIRMED &&
              result.reason == NUWAX_BARGE_REASON_PROBE_HUMAN_PERSISTS &&
              result.request_local_stop,
          "human persistence during duck did not confirm");

    CHECK(begin_controller(&controller, 31U, 1U, 100U),
          "second high-volume controller did not start");
    sequence = 0U;
    timestamp_us = 0U;
    CHECK(arm_controller(&controller,
                         31U,
                         1U,
                         100U,
                         &sequence,
                         &timestamp_us) == 0,
          "second high-volume controller did not arm");
    for (uint32_t i = 0U; i < 2U; i++) {
        sequence++;
        timestamp_us += 20000U;
        frame = healthy_frame(31U, 1U, 100U, sequence, timestamp_us);
        frame.evidence = HUMAN_STRICT;
        CHECK(nuwax_barge_controller_process_frame(&controller,
                                                   &frame,
                                                   &result),
              "strict evidence frame failed");
    }
    CHECK(result.event == NUWAX_BARGE_EVENT_PROBE_REQUESTED &&
              result.request_probe && !result.request_local_stop &&
              result.probe_attenuation_db == 15U &&
              result.probe_duration_ms == 80U,
          "high-volume independent evidence bypassed the duck probe");

    sequence++;
    timestamp_us += 20000U;
    frame = healthy_frame(31U, 1U, 100U, sequence, timestamp_us);
    frame.evidence = HUMAN_STRICT;
    frame.probe_observation = NUWAX_BARGE_PROBE_HUMAN_PERSISTS;
    CHECK(nuwax_barge_controller_process_frame(&controller, &frame, &result),
          "strict high-volume human probe result failed");
    CHECK(result.event == NUWAX_BARGE_EVENT_CONFIRMED &&
              result.reason == NUWAX_BARGE_REASON_PROBE_HUMAN_PERSISTS &&
              result.request_local_stop,
          "strict high-volume human persistence did not confirm after duck");
    return 0;
}

static int check_probe_rejection_is_terminal_once(void) {
    nuwax_barge_controller_t controller = {0};
    nuwax_barge_controller_result_t result = {0};
    uint32_t sequence = 0U;
    uint64_t timestamp_us = 0U;
    CHECK(begin_controller(&controller, 40U, 1U, 90U),
          "controller did not start");
    CHECK(arm_controller(&controller,
                         40U,
                         1U,
                         90U,
                         &sequence,
                         &timestamp_us) == 0,
          "controller did not arm");
    for (uint32_t i = 0U; i < 2U; i++) {
        sequence++;
        timestamp_us += 20000U;
        nuwax_barge_decision_frame_t frame =
            healthy_frame(40U, 1U, 90U, sequence, timestamp_us);
        frame.evidence = HUMAN_IMBALANCED;
        frame.echo_candidate = true;
        CHECK(nuwax_barge_controller_process_frame(&controller,
                                                   &frame,
                                                   &result),
              "probe candidate failed");
    }
    CHECK(result.event == NUWAX_BARGE_EVENT_PROBE_REQUESTED,
          "ambiguous high-volume candidate did not request a probe");
    const uint32_t candidate_id = result.candidate_id;

    sequence++;
    timestamp_us += 20000U;
    nuwax_barge_decision_frame_t frame =
        healthy_frame(40U, 1U, 90U, sequence, timestamp_us);
    frame.evidence = HUMAN_IMBALANCED;
    frame.echo_candidate = true;
    frame.probe_observation = NUWAX_BARGE_PROBE_ECHO_TRACKS_DUCK;
    CHECK(nuwax_barge_controller_process_frame(&controller, &frame, &result),
          "echo probe result failed");
    CHECK(result.event == NUWAX_BARGE_EVENT_REJECTED &&
              result.reason == NUWAX_BARGE_REASON_PROBE_ECHO_TRACKS &&
              result.candidate_id == candidate_id &&
              !result.request_local_stop,
          "duck-following echo was not rejected");

    sequence++;
    timestamp_us += 20000U;
    frame = healthy_frame(40U, 1U, 90U, sequence, timestamp_us);
    CHECK(nuwax_barge_controller_process_frame(&controller, &frame, &result),
          "post-rejection frame failed");
    CHECK(result.event == NUWAX_BARGE_EVENT_NONE &&
              nuwax_barge_controller_state(&controller) ==
                  NUWAX_BARGE_CONTROLLER_PLAYBACK_ARMING,
          "probe restore transient bypassed the fresh arming guard");
    for (uint32_t i = 1U;
         i < controller.config.active_aec_lock_frames;
         i++) {
        sequence++;
        timestamp_us += 20000U;
        frame = healthy_frame(40U, 1U, 90U, sequence, timestamp_us);
        CHECK(nuwax_barge_controller_process_frame(&controller,
                                                   &frame,
                                                   &result),
              "post-probe rearming frame failed");
    }
    CHECK(result.event == NUWAX_BARGE_EVENT_NONE &&
              nuwax_barge_controller_state(&controller) ==
                  NUWAX_BARGE_CONTROLLER_ECHO_TRACKING,
          "probe rejection did not rearm after a fresh stable guard");
    return 0;
}

static int check_aec_red_fail_closed_and_recovery(void) {
    nuwax_barge_controller_t controller = {0};
    nuwax_barge_controller_result_t result = {0};
    CHECK(begin_controller(&controller, 50U, 1U, 50U),
          "controller did not start");

    for (uint32_t sequence = 1U; sequence <= 6U; sequence++) {
        nuwax_barge_decision_frame_t frame =
            healthy_frame(50U, 1U, 50U, sequence, sequence * 20000ULL);
        frame.aec_health = NUWAX_BARGE_HEALTH_RED;
        frame.evidence = HUMAN_STRICT;
        CHECK(nuwax_barge_controller_process_frame(&controller,
                                                   &frame,
                                                   &result),
              "red-health frame failed");
        CHECK(!result.request_local_stop &&
                  result.event != NUWAX_BARGE_EVENT_CONFIRMED,
              "RED AEC health allowed aggressive local stop");
        CHECK(nuwax_barge_controller_state(&controller) ==
                  NUWAX_BARGE_CONTROLLER_DEGRADED_AEC,
              "RED AEC health was not externally visible");
    }

    for (uint32_t sequence = 7U;
         sequence < 7U + controller.config.active_aec_lock_frames;
         sequence++) {
        nuwax_barge_decision_frame_t frame =
            healthy_frame(50U, 1U, 50U, sequence, sequence * 20000ULL);
        CHECK(nuwax_barge_controller_process_frame(&controller,
                                                   &frame,
                                                   &result),
              "AEC recovery frame failed");
    }
    CHECK(nuwax_barge_controller_state(&controller) ==
              NUWAX_BARGE_CONTROLLER_ECHO_TRACKING &&
              result.event == NUWAX_BARGE_EVENT_REARMED,
          "AEC recovery did not require a fresh lock and guard");
    return 0;
}

static int check_reference_red_fail_closed(void) {
    nuwax_barge_controller_t controller = {0};
    nuwax_barge_controller_result_t result = {0};
    CHECK(begin_controller(&controller, 51U, 1U, 50U),
          "reference-health controller did not start");

    for (uint32_t sequence = 1U; sequence <= 6U; sequence++) {
        nuwax_barge_decision_frame_t frame =
            healthy_frame(51U, 1U, 50U, sequence, sequence * 20000ULL);
        frame.reference_health = NUWAX_BARGE_HEALTH_RED;
        frame.evidence = HUMAN_STRICT;
        CHECK(nuwax_barge_controller_process_frame(&controller,
                                                   &frame,
                                                   &result),
              "RED reference-health frame failed");
        CHECK(!result.request_local_stop &&
                  result.event != NUWAX_BARGE_EVENT_CONFIRMED &&
                  nuwax_barge_controller_state(&controller) ==
                      NUWAX_BARGE_CONTROLLER_DEGRADED_REF,
              "RED reference health did not remain fail-closed");
        if (sequence == 1U) {
            CHECK(result.event == NUWAX_BARGE_EVENT_DEGRADED &&
                      result.reason == NUWAX_BARGE_REASON_REFERENCE_RED,
                  "RED reference health did not publish its degradation reason");
        }
    }
    return 0;
}

static int check_reference_deadline_and_timing_fail_closed(void) {
    nuwax_barge_controller_t controller = {0};
    nuwax_barge_controller_result_t result = {0};
    CHECK(begin_controller(&controller, 60U, 1U, 50U),
          "controller did not start");
    for (uint32_t sequence = 1U; sequence <= 15U; sequence++) {
        nuwax_barge_decision_frame_t frame =
            healthy_frame(60U, 1U, 50U, sequence, sequence * 20000ULL);
        frame.reference_locked = false;
        frame.evidence = HUMAN_STRICT;
        CHECK(nuwax_barge_controller_process_frame(&controller,
                                                   &frame,
                                                   &result),
              "unlocked-reference frame failed");
        CHECK(!result.request_local_stop,
              "unlocked reference allowed a local stop");
    }
    CHECK(nuwax_barge_controller_state(&controller) ==
              NUWAX_BARGE_CONTROLLER_DEGRADED_REF &&
              result.reason == NUWAX_BARGE_REASON_REFERENCE_UNLOCKED,
          "300-ms reference deadline did not fail closed");

    CHECK(begin_controller(&controller, 61U, 1U, 50U),
          "timing controller did not start");
    uint32_t sequence = 0U;
    uint64_t timestamp_us = 0U;
    CHECK(arm_controller(&controller,
                         61U,
                         1U,
                         50U,
                         &sequence,
                         &timestamp_us) == 0,
          "timing controller did not arm");
    sequence++;
    timestamp_us += 20000U;
    nuwax_barge_decision_frame_t frame =
        healthy_frame(61U, 1U, 50U, sequence, timestamp_us);
    frame.evidence = HUMAN_IMBALANCED;
    CHECK(nuwax_barge_controller_process_frame(&controller, &frame, &result),
          "timing candidate failed");

    sequence++;
    timestamp_us += 20000U;
    frame = healthy_frame(61U, 1U, 50U, sequence, timestamp_us);
    frame.evidence = HUMAN_IMBALANCED;
    frame.frame_age_ms = 101U;
    CHECK(nuwax_barge_controller_process_frame(&controller, &frame, &result),
          "stale frame handling failed");
    CHECK(result.event == NUWAX_BARGE_EVENT_REJECTED &&
              result.reason == NUWAX_BARGE_REASON_FRAME_STALE &&
              !result.request_local_stop &&
              nuwax_barge_controller_state(&controller) ==
                  NUWAX_BARGE_CONTROLLER_DEGRADED_REF,
          "stale decision audio was allowed to confirm");
    return 0;
}

static int check_generation_binding(void) {
    nuwax_barge_controller_t controller = {0};
    nuwax_barge_controller_result_t result = {0};
    uint32_t sequence = 0U;
    uint64_t timestamp_us = 0U;
    CHECK(begin_controller(&controller, 70U, UINT32_MAX, 50U),
          "controller did not start");
    CHECK(arm_controller(&controller,
                         70U,
                         UINT32_MAX,
                         50U,
                         &sequence,
                         &timestamp_us) == 0,
          "controller did not arm");
    sequence++;
    timestamp_us += 20000U;
    nuwax_barge_decision_frame_t frame =
        healthy_frame(70U, UINT32_MAX, 50U, sequence, timestamp_us);
    frame.evidence = HUMAN_IMBALANCED;
    CHECK(nuwax_barge_controller_process_frame(&controller, &frame, &result),
          "old-generation candidate failed");
    const uint32_t old_candidate_id = result.candidate_id;

    timestamp_us += 20000U;
    frame = healthy_frame(71U, 0U, 50U, 1U, timestamp_us);
    frame.evidence = HUMAN_STRICT;
    CHECK(nuwax_barge_controller_process_frame(&controller, &frame, &result),
          "new playback generation was not accepted");
    CHECK(result.event == NUWAX_BARGE_EVENT_REJECTED &&
              result.reason == NUWAX_BARGE_REASON_GENERATION_SUPERSEDED &&
              result.event_playback_generation == 70U &&
              result.candidate_id == old_candidate_id &&
              nuwax_barge_controller_state(&controller) ==
                  NUWAX_BARGE_CONTROLLER_PLAYBACK_ARMING &&
              controller.playback_generation == 71U,
          "generation switch did not terminalize and isolate old candidate");

    frame = healthy_frame(70U, UINT32_MAX, 50U, 6U,
                          timestamp_us + 20000U);
    frame.evidence = HUMAN_STRICT;
    CHECK(nuwax_barge_controller_process_frame(&controller, &frame, &result),
          "stale playback frame was not safely ignored");
    CHECK(result.event == NUWAX_BARGE_EVENT_NONE &&
              result.reason == NUWAX_BARGE_REASON_STALE_GENERATION &&
              controller.playback_generation == 71U,
          "stale playback generation rebound the controller backwards");
    return 0;
}

static int check_volume_policy_and_hysteresis(void) {
    nuwax_barge_controller_t controller = {0};
    nuwax_barge_controller_result_t result = {0};
    uint32_t sequence = 0U;
    uint64_t timestamp_us = 0U;
    CHECK(begin_controller(&controller, 80U, 1U, 90U),
          "controller did not start");
    CHECK(arm_controller(&controller,
                         80U,
                         1U,
                         90U,
                         &sequence,
                         &timestamp_us) == 0,
          "controller did not arm");

    sequence++;
    timestamp_us += 20000U;
    nuwax_barge_decision_frame_t frame =
        healthy_frame(80U, 2U, 10U, sequence, timestamp_us);
    CHECK(nuwax_barge_controller_process_frame(&controller, &frame, &result),
          "volume-generation update failed");
    CHECK(nuwax_barge_controller_volume_band(&controller) ==
              NUWAX_BARGE_VOLUME_76_TO_100,
          "less-strict band was applied without hysteresis");
    for (uint32_t i = 0U; i < 8U; i++) {
        sequence++;
        timestamp_us += 20000U;
        frame = healthy_frame(80U, 2U, 10U, sequence, timestamp_us);
        CHECK(nuwax_barge_controller_process_frame(&controller,
                                                   &frame,
                                                   &result),
              "volume hysteresis frame failed");
    }
    CHECK(nuwax_barge_controller_volume_band(&controller) ==
              NUWAX_BARGE_VOLUME_76_TO_100,
          "volume band changed before 200 ms");
    sequence++;
    timestamp_us += 20000U;
    frame = healthy_frame(80U, 2U, 10U, sequence, timestamp_us);
    CHECK(nuwax_barge_controller_process_frame(&controller, &frame, &result),
          "volume hysteresis boundary failed");
    CHECK(nuwax_barge_controller_volume_band(&controller) ==
              NUWAX_BARGE_VOLUME_6_TO_30,
          "volume band did not change at 200 ms");

    sequence++;
    timestamp_us += 20000U;
    frame = healthy_frame(80U, 3U, 90U, sequence, timestamp_us);
    CHECK(nuwax_barge_controller_process_frame(&controller, &frame, &result),
          "stricter volume-generation update failed");
    CHECK(nuwax_barge_controller_volume_band(&controller) ==
              NUWAX_BARGE_VOLUME_76_TO_100 &&
              nuwax_barge_controller_state(&controller) ==
                  NUWAX_BARGE_CONTROLLER_PLAYBACK_ARMING,
          "stricter volume policy was not applied on the next frame");
    return 0;
}

static int check_tts_startup_aec_stabilization(void) {
    nuwax_barge_controller_t controller = {0};
    nuwax_barge_controller_result_t result = {0};
    uint32_t sequence = 0U;
    uint64_t timestamp_us = 0U;
    CHECK(begin_controller(&controller, 90U, 1U, 100U),
          "startup-stability controller did not start");

    /* Playback metadata may precede the first audible TTS frame by seconds.
     * Those silent frames must not consume the active AEC stabilization. */
    for (uint32_t i = 0U; i < 12U; i++) {
        sequence++;
        timestamp_us += 20000U;
        nuwax_barge_decision_frame_t frame =
            healthy_frame(90U, 1U, 100U, sequence, timestamp_us);
        frame.reference_active = false;
        frame.reference_locked = false;
        frame.aec_frame_processed = false;
        frame.aec_health = NUWAX_BARGE_HEALTH_YELLOW;
        frame.evidence = HUMAN_STRICT;
        CHECK(nuwax_barge_controller_process_frame(&controller,
                                                   &frame,
                                                   &result),
              "pre-TTS silent frame failed");
        CHECK(!result.request_local_stop &&
                  nuwax_barge_controller_state(&controller) ==
                      NUWAX_BARGE_CONTROLLER_PLAYBACK_ARMING,
              "pre-TTS wait incorrectly armed local barge-in");
    }

    /* Reproduce the observed startup failure: device echo looked like strict
     * human evidence before enough active AEC/reference history existed. */
    for (uint32_t i = 0U;
         i < controller.config.active_aec_lock_frames;
         i++) {
        sequence++;
        timestamp_us += 20000U;
        nuwax_barge_decision_frame_t frame =
            healthy_frame(90U, 1U, 100U, sequence, timestamp_us);
        frame.evidence = i + 1U < controller.config.active_aec_lock_frames
                             ? HUMAN_STRICT
                             : 0U;
        CHECK(nuwax_barge_controller_process_frame(&controller,
                                                   &frame,
                                                   &result),
              "active AEC startup frame failed");
        CHECK(!result.request_local_stop &&
                  result.event != NUWAX_BARGE_EVENT_CONFIRMED,
              "TTS startup echo stopped playback before AEC stabilized");
        if (i + 1U < controller.config.active_aec_lock_frames) {
            CHECK(nuwax_barge_controller_state(&controller) ==
                      NUWAX_BARGE_CONTROLLER_PLAYBACK_ARMING,
                  "pre-TTS silence polluted the active-reference deadline");
        }
    }
    CHECK(nuwax_barge_controller_state(&controller) ==
              NUWAX_BARGE_CONTROLLER_ECHO_TRACKING,
          "controller did not arm after valid active AEC stabilization");

    for (uint32_t i = 0U; i < 2U; i++) {
        sequence++;
        timestamp_us += 20000U;
        nuwax_barge_decision_frame_t frame =
            healthy_frame(90U, 1U, 100U, sequence, timestamp_us);
        frame.evidence = HUMAN_STRICT;
        CHECK(nuwax_barge_controller_process_frame(&controller,
                                                   &frame,
                                                   &result),
              "post-stabilization weak-human frame failed");
    }
    CHECK(result.event == NUWAX_BARGE_EVENT_PROBE_REQUESTED &&
              result.request_probe && !result.request_local_stop,
          "real human evidence did not reach the high-volume probe after AEC stabilization");

    sequence++;
    timestamp_us += 20000U;
    nuwax_barge_decision_frame_t frame =
        healthy_frame(90U, 1U, 100U, sequence, timestamp_us);
    frame.evidence = HUMAN_STRICT;
    frame.probe_observation = NUWAX_BARGE_PROBE_HUMAN_PERSISTS;
    CHECK(nuwax_barge_controller_process_frame(&controller, &frame, &result),
          "post-stabilization human probe frame failed");
    CHECK(result.event == NUWAX_BARGE_EVENT_CONFIRMED &&
              result.request_local_stop,
          "real human evidence was not retained through the high-volume probe");
    return 0;
}

static int check_reference_discontinuity_cannot_confirm(void) {
    nuwax_barge_controller_t controller = {0};
    nuwax_barge_controller_result_t result = {0};
    uint32_t sequence = 0U;
    uint64_t timestamp_us = 0U;
    CHECK(begin_controller(&controller, 91U, 1U, 100U),
          "reference-discontinuity controller did not start");
    CHECK(arm_controller(&controller,
                         91U,
                         1U,
                         100U,
                         &sequence,
                         &timestamp_us) == 0,
          "reference-discontinuity controller did not arm");

    sequence++;
    timestamp_us += 20000U;
    nuwax_barge_decision_frame_t frame =
        healthy_frame(91U, 1U, 100U, sequence, timestamp_us);
    frame.evidence = HUMAN_STRICT;
    CHECK(nuwax_barge_controller_process_frame(&controller, &frame, &result) &&
              result.event == NUWAX_BARGE_EVENT_CANDIDATE_STARTED,
          "candidate did not start before reference discontinuity");

    sequence++;
    timestamp_us += 20000U;
    frame = healthy_frame(91U, 1U, 100U, sequence, timestamp_us);
    frame.reference_generation = 0U;
    frame.reference_contiguous = false;
    frame.evidence = HUMAN_STRICT;
    CHECK(nuwax_barge_controller_process_frame(&controller, &frame, &result) &&
              result.event == NUWAX_BARGE_EVENT_REJECTED &&
              !result.request_local_stop &&
              controller.active_aec_stable_frames == 0U,
          "broken reference generation confirmed stale human evidence");
    return 0;
}

static int check_tts_only_matrix(void) {
    static const uint8_t volumes[] = {0U, 10U, 30U, 50U, 70U, 90U, 100U};
    for (size_t volume_index = 0U;
         volume_index < sizeof(volumes) / sizeof(volumes[0]);
         volume_index++) {
        nuwax_barge_controller_t controller = {0};
        nuwax_barge_controller_result_t result = {0};
        const uint32_t generation = 100U + (uint32_t)volume_index;
        CHECK(begin_controller(&controller, generation, 1U,
                               volumes[volume_index]),
              "TTS-only controller did not start");
        bool local_stop_seen = false;
        for (uint32_t sequence = 1U; sequence <= 100U; sequence++) {
            nuwax_barge_decision_frame_t frame =
                healthy_frame(generation,
                              1U,
                              volumes[volume_index],
                              sequence,
                              sequence * 20000ULL);
            frame.evidence = HUMAN_SYMMETRIC |
                             NUWAX_BARGE_EVIDENCE_AEC_NEAREND;
            frame.echo_candidate = true;
            if (nuwax_barge_controller_state(&controller) ==
                NUWAX_BARGE_CONTROLLER_AMBIGUOUS_PROBE) {
                frame.probe_observation =
                    NUWAX_BARGE_PROBE_ECHO_TRACKS_DUCK;
            }
            CHECK(nuwax_barge_controller_process_frame(&controller,
                                                       &frame,
                                                       &result),
                  "TTS-only frame failed");
            local_stop_seen = local_stop_seen || result.request_local_stop ||
                              result.event ==
                                  NUWAX_BARGE_EVENT_CONFIRMED;
        }
        CHECK(!local_stop_seen,
              "TTS-only evidence caused a false local stop");
    }
    return 0;
}

int main(void) {
    int failures = 0;
    failures += check_default_config_and_validation();
    failures += check_medium_confirmation_and_execution_chain();
    failures += check_single_frame_candidate_gap_tolerance();
    failures += check_low_volume_without_active_reference();
    failures += check_high_volume_strict_and_probe_paths();
    failures += check_probe_rejection_is_terminal_once();
    failures += check_aec_red_fail_closed_and_recovery();
    failures += check_reference_red_fail_closed();
    failures += check_reference_deadline_and_timing_fail_closed();
    failures += check_generation_binding();
    failures += check_volume_policy_and_hysteresis();
    failures += check_tts_startup_aec_stabilization();
    failures += check_reference_discontinuity_cannot_confirm();
    failures += check_tts_only_matrix();
    printf("barge-in controller host check: %s (%d failures)\n",
           failures == 0 ? "PASS" : "FAIL",
           failures);
    return failures == 0 ? 0 : 1;
}
