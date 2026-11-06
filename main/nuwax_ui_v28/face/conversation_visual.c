#include "conversation_visual.h"
#include "conversation_visual_assets.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include <string.h>

#define SIDE 360
#define GLYPH_W 44
#define GLYPH_H 24
#define GLYPH_X 158
#define GLYPH_Y 320
#define PERIOD_US 50000LL

static lv_obj_t *s_glyph;
static uint16_t *s_frame;
static lv_image_dsc_t s_frame_dsc, s_tile_dsc;
static nuwax_conversation_visual_t s_requested, s_mode;
static bool s_frozen, s_snapshot_ready;
static int64_t s_started_us, s_next_us, s_frozen_us;
static unsigned s_phase;
static uint8_t *s_glyph_pixels[2];
static unsigned s_glyph_slot;
static bool s_decode_failed;

static void update_glyph(unsigned phase)
{
    if (s_decode_failed) return;
    const nuwax_asset_rle_t *asset = s_mode == NUWAX_CONVERSATION_VISUAL_LISTENING
        ? &conversation_ripple : &conversation_bars;
    const unsigned slot = s_glyph_slot ^ 1U;
    uint8_t *mask = s_glyph_pixels[slot];
    if (!nuwax_asset_rle_decode_frame(asset, phase, mask, GLYPH_W * GLYPH_H)) {
        s_decode_failed = true;
        ESP_LOGE("conversation_visual", "invalid indicator asset; retaining last frame");
        return;
    }
    lv_image_cache_drop(&s_tile_dsc);
    s_tile_dsc.data=mask;
    s_glyph_slot=slot;
    lv_image_set_src(s_glyph,&s_tile_dsc);
    lv_obj_invalidate(s_glyph);
    s_phase=phase;
}

static void descriptor(lv_image_dsc_t *dsc, uint16_t *pixels, int w, int h)
{
    *dsc=(lv_image_dsc_t){
        .header.magic=LV_IMAGE_HEADER_MAGIC,
        .header.cf=LV_COLOR_FORMAT_RGB565,
        .header.w=w, .header.h=h, .header.stride=w*2,
        .data_size=w*h*2, .data=(const uint8_t *)pixels,
    };
}

static void configure_image(lv_obj_t *obj, const lv_image_dsc_t *dsc,
                            int x, int y, int w, int h)
{
    lv_obj_remove_style_all(obj);
    lv_image_set_src(obj,dsc);
    lv_obj_set_pos(obj,x,y);
    lv_obj_set_size(obj,w,h);
    lv_obj_remove_flag(obj,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(obj,LV_OBJ_FLAG_HIDDEN);
}

esp_err_t nuwax_conversation_visual_create(lv_obj_t *parent)
{
    if (!parent || s_frame) return ESP_ERR_INVALID_STATE;
    /* Only a transparent glyph is displayed. This reserved frame is solely
     * for the drawer's frozen composition, never a replacement expression. */
    s_frame=heap_caps_malloc(SIDE*SIDE*sizeof(uint16_t) + 2U*GLYPH_W*GLYPH_H,
                            MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (!s_frame) goto fail;
    s_glyph_pixels[0]=(uint8_t *)(s_frame+SIDE*SIDE);
    s_glyph_pixels[1]=s_glyph_pixels[0]+GLYPH_W*GLYPH_H;
    s_glyph_slot=0;
    s_decode_failed=false;
    if (!nuwax_asset_rle_decode_frame(&conversation_ripple,0,s_glyph_pixels[0],GLYPH_W*GLYPH_H)) {
        nuwax_conversation_visual_delete();
        return ESP_ERR_INVALID_STATE;
    }
    s_glyph=lv_image_create(parent);
    if (!s_glyph) goto fail;
    descriptor(&s_frame_dsc,s_frame,SIDE,SIDE);
    s_tile_dsc=(lv_image_dsc_t){
        .header.magic=LV_IMAGE_HEADER_MAGIC,.header.cf=LV_COLOR_FORMAT_A8,
        .header.w=GLYPH_W,.header.h=GLYPH_H,.header.stride=GLYPH_W,
        .header.flags=LV_IMAGE_FLAGS_MODIFIABLE,
        .data_size=GLYPH_W*GLYPH_H,.data=s_glyph_pixels[0],
    };
    configure_image(s_glyph,&s_tile_dsc,GLYPH_X,GLYPH_Y,GLYPH_W,GLYPH_H);
    lv_obj_set_style_image_recolor(s_glyph,lv_color_white(),0);
    lv_obj_set_style_image_recolor_opa(s_glyph,LV_OPA_COVER,0);
    return ESP_OK;
fail:
    nuwax_conversation_visual_delete();
    return ESP_ERR_NO_MEM;
}

void nuwax_conversation_visual_delete(void)
{
    if (s_glyph) lv_obj_delete(s_glyph);
    s_glyph=NULL;
    lv_image_cache_drop(&s_frame_dsc);
    lv_image_cache_drop(&s_tile_dsc);
    heap_caps_free(s_frame);
    s_frame=NULL;
    s_glyph_pixels[0]=s_glyph_pixels[1]=NULL;
    s_decode_failed=false;
    s_requested=s_mode=NUWAX_CONVERSATION_VISUAL_NONE;
    s_frozen=s_snapshot_ready=false;
    s_started_us=s_next_us=s_frozen_us=0;
    s_phase=0;
}

void nuwax_conversation_visual_request(nuwax_conversation_visual_t mode)
{
    s_requested=mode;
}

void nuwax_conversation_visual_tick(void)
{
    if (!s_frame || s_frozen) return;
    const int64_t now=esp_timer_get_time();
    if (s_mode != s_requested) {
        s_mode=s_requested;
        ESP_LOGI("conversation_visual","mode=%s overlay_only=1 indicator_fps=20 text=0",
                 s_mode==NUWAX_CONVERSATION_VISUAL_LISTENING ? "listening" :
                 s_mode==NUWAX_CONVERSATION_VISUAL_SPEAKING ? "playback" : "hidden");
        s_snapshot_ready=false;
        s_started_us=now;
        s_next_us=now+PERIOD_US;
        lv_obj_add_flag(s_glyph,LV_OBJ_FLAG_HIDDEN);
        if (s_mode != NUWAX_CONVERSATION_VISUAL_NONE) {
            update_glyph(0);
            lv_obj_remove_flag(s_glyph,LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    if (s_mode == NUWAX_CONVERSATION_VISUAL_NONE || now<s_next_us) return;
    s_next_us=now+PERIOD_US; /* Skip missed frames; never run catch-up work. */
    const unsigned step=(unsigned)((now-s_started_us)/PERIOD_US);
    const unsigned frame_count=s_mode==NUWAX_CONVERSATION_VISUAL_LISTENING
        ? conversation_ripple.frame_count : conversation_bars.frame_count;
    const unsigned phase=step%frame_count;
    if (phase!=s_phase) update_glyph(phase);
}

void nuwax_conversation_visual_freeze(bool frozen)
{
    if (s_frozen==frozen) return;
    s_frozen=frozen;
    s_snapshot_ready=false;
    if (frozen) s_frozen_us=esp_timer_get_time();
    else {
        s_started_us+=esp_timer_get_time()-s_frozen_us;
        s_next_us=esp_timer_get_time()+PERIOD_US;
    }
}

const uint16_t *nuwax_conversation_visual_frozen_rgb565(
    const uint16_t *base, size_t stride)
{
    if (!s_frozen || !s_frame || s_mode==NUWAX_CONVERSATION_VISUAL_NONE) return NULL;
    if (!base || stride<SIDE) return NULL;
    if (!s_snapshot_ready) {
        for (int y=0; y<SIDE; ++y)
            memcpy(s_frame+y*SIDE,base+y*stride,SIDE*sizeof(uint16_t));
        for (int y=0; y<GLYPH_H; ++y) {
            for (int x=0; x<GLYPH_W; ++x) {
                uint16_t *dst=s_frame+(GLYPH_Y+y)*SIDE+GLYPH_X+x;
                /* Match the live LVGL A8 blend, without clearing the base. */
                *dst=lv_color_16_16_mix(0xffff,*dst,s_tile_dsc.data[y*GLYPH_W+x]);
            }
        }
        s_snapshot_ready=true;
    }
    return s_frame;
}

bool nuwax_conversation_visual_draw_frozen(void *layer_ptr,
    const uint16_t *base,size_t stride)
{
    if (!layer_ptr || !nuwax_conversation_visual_frozen_rgb565(base,stride)) return false;
    lv_layer_t *layer=layer_ptr;
    lv_area_t area;
    lv_obj_get_coords(s_glyph,&area);
    area.x1-=GLYPH_X; area.y1-=GLYPH_Y;
    area.x2=area.x1+SIDE-1; area.y2=area.y1+SIDE-1;
    lv_draw_image_dsc_t draw;
    lv_draw_image_dsc_init(&draw);
    draw.base.layer=layer;
    draw.src=&s_frame_dsc;
    draw.opa=LV_OPA_COVER;
    draw.image_area=area;
    lv_draw_image(layer,&draw,&area);
    return true;
}
