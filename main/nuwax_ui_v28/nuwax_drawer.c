#include "nuwax_drawer.h"
#include "test/nuwax_test_panel.h"

#include <stdio.h>
#include <string.h>

#include "assets/nuwax_control_center_icons.h"
#include "control_center_timing_policy.h"
#include "control_center_input_policy.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "menu/nuwax_device_qr_view.h"
#include "menu/nuwax_detail_style.h"
#include "nuwax_device_uid.h"
#include "nuwax_face.h"
#include "face/emotion_animation_player.h"
#include "nuwax_lvgl_port.h"
#include "nuwax_provision_main.h"
#include "nuwax_theme.h"
#include "nuwax_ui_config.h"
#include "src/core/lv_obj_draw_private.h"
#include "src/misc/lv_area_private.h"
#include "src/core/lv_refr_private.h"
#include "src/display/lv_display_private.h"
#include "src/draw/lv_draw_private.h"


static const char *TAG = "nuwax_control_center";

LV_FONT_DECLARE(nuwax_font_control_center_cjk_7);
LV_FONT_DECLARE(nuwax_font_control_center_cjk_8);
LV_FONT_DECLARE(nuwax_font_control_center_cjk_9);
LV_FONT_DECLARE(nuwax_font_control_center_cjk_10);
LV_FONT_DECLARE(nuwax_font_control_center_cjk_11);
LV_FONT_DECLARE(nuwax_font_control_center_cjk_12);
LV_FONT_DECLARE(nuwax_font_control_center_cjk_13);
LV_FONT_DECLARE(nuwax_font_lingzhu_14);
LV_FONT_DECLARE(nuwax_font_lingzhu_17);
LV_FONT_DECLARE(nuwax_font_lingzhu_22);

#define CC_W NUWAX_SCREEN_W
#define CC_H NUWAX_SCREEN_H
#define CC_PAGE_ANIM_MS 230
#define CC_PAGE_ANIM_STEPS 10
#define CC_OPERATION_MIN_FPS 30
#define CC_PAGE_DRAG_RANGE_PX NUWAX_CC_PAGE_DRAG_RANGE_PX
#define CC_REVEAL_FULL_MS 350
#define CC_REVEAL_MIN_MS 75
#define CC_QR_REVEAL_MS 700U
#define CC_QR_CLOSE_MS 350U
#define CC_ANIM_STEPS 52
#define CC_REVEAL_MAX_DIAMETER 374
/* CSS `circle(52% at 50% 50%)` resolves to a 187.2 px radius on the
 * simulator's 360 px square. Q4 2995 is 187.1875 px and avoids floating-point
 * work in the LVGL owner while keeping the device edge within 1/16 pixel. */
#define CC_REVEAL_RADIUS_Q4 2995
#define CC_REVEAL_BACKDROP_DONE 1000
#define CC_REVEAL_DIRTY_BANDS 4
#define CC_REVEAL_CONTENT_DIRTY_AREAS 6
#define CC_REVEAL_INVALIDATION_BUDGET 32
#define CC_PSRAM_RUNTIME_RESERVE_BYTES (1024U * 1024U)
#define CC_PSRAM_LARGEST_BLOCK_MIN_BYTES (64U * 1024U)
#define CC_TRANSITION_CACHE_ALIGNMENT 16U
#define CC_STATUS_CACHE_COMPONENTS 6U
#define CC_MENU_CACHE_COMPONENTS 6U
#define CC_TRANSITION_CACHE_COMPONENTS \
    (CC_STATUS_CACHE_COMPONENTS + CC_MENU_CACHE_COMPONENTS)
#define CC_LOCAL_ANIM_FRAME_INTERVAL_US 25000LL
#ifndef CONFIG_NUWAX_UI_ANIMATION_TARGET_FPS
#define CONFIG_NUWAX_UI_ANIMATION_TARGET_FPS 40
#endif
#define CC_REVEAL_FRAME_MS (1000U / CONFIG_NUWAX_UI_ANIMATION_TARGET_FPS)
/* Each reveal frame invalidates the circular backdrop annulus, whose area
 * grows with the progress delta. Applying every wall-clock-skipped step at
 * once makes one frame paint a huge annulus (measured 56-83K px, ~9x the
 * 25 ms render budget) and spirals into sustained 10-18 fps. Cap the advance
 * per callback before the deadline. At the deadline publish the endpoint once
 * rather than replaying expired poses and retaining the input lock. */
#define CC_REVEAL_MAX_STEPS_PER_FRAME 2U
/* Row-merged annulus invalidation: per-side rectangle cap and the Q4 merge
 * tolerance (8 px). 12 rects/side + 6 content areas stays inside LVGL's
 * 32-entry invalidation buffer. */
#define CC_REVEAL_ANNULUS_RECTS_PER_SIDE 12U
#define CC_REVEAL_ANNULUS_MERGE_TOLERANCE_Q4 128
_Static_assert(CC_REVEAL_ANNULUS_RECTS_PER_SIDE * 2U +
                       CC_REVEAL_CONTENT_DIRTY_AREAS <
                   CC_REVEAL_INVALIDATION_BUDGET,
               "annulus rects must leave LVGL invalidation headroom");

_Static_assert(CC_PAGE_ANIM_STEPS * 1000U >=
                   CC_PAGE_ANIM_MS * CC_OPERATION_MIN_FPS,
               "control-center page animation must retain at least 30 FPS");
_Static_assert(CC_PAGE_ANIM_STEPS * 1000U >=
                   CC_PAGE_ANIM_MS * CONFIG_NUWAX_UI_ANIMATION_TARGET_FPS,
               "control-center page animation must provide at least 40 FPS");
_Static_assert(1000U % CONFIG_NUWAX_UI_ANIMATION_TARGET_FPS == 0U,
               "control-center reveal frame period must be integral");
_Static_assert(CC_REVEAL_DIRTY_BANDS * 2U +
                       CC_REVEAL_CONTENT_DIRTY_AREAS <
                   CC_REVEAL_INVALIDATION_BUDGET,
               "control-center reveal must leave LVGL invalidation headroom");
#define CC_HIDDEN_Y (-226)
#define CC_OPEN_Y 48
#define CC_MEETING_PENDING_TIMEOUT_MS 6000U
#define CC_PROVISION_PENDING_TIMEOUT_MS 15000U

/* One geometry contract for widgets, animation damage and touch targets. */
#define CC_HEADER_Y 30
#define CC_HEADER_H 40
#define CC_FOOTER_X 87
#define CC_FOOTER_Y 264
#define CC_FOOTER_W 186
#define CC_FOOTER_H 30
#define CC_PAGER_X 81
#define CC_PAGER_Y 300
#define CC_PAGER_W 198
#define CC_PAGER_H 50
#define CC_VOLUME_KNOB_D 23
#define CC_HEADER_X 80
#define CC_HEADER_W 200
#define CC_CARD_LEFT_X 42
#define CC_CARD_RIGHT_X 184
#define CC_CARD_W 134
#define CC_STATUS_CARD_H 86
#define CC_STATUS_ROW0_Y 77
#define CC_STATUS_ROW1_Y 169

#define CC_MENU_ROW0_Y 77
#define CC_MENU_ROW1_Y 169
#define CC_MENU_ACTION_Y CC_MENU_ROW1_Y
#define CC_MENU_ACTION_W 134
#define CC_MENU_ACTION_H 86
#define CC_MENU_VOLUME_X 64
#define CC_MENU_VOLUME_Y 259
#define CC_MENU_VOLUME_W 240
#define CC_MENU_VOLUME_H 32
#define CC_VOLUME_BAR_X 172
#define CC_VOLUME_BAR_Y 272
#define CC_VOLUME_BAR_W 118
#define CC_VOLUME_BAR_H 10

typedef enum {
    CC_PAGE_STATUS = 0,
    CC_PAGE_MENU,
#if NUWAX_TEST_PANEL_ENABLED
    CC_PAGE_TEST,
#endif
} cc_page_t;

typedef enum {
    CC_DETAIL_NONE = 0,
    CC_DETAIL_QR,
    CC_DETAIL_INFO,
} cc_detail_t;

typedef struct {
    lv_obj_t *clip;
    lv_obj_t *root;
    lv_obj_t *icon_back;
    lv_obj_t *image;
    lv_obj_t *value;
    lv_obj_t *offline_mark;
    nuwax_control_center_icon_id_t image_id;
} cc_status_card_t;

typedef struct {
    lv_obj_t *source;
    lv_draw_buf_t draw_buf;
    uint8_t *storage;
    uint32_t storage_size;
    uint32_t staging_offset;
    int16_t ext_draw_size;
    cc_page_t page;
    bool valid;
    bool dirty;
    lv_area_t frame_area;
    lv_opa_t frame_opa;
} cc_transition_cache_component_t;

static lv_obj_t *s_root;
static lv_obj_t *s_reveal_clip;
static lv_obj_t *s_surface;
static lv_obj_t *s_status_page;
static lv_obj_t *s_menu_page;
#if NUWAX_TEST_PANEL_ENABLED
static lv_obj_t *s_test_page;
static lv_obj_t *s_test_dot;
static bool s_test_page_drag;
static int16_t s_test_page_drag_dx;
#endif
static lv_obj_t *s_info_page;
static lv_obj_t *s_info_back;
static lv_obj_t *s_tab_indicator;
static lv_obj_t *s_pressed;
static lv_color_t s_pressed_bg;
static bool s_contact_down;
static uint16_t s_contact_x, s_contact_y;
static lv_timer_t *s_click_timer;
static unsigned s_click_target;
static lv_obj_t *s_sheet;
static bool s_sheet_active, s_sheet_return;
static int32_t s_sheet_from, s_sheet_to;
static int s_sheet_tab_from = -1;
static int64_t s_sheet_started_us, s_sheet_last_us;
static unsigned s_sheet_frames, s_sheet_gap_us;
static void clear_press(void);
static void cancel_click(void);
static void cancel_sheet(void);
static void start_sheet(lv_obj_t *obj, int32_t from, int32_t to, bool returning);
static void show_qr(void);
static void show_info(void);
static void navigation_cache_tick(void);
static void navigation_cache_release(void);
static void navigation_cache_compose(int32_t x, int32_t y, int32_t w, int32_t h,
                                     uint16_t *pixels, size_t stride);
static bool navigation_cache_begin(lv_obj_t *obj);
static void navigation_cache_end(void);
static bool s_navigation_cached;
typedef enum { CC_PRESENT_NONE, CC_PRESENT_PAGE, CC_PRESENT_REVEAL, CC_PRESENT_SHEET } cc_present_t;
static cc_present_t s_present_pending;
static bool s_present_finishing;
static void presented_frame_cb(lv_event_t *event);
static void sheet_completed(lv_anim_t *anim);
static void request_presented_finish(cc_present_t kind) {
    s_present_pending = kind;
    /* Ensure a refresh callback even when the endpoint was already rendered.
     * The animation itself has submitted the moved regions. */
    const lv_area_t pixel = {0, 0, 0, 0};
    lv_obj_invalidate_area(s_surface, &pixel);
}
static lv_obj_t *s_pager;
static lv_obj_t *s_cache_plane;
static lv_obj_t *s_status_dot;
static lv_obj_t *s_menu_dot;
static lv_obj_t *s_tab_labels[3];
static lv_obj_t *s_pager_clip;
static lv_obj_t *s_status_header_clip;
static lv_obj_t *s_menu_header_clip;
static lv_obj_t *s_status_header;
static lv_obj_t *s_menu_header;
static lv_obj_t *s_cloud_beacon;
static lv_obj_t *s_battery_fill;
static lv_obj_t *s_battery_charging_mark;
static lv_obj_t *s_footer;
static lv_obj_t *s_footer_clip;
static lv_obj_t *s_footer_dot;
static lv_obj_t *s_footer_label;
static lv_obj_t *s_menu_cards[4];
static lv_obj_t *s_menu_card_clips[4];
static lv_obj_t *s_meeting_state;
static lv_obj_t *s_meeting_switch;
static lv_obj_t *s_meeting_knob;
static lv_obj_t *s_provision_label;
static lv_obj_t *s_brightness_icon;
static lv_obj_t *s_brightness_bars[3];
static int s_brightness_visual = -1;
static lv_obj_t *s_volume_text;
static lv_obj_t *s_menu_volume;
static lv_obj_t *s_volume_bar;
static lv_obj_t *s_volume_fill;
static lv_obj_t *s_volume_knob;

static lv_obj_t *s_info_id;
static lv_obj_t *s_info_version;
static lv_obj_t *s_info_version_title;
static lv_obj_t *s_info_ssid;
static lv_obj_t *s_info_ip;
static lv_timer_t *s_pending_timer;
static EXT_RAM_BSS_ATTR cc_status_card_t s_status_cards[4];
static nuwax_device_state_t s_state;
static bool s_have_state;
static bool s_status_dirty = true;
static bool s_menu_dirty = true;
static bool s_info_dirty = true;
static bool s_open;
static uint32_t s_input_scene_generation = 1U;
static bool s_ota_blocked;
static bool s_ota_overlay_owner;
static bool s_power_overlay_owner;
static bool s_closing;
static bool s_dragging;
static bool s_cloud_animating;
static bool s_battery_animating;
static int64_t s_cloud_anim_last_frame_us;
static int64_t s_battery_anim_last_frame_us;
static uint8_t s_wifi_connect_phase;
static int64_t s_wifi_connect_last_us;
static int16_t s_motion_y = CC_HIDDEN_Y;
static int32_t s_reveal_raw;
static int32_t s_reveal_anim_from;
static int32_t s_reveal_anim_target;
static bool s_backdrop_frozen_face;
static lv_timer_t *s_reveal_timer;
static bool s_qr_reveal_active;
static bool s_qr_direct_open;
static uint32_t s_qr_render_frames, s_qr_render_gap_us;
static int64_t s_qr_render_last_us;
static void start_provisioning_qr_reveal(bool opening);
static void hide_detail(void);
static uint32_t s_reveal_anim_steps;
static uint32_t s_reveal_anim_step;
static uint32_t s_reveal_anim_duration_ms;
static int64_t s_reveal_anim_started_us;
static int32_t s_backdrop_progress;
static bool s_reveal_perf_active;
static bool s_reveal_perf_opening;
static uint32_t s_reveal_perf_frames;
static uint32_t s_reveal_perf_max_gap_us;
static uint32_t s_reveal_perf_backdrop_frames;
static uint32_t s_reveal_perf_backdrop_areas;
static uint32_t s_reveal_perf_backdrop_pixels;
static uint32_t s_reveal_perf_backdrop_max_pixels;
static uint32_t s_reveal_perf_skipped_steps;
static int64_t s_reveal_perf_started_us;
static int64_t s_reveal_perf_last_frame_us;
typedef struct {
    int16_t left_q4;
    int16_t right_q4;
} cc_reveal_mask_row_t;
static cc_reveal_mask_row_t s_reveal_mask_rows[CC_H];
static int32_t s_page_progress;
static cc_page_t s_page = CC_PAGE_STATUS;
static bool s_page_perf_active;
static bool s_page_perf_drag;
static uint32_t s_page_perf_frames;
static uint32_t s_page_perf_max_gap_us;
static int64_t s_page_perf_started_us;
static int64_t s_page_perf_last_frame_us;
static bool s_page_dragging;
static int32_t s_page_drag_origin_progress;
static bool s_page_transition_active;
static cc_page_t s_page_transition_target;
static cc_page_t s_page_transition_settle_page;
static EXT_RAM_BSS_ATTR cc_transition_cache_component_t
    s_transition_cache[CC_TRANSITION_CACHE_COMPONENTS];
static uint8_t *s_transition_cache_arena;
static size_t s_transition_cache_arena_size;
static size_t s_transition_cache_bank_size;
static int64_t s_cache_dirty_since_us[2];
static bool s_transition_cache_available;
static lv_draw_buf_t s_pager_frame_buf;
static uint8_t *s_pager_frame_storage;
static bool s_pager_frame_valid;
static lv_area_t s_pager_frame_area;
/* Immutable child pixels, mutable coordinates: sliding the pill must not
 * rerasterize the rounded borders and Chinese glyphs on every frame. */
#define CC_PAGER_SPRITES 5U
static struct {
    lv_obj_t *source;
    lv_draw_buf_t buffer;
    uint8_t *storage;
    uint32_t color_key;
    bool valid;
} s_pager_sprites[CC_PAGER_SPRITES];
static bool s_pager_staging_lease;
static struct {
    uint32_t builds, hits, prepared, fallback, sprite_frames, sprite_builds;
    uint32_t snapshot_us, snapshot_max_us, handoffs, handoff_us, handoff_max_us;
    int64_t logged_us;
} s_motion_cost;

static void report_motion_cost(int64_t now_us) {
    if (now_us - s_motion_cost.logged_us < 2000000LL) return;
    if (s_motion_cost.builds || s_motion_cost.handoffs) {
        ESP_LOGI(TAG, "motion work pager_builds=%u hits=%u prepared=%u fallback=%u pager_avg_us=%u pager_max_us=%u handoffs=%u handoff_avg_us=%u handoff_max_us=%u sprite_frames=%u sprite_builds=%u",
            (unsigned)s_motion_cost.builds, (unsigned)s_motion_cost.hits,
            (unsigned)s_motion_cost.prepared, (unsigned)s_motion_cost.fallback,
            (unsigned)(s_motion_cost.builds ? s_motion_cost.snapshot_us / s_motion_cost.builds : 0),
            (unsigned)s_motion_cost.snapshot_max_us, (unsigned)s_motion_cost.handoffs,
            (unsigned)(s_motion_cost.handoffs ? s_motion_cost.handoff_us / s_motion_cost.handoffs : 0),
            (unsigned)s_motion_cost.handoff_max_us,
            (unsigned)s_motion_cost.sprite_frames, (unsigned)s_motion_cost.sprite_builds);
    }
    memset(&s_motion_cost, 0, sizeof(s_motion_cost));
    s_motion_cost.logged_us = now_us;
}
static bool s_pager_cache_valid;
static int32_t s_pager_cache_key[12];
static bool s_transition_cache_active;
static bool s_cache_plan_valid;
static bool s_transition_cache_failure_logged;
static bool s_status_cache_dirty = true;
static bool s_menu_cache_dirty = true;
static uint32_t s_status_cache_generation = 1U;
static uint32_t s_menu_cache_generation = 1U;
static bool s_transition_cache_build_active;
static cc_page_t s_transition_cache_build_page;
static uint32_t s_transition_cache_build_generation;
static size_t s_transition_cache_build_next;
static uint32_t s_transition_cache_build_components;
static uint32_t s_transition_cache_build_mask;
static int s_volume_visual = -1;
static int64_t s_volume_cache_settle_us;
static int s_meeting_visual = -1;
static int s_provision_visual = -1;
static uint32_t s_transition_cache_build_work_us;
static uint32_t s_transition_cache_build_max_component_us;
static int64_t s_transition_cache_build_started_us;
static cc_detail_t s_detail = CC_DETAIL_NONE;
static bool s_meeting_pending;
static bool s_meeting_pending_target;
static bool s_provision_pending;
static bool s_provision_pending_target;
static int64_t s_pending_deadline_us;
static int64_t s_provision_pending_deadline_us;
#if NUWAX_TEST_NETWORK_ENABLED
static bool s_offline_test_pending;
static bool s_offline_test_pending_target;
static int64_t s_offline_test_pending_deadline_us;
#endif
#if CONFIG_NUWAX_UI_DISPLAY_TEST_MODE
static bool s_test_meeting_active;
static bool s_test_provision_active;
#endif

/* LVGL owner only. Keep the published bank intact until all changed tiles
 * belong to the same generation. Unchanged tiles need no snapshot or copy. */
static void transition_cache_mark_dirty(cc_page_t page, lv_obj_t *source) {
    for (size_t i = 0; i < CC_TRANSITION_CACHE_COMPONENTS; ++i) {
        if (s_transition_cache[i].page == page &&
            (!source || s_transition_cache[i].source == source)) {
            s_transition_cache[i].dirty = true;
        }
    }
    uint32_t *generation = page == CC_PAGE_STATUS
                               ? &s_status_cache_generation
                               : &s_menu_cache_generation;
    bool *dirty = page == CC_PAGE_STATUS
                      ? &s_status_cache_dirty
                      : &s_menu_cache_dirty;
    if (!*dirty) s_cache_dirty_since_us[page] = esp_timer_get_time();
    *dirty = true;
    (*generation)++;
    if (*generation == 0U) {
        *generation = 1U;
    }
}

static void transition_cache_mark_page_dirty(cc_page_t page) {
    transition_cache_mark_dirty(page, NULL);
}

static bool point_in_rect(uint16_t x,
                          uint16_t y,
                          int16_t rx,
                          int16_t ry,
                          int16_t rw,
                          int16_t rh) {
    return (int)x >= rx && (int)x < rx + rw &&
           (int)y >= ry && (int)y < ry + rh;
}

static bool state_affects_status(
    const nuwax_device_state_t *previous,
    const nuwax_device_state_t *next) {
    if (!previous || !next) return true;
    return previous->wifi != next->wifi ||
           previous->cloud != next->cloud ||
           previous->dialog != next->dialog ||
           previous->power != next->power ||
           previous->meeting != next->meeting ||
           previous->monitoring != next->monitoring ||
           previous->battery_valid != next->battery_valid ||
           previous->battery_percent != next->battery_percent ||
           previous->wifi_signal_valid != next->wifi_signal_valid ||
           previous->wifi_signal != next->wifi_signal ||
           previous->provisioning_active != next->provisioning_active ||
           previous->provisioning_starting != next->provisioning_starting;
}

static bool state_affects_menu(const nuwax_device_state_t *previous,
                               const nuwax_device_state_t *next) {
    if (!previous || !next) return true;
    return previous->meeting != next->meeting ||
           previous->monitoring != next->monitoring ||
           previous->brightness_percent != next->brightness_percent ||
           previous->provisioning_active != next->provisioning_active ||
           previous->provisioning_starting != next->provisioning_starting ||
           previous->simulated_offline != next->simulated_offline;
}

static bool state_affects_info(const nuwax_device_state_t *previous,
                               const nuwax_device_state_t *next) {
    if (!previous || !next) return true;
    return strncmp(previous->ssid, next->ssid, sizeof(previous->ssid)) != 0 ||
           strncmp(previous->ip, next->ip, sizeof(previous->ip)) != 0;
}

static bool control_center_memory_preflight(void) {
#if CONFIG_SPIRAM
    const size_t internal_free = heap_caps_get_free_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t internal_largest = heap_caps_get_largest_free_block(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t psram_free = heap_caps_get_free_size(
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t psram_largest = heap_caps_get_largest_free_block(
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG,
             "control-center memory preflight internal=%u/%u psram=%u/%u reserve=%u largest_min=%u",
             (unsigned)internal_free,
             (unsigned)internal_largest,
             (unsigned)psram_free,
             (unsigned)psram_largest,
             (unsigned)CC_PSRAM_RUNTIME_RESERVE_BYTES,
             (unsigned)CC_PSRAM_LARGEST_BLOCK_MIN_BYTES);
    if (psram_free < CC_PSRAM_RUNTIME_RESERVE_BYTES ||
        psram_largest < CC_PSRAM_LARGEST_BLOCK_MIN_BYTES) {
        ESP_LOGE(TAG,
                 "control-center creation rejected: PSRAM reserve is insufficient");
        return false;
    }
#endif
    return true;
}

static lv_obj_t *make_label(lv_obj_t *parent,
                            const lv_font_t *font,
                            lv_color_t color,
                            lv_opa_t opacity,
                            lv_text_align_t align) {
    lv_obj_t *label = lv_label_create(parent);
    if (!label) {
        return NULL;
    }
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_opa(label, opacity, 0);
    lv_obj_set_style_text_align(label, align, 0);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return label;
}

static lv_obj_t *make_panel(lv_obj_t *parent,
                            int16_t x,
                            int16_t y,
                            int16_t w,
                            int16_t h) {
    lv_obj_t *panel = lv_obj_create(parent);
    if (!panel) {
        return NULL;
    }
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_radius(panel, 15, 0);
    /* Children stay within the panel bounds. Avoiding software corner
     * clipping removes a mask pass from every animated card. */
    lv_obj_set_style_clip_corner(panel, false, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_border_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    return panel;
}

static void draw_backdrop_cb(lv_event_t *event);
/* These nodes only position/clip children. Running lv_obj's generic rectangle,
 * blur and scrollbar style resolution for each partial buffer is unnecessary.
 * Keep geometry, child traversal and all non-drawing events in LVGL. */
static void layout_only_draw_cb(lv_event_t *event) {
    const lv_event_code_t code = lv_event_get_code(event);
    if (code != LV_EVENT_DRAW_MAIN && code != LV_EVENT_DRAW_MAIN_END &&
        code != LV_EVENT_DRAW_POST) return;
    if (code == LV_EVENT_DRAW_MAIN && lv_event_get_current_target(event) == s_reveal_clip)
        draw_backdrop_cb(event);
    lv_event_stop_processing(event);
}
static void use_layout_only_draw(lv_obj_t *obj) {
    lv_obj_add_event_cb(obj, layout_only_draw_cb,
        (lv_event_code_t)(LV_EVENT_ALL | LV_EVENT_PREPROCESS), NULL);
}

static lv_obj_t *make_reveal_clip(lv_obj_t *parent,
                                  int16_t x,
                                  int16_t y,
                                  int16_t w,
                                  int16_t h) {
    lv_obj_t *clip = lv_obj_create(parent);
    if (!clip) {
        return NULL;
    }
    lv_obj_set_pos(clip, x, y);
    lv_obj_set_size(clip, w, h);
    lv_obj_set_style_radius(clip, 0, 0);
    lv_obj_set_style_clip_corner(clip, false, 0);
    lv_obj_set_style_bg_opa(clip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(clip, 0, 0);
    lv_obj_set_style_pad_all(clip, 0, 0);
    lv_obj_remove_flag(clip,
                       LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE |
                           LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    use_layout_only_draw(clip);
    return clip;
}

static lv_obj_t *create_icon_image(lv_obj_t *parent,
    nuwax_control_center_icon_id_t icon_id, int16_t x, int16_t y);

static lv_obj_t *create_page_header(lv_obj_t *page, const char *title,
                                   lv_obj_t **out_clip) {
    lv_obj_t *clip = make_reveal_clip(page, CC_HEADER_X, CC_HEADER_Y,
                                      CC_HEADER_W, CC_HEADER_H);
    if (!clip) return NULL;
    if (out_clip) *out_clip = clip;
    lv_obj_t *header = make_reveal_clip(clip, 0, 0, CC_HEADER_W, CC_HEADER_H);
    if (!header || !create_icon_image(header, NUWAX_CC_ICON_CAT, 26, 1)) return NULL;
    lv_obj_t *heading = make_label(header, &nuwax_font_lingzhu_22,
        lv_color_white(), LV_OPA_COVER, LV_TEXT_ALIGN_LEFT);
    if (!heading) return NULL;
    lv_obj_set_pos(heading, 72, 7);
    lv_obj_set_size(heading, 100, 30);
    lv_label_set_text(heading, title);
    static const lv_point_precise_t rays[4][2] = {
        {{9, 11}, {15, 14}}, {{9, 28}, {15, 25}},
        {{184, 14}, {190, 11}}, {{184, 25}, {190, 28}},
    };
    for (unsigned i = 0; i < 4; ++i) {
        lv_obj_t *ray = lv_line_create(header);
        if (!ray) return NULL;
        lv_line_set_points(ray, rays[i], 2);
        lv_obj_set_style_line_width(ray, 3, 0);
        lv_obj_set_style_line_rounded(ray, true, 0);
        lv_obj_set_style_line_color(ray, lv_color_white(), 0);
        lv_obj_remove_flag(ray, LV_OBJ_FLAG_CLICKABLE);
    }
    return header;
}

static void update_page_dots(void) {
    lv_obj_t *tabs[] = {s_status_dot, s_menu_dot,
#if NUWAX_TEST_PANEL_ENABLED
        s_test_dot
#else
        NULL
#endif
    };
    int32_t position = (int32_t)s_page * 1000;
    if (s_page_transition_active) position = s_page_progress;
    if (s_sheet_active && s_sheet_tab_from >= 0 && s_sheet_from != 0) {
        const int32_t remaining = lv_obj_get_x(s_sheet) * 1000 / s_sheet_from;
        position = (int32_t)s_page * 1000 + (s_sheet_tab_from - (int)s_page) * remaining;
    }
#if NUWAX_TEST_PANEL_ENABLED
    const int32_t pill_x = position * 69 / 1000;
#else
    const int32_t pill_x = 17 + position * 92 / 1000;
#endif
    const bool moving = s_page_transition_active || (s_sheet_active && s_sheet_tab_from >= 0);
    const lv_color_t pill_color = moving ? lv_color_hex(0x606060) : lv_color_white();
    if (s_tab_indicator && !lv_color_eq(lv_obj_get_style_bg_color(s_tab_indicator, 0), pill_color))
        lv_obj_set_style_bg_color(s_tab_indicator, pill_color, 0);
    if (s_tab_indicator && lv_obj_get_x(s_tab_indicator) != pill_x)
        lv_obj_set_x(s_tab_indicator, pill_x);
    for (unsigned i = 0; i < 3; ++i) {
        if (!tabs[i] || !s_tab_labels[i]) continue;
        const bool active = (unsigned)((position + 500) / 1000) == i;
        const lv_color_t color = active && !moving ? lv_color_black() : lv_color_white();
        if (!lv_color_eq(lv_obj_get_style_text_color(s_tab_labels[i], 0), color))
            lv_obj_set_style_text_color(s_tab_labels[i], color, 0);
    }
}

static lv_obj_t *create_page_tab(unsigned index, int x, int w, const char *name) {
    lv_obj_t *tab = make_panel(s_pager, x, 0, w, 26);
    if (!tab) return NULL;
    lv_obj_set_style_radius(tab, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(tab, LV_OPA_TRANSP, 0);
    s_tab_labels[index] = make_label(tab, &nuwax_font_lingzhu_17,
        lv_color_white(), LV_OPA_COVER, LV_TEXT_ALIGN_CENTER);
    if (!s_tab_labels[index]) return NULL;
    lv_obj_set_width(s_tab_labels[index], w - 4);
    lv_label_set_text(s_tab_labels[index], name);
    lv_obj_center(s_tab_labels[index]);
    return tab;
}

static bool create_page_dots(lv_obj_t *parent) {
    s_pager_clip = make_reveal_clip(parent, CC_PAGER_X, CC_PAGER_Y, CC_PAGER_W, CC_PAGER_H);
    if (!s_pager_clip) return false;
    s_pager = make_reveal_clip(s_pager_clip, 0, 0, CC_PAGER_W, CC_PAGER_H);
    if (!s_pager) return false;
    s_tab_indicator = make_panel(s_pager, 0, 0,
#if NUWAX_TEST_PANEL_ENABLED
        60,
#else
        72,
#endif
        26);
    if (!s_tab_indicator) return false;
    lv_obj_set_style_radius(s_tab_indicator, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_tab_indicator, lv_color_white(), 0);
#if NUWAX_TEST_PANEL_ENABLED
    s_status_dot = create_page_tab(0, 0, 60, "状态");
    s_menu_dot = create_page_tab(1, 69, 60, "菜单");
    s_test_dot = create_page_tab(2, 138, 60, "测试");
    if (!s_test_dot) return false;
#else
    s_status_dot = create_page_tab(0, 17, 72, "状态");
    s_menu_dot = create_page_tab(1, 109, 72, "菜单");
#endif
    if (!s_status_dot || !s_menu_dot ||
        !create_icon_image(s_pager, NUWAX_CC_ICON_UP, 81, 24)) return false;
    update_page_dots();
    return true;
}

static bool create_status_card(lv_obj_t *parent,
                               cc_status_card_t *card,
                               int16_t x,
                               int16_t y) {
    memset(card, 0, sizeof(*card));
    card->image_id = NUWAX_CC_ICON_COUNT;
    card->clip = make_reveal_clip(parent, x, y,
                                  CC_CARD_W, CC_STATUS_CARD_H);
    if (!card->clip) return false;
    card->root = make_panel(card->clip, 0, 0,
                            CC_CARD_W, CC_STATUS_CARD_H);
    if (!card->root) return false;
    lv_obj_set_style_transform_pivot_x(card->root, CC_CARD_W / 2, 0);
    lv_obj_set_style_transform_pivot_y(card->root, CC_STATUS_CARD_H / 2, 0);

    card->icon_back = lv_obj_create(card->root);
    if (!card->icon_back) return false;
    lv_obj_set_pos(card->icon_back, 35, 0);
    lv_obj_set_size(card->icon_back, 60, 60);
    lv_obj_set_style_radius(card->icon_back, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(card->icon_back, lv_color_hex(0x494949), 0);
    lv_obj_set_style_bg_opa(card->icon_back, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(card->icon_back, 0, 0);
    lv_obj_set_style_pad_all(card->icon_back, 0, 0);
    lv_obj_remove_flag(card->icon_back,
                       LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    card->image = lv_image_create(card->icon_back);
    if (!card->image) return false;
    lv_image_set_antialias(card->image, false);
    lv_obj_set_style_image_recolor(card->image, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_image_recolor_opa(card->image, LV_OPA_COVER, 0);
    lv_obj_center(card->image);
    lv_obj_remove_flag(card->image,
                       LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    card->value = make_label(card->root,
                             &nuwax_font_lingzhu_17,
                             lv_color_hex(0xffffff),
                             LV_OPA_COVER,
                             LV_TEXT_ALIGN_LEFT);
    if (!card->value) return false;
    lv_obj_set_style_text_align(card->value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(card->value, 4, 57);
    lv_obj_set_width(card->value, CC_CARD_W - 12);
    lv_label_set_long_mode(card->value, LV_LABEL_LONG_DOT);
    lv_label_set_text(card->value, "--");

    card->offline_mark = make_label(card->icon_back,
                                    &lv_font_montserrat_10,
                                    lv_color_hex(0xffffff),
                                    LV_OPA_COVER,
                                    LV_TEXT_ALIGN_CENTER);
    if (!card->offline_mark) return false;
    lv_obj_set_pos(card->offline_mark, 44, 43);
    lv_obj_set_size(card->offline_mark, 13, 13);
    lv_obj_set_style_radius(card->offline_mark, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(card->offline_mark, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(card->offline_mark, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card->offline_mark, 2, 0);
    lv_obj_set_style_border_color(card->offline_mark, lv_color_hex(0x494949), 0);
    lv_obj_set_style_pad_all(card->offline_mark, 0, 0);
    lv_label_set_text(card->offline_mark, "x");
    lv_obj_add_flag(card->offline_mark, LV_OBJ_FLAG_HIDDEN);
    return true;
}

static void status_card_set_icon(cc_status_card_t *card,
                                 nuwax_control_center_icon_id_t icon,
                                 lv_color_t color) {
    if (!card || !card->image) {
        return;
    }
    if (card->image_id != icon) {
        const lv_image_dsc_t *source = nuwax_control_center_icon_get(icon);
        if (!source) return;
        card->image_id = icon;
        lv_image_set_src(card->image, source);
        lv_obj_center(card->image);
    }
    lv_obj_set_style_image_recolor(card->image, color, 0);
    lv_obj_set_style_image_recolor_opa(card->image, LV_OPA_COVER, 0);
}

static nuwax_control_center_icon_id_t dialog_icon(
    const nuwax_device_state_t *state) {
    if (state->monitoring != NUWAX_MONITORING_UI_DISABLED) {
        return NUWAX_CC_ICON_MONITORING;
    }
    if (state->meeting != NUWAX_MEETING_UI_DISABLED) {
        return NUWAX_CC_ICON_MEETING_STATE;
    }
    if (state->provisioning_active) {
        return NUWAX_CC_ICON_PROVISIONING;
    }
    switch (state->dialog) {
        case NUWAX_DIALOG_SLEEP: return NUWAX_CC_ICON_SLEEPING;
        case NUWAX_DIALOG_ERROR: return NUWAX_CC_ICON_WIFI_0;
        case NUWAX_DIALOG_READY:
        case NUWAX_DIALOG_LISTENING:
        case NUWAX_DIALOG_RECOGNIZING:
        case NUWAX_DIALOG_THINKING:
        case NUWAX_DIALOG_SPEAKING:
        default:
            return NUWAX_CC_ICON_CONVERSATION;
    }
}

static const char *dialog_text(const nuwax_device_state_t *state) {
    if (state->monitoring != NUWAX_MONITORING_UI_DISABLED) return "监听";
    if (state->meeting != NUWAX_MEETING_UI_DISABLED) return "会议";
    if (state->provisioning_active) return "配网中";
    switch (state->dialog) {
        case NUWAX_DIALOG_SLEEP: return "休眠";
        case NUWAX_DIALOG_LISTENING: return "聆听中";
        case NUWAX_DIALOG_RECOGNIZING: return "识别中";
        case NUWAX_DIALOG_THINKING: return "思考中";
        case NUWAX_DIALOG_SPEAKING: return "说话中";
        case NUWAX_DIALOG_ERROR: return "发生错误";
        case NUWAX_DIALOG_READY:
        default:
            return "待命";
    }
}

static const char *wifi_text(const nuwax_device_state_t *state) {
    if (state->wifi == NUWAX_WIFI_DISCONNECTED) return "WiFi未连接";
    if (state->wifi == NUWAX_WIFI_CONNECTING) return "WiFi连接中";
    if (state->wifi_signal_valid) {
        switch (state->wifi_signal) {
            case NUWAX_WIFI_SIGNAL_STRONG: return "WiFi信号强";
            case NUWAX_WIFI_SIGNAL_FAIR: return "WiFi信号中";
            case NUWAX_WIFI_SIGNAL_WEAK: return "WiFi信号弱";
            case NUWAX_WIFI_SIGNAL_UNKNOWN:
            default:
                break;
        }
    }
    return state->wifi == NUWAX_WIFI_WEAK ? "WiFi信号弱" : "WiFi信号强";
}

static const char *cloud_text(const nuwax_device_state_t *state) {
    switch (state->cloud) {
        case NUWAX_CLOUD_CONNECTED: return "云端已连接";
        case NUWAX_CLOUD_CONNECTING: return "云端连接中";
        case NUWAX_CLOUD_AUTHFAIL: return "云端未连接";
        case NUWAX_CLOUD_DISCONNECTED:
        default:
            return "云端未连接";
    }
}

static nuwax_control_center_icon_id_t wifi_icon(
    const nuwax_device_state_t *state) {
    if (state->wifi == NUWAX_WIFI_DISCONNECTED) {
        return NUWAX_CC_ICON_WIFI_0;
    }
    if (state->wifi == NUWAX_WIFI_CONNECTING) {
        return (nuwax_control_center_icon_id_t)(NUWAX_CC_ICON_WIFI_0 +
                                                s_wifi_connect_phase);
    }
    if (state->wifi == NUWAX_WIFI_WEAK) {
        return NUWAX_CC_ICON_WIFI_1;
    }
    if (state->wifi_signal_valid) {
        switch (state->wifi_signal) {
            case NUWAX_WIFI_SIGNAL_WEAK: return NUWAX_CC_ICON_WIFI_1;
            case NUWAX_WIFI_SIGNAL_FAIR: return NUWAX_CC_ICON_WIFI_2;
            case NUWAX_WIFI_SIGNAL_STRONG: return NUWAX_CC_ICON_WIFI_3;
            case NUWAX_WIFI_SIGNAL_UNKNOWN:
            default:
                break;
        }
    }
    return NUWAX_CC_ICON_WIFI_3;
}

static void format_power(char *out,
                         size_t out_size,
                         const nuwax_device_state_t *state) {
    if (!state->battery_valid || state->power == NUWAX_POWER_UNKNOWN) {
        strlcpy(out, "--", out_size);
        return;
    }
    if (state->power == NUWAX_POWER_EXTERNAL) {
        strlcpy(out, "外接供电", out_size);
        return;
    }
    const unsigned percent = state->battery_percent > 100U
                                 ? 100U
                                 : state->battery_percent;
    if (state->power == NUWAX_POWER_CHARGING) {
        snprintf(out, out_size, "充电%u%%", percent);
    } else {
        snprintf(out, out_size, "电量%u%%", percent);
    }
}

static void cloud_opa_exec(void *obj, int32_t value) {
    const int64_t now_us = esp_timer_get_time();
    if (s_cloud_anim_last_frame_us != 0 &&
        now_us - s_cloud_anim_last_frame_us <
            CC_LOCAL_ANIM_FRAME_INTERVAL_US) {
        return;
    }
    s_cloud_anim_last_frame_us = now_us;
    lv_obj_t *target = (lv_obj_t *)obj;
    const lv_opa_t opacity = (lv_opa_t)value;
    if (target && lv_obj_get_style_opa(target, LV_PART_MAIN) != opacity) {
        lv_obj_set_style_opa(target, opacity, 0);
    }
}

static void battery_opa_exec(void *obj, int32_t value) {
    const int64_t now_us = esp_timer_get_time();
    if (s_battery_anim_last_frame_us != 0 &&
        now_us - s_battery_anim_last_frame_us <
            CC_LOCAL_ANIM_FRAME_INTERVAL_US) {
        return;
    }
    s_battery_anim_last_frame_us = now_us;
    lv_obj_t *target = (lv_obj_t *)obj;
    const lv_opa_t opacity = (lv_opa_t)value;
    if (target && lv_obj_get_style_opa(target, LV_PART_MAIN) != opacity) {
        lv_obj_set_style_opa(target, opacity, 0);
    }
}

static void stop_local_animations(void) {
    if (s_cloud_beacon) {
        lv_anim_delete(s_cloud_beacon, cloud_opa_exec);
        lv_obj_set_style_opa(s_cloud_beacon, 0, 0);
    }
    if (s_battery_fill) {
        lv_anim_delete(s_battery_fill, battery_opa_exec);
        lv_obj_set_style_opa(s_battery_fill, LV_OPA_COVER, 0);
    }
    s_cloud_animating = false;
    s_battery_animating = false;
    s_cloud_anim_last_frame_us = 0;
    s_battery_anim_last_frame_us = 0;
}

static void start_local_animations(void) {
    if (!s_open || s_closing || s_dragging || s_reveal_timer || s_page_transition_active ||
        s_page != CC_PAGE_STATUS || s_reveal_raw < 1000 || !s_have_state) {
        return;
    }
    const bool cloud_breathes =
        s_state.cloud == NUWAX_CLOUD_CONNECTING;
    if (cloud_breathes && s_cloud_beacon && !s_cloud_animating) {
        s_cloud_anim_last_frame_us = 0;
        lv_anim_t cloud;
        lv_anim_init(&cloud);
        lv_anim_set_var(&cloud, s_cloud_beacon);
        lv_anim_set_values(&cloud, 31, 107);
        lv_anim_set_duration(&cloud, 1200U);
        lv_anim_set_playback_duration(&cloud, 1200U);
        lv_anim_set_repeat_count(&cloud, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&cloud, lv_anim_path_ease_in_out);
        lv_anim_set_exec_cb(&cloud, cloud_opa_exec);
        lv_anim_start(&cloud);
        s_cloud_animating = true;
    } else if (!cloud_breathes && s_cloud_animating) {
        lv_anim_delete(s_cloud_beacon, cloud_opa_exec);
        lv_obj_set_style_opa(s_cloud_beacon, 0, 0);
        s_cloud_animating = false;
    }
    /* Transforming this nested icon made LVGL invalidate its full-screen
     * transformed ancestor on ESP32-S3. Keep it at native scale; the cloud
     * beacon still provides a bounded breathing status animation. */
    const bool battery_breathes = s_state.power == NUWAX_POWER_CHARGING;
    if (battery_breathes && s_battery_fill && !s_battery_animating) {
        s_battery_anim_last_frame_us = 0;
        lv_anim_t battery;
        lv_anim_init(&battery);
        lv_anim_set_var(&battery, s_battery_fill);
        lv_anim_set_values(&battery, 115, LV_OPA_COVER);
        lv_anim_set_duration(&battery, 900);
        lv_anim_set_playback_duration(&battery, 900);
        lv_anim_set_repeat_count(&battery, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&battery, lv_anim_path_ease_in_out);
        lv_anim_set_exec_cb(&battery, battery_opa_exec);
        lv_anim_start(&battery);
        s_battery_animating = true;
    } else if (!battery_breathes && s_battery_animating) {
        lv_anim_delete(s_battery_fill, battery_opa_exec);
        lv_obj_set_style_opa(s_battery_fill, LV_OPA_COVER, 0);
        s_battery_animating = false;
    }
}

static bool create_status_page(void) {
    s_status_page = lv_obj_create(s_surface);
    if (!s_status_page) return false;
    lv_obj_set_pos(s_status_page, 0, 0);
    lv_obj_set_size(s_status_page, CC_W, CC_H);
    lv_obj_set_style_bg_opa(s_status_page, LV_OPA_TRANSP, 0);
    use_layout_only_draw(s_status_page);
    lv_obj_set_style_border_width(s_status_page, 0, 0);
    lv_obj_set_style_pad_all(s_status_page, 0, 0);
    lv_obj_remove_flag(s_status_page,
                       LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_status_header = create_page_header(s_status_page, "灵珠状态",
                                          &s_status_header_clip);
    if (!s_status_header ||
        !create_status_card(s_status_page, &s_status_cards[0],
                            CC_CARD_LEFT_X, CC_STATUS_ROW0_Y) ||
        !create_status_card(s_status_page, &s_status_cards[1],
                            CC_CARD_RIGHT_X, CC_STATUS_ROW0_Y) ||
        !create_status_card(s_status_page, &s_status_cards[2],
                            CC_CARD_LEFT_X, CC_STATUS_ROW1_Y) ||
        !create_status_card(s_status_page, &s_status_cards[3],
                            CC_CARD_RIGHT_X, CC_STATUS_ROW1_Y)) {
        return false;
    }

    s_cloud_beacon = lv_obj_create(s_status_cards[1].icon_back);
    if (!s_cloud_beacon) return false;
    lv_obj_set_pos(s_cloud_beacon, 1, 1);
    lv_obj_set_size(s_cloud_beacon, 58, 58);
    lv_obj_set_style_radius(s_cloud_beacon, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_cloud_beacon, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_cloud_beacon, 1, 0);
    lv_obj_set_style_border_color(s_cloud_beacon, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_border_opa(s_cloud_beacon, 140, 0);
    lv_obj_set_style_opa(s_cloud_beacon, 0, 0);
    lv_obj_set_style_pad_all(s_cloud_beacon, 0, 0);
    lv_obj_remove_flag(s_cloud_beacon,
                       LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_to_index(s_cloud_beacon, 0);

    s_battery_fill = lv_obj_create(s_status_cards[2].icon_back);
    if (!s_battery_fill) return false;
    lv_obj_set_pos(s_battery_fill, 10, 20);
    lv_obj_set_size(s_battery_fill, 35, 20);
    lv_obj_set_style_radius(s_battery_fill, 1, 0);
    lv_obj_set_style_bg_color(s_battery_fill, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(s_battery_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_battery_fill, 0, 0);
    lv_obj_set_style_pad_all(s_battery_fill, 0, 0);
    lv_obj_remove_flag(s_battery_fill,
                       LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_to_index(s_battery_fill, 0);
    /* Keep the charging mark above the existing animated level fill. */
    s_battery_charging_mark = lv_image_create(s_status_cards[2].icon_back);
    if (!s_battery_charging_mark) return false;
    lv_image_set_src(s_battery_charging_mark,
                    nuwax_control_center_icon_get(NUWAX_CC_ICON_BATTERY_CHARGING_MARK));
    lv_obj_set_pos(s_battery_charging_mark, 16, 15);
    lv_obj_remove_flag(s_battery_charging_mark, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_battery_charging_mark, LV_OBJ_FLAG_HIDDEN);
    s_footer_clip = make_reveal_clip(s_status_page, CC_FOOTER_X, CC_FOOTER_Y, CC_FOOTER_W, CC_FOOTER_H);
    if (!s_footer_clip) return false;
    s_footer = lv_obj_create(s_footer_clip);
    if (!s_footer) return false;
    lv_obj_set_pos(s_footer, 0, 0);
    lv_obj_set_size(s_footer, CC_FOOTER_W, CC_FOOTER_H);
    lv_obj_set_style_bg_opa(s_footer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_footer, 0, 0);
    lv_obj_set_style_pad_all(s_footer, 0, 0);
    lv_obj_remove_flag(s_footer,
                       LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_footer_dot = create_icon_image(s_footer, NUWAX_CC_ICON_CHECK, 4, 0);
    if (!s_footer_dot) return false;

    s_footer_label = make_label(s_footer,
                                &nuwax_font_lingzhu_17,
                                lv_color_hex(0xffffff),
                                LV_OPA_COVER,
                                LV_TEXT_ALIGN_LEFT);
    if (!s_footer_label) return false;
    lv_obj_set_pos(s_footer_label, 43, 3);
    lv_obj_set_width(s_footer_label, 140);
    lv_label_set_text(s_footer_label, "设备运行正常");
    return true;
}

static lv_obj_t *create_icon_image(lv_obj_t *parent,
                                   nuwax_control_center_icon_id_t icon_id,
                                   int16_t x,
                                   int16_t y) {
    lv_obj_t *icon = lv_image_create(parent);
    if (!icon) return NULL;
    lv_image_set_src(icon, nuwax_control_center_icon_get(icon_id));
    lv_image_set_antialias(icon, false);
    lv_obj_set_pos(icon, x, y);
    lv_obj_set_style_image_recolor(icon, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return icon;
}

static bool create_switch(lv_obj_t *parent,
                          int16_t x,
                          int16_t y,
                          lv_obj_t **out_track,
                          lv_obj_t **out_knob) {
    lv_obj_t *track = lv_obj_create(parent);
    if (!track) return false;
    lv_obj_set_pos(track, x, y);
    lv_obj_set_size(track, 34, 18);
    lv_obj_set_style_radius(track, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(track, lv_color_hex(0x888888), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(track, 2, 0);
    lv_obj_set_style_border_color(track, lv_color_white(), 0);
    lv_obj_set_style_pad_all(track, 0, 0);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *knob = lv_obj_create(track);
    if (!knob) return false;
    lv_obj_set_pos(knob, 0, 0);
    lv_obj_set_size(knob, 14, 14);
    lv_obj_set_style_radius(knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(knob, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(knob, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(knob, 0, 0);
    lv_obj_set_style_pad_all(knob, 0, 0);
    lv_obj_remove_flag(knob, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    *out_track = track;
    *out_knob = knob;
    return true;
}

static lv_obj_t *create_menu_mode_card(lv_obj_t *parent,
                                       int16_t x,
                                       int16_t y,
                                       nuwax_control_center_icon_id_t icon_id,
                                       const char *title,
                                       lv_obj_t **out_state,
                                       lv_obj_t **out_switch,
                                       lv_obj_t **out_knob) {
    lv_obj_t *card = make_panel(parent, x, y,
                                CC_CARD_W, CC_MENU_ACTION_H);
    if (!card || !create_icon_image(card, icon_id, 41, 1)) return NULL;
    lv_obj_t *name = make_label(card,
                                &nuwax_font_lingzhu_17,
                                lv_color_hex(0xffffff),
                                LV_OPA_COVER,
                                LV_TEXT_ALIGN_CENTER);
    if (!name) return NULL;
    lv_obj_set_pos(name, 4, 42);
    lv_obj_set_width(name, CC_CARD_W - 12);
    lv_label_set_text(name, title);
    lv_obj_t *state = make_label(card,
                                 &nuwax_font_lingzhu_14,
                                 lv_color_white(),
                                 LV_OPA_COVER,
                                 LV_TEXT_ALIGN_CENTER);
    if (!state) return NULL;
    lv_obj_set_pos(state, 65, 62);
    lv_obj_set_width(state, 52);
    lv_label_set_text(state, "已关闭");
    if (!create_switch(card, 22, 62, out_switch, out_knob)) return NULL;
    *out_state = state;
    return card;
}

static lv_obj_t *create_menu_action_card(lv_obj_t *parent,
                                          int16_t x,
                                          int16_t y,
                                          nuwax_control_center_icon_id_t icon_id,
                                          const char *title) {
    lv_obj_t *card = make_panel(parent, x, y,
                                CC_MENU_ACTION_W, CC_MENU_ACTION_H);
    if (!card || !create_icon_image(card, icon_id, 41, 1)) return NULL;
    lv_obj_t *name = make_label(card,
                                &nuwax_font_lingzhu_17,
                                lv_color_hex(0xffffff),
                                LV_OPA_COVER,
                                LV_TEXT_ALIGN_CENTER);
    if (!name) return NULL;
    lv_obj_set_pos(name, 4, 55);
    lv_obj_set_width(name, CC_CARD_W - 12);
    lv_label_set_text(name, title);
    return card;
}

static bool create_menu_page(void) {
    s_menu_page = lv_obj_create(s_surface);
    if (!s_menu_page) return false;
    lv_obj_set_pos(s_menu_page, 0, 0);
    lv_obj_set_size(s_menu_page, CC_W, CC_H);
    lv_obj_set_style_bg_opa(s_menu_page, LV_OPA_TRANSP, 0);
    use_layout_only_draw(s_menu_page);
    lv_obj_set_style_border_width(s_menu_page, 0, 0);
    lv_obj_set_style_pad_all(s_menu_page, 0, 0);
    lv_obj_remove_flag(s_menu_page,
                       LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_set_style_opa(s_menu_page, LV_OPA_COVER, 0);

    s_menu_header = create_page_header(s_menu_page, "快捷应用",
                                       &s_menu_header_clip);
    if (!s_menu_header) return false;
    s_menu_card_clips[0] = make_reveal_clip(
        s_menu_page, CC_CARD_LEFT_X, CC_MENU_ROW0_Y,
        CC_CARD_W, CC_MENU_ACTION_H);
    if (!s_menu_card_clips[0]) return false;
    s_menu_cards[0] = create_menu_mode_card(s_menu_card_clips[0],
                          0,
                          0,
                          NUWAX_CC_ICON_MEETING,
                          "会议模式",
                          &s_meeting_state,
                           &s_meeting_switch,
                           &s_meeting_knob);
    if (!s_menu_cards[0]) return false;
    s_menu_card_clips[1] = make_reveal_clip(
        s_menu_page, CC_CARD_RIGHT_X, CC_MENU_ROW0_Y,
        CC_CARD_W, CC_MENU_ACTION_H);
    if (!s_menu_card_clips[1]) return false;
    s_menu_cards[1] = make_panel(s_menu_card_clips[1], 0, 0, CC_CARD_W, CC_MENU_ACTION_H);
    if (!s_menu_cards[1]) return false;
    s_brightness_icon = create_icon_image(s_menu_cards[1], NUWAX_CC_ICON_SUN_HIGH, 43, 0);
    lv_obj_t *brightness_title = make_label(s_menu_cards[1], &nuwax_font_lingzhu_17,
        lv_color_white(), LV_OPA_COVER, LV_TEXT_ALIGN_CENTER);
    if (!s_brightness_icon || !brightness_title) return false;
    lv_obj_set_pos(brightness_title, 4, 40);
    lv_obj_set_width(brightness_title, CC_CARD_W - 12);
    lv_label_set_text(brightness_title, "屏幕亮度");
    for (unsigned i = 0; i < 3U; ++i) {
        lv_obj_t *bar = lv_obj_create(s_menu_cards[1]);
        if (!bar) return false;
        lv_obj_remove_style_all(bar);
        lv_obj_set_pos(bar, 13 + (int)i * 37, 69);
        lv_obj_set_size(bar, 29, 5);
        lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        s_brightness_bars[i] = bar;
    }
    if (!s_menu_cards[1]) return false;
    s_menu_card_clips[2] = make_reveal_clip(
        s_menu_page, CC_CARD_LEFT_X, CC_MENU_ROW1_Y,
        CC_CARD_W, CC_MENU_ACTION_H);
    if (!s_menu_card_clips[2]) return false;
    s_menu_cards[2] = create_menu_action_card(s_menu_card_clips[2],
                            0,
                            0,
                            NUWAX_CC_ICON_PROVISIONING,
                            "点击配网");
    if (!s_menu_cards[2]) return false;
    s_provision_label = lv_obj_get_child(s_menu_cards[2], -1);
    s_menu_card_clips[3] = make_reveal_clip(
        s_menu_page, CC_CARD_RIGHT_X, CC_MENU_ROW1_Y,
        CC_CARD_W, CC_MENU_ACTION_H);
    if (!s_menu_card_clips[3]) return false;
    s_menu_cards[3] = create_menu_action_card(s_menu_card_clips[3],
                            0,
                            0,
                            NUWAX_CC_ICON_DEVICE_INFO,
                            "设备信息");
    if (!s_menu_cards[3]) return false;

    s_menu_volume = make_panel(s_menu_page,
                               CC_MENU_VOLUME_X,
                               CC_MENU_VOLUME_Y,
                               CC_MENU_VOLUME_W,
                               CC_MENU_VOLUME_H);
    if (!s_menu_volume ||
        !create_icon_image(s_menu_volume, NUWAX_CC_ICON_VOLUME, 0, 1)) {
        return false;
    }
    lv_obj_set_style_border_width(s_menu_volume, 0, 0);
    lv_obj_t *volume_label = make_label(s_menu_volume,
                                        &nuwax_font_lingzhu_14,
                                        lv_color_hex(0xffffff),
                                        LV_OPA_COVER,
                                        LV_TEXT_ALIGN_LEFT);
    if (!volume_label) return false;
    lv_obj_set_pos(volume_label, 32, 8);
    lv_obj_set_width(volume_label, 28);
    lv_label_set_text(volume_label, "音量");
    s_volume_text = make_label(s_menu_volume,
                               &nuwax_font_lingzhu_14,
                               lv_color_hex(0xffffff),
                               LV_OPA_COVER,
                               LV_TEXT_ALIGN_RIGHT);
    if (!s_volume_text) return false;
    lv_obj_set_pos(s_volume_text, 63, 8);
    lv_obj_set_width(s_volume_text, 39);
    lv_label_set_text(s_volume_text, "60%");

    /* Snapshot the overlapping slider and knob with their common background.
     * Independent RGB565 snapshots lose the knob's edge alpha over the bar. */
    s_volume_bar = lv_obj_create(s_menu_volume);
    if (!s_volume_bar) return false;
    lv_obj_set_pos(s_volume_bar, CC_VOLUME_BAR_X - CC_MENU_VOLUME_X,
                   CC_VOLUME_BAR_Y - CC_MENU_VOLUME_Y);
    lv_obj_set_size(s_volume_bar, CC_VOLUME_BAR_W, CC_VOLUME_BAR_H);
    lv_obj_set_style_radius(s_volume_bar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_volume_bar, lv_color_hex(0x5c5c5c), 0);
    lv_obj_set_style_bg_opa(s_volume_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_volume_bar, 0, 0);
    lv_obj_set_style_pad_all(s_volume_bar, 0, 0);
    lv_obj_remove_flag(s_volume_bar,
                       LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_volume_fill = lv_obj_create(s_volume_bar);
    if (!s_volume_fill) return false;
    lv_obj_set_pos(s_volume_fill, 0, 0);
    lv_obj_set_size(s_volume_fill, 1, CC_VOLUME_BAR_H);
    lv_obj_set_style_radius(s_volume_fill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_volume_fill, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(s_volume_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_volume_fill, 0, 0);
    lv_obj_set_style_pad_all(s_volume_fill, 0, 0);
    lv_obj_remove_flag(s_volume_fill,
                       LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_volume_knob = lv_obj_create(s_menu_volume);
    if (!s_volume_knob) return false;
    lv_obj_set_size(s_volume_knob, CC_VOLUME_KNOB_D, CC_VOLUME_KNOB_D);
    lv_obj_set_style_radius(s_volume_knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_volume_knob, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_volume_knob, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_volume_knob, 2, 0);
    lv_obj_set_style_border_color(s_volume_knob, lv_color_white(), 0);
    lv_obj_set_style_pad_all(s_volume_knob, 0, 0);
    lv_obj_remove_flag(s_volume_knob,
                       LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    return true;
}

static bool create_info_page(void) {
    s_info_page = lv_obj_create(s_surface);
    if (!s_info_page) return false;
    lv_obj_set_pos(s_info_page, 0, 0);
    lv_obj_set_size(s_info_page, CC_W, CC_H);
    lv_obj_set_style_radius(s_info_page, 0, 0);
    lv_obj_set_style_bg_color(s_info_page, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_info_page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_info_page, 0, 0);
    lv_obj_set_style_pad_all(s_info_page, 0, 0);
    lv_obj_remove_flag(s_info_page, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    s_info_back = nuwax_detail_back_button(s_info_page);
    if (!nuwax_detail_header(s_info_page, "设备信息") || !s_info_back) return false;
    lv_obj_t *identity = nuwax_detail_panel(s_info_page, 53, 76, 254, 60);
    lv_obj_t *firmware = nuwax_detail_panel(s_info_page, 53, 144, 254, 56);
    lv_obj_t *network = nuwax_detail_panel(s_info_page, 53, 208, 254, 98);
    if (!identity || !firmware || !network) return false;
    s_info_version_title = nuwax_detail_label(firmware, 14, 15, 108, &nuwax_font_lingzhu_17, "固件版本");
    if (!s_info_version_title || !nuwax_detail_label(identity, 14, 6, 222, &nuwax_font_lingzhu_14, "设备 ID") ||
        !nuwax_detail_label(network, 14, 7, 24, &lv_font_montserrat_14, LV_SYMBOL_WIFI) ||
        !nuwax_detail_label(network, 40, 6, 180, &nuwax_font_lingzhu_14, "当前网络")) return false;
    s_info_id = nuwax_detail_label(identity, 14, 28, 222, &nuwax_font_lingzhu_17, "--");
    s_info_version = nuwax_detail_label(firmware, 14, 25, 222, &nuwax_font_lingzhu_14, "--");
    s_info_ssid = nuwax_detail_label(network, 14, 29, 222, &nuwax_font_lingzhu_14, "未连接");
    s_info_ip = nuwax_detail_label(network, 14, 72, 222, &nuwax_font_lingzhu_14, "IP  --");
    if (!s_info_id || !s_info_version || !s_info_ssid || !s_info_ip) return false;
    lv_obj_add_flag(s_info_page, LV_OBJ_FLAG_HIDDEN);
    return true;
}

static void set_switch_visual(lv_obj_t *track,
                              lv_obj_t *knob,
                              bool enabled,
                              bool pending,
                              bool locked) {
    if (!track || !knob) {
        return;
    }
    const lv_color_t track_color = enabled
                                       ? lv_color_white()
                                       : pending
                                             ? lv_color_hex(0x666666)
                                             : lv_color_hex(0x888888);
    lv_obj_set_style_bg_color(track, track_color, 0);
    lv_obj_set_style_opa(track, locked ? 105 : LV_OPA_COVER, 0);
    lv_obj_set_x(knob, enabled ? 16 : 0);
    lv_obj_set_style_bg_color(knob, lv_color_black(), 0);
    lv_obj_set_style_border_width(knob, 1, 0);
    lv_obj_set_style_border_color(knob, lv_color_white(), 0);
}

static bool meeting_active(void) {
#if CONFIG_NUWAX_UI_DISPLAY_TEST_MODE
    return s_test_meeting_active;
#else
    return s_have_state && s_state.meeting == NUWAX_MEETING_UI_ACTIVE;
#endif
}

static bool meeting_transitioning(void) {
    return s_have_state &&
           (s_state.meeting == NUWAX_MEETING_UI_STARTING ||
            s_state.meeting == NUWAX_MEETING_UI_STOPPING);
}

static bool provisioning_active(void) {
#if CONFIG_NUWAX_UI_DISPLAY_TEST_MODE
    return s_test_provision_active;
#else
    return s_have_state && s_state.provisioning_active;
#endif
}

static bool monitoring_active(void) {
    return s_have_state &&
           s_state.monitoring != NUWAX_MONITORING_UI_DISABLED;
}

static const char *meeting_state_text(void) {
    if (s_meeting_pending) {
        return s_meeting_pending_target ? "启动中" : "停止中";
    }
#if CONFIG_NUWAX_UI_DISPLAY_TEST_MODE
    return s_test_meeting_active ? "运行中" : "已关闭";
#else
    if (!s_have_state) return "未可用";
    switch (s_state.meeting) {
        case NUWAX_MEETING_UI_STARTING: return "启动中";
        case NUWAX_MEETING_UI_ACTIVE: return "运行中";
        case NUWAX_MEETING_UI_STOPPING: return "停止中";
        case NUWAX_MEETING_UI_ERROR: return "失败";
        case NUWAX_MEETING_UI_DISABLED:
        default:
            return "已关闭";
    }
#endif
}

static void set_brightness_visual(uint8_t percent) {
    const int level = percent == 40U ? 0 : percent == 70U ? 1 : 2;
    if (!s_brightness_icon || s_brightness_visual == level) return;
    s_brightness_visual = level;
    static const nuwax_control_center_icon_id_t icons[] = {
        NUWAX_CC_ICON_SUN_LOW, NUWAX_CC_ICON_SUN_MID, NUWAX_CC_ICON_SUN_HIGH};
    lv_image_set_src(s_brightness_icon, nuwax_control_center_icon_get(icons[level]));
    for (int i = 0; i < 3; ++i)
        lv_obj_set_style_bg_color(s_brightness_bars[i], lv_color_hex(i <= level ? 0xffffff : 0x505050), 0);
    transition_cache_mark_dirty(CC_PAGE_MENU, s_menu_cards[1]);
}

static void set_volume_visual(uint8_t volume) {
    if (!s_volume_fill || !s_volume_knob || !s_volume_text) {
        return;
    }
    const uint8_t clamped = volume > 100U ? 100U : volume;
    if (s_volume_visual == clamped) return;
    s_volume_visual = clamped;
    int width = (CC_VOLUME_BAR_W * clamped) / 100;
    if (width < 1) width = 1;
    lv_obj_set_width(s_volume_fill, width);
    int knob_x = CC_VOLUME_BAR_X + width - CC_VOLUME_KNOB_D / 2;
    if (knob_x < CC_VOLUME_BAR_X - CC_VOLUME_KNOB_D / 2) knob_x = CC_VOLUME_BAR_X - CC_VOLUME_KNOB_D / 2;
    if (knob_x > CC_VOLUME_BAR_X + CC_VOLUME_BAR_W - CC_VOLUME_KNOB_D / 2) {
        knob_x = CC_VOLUME_BAR_X + CC_VOLUME_BAR_W - CC_VOLUME_KNOB_D / 2;
    }
    lv_obj_set_pos(s_volume_knob, knob_x - CC_MENU_VOLUME_X,
                   CC_VOLUME_BAR_Y - CC_MENU_VOLUME_Y -
                       (CC_VOLUME_KNOB_D - CC_VOLUME_BAR_H) / 2);
    lv_label_set_text_fmt(s_volume_text, "%u%%", (unsigned)clamped);
    s_volume_cache_settle_us = esp_timer_get_time() + 150000LL;
    transition_cache_mark_dirty(CC_PAGE_MENU, s_menu_volume);
}

static void render_menu(void) {
    if (!s_menu_page) {
        return;
    }
    const bool meeting = meeting_active();
    const bool provisioning = provisioning_active();
    const bool monitor = monitoring_active();
    const bool starting = s_have_state && s_state.provisioning_starting;
    const bool meeting_locked = monitor || provisioning || starting || s_provision_pending;
    const bool provision_locked = starting || s_provision_pending;
    const int meeting_visual = meeting | (s_meeting_pending << 1) |
                               (meeting_locked << 2);
    const bool provision_on = s_provision_pending ? s_provision_pending_target : provisioning;
    const int provision_visual = provision_on | (s_provision_pending << 1) |
                                 (provision_locked << 2);
    if (meeting_visual != s_meeting_visual ||
        strcmp(lv_label_get_text(s_meeting_state), meeting_state_text()) != 0) {
        s_meeting_visual = meeting_visual;
        lv_label_set_text(s_meeting_state, meeting_state_text());
        set_switch_visual(s_meeting_switch,
                          s_meeting_knob,
                          meeting,
                          s_meeting_pending,
                          meeting_locked);
        transition_cache_mark_dirty(CC_PAGE_MENU, s_menu_cards[0]);
    }
    const char *pair_text = provision_locked ?
        (s_provision_pending && !s_provision_pending_target ? "关闭中" : "开启中") : "点击配网";
    if (provision_visual != s_provision_visual || strcmp(lv_label_get_text(s_provision_label), pair_text) != 0) {
        s_provision_visual = provision_visual;
        lv_label_set_text(s_provision_label, pair_text);
        transition_cache_mark_dirty(CC_PAGE_MENU, s_menu_cards[2]);
    }
    set_brightness_visual(s_have_state ? s_state.brightness_percent : 100U);
    set_volume_visual(s_have_state ? s_state.volume : 60U);
#if NUWAX_TEST_NETWORK_ENABLED
    const bool simulated_offline =
        s_have_state && s_state.simulated_offline;
    const bool offline_pending = s_offline_test_pending;
    nuwax_test_panel_set_network(simulated_offline, offline_pending,
                                 s_offline_test_pending_target);
#endif
    s_menu_dirty = false;
}

static void render_info(void) {
    if (!s_info_id || !s_info_version || !s_info_ssid || !s_info_ip) return;
    const esp_app_desc_t *app = esp_app_get_description();
    const char *version = app && app->version[0] ? app->version : "--";
    const char *id = nuwax_device_uid_string()[0] ? nuwax_device_uid_string() : "--";
    lv_point_t extent;
    lv_text_get_size(&extent, id, &nuwax_font_lingzhu_22, 0, 0, 1000, LV_TEXT_FLAG_NONE);
    lv_obj_set_style_text_font(s_info_id, extent.x > 222 ? &nuwax_font_lingzhu_17 : &nuwax_font_lingzhu_22, 0);
    lv_label_set_text(s_info_id, id);
    /* Short versions use the reference's prominent inline value. Long custom
     * versions retain every character on a second row at a smaller size. */
    lv_text_get_size(&extent, version, &nuwax_font_lingzhu_22, 0, 0, 1000, LV_TEXT_FLAG_NONE);
    const bool short_version = extent.x <= 110;
    lv_obj_set_pos(s_info_version_title, 14, short_version ? 15 : 5);
    lv_obj_set_pos(s_info_version, short_version ? 126 : 14, short_version ? 11 : 25);
    lv_obj_set_width(s_info_version, short_version ? 110 : 222);
    lv_obj_set_style_text_align(s_info_version, short_version ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
    lv_text_get_size(&extent, version, &nuwax_font_lingzhu_14, 0, 0, 1000, LV_TEXT_FLAG_NONE);
    lv_obj_set_style_text_font(s_info_version, short_version ? &nuwax_font_lingzhu_22 :
        extent.x > 222 ? &lv_font_montserrat_10 : &nuwax_font_lingzhu_14, 0);
    lv_label_set_text(s_info_version, version);
    const char *ssid = s_have_state && s_state.ssid[0] ? s_state.ssid : "未连接";
    bool ascii = true;
    for (const unsigned char *c = (const unsigned char *)ssid; *c; ++c) if (*c >= 128) ascii = false;
    lv_text_get_size(&extent, ssid, &nuwax_font_lingzhu_17, 0, 0, 1000, LV_TEXT_FLAG_NONE);
    lv_obj_set_style_text_font(s_info_ssid, ascii && extent.x <= 222 ?
        &nuwax_font_lingzhu_17 : &nuwax_font_lingzhu_14, 0);
    lv_label_set_text(s_info_ssid, ssid);
    lv_label_set_text_fmt(s_info_ip, "IP  %s", s_have_state && s_state.ip[0] ? s_state.ip : "--");
    s_info_dirty = false;
}

static void render_status(void) {
    if (!s_have_state || !s_status_cards[0].value) {
        return;
    }
    const bool wifi_offline = s_state.wifi == NUWAX_WIFI_DISCONNECTED;
    const bool cloud_offline = s_state.cloud == NUWAX_CLOUD_DISCONNECTED ||
                               s_state.cloud == NUWAX_CLOUD_AUTHFAIL;
    status_card_set_icon(&s_status_cards[0], wifi_icon(&s_state),
                         lv_color_hex(wifi_offline ? 0x707070 : 0xffffff));
    status_card_set_icon(&s_status_cards[1], NUWAX_CC_ICON_CLOUD,
                         lv_color_hex(cloud_offline ? 0x707070 : 0xffffff));
    status_card_set_icon(&s_status_cards[2], NUWAX_CC_ICON_BATTERY,
                         lv_color_hex(s_state.power == NUWAX_POWER_CHARGING
                                          ? 0xffffff
                                          : 0xffffff));
    lv_color_t dialog_color = lv_color_hex(0xffffff);
    if (s_state.monitoring != NUWAX_MONITORING_UI_DISABLED) {
        dialog_color = lv_color_hex(0xffffff);
    } else if (s_state.meeting != NUWAX_MEETING_UI_DISABLED) {
        dialog_color = lv_color_hex(0xffffff);
    } else if (s_state.provisioning_active) {
        dialog_color = lv_color_hex(0xffffff);
    } else {
        switch (s_state.dialog) {
            case NUWAX_DIALOG_LISTENING:
            case NUWAX_DIALOG_RECOGNIZING:
                dialog_color = lv_color_hex(0xffffff);
                break;
            case NUWAX_DIALOG_THINKING:
                dialog_color = lv_color_hex(0xffffff);
                break;
            case NUWAX_DIALOG_SPEAKING:
                dialog_color = lv_color_hex(0xffffff);
                break;
            case NUWAX_DIALOG_SLEEP:
                dialog_color = lv_color_hex(0xffffff);
                break;
            case NUWAX_DIALOG_ERROR:
                dialog_color = lv_color_hex(0xffffff);
                break;
            case NUWAX_DIALOG_READY:
            default:
                break;
        }
    }
    status_card_set_icon(&s_status_cards[3], dialog_icon(&s_state), dialog_color);
    if (s_status_cards[0].offline_mark) {
        if (wifi_offline) lv_obj_remove_flag(s_status_cards[0].offline_mark,
                                              LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_status_cards[0].offline_mark, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_status_cards[1].offline_mark) {
        if (cloud_offline) lv_obj_remove_flag(s_status_cards[1].offline_mark,
                                               LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_status_cards[1].offline_mark, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(s_status_cards[0].value, wifi_text(&s_state));
    lv_label_set_text(s_status_cards[1].value, cloud_text(&s_state));
    char power[24] = {0};
    format_power(power, sizeof(power), &s_state);
    lv_label_set_text(s_status_cards[2].value, power);
    if (s_battery_charging_mark) {
        if (s_state.power == NUWAX_POWER_CHARGING)
            lv_obj_remove_flag(s_battery_charging_mark, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(s_battery_charging_mark, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(s_status_cards[3].value, dialog_text(&s_state));
    if (s_battery_fill) {
        const uint8_t percent = s_state.battery_valid
                                    ? (s_state.battery_percent > 100U
                                           ? 100U
                                           : s_state.battery_percent)
                                    : 100U;
        lv_obj_set_width(s_battery_fill, (35 * percent + 50) / 100);
        lv_obj_set_style_bg_color(
            s_battery_fill,
            lv_color_hex(s_state.power == NUWAX_POWER_CHARGING
                             ? 0xffffff
                             : 0xffffff), 0);
    }
    if (s_footer_label && s_footer_dot) {
        const bool low_battery = s_state.battery_valid &&
                                 s_state.battery_percent <= 15U &&
                                 s_state.power != NUWAX_POWER_CHARGING;
        const bool connecting = !wifi_offline && !cloud_offline &&
                                (s_state.wifi == NUWAX_WIFI_CONNECTING ||
                                 s_state.cloud == NUWAX_CLOUD_CONNECTING);
        if (wifi_offline || cloud_offline || low_battery) {
            lv_label_set_text(s_footer_label,
                              low_battery && !wifi_offline && !cloud_offline
                                  ? "设备电量偏低"
                                  : "部分服务不可用");
            lv_image_set_src(s_footer_dot, nuwax_control_center_icon_get(NUWAX_CC_ICON_WARNING));
        } else if (connecting) {
            lv_label_set_text(s_footer_label, "正在建立连接");
            lv_image_set_src(s_footer_dot, nuwax_control_center_icon_get(NUWAX_CC_ICON_WARNING));
        } else {
            lv_label_set_text(s_footer_label, "设备运行正常");
            lv_image_set_src(s_footer_dot, nuwax_control_center_icon_get(NUWAX_CC_ICON_CHECK));
        }
    }
    start_local_animations();
    s_status_dirty = false;
    transition_cache_mark_page_dirty(CC_PAGE_STATUS);
}

static int32_t backdrop_radius(int32_t progress);
static int32_t circle_half_width(int32_t radius, int32_t distance_y);
static bool suspend_display_invalidation(lv_obj_t *obj);
static void resume_display_invalidation(lv_obj_t *obj, bool suspended);
static void update_reveal_mask_rows(int32_t progress);
static void transition_cache_compose(int32_t x, int32_t y, int32_t width,
                                     int32_t height, uint16_t *pixels,
                                     size_t stride_pixels);
static void apply_reveal_progress(int32_t progress,
                                  bool quantize,
                                  bool force_content);
static bool transition_cache_page_valid(cc_page_t page);
static bool transition_cache_prepare_page(cc_page_t page);

static void render_page_if_dirty(cc_page_t page) {
    if (page == CC_PAGE_STATUS) {
        if (s_status_dirty) render_status();
    } else if (s_menu_dirty) {
        render_menu();
    }
}

static void pending_timer_cb(lv_timer_t *timer) {
    (void)timer;
    const int64_t now_us = esp_timer_get_time();
    report_motion_cost(now_us);
    if (s_open && !s_closing && s_have_state &&
        s_state.wifi == NUWAX_WIFI_CONNECTING) {
        if (s_wifi_connect_last_us == 0 ||
            now_us - s_wifi_connect_last_us >= 500000LL) {
            s_wifi_connect_last_us = now_us;
            s_wifi_connect_phase = (uint8_t)((s_wifi_connect_phase + 1U) % 4U);
            s_status_dirty = true;
            if (s_page == CC_PAGE_STATUS || s_page_transition_active) {
                render_status();
            }
        }
    } else {
        s_wifi_connect_phase = 0;
        s_wifi_connect_last_us = 0;
    }
    bool pending_changed = false;
    if (s_meeting_pending && now_us >= s_pending_deadline_us) {
        ESP_LOGW(TAG, "control-center meeting request visual timeout");
        s_meeting_pending = false;
        s_pending_deadline_us = 0;
        pending_changed = true;
    }
    if (s_provision_pending &&
        now_us >= s_provision_pending_deadline_us) {
        ESP_LOGW(TAG, "control-center provisioning request visual timeout");
        s_provision_pending = false;
        s_provision_pending_deadline_us = 0;
        pending_changed = true;
    }
#if NUWAX_TEST_NETWORK_ENABLED
    if (s_offline_test_pending &&
        now_us >= s_offline_test_pending_deadline_us) {
        ESP_LOGW(TAG, "control-center simulated-offline request visual timeout");
        s_offline_test_pending = false;
        s_offline_test_pending_deadline_us = 0;
        pending_changed = true;
    }
#endif
    if (pending_changed) {
        s_menu_dirty = true;
        if (s_page != CC_PAGE_STATUS || s_page_transition_active) {
            render_menu();
        }
    }
    const bool cache_pending = s_status_dirty || s_menu_dirty ||
        s_status_cache_dirty || s_menu_cache_dirty;
    lv_timer_set_period(timer, cache_pending ? 25U : 250U);
    navigation_cache_tick();
    /* Snapshot one component per idle tick. A page generation is committed
     * only after every component has been captured from the same state. */
    if (s_have_state && s_transition_cache_available && !s_transition_cache_active &&
        !nuwax_emotion_animation_player_transition_active() &&
        !s_sheet_active && !s_navigation_cached && !s_page_transition_active &&
        !s_click_timer && !s_pressed && !s_dragging && !s_closing &&
        s_detail == CC_DETAIL_NONE && (!s_open || s_reveal_raw == 1000)) {
        if (s_status_dirty || s_status_cache_dirty ||
            !transition_cache_page_valid(CC_PAGE_STATUS)) {
            (void)transition_cache_prepare_page(CC_PAGE_STATUS);
            stop_local_animations();
        } else if (s_menu_dirty || s_menu_cache_dirty ||
                   !transition_cache_page_valid(CC_PAGE_MENU)) {
            (void)transition_cache_prepare_page(CC_PAGE_MENU);
        }
    }
}

static lv_opa_t opacity_from_permille(int32_t value);
static void apply_reveal_progress(int32_t progress,
                                  bool quantize,
                                  bool force_content);

static int32_t integer_sqrt(int32_t value) {
    if (value <= 0) return 0;
    int32_t low = 1;
    int32_t high = CC_REVEAL_MAX_DIAMETER / 2;
    int32_t root = 0;
    while (low <= high) {
        const int32_t mid = low + (high - low) / 2;
        const int32_t square = mid * mid;
        if (square <= value) {
            root = mid;
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }
    return root;
}

static int32_t backdrop_radius(int32_t progress) {
    if (progress <= 0) return 0;
    if (progress >= 1000) return CC_REVEAL_MAX_DIAMETER / 2;
    return ((CC_REVEAL_MAX_DIAMETER * progress + 500) / 1000) / 2;
}

/* Row-merged annulus invalidation replaced the band bounding boxes that were
 * this helper's last caller; retained for the Q4 mask regression tooling. */
__attribute__((unused)) static int32_t circle_half_width(int32_t radius, int32_t distance_y) {
    if (radius <= 0 || distance_y >= radius) return 0;
    return integer_sqrt(radius * radius - distance_y * distance_y);
}

static uint32_t integer_sqrt_u32(uint32_t value) {
    uint32_t root = 0U;
    uint32_t bit = 1UL << 30;
    while (bit > value) bit >>= 2;
    while (bit != 0U) {
        if (value >= root + bit) {
            value -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return root;
}

static void update_reveal_mask_rows(int32_t progress) {
    if (progress < 0) progress = 0;
    if (progress > 1000) progress = 1000;
    const int32_t center_x_q4 = CC_W * 8;
    const int32_t center_y_q4 = CC_H * 8;
    const int32_t radius_q4 =
        (CC_REVEAL_RADIUS_Q4 * progress + 500) / 1000;
    const uint32_t radius_squared =
        (uint32_t)(radius_q4 * radius_q4);
    for (int32_t y = 0; y < CC_H; ++y) {
        const int32_t dy_q4 = y * 16 + 8 - center_y_q4;
        const uint32_t dy_squared = (uint32_t)(dy_q4 * dy_q4);
        if (radius_q4 <= 0 || dy_squared >= radius_squared) {
            s_reveal_mask_rows[y].left_q4 = (int16_t)center_x_q4;
            s_reveal_mask_rows[y].right_q4 = (int16_t)center_x_q4;
            continue;
        }
        const int32_t half_width_q4 = (int32_t)integer_sqrt_u32(
            radius_squared - dy_squared);
        s_reveal_mask_rows[y].left_q4 =
            (int16_t)(center_x_q4 - half_width_q4);
        s_reveal_mask_rows[y].right_q4 =
            (int16_t)(center_x_q4 + half_width_q4);
    }
}

static uint16_t blend_rgb565_q4(uint16_t background,
                                uint16_t foreground,
                                int32_t alpha_q4) {
    if (alpha_q4 <= 0) return background;
    if (alpha_q4 >= 16) return foreground;
    const int32_t inverse = 16 - alpha_q4;
    const uint16_t red = (uint16_t)((((background >> 11) & 0x1fU) * inverse +
                                      ((foreground >> 11) & 0x1fU) * alpha_q4 +
                                      8) >> 4);
    const uint16_t green = (uint16_t)((((background >> 5) & 0x3fU) * inverse +
                                        ((foreground >> 5) & 0x3fU) * alpha_q4 +
                                        8) >> 4);
    const uint16_t blue = (uint16_t)(((background & 0x1fU) * inverse +
                                       (foreground & 0x1fU) * alpha_q4 +
                                       8) >> 4);
    return (uint16_t)((red << 11) | (green << 5) | blue);
}

static uint16_t blend_rgb565_u8(uint16_t background,
                                uint16_t foreground,
                                uint8_t opacity) {
    /* Most pixels in the monochrome cards are black over black. This is exact
     * for every alpha and avoids six channel multiplies for those pixels. */
    if (background == foreground) return background;
    if (opacity == 0U) return background;
    if (opacity == LV_OPA_COVER) return foreground;
    const uint32_t alpha = (uint32_t)opacity + ((uint32_t)opacity >> 7);
    const uint32_t inverse = 256U - alpha;
    const uint16_t red = (uint16_t)((((background >> 11) & 0x1fU) * inverse +
                                      ((foreground >> 11) & 0x1fU) * alpha +
                                      128U) >> 8);
    const uint16_t green = (uint16_t)((((background >> 5) & 0x3fU) * inverse +
                                        ((foreground >> 5) & 0x3fU) * alpha +
                                        128U) >> 8);
    const uint16_t blue = (uint16_t)(((background & 0x1fU) * inverse +
                                       (foreground & 0x1fU) * alpha +
                                       128U) >> 8);
    return (uint16_t)((red << 11) | (green << 5) | blue);
}

static size_t align_size(size_t value, size_t alignment) {
    return (value + alignment - 1U) & ~(alignment - 1U);
}

static void pager_release_sprites(void) {
    for (unsigned i = 0; i < CC_PAGER_SPRITES; ++i)
        lv_image_cache_drop(&s_pager_sprites[i].buffer);
    /* Storage is borrowed from the pending bank, never individually freed. */
    memset(s_pager_sprites, 0, sizeof(s_pager_sprites));
    s_pager_staging_lease = false;
}

static void transition_cache_release(void) {
    pager_release_sprites();
    if (s_transition_cache_arena) {
        heap_caps_free(s_transition_cache_arena);
    }
    lv_image_cache_drop(&s_pager_frame_buf);
    heap_caps_free(s_pager_frame_storage);
    s_pager_frame_storage = NULL;
    s_pager_frame_valid = false;
    s_pager_cache_valid = false;
    s_transition_cache_bank_size = 0U;
    memset(s_cache_dirty_since_us, 0, sizeof(s_cache_dirty_since_us));
    memset(s_transition_cache, 0, sizeof(s_transition_cache));
    s_transition_cache_arena = NULL;
    s_transition_cache_arena_size = 0U;
    s_transition_cache_available = false;
    s_transition_cache_active = false;
    s_transition_cache_build_active = false;
    s_transition_cache_build_next = 0U;
    s_transition_cache_build_components = 0U;
    s_transition_cache_build_mask = 0U;
    s_transition_cache_build_work_us = 0U;
    s_transition_cache_build_max_component_us = 0U;
    s_transition_cache_build_started_us = 0;
}

static bool transition_cache_bind_sources(void) {
    lv_obj_t *sources[CC_TRANSITION_CACHE_COMPONENTS] = {
        s_status_header,
        s_status_cards[0].root,
        s_status_cards[1].root,
        s_status_cards[2].root,
        s_status_cards[3].root,
        s_footer,
        s_menu_header,
        s_menu_cards[0],
        s_menu_cards[1],
        s_menu_cards[2],
        s_menu_cards[3],
        s_menu_volume,
    };
    size_t total_size = 0U;
    size_t pending_bytes[2] = {0};
    for (size_t i = 0; i < CC_TRANSITION_CACHE_COMPONENTS; ++i) {
        if (!sources[i]) return false;
        lv_obj_update_layout(sources[i]);
        /* These cached roots deliberately use no shadow, outline, transform,
         * or other external drawing. Keep the cache on LVGL's public API;
         * snapshot reshape will fail closed if a future style adds extents. */
        const int32_t ext = 0;
        const int32_t width = lv_obj_get_width(sources[i]) + ext * 2;
        const int32_t height = lv_obj_get_height(sources[i]) + ext * 2;
        if (width <= 0 || height <= 0 || ext > INT16_MAX) return false;
        const uint32_t stride = lv_draw_buf_width_to_stride(
            (uint32_t)width, LV_COLOR_FORMAT_RGB565);
        const size_t storage_size = (size_t)stride * (size_t)height;
        if (storage_size == 0U || storage_size > UINT32_MAX) return false;
        total_size = align_size(total_size, CC_TRANSITION_CACHE_ALIGNMENT);
        if (SIZE_MAX - total_size < storage_size) return false;
        total_size += storage_size;
        s_transition_cache[i].source = sources[i];
        s_transition_cache[i].storage_size = (uint32_t)storage_size;
        s_transition_cache[i].ext_draw_size = (int16_t)ext;
        s_transition_cache[i].page =
            i < CC_STATUS_CACHE_COMPONENTS ? CC_PAGE_STATUS : CC_PAGE_MENU;
        const unsigned page = s_transition_cache[i].page;
        pending_bytes[page] = align_size(pending_bytes[page], CC_TRANSITION_CACHE_ALIGNMENT);
        s_transition_cache[i].staging_offset = pending_bytes[page];
        pending_bytes[page] += storage_size;
    }
    total_size = align_size(total_size, CC_TRANSITION_CACHE_ALIGNMENT);
    s_transition_cache_bank_size = total_size;
    /* Only one page is built at a time. Both pages share one pending bank. */
    total_size += align_size(pending_bytes[0] > pending_bytes[1]
        ? pending_bytes[0] : pending_bytes[1], CC_TRANSITION_CACHE_ALIGNMENT);

#if CONFIG_SPIRAM
    const size_t psram_free = heap_caps_get_free_size(
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t psram_largest = heap_caps_get_largest_free_block(
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (psram_free < total_size + CC_PSRAM_RUNTIME_RESERVE_BYTES ||
        psram_largest < total_size) {
        ESP_LOGW(TAG,
                 "transition cache disabled psram=%u/%u need=%u reserve=%u",
                 (unsigned)psram_free, (unsigned)psram_largest,
                 (unsigned)total_size,
                 (unsigned)CC_PSRAM_RUNTIME_RESERVE_BYTES);
        memset(s_transition_cache, 0, sizeof(s_transition_cache));
        return false;
    }
    s_transition_cache_arena = heap_caps_aligned_alloc(
        CC_TRANSITION_CACHE_ALIGNMENT, total_size,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    s_transition_cache_arena = NULL;
#endif
    if (!s_transition_cache_arena) {
        ESP_LOGW(TAG, "transition cache allocation failed bytes=%u",
                 (unsigned)total_size);
        memset(s_transition_cache, 0, sizeof(s_transition_cache));
        return false;
    }

    size_t offset = 0U;
    for (size_t i = 0; i < CC_TRANSITION_CACHE_COMPONENTS; ++i) {
        cc_transition_cache_component_t *component = &s_transition_cache[i];
        offset = align_size(offset, CC_TRANSITION_CACHE_ALIGNMENT);
        component->storage = s_transition_cache_arena + offset;
        const int32_t ext = component->ext_draw_size;
        const uint32_t width =
            (uint32_t)(lv_obj_get_width(component->source) + ext * 2);
        const uint32_t height =
            (uint32_t)(lv_obj_get_height(component->source) + ext * 2);
        const uint32_t stride = lv_draw_buf_width_to_stride(
            width, LV_COLOR_FORMAT_RGB565);
        if (lv_draw_buf_init(&component->draw_buf, width, height,
                             LV_COLOR_FORMAT_RGB565, stride,
                             component->storage,
                             component->storage_size) != LV_RESULT_OK) {
            ESP_LOGW(TAG,
                     "transition cache draw-buffer init failed index=%u",
                     (unsigned)i);
            transition_cache_release();
            return false;
        }
        offset += component->storage_size;
    }
    s_transition_cache_arena_size = total_size;
    const uint32_t pager_stride = lv_draw_buf_width_to_stride(CC_PAGER_W, LV_COLOR_FORMAT_RGB565);
    const size_t pager_bytes = pager_stride * CC_PAGER_H;
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) >=
        pager_bytes + CC_PSRAM_RUNTIME_RESERVE_BYTES) {
        s_pager_frame_storage = heap_caps_aligned_alloc(CC_TRANSITION_CACHE_ALIGNMENT,
            pager_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_pager_frame_storage && lv_draw_buf_init(&s_pager_frame_buf,
                CC_PAGER_W, CC_PAGER_H, LV_COLOR_FORMAT_RGB565, pager_stride,
                s_pager_frame_storage, pager_bytes) != LV_RESULT_OK) {
            heap_caps_free(s_pager_frame_storage);
            s_pager_frame_storage = NULL;
        }
    }
    s_transition_cache_available = true;
    s_status_cache_dirty = true;
    s_menu_cache_dirty = true;
    ESP_LOGI(TAG,
             "transition cache ready components=%u psram_bytes=%u remaining=%u component_dirty=1 volume_settle_ms=150",
             (unsigned)CC_TRANSITION_CACHE_COMPONENTS,
             (unsigned)total_size,
             (unsigned)heap_caps_get_free_size(
                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    return true;
}

static uint32_t transition_cache_page_generation(cc_page_t page) {
    return page == CC_PAGE_STATUS
               ? s_status_cache_generation
               : s_menu_cache_generation;
}

static void transition_cache_begin_build(cc_page_t page) {
    s_transition_cache_build_active = true;
    s_transition_cache_build_page = page;
    s_transition_cache_build_generation =
        transition_cache_page_generation(page);
    s_transition_cache_build_next = 0U;
    s_transition_cache_build_components = 0U;
    s_transition_cache_build_mask = 0U;
    for (size_t i = 0; i < CC_TRANSITION_CACHE_COMPONENTS; ++i) {
        if (s_transition_cache[i].page == page &&
            (!s_transition_cache[i].valid || s_transition_cache[i].dirty)) {
            s_transition_cache_build_mask |= 1U << i;
        }
    }
    s_transition_cache_build_work_us = 0U;
    s_transition_cache_build_max_component_us = 0U;
    s_transition_cache_build_started_us = esp_timer_get_time();
}

static void copy_rgb565_span(uint16_t *destination, const uint16_t *source, size_t count) {
    if (!count) return;
    if (((uintptr_t)destination & 3U) && count) { *destination++ = *source++; --count; }
    uint8_t *dst = (uint8_t *)__builtin_assume_aligned(destination, 4);
    const size_t pairs = count / 2U;
    if (((uintptr_t)source & 3U) == 0U) {
        const uint8_t *src = (const uint8_t *)__builtin_assume_aligned(source, 4);
        for (size_t i = 0; i < pairs; ++i) {
            uint32_t word;
            __builtin_memcpy(&word, src + i * 4U, 4);
            __builtin_memcpy(dst + i * 4U, &word, 4);
        }
    } else {
        for (size_t i = 0; i < pairs; ++i) {
            const uint32_t word = source[i * 2U] | ((uint32_t)source[i * 2U + 1U] << 16);
            __builtin_memcpy(dst + i * 4U, &word, 4);
        }
    }
    if (count & 1U) destination[count - 1U] = source[count - 1U];
}

static bool transition_cache_capture_page(cc_page_t page) {
    if (!s_transition_cache_available || !s_surface || s_pager_staging_lease) return false;
    const uint32_t generation = transition_cache_page_generation(page);
    if (!s_transition_cache_build_active ||
        s_transition_cache_build_page != page ||
        s_transition_cache_build_generation != generation) {
        transition_cache_begin_build(page);
    }

    size_t component_index = s_transition_cache_build_next;
    while (component_index < CC_TRANSITION_CACHE_COMPONENTS &&
           !(s_transition_cache_build_mask & (1U << component_index))) {
        component_index++;
    }
    if (component_index >= CC_TRANSITION_CACHE_COMPONENTS) {
        s_transition_cache_build_active = false;
        return false;
    }

    cc_transition_cache_component_t *component =
        &s_transition_cache[component_index];
    const int64_t started_us = esp_timer_get_time();
    const bool invalidation_suspended =
        suspend_display_invalidation(s_surface);
    const lv_opa_t previous_opa = lv_obj_get_style_opa(
        component->source, LV_PART_MAIN);
    lv_obj_set_style_opa(component->source, LV_OPA_COVER, 0);
    lv_draw_buf_t pending;
    const bool captured = lv_draw_buf_init(&pending,
            component->draw_buf.header.w, component->draw_buf.header.h,
            LV_COLOR_FORMAT_RGB565, component->draw_buf.header.stride,
            s_transition_cache_arena + s_transition_cache_bank_size + component->staging_offset,
            component->storage_size) == LV_RESULT_OK &&
        lv_snapshot_take_to_draw_buf(component->source,
                                     LV_COLOR_FORMAT_RGB565,
                                     &pending) == LV_RESULT_OK;
    lv_obj_set_style_opa(component->source, previous_opa, 0);
    resume_display_invalidation(s_surface, invalidation_suspended);
    if (!captured) {
        if (!s_transition_cache_failure_logged) {
            s_transition_cache_failure_logged = true;
            ESP_LOGW(TAG,
                     "transition cache capture failed page=%s; using live LVGL fallback",
                     page == CC_PAGE_STATUS ? "status" : "menu");
        }
        transition_cache_release();
        return false;
    }

    const uint32_t component_us = (uint32_t)(esp_timer_get_time() - started_us);
    s_transition_cache_build_work_us += component_us;
    if (component_us > s_transition_cache_build_max_component_us) {
        s_transition_cache_build_max_component_us = component_us;
    }
    s_transition_cache_build_components++;
    s_transition_cache_build_next = component_index + 1U;

    if (s_transition_cache_build_generation !=
        transition_cache_page_generation(page)) {
        transition_cache_begin_build(page);
        return false;
    }

    size_t next = s_transition_cache_build_next;
    while (next < CC_TRANSITION_CACHE_COMPONENTS &&
           !(s_transition_cache_build_mask & (1U << next))) {
        next++;
    }
    if (next < CC_TRANSITION_CACHE_COMPONENTS) {
        s_transition_cache_build_next = next;
        return false;
    }

    /* Commit under the LVGL owner, only after all tiles have the same state
     * generation. Readers never see a half-built page or a reclaimed buffer. */
    for (size_t i = 0; i < CC_TRANSITION_CACHE_COMPONENTS; ++i) {
        cc_transition_cache_component_t *tile = &s_transition_cache[i];
        if (!(s_transition_cache_build_mask & (1U << i))) continue;
        copy_rgb565_span((uint16_t *)tile->storage,
            (const uint16_t *)(s_transition_cache_arena + s_transition_cache_bank_size + tile->staging_offset),
            tile->storage_size / sizeof(uint16_t));
        tile->valid = true;
        tile->dirty = false;
    }
    s_cache_dirty_since_us[page] = 0;
    s_transition_cache_build_active = false;
    if (page == CC_PAGE_STATUS) {
        s_status_cache_dirty = false;
    } else {
        s_menu_cache_dirty = false;
    }
    ESP_LOGI(TAG,
             "transition cache refreshed page=%s generation=%lu components=%lu work_ms=%lu wall_ms=%lu max_component_ms=%lu",
             page == CC_PAGE_STATUS ? "status" : "menu",
             (unsigned long)generation,
             (unsigned long)s_transition_cache_build_components,
             (unsigned long)(s_transition_cache_build_work_us / 1000U),
             (unsigned long)((esp_timer_get_time() -
                              s_transition_cache_build_started_us) /
                             1000LL),
             (unsigned long)(s_transition_cache_build_max_component_us /
                             1000U));
    return true;
}



static void transition_cache_compose_component(
    const cc_transition_cache_component_t *component,
    int32_t flush_x,
    int32_t flush_y,
    int32_t flush_width,
    int32_t flush_height,
    uint16_t *pixels,
    size_t stride_pixels) {
    if (!component || !component->valid || !pixels) return;
    const lv_opa_t opacity = component->frame_opa;
    if (opacity <= LV_OPA_MIN) return;
    const lv_area_t target = component->frame_area;
    int32_t x1 = target.x1 > flush_x ? target.x1 : flush_x;
    int32_t y1 = target.y1 > flush_y ? target.y1 : flush_y;
    const int32_t flush_x2 = flush_x + flush_width - 1;
    const int32_t flush_y2 = flush_y + flush_height - 1;
    int32_t x2 = target.x2 < flush_x2 ? target.x2 : flush_x2;
    int32_t y2 = target.y2 < flush_y2 ? target.y2 : flush_y2;
    if (x1 > x2 || y1 > y2) return;

    const size_t source_stride = component->draw_buf.header.stride /
                                 sizeof(uint16_t);
    const uint16_t *source_pixels =
        (const uint16_t *)component->draw_buf.data;
    for (int32_t global_y = y1; global_y <= y2; ++global_y) {
        if (global_y < 0 || global_y >= CC_H) continue;
        /* The output mask is disabled at full reveal. Match that contract
         * while horizontal motion temporarily crosses the circular edge. */
        const int32_t left_q4 = s_backdrop_progress >= 1000 ? 0 :
            s_reveal_mask_rows[global_y].left_q4;
        const int32_t right_q4 = s_backdrop_progress >= 1000 ? CC_W * 16 :
            s_reveal_mask_rows[global_y].right_q4;
        if (right_q4 <= left_q4) continue;
        const int32_t circle_x1 = left_q4 <= 0 ? 0 : left_q4 >> 4;
        const int32_t circle_x2 = right_q4 >= CC_W * 16
                                      ? CC_W - 1
                                      : (right_q4 - 1) >> 4;
        const int32_t row_x1 = x1 > circle_x1 ? x1 : circle_x1;
        const int32_t row_x2 = x2 < circle_x2 ? x2 : circle_x2;
        if (row_x1 > row_x2) continue;
        const size_t source_y = (size_t)(global_y - target.y1);
        const size_t source_x = (size_t)(row_x1 - target.x1);
        const size_t target_y = (size_t)(global_y - flush_y);
        const size_t target_x = (size_t)(row_x1 - flush_x);
        const size_t count = (size_t)(row_x2 - row_x1 + 1);
        const uint16_t *source = source_pixels +
            source_y * source_stride + source_x;
        uint16_t *destination = pixels +
            target_y * stride_pixels + target_x;
        if (opacity == LV_OPA_COVER) {
            copy_rgb565_span(destination, source, count);
        } else {
            for (size_t x = 0; x < count; ++x) {
                destination[x] = blend_rgb565_u8(
                    destination[x], source[x], opacity);
            }
        }
    }
}

static void transition_cache_compose(int32_t x,
                                     int32_t y,
                                     int32_t width,
                                     int32_t height,
                                     uint16_t *pixels,
                                     size_t stride_pixels) {
    /* Detail views are live LVGL foreground trees. Compositing a cached page
     * after LVGL has drawn one would bypass sibling z-order and cover the
     * provisioning QR or device information with status/menu components. */
    if (!s_transition_cache_active || s_backdrop_progress <= 0 ||
        (s_detail != CC_DETAIL_NONE && !s_navigation_cached)) {
        return;
    }
    if (!s_cache_plan_valid) {
        for (size_t i = 0; i < CC_TRANSITION_CACHE_COMPONENTS; ++i) {
            cc_transition_cache_component_t *c = &s_transition_cache[i];
            c->frame_opa = c->valid && c->source && !lv_obj_has_flag(c->source, LV_OBJ_FLAG_HIDDEN)
                ? lv_obj_get_style_opa(c->source, LV_PART_MAIN) : LV_OPA_TRANSP;
            if (c->frame_opa > LV_OPA_MIN) {
                lv_obj_get_coords(c->source, &c->frame_area);
                lv_area_increase(&c->frame_area, c->ext_draw_size, c->ext_draw_size);
            }
        }
        s_cache_plan_valid = true;
    }
    for (size_t i = 0; i < CC_TRANSITION_CACHE_COMPONENTS; ++i) {
        transition_cache_compose_component(&s_transition_cache[i],
                                           x, y, width, height,
                                           pixels, stride_pixels);
    }
}

static void copy_frozen_span(uint16_t *target,
                             size_t target_stride,
                             int32_t target_x,
                             int32_t local_y,
                             int32_t global_y,
                             int32_t x1,
                             int32_t x2,
                             const uint16_t *frozen,
                             size_t frozen_stride) {
    if (x1 > x2) return;
    copy_rgb565_span(target + (size_t)local_y * target_stride +
               (size_t)(x1 - target_x),
           frozen + (size_t)global_y * frozen_stride + (size_t)x1,
           (size_t)(x2 - x1 + 1));
}

static void control_center_flush_filter(
    int32_t x, int32_t y, int32_t width, int32_t height,
    uint16_t *pixels, size_t stride_pixels, void *user_data) {
    (void)user_data;
    if (s_ota_blocked || !pixels || width <= 0 || height <= 0 || stride_pixels == 0U) {
        return;
    }
    /* Incoming detail is opaque. Do not copy menu pixels only to overwrite
     * them immediately; retain the original stride for the visible strip. */
    int32_t menu_width = width;
    if (s_qr_reveal_active && s_navigation_cached) {
        menu_width = 0;
    } else if (s_navigation_cached && s_sheet) {
        const int32_t edge = lv_obj_get_x(s_sheet);
        if (edge < x + width) menu_width = edge > x ? edge - x : 0;
    }
    if (menu_width > 0)
        transition_cache_compose(x, y, menu_width, height, pixels, stride_pixels);
    navigation_cache_compose(x, y, width, height, pixels, stride_pixels);
    if (!s_backdrop_frozen_face ||
        (s_backdrop_progress <= 0 && !s_qr_reveal_active) ||
        s_backdrop_progress >= 1000) {
        return;
    }
    size_t frozen_stride = 0U;
    const uint16_t *frozen = nuwax_face_frozen_rgb565(&frozen_stride);
    if (!frozen || frozen_stride < CC_W) return;

    const int32_t clipped_x1 = x < 0 ? 0 : x;
    const int32_t unclipped_x2 = x + width - 1;
    const int32_t clipped_x2 = unclipped_x2 >= CC_W
                                   ? CC_W - 1
                                   : unclipped_x2;
    if (clipped_x1 > clipped_x2) return;

    for (int32_t local_y = 0; local_y < height; ++local_y) {
        const int32_t global_y = y + local_y;
        if (global_y < 0 || global_y >= CC_H) continue;
        const int32_t left_q4 = s_reveal_mask_rows[global_y].left_q4;
        const int32_t right_q4 = s_reveal_mask_rows[global_y].right_q4;
        if (right_q4 <= left_q4 || right_q4 <= 0 ||
            left_q4 >= CC_W * 16) {
            copy_frozen_span(pixels, stride_pixels, x, local_y, global_y,
                             clipped_x1, clipped_x2, frozen, frozen_stride);
            continue;
        }

        const int32_t left_pixel = left_q4 <= 0 ? 0 : left_q4 >> 4;
        const int32_t right_pixel = right_q4 >= CC_W * 16
                                        ? CC_W - 1
                                        : (right_q4 - 1) >> 4;
        copy_frozen_span(pixels, stride_pixels, x, local_y, global_y,
                         clipped_x1,
                         left_pixel - 1 < clipped_x2
                             ? left_pixel - 1
                             : clipped_x2,
                         frozen, frozen_stride);
        copy_frozen_span(pixels, stride_pixels, x, local_y, global_y,
                         right_pixel + 1 > clipped_x1
                             ? right_pixel + 1
                             : clipped_x1,
                         clipped_x2, frozen, frozen_stride);

        if (left_pixel > clipped_x2 || right_pixel < clipped_x1) continue;
        if (left_pixel == right_pixel) {
            if (left_pixel >= clipped_x1 && left_pixel <= clipped_x2) {
                const int32_t pixel_left_q4 = left_pixel << 4;
                int32_t covered_q4 =
                    (right_q4 < pixel_left_q4 + 16
                         ? right_q4
                         : pixel_left_q4 + 16) -
                    (left_q4 > pixel_left_q4 ? left_q4 : pixel_left_q4);
                if (covered_q4 < 0) covered_q4 = 0;
                uint16_t *target = pixels +
                    (size_t)local_y * stride_pixels +
                    (size_t)(left_pixel - x);
                const uint16_t background =
                    frozen[(size_t)global_y * frozen_stride +
                           (size_t)left_pixel];
                *target = blend_rgb565_q4(background, *target, covered_q4);
            }
            continue;
        }

        if (left_q4 > 0 && left_pixel >= clipped_x1 &&
            left_pixel <= clipped_x2) {
            const int32_t alpha_q4 = 16 - (left_q4 & 0x0f);
            uint16_t *target = pixels + (size_t)local_y * stride_pixels +
                               (size_t)(left_pixel - x);
            const uint16_t background =
                frozen[(size_t)global_y * frozen_stride +
                       (size_t)left_pixel];
            *target = blend_rgb565_q4(background, *target, alpha_q4);
        }
        if (right_q4 < CC_W * 16 && right_pixel >= clipped_x1 &&
            right_pixel <= clipped_x2) {
            const int32_t alpha_q4 = right_q4 - (right_pixel << 4);
            uint16_t *target = pixels + (size_t)local_y * stride_pixels +
                               (size_t)(right_pixel - x);
            const uint16_t background =
                frozen[(size_t)global_y * frozen_stride +
                       (size_t)right_pixel];
            *target = blend_rgb565_q4(background, *target, alpha_q4);
        }
    }
}

static uint32_t invalidate_backdrop_band(int32_t x1,
                                         int32_t y1,
                                         int32_t x2,
                                         int32_t y2) {
    if (!s_reveal_clip || x1 > x2 || y1 > y2) return 0;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= CC_W) x2 = CC_W - 1;
    if (y2 >= CC_H) y2 = CC_H - 1;
    if (x1 > x2 || y1 > y2) return 0;
    const lv_area_t area = {
        .x1 = x1,
        .y1 = y1,
        .x2 = x2,
        .y2 = y2,
    };
    (void)lv_obj_invalidate_area(s_reveal_clip, &area);
    return (uint32_t)(x2 - x1 + 1) * (uint32_t)(y2 - y1 + 1);
}

static void update_backdrop_progress(int32_t progress) {
    if (!s_reveal_clip) return;
    if (progress < 0) progress = 0;
    if (progress > 1000) progress = 1000;
    if (progress == s_backdrop_progress) return;

    if (s_qr_reveal_active) {
        /* Damage follows the exact Q4 mask. Integer-radius bounds can miss
         * subpixel edge coverage near a band boundary on the opaque QR page. */
        int16_t bounds[CC_REVEAL_DIRTY_BANDS][4];
        const int32_t rows = (CC_H + CC_REVEAL_DIRTY_BANDS - 1) / CC_REVEAL_DIRTY_BANDS;
        for (unsigned band = 0; band < CC_REVEAL_DIRTY_BANDS; ++band) {
            bounds[band][0] = bounds[band][2] = INT16_MAX;
            bounds[band][1] = bounds[band][3] = INT16_MIN;
        }
        for (unsigned pass = 0; pass < 2; ++pass) {
            for (int32_t y = 0; y < CC_H; ++y) {
                int16_t *b = bounds[y / rows];
                const int16_t left = s_reveal_mask_rows[y].left_q4;
                const int16_t right = s_reveal_mask_rows[y].right_q4;
                if (left < b[0]) b[0] = left;
                if (left > b[1]) b[1] = left;
                if (right < b[2]) b[2] = right;
                if (right > b[3]) b[3] = right;
            }
            if (pass == 0) update_reveal_mask_rows(progress);
        }
        for (unsigned band = 0; band < CC_REVEAL_DIRTY_BANDS; ++band) {
            const int16_t *b = bounds[band];
            const int32_t y1 = (int32_t)band * rows;
            (void)invalidate_backdrop_band((b[0] >> 4) - 1, y1,
                                          (b[1] >> 4) + 1, y1 + rows - 1);
            (void)invalidate_backdrop_band((b[2] >> 4) - 1, y1,
                                          (b[3] >> 4) + 1, y1 + rows - 1);
        }
        s_backdrop_progress = progress;
        return;
    }

    update_reveal_mask_rows(progress);

    const int32_t old_radius = backdrop_radius(s_backdrop_progress);
    const int32_t new_radius = backdrop_radius(progress);
    const int32_t center_x = CC_W / 2;
    const int32_t center_y = CC_H / 2;
    const int32_t band_height =
        (CC_H + CC_REVEAL_DIRTY_BANDS - 1) / CC_REVEAL_DIRTY_BANDS;

    uint32_t invalidated_areas = 0;
    uint32_t invalidated_pixels = 0;
    /* Each band contributes at most two non-overlapping invalid areas. Four
     * broad bands trade bounded annulus overdraw for substantially fewer
     * render/flush traversals; staged content is coalesced into six regions. */
    for (int32_t band = 0; band < CC_REVEAL_DIRTY_BANDS; ++band) {
        const int32_t y1 = band * band_height;
        int32_t y2 = y1 + band_height - 1;
        if (y2 >= CC_H) y2 = CC_H - 1;
        if (y1 >= CC_H) break;

        int32_t near_y = 0;
        if (y2 < center_y) {
            near_y = center_y - y2;
        } else if (y1 > center_y) {
            near_y = y1 - center_y;
        }
        int32_t far_y = center_y - y1;
        if (far_y < 0) far_y = -far_y;
        int32_t opposite = center_y - y2;
        if (opposite < 0) opposite = -opposite;
        if (opposite > far_y) far_y = opposite;

        const int32_t old_near = circle_half_width(old_radius, near_y);
        const int32_t old_far = circle_half_width(old_radius, far_y);
        const int32_t new_near = circle_half_width(new_radius, near_y);
        const int32_t new_far = circle_half_width(new_radius, far_y);
        const bool expanding = new_radius > old_radius;
        const int32_t outer = expanding ? new_near : old_near;
        const int32_t inner = expanding ? old_far : new_far;
        if (outer <= inner) continue;

        /* Include one boundary pixel on both sides. The RGB565 compositor
         * uses Q4 subpixel coverage, while these rectangles use integer
         * geometry; without this pad the antialiased edge can retain one
         * pixel from the preceding frame. */
        uint32_t pixels = invalidate_backdrop_band(center_x - outer - 1, y1,
                                                   center_x - inner, y2);
        if (pixels > 0) {
            invalidated_areas++;
            invalidated_pixels += pixels;
        }
        pixels = invalidate_backdrop_band(center_x + inner - 1, y1,
                                          center_x + outer, y2);
        if (pixels > 0) {
            invalidated_areas++;
            invalidated_pixels += pixels;
        }
    }
    if (s_reveal_perf_active && invalidated_areas > 0) {
        s_reveal_perf_backdrop_frames++;
        s_reveal_perf_backdrop_areas += invalidated_areas;
        s_reveal_perf_backdrop_pixels += invalidated_pixels;
        if (invalidated_pixels > s_reveal_perf_backdrop_max_pixels) {
            s_reveal_perf_backdrop_max_pixels = invalidated_pixels;
        }
    }
    s_backdrop_progress = progress;
}

static void backdrop_cover_cb(lv_event_t *event) {
    if (!s_backdrop_frozen_face || s_backdrop_progress <= 0 ||
        !nuwax_face_frozen_rgb565(NULL)) {
        return;
    }
    const lv_area_t *area = lv_event_get_cover_area(event);
    if (!area || area->x1 < 0 || area->y1 < 0 ||
        area->x2 >= CC_W || area->y2 >= CC_H) {
        return;
    }
    /* The flush compositor restores the frozen face outside the circle. The
     * backdrop can therefore cover the dirty area and skip a second full-face
     * image draw without changing the final pixels. */
    lv_event_set_cover_res(event, LV_COVER_RES_COVER);
    lv_event_stop_processing(event);
}

/* A childless opaque presentation plane lets LVGL's cover search bypass the
 * dormant drawer hierarchy before every partial buffer. The existing flush
 * compositor supplies page/detail pixels and the frozen face outside the circle.
 * Keep the live pager (including its moving selection pill) in the same layer;
 * it is rendered explicitly without reparenting or allocating during motion. */
static bool cache_plane_ready(void) {
    return !s_ota_blocked && s_transition_cache_active && (s_backdrop_progress > 0 || s_qr_reveal_active) &&
           s_backdrop_frozen_face && nuwax_face_frozen_rgb565(NULL) &&
           (s_detail == CC_DETAIL_NONE || s_navigation_cached);
}

/* The scene hook replaces only the known, opaque cached drawer. Other screen
 * children (e.g. OTA overlays) conservatively keep the normal LVGL path. */
static bool s_scene_frame_ready;
static bool s_scene_pager_visible;
static lv_area_t s_scene_pager_area;

static bool cached_scene_geometry_ready(lv_display_t *disp) {
    if (!cache_plane_ready() || !s_cache_plane ||
        lv_obj_has_flag(s_cache_plane, LV_OBJ_FLAG_HIDDEN) ||
        lv_obj_has_flag(s_root, LV_OBJ_FLAG_HIDDEN) ||
        lv_obj_get_child(s_root, -1) != s_cache_plane ||
        lv_display_get_rotation(disp) != LV_DISPLAY_ROTATION_0 ||
        lv_obj_get_parent(s_root) != lv_display_get_screen_active(disp) ||
        lv_obj_get_style_opa(s_root, 0) != LV_OPA_COVER ||
        lv_obj_get_style_opa_layered(s_root, 0) != LV_OPA_COVER ||
        lv_obj_get_style_transform_rotation(s_root, 0) != 0 ||
        lv_obj_get_style_transform_scale_x(s_root, 0) != 256 ||
        lv_obj_get_style_transform_scale_y(s_root, 0) != 256) return false;
    lv_obj_t *screen = lv_obj_get_parent(s_root);
    if (lv_obj_get_style_opa(screen, 0) != LV_OPA_COVER ||
        lv_obj_get_style_opa_layered(screen, 0) != LV_OPA_COVER ||
        lv_obj_get_style_transform_rotation(screen, 0) != 0) return false;
    lv_area_t bounds;
    lv_obj_get_coords(s_cache_plane, &bounds);
    if (bounds.x1 != 0 || bounds.y1 != 0 || bounds.x2 != CC_W-1 || bounds.y2 != CC_H-1) return false;
    return true;
}

static bool cached_scene_eligible(lv_display_t *disp) {
    if (!cached_scene_geometry_ready(disp)) return false;
    lv_obj_t *screen = lv_obj_get_parent(s_root);
    bool above = false;
    for (uint32_t i=0; i<lv_obj_get_child_count(screen); ++i) {
        lv_obj_t *child = lv_obj_get_child(screen, i);
        if (above && !lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) return false;
        if (child == s_root) above = true;
    }
    return true;
}

/* State is stable for the entire refresh under the LVGL owner. Resolve styles
 * and z-order once per frame, not once per 20-line partial buffer. */
/* RENDER_START runs after layout. This known transparent clip has no external
 * drawing and no child covering its full height. Draw it into the existing
 * buffer without a second layout pass or recursive cover search. Use LVGL's
 * normal draw tasks and snapshot layer lifetime; retain the general fallback. */
/* Cache local child pixels independently of parent translation/opacity.
 * The existing layer clips the moving pager, including during reveal. */
static bool pager_prepare_sprites(void) {
    if (!s_transition_cache_active || !s_transition_cache_arena ||
        !s_pager || lv_obj_get_width(s_pager_clip) > CC_PAGER_W ||
        lv_obj_get_height(s_pager_clip) > CC_PAGER_H) return false;
    const unsigned count = lv_obj_get_child_count(s_pager);
    if (count > CC_PAGER_SPRITES) return false;
    /* Motion freezes the committed tiles. Their pending build bank is idle:
     * lease it for pager glyphs instead of consuming the audio heap reserve. */
    size_t required = 0U;
    for (unsigned i = 0; i < count; ++i) {
        lv_obj_t *child = lv_obj_get_child(s_pager, i);
        const int32_t w = lv_obj_get_width(child), h = lv_obj_get_height(child);
        if (w <= 0 || h <= 0 || w > CC_PAGER_W || h > CC_PAGER_H ||
            lv_obj_get_ext_draw_size(child) != 0 ||
            lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) return false;
        required = align_size(required, CC_TRANSITION_CACHE_ALIGNMENT);
        required += (size_t)lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_ARGB8888) * h;
    }
    if (s_transition_cache_arena_size < s_transition_cache_bank_size ||
        required > s_transition_cache_arena_size - s_transition_cache_bank_size) return false;
    if (!s_pager_staging_lease) {
        s_transition_cache_build_active = false; /* A partial build must restart. */
        s_pager_staging_lease = true;
    }
    size_t offset = 0U;
    for (unsigned i = 0; i < count; ++i) {
        lv_obj_t *child = lv_obj_get_child(s_pager, i);
        if (lv_obj_get_ext_draw_size(child) != 0 ||
            lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) return false;
        const int32_t w = lv_obj_get_width(child), h = lv_obj_get_height(child);
        if (w <= 0 || h <= 0 || w > CC_PAGER_W || h > CC_PAGER_H) return false;
        const uint32_t stride = lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_ARGB8888);
        const size_t bytes = (size_t)stride * h;
        offset = align_size(offset, CC_TRANSITION_CACHE_ALIGNMENT);
        if (!s_pager_sprites[i].storage) {
            s_pager_sprites[i].storage = s_transition_cache_arena +
                s_transition_cache_bank_size + offset;
            if (lv_draw_buf_init(&s_pager_sprites[i].buffer, w, h,
                    LV_COLOR_FORMAT_ARGB8888, stride,
                    s_pager_sprites[i].storage, bytes) != LV_RESULT_OK) {
                s_pager_sprites[i].storage = NULL;
                return false;
            }
        }
        if (s_pager_sprites[i].storage != s_transition_cache_arena +
                s_transition_cache_bank_size + offset) return false;
        offset += bytes;
        if (s_pager_sprites[i].buffer.header.w != w ||
            s_pager_sprites[i].buffer.header.h != h) return false;
        /* Pager labels are fixed; only their monochrome color and the pill
         * background change. Positions deliberately do not invalidate pixels. */
        lv_obj_t *label = lv_obj_get_child_count(child) == 1
            ? lv_obj_get_child(child, 0) : NULL;
        const uint32_t key = lv_color_to_u32(lv_obj_get_style_bg_color(child, 0)) ^
            (label ? lv_color_to_u32(lv_obj_get_style_text_color(label, 0)) << 1 : 0);
        if (!s_pager_sprites[i].valid || s_pager_sprites[i].source != child ||
            s_pager_sprites[i].color_key != key) {
            lv_image_cache_drop(&s_pager_sprites[i].buffer);
            if (lv_snapshot_take_to_draw_buf(child, LV_COLOR_FORMAT_ARGB8888,
                    &s_pager_sprites[i].buffer) != LV_RESULT_OK) return false;
            s_pager_sprites[i].source = child;
            s_pager_sprites[i].color_key = key;
            s_pager_sprites[i].valid = true;
            s_motion_cost.sprite_builds++;
        }
    }
    return true;
}

static lv_result_t pager_snapshot_prepared(void) {
    const int32_t w = lv_area_get_width(&s_scene_pager_area);
    const int32_t h = lv_area_get_height(&s_scene_pager_area);
    if (w <= 0 || h <= 0 || w > CC_PAGER_W || h > CC_PAGER_H ||
        lv_obj_get_ext_draw_size(s_pager_clip) != 0 ||
        lv_draw_buf_reshape(&s_pager_frame_buf, LV_COLOR_FORMAT_RGB565,
                            w, h, LV_STRIDE_AUTO) == NULL) {
        s_motion_cost.fallback++;
        return lv_snapshot_take_to_draw_buf(s_pager_clip, LV_COLOR_FORMAT_RGB565,
                                            &s_pager_frame_buf);
    }
    s_motion_cost.prepared++;
    lv_layer_t layer;
    lv_layer_init(&layer);
    layer.draw_buf = &s_pager_frame_buf;
    layer.buf_area = layer._clip_area = layer.phy_clip_area = s_scene_pager_area;
    layer.color_format = LV_COLOR_FORMAT_RGB565;
    lv_draw_buf_clear(&s_pager_frame_buf, NULL);
    lv_draw_unit_send_event(NULL, LV_EVENT_CHILD_CREATED, &layer);
    lv_display_t *display = lv_obj_get_display(s_pager_clip);
    lv_display_t *old_display = lv_refr_get_disp_refreshing();
    lv_layer_t *old_layer = display->layer_head;
    display->layer_head = &layer;
    lv_refr_set_disp_refreshing(display);
    if (pager_prepare_sprites()) {
        s_motion_cost.sprite_frames++;
        for (unsigned i = 0; i < lv_obj_get_child_count(s_pager); ++i) {
            lv_area_t area;
            lv_obj_get_coords(s_pager_sprites[i].source, &area);
            lv_draw_image_dsc_t image;
            lv_draw_image_dsc_init(&image);
            image.src = &s_pager_sprites[i].buffer;
            image.opa = lv_obj_get_style_opa(s_pager, 0);
            lv_draw_image(&layer, &image, &area);
        }
    } else {
        lv_obj_redraw(&layer, s_pager_clip);
    }
    layer.all_tasks_added = true;
    while (layer.draw_task_head) {
        lv_draw_dispatch_wait_for_request();
        lv_draw_dispatch();
    }
    display->layer_head = old_layer;
    lv_refr_set_disp_refreshing(old_display);
    lv_draw_unit_send_event(NULL, LV_EVENT_SCREEN_LOAD_START, &layer);
    lv_draw_unit_send_event(NULL, LV_EVENT_CHILD_DELETED, &layer);
    return LV_RESULT_OK;
}

static void cached_scene_frame_cb(lv_event_t *event) {
    lv_display_t *disp = lv_event_get_target(event);
    if (s_qr_reveal_active) {
        const int64_t now = esp_timer_get_time();
        if (s_qr_render_last_us && now > s_qr_render_last_us) {
            const uint32_t gap = (uint32_t)(now - s_qr_render_last_us);
            if (gap > s_qr_render_gap_us) s_qr_render_gap_us = gap;
        }
        s_qr_render_last_us = now;
        s_qr_render_frames++;
    }
    s_scene_frame_ready = cached_scene_eligible(disp);
    if (s_scene_frame_ready) nuwax_lvgl_port_note_interaction();
    /* Even with a newer screen sibling, the opaque plane may reuse its pager
     * snapshot. Otherwise fallback traverses the pager once per partial strip.
     * Geometry/opacity constraints remain identical to the direct scene path. */
    s_scene_pager_visible = !s_qr_reveal_active && cached_scene_geometry_ready(disp) && s_pager_clip &&
        !lv_obj_has_flag(s_pager_clip, LV_OBJ_FLAG_HIDDEN);
    s_pager_frame_valid = false;
    if (s_scene_pager_visible) {
        lv_obj_get_coords(s_pager_clip, &s_scene_pager_area);
        s_pager_frame_area = s_scene_pager_area;
        const lv_color_t pill = lv_obj_get_style_bg_color(s_tab_indicator, 0);
        int32_t key[12] = {
            lv_obj_get_width(s_pager_clip), lv_obj_get_height(s_pager_clip),
            lv_obj_get_x(s_pager), lv_obj_get_y(s_pager),
            lv_obj_get_style_opa(s_pager, 0), lv_obj_get_x(s_tab_indicator),
            pill.red, pill.green, pill.blue,
            lv_obj_get_style_text_color(s_tab_labels[0], 0).red,
            lv_obj_get_style_text_color(s_tab_labels[1], 0).red,
#if NUWAX_TEST_PANEL_ENABLED
            lv_obj_get_style_text_color(s_tab_labels[2], 0).red
#else
            0
#endif
        };
        if (s_pager_cache_valid && memcmp(key, s_pager_cache_key, sizeof(key)) == 0) {
            s_motion_cost.hits++;
            s_pager_frame_valid = true;
        } else if (s_pager_frame_storage) {
            /* The preceding refresh has drained before RENDER_START. Do not
             * let an image decoder retain a previous snapshot's dimensions. */
            lv_image_cache_drop(&s_pager_frame_buf);
            const int64_t started_us = esp_timer_get_time();
            const lv_result_t result = pager_snapshot_prepared();
            const uint32_t elapsed_us = (uint32_t)(esp_timer_get_time() - started_us);
            s_motion_cost.builds++;
            s_motion_cost.snapshot_us += elapsed_us;
            if (elapsed_us > s_motion_cost.snapshot_max_us) s_motion_cost.snapshot_max_us = elapsed_us;
            if (result == LV_RESULT_OK) {
                memcpy(s_pager_cache_key, key, sizeof(key));
                s_pager_cache_valid = s_pager_frame_valid = true;
            } else {
                s_pager_cache_valid = false;
            }
        } else {
            s_pager_cache_valid = false;
        }
    }
}

static bool cached_scene_render(lv_display_t *disp, lv_layer_t *layer) {
    if (!s_scene_frame_ready)
        return nuwax_emotion_animation_player_render_scene(disp, layer);
    if (!s_scene_frame_ready || layer->opa != LV_OPA_COVER ||
        layer->recolor.alpha != LV_OPA_TRANSP ||
        layer->_clip_area.x1 < 0 || layer->_clip_area.y1 < 0 ||
        layer->_clip_area.x2 >= CC_W || layer->_clip_area.y2 >= CC_H) return false;
    if (!layer->draw_buf || layer->color_format != LV_COLOR_FORMAT_RGB565 ||
        (s_scene_pager_visible && !s_pager_frame_valid)) return false;
    /* The single/double-buffer ownership fence runs before this hook. There
     * are no queued draws for this strip yet. Initialize pixels directly,
     * avoiding a software draw task for every black background strip. */
    const lv_area_t *clip = &layer->_clip_area;
    for (int32_t y = clip->y1; y <= clip->y2; ++y) {
        uint16_t *dst = (uint16_t *)(layer->draw_buf->data +
            (size_t)(y - layer->buf_area.y1) * layer->draw_buf->header.stride) +
            clip->x1 - layer->buf_area.x1;
        memset(dst, 0, (size_t)lv_area_get_width(clip) * sizeof(uint16_t));
    }
    lv_area_t intersection;
    if (s_scene_pager_visible &&
        lv_area_intersect(&intersection, &s_pager_frame_area, clip)) {
        for (int32_t y = intersection.y1; y <= intersection.y2; ++y) {
            uint8_t *dst = layer->draw_buf->data +
                (size_t)(y - layer->buf_area.y1) * layer->draw_buf->header.stride +
                (size_t)(intersection.x1 - layer->buf_area.x1) * 2U;
            const uint8_t *src = s_pager_frame_buf.data +
                (size_t)(y - s_pager_frame_area.y1) * s_pager_frame_buf.header.stride +
                (size_t)(intersection.x1 - s_pager_frame_area.x1) * 2U;
            copy_rgb565_span((uint16_t *)dst, (const uint16_t *)src,
                            (size_t)lv_area_get_width(&intersection));
        }
    }
    return true;
}

static void cache_plane_draw_cb(lv_event_t *event) {
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_COVER_CHECK) {
        lv_event_set_cover_res(event, cache_plane_ready()
            ? LV_COVER_RES_COVER : LV_COVER_RES_NOT_COVER);
        lv_event_stop_processing(event);
    } else if (code == LV_EVENT_DRAW_MAIN) {
        if (cache_plane_ready()) {
            lv_layer_t *layer = lv_event_get_layer(event);
            lv_draw_rect_dsc_t draw;
            lv_draw_rect_dsc_init(&draw);
            draw.bg_color = lv_color_black();
            draw.bg_opa = LV_OPA_COVER;
            lv_draw_rect(layer, &draw, &layer->_clip_area);
            if (s_scene_pager_visible && s_pager_frame_valid) {
                lv_draw_image_dsc_t image;
                lv_draw_image_dsc_init(&image);
                image.src = &s_pager_frame_buf;
                lv_draw_image(layer, &image, &s_pager_frame_area);
            } else if (s_pager_clip && !s_qr_reveal_active) {
                lv_obj_refr(layer, s_pager_clip);
            }
        }
        lv_event_stop_processing(event);
    } else if (code == LV_EVENT_DRAW_MAIN_END || code == LV_EVENT_DRAW_POST) {
        lv_event_stop_processing(event);
    }
}

static void draw_backdrop_cb(lv_event_t *event) {
    lv_layer_t *layer = lv_event_get_layer(event);
    if (!layer) return;
    if (s_backdrop_progress <= 0) return;
    const bool exact_mask = s_backdrop_frozen_face &&
                            nuwax_face_frozen_rgb565(NULL) != NULL;
    int32_t diameter = CC_REVEAL_MAX_DIAMETER;
    lv_area_t area = layer->_clip_area;
    if (!exact_mask) {
        diameter =
            (CC_REVEAL_MAX_DIAMETER * s_backdrop_progress + 500) / 1000;
        if (diameter < 1) diameter = 1;
        area.x1 = (CC_W - diameter) / 2;
        area.y1 = (CC_H - diameter) / 2;
        area.x2 = area.x1 + diameter - 1;
        area.y2 = area.y1 + diameter - 1;
    }
    lv_draw_rect_dsc_t draw;
    lv_draw_rect_dsc_init(&draw);
    draw.bg_color = lv_color_hex(0x000000);
    draw.bg_opa = LV_OPA_COVER;
    draw.radius = exact_mask ? 0 : LV_RADIUS_CIRCLE;
    lv_draw_rect(layer, &draw, &area);


}

static void apply_page_progress(int32_t progress, bool quantize) {
    /* Timer and finger positions share the same continuous coordinate. */
    (void)quantize;
    if (!s_status_page || !s_menu_page) return;
    if (progress < 0) progress = 0;
    if (progress > 1000) progress = 1000;
    if (progress == s_page_progress) return;
    nuwax_lvgl_port_note_interaction();
    s_page_progress = progress;
    update_page_dots();
    if (s_page_perf_active) {
        const int64_t now_us = esp_timer_get_time();
        if (s_page_perf_last_frame_us > 0 &&
            now_us > s_page_perf_last_frame_us) {
            const uint32_t gap_us =
                (uint32_t)(now_us - s_page_perf_last_frame_us);
            if (gap_us > s_page_perf_max_gap_us) {
                s_page_perf_max_gap_us = gap_us;
            }
        }
        s_page_perf_last_frame_us = now_us;
        s_page_perf_frames++;
    }

    s_cache_plan_valid = false;
    /* Both page trees remain at fixed coordinates. The shared reveal updater
     * changes only child clips and opacity. During transitions the trees are
     * hidden and these values drive the PSRAM component compositor. */
    if (s_reveal_raw >= 0) {
        apply_reveal_progress(s_reveal_raw, false, true);
    }
}

static void page_progress_exec(void *obj, int32_t value) {
    (void)obj;
    if (value > 0 && value < 1000 && s_page_perf_active &&
        esp_timer_get_time() - s_page_perf_last_frame_us < CC_LOCAL_ANIM_FRAME_INTERVAL_US)
        return;
    apply_page_progress(value, true);
}

static void start_page_perf(void) {
    s_page_perf_active = true;
    s_page_perf_drag = s_page_dragging;
    s_page_perf_frames = 1U;
    s_page_perf_max_gap_us = 0U;
    s_page_perf_started_us = esp_timer_get_time();
    s_page_perf_last_frame_us = s_page_perf_started_us;
}

static void finish_page_perf(void) {
    if (!s_page_perf_active) return;
    const int64_t elapsed_us = esp_timer_get_time() - s_page_perf_started_us;
    const uint32_t intervals = s_page_perf_frames > 0U
                                   ? s_page_perf_frames - 1U : 0U;
    const uint32_t fps_x10 = elapsed_us > 0
        ? (uint32_t)(((uint64_t)intervals * 10000000ULL) /
                     (uint64_t)elapsed_us)
        : 0U;
    const bool minimum_met = fps_x10 >= CC_OPERATION_MIN_FPS * 10U;
    if (minimum_met || s_page_perf_drag) {
        ESP_LOGI(TAG,
                 "control center page perf phase=%s frames=%u elapsed_ms=%u fps=%u.%u max_gap_ms=%u minimum_fps=%u target_fps=%u",
                 s_page_perf_drag ? "drag" : "animation",
                 (unsigned)s_page_perf_frames,
                 (unsigned)(elapsed_us > 0 ? elapsed_us / 1000LL : 0LL),
                 (unsigned)(fps_x10 / 10U), (unsigned)(fps_x10 % 10U),
                 (unsigned)(s_page_perf_max_gap_us / 1000U),
                 (unsigned)CC_OPERATION_MIN_FPS,
                 (unsigned)CONFIG_NUWAX_UI_ANIMATION_TARGET_FPS);
    } else {
        ESP_LOGW(TAG,
                 "control center page below minimum phase=%s frames=%u elapsed_ms=%u fps=%u.%u max_gap_ms=%u minimum_fps=%u target_fps=%u",
                 s_page_perf_drag ? "drag" : "animation",
                 (unsigned)s_page_perf_frames,
                 (unsigned)(elapsed_us > 0 ? elapsed_us / 1000LL : 0LL),
                 (unsigned)(fps_x10 / 10U), (unsigned)(fps_x10 % 10U),
                 (unsigned)(s_page_perf_max_gap_us / 1000U),
                 (unsigned)CC_OPERATION_MIN_FPS,
                 (unsigned)CONFIG_NUWAX_UI_ANIMATION_TARGET_FPS);
    }
    s_page_perf_active = false;
}

static void cancel_page_perf(void) {
    s_page_perf_active = false;
    s_page_perf_frames = 0U;
    s_page_perf_max_gap_us = 0U;
    s_page_perf_started_us = 0;
    s_page_perf_last_frame_us = 0;
}

static bool suspend_display_invalidation(lv_obj_t *obj) {
    lv_display_t *display = obj ? lv_obj_get_display(obj) : NULL;
    if (!display || !lv_display_is_invalidation_enabled(display)) {
        return false;
    }
    lv_display_enable_invalidation(display, false);
    return true;
}

static void resume_display_invalidation(lv_obj_t *obj, bool suspended) {
    if (!suspended || !obj) return;
    lv_display_t *display = lv_obj_get_display(obj);
    if (display) lv_display_enable_invalidation(display, true);
}

static void set_page_tree_mode(cc_page_t active_page, bool transitioning) {
    if (!s_surface || !s_status_page || !s_menu_page) return;
    const bool suspended = suspend_display_invalidation(s_surface);
#if NUWAX_TEST_PANEL_ENABLED
    if (s_test_page) {
        if (active_page == CC_PAGE_TEST && !transitioning) {
            lv_obj_remove_flag(s_test_page, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_status_page, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_menu_page, LV_OBJ_FLAG_HIDDEN);
            resume_display_invalidation(s_surface, suspended);
            return;
        }
        lv_obj_add_flag(s_test_page, LV_OBJ_FLAG_HIDDEN);
    }
#endif
    if (s_transition_cache_active) {
        lv_obj_add_flag(s_status_page, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_menu_page, LV_OBJ_FLAG_HIDDEN);
    } else if (transitioning || active_page == CC_PAGE_STATUS) {
        lv_obj_remove_flag(s_status_page, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_menu_page, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_status_page, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_menu_page, LV_OBJ_FLAG_HIDDEN);
    }
    if (!s_transition_cache_active && transitioning) {
        lv_obj_remove_flag(s_status_page, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_menu_page, LV_OBJ_FLAG_HIDDEN);
    }
    resume_display_invalidation(s_surface, suspended);
}

static bool transition_cache_page_valid(cc_page_t page) {
    for (size_t i = 0; i < CC_TRANSITION_CACHE_COMPONENTS; ++i) {
        if (s_transition_cache[i].page == page &&
            !s_transition_cache[i].valid) {
            return false;
        }
    }
    return true;
}

static bool transition_cache_prepare_page(cc_page_t page) {
    render_page_if_dirty(page);
    const bool dirty = page == CC_PAGE_STATUS
                           ? s_status_cache_dirty
                           : s_menu_cache_dirty;
    if (!dirty && transition_cache_page_valid(page)) return true;
    /* Preview updates are live immediately; avoid rebuilding while the
     * slider is still changing. First-time pages must still be populated. */
    if (page == CC_PAGE_MENU && transition_cache_page_valid(page) &&
        esp_timer_get_time() < s_volume_cache_settle_us) return false;
    return transition_cache_capture_page(page);
}

static bool transition_cache_activate(bool both_pages) {
#if NUWAX_TEST_PANEL_ENABLED
    if (s_page == CC_PAGE_TEST) return false;
#endif
    if (s_transition_cache_active) return true;
    const cc_page_t other_page = s_page == CC_PAGE_STATUS
                                     ? CC_PAGE_MENU : CC_PAGE_STATUS;
    /* Freeze the last complete visual generation for motion only. Idle-only
     * rebuild plus a 200ms dirty-age cutoff made continuous gestures fall back
     * to the full live tree. Dirty flags are deliberately retained: endpoint
     * handoff renders current state, and control decisions use the live model.
     * Never use a partial/uninitialized page or commit a build during motion. */
    const bool current_ready = transition_cache_page_valid(s_page);
    const bool other_ready = transition_cache_page_valid(other_page);
    if (!s_transition_cache_available || !current_ready ||
        (both_pages && !other_ready)) {
        ESP_LOGD(TAG,
                 "transition cache activation deferred mode=%s current_ready=%d other_ready=%d",
                 both_pages ? "page" : "reveal",
                 current_ready ? 1 : 0,
                 other_ready ? 1 : 0);
        return false;
    }
    s_transition_cache_active = true;
    s_cache_plan_valid = false;
    if (s_cache_plane) {
        const bool suspended = suspend_display_invalidation(s_root);
        lv_obj_remove_flag(s_cache_plane, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_cache_plane);
        resume_display_invalidation(s_root, suspended);
    }
    set_page_tree_mode(s_page, both_pages);
    ESP_LOGI(TAG, "transition compositor active mode=%s cache_bytes=%u",
             both_pages ? "page" : "reveal",
             (unsigned)s_transition_cache_arena_size);
    return true;
}

static bool transition_cache_deactivate(void) {
    s_present_pending = CC_PRESENT_NONE;
    pager_release_sprites();
    if (!s_transition_cache_active) return false;
    s_transition_cache_active = false;
    s_cache_plan_valid = false;
    if (s_cache_plane) {
        const bool suspended = suspend_display_invalidation(s_root);
        lv_obj_add_flag(s_cache_plane, LV_OBJ_FLAG_HIDDEN);
        resume_display_invalidation(s_root, suspended);
    }
    return true;
}

static void transition_cache_invalidate_page(cc_page_t page) {
    if (!s_surface) return;
    for (size_t i = 0; i < CC_TRANSITION_CACHE_COMPONENTS; ++i) {
        cc_transition_cache_component_t *component = &s_transition_cache[i];
        if (component->page != page || !component->source) continue;
        lv_area_t area;
        lv_obj_get_coords(component->source, &area);
        lv_area_increase(&area, component->ext_draw_size,
                         component->ext_draw_size);
        (void)lv_obj_invalidate_area(s_surface, &area);
    }
}

static void settle_page_transition(cc_page_t page) {
    const int32_t target_progress =
        page == CC_PAGE_STATUS ? 0 : 1000;
    if (s_page != page) s_input_scene_generation++;
    s_page = page;
    s_page_progress = target_progress;
    if (s_reveal_raw >= 0) {
        apply_reveal_progress(s_reveal_raw, false, true);
    }
    const bool was_cached = transition_cache_deactivate();
    /* At the endpoint the retired tree has zero visible opacity. Hide it
     * without scheduling a redundant full-screen repaint so future partial
     * refreshes traverse only the active page. */
    set_page_tree_mode(page, false);
    if (was_cached &&
        (page == CC_PAGE_STATUS ? s_status_cache_dirty
                                : s_menu_cache_dirty)) {
        transition_cache_invalidate_page(page);
    }
    s_page_transition_active = false;
    if (s_pager_clip) lv_obj_move_foreground(s_pager_clip);
    update_page_dots();
    if (s_open && !s_closing) {
        start_local_animations();
    }
}

static void page_transition_completed(lv_anim_t *anim) {
    (void)anim;
    if (s_transition_cache_active && !s_present_finishing) {
        request_presented_finish(CC_PRESENT_PAGE); return;
    }
    finish_page_perf();
    settle_page_transition(s_page_transition_settle_page);
}

static bool reveal_owns_motion(void) {
    return s_dragging || s_closing || s_reveal_raw != 1000 ||
           s_reveal_timer != NULL || s_present_pending == CC_PRESENT_REVEAL;
}

static bool drawer_accepts_tap(void) {
    return s_open && !s_ota_blocked && !reveal_owns_motion() &&
           !s_page_transition_active && !s_sheet_active &&
           !(s_click_timer && s_click_target != 0);
}

static bool begin_page_transition(cc_page_t target) {
    cancel_click();
    if (!s_open || reveal_owns_motion() || s_sheet_active ||
        s_page_transition_active || target == s_page) {
        return false;
    }
    s_input_scene_generation++;
    stop_local_animations();
    s_page_transition_active = true;
    s_page_transition_target = target;
    s_page_transition_settle_page = target;
    set_page_tree_mode(s_page, true);
    if (s_pager_clip) lv_obj_move_foreground(s_pager_clip);
    render_page_if_dirty(target);
    (void)transition_cache_activate(true);
    return true;
}

static void switch_page_without_animation(cc_page_t target) {
    lv_anim_delete(s_surface, page_progress_exec);
    cancel_page_perf();
    (void)transition_cache_deactivate();
    set_page_tree_mode(s_page, true);
    render_page_if_dirty(target);
    settle_page_transition(target);
}

/* A vertical gesture takes ownership before touching the shared compositor.
 * Retire the horizontal callback first, including a final frame awaiting
 * presentation, so it cannot release the vertical gesture's cache later. */
static void retire_page_motion(void) {
#if NUWAX_TEST_PANEL_ENABLED
    s_test_page_drag = false;
    s_test_page_drag_dx = 0;
#endif
    if (s_page_transition_active || s_page_dragging) {
        const cc_page_t nearest = s_page_progress >= 500
                                     ? CC_PAGE_MENU : CC_PAGE_STATUS;
        switch_page_without_animation(nearest);
    }
    s_page_dragging = false;
}

static void set_page(cc_page_t page, bool animate) {
    if (!s_status_page || !s_menu_page || s_page == page ||
        s_page_transition_active || s_sheet_active || (s_click_timer && s_click_target != 0)) {
        return;
    }
    if (animate && !s_open) animate = false;
    if (animate && reveal_owns_motion()) return;
    clear_press();
#if NUWAX_TEST_PANEL_ENABLED
    const bool test_boundary = page == CC_PAGE_TEST || s_page == CC_PAGE_TEST;
    if (test_boundary && animate && s_open && s_reveal_raw == 1000) {
        const cc_page_t old = s_page;
        switch_page_without_animation(page);
        lv_obj_invalidate(s_surface);
        s_sheet_tab_from = (int)old;
        start_sheet(page == CC_PAGE_TEST ? s_test_page :
                    page == CC_PAGE_MENU ? s_menu_page : s_status_page,
                    page > old ? CC_W : -CC_W, 0, false);
        return;
    }
    if (test_boundary) animate = false;
#endif
    const int32_t target = page == CC_PAGE_STATUS ? 0 : 1000;
    lv_anim_delete(s_surface, page_progress_exec);
    if (!animate) {
        switch_page_without_animation(page);
#if NUWAX_TEST_PANEL_ENABLED
        /* Menu and test share the menu endpoint of the two-page compositor.
         * Tree visibility changes there are batched without invalidation. */
        if (test_boundary) lv_obj_invalidate(s_surface);
#endif
    } else if (begin_page_transition(page)) {
        start_page_perf();
        lv_anim_t page_anim;
        lv_anim_init(&page_anim);
        lv_anim_set_var(&page_anim, s_surface);
        lv_anim_set_values(&page_anim, s_page_progress, target);
        lv_anim_set_duration(&page_anim, CC_PAGE_ANIM_MS);
        lv_anim_set_path_cb(&page_anim, lv_anim_path_linear);
        lv_anim_set_exec_cb(&page_anim, page_progress_exec);
        lv_anim_set_completed_cb(&page_anim, page_transition_completed);
        lv_anim_start(&page_anim);
        update_page_dots();
    } else {
        switch_page_without_animation(page);
    }
}

void nuwax_drawer_page_drag_to(int16_t delta_x) {
    cancel_click();
    if (!s_open || reveal_owns_motion() || s_detail != CC_DETAIL_NONE || s_sheet_active || !s_surface) {
        return;
    }
#if NUWAX_TEST_PANEL_ENABLED
    if (s_test_page_drag || (!s_page_dragging && !s_page_transition_active &&
        (s_page == CC_PAGE_TEST || (s_page == CC_PAGE_MENU && delta_x < 0)))) {
        s_test_page_drag = true;
        s_test_page_drag_dx = delta_x;
        return;
    }
#endif
    if (!s_page_dragging) {
        const bool resuming = s_page_transition_active;
        const bool toward_other =
            resuming ||
            (s_page == CC_PAGE_STATUS && delta_x < 0) ||
            (s_page == CC_PAGE_MENU && delta_x > 0);
        if (!toward_other) return;
        const cc_page_t target = s_page == CC_PAGE_STATUS
                                     ? CC_PAGE_MENU
                                     : CC_PAGE_STATUS;
        if (!resuming && !begin_page_transition(target)) {
            return;
        }
        /* Re-grab the visible frame, including a pending endpoint. Do not
         * settle to a page before giving the finger ownership. */
        if (resuming) {
            lv_anim_delete(s_surface, page_progress_exec);
            if (s_present_pending == CC_PRESENT_PAGE) s_present_pending = CC_PRESENT_NONE;
            finish_page_perf();
        }
        s_page_dragging = true;
        start_page_perf();
        s_page_drag_origin_progress = s_page_progress;
        lv_anim_delete(s_surface, page_progress_exec);
    }
    int32_t adjusted_dx = delta_x;
    if ((s_page_drag_origin_progress <= 0 && adjusted_dx > 0) ||
        (s_page_drag_origin_progress >= 1000 && adjusted_dx < 0)) {
        adjusted_dx = adjusted_dx * 16 / 100;
    }
    int32_t progress = s_page_drag_origin_progress -
        adjusted_dx * 1000 / CC_PAGE_DRAG_RANGE_PX;
    if (progress < 0) progress = 0;
    if (progress > 1000) progress = 1000;
    apply_page_progress(progress, false);
}

void nuwax_drawer_page_finish_drag(bool commit) {
#if NUWAX_TEST_PANEL_ENABLED
    if (s_test_page_drag) {
        s_test_page_drag = false;
        if (commit && s_open && !s_closing && s_detail == CC_DETAIL_NONE) {
            if (s_page == CC_PAGE_MENU && s_test_page_drag_dx < 0)
                set_page(CC_PAGE_TEST, true);
            else if (s_page == CC_PAGE_TEST && s_test_page_drag_dx > 0)
                set_page(CC_PAGE_MENU, true);
        }
        s_test_page_drag_dx = 0;
        return;
    }
#endif
    if (!s_open || s_detail != CC_DETAIL_NONE || !s_surface ||
        !s_page_dragging) {
        return;
    }
    const cc_page_t origin = s_page_drag_origin_progress >= 500
                                 ? CC_PAGE_MENU
                                 : CC_PAGE_STATUS;
    cc_page_t target = origin;
    const bool moved_toward_other =
        origin == CC_PAGE_STATUS
            ? s_page_progress > s_page_drag_origin_progress
            : s_page_progress < s_page_drag_origin_progress;
    if (commit && moved_toward_other) {
        target = origin == CC_PAGE_STATUS ? CC_PAGE_MENU : CC_PAGE_STATUS;
    }
    /* Drag duration includes finger holds. Measure automatic settling
     * separately instead of reporting a hold as an animation frame stall. */
    finish_page_perf();
    s_page_dragging = false;
    s_page_transition_settle_page = target;
    const int32_t target_progress = target == CC_PAGE_STATUS ? 0 : 1000;
    lv_anim_delete(s_surface, page_progress_exec);
    if (s_page_progress == target_progress) {
        /* Even when the finger reached the endpoint, its last frame may still
         * be dirty. Submit it from the cache before exposing the live tree. */
        page_transition_completed(NULL);
    } else {
        const uint32_t distance = (uint32_t)(
            s_page_progress > target_progress
                ? s_page_progress - target_progress
                : target_progress - s_page_progress);
        uint32_t duration_ms = (CC_PAGE_ANIM_MS * distance + 999U) / 1000U;
        if (duration_ms < 80U) duration_ms = 80U;
        lv_anim_t page_anim;
        if (!s_page_perf_active) start_page_perf();
        lv_anim_init(&page_anim);
        lv_anim_set_var(&page_anim, s_surface);
        lv_anim_set_values(&page_anim, s_page_progress, target_progress);
        lv_anim_set_duration(&page_anim, duration_ms);
        lv_anim_set_path_cb(&page_anim, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&page_anim, page_progress_exec);
        lv_anim_set_completed_cb(&page_anim, page_transition_completed);
        lv_anim_start(&page_anim);
    }
    update_page_dots();
}

static int32_t smoothstep_permille(int32_t value,
                                   int32_t edge0,
                                   int32_t edge1) {
    if (value <= edge0) return 0;
    if (value >= edge1) return 1000;
    const int32_t x = (value - edge0) * 1000 / (edge1 - edge0);
    return (int32_t)((int64_t)x * x * (3000 - 2 * x) / 1000000LL);
}

static lv_opa_t opacity_from_permille(int32_t value) {
    if (value <= 0) return 0;
    if (value >= 1000) return LV_OPA_COVER;
    return (lv_opa_t)((value * 255 + 500) / 1000);
}

static int32_t multiply_permille(int32_t left, int32_t right) {
    if (left <= 0 || right <= 0) return 0;
    if (left >= 1000) return right;
    if (right >= 1000) return left;
    return (int32_t)((int64_t)left * right / 1000LL);
}

static int32_t status_page_visibility(int32_t start, int32_t end) {
    return 1000 - smoothstep_permille(s_page_progress, start, end);
}

static int32_t menu_page_visibility(int32_t start, int32_t end) {
    return smoothstep_permille(s_page_progress, start, end);
}

static bool set_opa_if_changed(lv_obj_t *obj, lv_opa_t opacity) {
    if (obj && lv_obj_get_style_opa(obj, LV_PART_MAIN) != opacity) {
        lv_obj_set_style_opa(obj, opacity, 0);
        return true;
    }
    return false;
}

static bool set_hidden_if_changed(lv_obj_t *obj, bool hidden) {
    if (!obj) return false;
    const bool currently_hidden = lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
    if (currently_hidden == hidden) return false;
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
    return true;
}

static bool set_pos_if_changed(lv_obj_t *obj, int32_t x, int32_t y) {
    if (!obj) return false;
    if (lv_obj_get_x(obj) != x || lv_obj_get_y(obj) != y) {
        lv_obj_set_pos(obj, x, y);
        return true;
    }
    return false;
}

static bool set_size_if_changed(lv_obj_t *obj, int32_t w, int32_t h) {
    if (!obj) return false;
    if (lv_obj_get_width(obj) != w || lv_obj_get_height(obj) != h) {
        lv_obj_set_size(obj, w, h);
        return true;
    }
    return false;
}

static bool apply_content_reveal(lv_obj_t *clip,
                                 lv_obj_t *content,
                                 int32_t base_x,
                                 int32_t base_y,
                                 int32_t width,
                                 int32_t height,
                                 int32_t offset_x,
                                 int32_t offset_y,
                                 int32_t visibility_progress) {
    if (!clip || !content) return false;
    bool changed = false;
    if (visibility_progress <= 0) {
        return set_hidden_if_changed(clip, true);
    }
    changed |= set_hidden_if_changed(clip, false);

    const int32_t content_x =
        base_x + offset_x * (1000 - visibility_progress) / 1000;
    const int32_t content_y =
        base_y + offset_y * (1000 - visibility_progress) / 1000;

    /* The display flush compositor owns the one global circular mask. Keep
     * each wrapper at its stable rectangular bounds so text, icons, and cards
     * all meet the same moving circle edge instead of revealing through
     * unrelated vertical clip rectangles. */
    changed |= set_pos_if_changed(clip, content_x, content_y);
    changed |= set_size_if_changed(clip, width, height);
    changed |= set_pos_if_changed(content, 0, 0);
    return changed;
}

static void invalidate_surface_region(int32_t x, int32_t y,
                                      int32_t width, int32_t height) {
    if (!s_surface || width <= 0 || height <= 0) return;
    lv_area_t area = {
        .x1 = x,
        .y1 = y,
        .x2 = x + width - 1,
        .y2 = y + height - 1,
    };
    (void)lv_obj_invalidate_area(s_surface, &area);
}

static void apply_reveal_progress(int32_t progress,
                                  bool quantize,
                                  bool force_content) {
    if (!s_reveal_clip || !s_surface) return;
    s_cache_plan_valid = false;
    if (progress < 0) progress = 0;
    if (progress > 1000) progress = 1000;
    if (quantize && progress > 0 && progress < 1000) {
        const int32_t step = (progress * CC_ANIM_STEPS + 500) / 1000;
        progress = step * 1000 / CC_ANIM_STEPS;
    }
    const bool progress_changed = progress != s_reveal_raw;
    if (!progress_changed && !force_content) return;
    nuwax_lvgl_port_note_interaction();
    if (progress_changed) {
        s_reveal_raw = progress;
    }

    if (progress_changed && s_reveal_perf_active) {
        const int64_t now_us = esp_timer_get_time();
        if (s_reveal_perf_last_frame_us > 0 &&
            now_us > s_reveal_perf_last_frame_us) {
            const uint32_t gap_us =
                (uint32_t)(now_us - s_reveal_perf_last_frame_us);
            if (gap_us > s_reveal_perf_max_gap_us) {
                s_reveal_perf_max_gap_us = gap_us;
            }
        }
        s_reveal_perf_last_frame_us = now_us;
        s_reveal_perf_frames++;
    }

    if (progress_changed) {
        const int32_t backdrop_progress = progress >= CC_REVEAL_BACKDROP_DONE
                                              ? 1000
                                              : progress * 1000 /
                                                    CC_REVEAL_BACKDROP_DONE;
        update_backdrop_progress(backdrop_progress);
        nuwax_face_set_overlay_progress((uint16_t)backdrop_progress);
    }

    /* Moving/resizing each clip and changing each opacity would otherwise add
     * several invalid areas per widget. Batch those changes into six stable
     * regions so cached and fallback rendering remain partial. */
    const bool paging = s_page_transition_active && progress == 1000;
    lv_area_t old_bounds[CC_TRANSITION_CACHE_COMPONENTS] = {0};
    bool old_visible[CC_TRANSITION_CACHE_COMPONENTS] = {false};
    if (paging) {
        for (size_t i = 0; i < CC_TRANSITION_CACHE_COMPONENTS; ++i) {
            lv_obj_t *obj = s_transition_cache[i].source;
            /* Allocation failure clears cache bindings; live fallback uses
             * the original broad damage region below. */
            if (obj && lv_obj_get_style_opa(obj, 0) > LV_OPA_MIN) {
                lv_obj_get_coords(obj, &old_bounds[i]); old_visible[i] = true;
            }
        }
    }
    const bool content_invalidation_suspended =
        suspend_display_invalidation(s_surface);
    bool pager_changed = false;
    bool header_changed = false;
    bool footer_changed = false;
    bool card_changed[4] = {false, false, false, false};
    bool volume_changed = false;

    /* Component opacity and motion follow the simulator. A single RGB565
     * output mask clips the complete page, so every component follows the
     * same circular edge. */
    const int32_t pager_progress = smoothstep_permille(progress, 160, 620);
    if (s_pager) {
        pager_changed |= apply_content_reveal(
            s_pager_clip, s_pager, CC_PAGER_X, CC_PAGER_Y, CC_PAGER_W, CC_PAGER_H,
            0, 0, pager_progress);
        pager_changed |= set_opa_if_changed(
            s_pager, opacity_from_permille(pager_progress));
    }

    const int32_t status_dx = paging ? -CC_W * s_page_progress / 1000 : 0;
    const int32_t menu_dx = paging ? CC_W * (1000 - s_page_progress) / 1000 : 0;
    const int32_t header_progress = smoothstep_permille(progress, 160, 620);
    const int32_t status_header_progress = multiply_permille(
        header_progress, paging ? 1000 : status_page_visibility(0, 350));
    const int32_t menu_header_progress = multiply_permille(
        header_progress, paging ? 1000 : menu_page_visibility(400, 750));
    if (s_status_header) {
        header_changed |= apply_content_reveal(
            s_status_header_clip, s_status_header,
            CC_HEADER_X + status_dx, CC_HEADER_Y, CC_HEADER_W, CC_HEADER_H,
            0, 7, status_header_progress);
        header_changed |= set_opa_if_changed(
            s_status_header, opacity_from_permille(status_header_progress));
    }
    if (s_menu_header) {
        header_changed |= apply_content_reveal(
            s_menu_header_clip, s_menu_header,
            CC_HEADER_X + menu_dx, CC_HEADER_Y, CC_HEADER_W, CC_HEADER_H,
            0, 7, menu_header_progress);
        header_changed |= set_opa_if_changed(
            s_menu_header, opacity_from_permille(menu_header_progress));
    }
    const int32_t footer_progress = smoothstep_permille(progress, 580, 920);
    const int32_t status_footer_progress = multiply_permille(
        footer_progress, paging ? 1000 : status_page_visibility(260, 440));
    if (s_footer) {
        footer_changed |= apply_content_reveal(
            s_footer_clip, s_footer, CC_FOOTER_X + status_dx, CC_FOOTER_Y, CC_FOOTER_W, CC_FOOTER_H,
            0, 7, status_footer_progress);
        footer_changed |= set_opa_if_changed(
            s_footer, opacity_from_permille(status_footer_progress));
    }

    static const int16_t base_x[4] = {
        CC_CARD_LEFT_X, CC_CARD_RIGHT_X, CC_CARD_LEFT_X, CC_CARD_RIGHT_X,
    };
    static const int16_t base_y[4] = {
        CC_STATUS_ROW0_Y, CC_STATUS_ROW0_Y, CC_STATUS_ROW1_Y, CC_STATUS_ROW1_Y,
    };
    static const int16_t offset_x[4] = {-8, 8, -8, 8};
    static const int16_t offset_y[4] = {-6, -6, 7, 7};
    /* Match the simulator's stagger; geometric clipping is shared globally. */
    static const int16_t reveal_start[4] = {240, 320, 400, 480};
    /* A 300-permille stagger gives horizontal motion visible intermediate
     * samples. The existing RGB565 component cache avoids re-rasterizing text. */
    static const int16_t status_page_fade_start[4] = {0, 60, 120, 180};
    static const int16_t menu_page_fade_start[4] = {400, 460, 520, 580};
    for (size_t i = 0; i < 4; ++i) {
        const int32_t tile_progress = smoothstep_permille(
            progress, reveal_start[i], reveal_start[i] + 340);
        const int32_t status_tile_progress = multiply_permille(
            tile_progress,
            paging ? 1000 : status_page_visibility(status_page_fade_start[i],
                                   status_page_fade_start[i] + 300));
        const int32_t menu_tile_progress = multiply_permille(
            tile_progress,
            paging ? 1000 : menu_page_visibility(menu_page_fade_start[i],
                                 menu_page_fade_start[i] + 300));
        if (s_status_cards[i].root) {
            card_changed[i] |= apply_content_reveal(
                s_status_cards[i].clip,
                s_status_cards[i].root,
                base_x[i] + status_dx, base_y[i],
                CC_CARD_W, CC_STATUS_CARD_H,
                offset_x[i], offset_y[i], status_tile_progress);
            card_changed[i] |= set_opa_if_changed(
                s_status_cards[i].root,
                opacity_from_permille(status_tile_progress));
        }
        if (s_menu_cards[i]) {
            const int16_t menu_y = i < 2 ? CC_MENU_ROW0_Y : CC_MENU_ROW1_Y;
            card_changed[i] |= apply_content_reveal(
                s_menu_card_clips[i], s_menu_cards[i],
                base_x[i] + menu_dx, menu_y,
                CC_CARD_W, CC_MENU_ACTION_H,
                offset_x[i], offset_y[i], menu_tile_progress);
            card_changed[i] |= set_opa_if_changed(
                s_menu_cards[i], opacity_from_permille(menu_tile_progress));
        }
    }

    const int32_t volume_progress = smoothstep_permille(progress, 880, 980);
    const int32_t menu_volume_progress = multiply_permille(
        volume_progress, paging ? 1000 : menu_page_visibility(820, 1000));
    const lv_opa_t volume_opa = opacity_from_permille(menu_volume_progress);
    if (s_menu_volume) {
        volume_changed |= set_hidden_if_changed(
            s_menu_volume, menu_volume_progress <= 0);
        if (menu_volume_progress > 0) {
            volume_changed |= set_pos_if_changed(s_menu_volume, CC_MENU_VOLUME_X + menu_dx, CC_MENU_VOLUME_Y);
        }
        volume_changed |= set_opa_if_changed(s_menu_volume, volume_opa);
    }

    resume_display_invalidation(s_surface, content_invalidation_suspended);
    if (content_invalidation_suspended && paging) {
        if (s_transition_cache_available) {
            lv_obj_update_layout(s_surface);
            for (size_t i = 0; i < CC_TRANSITION_CACHE_COMPONENTS; ++i) {
                lv_area_t damage = old_bounds[i];
                bool dirty = old_visible[i];
                lv_obj_t *obj = s_transition_cache[i].source;
                if (obj && lv_obj_get_style_opa(obj, 0) > LV_OPA_MIN) {
                    lv_area_t next; lv_obj_get_coords(obj, &next);
                    /* Adjacent positions normally overlap. Submit their union
                     * once rather than forcing LVGL to join duplicate areas.
                     * For large jumps keep disjoint damage to avoid overdraw. */
                    if (dirty && !(next.x1 > damage.x2 + 1 || next.x2 < damage.x1 - 1 ||
                                   next.y1 > damage.y2 + 1 || next.y2 < damage.y1 - 1)) {
                        if (next.x1 < damage.x1) damage.x1 = next.x1;
                        if (next.y1 < damage.y1) damage.y1 = next.y1;
                        if (next.x2 > damage.x2) damage.x2 = next.x2;
                        if (next.y2 > damage.y2) damage.y2 = next.y2;
                    } else {
                        invalidate_surface_region(next.x1, next.y1,
                                                  next.x2-next.x1+1, next.y2-next.y1+1);
                    }
                }
                if (dirty) invalidate_surface_region(damage.x1, damage.y1,
                                                     damage.x2-damage.x1+1, damage.y2-damage.y1+1);
            }
        } else invalidate_surface_region(0, CC_HEADER_Y, CC_W, CC_PAGER_Y - CC_HEADER_Y);
    } else if (content_invalidation_suspended) {
        if (pager_changed) invalidate_surface_region(CC_PAGER_X, CC_PAGER_Y, CC_PAGER_W, CC_PAGER_H);
        if (header_changed) {
            invalidate_surface_region(CC_HEADER_X - 28, CC_HEADER_Y, CC_HEADER_W + 56, CC_HEADER_H + 7);
        }
        if (footer_changed) invalidate_surface_region(CC_FOOTER_X - 28, CC_FOOTER_Y, CC_FOOTER_W + 56, CC_FOOTER_H + 7);
        const int32_t cards_x = CC_CARD_LEFT_X - 28;
        const int32_t cards_width =
            CC_CARD_RIGHT_X + CC_CARD_W + 28 - cards_x;
        if (card_changed[0] || card_changed[1]) {
            invalidate_surface_region(cards_x, CC_STATUS_ROW0_Y - 7,
                                      cards_width, CC_STATUS_CARD_H + 14);
        }
        if (card_changed[2] || card_changed[3]) {
            invalidate_surface_region(cards_x, CC_MENU_ROW1_Y - 7,
                                      cards_width, CC_STATUS_CARD_H + 14);
        }
        if (volume_changed) {
            invalidate_surface_region(CC_MENU_VOLUME_X, CC_MENU_VOLUME_Y,
                                      CC_MENU_VOLUME_W, CC_MENU_VOLUME_H);
        }
    }
}

static void apply_reveal_animation_step(uint32_t step) {
    if (s_reveal_anim_steps == 0U) return;
    if (step > s_reveal_anim_steps) step = s_reveal_anim_steps;
    const int32_t distance = s_reveal_anim_target - s_reveal_anim_from;
    const int32_t time_progress =
        (int32_t)((uint64_t)step * 1000U / s_reveal_anim_steps);
    if (s_qr_reveal_active) {
        /* Smooth radius acceleration/deceleration. QR modules remain at their
         * final size; only the circular mask grows over the frozen face. */
        const int32_t radius = (int32_t)((int64_t)time_progress * time_progress *
                                        (3000 - 2 * time_progress) / 1000000LL);
        const int32_t visual = s_reveal_anim_from +
            (int32_t)((int64_t)distance * radius / 1000);
        s_reveal_raw = visual;
        update_backdrop_progress(visual);
        nuwax_face_set_overlay_progress((uint16_t)visual);
        nuwax_lvgl_port_note_interaction();
        return;
    }
    const int32_t remaining = 1000 - time_progress;
    int32_t eased_progress =
        1000 - (int32_t)((int64_t)remaining * remaining * remaining /
                         1000000LL);
    /* Cubic ease-out matches the HTML reference but collapses several final
     * integer samples to 1000. Reserve one permille per remaining logical
     * frame so completion stays monotonic without changing the visible curve. */
    if (step < s_reveal_anim_steps) {
        const int32_t remaining_steps =
            (int32_t)(s_reveal_anim_steps - step);
        const int32_t tail_cap = 1000 - remaining_steps;
        if (eased_progress > tail_cap) eased_progress = tail_cap;
    }
    const int32_t visual = s_reveal_anim_from +
        (int32_t)((int64_t)distance * eased_progress / 1000LL);
    apply_reveal_progress(visual, false, false);
}

static void reveal_completed(lv_anim_t *anim) {
    if (s_qr_reveal_active) {
        const bool opening = s_reveal_anim_target == 1000;
        s_reveal_raw = s_reveal_anim_target;
        update_backdrop_progress(s_reveal_anim_target);
        nuwax_face_set_overlay_progress((uint16_t)s_reveal_anim_target);
        /* Present the final mask before restoring live widgets, including the
         * zero-radius close frame. Do not put a native redraw in the last step. */
        if (s_navigation_cached && !s_present_finishing) {
            lv_obj_invalidate(s_root);
            request_presented_finish(CC_PRESENT_REVEAL);
            return;
        }
        const bool cached = s_navigation_cached;
        navigation_cache_end();
        s_qr_reveal_active = false;
        const uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - s_reveal_anim_started_us) / 1000LL);
        ESP_LOGI(TAG, "provisioning QR circular reveal complete direction=%s cached=%d elapsed_ms=%u render_frames=%u max_gap_ms=%u",
                 opening ? "open" : "close", cached, elapsed_ms,
                 s_qr_render_frames, s_qr_render_gap_us / 1000U);
        if (opening) {
            if (!cached) lv_obj_invalidate(s_root);
            return;
        }
        hide_detail();
        /* Continue through the ordinary closed-state cleanup, without ever
         * presenting the menu that used to sit underneath the QR page. */
    }
    if (s_transition_cache_active && s_reveal_anim_target == 1000 && !s_present_finishing) {
        request_presented_finish(CC_PRESENT_REVEAL); return;
    }
    (void)anim;
    apply_reveal_progress(s_reveal_anim_target, false, true);
    if (s_reveal_perf_active) {
        const int64_t elapsed_us = esp_timer_get_time() -
                                   s_reveal_perf_started_us;
        const uint32_t intervals = s_reveal_perf_frames > 0
                                       ? s_reveal_perf_frames - 1
                                       : 0;
        const uint32_t fps_x10 = elapsed_us > 0
                                     ? (uint32_t)(((uint64_t)intervals *
                                                   10000000ULL) /
                                                  (uint64_t)elapsed_us)
                                     : 0;
        const bool target_met =
            fps_x10 >= CONFIG_NUWAX_UI_ANIMATION_TARGET_FPS * 10U;
        const uint32_t backdrop_frames = s_reveal_perf_backdrop_frames;
        const uint32_t backdrop_avg_areas = backdrop_frames > 0
                                                ? s_reveal_perf_backdrop_areas /
                                                      backdrop_frames
                                                : 0;
        const uint32_t backdrop_avg_pixels = backdrop_frames > 0
                                                 ? s_reveal_perf_backdrop_pixels /
                                                       backdrop_frames
                                                 : 0;
        if (target_met) {
            ESP_LOGI(TAG,
                     "control center reveal perf direction=%s frames=%u elapsed_ms=%u fps=%u.%u max_gap_ms=%u skipped_steps=%u backdrop_avg_areas=%u backdrop_avg_px=%u backdrop_max_px=%u target_fps=%u",
                     s_reveal_perf_opening ? "open" : "close",
                     (unsigned)s_reveal_perf_frames,
                     (unsigned)(elapsed_us > 0 ? elapsed_us / 1000 : 0),
                     (unsigned)(fps_x10 / 10),
                     (unsigned)(fps_x10 % 10),
                     (unsigned)(s_reveal_perf_max_gap_us / 1000U),
                     (unsigned)s_reveal_perf_skipped_steps,
                     (unsigned)backdrop_avg_areas,
                     (unsigned)backdrop_avg_pixels,
                     (unsigned)s_reveal_perf_backdrop_max_pixels,
                     (unsigned)CONFIG_NUWAX_UI_ANIMATION_TARGET_FPS);
        } else {
            ESP_LOGW(TAG,
                     "control center reveal below target direction=%s frames=%u elapsed_ms=%u fps=%u.%u max_gap_ms=%u skipped_steps=%u backdrop_avg_areas=%u backdrop_avg_px=%u backdrop_max_px=%u target_fps=%u",
                     s_reveal_perf_opening ? "open" : "close",
                     (unsigned)s_reveal_perf_frames,
                     (unsigned)(elapsed_us > 0 ? elapsed_us / 1000 : 0),
                     (unsigned)(fps_x10 / 10),
                     (unsigned)(fps_x10 % 10),
                     (unsigned)(s_reveal_perf_max_gap_us / 1000U),
                     (unsigned)s_reveal_perf_skipped_steps,
                     (unsigned)backdrop_avg_areas,
                     (unsigned)backdrop_avg_pixels,
                     (unsigned)s_reveal_perf_backdrop_max_pixels,
                     (unsigned)CONFIG_NUWAX_UI_ANIMATION_TARGET_FPS);
        }
        s_reveal_perf_active = false;
    }
    if (s_reveal_anim_target == 0) {
        apply_reveal_progress(0, false, true);
        (void)transition_cache_deactivate();
        set_page_tree_mode(s_page, false);
        s_open = false;
        s_closing = false;
        s_motion_y = CC_HIDDEN_Y;
        s_dragging = false;
        nuwax_face_set_overlay_progress(0);
        nuwax_face_set_interaction_active(false);
        s_backdrop_frozen_face = false;
        if (s_page != CC_PAGE_STATUS) {
            switch_page_without_animation(CC_PAGE_STATUS);
        }
    } else {
        apply_reveal_progress(1000, false, true);
        const bool was_cached = transition_cache_deactivate();
        set_page_tree_mode(s_page, false);
        if (was_cached &&
            (s_page == CC_PAGE_STATUS ? s_status_cache_dirty
                                      : s_menu_cache_dirty)) {
            transition_cache_invalidate_page(s_page);
        }
        s_closing = false;
        start_local_animations();
    }
}

static void cancel_reveal_animation(void) {
    if (s_qr_reveal_active) navigation_cache_end();
    s_qr_reveal_active = false;
    if (s_present_pending == CC_PRESENT_REVEAL) s_present_pending = CC_PRESENT_NONE;
    if (s_reveal_timer) {
        lv_timer_delete(s_reveal_timer);
        s_reveal_timer = NULL;
    }
    s_reveal_anim_step = 0U;
    s_reveal_anim_steps = 0U;
    s_reveal_anim_duration_ms = 0U;
    s_reveal_anim_started_us = 0;
    s_reveal_perf_active = false;
}

static void reveal_timer_cb(lv_timer_t *timer) {
    if (!timer || timer != s_reveal_timer) return;
    const uint32_t planned_step = nuwax_control_center_timeline_step(
        s_reveal_anim_steps,
        s_reveal_anim_duration_ms,
        s_reveal_anim_started_us,
        esp_timer_get_time());
    if (planned_step <= s_reveal_anim_step) return;
    /* Bounded advance: see CC_REVEAL_MAX_STEPS_PER_FRAME. */
    uint32_t applied_step = planned_step;
    if (planned_step < s_reveal_anim_steps &&
        applied_step > s_reveal_anim_step + CC_REVEAL_MAX_STEPS_PER_FRAME) {
        applied_step = s_reveal_anim_step + CC_REVEAL_MAX_STEPS_PER_FRAME;
    }
    if (applied_step > s_reveal_anim_step + 1U) {
        s_reveal_perf_skipped_steps += applied_step - s_reveal_anim_step - 1U;
    }
    s_reveal_anim_step = applied_step;
    apply_reveal_animation_step(s_reveal_anim_step);
    if (s_reveal_anim_step < s_reveal_anim_steps) return;

    s_reveal_timer = NULL;
    lv_timer_delete(timer);
    reveal_completed(NULL);
}

static void animate_reveal(bool opening) {
    if (!s_reveal_clip) return;
    s_input_scene_generation++;
    retire_page_motion();
    stop_local_animations();
    cancel_reveal_animation();
    /* One presentation owner: LVGL establishes the backdrop before the
     * flush filter overlays cached components and restores the frozen face.
     * Never bypass content staging or publish a partially composed surface. */
    (void)transition_cache_activate(false);
    s_reveal_anim_from = s_reveal_raw;
    s_reveal_anim_target = opening ? 1000 : 0;
    s_reveal_perf_active = true;
    s_reveal_perf_opening = opening;
    s_reveal_perf_frames = 1;
    s_reveal_perf_max_gap_us = 0;
    s_reveal_perf_backdrop_frames = 0;
    s_reveal_perf_backdrop_areas = 0;
    s_reveal_perf_backdrop_pixels = 0;
    s_reveal_perf_backdrop_max_pixels = 0;
    s_reveal_perf_skipped_steps = 0;
    s_reveal_perf_started_us = esp_timer_get_time();
    s_reveal_perf_last_frame_us = s_reveal_perf_started_us;
    const uint32_t distance = (uint32_t)(s_reveal_anim_target > s_reveal_anim_from
                                            ? s_reveal_anim_target - s_reveal_anim_from
                                            : s_reveal_anim_from - s_reveal_anim_target);
    uint32_t duration_ms =
        (CC_REVEAL_FULL_MS * distance + 999U) / 1000U;
    if (duration_ms < CC_REVEAL_MIN_MS) {
        duration_ms = CC_REVEAL_MIN_MS;
    }
    s_reveal_anim_duration_ms = duration_ms;
    s_reveal_anim_started_us = s_reveal_perf_started_us;
    s_reveal_anim_steps =
        (duration_ms * CONFIG_NUWAX_UI_ANIMATION_TARGET_FPS + 999U) /
        1000U;
    if (s_reveal_anim_steps == 0) s_reveal_anim_steps = 1;
    s_reveal_anim_step = 0U;
    s_reveal_timer = lv_timer_create(reveal_timer_cb,
                                     CC_REVEAL_FRAME_MS,
                                     NULL);
    if (!s_reveal_timer) {
        ESP_LOGE(TAG,
                 "control-center reveal timer allocation failed; settling immediately");
        apply_reveal_animation_step(s_reveal_anim_steps);
        reveal_completed(NULL);
    }
}

static void hide_detail(void) {
    s_qr_direct_open = false;
    cancel_click();
    cancel_sheet();
    nuwax_device_qr_view_hide();
    if (s_info_page) {
        lv_obj_add_flag(s_info_page, LV_OBJ_FLAG_HIDDEN);
    }
    s_detail = CC_DETAIL_NONE;
}

static void open_control_center(cc_page_t page) {
    if (s_ota_blocked || !s_root || !s_surface) {
        return;
    }
    const bool reversing_close = s_open && s_closing;
    retire_page_motion();
    hide_detail();
    if (s_page != page) {
        set_page(page, false);
    }
    if (s_open && !s_closing) {
        return;
    }
    if (!reversing_close) {
        render_page_if_dirty(page);
        s_open = true;
    }
    s_open = true;
    s_closing = false;
    s_dragging = false;
    s_motion_y = CC_OPEN_Y;
    const esp_err_t freeze_err = nuwax_face_freeze_interaction();
    s_backdrop_frozen_face = nuwax_face_frozen_frame_ready();
    if (freeze_err != ESP_OK) {
        ESP_LOGW(TAG,
                 "control center face freeze degraded err=%s frozen_frame=%d",
                 esp_err_to_name(freeze_err),
                 s_backdrop_frozen_face ? 1 : 0);
    }
    nuwax_face_set_overlay_progress((uint16_t)s_reveal_raw);
    animate_reveal(true);
    ESP_LOGI(TAG, "control center opened page=%s frozen_face=%d",
             page == CC_PAGE_STATUS ? "status" : "menu",
             s_backdrop_frozen_face ? 1 : 0);
}

esp_err_t nuwax_drawer_create(lv_obj_t *parent) {
    if (!parent) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_root) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!control_center_memory_preflight()) {
        return ESP_ERR_NO_MEM;
    }
    s_root = lv_obj_create(parent);
    if (!s_root) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_size(s_root, CC_W, CC_H);
    lv_obj_set_style_radius(s_root, LV_RADIUS_CIRCLE, 0);
    /* The physical panel is already round. Full-screen software clipping costs
     * hundreds of milliseconds on ESP32-S3 and provides no visible benefit. */
    lv_obj_set_style_clip_corner(s_root, false, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    use_layout_only_draw(s_root);
    s_reveal_clip = lv_obj_create(s_root);
    if (!s_reveal_clip) {
        nuwax_drawer_deinit();
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_pos(s_reveal_clip, 0, 0);
    lv_obj_set_size(s_reveal_clip, CC_W, CC_H);
    lv_obj_set_style_radius(s_reveal_clip, 0, 0);
    /* The backdrop stays fixed. Its draw callback uses incremental dirty bands
     * so changing reveal progress never invalidates a full-screen object. */
    lv_obj_set_style_clip_corner(s_reveal_clip, false, 0);
    lv_obj_set_style_bg_opa(s_reveal_clip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_reveal_clip, 0, 0);
    lv_obj_set_style_pad_all(s_reveal_clip, 0, 0);
    lv_obj_remove_flag(s_reveal_clip,
                       LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    use_layout_only_draw(s_reveal_clip);
    lv_obj_add_event_cb(s_reveal_clip, backdrop_cover_cb,
                        (lv_event_code_t)(LV_EVENT_COVER_CHECK |
                                          LV_EVENT_PREPROCESS),
                        NULL);

    s_surface = lv_obj_create(s_root);
    if (!s_surface) {
        nuwax_drawer_deinit();
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_pos(s_surface, 0, 0);
    lv_obj_set_size(s_surface, CC_W, CC_H);
    lv_obj_set_style_radius(s_surface, 0, 0);
    lv_obj_set_style_clip_corner(s_surface, false, 0);
    lv_obj_set_style_bg_opa(s_surface, LV_OPA_TRANSP, 0);
    use_layout_only_draw(s_surface);
    lv_obj_set_style_border_width(s_surface, 0, 0);
    lv_obj_set_style_pad_all(s_surface, 0, 0);
    lv_obj_remove_flag(s_surface,
                       LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    const char *failed_stage = NULL;
    if (!create_status_page()) {
        failed_stage = "status_page";
    } else if (!create_menu_page()) {
        failed_stage = "menu_page";
#if NUWAX_TEST_PANEL_ENABLED
    } else if (!(s_test_page = nuwax_test_panel_create(s_surface))) {
        failed_stage = "test_page";
#endif
    } else if (!create_page_dots(s_surface)) {
        failed_stage = "page_indicator";
    } else if (!create_info_page()) {
        failed_stage = "device_info";
    }
    if (failed_stage) {
        ESP_LOGE(TAG, "control-center creation failed stage=%s",
                 failed_stage);
        nuwax_drawer_deinit();
        return ESP_ERR_NO_MEM;
    }
    if (!nuwax_device_qr_view_create(s_surface)) {
        ESP_LOGE(TAG, "failed to create control-center QR detail");
        nuwax_drawer_deinit();
        return ESP_ERR_NO_MEM;
    }
    s_cache_plane = lv_obj_create(s_root);
    if (!s_cache_plane) {
        nuwax_drawer_deinit();
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(s_cache_plane);
    lv_obj_set_size(s_cache_plane, CC_W, CC_H);
    lv_obj_remove_flag(s_cache_plane, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_cache_plane, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_cache_plane, cache_plane_draw_cb,
        (lv_event_code_t)(LV_EVENT_ALL | LV_EVENT_PREPROCESS), NULL);
    s_pending_timer = lv_timer_create(pending_timer_cb, 250, NULL);
    if (!s_pending_timer) {
        ESP_LOGE(TAG, "failed to create control-center transaction timer");
        nuwax_drawer_deinit();
        return ESP_ERR_NO_MEM;
    }
    s_page = CC_PAGE_STATUS;
    s_page_progress = -1;
    s_page_dragging = false;
    s_page_drag_origin_progress = 0;
    s_page_transition_active = false;
    apply_page_progress(0, false);
    set_page_tree_mode(CC_PAGE_STATUS, false);
    s_detail = CC_DETAIL_NONE;
    s_open = false;
    s_closing = false;
    s_dragging = false;
    s_motion_y = CC_HIDDEN_Y;
    s_reveal_raw = -1;
    s_backdrop_progress = -1;
    s_backdrop_frozen_face = false;
    apply_reveal_progress(0, false, false);
    (void)transition_cache_bind_sources();
    lv_display_add_event_cb(lv_obj_get_display(s_root), presented_frame_cb, LV_EVENT_REFR_READY, NULL);
    nuwax_lvgl_port_set_flush_filter(control_center_flush_filter, NULL);
    lv_display_add_event_cb(lv_obj_get_display(s_root), cached_scene_frame_cb, LV_EVENT_RENDER_START, NULL);
    lv_display_set_scene_render_cb(lv_obj_get_display(s_root), cached_scene_render);
    ESP_LOGI(TAG,
             "control center created pages=status,menu details=qr,device_info transition=rgb565-component-cache cache=%d psram_free=%u psram_largest=%u",
             s_transition_cache_available ? 1 : 0,
             (unsigned)heap_caps_get_free_size(
                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(
                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    return ESP_OK;
}

void nuwax_drawer_deinit(void) {
    s_input_scene_generation++;
    s_ota_blocked = false;
    s_ota_overlay_owner = false;
    s_power_overlay_owner = false;
    s_scene_frame_ready = false;
    if (s_root) {
        lv_display_set_scene_render_cb(lv_obj_get_display(s_root), NULL);
        lv_display_remove_event_cb_with_user_data(lv_obj_get_display(s_root), cached_scene_frame_cb, NULL);
    }
    if (s_root) lv_display_remove_event_cb_with_user_data(lv_obj_get_display(s_root), presented_frame_cb, NULL);
    s_present_pending = CC_PRESENT_NONE;
    s_present_finishing = false;
    nuwax_lvgl_port_set_flush_filter(NULL, NULL);
    navigation_cache_release();
    transition_cache_release();
    stop_local_animations();
    if (s_surface) {
        lv_anim_delete(s_surface, page_progress_exec);
    }
    cancel_page_perf();
    s_page_transition_active = false;
    s_transition_cache_failure_logged = false;
    s_status_cache_dirty = true;
    s_menu_cache_dirty = true;
    cancel_reveal_animation();
    s_volume_visual = -1;
    s_volume_cache_settle_us = 0;
    s_meeting_visual = -1;
    s_provision_visual = -1;
    s_brightness_visual = -1;
    if (s_pending_timer) {
        lv_timer_delete(s_pending_timer);
        s_pending_timer = NULL;
    }
    hide_detail();
    nuwax_face_set_overlay_progress(0);
    nuwax_face_set_interaction_active(false);
    if (s_root) {
        lv_obj_delete(s_root);
    }
    nuwax_device_qr_view_reset();
    s_root = NULL;
    s_cache_plane = NULL;
    s_reveal_clip = NULL;
    s_surface = NULL;
    s_status_page = NULL;
    s_menu_page = NULL;
#if NUWAX_TEST_PANEL_ENABLED
    s_test_page = NULL;
    s_test_dot = NULL;
    s_test_page_drag = false;
    s_test_page_drag_dx = 0;
    nuwax_test_panel_reset();
#endif
    s_info_page = NULL;
    s_info_back = NULL;
    s_tab_indicator = NULL;
    s_contact_down = false;
    s_pager = NULL;
    s_pager_clip = NULL;
    s_status_header = NULL;
    s_status_header_clip = NULL;
    s_menu_header = NULL;
    s_menu_header_clip = NULL;
    s_status_dot = NULL;
    s_menu_dot = NULL;
    memset(s_tab_labels, 0, sizeof(s_tab_labels));
    s_cloud_beacon = NULL;
    s_battery_fill = NULL;
    s_battery_charging_mark = NULL;
    s_footer = NULL;
    s_footer_clip = NULL;
    s_footer_dot = NULL;
    s_footer_label = NULL;
    memset(s_status_cards, 0, sizeof(s_status_cards));
    memset(s_menu_cards, 0, sizeof(s_menu_cards));
    memset(s_menu_card_clips, 0, sizeof(s_menu_card_clips));
    s_meeting_state = NULL;
    s_meeting_switch = NULL;
    s_meeting_knob = NULL;
    s_provision_label = NULL;
    s_brightness_icon = NULL;
    memset(s_brightness_bars, 0, sizeof(s_brightness_bars));
    s_volume_text = NULL;
    s_menu_volume = NULL;
    s_volume_bar = NULL;
    s_volume_fill = NULL;
    s_volume_knob = NULL;
#if NUWAX_TEST_NETWORK_ENABLED
    /* Test widget handles are reset by their owner below. */
#endif
    s_info_id = NULL;
    s_info_version = NULL;
    s_info_version_title = NULL;
    s_info_ssid = NULL;
    s_info_ip = NULL;
    memset(&s_state, 0, sizeof(s_state));
    s_have_state = false;
    s_status_dirty = true;
    s_menu_dirty = true;
    s_info_dirty = true;
    s_open = false;
    s_closing = false;
    s_dragging = false;
    s_cloud_animating = false;
    s_battery_animating = false;
    s_wifi_connect_phase = 0;
    s_wifi_connect_last_us = 0;
    s_motion_y = CC_HIDDEN_Y;
    s_reveal_raw = 0;
    s_reveal_anim_from = 0;
    s_reveal_anim_target = 0;
    s_backdrop_progress = 0;
    s_backdrop_frozen_face = false;
    s_reveal_anim_steps = 0;
    s_reveal_perf_active = false;
    s_reveal_perf_opening = false;
    s_reveal_perf_frames = 0;
    s_reveal_perf_max_gap_us = 0;
    s_reveal_perf_backdrop_frames = 0;
    s_reveal_perf_backdrop_areas = 0;
    s_reveal_perf_backdrop_pixels = 0;
    s_reveal_perf_backdrop_max_pixels = 0;
    s_reveal_perf_started_us = 0;
    s_reveal_perf_last_frame_us = 0;
    s_page_progress = 0;
    s_page = CC_PAGE_STATUS;
    s_page_perf_active = false;
    s_page_perf_frames = 0U;
    s_page_perf_max_gap_us = 0U;
    s_page_perf_started_us = 0;
    s_page_perf_last_frame_us = 0;
    s_page_dragging = false;
    s_page_drag_origin_progress = 0;
    s_page_transition_active = false;
    s_page_transition_target = CC_PAGE_STATUS;
    s_page_transition_settle_page = CC_PAGE_STATUS;
    s_detail = CC_DETAIL_NONE;
    s_meeting_pending = false;
    s_meeting_pending_target = false;
    s_provision_pending = false;
    s_provision_pending_target = false;
    s_pending_deadline_us = 0;
    s_provision_pending_deadline_us = 0;
#if NUWAX_TEST_NETWORK_ENABLED
    s_offline_test_pending = false;
    s_offline_test_pending_target = false;
    s_offline_test_pending_deadline_us = 0;
#endif
}

void nuwax_drawer_update(const nuwax_device_state_t *state) {
    if (!state) {
        return;
    }
    const bool status_changed =
        !s_have_state || state_affects_status(&s_state, state);
    bool menu_changed =
        !s_have_state || state_affects_menu(&s_state, state);
    const bool volume_changed = !s_have_state || s_state.volume != state->volume;
    const bool info_changed =
        !s_have_state || state_affects_info(&s_state, state);
    const bool meeting_pending_before = s_meeting_pending;
    const bool provision_pending_before = s_provision_pending;
    s_state = *state;
    s_have_state = true;
    s_status_dirty = s_status_dirty || status_changed;
    s_menu_dirty = s_menu_dirty || menu_changed;
    s_info_dirty = s_info_dirty || info_changed;
    if (s_meeting_pending) {
        const bool settled = s_meeting_pending_target
                                 ? state->meeting == NUWAX_MEETING_UI_ACTIVE ||
                                       state->meeting == NUWAX_MEETING_UI_ERROR
                                 : state->meeting == NUWAX_MEETING_UI_DISABLED ||
                                       state->meeting == NUWAX_MEETING_UI_ERROR;
        if (settled) {
            s_meeting_pending = false;
            s_pending_deadline_us = 0;
        }
    }
    if (s_provision_pending &&
        state->provisioning_active == s_provision_pending_target) {
        s_provision_pending = false;
        s_provision_pending_deadline_us = 0;
    }
#if NUWAX_TEST_NETWORK_ENABLED
    if (s_offline_test_pending &&
        state->simulated_offline == s_offline_test_pending_target) {
        s_offline_test_pending = false;
        s_offline_test_pending_deadline_us = 0;
    }
#endif
    menu_changed = menu_changed ||
                   meeting_pending_before != s_meeting_pending ||
                   provision_pending_before != s_provision_pending;
    s_menu_dirty = s_menu_dirty || menu_changed;
    if (volume_changed) {
        if (s_open && !s_closing && (s_page == CC_PAGE_MENU || s_page_transition_active)) {
            set_volume_visual(state->volume);
        } else {
            s_menu_dirty = true;
        }
    }
    if (s_open && !s_closing) {
        if (s_status_dirty &&
            (s_page == CC_PAGE_STATUS || s_page_transition_active)) {
            render_status();
        }
        if (s_menu_dirty &&
            (s_page != CC_PAGE_STATUS || s_page_transition_active)) {
            render_menu();
        }
        if (s_info_dirty && s_detail == CC_DETAIL_INFO) {
            render_info();
        }
    }
}

void nuwax_drawer_open(void) {
    open_control_center(CC_PAGE_STATUS);
}

void nuwax_drawer_open_menu(void) {
    open_control_center(CC_PAGE_MENU);
}

void nuwax_drawer_begin_pull(void) {
    if (s_ota_blocked || !s_root || !s_surface || s_open) return;
    s_input_scene_generation++;
    retire_page_motion();
    hide_detail();
    if (s_page != CC_PAGE_STATUS) {
        set_page(CC_PAGE_STATUS, false);
    }
    stop_local_animations();
    render_page_if_dirty(CC_PAGE_STATUS);
    (void)transition_cache_activate(false);
    s_open = true;
    s_closing = false;
    s_dragging = true;
    s_motion_y = CC_HIDDEN_Y;
    const esp_err_t freeze_err = nuwax_face_freeze_interaction();
    s_backdrop_frozen_face = nuwax_face_frozen_frame_ready();
    if (freeze_err != ESP_OK) {
        ESP_LOGW(TAG,
                 "control center pull freeze degraded err=%s frozen_frame=%d",
                 esp_err_to_name(freeze_err),
                 s_backdrop_frozen_face ? 1 : 0);
    }
    cancel_reveal_animation();
    apply_reveal_progress(0, false, false);
}

void nuwax_drawer_close(void) {
#if NUWAX_TEST_PANEL_ENABLED
    s_test_page_drag = false;
    s_test_page_drag_dx = 0;
#endif
    if (!s_open || !s_reveal_clip || s_closing) {
        return;
    }
    if (s_page_transition_active) {
        lv_anim_delete(s_surface, page_progress_exec);
        cancel_page_perf();
        settle_page_transition(s_page_transition_settle_page);
        s_page_dragging = false;
    }
    if (s_qr_direct_open && s_detail == CC_DETAIL_QR && !s_sheet_active) {
        start_provisioning_qr_reveal(false);
        return;
    }
    s_closing = true;
    hide_detail();
    stop_local_animations();
    animate_reveal(false);
    ESP_LOGI(TAG, "control center closing");
}

static void set_modal_blocked(bool blocked) {
    if (s_ota_blocked == blocked) return;
    s_input_scene_generation++;
    s_ota_blocked = blocked;
    if (!blocked || !s_root || !s_surface) return;

    /* Retire every callback before releasing shared caches. Closing via an
     * animation is insufficient: its flush filter runs AFTER LVGL's top layer.
     * OTA has already paused the face, so ending interaction cannot resume it. */
    cancel_reveal_animation();
    lv_anim_delete(s_surface, page_progress_exec);
    cancel_page_perf();
    s_page_transition_active = false;
    s_page_dragging = false;
    hide_detail(); /* Also cancels delayed clicks and detail sheet motion. */
    clear_press();
    s_contact_down = false;
    stop_local_animations();
    s_present_pending = CC_PRESENT_NONE;
    s_present_finishing = false;
    s_scene_frame_ready = false;
    s_scene_pager_visible = false;
#if NUWAX_TEST_PANEL_ENABLED
    s_test_page_drag = false;
    s_test_page_drag_dx = 0;
#endif
    s_reveal_anim_target = 0;
    reveal_completed(NULL);
    ESP_LOGI(TAG, "OTA display takeover revision=ota-drawer-v1 motion=retired compositor=blocked");
}

void nuwax_drawer_set_ota_blocked(bool blocked) {
    s_ota_overlay_owner = blocked;
    set_modal_blocked(s_ota_overlay_owner || s_power_overlay_owner);
}

void nuwax_drawer_set_power_blocked(bool blocked) {
    s_power_overlay_owner = blocked;
    set_modal_blocked(s_ota_overlay_owner || s_power_overlay_owner);
}

bool nuwax_drawer_is_open(void) {
    return s_open;
}

bool nuwax_drawer_is_detail(void) {
    return s_open && s_detail != CC_DETAIL_NONE;
}

bool nuwax_drawer_is_menu_page(void) {
    return s_open && s_page == CC_PAGE_MENU;
}

void nuwax_drawer_next_page(void) {
    if (s_open && s_detail == CC_DETAIL_NONE) {
#if NUWAX_TEST_PANEL_ENABLED
        if (s_page == CC_PAGE_MENU) set_page(CC_PAGE_TEST, true);
        else if (s_page == CC_PAGE_STATUS) set_page(CC_PAGE_MENU, true);
#else
        set_page(CC_PAGE_MENU, true);
#endif
    }
}

void nuwax_drawer_prev_page(void) {
    if (s_open && s_detail == CC_DETAIL_NONE) {
#if NUWAX_TEST_PANEL_ENABLED
        if (s_page == CC_PAGE_TEST) set_page(CC_PAGE_MENU, true);
        else set_page(CC_PAGE_STATUS, true);
#else
        set_page(CC_PAGE_STATUS, true);
#endif
    }
}

#define CC_NAV_TILES 10U
typedef struct {
    lv_obj_t *source;
    lv_draw_buf_t buffer;
    lv_area_t area;
    bool visible;
} cc_nav_tile_t;
typedef struct {
    lv_obj_t *root;
    cc_nav_tile_t tiles[CC_NAV_TILES];
    uint8_t *arena;
    size_t bytes;
    unsigned count, next;
    bool ready;
    char key[384];
    uint32_t work_us, max_tile_us;
} cc_nav_cache_t;
/* Descriptors and keys also belong in PSRAM: internal SRAM is reserved for
 * transport, audio and task stacks, even in builds without external BSS. */
static cc_nav_cache_t *s_nav_cache;
static cc_nav_cache_t *s_nav_active;
static bool s_nav_allocation_failed;

static bool navigation_key(unsigned index, char *key, size_t size) {
    const char *id = nuwax_device_uid_string();
    const esp_app_desc_t *app = esp_app_get_description();
    const char *payload = nuwax_provision_main_qr_payload();
#if CONFIG_NUWAX_UI_DISPLAY_TEST_MODE
    if (!payload || !payload[0]) payload = "NUWAX:NX-DISPLAY-TEST";
#endif
    const int n = index == 0 ? snprintf(key, size, "%s\n%s", id, payload ? payload : "") :
        snprintf(key, size, "%s\n%s\n%s\n%s", id, app ? app->version : "--", s_state.ssid, s_state.ip);
    return n >= 0 && (size_t)n < size;
}
static bool navigation_bind(cc_nav_cache_t *cache, lv_obj_t *root) {
    if (cache->arena) return true;
    if (s_nav_allocation_failed) return false;
    cache->root = root;
    cache->count = lv_obj_get_child_count(root);
    if (!cache->count || cache->count > CC_NAV_TILES) return false;
    lv_obj_update_layout(root);
    size_t bytes = 0;
    for (unsigned i = 0; i < cache->count; ++i) {
        cc_nav_tile_t *t = &cache->tiles[i];
        t->source = lv_obj_get_child(root, i);
        const int ext = lv_obj_get_ext_draw_size(t->source);
        const int w = lv_obj_get_width(t->source) + 2 * ext, h = lv_obj_get_height(t->source) + 2 * ext;
        if (w <= 0 || h <= 0) return false;
        bytes = align_size(bytes, CC_TRANSITION_CACHE_ALIGNMENT);
        bytes += lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_RGB565) * h;
    }
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) <
            bytes + CC_PSRAM_RUNTIME_RESERVE_BYTES ||
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) < bytes)
        return false;
    cache->arena = heap_caps_aligned_alloc(CC_TRANSITION_CACHE_ALIGNMENT, bytes,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!cache->arena) { s_nav_allocation_failed = true; return false; }
    cache->bytes = bytes;
    size_t offset = 0;
    for (unsigned i = 0; i < cache->count; ++i) {
        cc_nav_tile_t *t = &cache->tiles[i];
        const int ext = lv_obj_get_ext_draw_size(t->source);
        const int w = lv_obj_get_width(t->source) + 2 * ext, h = lv_obj_get_height(t->source) + 2 * ext;
        const uint32_t stride = lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_RGB565);
        offset = align_size(offset, CC_TRANSITION_CACHE_ALIGNMENT);
        if (lv_draw_buf_init(&t->buffer, w, h, LV_COLOR_FORMAT_RGB565, stride,
                            cache->arena + offset, stride * h) != LV_RESULT_OK) {
            heap_caps_free(cache->arena); cache->arena = NULL; cache->bytes = 0;
            s_nav_allocation_failed = true; return false;
        }
        offset += stride * h;
    }
    ESP_LOGI(TAG, "navigation cache allocated tiles=%u bytes=%u reserve=%u",
             cache->count, (unsigned)bytes, (unsigned)CC_PSRAM_RUNTIME_RESERVE_BYTES);
    return true;
}
/* Follow LVGL's snapshot layer lifecycle, but render the owning page clipped to
 * one tile. Text extents can overlap neighbouring widgets: snapshotting each
 * child against black independently would erase those neighbouring glyphs. */
static void navigation_snapshot_tile(cc_nav_cache_t *cache, cc_nav_tile_t *tile) {
    lv_layer_t layer;
    lv_layer_init(&layer);
    layer.draw_buf = &tile->buffer;
    layer.buf_area = tile->area;
    layer.color_format = LV_COLOR_FORMAT_RGB565;
    layer._clip_area = tile->area;
    layer.phy_clip_area = tile->area;
    lv_draw_buf_clear(&tile->buffer, NULL);
    lv_draw_unit_send_event(NULL, LV_EVENT_CHILD_CREATED, &layer);
    lv_display_t *old_display = lv_refr_get_disp_refreshing();
    lv_display_t *display = lv_obj_get_display(cache->root);
    lv_layer_t *old_layer = display->layer_head;
    display->layer_head = &layer;
    lv_refr_set_disp_refreshing(display);
    lv_obj_redraw(&layer, cache->root);
    layer.all_tasks_added = true;
    while (layer.draw_task_head) {
        lv_draw_dispatch_wait_for_request();
        lv_draw_dispatch();
    }
    display->layer_head = old_layer;
    lv_refr_set_disp_refreshing(old_display);
    lv_draw_unit_send_event(NULL, LV_EVENT_SCREEN_LOAD_START, &layer);
    lv_draw_unit_send_event(NULL, LV_EVENT_CHILD_DELETED, &layer);
}
static void navigation_cache_tick(void) {
    if (!s_have_state || !s_info_page || !nuwax_device_qr_view_root() ||
        s_detail != CC_DETAIL_NONE || s_sheet_active || s_navigation_cached ||
        s_page_transition_active || s_click_timer || s_pressed || s_dragging ||
        s_closing || (s_open && s_reveal_raw != 1000)) return;
    if (!s_nav_cache) {
        const size_t bytes = 2U * sizeof(*s_nav_cache);
        if (s_nav_allocation_failed ||
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) <
                bytes + CC_PSRAM_RUNTIME_RESERVE_BYTES) return;
        s_nav_cache = heap_caps_aligned_alloc(CC_TRANSITION_CACHE_ALIGNMENT, bytes,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_nav_cache) { s_nav_allocation_failed = true; return; }
        memset(s_nav_cache, 0, bytes);
    }
    /* At most one small widget snapshot per idle tick. No full-page capture. */
    for (unsigned index = 0; index < 2; ++index) {
        cc_nav_cache_t *cache = &s_nav_cache[index];
        char key[384];
        if (!navigation_key(index, key, sizeof(key))) continue;
        if (strcmp(key, cache->key) != 0) {
            cache->ready = false; cache->next = 0; cache->work_us = cache->max_tile_us = 0;
            memcpy(cache->key, key, strlen(key) + 1U);
            if (index == 0) {
                const char *payload = nuwax_provision_main_qr_payload();
#if CONFIG_NUWAX_UI_DISPLAY_TEST_MODE
                if (!payload || !payload[0]) payload = "NUWAX:NX-DISPLAY-TEST";
#endif
                (void)nuwax_device_qr_view_show(payload, nuwax_device_uid_string());
                nuwax_device_qr_view_hide();
            } else render_info();
        }
        if (cache->ready) continue;
        if (!navigation_bind(cache, index == 0 ? nuwax_device_qr_view_root() : s_info_page)) continue;
        cc_nav_tile_t *tile = &cache->tiles[cache->next];
        tile->visible = !lv_obj_has_flag(tile->source, LV_OBJ_FLAG_HIDDEN);
        lv_obj_update_layout(tile->source);
        lv_obj_get_coords(tile->source, &tile->area);
        const int ext = lv_obj_get_ext_draw_size(tile->source);
        lv_area_increase(&tile->area, ext, ext);
        const int64_t started = esp_timer_get_time();
        if (tile->visible && lv_snapshot_reshape_draw_buf(tile->source,
                &tile->buffer) != LV_RESULT_OK) {
            ESP_LOGW(TAG, "navigation snapshot failed tile=%u; motion disabled", cache->next);
            heap_caps_free(cache->arena); cache->arena = NULL; cache->ready = false;
            cache->next = 0; s_nav_allocation_failed = true; return;
        }
        if (tile->visible) navigation_snapshot_tile(cache, tile);
        const uint32_t elapsed = (uint32_t)(esp_timer_get_time() - started);
        cache->work_us += elapsed;
        if (elapsed > cache->max_tile_us) cache->max_tile_us = elapsed;
        if (++cache->next == cache->count) {
            cache->ready = true;
            ESP_LOGI(TAG, "navigation cache ready page=%s work_us=%u max_tile_us=%u bytes=%u",
                index ? "info" : "qr", cache->work_us, cache->max_tile_us, (unsigned)cache->bytes);
        }
        return;
    }
}
static bool navigation_cache_begin(lv_obj_t *obj) {
    if (!s_nav_cache) return false;
    unsigned index = obj == nuwax_device_qr_view_root() ? 0 : 1;
    if (obj != nuwax_device_qr_view_root() && obj != s_info_page) return false;
    cc_nav_cache_t *cache = &s_nav_cache[index];
    char key[384];
    if (!cache->ready || !navigation_key(index, key, sizeof(key)) || strcmp(key, cache->key)) return false;
    /* Keep the stationary menu cached as well; otherwise it would be repainted
     * under the hidden foreground on every frame of the slide. */
    if (!transition_cache_activate(false)) return false;
    s_nav_active = cache; s_navigation_cached = true;
    const bool suspended = suspend_display_invalidation(obj);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    resume_display_invalidation(obj, suspended);
    return true;
}
static void navigation_cache_end(void) {
    if (!s_nav_active) return;
    lv_obj_remove_flag(s_nav_active->root, LV_OBJ_FLAG_HIDDEN);
    s_nav_active = NULL; s_navigation_cached = false;
    if (transition_cache_deactivate()) set_page_tree_mode(s_page, false);
}
static void navigation_cache_release(void) {
    navigation_cache_end();
    for (unsigned i = 0; s_nav_cache && i < 2; ++i) {
        heap_caps_free(s_nav_cache[i].arena);
    }
    heap_caps_free(s_nav_cache);
    s_nav_cache = NULL;
    s_nav_allocation_failed = false;
}
static void navigation_cache_damage(int32_t old_x, int32_t new_x) {
    if (!s_nav_active) return;
    /* Clear the newly covered/exposed strip, plus each moved tile's old/new
     * bounds. Empty black margins do not need a full-screen redraw. */
    const int32_t lo = old_x < new_x ? old_x : new_x;
    const int32_t hi = old_x > new_x ? old_x : new_x;
    const int32_t left = lo < 0 ? 0 : lo;
    const int32_t right = hi > CC_W ? CC_W : hi;
    if (right > left) invalidate_surface_region(left, 0, right - left, CC_H);
    for (unsigned i = 0; i < s_nav_active->count; ++i) {
        const cc_nav_tile_t *t = &s_nav_active->tiles[i];
        if (!t->visible) continue;
        int32_t x1 = t->area.x1 + lo, x2 = t->area.x2 + hi;
        if (x1 < 0) x1 = 0;
        if (x2 >= CC_W) x2 = CC_W - 1;
        if (x2 >= x1) invalidate_surface_region(x1, t->area.y1, x2 - x1 + 1,
                                               t->area.y2 - t->area.y1 + 1);
    }
}
static void navigation_cache_compose(int32_t x, int32_t y, int32_t w, int32_t h,
                                     uint16_t *pixels, size_t stride) {
    if (!s_nav_active || !s_navigation_cached) return;
    const int32_t offset = lv_obj_get_x(s_nav_active->root);
    int32_t first = offset > x ? offset : x;
    int32_t last = offset + CC_W < x + w ? offset + CC_W : x + w;
    if (last <= first) return;
    for (int32_t row = 0; row < h; ++row)
        memset(pixels + (size_t)row * stride + first - x, 0, (last - first) * 2U);
    for (unsigned i = 0; i < s_nav_active->count; ++i) {
        const cc_nav_tile_t *t = &s_nav_active->tiles[i];
        if (!t->visible) continue;
        const int32_t tx = t->area.x1 + offset, ty = t->area.y1;
        const int32_t x1 = tx > x ? tx : x, y1 = ty > y ? ty : y;
        const int32_t x2 = t->area.x2 + offset < x+w-1 ? t->area.x2 + offset : x+w-1;
        const int32_t y2 = t->area.y2 < y+h-1 ? t->area.y2 : y+h-1;
        if (x1 > x2 || y1 > y2) continue;
        const uint16_t *src = (const uint16_t *)t->buffer.data;
        for (int32_t row = y1; row <= y2; ++row)
            copy_rgb565_span(pixels + (size_t)(row-y)*stride + x1-x,
                src + (size_t)(row-ty)*(t->buffer.header.stride/2U) + x1-tx, x2-x1+1);
    }
}

/* Contact feedback changes one small widget only; all calls own LVGL. */
static void clear_press(void) {
    if (s_pressed) {
        s_pager_cache_valid = false;
        lv_obj_set_style_bg_color(s_pressed, s_pressed_bg, 0);
        /* Page tabs are transparent above the moving selection pill. */
        if (s_pressed == s_status_dot || s_pressed == s_menu_dot
#if NUWAX_TEST_PANEL_ENABLED
            || s_pressed == s_test_dot
#endif
        ) lv_obj_set_style_bg_opa(s_pressed, LV_OPA_TRANSP, 0);
        s_pressed = NULL;
    }
}
static void press_widget(lv_obj_t *obj) {
    if (obj == s_pressed) return;
    clear_press();
    if (!obj) return;
    s_pager_cache_valid = false;
    s_pressed = obj;
    s_pressed_bg = lv_obj_get_style_bg_color(obj, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x484848), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
}
static void cancel_click(void) {
    if (s_click_timer) { lv_timer_delete(s_click_timer); s_click_timer = NULL; }
    s_click_target = 0;
    clear_press();
}
static lv_obj_t *contact_target(uint16_t x, uint16_t y) {
    if (s_detail != CC_DETAIL_NONE)
        return nuwax_detail_hit_back(x, y) ? (s_detail == CC_DETAIL_QR ?
            nuwax_device_qr_view_back_button() : s_info_back) : NULL;
    if (point_in_rect(x, y, CC_PAGER_X, CC_PAGER_Y, CC_PAGER_W, 26)) {
#if NUWAX_TEST_PANEL_ENABLED
        const int dx = x - CC_PAGER_X;
        return dx < 60 ? s_status_dot : dx >= 69 && dx < 129 ? s_menu_dot :
               dx >= 138 ? s_test_dot : NULL;
#else
        return x >= CC_PAGER_X+17 && x < CC_PAGER_X+89 ? s_status_dot :
               x >= CC_PAGER_X+109 && x < CC_PAGER_X+181 ? s_menu_dot : NULL;
#endif
    }
    if (s_page == CC_PAGE_MENU) {
        for (unsigned i = 0; i < 4; ++i)
            if (point_in_rect(x, y, i%2 ? CC_CARD_RIGHT_X : CC_CARD_LEFT_X,
                    i < 2 ? CC_MENU_ROW0_Y : CC_MENU_ROW1_Y, CC_CARD_W, CC_MENU_ACTION_H))
                return s_menu_cards[i];
    }
    return NULL;
}
void nuwax_drawer_contact(bool down, uint16_t x, uint16_t y) {
    if (!down) { s_contact_down = false; if (!s_click_timer) clear_press(); return; }
    if (!drawer_accepts_tap()) return;
    if (!s_contact_down) {
        s_contact_down = true; s_contact_x = x; s_contact_y = y;
        press_widget(contact_target(x, y));
    } else {
        int dx = (int)x - s_contact_x, dy = (int)y - s_contact_y;
        if (nuwax_cc_contact_moved(dx, dy) || contact_target(x,y) != s_pressed)
            clear_press(); /* No re-arming after sliding out of the original button. */
    }
}
static void click_completed(lv_timer_t *timer) {
    const unsigned target = s_click_target;
    s_click_timer = NULL; s_click_target = 0;
    lv_timer_delete(timer); clear_press();
    if (!s_open || s_closing || s_sheet_active) return;
    if (target == 2) show_qr();
    else if (target == 3) show_info();
    else if (target == 4) nuwax_drawer_back();
}
static void click_then_navigate(unsigned target, lv_obj_t *obj) {
    cancel_click();
    if (target != 0U) s_input_scene_generation++;
    press_widget(obj);
    s_click_target = target;
    /* Keep short taps visible for three 40 Hz frames; never block the UI task. */
    s_click_timer = lv_timer_create(click_completed, 75, NULL);
    if (!s_click_timer) {
        s_click_target = 0; clear_press();
        if (target == 2) show_qr();
        else if (target == 3) show_info();
        else if (target == 4) nuwax_drawer_back();
    }
}
static void sheet_exec(void *obj, int32_t x) {
    if (lv_obj_get_x(obj) == x) return;
    const int64_t frame_now = esp_timer_get_time();
    if (x != s_sheet_to && s_sheet_last_us &&
        frame_now - s_sheet_last_us < CC_LOCAL_ANIM_FRAME_INTERVAL_US) return;
    const int32_t previous_x = lv_obj_get_x(obj);
    lv_obj_set_x(obj, x); /* Cached tree is hidden; explicitly damage old/new positions. */
    navigation_cache_damage(previous_x, x);
    const int64_t now = esp_timer_get_time();
    if (s_sheet_last_us && now > s_sheet_last_us) {
        const unsigned gap = (unsigned)(now - s_sheet_last_us);
        if (gap > s_sheet_gap_us) s_sheet_gap_us = gap;
    }
    s_sheet_last_us = now; s_sheet_frames++;
    if (s_sheet_tab_from >= 0) update_page_dots();
}
static void cancel_sheet(void) {
    if (s_present_pending == CC_PRESENT_SHEET) s_present_pending = CC_PRESENT_NONE;
    navigation_cache_end();
    if (s_sheet) { lv_anim_delete(s_sheet, sheet_exec); lv_obj_set_x(s_sheet, 0); }
    s_sheet = NULL; s_sheet_active = false; s_sheet_return = false; s_sheet_tab_from = -1;
    if (s_pager) update_page_dots();
}
static void sheet_completed(lv_anim_t *anim) {
    (void)anim;
    if (s_navigation_cached && !s_present_finishing) {
        request_presented_finish(CC_PRESENT_SHEET); return;
    }
    const int64_t elapsed = esp_timer_get_time() - s_sheet_started_us;
    const bool returning = s_sheet_return;
    ESP_LOGI(TAG, "navigation perf return=%d frames=%u elapsed_ms=%u max_gap_ms=%u",
        returning, s_sheet_frames, (unsigned)(elapsed / 1000), s_sheet_gap_us / 1000);
    cancel_sheet();
    if (returning) {
        s_qr_direct_open = false;
        nuwax_device_qr_view_hide();
        lv_obj_add_flag(s_info_page, LV_OBJ_FLAG_HIDDEN);
        s_detail = CC_DETAIL_NONE;
        lv_obj_move_foreground(s_pager_clip);
    }
    update_page_dots();
    if (s_detail == CC_DETAIL_INFO && s_info_dirty) render_info();
    if (s_detail == CC_DETAIL_NONE && s_open && !s_closing) start_local_animations();
}
/* Animation completion runs before LVGL renders its final dirty rectangles.
 * Restore the live tree only AFTER that cached frame was submitted. Otherwise
 * the most expensive native page redraw lands in the animation's last frame. */
static void presented_frame_cb(lv_event_t *event) {
    (void)event;
    const cc_present_t pending = s_present_pending;
    if (pending == CC_PRESENT_NONE || !s_root) return;
    const int64_t started_us = esp_timer_get_time();
    bool stale_detail = false;
    if (s_nav_active) {
        char key[384];
        const unsigned index = s_nav_active->root == s_info_page ? 1U : 0U;
        stale_detail = !navigation_key(index, key, sizeof(key)) || strcmp(key, s_nav_active->key);
    }
    const cc_page_t presented_page = pending == CC_PRESENT_PAGE
                                        ? s_page_transition_settle_page : s_page;
    const bool stale_page = presented_page == CC_PAGE_STATUS
                               ? s_status_cache_dirty : s_menu_cache_dirty;
    s_present_pending = CC_PRESENT_NONE;
    s_present_finishing = true;
    const bool suspended = suspend_display_invalidation(s_surface);
    if (pending == CC_PRESENT_SHEET) sheet_completed(NULL);
    else if (pending == CC_PRESENT_PAGE) page_transition_completed(NULL);
    else reveal_completed(NULL);
    resume_display_invalidation(s_surface, suspended);
    s_present_finishing = false;
    /* Selection style changes at completion, and live data may have changed
     * while an immutable cache was in use. Redraw only what actually changed. */
    if (s_detail != CC_DETAIL_NONE && stale_detail) {
        lv_obj_invalidate(s_detail == CC_DETAIL_INFO ? s_info_page : nuwax_device_qr_view_root());
    } else if (s_detail == CC_DETAIL_NONE) {
        lv_obj_invalidate(s_pager_clip);
        if (stale_page) transition_cache_invalidate_page(s_page);
    }
    const uint32_t elapsed_us = (uint32_t)(esp_timer_get_time() - started_us);
    s_motion_cost.handoffs++;
    s_motion_cost.handoff_us += elapsed_us;
    if (elapsed_us > s_motion_cost.handoff_max_us) s_motion_cost.handoff_max_us = elapsed_us;
}

static void start_sheet(lv_obj_t *obj, int32_t from, int32_t to, bool returning) {
    s_input_scene_generation++;
    clear_press(); stop_local_animations();
    if (transition_cache_deactivate()) {
        set_page_tree_mode(s_page, false);
        lv_obj_invalidate(s_surface);
    }
    s_sheet = obj; s_sheet_active = true; s_sheet_return = returning;
    s_sheet_from = from; s_sheet_to = to;
    s_sheet_started_us = esp_timer_get_time(); s_sheet_last_us = 0;
    s_sheet_frames = 0; s_sheet_gap_us = 0;
    const bool detail_sheet = obj == nuwax_device_qr_view_root() || obj == s_info_page;
    if (detail_sheet && !navigation_cache_begin(obj)) {
        ESP_LOGI(TAG, "navigation settled without motion: cache not ready");
        lv_obj_set_x(obj, to); sheet_completed(NULL); return;
    }
    lv_obj_set_x(obj, from);
    lv_obj_invalidate(s_surface);
    int32_t distance = to - from; if (distance < 0) distance = -distance;
    unsigned duration = (unsigned)distance * 220U / CC_W;
    if (duration < 80) duration = 80;
    lv_anim_t animation; lv_anim_init(&animation);
    lv_anim_set_var(&animation, obj);
    lv_anim_set_values(&animation, from, to);
    lv_anim_set_duration(&animation, duration);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&animation, sheet_exec);
    lv_anim_set_completed_cb(&animation, sheet_completed);
    if (!lv_anim_start(&animation)) { lv_obj_set_x(obj, to); sheet_completed(NULL); }
    if (s_sheet_tab_from >= 0) update_page_dots();
}

void nuwax_drawer_back(void) {
    if (s_qr_reveal_active) return;
    if (!s_open || s_detail == CC_DETAIL_NONE) {
        return;
    }
    cancel_click();
    if (s_sheet_active && s_sheet_return) return;
    lv_obj_t *obj = s_detail == CC_DETAIL_QR ? nuwax_device_qr_view_root() : s_info_page;
    const int32_t from = lv_obj_get_x(obj);
    cancel_sheet();
    s_sheet_tab_from = -1;
    start_sheet(obj, from, CC_W, true);
}

static void show_qr(void) {
    const char *payload = nuwax_provision_main_qr_payload();
#if CONFIG_NUWAX_UI_DISPLAY_TEST_MODE
    if (!payload || !payload[0]) {
        payload = "NUWAX:NX-DISPLAY-TEST";
    }
#endif
    const bool qr_encoded = payload && payload[0];
    if (!nuwax_device_qr_view_show(payload, nuwax_device_uid_string())) {
        ESP_LOGW(TAG, "device QR unavailable");
    }
    if (nuwax_device_qr_view_is_open()) {
        s_detail = CC_DETAIL_QR;
        if (s_reveal_raw == 1000 && !s_closing) {
            s_sheet_tab_from = -1;
            start_sheet(nuwax_device_qr_view_root(), CC_W, 0, false);
        }
        if (!qr_encoded) {
            ESP_LOGW(TAG, "device QR detail shown before payload is ready");
        }
    }
}

static void start_provisioning_qr_reveal(bool opening) {
    s_input_scene_generation++;
    const int32_t from = opening ? 0 : s_reveal_raw;
    cancel_click();
    clear_press();
    s_contact_down = false;
    cancel_reveal_animation();
    stop_local_animations();
    s_qr_direct_open = true;
    s_closing = !opening;
    if (!s_backdrop_frozen_face || !nuwax_face_frozen_rgb565(NULL)) {
        ESP_LOGW(TAG, "provisioning QR reveal unavailable; settling directly");
        if (!opening) {
            hide_detail();
            s_reveal_anim_target = 0;
            reveal_completed(NULL);
        }
        return;
    }
    s_qr_reveal_active = true;
    s_qr_render_frames = s_qr_render_gap_us = 0;
    s_qr_render_last_us = 0;
    s_reveal_raw = from;
    update_backdrop_progress(from);
    nuwax_face_set_overlay_progress((uint16_t)from);
    s_reveal_anim_from = from;
    s_reveal_anim_target = opening ? 1000 : 0;
    s_reveal_anim_duration_ms = opening ? CC_QR_REVEAL_MS : CC_QR_CLOSE_MS;
    s_reveal_anim_step = 0;
    s_reveal_anim_steps = (s_reveal_anim_duration_ms * CONFIG_NUWAX_UI_ANIMATION_TARGET_FPS + 999U) / 1000U;
    /* The idle worker already prepares this QR during the three-second hold.
     * Never allocate or capture synchronously to enter an animation. */
    const bool cached = navigation_cache_begin(nuwax_device_qr_view_root());
    s_reveal_anim_started_us = esp_timer_get_time();
    ESP_LOGI(TAG, "provisioning QR circular reveal start direction=%s cached=%d duration_ms=%u",
             opening ? "open" : "close", cached, (unsigned)s_reveal_anim_duration_ms);
    s_reveal_timer = lv_timer_create(reveal_timer_cb, CC_REVEAL_FRAME_MS, NULL);
    if (!s_reveal_timer) reveal_completed(NULL);
}

bool nuwax_drawer_show_provisioning_qr(void) {
    if (s_ota_blocked || !s_root || !s_surface || !s_have_state ||
        !s_state.provisioning_active) return false;
    if (s_open && !s_closing && s_detail == CC_DETAIL_QR && !s_sheet_active)
        return true;

    const char *payload = nuwax_provision_main_qr_payload();
    if (!payload || !payload[0]) return false;
    /* Encode before replacing the current visual. This runs on the LVGL owner:
     * no refresh can present the menu between setup and the opaque QR page. */
    if (!nuwax_device_qr_view_show(payload, nuwax_device_uid_string())) {
        nuwax_device_qr_view_hide();
        return false;
    }
    cancel_reveal_animation();
    lv_anim_delete(s_surface, page_progress_exec);
    cancel_page_perf();
    s_page_transition_active = false;
    s_page_dragging = false;
    cancel_click();
    cancel_sheet();
    clear_press();
    s_contact_down = false;
    stop_local_animations();
    (void)transition_cache_deactivate();
    s_scene_frame_ready = false;
    s_scene_pager_visible = false;
#if NUWAX_TEST_PANEL_ENABLED
    s_test_page_drag = false;
    s_test_page_drag_dx = 0;
#endif
    if (!s_open) {
        (void)nuwax_face_freeze_interaction();
        s_backdrop_frozen_face = nuwax_face_frozen_frame_ready();
    }
    s_open = true;
    s_closing = false;
    s_dragging = false;
    s_motion_y = CC_OPEN_Y;
    s_page = CC_PAGE_MENU;
    s_page_progress = 1000;
    s_detail = CC_DETAIL_QR;
    if (s_info_page) lv_obj_add_flag(s_info_page, LV_OBJ_FLAG_HIDDEN);
    /* Prepare the underlying menu only for a later Back action. It is never
     * presented during automatic provisioning entry. */
    render_page_if_dirty(CC_PAGE_MENU);
    set_page_tree_mode(CC_PAGE_MENU, false);
    s_reveal_anim_target = 1000;
    apply_reveal_progress(1000, false, true);
    update_page_dots();
    lv_obj_t *qr = nuwax_device_qr_view_root();
    lv_obj_set_pos(qr, 0, 0);
    lv_obj_remove_flag(qr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(qr);
    start_provisioning_qr_reveal(true);
    lv_obj_invalidate(s_root);
    ESP_LOGI(TAG, "provisioning QR entering from center duration_ms=%u", (unsigned)CC_QR_REVEAL_MS);
    return true;
}

void nuwax_drawer_hide_provisioning_qr(void) {
    if (!s_open || s_detail != CC_DETAIL_QR) {
        return;
    }
    nuwax_drawer_close();
}

static void show_info(void) {
    render_info();
    lv_obj_remove_flag(s_info_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_info_page);
    s_detail = CC_DETAIL_INFO;
    s_sheet_tab_from = -1;
    start_sheet(s_info_page, CC_W, 0, false);
}

nuwax_drawer_action_result_t nuwax_drawer_tap(uint16_t x, uint16_t y) {
    nuwax_drawer_action_result_t result = {
        .action = NUWAX_DRAWER_ACTION_NONE,
        .value = 0,
    };
    if (!drawer_accepts_tap()) {
        return result;
    }
    if (s_detail != CC_DETAIL_NONE) {
        if (nuwax_detail_hit_back(x, y)) {
            if (s_detail == CC_DETAIL_QR && provisioning_active()) {
                if (s_provision_pending) return result;
                s_provision_pending = true;
                s_provision_pending_target = false;
                s_provision_pending_deadline_us = esp_timer_get_time() +
                    (int64_t)CC_PROVISION_PENDING_TIMEOUT_MS * 1000LL;
                result.action = NUWAX_DRAWER_ACTION_REPROVISION_WIFI;
                result.value = 0;
                return result;
            }
            click_then_navigate(4, s_detail == CC_DETAIL_QR ? nuwax_device_qr_view_back_button() : s_info_back);
        }
        return result;
    }
    if (point_in_rect(x, y, 148, 327, 64, 30)) {
        nuwax_drawer_close();
        return result;
    }
    if (point_in_rect(x, y, CC_PAGER_X, CC_PAGER_Y, CC_PAGER_W, 26)) {
#if NUWAX_TEST_PANEL_ENABLED
        const int tab_x = x - CC_PAGER_X;
        if (tab_x < 60) set_page(CC_PAGE_STATUS, true);
        else if (tab_x >= 69 && tab_x < 129) set_page(CC_PAGE_MENU, true);
        else if (tab_x >= 138) set_page(CC_PAGE_TEST, true);
#else
        if (x >= CC_PAGER_X + 17 && x < CC_PAGER_X + 89) set_page(CC_PAGE_STATUS, true);
        else if (x >= CC_PAGER_X + 109 && x < CC_PAGER_X + 181) set_page(CC_PAGE_MENU, true);
#endif
        return result;
    }
#if NUWAX_TEST_NETWORK_ENABLED
    if (s_page == CC_PAGE_TEST && nuwax_test_panel_hit_network(x, y)) {
        if (s_offline_test_pending) {
            return result;
        }
        s_offline_test_pending_target =
            !(s_have_state && s_state.simulated_offline);
        s_offline_test_pending = true;
        s_offline_test_pending_deadline_us =
            esp_timer_get_time() + 6000LL * 1000LL;
        render_menu();
        result.action = NUWAX_DRAWER_ACTION_SET_SIMULATED_OFFLINE;
        result.value = s_offline_test_pending_target ? 1 : 0;
        return result;
    }
#endif
    if (s_page != CC_PAGE_MENU) {
        return result;
    }
    if (point_in_rect(x, y, CC_CARD_LEFT_X, CC_MENU_ROW0_Y,
                      CC_CARD_W, CC_MENU_ACTION_H)) {
        if (monitoring_active() || provisioning_active() || s_state.provisioning_starting ||
            meeting_transitioning() ||
            s_meeting_pending || s_provision_pending) {
            return result;
        }
#if CONFIG_NUWAX_UI_DISPLAY_TEST_MODE
        s_test_meeting_active = !s_test_meeting_active;
        s_test_provision_active = false;
        render_menu();
#else
        /* ERROR may retain a disconnected recording intent. The first
         * action must cancel it, not send another enable which cannot work
         * offline. The owner then publishes DISABLED for a fresh start. */
        s_meeting_pending_target = !(meeting_active() ||
            (s_have_state && s_state.meeting == NUWAX_MEETING_UI_ERROR));
        s_meeting_pending = true;
        s_pending_deadline_us = esp_timer_get_time() +
                                (int64_t)CC_MEETING_PENDING_TIMEOUT_MS * 1000LL;
        render_menu();
        result.action = NUWAX_DRAWER_ACTION_SET_MEETING_MODE;
        result.value = s_meeting_pending_target ? 1 : 0;
#endif
        return result;
    }
    if (point_in_rect(x, y, CC_CARD_RIGHT_X, CC_MENU_ROW0_Y,
                      CC_CARD_W, CC_MENU_ACTION_H)) {
        click_then_navigate(0, s_menu_cards[1]);
        /* Bars are indicators. The main owner resolves each click against
         * its latest applied value, including clicks queued before UI refresh. */
        result.action = NUWAX_DRAWER_ACTION_CYCLE_BRIGHTNESS;
        ESP_LOGI(TAG, "brightness cycle touch x=%u y=%u", x, y);
        return result;
    }
    if (point_in_rect(x, y, CC_CARD_LEFT_X, CC_MENU_ACTION_Y,
                      CC_MENU_ACTION_W, CC_MENU_ACTION_H)) {
        if (s_provision_pending || s_state.provisioning_starting) return result;
        if (provisioning_active()) {
            click_then_navigate(2, s_menu_cards[2]);
            return result;
        }
        s_provision_pending_target = true;
        s_provision_pending = true;
        s_provision_pending_deadline_us = esp_timer_get_time() +
            (int64_t)CC_PROVISION_PENDING_TIMEOUT_MS * 1000LL;
        render_menu();
        result.action = NUWAX_DRAWER_ACTION_REPROVISION_WIFI;
        result.value = 1;
        return result;
    }
    if (point_in_rect(x, y, CC_CARD_RIGHT_X, CC_MENU_ACTION_Y,
                      CC_MENU_ACTION_W, CC_MENU_ACTION_H)) {
        click_then_navigate(3, s_menu_cards[3]);
        return result;
    }
    uint8_t volume = 0;
    if (nuwax_drawer_hit_volume(x, y, &volume)) {
        set_volume_visual(volume);
        result.action = NUWAX_DRAWER_ACTION_SET_VOLUME;
        result.value = volume;
    }
    return result;
}

void nuwax_drawer_reject_action(nuwax_drawer_action_t action) {
    bool changed = false;
    if (action == NUWAX_DRAWER_ACTION_CYCLE_BRIGHTNESS) {
        s_brightness_visual = -1;
        changed = true;
    } else if (action == NUWAX_DRAWER_ACTION_SET_VOLUME) {
        set_volume_visual(s_have_state ? s_state.volume : 60U);
        changed = true;
    } else if (action == NUWAX_DRAWER_ACTION_SET_MEETING_MODE &&
               s_meeting_pending) {
        s_meeting_pending = false;
        s_pending_deadline_us = 0;
        changed = true;
    } else if (action == NUWAX_DRAWER_ACTION_REPROVISION_WIFI &&
               s_provision_pending) {
        s_provision_pending = false;
        s_provision_pending_deadline_us = 0;
        changed = true;
#if NUWAX_TEST_NETWORK_ENABLED
    } else if (action == NUWAX_DRAWER_ACTION_SET_SIMULATED_OFFLINE &&
               s_offline_test_pending) {
        s_offline_test_pending = false;
        s_offline_test_pending_deadline_us = 0;
        changed = true;
#endif
    }
    if (!changed) {
        return;
    }
    ESP_LOGW(TAG, "control-center action rejected action=%d", (int)action);
    if (s_open && !s_closing) {
        render_menu();
    }
}

bool nuwax_drawer_hit_volume_at_panel_y(uint16_t x,
                                        uint16_t y,
                                        int16_t panel_y,
                                        uint8_t *volume_out) {
    (void)panel_y;
    if (!s_open || reveal_owns_motion() || s_page_transition_active || s_sheet_active ||
        s_detail != CC_DETAIL_NONE || s_page != CC_PAGE_MENU) {
        return false;
    }
    return nuwax_drawer_volume_value_at(x, y, false, volume_out);
}

/* Geometry only: safe on the touch task after admission by a UI snapshot. */
bool nuwax_drawer_volume_value_at(uint16_t x, uint16_t y,
                                 bool captured, uint8_t *volume_out) {
    if (!captured && !point_in_rect(x, y, CC_VOLUME_BAR_X - 12,
            CC_VOLUME_BAR_Y - 14, CC_VOLUME_BAR_W + 24, 34)) return false;
    int local_x = (int)x - CC_VOLUME_BAR_X;
    if (local_x < 0) local_x = 0;
    if (local_x > CC_VOLUME_BAR_W) local_x = CC_VOLUME_BAR_W;
    const uint8_t volume =
        (uint8_t)((local_x * 100 + CC_VOLUME_BAR_W / 2) / CC_VOLUME_BAR_W);
    if (volume_out) {
        *volume_out = volume;
    }
    return true;
}

bool nuwax_drawer_hit_simulated_offline(uint16_t x, uint16_t y) {
#if NUWAX_TEST_NETWORK_ENABLED
    return s_open && !s_closing && s_detail == CC_DETAIL_NONE && s_page == CC_PAGE_TEST &&
           nuwax_test_panel_hit_network(x, y);
#else
    (void)x;
    (void)y;
    return false;
#endif
}

bool nuwax_drawer_hit_volume(uint16_t x,
                             uint16_t y,
                             uint8_t *volume_out) {
    return nuwax_drawer_hit_volume_at_panel_y(x, y, s_motion_y, volume_out);
}

void nuwax_drawer_preview_volume(uint8_t volume) {
    if (s_open && s_page == CC_PAGE_MENU && s_detail == CC_DETAIL_NONE) {
        set_volume_visual(volume);
    }
}

void nuwax_drawer_drag_to(int16_t y) {
    cancel_click();
    if (!s_open || !s_reveal_clip) {
        return;
    }
    if (s_sheet_active) { cancel_sheet(); update_page_dots(); }
    retire_page_motion();
    if (!s_dragging) {
        s_input_scene_generation++;
        stop_local_animations();
    }
    if (y > CC_OPEN_Y) y = CC_OPEN_Y;
    if (y < CC_HIDDEN_Y) y = CC_HIDDEN_Y;
    s_dragging = true;
    s_motion_y = y;
    cancel_reveal_animation();
    s_closing = false;
    /* A fresh pull from an already open page also needs the motion cache. */
    if (!s_transition_cache_active) (void)transition_cache_activate(false);
    /* A zero-displacement grab keeps the exact animation progress. The
     * integer y returned to the input owner is rounded from that progress. */
    if (y == nuwax_drawer_current_y()) return;
    const int32_t range = CC_OPEN_Y - CC_HIDDEN_Y;
    int32_t progress = ((int32_t)y - CC_HIDDEN_Y) * 1000 / range;
    if (progress < 0) progress = 0;
    if (progress > 1000) progress = 1000;
    apply_reveal_progress(progress, false, false);
}

void nuwax_drawer_finish_drag(bool close) {
    if (!s_open || !s_reveal_clip) {
        return;
    }
    s_dragging = false;
    if (close) {
        nuwax_drawer_close();
        return;
    }
    animate_reveal(true);
}

int16_t nuwax_drawer_current_y(void) {
    const int32_t progress = s_reveal_raw < 0 ? 0 :
        s_reveal_raw > 1000 ? 1000 : s_reveal_raw;
    return s_open ? CC_HIDDEN_Y +
        ((CC_OPEN_Y - CC_HIDDEN_Y) * progress + 500) / 1000 : CC_HIDDEN_Y;
}

void nuwax_drawer_get_input_state(nuwax_drawer_input_state_t *state) {
    if (!state) return;
    memset(state, 0, sizeof(*state));
    state->scene_generation = s_input_scene_generation;
    state->modal_blocked = s_ota_blocked;
    state->open = s_open && !s_ota_blocked;
    state->detail = s_detail != CC_DETAIL_NONE;
    state->tap_allowed = drawer_accepts_tap();
    const bool available = state->open && !state->detail &&
        !reveal_owns_motion() && !s_sheet_active &&
        !(s_click_timer && s_click_target != 0);
    state->volume_available = state->tap_allowed && !state->detail && s_page == CC_PAGE_MENU;
    state->page_left = available && (s_page == CC_PAGE_STATUS || s_page_transition_active);
    state->page_right = available && (s_page == CC_PAGE_MENU || s_page_transition_active);
#if NUWAX_TEST_PANEL_ENABLED
    if (available && s_page == CC_PAGE_MENU) state->page_left = true;
    if (available && s_page == CC_PAGE_TEST) state->page_right = true;
#endif
}

#ifdef NUWAX_LVGL_SIMULATOR
void nuwax_drawer_sim_set_reveal_progress(uint16_t progress_permille) {
    if (!s_root || !s_reveal_clip) return;
    cancel_reveal_animation();
    const uint16_t clamped = progress_permille > 1000U
                                 ? 1000U
                                 : progress_permille;
    s_open = clamped > 0U;
    s_closing = false;
    apply_reveal_progress(clamped, false, false);
}

void nuwax_drawer_sim_set_page(bool menu_page) {
    set_page(menu_page ? CC_PAGE_MENU : CC_PAGE_STATUS, false);
}
#endif
