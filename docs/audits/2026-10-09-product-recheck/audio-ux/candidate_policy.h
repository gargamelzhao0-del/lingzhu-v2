/* AUDIT-ONLY CANDIDATE WG-EPOCH-RECOVERY-v1. Not production code. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "conversation/conversation_visual_round.h"
#include "conversation/turn_guard.h"
#include "ws_close_policy.h"

typedef enum { WG_FAILURE_NONE, WG_FAILURE_TEXT_TIMEOUT, WG_FAILURE_TTS_TIMEOUT,
               WG_FAILURE_FIRST_PCM_TIMEOUT } wg_failure_t;
typedef struct {
    bool valid;
    wg_failure_t failure;
    uint32_t ws_epoch, awake_generation, turn_epoch;
    char request_id[48];
} wg_ticket_t;

static inline wg_ticket_t wg_capture(const nuwax_conversation_visual_round_t *v,
    const nuwax_turn_guard_t *g, uint32_t epoch, uint32_t awake, wg_failure_t failure) {
    wg_ticket_t t={0};
    if(!v||!g||!g->active||g->speaker_generation||v->first_pcm_seen||
       v->kind!=NUWAX_VISUAL_ROUND_WAKE_GREETING||!epoch||!awake||
       v->ws_epoch!=epoch||v->awake_generation!=awake||g->awake_generation!=awake||
       !g->request_id[0]||strcmp(v->request_id,g->request_id)||failure==WG_FAILURE_NONE)
        return t;
    t.valid=true;t.failure=failure;t.ws_epoch=epoch;t.awake_generation=awake;t.turn_epoch=g->epoch;
    memcpy(t.request_id,g->request_id,sizeof(t.request_id));
    return t;
}
typedef enum { WG_NO_ACTION,WG_DEFER_OTA,WG_YIELD_NEW_OWNER,WG_ROTATE_EPOCH } wg_action_t;
static inline wg_action_t wg_decide(const wg_ticket_t *t,uint32_t epoch,uint32_t awake,
    uint32_t turn_epoch,bool new_turn,bool connected,bool awake_active,bool real_sleep,
    bool shutdown,bool ota,bool reprovision,bool notification,bool exclusive) {
    if(!t||!t->valid)return WG_NO_ACTION;
    if(t->ws_epoch!=epoch||t->awake_generation!=awake||t->turn_epoch!=turn_epoch||
       new_turn||!connected||!awake_active||real_sleep||shutdown||reprovision||notification||exclusive)
        return WG_YIELD_NEW_OWNER;
    if(ota)return WG_DEFER_OTA;
    return WG_ROTATE_EPOCH;
}

/* Proposed new, scoped close reason. Do not relabel this transport corruption. */
#define WG_CLOSE_UNRESOLVED_GREETING 7
static inline bool wg_candidate_close_admits(int cause,bool heartbeat_healthy) {
    return cause==WG_CLOSE_UNRESOLVED_GREETING ||
        nuwax_ws_close_policy_admits((nuwax_ws_close_cause_t)cause,heartbeat_healthy);
}
