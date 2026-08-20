#pragma once

#include <stdbool.h>
#include <stdint.h>

#define NUWAX_BARGE_CONTROLLER_FRAME_MS 20U

typedef enum {
    NUWAX_BARGE_CONTROLLER_INACTIVE = 0,
    NUWAX_BARGE_CONTROLLER_PLAYBACK_ARMING,
    NUWAX_BARGE_CONTROLLER_ECHO_TRACKING,
    NUWAX_BARGE_CONTROLLER_HUMAN_CANDIDATE,
    NUWAX_BARGE_CONTROLLER_AMBIGUOUS_PROBE,
    NUWAX_BARGE_CONTROLLER_STRICT_CONFIRMED,
    NUWAX_BARGE_CONTROLLER_REJECTED,
    NUWAX_BARGE_CONTROLLER_LOCAL_STOPPING,
    NUWAX_BARGE_CONTROLLER_POST_STOP_TAIL,
    NUWAX_BARGE_CONTROLLER_UPLINK_ACTIVE,
    NUWAX_BARGE_CONTROLLER_DEGRADED_REF,
    NUWAX_BARGE_CONTROLLER_DEGRADED_AEC,
} nuwax_barge_controller_state_t;

/* RED is zero so an uninitialized health input always fails closed. */
typedef enum {
    NUWAX_BARGE_HEALTH_RED = 0,
    NUWAX_BARGE_HEALTH_YELLOW,
    NUWAX_BARGE_HEALTH_GREEN,
} nuwax_barge_health_t;

typedef enum {
    NUWAX_BARGE_VOLUME_0_TO_5 = 0,
    NUWAX_BARGE_VOLUME_6_TO_30,
    NUWAX_BARGE_VOLUME_31_TO_75,
    NUWAX_BARGE_VOLUME_76_TO_100,
} nuwax_barge_volume_band_t;

typedef enum {
    NUWAX_BARGE_PROBE_NOT_AVAILABLE = 0,
    NUWAX_BARGE_PROBE_HUMAN_PERSISTS,
    NUWAX_BARGE_PROBE_ECHO_TRACKS_DUCK,
    NUWAX_BARGE_PROBE_INCONCLUSIVE,
} nuwax_barge_probe_observation_t;

typedef enum {
    NUWAX_BARGE_EVENT_NONE = 0,
    NUWAX_BARGE_EVENT_CANDIDATE_STARTED,
    NUWAX_BARGE_EVENT_PROBE_REQUESTED,
    NUWAX_BARGE_EVENT_CONFIRMED,
    NUWAX_BARGE_EVENT_REJECTED,
    NUWAX_BARGE_EVENT_DEGRADED,
    NUWAX_BARGE_EVENT_REARMED,
} nuwax_barge_controller_event_t;

typedef enum {
    NUWAX_BARGE_REASON_NONE = 0,
    NUWAX_BARGE_REASON_HUMAN_EVIDENCE,
    NUWAX_BARGE_REASON_STRICT_HUMAN_EVIDENCE,
    NUWAX_BARGE_REASON_PROBE_HUMAN_PERSISTS,
    NUWAX_BARGE_REASON_EVIDENCE_LOST,
    NUWAX_BARGE_REASON_EVIDENCE_TIMEOUT,
    NUWAX_BARGE_REASON_ECHO_CONFLICT,
    NUWAX_BARGE_REASON_PROBE_ECHO_TRACKS,
    NUWAX_BARGE_REASON_PROBE_TIMEOUT,
    NUWAX_BARGE_REASON_AEC_RED,
    NUWAX_BARGE_REASON_AEC_STABILIZING,
    NUWAX_BARGE_REASON_REFERENCE_RED,
    NUWAX_BARGE_REASON_REFERENCE_UNLOCKED,
    NUWAX_BARGE_REASON_REFERENCE_GENERATION,
    NUWAX_BARGE_REASON_FRAME_STALE,
    NUWAX_BARGE_REASON_SEQUENCE_DISCONTINUITY,
    NUWAX_BARGE_REASON_TIMESTAMP_DISCONTINUITY,
    NUWAX_BARGE_REASON_GENERATION_SUPERSEDED,
    NUWAX_BARGE_REASON_STALE_GENERATION,
    NUWAX_BARGE_REASON_VOLUME_CHANGED,
    NUWAX_BARGE_REASON_STALE_VOLUME_GENERATION,
    NUWAX_BARGE_REASON_INVALID_INPUT,
} nuwax_barge_controller_reason_t;

typedef uint32_t nuwax_barge_evidence_t;

#define NUWAX_BARGE_EVIDENCE_VOICE_LIKE (1UL << 0)
#define NUWAX_BARGE_EVIDENCE_DUAL_MIC_IMBALANCED (1UL << 1)
#define NUWAX_BARGE_EVIDENCE_DUAL_MIC_SYMMETRIC (1UL << 2)
#define NUWAX_BARGE_EVIDENCE_AEC_NEAREND (1UL << 3)
#define NUWAX_BARGE_EVIDENCE_LOW_REF_CORRELATION (1UL << 4)

typedef struct {
    uint8_t minimum_guard_frames;
    uint8_t reference_lock_frames;
    uint8_t active_aec_lock_frames;
    uint8_t reference_lock_deadline_frames;
    uint8_t imbalanced_confirm_frames;
    uint8_t symmetric_confirm_frames;
    uint8_t candidate_timeout_frames;
    uint8_t candidate_max_gap_frames;
    uint8_t volume_hysteresis_frames;
    bool ambiguous_probe_enabled;
    uint8_t probe_duration_frames;
    uint8_t probe_attenuation_db;
    uint16_t maximum_frame_age_ms;
    uint16_t maximum_timestamp_gap_ms;
} nuwax_barge_controller_config_t;

typedef struct {
    uint32_t playback_generation;
    uint32_t volume_generation;
    uint32_t reference_generation;
    uint32_t sequence;
    uint64_t capture_timestamp_us;
    uint16_t frame_age_ms;
    uint8_t volume_percent;
    nuwax_barge_health_t aec_health;
    nuwax_barge_health_t reference_health;
    bool reference_active;
    bool reference_locked;
    bool reference_contiguous;
    bool aec_frame_processed;
    bool verified_silent_passthrough;
    bool microphone_clipped;
    bool reference_clipped;
    bool echo_candidate;
    nuwax_barge_evidence_t evidence;
    nuwax_barge_probe_observation_t probe_observation;
} nuwax_barge_decision_frame_t;

typedef struct {
    nuwax_barge_controller_state_t state_before;
    nuwax_barge_controller_state_t state_after;
    nuwax_barge_controller_event_t event;
    nuwax_barge_controller_reason_t reason;
    uint32_t event_playback_generation;
    uint32_t candidate_id;
    uint32_t candidate_start_sequence;
    uint64_t candidate_started_timestamp_us;
    uint64_t event_timestamp_us;
    bool request_local_stop;
    bool request_probe;
    uint8_t probe_attenuation_db;
    uint16_t probe_duration_ms;
} nuwax_barge_controller_result_t;

typedef struct {
    bool playback_started_valid;
    bool candidate_started_valid;
    bool confirmed_valid;
    bool local_stop_started_valid;
    bool playback_stopped_valid;
    bool uplink_active_valid;
    uint64_t playback_started_timestamp_us;
    uint64_t candidate_started_timestamp_us;
    uint64_t confirmed_timestamp_us;
    uint64_t local_stop_started_timestamp_us;
    uint64_t playback_stopped_timestamp_us;
    uint64_t uplink_active_timestamp_us;
} nuwax_barge_controller_timestamps_t;

typedef struct {
    nuwax_barge_controller_config_t config;
    nuwax_barge_controller_state_t state;
    nuwax_barge_controller_reason_t degraded_reason;
    nuwax_barge_volume_band_t active_volume_band;
    nuwax_barge_volume_band_t pending_volume_band;
    uint32_t playback_generation;
    uint32_t volume_generation;
    uint32_t candidate_generation;
    uint32_t candidate_id;
    uint32_t next_candidate_id;
    uint32_t candidate_start_sequence;
    uint32_t last_sequence;
    uint32_t last_reference_generation;
    uint64_t last_capture_timestamp_us;
    uint8_t volume_percent;
    uint8_t frames_since_context_change;
    uint8_t reference_stable_frames;
    uint8_t active_aec_stable_frames;
    uint8_t imbalanced_run_frames;
    uint8_t symmetric_run_frames;
    uint8_t candidate_frames;
    uint8_t candidate_gap_frames;
    uint8_t pending_volume_frames;
    uint8_t probe_frames;
    bool initialized;
    bool playback_active;
    bool active_reference_seen;
    bool timeline_initialized;
    bool candidate_open;
    bool pending_volume_band_valid;
    nuwax_barge_controller_timestamps_t timestamps;
} nuwax_barge_controller_t;

void nuwax_barge_controller_default_config(
    nuwax_barge_controller_config_t *config);

bool nuwax_barge_controller_init(
    nuwax_barge_controller_t *controller,
    const nuwax_barge_controller_config_t *config);

void nuwax_barge_controller_reset(nuwax_barge_controller_t *controller);

bool nuwax_barge_controller_begin_playback(
    nuwax_barge_controller_t *controller,
    uint32_t playback_generation,
    uint32_t volume_generation,
    uint8_t volume_percent,
    uint64_t timestamp_us);

bool nuwax_barge_controller_process_frame(
    nuwax_barge_controller_t *controller,
    const nuwax_barge_decision_frame_t *frame,
    nuwax_barge_controller_result_t *result);

bool nuwax_barge_controller_mark_local_stop_started(
    nuwax_barge_controller_t *controller,
    uint32_t playback_generation,
    uint32_t candidate_id,
    uint64_t timestamp_us);

bool nuwax_barge_controller_mark_playback_stopped(
    nuwax_barge_controller_t *controller,
    uint32_t playback_generation,
    uint32_t candidate_id,
    uint64_t timestamp_us);

bool nuwax_barge_controller_mark_uplink_active(
    nuwax_barge_controller_t *controller,
    uint32_t playback_generation,
    uint32_t candidate_id,
    uint64_t timestamp_us);

nuwax_barge_controller_state_t nuwax_barge_controller_state(
    const nuwax_barge_controller_t *controller);

nuwax_barge_volume_band_t nuwax_barge_controller_volume_band(
    const nuwax_barge_controller_t *controller);

const nuwax_barge_controller_timestamps_t *nuwax_barge_controller_timestamps(
    const nuwax_barge_controller_t *controller);
