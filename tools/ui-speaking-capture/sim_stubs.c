#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include "esp_err.h"
#include "lvgl.h"
#include "nuwax_ui_v28/face/emotion_animation_player.h"

static lv_obj_t *s_background;
static lv_image_dsc_t s_background_dsc;
static uint16_t s_black_pixels[360 * 360];
static bool s_paused;
int sim_fail_allocation;
unsigned sim_player_ticks;
unsigned sim_random_calls;
uint32_t sim_random_values[2];
nuwax_emotion_asset_id_t sim_asset;
unsigned sim_asset_sets;
uint32_t esp_random(void)
{
    return sim_random_values[sim_random_calls++ % 2U];
}
const uint16_t *sim_base_pixels(void) { return s_black_pixels; }
void sim_advance_base(void)
{
    s_black_pixels[140*360+125]^=0xffff;
    s_black_pixels[320*360+158]^=0xffff;
    lv_obj_invalidate(s_background);
}
size_t sim_caps_live_bytes;
unsigned sim_caps_alloc_calls;
static struct { void *ptr; size_t bytes; } sim_caps[64];
void *heap_caps_malloc(size_t n,uint32_t caps) {
    (void)caps; ++sim_caps_alloc_calls;
    if (sim_fail_allocation) return NULL;
    void *p=malloc(n); if (!p) return NULL;
    for (unsigned i=0;i<64;i++) if (!sim_caps[i].ptr) {
        sim_caps[i].ptr=p;sim_caps[i].bytes=n;sim_caps_live_bytes+=n;return p;
    }
    assert(!"simulation allocation tracking full");return NULL;
}
void *heap_caps_calloc(size_t n,size_t size,uint32_t caps) {
    void *p=heap_caps_malloc(n*size,caps);if(p)memset(p,0,n*size);return p;
}
void heap_caps_free(void *p) {
    if (!p) return;
    for (unsigned i=0;i<64;i++) if (sim_caps[i].ptr==p) {
        sim_caps_live_bytes-=sim_caps[i].bytes;sim_caps[i].ptr=NULL;break;
    }
    free(p);
}

int nuwax_sim_log(const char *level, const char *tag, const char *fmt, ...)
{
    va_list args;
    fprintf(stderr, "[%s] %s: ", level, tag ? tag : "sim");
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    return 0;
}

const char *esp_err_to_name(esp_err_t err)
{
    switch (err) {
        case ESP_OK: return "ESP_OK";
        case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
        case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
        case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
        default: return "ESP_FAIL";
    }
}

int64_t esp_timer_get_time(void)
{
    return (int64_t)lv_tick_get() * 1000LL;
}

esp_err_t nuwax_emotion_animation_player_init(lv_obj_t *parent)
{
    /* Nonzero content detects destructive overlay clears and bad row strides. */
    for (unsigned i=0;i<360*360;i++) s_black_pixels[i]=(uint16_t)(i*37U+1U);
    s_background_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_background_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_background_dsc.header.w = 360;
    s_background_dsc.header.h = 360;
    s_background_dsc.header.stride = 360 * 2;
    s_background_dsc.data_size = sizeof(s_black_pixels);
    s_background_dsc.data = (const uint8_t *)s_black_pixels;
    s_background = lv_image_create(parent);
    if (!s_background) return ESP_ERR_NO_MEM;
    lv_image_set_src(s_background, &s_background_dsc);
    lv_obj_set_pos(s_background, 0, 0);
    lv_obj_set_size(s_background, 360, 360);
    return ESP_OK;
}

esp_err_t nuwax_emotion_animation_player_init_with_initial_asset(lv_obj_t *parent,nuwax_emotion_asset_id_t asset)
{ (void)asset; s_paused=false; return nuwax_emotion_animation_player_init(parent); }
esp_err_t nuwax_emotion_animation_player_set_asset_immediate(nuwax_emotion_asset_id_t asset)
{ return nuwax_emotion_animation_player_set_asset(asset); }
esp_err_t nuwax_emotion_animation_player_recover(void) { return ESP_OK; }
void nuwax_emotion_animation_player_service_decode(void) {}

esp_err_t nuwax_emotion_animation_player_deinit(void)
{
    if (s_background) lv_obj_delete(s_background);
    s_background = NULL;
    return ESP_OK;
}

esp_err_t nuwax_emotion_animation_player_reap(void) { return ESP_OK; }
esp_err_t nuwax_emotion_animation_player_set_asset(
    nuwax_emotion_asset_id_t asset_id)
{
    sim_asset = asset_id;
    sim_asset_sets++;
    return ESP_OK;
}
esp_err_t nuwax_emotion_animation_player_trigger_touch(void) { return ESP_OK; }
esp_err_t nuwax_emotion_animation_player_tick(void) { if (!s_paused) sim_player_ticks++; return ESP_OK; }
esp_err_t nuwax_emotion_animation_player_freeze(void) { s_paused=true; return ESP_OK; }
void nuwax_emotion_animation_player_set_interaction_active(bool active)
{
    s_paused=active;
}
void nuwax_emotion_animation_player_set_overlay_progress(uint16_t progress)
{
    (void)progress;
}
bool nuwax_emotion_animation_player_frozen_frame_ready(void) { return s_paused; }
const uint16_t *nuwax_emotion_animation_player_frozen_rgb565(size_t *stride)
{ if (stride) *stride=s_paused ? 360 : 0; return s_paused ? s_black_pixels : NULL; }
bool nuwax_emotion_animation_player_draw_frozen(void *layer)
{
    (void)layer;
    return false;
}
bool nuwax_emotion_animation_player_ready(void) { return true; }
/* This isolated face fixture has no drawer. Full OTA/menu takeover is tested
 * with the production drawer in tools/ui-drawer-capture. */
void nuwax_drawer_set_ota_blocked(bool blocked) { (void)blocked; }
bool sim_power_blocked;
void nuwax_drawer_set_power_blocked(bool blocked) { sim_power_blocked=blocked; }
