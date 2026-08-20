#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "../app_state.h"

bool nuwax_state_bridge_is_sleeping(void);
bool nuwax_state_bridge_is_semantic_emotion_name_supported(
    const char *emotion);
bool nuwax_state_bridge_set_semantic_emotion_name(const char *emotion,
                                                  const char *source);
bool nuwax_state_bridge_set_semantic_emotion_name_owned(
    const char *emotion,
    const char *source,
    uint32_t owner_revision);
bool nuwax_state_bridge_set_semantic_emotion_name_owned_leased(
    const char *emotion,
    const char *source,
    uint32_t owner_revision,
    uint32_t duration_ms,
    bool finite_lease);
void nuwax_state_bridge_invalidate_semantic_revisions_through(
    uint32_t owner_revision);
void nuwax_state_bridge_clear_semantic_emotion(void);
bool nuwax_state_bridge_clear_semantic_emotion_if_owner(
    const char *source,
    uint32_t owner_revision);
void nuwax_state_bridge_update(nuwax_app_state_t app_state);
void nuwax_state_bridge_service(uint32_t now_ms);
void nuwax_state_bridge_start_waiting_question(void);
void nuwax_state_bridge_clear_waiting_question(void);
void nuwax_state_bridge_start_pending_sleep_visual(void);
void nuwax_state_bridge_clear_pending_sleep_visual(void);
void nuwax_state_bridge_trigger_touch(void);
void nuwax_state_bridge_trigger_shake(void);
void nuwax_state_bridge_pin_touch(void);
void nuwax_state_bridge_pin_shake(void);
void nuwax_state_bridge_cancel_touch(void);
bool nuwax_state_bridge_special_semantic_active(void);
void nuwax_state_bridge_note_playback_pcm_started(uint32_t connection_epoch,
                                                  uint32_t generation);
void nuwax_state_bridge_note_playback_stopped(uint32_t connection_epoch,
                                              uint32_t generation);
void nuwax_state_bridge_clear_playback(void);
void nuwax_state_bridge_refresh_power(void);
