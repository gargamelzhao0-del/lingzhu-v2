#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "lvgl.h"
#include "nuwax_ui_v28/nuwax_face.h"
#include "nuwax_ui_v28/face/conversation_visual.h"
#include "nuwax_ui_v28/ota_progress.h"
#include "nuwax_ui_v28/low_battery_warning.h"
#include "nuwax_ota_new/nuwax_ota_ui_policy.h"

#define CAPTURE_W 360
#define CAPTURE_H 360
#define DRAW_LINES 24

static uint16_t s_framebuffer[CAPTURE_W * CAPTURE_H];
static uint16_t s_draw_buffer[CAPTURE_W * DRAW_LINES];
static uint32_t s_tick_ms = 1000U;
static lv_display_t *s_display;
static uint16_t s_saved[CAPTURE_W*CAPTURE_H];
extern int sim_fail_allocation;
extern unsigned sim_player_ticks;
extern unsigned sim_random_calls;
extern uint32_t sim_random_values[2];
extern nuwax_emotion_asset_id_t sim_asset;
extern unsigned sim_asset_sets;
extern const uint16_t *sim_base_pixels(void);
extern void sim_advance_base(void);
static uint32_t s_flush_pixels;

static uint32_t capture_tick_cb(void) { return s_tick_ms; }

static void capture_flush_cb(lv_display_t *display,
                             const lv_area_t *area,
                             uint8_t *pixels)
{
    s_flush_pixels+=(uint32_t)lv_area_get_width(area)*lv_area_get_height(area);
    const int32_t width = lv_area_get_width(area);
    const uint16_t *source = (const uint16_t *)pixels;
    for (int32_t y = area->y1; y <= area->y2; ++y) {
        for (int32_t x = area->x1; x <= area->x2; ++x) {
            const uint16_t pixel = *source++;
            if (x >= 0 && x < CAPTURE_W && y >= 0 && y < CAPTURE_H) {
                s_framebuffer[y * CAPTURE_W + x] = pixel;
            }
        }
        if (width <= 0) break;
    }
    lv_display_flush_ready(display);
}

static void run_until(uint32_t target_ms)
{
    while (s_tick_ms < target_ms) {
        const uint32_t remaining = target_ms - s_tick_ms;
        s_tick_ms += remaining > 5U ? 5U : remaining;
        nuwax_face_tick();
        lv_anim_refr_now();
        lv_timer_handler();
        lv_refr_now(s_display);
    }
}

static bool write_ppm(const char *path)
{
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    fprintf(file, "P6\n%d %d\n255\n", CAPTURE_W, CAPTURE_H);
    for (size_t i = 0; i < CAPTURE_W * CAPTURE_H; ++i) {
        const uint16_t pixel = s_framebuffer[i];
        const uint8_t rgb[3] = {
            (uint8_t)((((pixel >> 11) & 0x1f) * 255 + 15) / 31),
            (uint8_t)((((pixel >> 5) & 0x3f) * 255 + 31) / 63),
            (uint8_t)(((pixel & 0x1f) * 255 + 15) / 31),
        };
        fwrite(rgb, 1, sizeof(rgb), file);
    }
    fclose(file);
    return true;
}

static void policy_checks(void)
{
    nuwax_device_state_t s={.wifi=NUWAX_WIFI_CONNECTED,.cloud=NUWAX_CLOUD_CONNECTED,
        .dialog=NUWAX_DIALOG_LISTENING};
    assert(nuwax_conversation_visual_for_state(&s)==NUWAX_CONVERSATION_VISUAL_LISTENING);
    s.dialog=NUWAX_DIALOG_SPEAKING;
    assert(nuwax_conversation_visual_for_state(&s)==NUWAX_CONVERSATION_VISUAL_NONE);
    s.speaking_pcm_active=true;
    assert(nuwax_conversation_visual_for_state(&s)==NUWAX_CONVERSATION_VISUAL_SPEAKING);
    s.dialog=NUWAX_DIALOG_LISTENING;
    assert(nuwax_conversation_visual_for_state(&s)==NUWAX_CONVERSATION_VISUAL_NONE);
    s.speaking_pcm_active=false;
#define BLOCK(field,value) do { nuwax_device_state_t b=s; b.field=value; \
    assert(nuwax_conversation_visual_for_state(&b)==NUWAX_CONVERSATION_VISUAL_NONE); } while(0)
    BLOCK(dialog,NUWAX_DIALOG_THINKING);
    BLOCK(dialog,NUWAX_DIALOG_SLEEP);
    BLOCK(dialog,NUWAX_DIALOG_RECOGNIZING);
    BLOCK(cloud,NUWAX_CLOUD_DISCONNECTED);
    BLOCK(wifi,NUWAX_WIFI_CONNECTING);
    BLOCK(provisioning_active,true);
    BLOCK(provisioning_starting,true);
    BLOCK(meeting,NUWAX_MEETING_UI_ACTIVE);
    BLOCK(monitoring,NUWAX_MONITORING_UI_ACTIVE);
    BLOCK(touch_emotion_active,true);
    BLOCK(ota_update_active,true);
    BLOCK(app_low_power,true);
#undef BLOCK
    puts("PASS: listening/playback authority and exclusive-mode gates");
}

static void refresh(void)
{
    nuwax_face_tick();
    lv_timer_handler();
    lv_refr_now(s_display);
}

static void assert_frozen_matches(void)
{
    size_t stride=0;
    const uint16_t *pixels=nuwax_face_frozen_rgb565(&stride);
    assert(pixels && stride==CAPTURE_W);
    assert(memcmp(pixels,s_framebuffer,sizeof(s_framebuffer))==0);
}

static void assert_original_face_preserved(void)
{
    const uint16_t *base=sim_base_pixels();
    for (int y=0;y<360;y++) for (int x=0;x<360;x++)
        if (!(x>=158 && x<202 && y>=320 && y<344))
            assert(s_framebuffer[y*360+x]==base[y*360+x]);
    /* The alpha-zero corner inside the glyph also preserves the base. */
    assert(s_framebuffer[320*360+158]==base[320*360+158]);
}

static void ota_capsule_checks(lv_obj_t *screen)
{
    /* Keep an original expression underneath: OTA covers it temporarily and
     * must restore every pixel on hide, including mid-animation cancellation. */
    static uint16_t base_pixels[360*360];
    FILE *base_file=fopen("ota-sleeping.rgb565", "rb");
    if (base_file) {
        assert(fread(base_pixels,sizeof(base_pixels),1,base_file)==1);
        fclose(base_file);
    } else memcpy(base_pixels,sim_base_pixels(),sizeof(base_pixels));
    lv_image_dsc_t base={0};
    base.header.magic=LV_IMAGE_HEADER_MAGIC;
    base.header.cf=LV_COLOR_FORMAT_RGB565;
    base.header.w=360; base.header.h=360; base.header.stride=720;
    base.data=(const uint8_t *)base_pixels; base.data_size=sizeof(base_pixels);
    lv_obj_t *background=lv_image_create(screen);
    lv_image_set_src(background,&base);
    lv_obj_set_pos(background,0,0);
    refresh();
    memcpy(s_saved, s_framebuffer, sizeof(s_saved));
    nuwax_device_state_t state = {.ota_update_active=true, .ota_update_percent=62};
    assert(nuwax_ota_progress_create(screen)==ESP_OK);
    assert(nuwax_ota_progress_create(screen)==ESP_ERR_INVALID_STATE);
    refresh();
    lv_obj_t *scene=lv_obj_get_child(lv_display_get_layer_top(s_display),-1);
    lv_obj_t *pill=lv_obj_get_child(scene,1);
    assert(lv_obj_has_flag(scene,LV_OBJ_FLAG_HIDDEN));
    assert(!nuwax_ota_progress_is_visible());
    nuwax_device_state_t normal={0};
    nuwax_ota_progress_update(&normal);
    assert(!normal.ota_update_active && normal.ota_update_percent==0);
    run_until(s_tick_ms+25000);
    assert(!nuwax_ota_progress_is_visible());
    assert(memcmp(s_saved,s_framebuffer,sizeof(s_saved))==0);
    puts("PASS: normal boot remains unchanged beyond the former demo cycle; OTA requires real active state");
    nuwax_ota_progress_update(&state);
    refresh();
    assert(lv_obj_has_flag(scene,LV_OBJ_FLAG_CLICKABLE));
    assert(lv_obj_get_y(pill)==252);
    unsigned logo_pixels=0;
    for (unsigned y=0;y<360;y++) for (unsigned x=0;x<360;x++) {
        const bool logo=x>=96 && x<264 && y>=52 && y<239;
        const bool capsule=x>=68 && x<292 && y>=252 && y<304;
        if (!logo && !capsule) assert(s_framebuffer[y*360+x]==0);
        if (logo && s_framebuffer[y*360+x]!=0) ++logo_pixels;
    }
    assert(logo_pixels>8000); /* A8 tint must be white, never invisible black. */
    static uint16_t logo_before[360*252];
    memcpy(logo_before,s_framebuffer,sizeof(logo_before));
    assert(write_ppm("ota-62.ppm"));
    /* The production widget draws only when its own state changes. */
    s_flush_pixels=0;
    nuwax_ota_progress_update(&state);
    lv_refr_now(s_display);
    assert(s_flush_pixels==0);
    state.ota_update_percent=255;
    nuwax_ota_progress_update(&state);
    run_until(s_tick_ms+250);
    assert(memcmp(logo_before,s_framebuffer,sizeof(logo_before))==0);
    assert(write_ppm("ota-100.ppm"));
    state.ota_update_percent=100;
    const char *success_phases[]={"done","rebooting"};
    for (unsigned i=0;i<2;i++) {
        state.ota_update_active=nuwax_ota_ui_progress_active(success_phases[i]);
        nuwax_ota_progress_update(&state);
        run_until(s_tick_ms+500);
        assert(nuwax_ota_progress_is_visible());
        assert(strcmp(lv_label_get_text(lv_obj_get_child(pill,1)),"100%")==0);
    }
    puts("PASS: real OTA done/rebooting retains 100% scene until reset or idle");
    state.ota_update_active=nuwax_ota_ui_progress_active("failed");
    nuwax_ota_progress_update(&state);
    refresh();
    assert(memcmp(s_saved,s_framebuffer,sizeof(s_saved))==0);
    state.ota_update_active=nuwax_ota_ui_progress_active("idle");
    nuwax_ota_progress_update(&state);
    assert(!nuwax_ota_progress_is_visible());
    state.ota_update_active=true;
    state.ota_update_percent=0;
    nuwax_ota_progress_update(&state);
    refresh();
    assert(write_ppm("ota-0.ppm"));
    const unsigned anims=lv_anim_count_running();
    state.ota_update_percent=83;
    nuwax_ota_progress_update(&state);
    assert(lv_anim_count_running()>anims);
    state.ota_update_active=false;
    nuwax_ota_progress_update(&state);
    assert(lv_anim_count_running()==anims);
    state.ota_update_active=true;
    state.ota_update_percent=0;
    nuwax_ota_progress_update(&state);
    state.ota_update_percent=83;
    nuwax_ota_progress_update(&state);
    nuwax_ota_progress_delete(); /* Deletion also cancels an active bar animation. */
    run_until(s_tick_ms+250);
    refresh();
    assert(memcmp(s_saved,s_framebuffer,sizeof(s_saved))==0);
    assert(nuwax_ota_progress_create(screen)==ESP_OK);
    nuwax_ota_progress_delete();
    lv_obj_delete(background);
    puts("PASS: OTA logo scene covers expression, preserves logo across progress updates, clamps percentage, skips identical updates and restores original pixels on hide/delete");
}

static void fallback_advance(uint32_t ms)
{
    s_tick_ms += ms;
    nuwax_face_tick();
}

static void ota_cover_checks(lv_obj_t *screen)
{
    nuwax_device_state_t state = {.wifi=NUWAX_WIFI_CONNECTED,
        .cloud=NUWAX_CLOUD_CONNECTED,.dialog=NUWAX_DIALOG_SLEEP,
        .ota_update_active=true,.ota_update_percent=62};
    assert(nuwax_ota_progress_create(screen)==ESP_OK);
    nuwax_ota_progress_update(&state);
    const unsigned ticks=sim_player_ticks, sets=sim_asset_sets;
    nuwax_face_update(&state);
    nuwax_face_set_menu_open(true);
    nuwax_face_set_menu_open(false); /* Drawer release must not release OTA. */
    run_until(s_tick_ms+1000);
    assert(sim_player_ticks==ticks && sim_asset_sets==sets);
    state.ota_update_active=false;
    nuwax_face_update(&state);
    nuwax_ota_progress_update(&state);
    assert(!nuwax_ota_progress_is_visible());
    assert(sim_asset==NUWAX_EMOTION_ASSET_SLEEPING);
    run_until(s_tick_ms+100);
    assert(sim_player_ticks>ticks);
    nuwax_face_set_menu_open(true);
    state.ota_update_active=true;
    nuwax_ota_progress_update(&state);
    state.ota_update_active=false;
    nuwax_ota_progress_update(&state);
    const unsigned paused_ticks=sim_player_ticks;
    run_until(s_tick_ms+100);
    assert(sim_player_ticks==paused_ticks); /* OTA release retains menu owner. */
    nuwax_face_set_menu_open(false);
    nuwax_ota_progress_delete();
    puts("PASS: OTA pauses covered expression, defers state and preserves independent drawer pause ownership");
}

static void speaking_fallback_checks(void)
{
    nuwax_device_state_t s = {.wifi=NUWAX_WIFI_CONNECTED,
        .cloud=NUWAX_CLOUD_CONNECTED, .dialog=NUWAX_DIALOG_LISTENING,
        .emotion=NUWAX_EMOTION_HAPPY};
    nuwax_face_update(&s);
    sim_random_calls=0;
    sim_random_values[0]=0;
    sim_random_values[1]=0;
    s.dialog=NUWAX_DIALOG_SPEAKING;
    nuwax_face_update(&s);
    fallback_advance(8000);
    assert(sim_random_calls==0); /* No animation timer before actual PCM. */
    s.speaking_pcm_active=true;
    nuwax_face_update(&s);
    assert(sim_asset==NUWAX_EMOTION_ASSET_HAPPY && sim_random_calls==2);
    for (unsigned i=0;i<29;i++) {
        fallback_advance(100);
        nuwax_face_update(&s); /* Status updates must not restart the lease. */
    }
    fallback_advance(99);
    assert(sim_random_calls==2);
    sim_random_values[0]=1;
    sim_random_values[1]=4000;
    fallback_advance(1);
    assert(sim_asset==NUWAX_EMOTION_ASSET_PLAYFUL && sim_random_calls==4);
    fallback_advance(6999);
    assert(sim_random_calls==4);
    const unsigned sets=sim_asset_sets;
    fallback_advance(1);
    assert(sim_random_calls==6 && sim_asset_sets==sets);
    fallback_advance(30000); /* A delayed UI tick never replays missed changes. */
    assert(sim_random_calls==8);
    s.semantic_emotion_active=true;
    s.emotion=NUWAX_EMOTION_ANGRY;
    nuwax_face_update(&s);
    fallback_advance(10000);
    assert(sim_asset==NUWAX_EMOTION_ASSET_ANGRY && sim_random_calls==8);
    s.semantic_emotion_active=false;
    s.emotion=NUWAX_EMOTION_HAPPY;
    nuwax_face_update(&s);
    fallback_advance(10000);
    assert(sim_random_calls==8); /* Cloud ownership persists for this playback. */

    s.dialog=NUWAX_DIALOG_LISTENING;
    s.speaking_pcm_active=false;
    nuwax_face_update(&s);
    sim_random_values[1]=0;
    s.dialog=NUWAX_DIALOG_SPEAKING;
    s.speaking_pcm_active=true;
    nuwax_face_update(&s);
    assert(sim_random_calls==10);
    fallback_advance(1000);
    assert(nuwax_face_freeze_interaction()==ESP_OK);
    fallback_advance(20000);
    nuwax_face_update(&s);
    assert(sim_random_calls==10);
    nuwax_face_set_menu_open(false);
    fallback_advance(1999);
    assert(sim_random_calls==10);
    fallback_advance(1);
    assert(sim_random_calls==12);
    s.speaking_pcm_active=false;
    nuwax_face_update(&s);
    fallback_advance(8000);
    assert(sim_random_calls==12);

    for (unsigned gate=0;gate<7;gate++) {
        nuwax_device_state_t blocked=s;
        blocked.speaking_pcm_active=true;
        switch (gate) {
            case 0: blocked.ota_update_active=true; break;
            case 1: blocked.meeting=NUWAX_MEETING_UI_ACTIVE; break;
            case 2: blocked.monitoring=NUWAX_MONITORING_UI_ACTIVE; break;
            case 3: blocked.provisioning_starting=true; break;
            case 4: blocked.wifi=NUWAX_WIFI_DISCONNECTED; break;
            case 5: blocked.touch_emotion_active=true; break;
            default: blocked.app_low_power=true; break;
        }
        nuwax_face_update(&blocked);
        fallback_advance(8000);
        assert(sim_random_calls==12);
    }
    s.dialog=NUWAX_DIALOG_LISTENING;
    nuwax_face_update(&s);
    assert(sim_asset==NUWAX_EMOTION_ASSET_LISTENING);
    puts("PASS: speaking fallback 3-7s boundaries, random repeats, cloud ownership, PCM stop, menu pause and mode gates");
}

static void low_battery_checks(lv_obj_t *screen)
{
    extern bool sim_power_blocked;
    extern size_t sim_caps_live_bytes;
    extern unsigned sim_caps_alloc_calls;
    const size_t baseline_bytes=sim_caps_live_bytes;
    assert(nuwax_low_battery_warning_create(screen)==ESP_OK);
    assert(sim_caps_live_bytes==baseline_bytes+8112U);
    assert(!nuwax_low_battery_warning_is_visible());
    refresh();
    assert(nuwax_low_battery_warning_show(true)==ESP_OK);
    assert(sim_caps_live_bytes==baseline_bytes+3U*8112U);
    const unsigned animation_alloc_calls=sim_caps_alloc_calls;
    refresh();
    assert(nuwax_low_battery_warning_is_visible() && sim_power_blocked);
    assert(write_ppm("low-battery-start.ppm"));
    memcpy(s_saved,s_framebuffer,sizeof(s_saved));
    const unsigned player_ticks=sim_player_ticks;
    const uint32_t start=s_tick_ms;
    s_flush_pixels=0;
    unsigned changed_frames=0;
    static uint16_t previous[360*360];
    memcpy(previous,s_framebuffer,sizeof(previous));
    for (unsigned i=1;i<=200;i++) {
        run_until(start+i*25U);
        if (memcmp(previous,s_framebuffer,sizeof(previous))!=0) changed_frames++;
        memcpy(previous,s_framebuffer,sizeof(previous));
        for (int y=0;y<360;y++) for (int x=0;x<360;x++) {
            if (!(x>=128 && x<232 && y>=95 && y<173))
                assert(s_saved[y*360+x]==s_framebuffer[y*360+x]);
        }
        if (i==50) assert(write_ppm("low-battery-pulse.ppm"));
        if (i<=100) {
            char name[64];
            snprintf(name,sizeof(name),"low-battery-frame-%03u.ppm",i);
            assert(write_ppm(name));
        }
    }
    assert(changed_frames>=160); /* Smooth geometry across both 2.5-second cycles. */
    assert(sim_caps_alloc_calls==animation_alloc_calls); /* No allocations per frame. */
    printf("low-battery 5s changed_frames=%u/200 damage=%u pixels\n",changed_frames,s_flush_pixels);
    assert(s_flush_pixels<=201U*104U*78U); /* No full-screen or transform redraws. */
    /* A repeated cycle must return to the same rest geometry/brightness. */
    unsigned end_delta=0;
    for (unsigned i=0;i<360U*360U;i++) end_delta+=(s_saved[i]!=s_framebuffer[i]);
    assert(end_delta<100);
    assert(sim_player_ticks==player_ticks); /* The expression player is covered. */
    assert(write_ppm("low-battery-return.ppm"));
    assert(nuwax_low_battery_warning_show(false)==ESP_OK);
    assert(sim_caps_live_bytes==baseline_bytes+8112U); /* Transient cache released. */
    assert(!sim_power_blocked);
    refresh();
    assert_original_face_preserved();
    run_until(start+5300);
    assert(sim_player_ticks>player_ticks);
    assert(nuwax_low_battery_warning_show(true)==ESP_OK);
    refresh();
    nuwax_low_battery_warning_delete();
    assert(!nuwax_low_battery_warning_is_visible() && !sim_power_blocked);
    assert(sim_caps_live_bytes==baseline_bytes);
    assert(nuwax_low_battery_warning_create(screen)==ESP_OK);
    nuwax_low_battery_warning_delete();
    sim_fail_allocation=1;
    assert(nuwax_low_battery_warning_create(screen)==ESP_OK);
    assert(nuwax_low_battery_warning_show(true)==ESP_OK);
    refresh();
    assert(write_ppm("low-battery-low-memory.ppm"));
    memcpy(s_saved,s_framebuffer,sizeof(s_saved));
    run_until(start+6500);
    assert(memcmp(s_saved,s_framebuffer,sizeof(s_saved))==0);
    nuwax_low_battery_warning_delete();
    sim_fail_allocation=0;
    /* Output buffer exists, but the new transient decode cache cannot be
     * allocated. Keep the same static warning; retry on the next show. */
    assert(nuwax_low_battery_warning_create(screen)==ESP_OK);
    sim_fail_allocation=1;
    assert(nuwax_low_battery_warning_show(true)==ESP_OK);
    refresh();
    assert(write_ppm("low-battery-cache-low-memory.ppm"));
    memcpy(s_saved,s_framebuffer,sizeof(s_saved));
    run_until(s_tick_ms+1000);
    assert(memcmp(s_saved,s_framebuffer,sizeof(s_saved))==0);
    assert(nuwax_low_battery_warning_show(false)==ESP_OK);
    sim_fail_allocation=0;
    assert(nuwax_low_battery_warning_show(true)==ESP_OK);
    refresh();
    memcpy(s_saved,s_framebuffer,sizeof(s_saved));
    run_until(s_tick_ms+1000);
    assert(memcmp(s_saved,s_framebuffer,sizeof(s_saved))!=0);
    nuwax_low_battery_warning_delete();
    assert(sim_caps_live_bytes==baseline_bytes);
    assert(nuwax_low_battery_warning_create(screen)==ESP_OK);
    for (unsigned i=0;i<100;i++) {
        assert(nuwax_low_battery_warning_show(true)==ESP_OK);
        assert(sim_caps_live_bytes==baseline_bytes+3U*8112U);
        assert(nuwax_low_battery_warning_show(false)==ESP_OK);
        assert(sim_caps_live_bytes==baseline_bytes+8112U);
    }
    nuwax_low_battery_warning_delete();
    assert(sim_caps_live_bytes==baseline_bytes);
    puts("PASS: low-battery real LVGL render, small-region pulse, static text, modal cancellation and recreate");
}

int main(void)
{
    setbuf(stdout,NULL);
    setbuf(stderr,NULL);
    policy_checks();

    lv_init();
    lv_tick_set_cb(capture_tick_cb);
    s_display = lv_display_create(CAPTURE_W, CAPTURE_H);
    if (!s_display) return 3;
    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_display, capture_flush_cb);
    lv_display_set_buffers(s_display, s_draw_buffer, NULL,
                           sizeof(s_draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    if (nuwax_face_create(screen) != ESP_OK) return 4;

    nuwax_device_state_t state = {0};
    state.emotion = NUWAX_EMOTION_LISTENING;
    state.wifi = NUWAX_WIFI_CONNECTED;
    state.cloud = NUWAX_CLOUD_CONNECTED;
    state.dialog = NUWAX_DIALOG_LISTENING;
    nuwax_face_update(&state);
    lv_obj_invalidate(screen);
    refresh();
    assert_original_face_preserved();
    sim_advance_base();
    refresh();
    assert_original_face_preserved();
    assert(write_ppm("listening.ppm"));
    memcpy(s_saved,s_framebuffer,sizeof(s_saved));
    const unsigned base_ticks=sim_player_ticks;
    s_flush_pixels=0;
    run_until(1550);
    assert(sim_player_ticks>base_ticks);
    assert(memcmp(s_saved,s_framebuffer,sizeof(s_saved))!=0);
    for (int y=0;y<360;y++) for (int x=0;x<360;x++)
        if (!(x>=158 && x<202 && y>=320 && y<344))
            assert(s_saved[y*360+x]==s_framebuffer[y*360+x]);
    assert(s_flush_pixels<20000); /* 11 bounded glyph frames, not full redraws. */
    printf("PASS: 550ms glyph-only damage=%u pixels; original player continues\n",s_flush_pixels);
    assert_original_face_preserved();
    assert(write_ppm("listening-expanded.ppm"));
    assert(nuwax_face_freeze_interaction()==ESP_OK);
    assert_frozen_matches();
    memcpy(s_saved,s_framebuffer,sizeof(s_saved));
    state.dialog=NUWAX_DIALOG_SPEAKING;
    state.speaking_pcm_active=true;
    nuwax_face_update(&state);
    run_until(2200);
    assert(memcmp(s_saved,s_framebuffer,sizeof(s_saved))==0);
    assert_frozen_matches();
    nuwax_face_set_interaction_active(false);
    refresh();
    assert_original_face_preserved();
    assert(sim_player_ticks>base_ticks);
    assert(write_ppm("speaking-overlay.ppm"));
    assert(nuwax_face_freeze_interaction()==ESP_OK);
    assert_frozen_matches();
    puts("PASS: frozen RGB matches live listening and speaking, state deferred during drawer");
    nuwax_face_set_menu_open(false);
    memcpy(s_saved,s_framebuffer,sizeof(s_saved));
    run_until(2550);
    assert(memcmp(s_saved,s_framebuffer,sizeof(s_saved))!=0);
    for (int y=0;y<360;y++) for (int x=0;x<360;x++)
        if (!(x>=158 && x<202 && y>=320 && y<344))
            assert(s_saved[y*360+x]==s_framebuffer[y*360+x]);
    memcpy(s_saved,s_framebuffer,sizeof(s_saved));
    const uint32_t cycle_start=s_tick_ms;
    for (unsigned i=1;i<=60;i++) {
        nuwax_face_update(&state); /* Same-state reports must not restart motion. */
        run_until(cycle_start+i*50U);
    }
    assert(memcmp(s_saved,s_framebuffer,sizeof(s_saved))==0);
    puts("PASS: playback repeats seamlessly at 3000ms despite repeated state reports");
    state.speaking_pcm_active=false;
    nuwax_face_update(&state);
    refresh();
    assert(memcmp(s_framebuffer,sim_base_pixels(),sizeof(s_framebuffer))==0);
    puts("PASS: speaking glyph stops with PCM; no text or stale pixels");
    state.dialog=NUWAX_DIALOG_LISTENING;
    nuwax_face_update(&state);
    refresh();
    run_until(7000); /* Original expression remains visible through every cycle. */
    assert_original_face_preserved();
    ota_cover_checks(screen);
    low_battery_checks(screen);
    speaking_fallback_checks();
    refresh();
    assert(nuwax_face_freeze_interaction()==ESP_OK);
    assert_frozen_matches();
    assert(nuwax_face_deinit()==ESP_OK);
    sim_fail_allocation=1;
    assert(nuwax_face_create(screen)==ESP_OK);
    nuwax_face_update(&state);
    refresh();
    assert(memcmp(s_framebuffer,sim_base_pixels(),sizeof(s_framebuffer))==0);
    assert(nuwax_face_deinit()==ESP_OK);
    sim_fail_allocation=0;
    assert(nuwax_face_create(screen)==ESP_OK);
    nuwax_face_update(&state);
    refresh();
    assert_original_face_preserved();
    assert(memcmp(s_framebuffer,sim_base_pixels(),sizeof(s_framebuffer))!=0);
    assert(nuwax_face_deinit()==ESP_OK);
    puts("PASS: allocation fallback and deinit/recreate lifecycle");
    ota_capsule_checks(screen);
    return 0;
}
