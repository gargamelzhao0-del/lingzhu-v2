#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "nuwax_device_state.h"

#define NUWAX_EMOTION_ORDINARY_DURATION_MS 3000U
#define NUWAX_EMOTION_THOUGHT_COMPLETE_DURATION_MS 1500U
#define NUWAX_EMOTION_TOUCH_OVERRIDE_DURATION_MS 2000U
#define NUWAX_EMOTION_SHAKE_OVERRIDE_DURATION_MS 3000U

typedef enum {
    NUWAX_EMOTION_DISPLAY_NONE = 0,
    NUWAX_EMOTION_DISPLAY_ORDINARY,
    NUWAX_EMOTION_DISPLAY_THINKING,
    NUWAX_EMOTION_DISPLAY_THOUGHT_COMPLETE,
} nuwax_emotion_display_mode_t;

typedef struct {
    bool semantic_valid;
    nuwax_emotion_t semantic_emotion;
    nuwax_emotion_display_mode_t semantic_mode;
    bool semantic_deadline_active;
    bool semantic_finite_lease;
    uint32_t semantic_deadline_ms;
    uint32_t semantic_paused_remaining_ms;
    uint32_t semantic_generation;
    bool waiting_question_active;
    bool pending_sleep_visual_active;
    bool touch_active;
    bool interaction_pinned;
    nuwax_emotion_t interaction_emotion;
    bool touch_deadline_active;
    uint32_t touch_deadline_ms;
    uint32_t touch_generation;
    uint32_t playback_epoch;
    uint32_t playback_generation;
    bool playback_pcm_started;
} nuwax_emotion_display_policy_t;

void nuwax_emotion_display_policy_init(
    nuwax_emotion_display_policy_t *policy);
bool nuwax_emotion_display_policy_apply_cloud(
    nuwax_emotion_display_policy_t *policy,
    nuwax_emotion_t emotion,
    uint32_t now_ms,
    uint32_t *out_generation);
bool nuwax_emotion_display_policy_apply_cloud_leased(
    nuwax_emotion_display_policy_t *policy,
    nuwax_emotion_t emotion,
    uint32_t duration_ms,
    bool finite_lease,
    uint32_t now_ms,
    uint32_t *out_generation);
bool nuwax_emotion_display_policy_start_waiting_question(
    nuwax_emotion_display_policy_t *policy);
bool nuwax_emotion_display_policy_clear_waiting_question(
    nuwax_emotion_display_policy_t *policy);
bool nuwax_emotion_display_policy_start_pending_sleep_visual(
    nuwax_emotion_display_policy_t *policy);
bool nuwax_emotion_display_policy_clear_pending_sleep_visual(
    nuwax_emotion_display_policy_t *policy);
uint32_t nuwax_emotion_display_policy_start_interaction(
    nuwax_emotion_display_policy_t *policy,
    nuwax_emotion_t emotion,
    uint32_t duration_ms,
    uint32_t now_ms);
uint32_t nuwax_emotion_display_policy_start_touch(
    nuwax_emotion_display_policy_t *policy,
    uint32_t now_ms);
uint32_t nuwax_emotion_display_policy_pin_interaction(
    nuwax_emotion_display_policy_t *policy,
    nuwax_emotion_t emotion);
bool nuwax_emotion_display_policy_enter_lifecycle(
    nuwax_emotion_display_policy_t *policy,
    nuwax_dialog_state_t dialog,
    uint32_t now_ms);
bool nuwax_emotion_display_policy_service(
    nuwax_emotion_display_policy_t *policy,
    uint32_t now_ms);
bool nuwax_emotion_display_policy_cancel_touch(
    nuwax_emotion_display_policy_t *policy,
    uint32_t now_ms);
bool nuwax_emotion_display_policy_note_first_pcm(
    nuwax_emotion_display_policy_t *policy,
    uint32_t connection_epoch,
    uint32_t playback_generation);
bool nuwax_emotion_display_policy_stop_playback(
    nuwax_emotion_display_policy_t *policy,
    uint32_t connection_epoch,
    uint32_t playback_generation);
bool nuwax_emotion_display_policy_clear_playback(
    nuwax_emotion_display_policy_t *policy);
bool nuwax_emotion_display_policy_special_semantic_active(
    const nuwax_emotion_display_policy_t *policy);
