#pragma once
#include "lvgl.h"
#include "esp_err.h"
#include "../nuwax_device_state.h"

typedef enum {
    NUWAX_CONVERSATION_VISUAL_NONE,
    NUWAX_CONVERSATION_VISUAL_LISTENING,
    NUWAX_CONVERSATION_VISUAL_SPEAKING,
} nuwax_conversation_visual_t;

/* State is authoritative; an emotion name alone never enables an indicator. */
static inline nuwax_conversation_visual_t nuwax_conversation_visual_for_state(
    const nuwax_device_state_t *s)
{
    if (!s || s->provisioning_active || s->provisioning_starting ||
        s->app_low_power || s->touch_emotion_active || s->ota_update_active ||
        s->meeting != NUWAX_MEETING_UI_DISABLED ||
        s->monitoring != NUWAX_MONITORING_UI_DISABLED ||
        s->wifi == NUWAX_WIFI_DISCONNECTED || s->wifi == NUWAX_WIFI_CONNECTING ||
        s->cloud != NUWAX_CLOUD_CONNECTED) return NUWAX_CONVERSATION_VISUAL_NONE;
    if (s->dialog == NUWAX_DIALOG_SPEAKING && s->speaking_pcm_active)
        return NUWAX_CONVERSATION_VISUAL_SPEAKING;
    if (s->dialog == NUWAX_DIALOG_LISTENING && !s->speaking_pcm_active)
        return NUWAX_CONVERSATION_VISUAL_LISTENING;
    return NUWAX_CONVERSATION_VISUAL_NONE;
}

esp_err_t nuwax_conversation_visual_create(lv_obj_t *parent);
void nuwax_conversation_visual_delete(void);
void nuwax_conversation_visual_request(nuwax_conversation_visual_t mode);
void nuwax_conversation_visual_tick(void);
void nuwax_conversation_visual_freeze(bool frozen);
/* A single reusable PSRAM frame includes the glyph for the drawer compositor. */
const uint16_t *nuwax_conversation_visual_frozen_rgb565(
    const uint16_t *base, size_t stride);
bool nuwax_conversation_visual_draw_frozen(void *layer,
    const uint16_t *base, size_t stride);
