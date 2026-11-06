/* Native LVGL rendering and interaction regression for the production drawer.
 * Only hardware/services are stubbed. Widgets, fonts, compositor and events
 * come from firmware source. Run via tools/capture-lingzhu-ui.py. */
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static size_t strlcpy(char *dst,const char *src,size_t size) {
    size_t n=strlen(src);if(size) {size_t copy=n<size-1?n:size-1;memcpy(dst,src,copy);dst[copy]=0;}return n;
}
#define ESP_LOGD(...) ((void)0)
#include "../../main/nuwax_ui_v28/nuwax_drawer.c"
#include "../../main/nuwax_ui_v28/ota_progress.h"
static bool ota_face_covered;
bool nuwax_emotion_animation_player_transition_active(void) { return false; }
void nuwax_face_set_overlay_covered(bool covered) {ota_face_covered=covered;}

bool nuwax_emotion_animation_player_render_scene(lv_display_t *display, lv_layer_t *layer) {
    (void)display; (void)layer; return false;
}

static uint32_t ticks=1000;
static uint16_t pixels[360*360], draw_buffer[360*24];
static nuwax_lvgl_port_flush_filter_cb_t filter;
static void *filter_arg;
static const char *qr_payload = "https://nuwax.com/lz.html?n=PROV_A1B2C3&u=NX-A1B2-C3D4-E5F6&p=Nuwax7-Dev-PoP";
static bool no_cache;
static unsigned cache_allocations;
static size_t simulated_heap_free = 4U*1024U*1024U;
static bool face_freeze_missing;
static unsigned cached_reference_frames;
static unsigned pager_reference_frames;
static const uint16_t frozen[360*360];
static uint32_t tick(void) { return ticks; }
int64_t esp_timer_get_time(void) {return (int64_t)ticks*1000;}
int nuwax_sim_log(const char *level,const char *tag,const char *fmt,...) {
    va_list ap;va_start(ap,fmt);fprintf(stderr,"%s %s: ",level,tag);vfprintf(stderr,fmt,ap);fputc('\n',stderr);va_end(ap);return 0;
}
const char *esp_err_to_name(esp_err_t err) {return err ? "error" : "OK";}
void *heap_caps_malloc(size_t n,uint32_t caps) {(void)caps;return malloc(n);}
void *heap_caps_aligned_alloc(size_t a,size_t n,uint32_t caps) {(void)a;(void)caps;cache_allocations++;return no_cache?NULL:malloc(n);}
void heap_caps_free(void *p) {free(p);}
size_t heap_caps_get_free_size(uint32_t c) {(void)c;return simulated_heap_free;}
size_t heap_caps_get_largest_free_block(uint32_t c) {(void)c;return 4*1024*1024;}
static const esp_app_desc_t app={.version="7.0.98"};
const esp_app_desc_t *esp_app_get_description(void) {return &app;}
const char *nuwax_device_uid_string(void) {return "NX-A1B2-C3D4-E5F6";}
const char *nuwax_provision_main_qr_payload(void) {return qr_payload;}
esp_err_t nuwax_face_freeze_interaction(void) {return ESP_OK;}
bool nuwax_face_frozen_frame_ready(void) {return !face_freeze_missing;}
const uint16_t *nuwax_face_frozen_rgb565(size_t *stride) {if(stride)*stride=360;return face_freeze_missing?NULL:frozen;}
void nuwax_face_set_overlay_progress(uint16_t p) {(void)p;}
void nuwax_face_set_interaction_active(bool b) {(void)b;}
void nuwax_lvgl_port_note_interaction(void) {}
void nuwax_lvgl_port_set_flush_filter(nuwax_lvgl_port_flush_filter_cb_t f,void *arg) {filter=f;filter_arg=arg;}
static void flush(lv_display_t *d,const lv_area_t *a,uint8_t *p) {
    const int w=lv_area_get_width(a),h=lv_area_get_height(a);
    if(filter)filter(a->x1,a->y1,w,h,(uint16_t *)p,w,filter_arg);
    for(int y=0;y<h;y++)memcpy(pixels+(a->y1+y)*360+a->x1,(uint16_t *)p+y*w,w*2);
    lv_display_flush_ready(d);
}
static void settle(unsigned duration) {
    unsigned end=ticks+duration;
    while(ticks<end) {ticks+=5;lv_timer_handler();lv_refr_now(NULL);}
}
static unsigned obj_draws, cover_checks, task_types[32], presented_endpoints, fast_strips, fast_frames;
static bool scene_probe(lv_display_t *d, lv_layer_t *layer) {
 bool handled=cached_scene_render(d,layer);if(handled)fast_strips++;return handled;
}
static void probe_endpoint(lv_event_t *e) {
 (void)e;
 if(s_present_pending!=CC_PRESENT_NONE) {
   assert(s_transition_cache_active);
   if(s_present_pending==CC_PRESENT_SHEET)assert(s_navigation_cached);
   presented_endpoints++;
 }
}
static void probe_event(lv_event_t *e) {
 if(lv_event_get_code(e)==LV_EVENT_COVER_CHECK) cover_checks++;
 if(lv_event_get_code(e)==LV_EVENT_DRAW_MAIN) obj_draws++;
 else if(lv_event_get_code(e)==LV_EVENT_DRAW_TASK_ADDED) {
   lv_draw_task_t *t=lv_event_get_param(e);if(t->type<32)task_types[t->type]++;
 }
}
static void probe_tree(lv_obj_t *o) {
 lv_obj_add_flag(o,LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
 lv_obj_add_event_cb(o,probe_event,LV_EVENT_ALL,NULL);
 for(unsigned i=0;i<lv_obj_get_child_count(o);i++)probe_tree(lv_obj_get_child(o,i));
}
static void assert_live_pager_equivalent(const uint16_t *a,const uint16_t *b) {
    for(unsigned y=0;y<360;y++)for(unsigned x=0;x<360;x++) {
        unsigned i=y*360+x;
        if(a[i]==b[i])continue;
        assert(x>=CC_PAGER_X && x<CC_PAGER_X+CC_PAGER_W &&
               y>=CC_PAGER_Y && y<CC_PAGER_Y+CC_PAGER_H);
        assert(abs((a[i]&31)-(b[i]&31))<=1);
        assert(abs(((a[i]>>5)&63)-((b[i]>>5)&63))<=1);
        assert(abs((a[i]>>11)-(b[i]>>11))<=1);
    }
}
static void capture(const char *name) {
    lv_refr_now(NULL);
    obj_draws=cover_checks=fast_strips=0;memset(task_types,0,sizeof(task_types));
    static uint16_t incremental[360*360];memcpy(incremental,pixels,sizeof(pixels));
    lv_obj_invalidate(lv_screen_active());lv_refr_now(NULL);
    if(!no_cache && s_qr_reveal_active &&
       (!strncmp(name,"qr-circle-",10) || !strncmp(name,"qr-close-",9))) {
        assert(fast_strips>0 && obj_draws==0);
    }
    if(cache_plane_ready()) {
        const lv_area_t strip={0,100,359,119};
        assert(lv_refr_get_top_obj(&strip,lv_screen_active())==s_cache_plane);
    }
    if (s_scene_pager_visible && s_pager_frame_valid && s_transition_cache_active) {
        const unsigned saved_draws=obj_draws, saved_covers=cover_checks;
        unsigned saved_tasks[32];memcpy(saved_tasks,task_types,sizeof saved_tasks);
        /* Independent reference: use the general LVGL snapshot, including
         * layout and cover search, and compare every pixel/stride row. */
        static uint8_t reference_storage[CC_PAGER_W * CC_PAGER_H * 4];
        lv_draw_buf_t reference;
        assert(lv_draw_buf_init(&reference,CC_PAGER_W,CC_PAGER_H,
            LV_COLOR_FORMAT_RGB565,LV_STRIDE_AUTO,reference_storage,
            sizeof reference_storage)==LV_RESULT_OK);
        assert(lv_snapshot_take_to_draw_buf(s_pager_clip,LV_COLOR_FORMAT_RGB565,
                                           &reference)==LV_RESULT_OK);
        assert(reference.header.w==s_pager_frame_buf.header.w);
        assert(reference.header.h==s_pager_frame_buf.header.h);
        unsigned differences=0, max_channel_delta=0;
        for (unsigned y=0;y<reference.header.h;y++) {
            const uint16_t *a=(const uint16_t *)(reference.data+y*reference.header.stride);
            const uint16_t *b=(const uint16_t *)(s_pager_frame_buf.data+y*s_pager_frame_buf.header.stride);
            for(unsigned x=0;x<reference.header.w;x++) if(a[x]!=b[x]) {
                
                differences++;
                unsigned d=abs((a[x]&31)-(b[x]&31)); if(d>max_channel_delta)max_channel_delta=d;
                d=abs(((a[x]>>5)&63)-((b[x]>>5)&63)); if(d>max_channel_delta)max_channel_delta=d;
                d=abs((a[x]>>11)-(b[x]>>11)); if(d>max_channel_delta)max_channel_delta=d;
            }
        }
        
        /* ARGB child snapshots quantize antialiased edges once more than a
         * direct RGB565 draw. Geometry must match and error is <= one native
         * channel step. Incremental/full and fast/fallback comparisons below
         * remain bit-exact, so stale pixels cannot hide behind this bound. */
        assert(max_channel_delta<=1);
        pager_reference_frames++;
        obj_draws=saved_draws;cover_checks=saved_covers;
        memcpy(task_types,saved_tasks,sizeof saved_tasks);
    }
    if(!no_cache && (!strcmp(name,"qr-enter-05") || !strcmp(name,"page-03") || !strcmp(name,"info"))) {
 if(strcmp(name,"info")) assert(obj_draws<=16);
 fprintf(stderr,"TRACE %s obj_draws=%u covers=%u tasks",name,obj_draws,cover_checks);for(unsigned i=0;i<32;i++)if(task_types[i])fprintf(stderr," %u=%u",i,task_types[i]);fputc('\n',stderr);
 }
 unsigned diff=0;for(unsigned i=0;i<360*360;i++)if(incremental[i]!=pixels[i]) {
        diff++;
    }
    
    /* This fixture deliberately switches to live widget rendering after each
     * capture. Returning to cached ARGB edges may differ by one RGB565 step;
     * all pixels outside the fixed pager remain strictly identical. */
    if(s_qr_reveal_active && diff) {
        for(unsigned i=0;i<360*360;i++)if(incremental[i]!=pixels[i]) {
            fprintf(stderr,"QR DAMAGE %s progress=%d pixel=%u,%u incremental=%x full=%x\n",name,(int)s_backdrop_progress,i%360,i/360,incremental[i],pixels[i]);break;
        }
    }
    assert_live_pager_equivalent(incremental,pixels);
    if(fast_strips) {
        fast_frames++;
        unsigned strips=fast_strips,fast_draws=obj_draws;
        lv_display_set_scene_render_cb(lv_display_get_default(), NULL);
        obj_draws=0;
        lv_obj_invalidate(lv_screen_active());lv_refr_now(NULL);
        assert_live_pager_equivalent(incremental,pixels);
        if(!strcmp(name,"page-03") || !strcmp(name,"qr-enter-05"))
            fprintf(stderr,"SCENE %s strips=%u object draws %u -> %u\n",name,strips,obj_draws,fast_draws);
        lv_display_set_scene_render_cb(lv_display_get_default(), scene_probe);
    }
    if(!no_cache && !strcmp(name,"page-03")) {
        /* A newer screen sibling must force fallback; top-layer overlays must
         * remain on LVGL's unchanged overlay path. Compare both renderers. */
        for(unsigned which=0;which<2;which++) {
            lv_obj_t *overlay=lv_obj_create(which?lv_layer_top():lv_screen_active());
            lv_obj_remove_style_all(overlay);lv_obj_set_size(overlay,31,17);lv_obj_set_pos(overlay,135,305);
            lv_obj_set_style_bg_color(overlay,lv_color_hex(0x40ff20),0);
            lv_obj_set_style_bg_opa(overlay,LV_OPA_COVER,0);
            fast_strips=0;obj_draws=0;lv_obj_invalidate(lv_screen_active());lv_refr_now(NULL);
            unsigned cached_pager_draws=obj_draws;
            assert(which ? fast_strips>0 : fast_strips==0);
            static uint16_t with_overlay[360*360];memcpy(with_overlay,pixels,sizeof pixels);
            lv_display_set_scene_render_cb(lv_display_get_default(),NULL);
            /* Compare optimized fallback with the original per-strip pager
             * traversal, not with another invocation of the same image path. */
            lv_display_remove_event_cb_with_user_data(lv_display_get_default(), cached_scene_frame_cb, NULL);
            s_scene_frame_ready=false; s_scene_pager_visible=false; s_pager_frame_valid=false;
            obj_draws=0;
            lv_obj_invalidate(lv_screen_active());lv_refr_now(NULL);
            assert_live_pager_equivalent(with_overlay,pixels);
            if(!which) {
                assert(obj_draws>cached_pager_draws);
                fprintf(stderr,"PAGER fallback object draws %u -> %u with bounded pager quantization\n",obj_draws,cached_pager_draws);
            }
            lv_display_add_event_cb(lv_display_get_default(), cached_scene_frame_cb, LV_EVENT_RENDER_START, NULL);
            lv_obj_delete(overlay);lv_display_set_scene_render_cb(lv_display_get_default(),scene_probe);
        }
        lv_obj_invalidate(lv_screen_active());lv_refr_now(NULL);
        assert_live_pager_equivalent(incremental,pixels);
        puts("PASS: visible screen sibling fallback and top-layer overlay retained");
    }
    if(cache_plane_ready()) {
        /* Measure both fallback cover searches, independent of the scene hook. */
        lv_display_set_scene_render_cb(lv_display_get_default(), NULL);
        cover_checks=0;lv_obj_invalidate(lv_screen_active());lv_refr_now(NULL);
        unsigned fast_covers=cover_checks;
        bool suspended=suspend_display_invalidation(s_root);
        lv_obj_add_flag(s_cache_plane,LV_OBJ_FLAG_HIDDEN);
        resume_display_invalidation(s_root,suspended);
        cover_checks=0;
        lv_obj_invalidate(lv_screen_active());lv_refr_now(NULL);
        assert_live_pager_equivalent(incremental,pixels);
        if(!strcmp(name,"page-03") || !strcmp(name,"qr-enter-05")) {
            fprintf(stderr,"PLANE %s cover checks %u -> %u\n",name,cover_checks,fast_covers);
            assert(fast_covers<cover_checks);
        }
        suspended=suspend_display_invalidation(s_root);
        lv_obj_remove_flag(s_cache_plane,LV_OBJ_FLAG_HIDDEN);
        resume_display_invalidation(s_root,suspended);
        lv_display_set_scene_render_cb(lv_display_get_default(), scene_probe);
    }
    if (s_navigation_cached) {
        ++cached_reference_frames;
        cc_nav_cache_t *saved = s_nav_active;
        const bool cached = s_transition_cache_active;
        s_navigation_cached = false; s_transition_cache_active = false;
        lv_obj_remove_flag(saved->root, LV_OBJ_FLAG_HIDDEN);
        set_page_tree_mode(s_page, false);
        lv_obj_invalidate(lv_screen_active());lv_refr_now(NULL);
        assert_live_pager_equivalent(incremental,pixels);
        s_navigation_cached = true; s_transition_cache_active = cached;
        lv_obj_add_flag(saved->root, LV_OBJ_FLAG_HIDDEN);
        set_page_tree_mode(s_page, false);
    }
    char path[128];snprintf(path,sizeof(path),"%s.ppm",name);
    FILE *f=fopen(path,"wb");assert(f);fprintf(f,"P6\n360 360\n255\n");
    for(unsigned i=0;i<360*360;i++) {
        uint16_t p=pixels[i];uint8_t rgb[]={((p>>11)&31)*255/31,((p>>5)&63)*255/63,(p&31)*255/31};
        fwrite(rgb,1,3,f);
    }fclose(f);
}
static void capture_motion(const char *prefix, unsigned frames) {
    char name[80];
    for(unsigned i=0;i<frames;i++) {settle(25);snprintf(name,sizeof(name),"%s-%02u",prefix,i);capture(name);}
}
static void check_label(lv_obj_t *obj) {
    if(lv_obj_check_type(obj,&lv_label_class) && !lv_obj_has_flag(obj,LV_OBJ_FLAG_HIDDEN)) {
        const char *text=lv_label_get_text(obj);
        const lv_font_t *font=lv_obj_get_style_text_font(obj,0);
        lv_point_t size;lv_text_get_size(&size,text,font,0,0,1000,LV_TEXT_FLAG_NONE);
        if(obj==s_status_cards[0].value || obj==s_status_cards[1].value || obj==s_status_cards[2].value || obj==s_status_cards[3].value || obj==s_footer_label || obj==s_meeting_state || obj==s_provision_label || obj==s_volume_text) {
            if(size.x>lv_obj_get_width(obj))fprintf(stderr,"OVERFLOW: %s %ld>%ld\n",text,(long)size.x,(long)lv_obj_get_width(obj));
            assert(size.x<=lv_obj_get_width(obj));
        }
    }
    for(unsigned i=0;i<lv_obj_get_child_count(obj);i++)check_label(lv_obj_get_child(obj,i));
}
static void check_pull_motion(void) {
    /* Follow the shell's begin/move/release ordering. Deliberately inject
     * a horizontal request from the same mailbox batch and delayed completion. */
    nuwax_drawer_close();settle(1500);
    for (unsigned cycle=0;cycle<3;cycle++) {
        nuwax_drawer_begin_pull();
        assert(s_open && s_page==CC_PAGE_STATUS && !s_page_transition_active);
        nuwax_drawer_drag_to(-120);
        nuwax_drawer_page_drag_to(-90);nuwax_drawer_next_page();
        assert(!s_page_transition_active && !s_page_dragging);
        assert(no_cache || s_transition_cache_active);
        settle(400); /* Longer than an old horizontal animation. */
        assert(s_page==CC_PAGE_STATUS && s_dragging);
        assert(no_cache || s_transition_cache_active);
        nuwax_drawer_drag_to(0);nuwax_drawer_finish_drag(false);
        nuwax_drawer_page_drag_to(-90);nuwax_drawer_next_page();
        assert(!s_page_transition_active);
        settle(1600);assert(s_reveal_raw==1000 && s_page==CC_PAGE_STATUS);
        nuwax_drawer_next_page();settle(500);assert(s_page==CC_PAGE_MENU);
        nuwax_drawer_drag_to(CC_OPEN_Y);
        assert(nuwax_drawer_tap(100,110).action==NUWAX_DRAWER_ACTION_NONE);
        nuwax_drawer_tap(110,210);assert(!s_click_timer && s_detail==CC_DETAIL_NONE);
        uint8_t blocked_volume=0;
        assert(!nuwax_drawer_hit_volume(CC_VOLUME_BAR_X,275,&blocked_volume));
        nuwax_drawer_drag_to(-40);
        assert(no_cache || s_transition_cache_active);
        nuwax_drawer_finish_drag(true);settle(50);
        nuwax_drawer_drag_to(0);nuwax_drawer_finish_drag(false);
        settle(1600);assert(s_open && s_page==CC_PAGE_MENU && s_reveal_raw==1000);
        nuwax_drawer_close();settle(1600);assert(!s_open && s_page==CC_PAGE_STATUS);
    }
    nuwax_drawer_open();settle(1600);
    nuwax_drawer_next_page();settle(75);assert(s_page_transition_active);
    nuwax_drawer_drag_to(-70);
    const cc_page_t chosen=s_page;
    assert(!s_page_transition_active && !s_page_dragging);
    assert(s_present_pending!=CC_PRESENT_PAGE);
    settle(600);assert(s_page==chosen && s_dragging);
    assert(no_cache || s_transition_cache_active);
    nuwax_drawer_page_finish_drag(true); /* Old horizontal release is inert. */
    nuwax_drawer_finish_drag(false);settle(1600);
    assert(s_page==chosen && s_reveal_raw==1000);
    /* Cancel a page endpoint already waiting for presentation. */
    nuwax_drawer_close();settle(1600);nuwax_drawer_open();settle(1600);
    nuwax_drawer_next_page();
    if (!no_cache) {
        page_progress_exec(s_surface,1000);page_transition_completed(NULL);
        assert(s_present_pending==CC_PRESENT_PAGE);
        nuwax_drawer_drag_to(-70);
        assert(s_present_pending==CC_PRESENT_NONE && !s_page_transition_active);
        settle(400);assert(s_transition_cache_active && s_dragging);
        nuwax_drawer_finish_drag(false);settle(1600);
    } else settle(1600);
    nuwax_drawer_close();settle(1600);
    assert(!s_open && !s_reveal_timer && !s_transition_cache_active);
    assert(!s_page_transition_active && s_present_pending==CC_PRESENT_NONE);
    puts("PASS: status pull entry, vertical/page exclusion, repeated pulls, reverse close and stale completion");
}

static void check_drag_endpoint(void) {
    /* Earlier cache-failure tests deliberately advance different amounts of
     * simulated time. Use stable labels for this cross-run pixel comparison. */
    nuwax_device_state_t stable=s_state;
    stable.wifi=NUWAX_WIFI_CONNECTED;stable.cloud=NUWAX_CLOUD_CONNECTED;
    stable.power=NUWAX_POWER_BATTERY;
    nuwax_drawer_update(&stable);settle(1800);
    nuwax_drawer_open();settle(1800);
    for (unsigned i=0;i<4;i++) {
        const cc_page_t origin=s_page;
        const cc_page_t target=origin==CC_PAGE_STATUS ? CC_PAGE_MENU : CC_PAGE_STATUS;
        nuwax_drawer_page_drag_to(origin==CC_PAGE_STATUS ? -CC_PAGE_DRAG_RANGE_PX : CC_PAGE_DRAG_RANGE_PX);
        if(i&1) lv_refr_now(NULL); /* Endpoint already drawn vs still dirty. */
        nuwax_drawer_page_finish_drag(true);
        if(!no_cache) {
            assert(s_page==origin && s_page_transition_active);
            assert(s_transition_cache_active && s_present_pending==CC_PRESENT_PAGE);
        }
        settle(100);
        assert(s_page==target && !s_page_transition_active);
        assert(!s_transition_cache_active && s_present_pending==CC_PRESENT_NONE);
        capture(target==CC_PAGE_STATUS ? "drag-end-status" : "drag-end-menu");
    }
    /* A changed target page must redraw after handoff, even with a clean
     * origin. The final cached frame is intentionally the old generation. */
    nuwax_drawer_page_drag_to(-CC_PAGE_DRAG_RANGE_PX);
    if(!no_cache) s_status_cache_dirty=false;
    nuwax_device_state_t changed=s_state;changed.volume=37;
    nuwax_drawer_update(&changed);assert(no_cache || s_menu_cache_dirty);
    nuwax_drawer_page_finish_drag(true);settle(100);
    capture("drag-end-updated-target");
    nuwax_drawer_close();settle(1800);
    puts("PASS: full-distance drag endpoint submitted before live handoff; changed target repainted");
}

#include "../../main/nuwax_ui_v28/face/rgb565_scene.h"
static uint16_t scene_frame[360*360];
static lv_obj_t *scene_image;
static unsigned face_scene_fast_strips;
static bool scene_hook(lv_display_t *d, lv_layer_t *l) {
    bool rendered=nuwax_rgb565_scene_render(d,l,scene_image,NULL,scene_frame,360,360);
    if(rendered)face_scene_fast_strips++;
    return rendered;
}
static void check_face_scene(void) {
    lv_obj_t *screen=lv_obj_create(NULL);
    lv_screen_load(screen);
    filter=NULL;
    scene_image=lv_image_create(screen);
    static lv_image_dsc_t image={.header={.magic=LV_IMAGE_HEADER_MAGIC,
        .cf=LV_COLOR_FORMAT_RGB565,.w=360,.h=360,.stride=720},
        .data_size=sizeof(scene_frame),.data=(const uint8_t *)scene_frame};
    for(unsigned i=0;i<360*360;i++)scene_frame[i]=(uint16_t)(i*73U+(i>>3));
    lv_image_set_src(scene_image,&image);
    lv_obj_set_pos(scene_image,0,0);lv_obj_set_size(scene_image,360,360);
    lv_obj_t *overlay=lv_obj_create(screen);
    lv_obj_set_size(overlay,97,35);lv_obj_set_pos(overlay,121,301);
    lv_obj_set_style_bg_color(overlay,lv_color_white(),0);
    lv_obj_set_style_bg_opa(overlay,LV_OPA_70,0);
    lv_obj_set_style_radius(overlay,12,0);
    static uint16_t reference[360*360];
    for(unsigned phase=0;phase<5;phase++) {
        if(phase==1)lv_obj_set_pos(overlay,1,39);
        if(phase==2)lv_obj_add_flag(overlay,LV_OBJ_FLAG_HIDDEN);
        if(phase==3)lv_obj_set_style_opa(scene_image,LV_OPA_50,0);
        if(phase==4){lv_obj_set_style_opa(scene_image,LV_OPA_COVER,0);lv_image_set_rotation(scene_image,100);}
        lv_display_set_scene_render_cb(lv_display_get_default(),NULL);
        lv_obj_invalidate(screen);lv_refr_now(NULL);memcpy(reference,pixels,sizeof pixels);
        lv_display_set_scene_render_cb(lv_display_get_default(),scene_hook);
        face_scene_fast_strips=0;
        lv_obj_invalidate(screen);lv_refr_now(NULL);
        assert(!memcmp(reference,pixels,sizeof pixels));
        assert(phase<3 ? face_scene_fast_strips>0 : face_scene_fast_strips==0);
    }
    lv_image_set_rotation(scene_image,0);
    lv_obj_invalidate(screen);lv_refr_now(NULL);
    const lv_area_t odd={1,39,97,60};
    for(unsigned y=39;y<=60;y++)for(unsigned x=1;x<=97;x++)scene_frame[y*360+x]^=0x5a5a;
    lv_obj_invalidate_area(scene_image,&odd);lv_refr_now(NULL);
    memcpy(reference,pixels,sizeof pixels);
    lv_display_set_scene_render_cb(lv_display_get_default(),NULL);
    lv_obj_invalidate(screen);lv_refr_now(NULL);
    assert(!memcmp(reference,pixels,sizeof pixels));
    puts("PASS: face RGB565 scene equals LVGL, overlays/clipping/opacity/rotation fallback");
}

static void check_direct_provisioning_qr(void) {
    static uint16_t reference[360*360], before[360*360];
    nuwax_device_state_t state=s_state;
    state.provisioning_active=true;state.provisioning_starting=false;
    nuwax_drawer_update(&state);
    nuwax_drawer_close();settle(1500);
    nuwax_drawer_open_menu();settle(1500);show_qr();settle(400);
    lv_refr_now(NULL);memcpy(reference,pixels,sizeof pixels);
    for(unsigned scenario=0;scenario<7;scenario++) {
        nuwax_drawer_close();settle(1500);assert(!s_open);
        if(scenario==1) {nuwax_drawer_open();settle(100);assert(s_reveal_timer);}
        if(scenario==2) {nuwax_drawer_open_menu();settle(1500);nuwax_drawer_close();settle(100);assert(s_closing);}
        if(scenario==3) {nuwax_drawer_open();settle(1500);nuwax_drawer_page_drag_to(-80);assert(s_page_dragging);}
        if(scenario==4 || scenario==5) {
            nuwax_drawer_open_menu();settle(1500);nuwax_drawer_tap(250,210);
            if(scenario==4) settle(110);else assert(s_click_timer);
        }
        if(scenario==6) {nuwax_drawer_open_menu();settle(1500);show_qr();}
        const bool already_shown=s_open && s_detail==CC_DETAIL_QR && !s_sheet_active;
        memcpy(before,pixels,sizeof pixels);
        const unsigned allocations_before = cache_allocations;
        assert(nuwax_drawer_show_provisioning_qr());
        assert(cache_allocations == allocations_before);
        if(already_shown) {
            assert(s_reveal_raw==1000 && !s_qr_reveal_active && !s_reveal_timer);
            lv_refr_now(NULL);assert(!memcmp(reference,pixels,sizeof pixels));
            continue;
        }
        /* Only the QR reveal owns motion; no menu/detail callback survives. */
        assert(!memcmp(before,pixels,sizeof pixels));
        assert(s_open && s_detail==CC_DETAIL_QR && s_reveal_raw==0 && !s_closing);
        assert(s_qr_reveal_active && s_reveal_timer && !s_sheet_active && !s_click_timer && !s_page_transition_active);
        assert(!s_dragging && !s_page_dragging);
        assert(no_cache || (s_transition_cache_active && s_navigation_cached));
        assert(s_present_pending==CC_PRESENT_NONE);
        assert(lv_obj_get_x(nuwax_device_qr_view_root())==0);
        lv_refr_now(NULL);assert(!memcmp(frozen,pixels,sizeof pixels));
        if(scenario==0) {
            capture("qr-circle-00");
            if(!no_cache) {
                obj_draws=0;fast_strips=0;
                lv_obj_invalidate(lv_screen_active());lv_refr_now(NULL);
                const unsigned cached_draws=obj_draws;
                assert(fast_strips>0);
                const bool active=s_transition_cache_active;
                s_navigation_cached=false;s_transition_cache_active=false;
                lv_obj_add_flag(s_cache_plane,LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(nuwax_device_qr_view_root(),LV_OBJ_FLAG_HIDDEN);
                set_page_tree_mode(s_page,false);
                obj_draws=0;
                lv_obj_invalidate(lv_screen_active());lv_refr_now(NULL);
                const unsigned live_draws=obj_draws;
                assert(cached_draws<live_draws);
                fprintf(stderr,"QR OPTIMIZATION full-frame object draws %u -> %u; no image-cache allocation\n",live_draws,cached_draws);
                s_navigation_cached=true;s_transition_cache_active=active;
                lv_obj_add_flag(nuwax_device_qr_view_root(),LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(s_cache_plane,LV_OBJ_FLAG_HIDDEN);
                set_page_tree_mode(s_page,false);
            }
        }
        for(unsigned frame=1;frame<=28;frame++) {
            settle(25);
            for(int y=0;y<360;y++)for(int x=0;x<360;x++) {
                const int left=s_reveal_mask_rows[y].left_q4;
                const int right=s_reveal_mask_rows[y].right_q4;
                if(s_backdrop_progress==1000 || (x*16>=left && (x+1)*16<=right))
                    assert(pixels[y*360+x]==reference[y*360+x]);
                else if((x+1)*16<left || x*16>right || right<=left)
                    assert(pixels[y*360+x]==frozen[y*360+x]);
            }
            if(scenario==0) {char name[40];snprintf(name,sizeof(name),"qr-circle-%02u",frame);capture(name);}
        }
        assert(!s_qr_reveal_active && !s_reveal_timer);
        settle(800);assert(!memcmp(reference,pixels,sizeof pixels));
        assert(nuwax_drawer_show_provisioning_qr()); /* Idempotent. */
    }
    capture("qr-direct");
    nuwax_drawer_action_result_t cancel=nuwax_drawer_tap(180,335);
    assert(cancel.action==NUWAX_DRAWER_ACTION_REPROVISION_WIFI && cancel.value==0);
    nuwax_drawer_reject_action(NUWAX_DRAWER_ACTION_REPROVISION_WIFI);
    nuwax_drawer_back();settle(400);assert(!nuwax_drawer_is_detail() && s_page==CC_PAGE_MENU);
    assert(nuwax_drawer_show_provisioning_qr());
    settle(800);
    const unsigned close_allocations=cache_allocations;
    nuwax_drawer_hide_provisioning_qr();
    assert(cache_allocations==close_allocations);
    assert(s_closing && s_qr_reveal_active && s_detail==CC_DETAIL_QR);
    for(unsigned frame=1;frame<=14;frame++) {
        settle(25);
        if(s_qr_reveal_active) {
            for(int y=0;y<360;y++)for(int x=0;x<360;x++) {
                const int left=s_reveal_mask_rows[y].left_q4,right=s_reveal_mask_rows[y].right_q4;
                if(x*16>=left && (x+1)*16<=right)
                    assert(pixels[y*360+x]==reference[y*360+x]);
                else if((x+1)*16<left || x*16>right || right<=left)
                    assert(pixels[y*360+x]==frozen[y*360+x]);
            }
            char name[40];snprintf(name,sizeof(name),"qr-close-%02u",frame);capture(name);
        }
    }
    assert(!s_open && !s_qr_reveal_active && !s_navigation_cached && !s_reveal_timer);
    assert(!memcmp(frozen,pixels,sizeof pixels));
    const char *saved_payload=qr_payload;qr_payload="";
    memcpy(before,pixels,sizeof pixels);
    assert(!nuwax_drawer_show_provisioning_qr());assert(!s_open && !nuwax_device_qr_view_is_open());
    lv_refr_now(NULL);assert(!memcmp(before,pixels,sizeof pixels));
    qr_payload=saved_payload;assert(nuwax_drawer_show_provisioning_qr());
    settle(150);assert(s_qr_reveal_active);
    nuwax_drawer_back();assert(s_qr_reveal_active && !s_sheet_active);
    nuwax_drawer_set_power_blocked(true);
    assert(!s_qr_reveal_active && !s_reveal_timer && !s_open);
    assert(!nuwax_drawer_show_provisioning_qr());
    nuwax_drawer_set_power_blocked(false);
    face_freeze_missing=true;assert(nuwax_drawer_show_provisioning_qr());
    assert(!s_qr_reveal_active && !s_reveal_timer && s_reveal_raw==1000);
    lv_refr_now(NULL);assert(!memcmp(reference,pixels,sizeof pixels));
    face_freeze_missing=false;nuwax_drawer_hide_provisioning_qr();settle(1500);
    state.provisioning_active=false;nuwax_drawer_update(&state);
    assert(!nuwax_drawer_show_provisioning_qr() && !s_open);
    state.provisioning_active=true;nuwax_drawer_update(&state);settle(1500);
    assert(nuwax_drawer_show_provisioning_qr());settle(150);
    const int32_t interrupted_radius=s_reveal_raw;
    nuwax_drawer_hide_provisioning_qr();
    assert(s_closing && s_qr_reveal_active && s_reveal_raw==interrupted_radius);
    settle(500);assert(!s_open && !s_navigation_cached && !s_reveal_timer);
    assert(nuwax_drawer_show_provisioning_qr());settle(800);
    nuwax_drawer_back();settle(25);
    if(s_detail==CC_DETAIL_QR) nuwax_drawer_hide_provisioning_qr();
    else {assert(no_cache && s_detail==CC_DETAIL_NONE);nuwax_drawer_close();}
    settle(1500);
    assert(!s_open && !s_navigation_cached && !s_reveal_timer && !s_sheet_active);
    assert(nuwax_drawer_show_provisioning_qr());settle(800);
    nuwax_drawer_hide_provisioning_qr();settle(100);
    nuwax_drawer_set_ota_blocked(true);
    assert(!s_open && !s_navigation_cached && !s_reveal_timer && !s_qr_reveal_active);
    settle(1000);assert(!s_open);nuwax_drawer_set_ota_blocked(false);
    if(!no_cache) {
        state.provisioning_active=true;nuwax_drawer_update(&state);settle(1500);
        assert(s_nav_cache[0].ready);
        s_nav_cache[0].key[0]='!';
        assert(nuwax_drawer_show_provisioning_qr());
        assert(s_qr_reveal_active && !s_navigation_cached);
        settle(800);assert(!memcmp(reference,pixels,sizeof pixels));
        nuwax_drawer_hide_provisioning_qr();settle(500);assert(!s_open);
        state.provisioning_active=false;nuwax_drawer_update(&state);
    }
    puts("PASS: QR cached entry and close use no image-cache allocations; cached scene reduces object draws; stale cache falls back to live QR; close masks contain no menu pixels");
    puts("PASS: QR circular reveal starts at face, all 28 frames match QR inside and face outside, final QR exact; no menu frame; reveal/close/drag/detail/click retirement; cancel/back/reentry/readiness");
}

static void check_input_motion_policy(void) {
    nuwax_drawer_open_menu();settle(800);
    nuwax_drawer_input_state_t input;
    nuwax_drawer_get_input_state(&input);
    assert(input.open && input.tap_allowed && input.volume_available && input.page_right);
    const uint32_t menu_scene=input.scene_generation;
    const nuwax_device_state_t before_volume=s_state;
    nuwax_device_state_t volume_update=before_volume;
    volume_update.volume=before_volume.volume==83 ? 37 : 83;
    assert(!state_affects_menu(&before_volume,&volume_update));
    nuwax_drawer_update(&volume_update);
    assert(s_volume_visual==volume_update.volume && !s_menu_dirty);
    nuwax_drawer_update(&before_volume);
    uint8_t volume;
    assert(nuwax_drawer_volume_value_at(290,310,true,&volume) && volume==100);
    assert(!nuwax_drawer_volume_value_at(290,310,false,&volume));
    /* A release and re-grab retain the visible pose, including while settling. */
    nuwax_drawer_drag_to(-116);
    const int first_y=nuwax_drawer_current_y();
    assert(abs(first_y+116)<=1);
    nuwax_drawer_finish_drag(false);
    assert(nuwax_drawer_current_y()==first_y);
    settle(25);
    const int visible_y=nuwax_drawer_current_y();
    const int visible_raw=s_reveal_raw;
    nuwax_drawer_drag_to(visible_y);
    assert(nuwax_drawer_current_y()==visible_y && s_reveal_raw==visible_raw);
    nuwax_drawer_get_input_state(&input);
    assert(!input.tap_allowed && !input.volume_available && input.scene_generation!=menu_scene);
    nuwax_drawer_finish_drag(false);settle(500);
    nuwax_drawer_get_input_state(&input);
    assert(input.tap_allowed && input.volume_available);
    const uint32_t before_page=input.scene_generation;
    nuwax_drawer_page_drag_to(92);
    assert(s_page_dragging && s_page_progress==1000-92000/CC_PAGE_DRAG_RANGE_PX);
    nuwax_drawer_get_input_state(&input);
    assert(!input.tap_allowed && !input.volume_available && input.scene_generation!=before_page);
    nuwax_drawer_page_finish_drag(true);settle(25);
    const int visible_progress=s_page_progress;
    nuwax_drawer_page_drag_to(17);
    assert(s_page_dragging && s_page_progress==visible_progress-17000/CC_PAGE_DRAG_RANGE_PX);
    apply_page_progress(920,true);assert(s_page_progress==920);
    apply_page_progress(960,true);assert(s_page_progress==960);
    nuwax_drawer_page_finish_drag(false);settle(500);
    nuwax_drawer_get_input_state(&input);
    assert(input.tap_allowed && input.scene_generation!=before_page);
    nuwax_drawer_close();settle(500);
    nuwax_drawer_get_input_state(&input);
    assert(!input.open && !input.tap_allowed);
    if (!no_cache) {
        nuwax_drawer_open();settle(800);
        const size_t bytes=s_transition_cache_bank_size;
        uint8_t *committed=malloc(bytes);assert(committed);
        transition_cache_mark_page_dirty(CC_PAGE_STATUS);
        assert(!transition_cache_capture_page(CC_PAGE_STATUS));
        assert(s_transition_cache_build_active);
        memcpy(committed,s_transition_cache_arena,bytes);
        simulated_heap_free=1059420U;
        const unsigned allocations=cache_allocations;
        nuwax_drawer_page_drag_to(-40);
        assert(s_transition_cache_active && pager_prepare_sprites());
        assert(s_pager_staging_lease && !s_transition_cache_build_active);
        for(unsigned i=0;i<20;i++) assert(pager_prepare_sprites());
        assert(cache_allocations==allocations);
        assert(!transition_cache_capture_page(CC_PAGE_STATUS));
        assert(!memcmp(committed,s_transition_cache_arena,bytes));
        nuwax_drawer_page_finish_drag(false);settle(500);
        assert(!s_pager_staging_lease && !s_transition_cache_active);
        simulated_heap_free=4U*1024U*1024U;
        settle(800);assert(!s_status_cache_dirty);
        free(committed);
        nuwax_drawer_close();settle(500);
    }
    /* 100 ms callback gaps must not stretch a 350 ms input lock to seconds. */
    nuwax_drawer_open();
    const unsigned began=ticks;
    for(unsigned i=0;i<5 && s_reveal_timer;i++) {
        ticks+=100;lv_timer_handler();lv_refr_now(NULL);
    }
    assert(!s_reveal_timer && s_reveal_raw==1000 && ticks-began<=CC_REVEAL_FULL_MS+100);
    nuwax_drawer_close();settle(500);
    puts("PASS: visible-pose re-grab, continuous one-to-one page motion, bounded reveal deadline, low-memory pager staging lease and committed-tile integrity");
}

static void check_ota_takeover(void) {
    static uint16_t reference[3][360*360];
    const unsigned progress[]={0,51,100};
    nuwax_device_state_t ota={.ota_update_active=true};
    assert(nuwax_ota_progress_create(lv_screen_active())==ESP_OK);
    for(unsigned i=0;i<3;i++) {
        ota.ota_update_percent=progress[i];nuwax_ota_progress_update(&ota);
        settle(250);memcpy(reference[i],pixels,sizeof pixels);
    }
    for(unsigned scenario=0;scenario<8;scenario++) {
        ota.ota_update_active=false;nuwax_ota_progress_update(&ota);settle(30);
        assert(!ota_face_covered && !s_ota_blocked && !s_open);
        if(scenario==1) {nuwax_drawer_open();settle(150);assert(s_reveal_timer);}
        if(scenario==2) {
            nuwax_drawer_open();settle(1400);nuwax_drawer_close();settle(CC_REVEAL_FULL_MS/2);
            assert(s_closing && s_reveal_timer);
        }
        if(scenario==3) {nuwax_drawer_begin_pull();nuwax_drawer_drag_to(-140);assert(s_dragging);}
        if(scenario==4) {
            nuwax_drawer_open();settle(1400);nuwax_drawer_next_page();settle(60);
            assert(s_page_transition_active);
        }
        if(scenario==5 || scenario==6) {
            nuwax_drawer_open_menu();settle(1400);
            nuwax_drawer_tap(250,210);assert(s_click_timer);
            if(scenario==5) {settle(110);assert(s_detail==CC_DETAIL_INFO);}
        }
        if(scenario==7) {
            nuwax_device_state_t pairing=s_state;pairing.provisioning_active=true;
            nuwax_drawer_update(&pairing);assert(nuwax_drawer_show_provisioning_qr());
            settle(150);assert(s_qr_reveal_active);
        }
        ota.ota_update_active=true;ota.ota_update_percent=0;
        nuwax_ota_progress_update(&ota);
        assert(ota_face_covered && s_ota_blocked && !s_open && !s_closing && !s_dragging);
        assert(!s_reveal_timer && !s_click_timer && !s_sheet_active && !s_page_transition_active);
        assert(!s_transition_cache_active && !s_navigation_cached && !s_backdrop_frozen_face);
        assert(s_present_pending==CC_PRESENT_NONE && !s_contact_down);
        /* Exercise entry points even when callers fail to quarantine input. */
        nuwax_drawer_open();nuwax_drawer_open_menu();nuwax_drawer_begin_pull();
        assert(!nuwax_drawer_show_provisioning_qr());
        nuwax_drawer_drag_to(0);nuwax_drawer_finish_drag(false);
        assert(!s_open && !s_reveal_timer);
        for(unsigned i=0;i<3;i++) {
            ota.ota_update_percent=progress[i];nuwax_ota_progress_update(&ota);
            settle(250);
            /* No forced full redraw: detect persistent damage to static artwork. */
            assert(!memcmp(reference[i],pixels,sizeof pixels));
        }
        settle(1500);assert(!memcmp(reference[2],pixels,sizeof pixels));
    }
    capture("ota-takeover");
    ota.ota_update_active=false;nuwax_ota_progress_update(&ota);settle(100);
    assert(!s_ota_blocked && !ota_face_covered);
    nuwax_drawer_open();settle(1400);assert(s_open && s_reveal_raw==1000);
    ota.ota_update_active=true;nuwax_ota_progress_update(&ota);
    nuwax_ota_progress_delete();assert(!s_ota_blocked && !ota_face_covered);
    /* Low-battery takeover shares the same compositor barrier but owns a
     * separate bit: clearing either modal must not release the other. */
    nuwax_drawer_open();settle(150);assert(s_reveal_timer);
    nuwax_drawer_set_power_blocked(true);
    assert(s_ota_blocked && !s_open && !s_reveal_timer && !s_page_transition_active);
    nuwax_drawer_set_ota_blocked(true);
    nuwax_drawer_set_power_blocked(false);
    assert(s_ota_blocked);
    nuwax_drawer_set_power_blocked(true);
    nuwax_drawer_set_ota_blocked(false);
    nuwax_drawer_open();assert(s_ota_blocked && !s_open);
    nuwax_drawer_set_power_blocked(false);
    nuwax_drawer_open();settle(1400);assert(s_open);
    nuwax_drawer_close();settle(1400);
    puts("PASS: low-battery takeover retires menu animation and preserves independent OTA/power modal owners");
    puts("PASS: OTA native pixels at 0/51/100%, 7 menu motion states, blocked reentry, delayed callbacks, exit/retry/delete");
}

int main(int argc,char **argv) {
    (void)argv;no_cache=argc>1;
    lv_init();lv_tick_set_cb(tick);lv_display_t *d=lv_display_create(360,360);
    lv_display_set_color_format(d,LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d,draw_buffer,NULL,sizeof(draw_buffer),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d,flush);lv_obj_set_style_bg_color(lv_screen_active(),lv_color_black(),0);
    assert(nuwax_drawer_create(lv_screen_active())==ESP_OK);probe_tree(lv_screen_active());lv_display_set_scene_render_cb(d,scene_probe);lv_display_add_event_cb(d,probe_endpoint,LV_EVENT_RENDER_START,NULL);
    nuwax_device_state_t state={0};state.wifi=NUWAX_WIFI_CONNECTED;state.cloud=NUWAX_CLOUD_CONNECTED;
    state.wifi_signal_valid=true;state.wifi_rssi_dbm=-42;state.wifi_signal=NUWAX_WIFI_SIGNAL_STRONG;
    strlcpy(state.ssid,"LingZhu-Home",sizeof(state.ssid));strlcpy(state.ip,"192.168.31.86",sizeof(state.ip));
    state.power=NUWAX_POWER_BATTERY;state.battery_valid=true;state.battery_percent=82;state.volume=60;state.brightness_percent=100;state.dialog=NUWAX_DIALOG_LISTENING;
    nuwax_drawer_update(&state);settle(5000);nuwax_drawer_open();
    assert(no_cache || s_transition_cache_active);
    assert(nuwax_drawer_tap(180,310).action==NUWAX_DRAWER_ACTION_NONE);
    settle(2500);assert(s_reveal_raw==1000);capture("status");check_label(s_root);
    nuwax_drawer_tap(200,310);capture("page-start");
    capture_motion("page",12);settle(300);assert(nuwax_drawer_is_menu_page());capture("menu");
    nuwax_drawer_page_drag_to(36);capture("drag-mid");
    nuwax_drawer_page_finish_drag(false);capture_motion("drag-cancel",10);assert(s_page==CC_PAGE_MENU);
    nuwax_drawer_page_drag_to(65);nuwax_drawer_page_finish_drag(true);settle(300);assert(s_page==CC_PAGE_STATUS);
    nuwax_drawer_next_page();settle(300);assert(s_page==CC_PAGE_MENU);
    assert(nuwax_drawer_tap(100,110).action==NUWAX_DRAWER_ACTION_SET_MEETING_MODE);
    capture("meeting-pending");check_label(s_root);
    nuwax_drawer_reject_action(NUWAX_DRAWER_ACTION_SET_MEETING_MODE);assert(!s_meeting_pending);
    state.meeting=NUWAX_MEETING_UI_ACTIVE;nuwax_drawer_update(&state);settle(100);capture("meeting-active");
    assert(!strcmp(lv_label_get_text(s_meeting_state),"运行中"));
    /* Every part cycles: sun, title, and bars visited right-to-left. */
    for (unsigned i=0;i<3;++i) {
        const uint8_t expected[]={40,70,100};
        nuwax_drawer_action_result_t action=nuwax_drawer_tap(250,i%2 ? 127 : 100);
        assert(action.action==NUWAX_DRAWER_ACTION_CYCLE_BRIGHTNESS && action.value==0);
        state.brightness_percent=expected[i];nuwax_drawer_update(&state);settle(400);
        assert(s_brightness_visual==(int)i);
    }
    /* Hitbox edges belong to this card; the gap beside it does not. */
    const unsigned edge_x[]={184,317,184,317};
    const unsigned edge_y[]={77,77,162,162};
    for(unsigned i=0;i<4;++i) {
        nuwax_drawer_action_result_t a=nuwax_drawer_tap(edge_x[i],edge_y[i]);
        assert(a.action==NUWAX_DRAWER_ACTION_CYCLE_BRIGHTNESS && a.value==0);
    }
    assert(nuwax_drawer_tap(320,145).action==NUWAX_DRAWER_ACTION_NONE);
    for (unsigned i=0;i<3;++i) {
        const uint8_t levels[]={40,70,100};
        nuwax_drawer_action_result_t level=nuwax_drawer_tap(290-45*i,148);
        assert(level.action==NUWAX_DRAWER_ACTION_CYCLE_BRIGHTNESS && level.value==0);
        state.brightness_percent=levels[i];nuwax_drawer_update(&state);settle(400);
        assert(s_brightness_visual==(int)i);
        char name[32];snprintf(name,sizeof(name),"brightness-%u",levels[i]);capture(name);
        check_label(s_root);
    }
    assert(nuwax_drawer_tap(110,210).action==NUWAX_DRAWER_ACTION_REPROVISION_WIFI);
    assert(!nuwax_drawer_is_detail()); /* A QR alone is never an enable request. */
    assert(nuwax_drawer_tap(110,210).action==NUWAX_DRAWER_ACTION_NONE);
    nuwax_drawer_reject_action(NUWAX_DRAWER_ACTION_REPROVISION_WIFI);
    assert(!s_provision_pending);
    uint8_t volume;assert(nuwax_drawer_hit_volume(CC_VOLUME_BAR_X,275,&volume)&&volume==0);
    assert(nuwax_drawer_hit_volume(CC_VOLUME_BAR_X+CC_VOLUME_BAR_W,275,&volume)&&volume==100);
    assert(!nuwax_drawer_hit_volume(180,310,&volume));
    nuwax_drawer_preview_volume(100);check_label(s_root);capture("volume-100");
    nuwax_drawer_preview_volume(0);check_label(s_root);capture("volume-0");
    nuwax_drawer_preview_volume(60);settle(4000);
    if (!no_cache) {
        assert(!s_menu_cache_dirty && transition_cache_page_valid(CC_PAGE_MENU));
        static uint8_t previous_bank[360*360*2];
        assert(s_transition_cache_bank_size <= sizeof previous_bank);
        memcpy(previous_bank,s_transition_cache_arena,s_transition_cache_bank_size);
        uint32_t generation=s_menu_cache_generation;
        nuwax_drawer_preview_volume(60);
        assert(s_menu_cache_generation==generation && !s_menu_cache_dirty);
        /* Rapid drag: live label changes, published cache stays coherent. */
        for(unsigned v=10;v<=80;v+=10) {
            nuwax_drawer_preview_volume(v);
            settle(25);
            assert(s_menu_cache_dirty);
            assert(!transition_cache_prepare_page(CC_PAGE_MENU));
            assert(memcmp(previous_bank,s_transition_cache_arena,s_transition_cache_bank_size)==0);
        }
        settle(250);
        assert(!s_menu_cache_dirty && s_transition_cache_build_components==1);
        assert(s_transition_cache_build_mask==(1U<<11));
        for(unsigned i=0;i<CC_TRANSITION_CACHE_COMPONENTS;i++) {
            cc_transition_cache_component_t *c=&s_transition_cache[i];
            if(c->source==s_menu_volume)continue;
            size_t offset=(size_t)(c->storage-s_transition_cache_arena);
            assert(memcmp(c->storage,previous_bank+offset,c->storage_size)==0);
        }
        assert(!strcmp(lv_label_get_text(s_volume_text),"80%"));
        /* Update a second tile while a page is staged: nothing is published
         * until the new generation has all its changed components ready. */
        transition_cache_mark_page_dirty(CC_PAGE_MENU);
        memcpy(previous_bank,s_transition_cache_arena,s_transition_cache_bank_size);
        assert(!transition_cache_capture_page(CC_PAGE_MENU));
        set_volume_visual(45);
        assert(!transition_cache_capture_page(CC_PAGE_MENU));
        assert(memcmp(previous_bank,s_transition_cache_arena,s_transition_cache_bank_size)==0);
        while(!transition_cache_capture_page(CC_PAGE_MENU)) {}
        assert(!s_menu_cache_dirty && s_transition_cache_build_components==6);
        nuwax_drawer_preview_volume(60);settle(250);
        puts("PASS: unchanged volume no-op, rapid preview coalescing, one-tile commit, generation restart and unchanged-tile integrity");
    }
    assert(no_cache || (s_nav_cache[0].ready && s_nav_cache[1].ready));
    nuwax_drawer_contact(true,110,210);capture("qr-pressed");assert(s_pressed==s_menu_cards[2]);
    nuwax_drawer_contact(true,145,210);assert(!s_pressed);capture("press-cancel");
    nuwax_drawer_contact(false,0,0);
    nuwax_drawer_action_result_t pairing=nuwax_drawer_tap(110,210);
    assert(pairing.action==NUWAX_DRAWER_ACTION_REPROVISION_WIFI && pairing.value==1);
    assert(!nuwax_drawer_is_detail());
    /* Pairing changes the meeting lock as well. Complete its staged cache
     * generation before comparing transition pixels to the current live tree.
     * The stale-generation admission contract is tested separately below. */
    state.provisioning_active=true;nuwax_drawer_update(&state);settle(1500);
    assert(!s_provision_pending);
    nuwax_drawer_tap(110,210);assert(s_click_timer);capture("qr-click");
    nuwax_drawer_tap(250,210);assert(s_click_target==2); /* No double navigation. */
    capture_motion("qr-enter",13);assert(nuwax_drawer_is_detail() && !s_sheet_active);capture("qr");
    nuwax_drawer_tap(60,60);assert(nuwax_drawer_is_detail());
    nuwax_drawer_action_result_t cancel=nuwax_drawer_tap(180,335);
    assert(cancel.action==NUWAX_DRAWER_ACTION_REPROVISION_WIFI && cancel.value==0);
    assert(nuwax_drawer_tap(180,335).action==NUWAX_DRAWER_ACTION_NONE);
    nuwax_drawer_reject_action(NUWAX_DRAWER_ACTION_REPROVISION_WIFI);
    assert(!s_provision_pending && nuwax_drawer_is_detail());
    nuwax_drawer_back();capture_motion("detail-back",13);assert(!nuwax_drawer_is_detail());
    qr_payload="";nuwax_drawer_tap(110,210);settle(400);capture("qr-unavailable");
    assert(nuwax_drawer_is_detail());nuwax_drawer_back();settle(400);state.provisioning_active=false;nuwax_drawer_update(&state);settle(1500);qr_payload="NUWAX7:NX-A1B2-C3D4-E5F6";
    nuwax_drawer_tap(250,210);capture_motion("info-enter",13);assert(nuwax_drawer_is_detail());capture("info");
    assert(!strcmp(lv_label_get_text(s_info_id), "NX-A1B2-C3D4-E5F6"));
    strlcpy(state.ssid,"WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW",sizeof(state.ssid));
    nuwax_drawer_update(&state);settle(50);capture("info-long");
    assert(!strcmp(lv_label_get_text(s_info_ssid),state.ssid));
    assert(lv_obj_get_height(s_info_ssid)<=40);
    strlcpy(state.ssid,"灵珠家庭网络",sizeof(state.ssid));nuwax_drawer_update(&state);settle(50);capture("info-chinese");
    nuwax_drawer_tap(180,335);settle(400);assert(!nuwax_drawer_is_detail());
#if NUWAX_TEST_PANEL_ENABLED
    nuwax_drawer_tap(250,310);capture_motion("test-enter",10);settle(100);assert(s_page==CC_PAGE_TEST);capture("test");
    assert(nuwax_drawer_tap(220,125).action==NUWAX_DRAWER_ACTION_SET_SIMULATED_OFFLINE);
#endif
    nuwax_drawer_tap(110,310);capture_motion("page-return",12);settle(300);assert(s_page==CC_PAGE_STATUS);
    state.wifi=NUWAX_WIFI_DISCONNECTED;state.cloud=NUWAX_CLOUD_AUTHFAIL;state.battery_percent=10;
    nuwax_drawer_update(&state);settle(100);capture("offline");check_label(s_root);
    for(int wifi=0;wifi<=NUWAX_WIFI_DISCONNECTED;wifi++)for(int cloud=0;cloud<=NUWAX_CLOUD_AUTHFAIL;cloud++) {
        state.wifi=wifi;state.cloud=cloud;nuwax_drawer_update(&state);check_label(s_root);
    }
    state.meeting=NUWAX_MEETING_UI_DISABLED;
    for(int dialog=0;dialog<=NUWAX_DIALOG_READY;dialog++)for(int power=0;power<=NUWAX_POWER_LOW;power++) {
        state.dialog=dialog;state.power=power;state.battery_percent=100;
        nuwax_drawer_update(&state);check_label(s_root);
    }
    nuwax_drawer_tap(180,340);settle(2500);assert(!nuwax_drawer_is_open());
    capture("closed");
    nuwax_drawer_open_menu();settle(1600);
    nuwax_drawer_tap(110,210);nuwax_drawer_close();settle(1600);
    assert(!s_click_timer && !s_sheet_active && !nuwax_drawer_is_detail());
    nuwax_drawer_open_menu();settle(6500);
    assert(no_cache || (s_nav_cache[1].ready && !strcmp(lv_label_get_text(s_info_ssid),state.ssid)));
    nuwax_drawer_tap(250,210);settle(130);assert(no_cache || s_sheet_active);
    strlcpy(state.ip,"192.168.31.87",sizeof(state.ip));nuwax_drawer_update(&state);
    settle(200);capture("info-updated-during-motion");
    assert(!s_sheet_active && s_detail==CC_DETAIL_INFO);
    nuwax_drawer_back();settle(400);settle(4000);
    nuwax_drawer_tap(250,210);settle(130);assert(no_cache || s_sheet_active);
    nuwax_drawer_back();capture_motion("reverse-detail",10);assert(!nuwax_drawer_is_detail());
    nuwax_drawer_tap(110,210);settle(130);nuwax_drawer_close();settle(1600);
    assert(!s_click_timer && !s_sheet_active && !nuwax_drawer_is_detail());
    if (!no_cache) {
        /* A partially built generation cannot corrupt the last committed page.
         * Replace the source, interrupt its build, and verify it restarts before
         * atomically committing all six components. */
        settle(2000);
        while (!transition_cache_prepare_page(CC_PAGE_STATUS)) {}
        static uint8_t committed[300000];
        assert(s_transition_cache_bank_size <= sizeof committed);
        memcpy(committed, s_transition_cache_arena, s_transition_cache_bank_size);
        lv_label_set_text(s_status_cards[0].value, "WiFi test");
        transition_cache_mark_page_dirty(CC_PAGE_STATUS);
        assert(!transition_cache_capture_page(CC_PAGE_STATUS));
        assert(transition_cache_page_valid(CC_PAGE_STATUS));
        assert(memcmp(committed, s_transition_cache_arena, s_transition_cache_bank_size)==0);
        /* No idle time to finish the pending bank: an old committed generation
         * must still carry motion, with dirty state retained for its endpoint. */
        const cc_page_t saved_page=s_page;
        s_page=CC_PAGE_STATUS;
        ticks+=500;
        assert(transition_cache_activate(false));
        assert(s_status_cache_dirty);
        assert(memcmp(committed,s_transition_cache_arena,s_transition_cache_bank_size)==0);
        transition_cache_deactivate(); s_page=saved_page; set_page_tree_mode(s_page,false);
        transition_cache_mark_page_dirty(CC_PAGE_STATUS);
        assert(!transition_cache_capture_page(CC_PAGE_STATUS));
        assert(s_transition_cache_build_components==1);
        assert(memcmp(committed, s_transition_cache_arena, s_transition_cache_bank_size)==0);
        while (!transition_cache_capture_page(CC_PAGE_STATUS)) {}
        assert(!s_status_cache_dirty && transition_cache_page_valid(CC_PAGE_STATUS));
        assert(memcmp(committed, s_transition_cache_arena, s_transition_cache_bank_size)!=0);
        puts("PASS: staged page remains readable; generation change restarts build; six tiles commit atomically");
    }
    check_pull_motion();
    check_drag_endpoint();
    check_direct_provisioning_qr();
    check_input_motion_policy();
    check_ota_takeover();
    nuwax_drawer_deinit();assert(filter==NULL);assert(lv_obj_get_child_count(lv_screen_active())==0);
    lv_obj_t *scene=lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(scene);lv_obj_set_size(scene,360,360);
    lv_obj_set_style_bg_color(scene,lv_color_hex(0x182838),0);
    lv_obj_set_style_bg_opa(scene,LV_OPA_COVER,0);
    for(int i=0;i<5;i++) {
        lv_obj_t *box=lv_obj_create(scene);lv_obj_set_pos(box,30+i*60,32+i*51);
        lv_obj_set_size(box,47,33);lv_obj_set_style_bg_color(box,lv_color_hex(0x58d8a0),0);
        lv_obj_set_style_shadow_width(box,16,0);lv_obj_set_style_shadow_color(box,lv_color_hex(0xf03080),0);
        lv_obj_set_style_shadow_opa(box,180,0);lv_obj_set_style_outline_width(box,3,0);
        if(i==1)lv_obj_set_style_opa(box,128,0);
        if(i==2)lv_obj_set_style_opa_layered(box,128,0);
        if(i==3)lv_obj_set_style_transform_rotation(box,300,0);
        if(i==4) {
            lv_obj_add_flag(box,LV_OBJ_FLAG_OVERFLOW_VISIBLE);
            lv_obj_t *child=lv_obj_create(box);lv_obj_set_size(child,35,20);lv_obj_set_pos(child,-12,-10);
        }
    }
    settle(100);capture("clip-special-cases");
    lv_obj_delete(scene);lv_refr_now(NULL);
    assert(no_cache || fast_frames >= 20);
    printf("PASS: cached scene matches ordinary LVGL rendering frames=%u\n",fast_frames);
    assert(no_cache || cached_reference_frames >= 20);
    assert(no_cache || presented_endpoints >= 6);
    assert(s_present_pending==CC_PRESENT_NONE);
    printf("PASS: cached final frames before live handoff endpoints=%u; empty layout draw bypass; redraw equivalence\n",presented_endpoints);
    puts("PASS: production drawer rendering, tabs, close, modes, rollback, detail routing and volume endpoints");
    assert(no_cache || pager_reference_frames>=20);
    printf("PASS: prepared pager equals general LVGL snapshot frames=%u\n",pager_reference_frames);
    check_face_scene();
}
