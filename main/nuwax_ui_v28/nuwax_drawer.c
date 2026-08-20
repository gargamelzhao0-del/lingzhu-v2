#include "nuwax_drawer.h"

#include <stdio.h>
#include <string.h>

#include "assets/nuwax_control_center_icons.h"
#include "control_center_timing_policy.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "menu/nuwax_device_qr_view.h"
#include "nuwax_device_uid.h"
#include "nuwax_face.h"
#include "nuwax_lvgl_port.h"
#include "nuwax_provision_main.h"
#include "nuwax_theme.h"
#include "nuwax_ui_config.h"
#include "src/core/lv_obj_draw_private.h"

static const char *TAG = "nuwax_control_center";

LV_FONT_DECLARE(nuwax_font_control_center_cjk_7);
LV_FONT_DECLARE(nuwax_font_control_center_cjk_8);
LV_FONT_DECLARE(nuwax_font_control_center_cjk_9);
LV_FONT_DECLARE(nuwax_font_control_center_cjk_10);
LV_FONT_DECLARE(nuwax_font_control_center_cjk_11);
LV_FONT_DECLARE(nuwax_font_control_center_cjk_12);
LV_FONT_DECLARE(nuwax_font_control_center_cjk_13);

#define CC_W NUWAX_SCREEN_W
#define CC_H NUWAX_SCREEN_H
#define CC_PAGE_ANIM_MS 230
#define CC_PAGE_ANIM_STEPS 10
#define CC_OPERATION_MIN_FPS 30
#define CC_PAGE_DRAG_RANGE_PX 92
#define CC_REVEAL_FULL_MS 1300
#define CC_REVEAL_MIN_MS 25
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
#define CC_MENU_CACHE_COMPONENTS 8U
#define CC_TRANSITION_CACHE_COMPONENTS \
    (CC_STATUS_CACHE_COMPONENTS + CC_MENU_CACHE_COMPONENTS)
#define CC_LOCAL_ANIM_FRAME_INTERVAL_US 25000LL
#ifndef CONFIG_NUWAX_UI_ANIMATION_TARGET_FPS
#define CONFIG_NUWAX_UI_ANIMATION_TARGET_FPS 40
#endif
#define CC_REVEAL_FRAME_MS (1000U / CONFIG_NUWAX_UI_ANIMATION_TARGET_FPS)

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

#define CC_HEADER_X 90
#define CC_HEADER_W 180
#define CC_CARD_LEFT_X 46
#define CC_CARD_RIGHT_X 190
#define CC_CARD_W 124
#define CC_STATUS_CARD_H 60
#define CC_STATUS_ROW0_Y 103
#define CC_STATUS_ROW1_Y 181

#define CC_MENU_ROW0_Y 103
#define CC_MENU_ROW1_Y 173
#define CC_MENU_ACTION_Y CC_MENU_ROW1_Y
#define CC_MENU_ACTION_W 124
#define CC_MENU_ACTION_H 58
#define CC_MENU_VOLUME_X 60
#define CC_MENU_VOLUME_Y 243
#define CC_MENU_VOLUME_W 240
#define CC_MENU_VOLUME_H 53
#define CC_VOLUME_BAR_X 104
#define CC_VOLUME_BAR_Y 277
#define CC_VOLUME_BAR_W 141
#define CC_VOLUME_BAR_H 3

typedef enum {
    CC_PAGE_STATUS = 0,
    CC_PAGE_MENU,
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
    int16_t ext_draw_size;
    cc_page_t page;
    bool valid;
} cc_transition_cache_component_t;

static lv_obj_t *s_root;
static lv_obj_t *s_reveal_clip;
static lv_obj_t *s_surface;
static lv_obj_t *s_status_page;
static lv_obj_t *s_menu_page;
static lv_obj_t *s_info_page;
static lv_obj_t *s_pager;
static lv_obj_t *s_status_dot;
static lv_obj_t *s_menu_dot;
static lv_obj_t *s_page_name;
static lv_obj_t *s_pager_clip;
static lv_obj_t *s_status_header_clip;
static lv_obj_t *s_menu_header_clip;
static lv_obj_t *s_status_header;
static lv_obj_t *s_menu_header;
static lv_obj_t *s_cloud_beacon;
static lv_obj_t *s_battery_fill;
static lv_obj_t *s_footer;
static lv_obj_t *s_footer_clip;
static lv_obj_t *s_footer_dot;
static lv_obj_t *s_footer_label;
static lv_obj_t *s_menu_cards[4];
static lv_obj_t *s_menu_card_clips[4];
static lv_obj_t *s_meeting_state;
static lv_obj_t *s_meeting_switch;
static lv_obj_t *s_meeting_knob;
static lv_obj_t *s_provision_state;
static lv_obj_t *s_provision_switch;
static lv_obj_t *s_provision_knob;
static lv_obj_t *s_volume_text;
static lv_obj_t *s_menu_volume;
static lv_obj_t *s_volume_bar;
static lv_obj_t *s_volume_fill;
static lv_obj_t *s_volume_knob;
static lv_obj_t *s_info_body;
static lv_timer_t *s_pending_timer;
static EXT_RAM_BSS_ATTR cc_status_card_t s_status_cards[4];
static nuwax_device_state_t s_state;
static bool s_have_state;
static bool s_status_dirty = true;
static bool s_menu_dirty = true;
static bool s_info_dirty = true;
static bool s_open;
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
static bool s_transition_cache_available;
static bool s_transition_cache_active;
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
#if CONFIG_NUWAX_UI_DISPLAY_TEST_MODE
static bool s_test_meeting_active;
static bool s_test_provision_active;
#endif

static void transition_cache_mark_page_dirty(cc_page_t page) {
    uint32_t *generation = page == CC_PAGE_STATUS
                               ? &s_status_cache_generation
                               : &s_menu_cache_generation;
    bool *dirty = page == CC_PAGE_STATUS
                      ? &s_status_cache_dirty
                      : &s_menu_cache_dirty;
    *dirty = true;
    (*generation)++;
    if (*generation == 0U) {
        *generation = 1U;
    }
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
           previous->provisioning_active != next->provisioning_active;
}

static bool state_affects_menu(const nuwax_device_state_t *previous,
                               const nuwax_device_state_t *next) {
    if (!previous || !next) return true;
    return previous->meeting != next->meeting ||
           previous->monitoring != next->monitoring ||
           previous->volume != next->volume ||
           previous->provisioning_active != next->provisioning_active;
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
    lv_obj_set_style_radius(panel, 8, 0);
    /* Children stay within the panel bounds. Avoiding software corner
     * clipping removes a mask pass from every animated card. */
    lv_obj_set_style_clip_corner(panel, false, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x18191a), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x3f4245), 0);
    lv_obj_set_style_border_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *highlight = lv_obj_create(panel);
    if (!highlight) {
        return NULL;
    }
    lv_obj_set_pos(highlight, 2, 1);
    lv_obj_set_size(highlight, w - 4, 1);
    lv_obj_set_style_bg_color(highlight, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(highlight, 14, 0);
    lv_obj_set_style_border_width(highlight, 0, 0);
    lv_obj_set_style_pad_all(highlight, 0, 0);
    lv_obj_remove_flag(highlight,
                       LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return panel;
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
    return clip;
}

static lv_obj_t *create_page_header(lv_obj_t *page,
                                    const char *title,
                                    lv_obj_t **out_clip) {
    lv_obj_t *clip = make_reveal_clip(page, CC_HEADER_X, 67,
                                      CC_HEADER_W, 28);
    if (!clip) {
        return NULL;
    }
    if (out_clip) *out_clip = clip;
    lv_obj_t *header = lv_obj_create(clip);
    if (!header) {
        return NULL;
    }
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_size(header, CC_HEADER_W, 28);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *brand = make_label(header,
                                 &nuwax_font_control_center_cjk_7,
                                 lv_color_hex(0x79cbd8),
                                 LV_OPA_COVER,
                                 LV_TEXT_ALIGN_CENTER);
    if (!brand) {
        return NULL;
    }
    lv_obj_set_pos(brand, 0, 0);
    lv_obj_set_size(brand, CC_HEADER_W, 9);
    lv_label_set_text(brand, "NUWAX");

    lv_obj_t *heading = make_label(header,
                                   &nuwax_font_control_center_cjk_13,
                                   lv_color_hex(0xf8fbfc),
                                   LV_OPA_COVER,
                                   LV_TEXT_ALIGN_CENTER);
    if (!heading) {
        return NULL;
    }
    lv_obj_set_pos(heading, 0, 10);
    lv_obj_set_size(heading, CC_HEADER_W, 18);
    lv_label_set_text(heading, title);
    return header;
}

static void update_page_dots(void) {
    if (!s_status_dot || !s_menu_dot) {
        return;
    }
    lv_obj_set_style_bg_opa(s_status_dot,
                            s_page == CC_PAGE_STATUS ? LV_OPA_COVER : 65,
                            0);
    lv_obj_set_style_bg_opa(s_menu_dot,
                            s_page == CC_PAGE_MENU ? LV_OPA_COVER : 65,
                            0);
    if (s_page == CC_PAGE_STATUS) {
        lv_obj_set_pos(s_status_dot, 10, 7);
        lv_obj_set_size(s_status_dot, 11, 4);
        lv_obj_set_pos(s_menu_dot, 25, 7);
        lv_obj_set_size(s_menu_dot, 4, 4);
        if (s_page_name) lv_label_set_text(s_page_name, "状态");
    } else {
        lv_obj_set_pos(s_status_dot, 10, 7);
        lv_obj_set_size(s_status_dot, 4, 4);
        lv_obj_set_pos(s_menu_dot, 18, 7);
        lv_obj_set_size(s_menu_dot, 11, 4);
        if (s_page_name) lv_label_set_text(s_page_name, "菜单");
    }
}

static bool create_page_dots(lv_obj_t *parent) {
    s_pager_clip = make_reveal_clip(parent, 148, 43, 64, 18);
    if (!s_pager_clip) return false;
    s_pager = lv_obj_create(s_pager_clip);
    if (!s_pager) return false;
    lv_obj_set_pos(s_pager, 0, 0);
    lv_obj_set_size(s_pager, 64, 18);
    lv_obj_set_style_bg_opa(s_pager, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_pager, 0, 0);
    lv_obj_set_style_pad_all(s_pager, 0, 0);
    lv_obj_remove_flag(s_pager, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_status_dot = lv_obj_create(s_pager);
    if (!s_status_dot) return false;
    lv_obj_set_style_radius(s_status_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_status_dot, lv_color_hex(0x8edce8), 0);
    lv_obj_set_style_border_width(s_status_dot, 0, 0);
    lv_obj_set_style_pad_all(s_status_dot, 0, 0);
    lv_obj_remove_flag(s_status_dot,
                       LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_menu_dot = lv_obj_create(s_pager);
    if (!s_menu_dot) return false;
    lv_obj_set_style_radius(s_menu_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_menu_dot, lv_color_hex(0x8edce8), 0);
    lv_obj_set_style_border_width(s_menu_dot, 0, 0);
    lv_obj_set_style_pad_all(s_menu_dot, 0, 0);
    lv_obj_remove_flag(s_menu_dot,
                       LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_page_name = make_label(s_pager,
                             &nuwax_font_control_center_cjk_9,
                             lv_color_hex(0x7f878d),
                             LV_OPA_COVER,
                             LV_TEXT_ALIGN_LEFT);
    if (!s_page_name) return false;
    lv_obj_set_pos(s_page_name, 36, 3);
    lv_obj_set_size(s_page_name, 24, 12);
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
    lv_obj_set_pos(card->icon_back, 9, 15);
    lv_obj_set_size(card->icon_back, 30, 30);
    lv_obj_set_style_radius(card->icon_back, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(card->icon_back, lv_color_hex(0x47494b), 0);
    lv_obj_set_style_bg_opa(card->icon_back, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card->icon_back, 0, 0);
    lv_obj_set_style_pad_all(card->icon_back, 0, 0);
    lv_obj_remove_flag(card->icon_back,
                       LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    card->image = lv_image_create(card->icon_back);
    if (!card->image) return false;
    lv_image_set_antialias(card->image, false);
    lv_obj_set_style_image_recolor(card->image, lv_color_hex(0xf8fbfc), 0);
    lv_obj_set_style_image_recolor_opa(card->image, LV_OPA_COVER, 0);
    lv_obj_center(card->image);
    lv_obj_remove_flag(card->image,
                       LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    card->value = make_label(card->root,
                             &nuwax_font_control_center_cjk_11,
                             lv_color_hex(0xf5f7f8),
                             LV_OPA_COVER,
                             LV_TEXT_ALIGN_LEFT);
    if (!card->value) return false;
    lv_obj_set_pos(card->value, 49, 23);
    lv_obj_set_width(card->value, 68);
    lv_label_set_long_mode(card->value, LV_LABEL_LONG_DOT);
    lv_label_set_text(card->value, "--");

    card->offline_mark = make_label(card->icon_back,
                                    &lv_font_montserrat_10,
                                    lv_color_hex(0xf5f7f8),
                                    LV_OPA_COVER,
                                    LV_TEXT_ALIGN_CENTER);
    if (!card->offline_mark) return false;
    lv_obj_set_pos(card->offline_mark, 17, 17);
    lv_obj_set_size(card->offline_mark, 13, 13);
    lv_obj_set_style_radius(card->offline_mark, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(card->offline_mark, lv_color_hex(0x202224), 0);
    lv_obj_set_style_bg_opa(card->offline_mark, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card->offline_mark, 2, 0);
    lv_obj_set_style_border_color(card->offline_mark, lv_color_hex(0x47494b), 0);
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
    if (!s_open || s_closing || s_page_transition_active ||
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
    lv_obj_set_style_border_width(s_status_page, 0, 0);
    lv_obj_set_style_pad_all(s_status_page, 0, 0);
    lv_obj_remove_flag(s_status_page,
                       LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_status_header = create_page_header(s_status_page, "设备状态",
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
    lv_obj_set_size(s_cloud_beacon, 28, 28);
    lv_obj_set_style_radius(s_cloud_beacon, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_cloud_beacon, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_cloud_beacon, 1, 0);
    lv_obj_set_style_border_color(s_cloud_beacon, lv_color_hex(0x75daec), 0);
    lv_obj_set_style_border_opa(s_cloud_beacon, 140, 0);
    lv_obj_set_style_opa(s_cloud_beacon, 0, 0);
    lv_obj_set_style_pad_all(s_cloud_beacon, 0, 0);
    lv_obj_remove_flag(s_cloud_beacon,
                       LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_to_index(s_cloud_beacon, 0);

    s_battery_fill = lv_obj_create(s_status_cards[2].icon_back);
    if (!s_battery_fill) return false;
    lv_obj_set_pos(s_battery_fill, 7, 11);
    lv_obj_set_size(s_battery_fill, 12, 8);
    lv_obj_set_style_radius(s_battery_fill, 1, 0);
    lv_obj_set_style_bg_color(s_battery_fill, lv_color_hex(0xf8fbfc), 0);
    lv_obj_set_style_bg_opa(s_battery_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_battery_fill, 0, 0);
    lv_obj_set_style_pad_all(s_battery_fill, 0, 0);
    lv_obj_remove_flag(s_battery_fill,
                       LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_to_index(s_battery_fill, 0);
    s_footer_clip = make_reveal_clip(s_status_page, 144, 306, 100, 14);
    if (!s_footer_clip) return false;
    s_footer = lv_obj_create(s_footer_clip);
    if (!s_footer) return false;
    lv_obj_set_pos(s_footer, 0, 0);
    lv_obj_set_size(s_footer, 100, 14);
    lv_obj_set_style_bg_opa(s_footer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_footer, 0, 0);
    lv_obj_set_style_pad_all(s_footer, 0, 0);
    lv_obj_remove_flag(s_footer,
                       LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_footer_dot = lv_obj_create(s_footer);
    if (!s_footer_dot) return false;
    lv_obj_set_pos(s_footer_dot, 0, 4);
    lv_obj_set_size(s_footer_dot, 5, 5);
    lv_obj_set_style_radius(s_footer_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_footer_dot, lv_color_hex(0x78d7ad), 0);
    lv_obj_set_style_border_width(s_footer_dot, 0, 0);
    lv_obj_set_style_pad_all(s_footer_dot, 0, 0);
    lv_obj_remove_flag(s_footer_dot,
                       LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *footer_halo = lv_obj_create(s_footer);
    if (!footer_halo) return false;
    lv_obj_set_pos(footer_halo, -3, 2);
    lv_obj_set_size(footer_halo, 11, 11);
    lv_obj_set_style_radius(footer_halo, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(footer_halo, lv_color_hex(0x78d7ad), 0);
    lv_obj_set_style_bg_opa(footer_halo, 26, 0);
    lv_obj_set_style_border_width(footer_halo, 0, 0);
    lv_obj_set_style_pad_all(footer_halo, 0, 0);
    lv_obj_remove_flag(footer_halo,
                       LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_to_index(footer_halo, 0);

    s_footer_label = make_label(s_footer,
                                &nuwax_font_control_center_cjk_9,
                                lv_color_hex(0x969da3),
                                LV_OPA_COVER,
                                LV_TEXT_ALIGN_LEFT);
    if (!s_footer_label) return false;
    lv_obj_set_pos(s_footer_label, 11, 0);
    lv_obj_set_width(s_footer_label, 89);
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
    lv_obj_set_style_image_recolor(icon, lv_color_hex(0xf5f7f8), 0);
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
    lv_obj_set_size(track, 28, 15);
    lv_obj_set_style_radius(track, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(track, lv_color_hex(0x454a53), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(track, 0, 0);
    lv_obj_set_style_pad_all(track, 0, 0);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *knob = lv_obj_create(track);
    if (!knob) return false;
    lv_obj_set_pos(knob, 2, 2);
    lv_obj_set_size(knob, 11, 11);
    lv_obj_set_style_radius(knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(knob, lv_color_hex(0xdde3e9), 0);
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
    if (!card || !create_icon_image(card, icon_id, 9, 18)) return NULL;
    lv_obj_t *name = make_label(card,
                                &nuwax_font_control_center_cjk_10,
                                lv_color_hex(0xf5f7f8),
                                LV_OPA_COVER,
                                LV_TEXT_ALIGN_LEFT);
    if (!name) return NULL;
    lv_obj_set_pos(name, 43, 9);
    lv_obj_set_width(name, 72);
    lv_label_set_text(name, title);
    lv_obj_t *state = make_label(card,
                                 &nuwax_font_control_center_cjk_8,
                                 lv_color_hex(0x858d93),
                                 LV_OPA_COVER,
                                 LV_TEXT_ALIGN_LEFT);
    if (!state) return NULL;
    lv_obj_set_pos(state, 43, 25);
    lv_obj_set_width(state, 52);
    lv_label_set_text(state, "已关闭");
    if (!create_switch(card, 87, 29, out_switch, out_knob)) return NULL;
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
    if (!card || !create_icon_image(card, icon_id, 9, 18)) return NULL;
    lv_obj_t *name = make_label(card,
                                &nuwax_font_control_center_cjk_10,
                                lv_color_hex(0xf5f7f8),
                                LV_OPA_COVER,
                                LV_TEXT_ALIGN_LEFT);
    if (!name) return NULL;
    lv_obj_set_pos(name, 43, 24);
    lv_obj_set_width(name, 70);
    lv_label_set_text(name, title);
    lv_obj_t *arrow = make_label(card,
                                 &lv_font_montserrat_12,
                                 lv_color_hex(0x7d858b),
                                 LV_OPA_COVER,
                                 LV_TEXT_ALIGN_CENTER);
    if (!arrow) return NULL;
    lv_obj_set_pos(arrow, 108, 22);
    lv_obj_set_size(arrow, 8, 14);
    lv_label_set_text(arrow, ">");
    return card;
}

static bool create_menu_page(void) {
    s_menu_page = lv_obj_create(s_surface);
    if (!s_menu_page) return false;
    lv_obj_set_pos(s_menu_page, 0, 0);
    lv_obj_set_size(s_menu_page, CC_W, CC_H);
    lv_obj_set_style_bg_opa(s_menu_page, LV_OPA_TRANSP, 0);
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
    s_menu_cards[1] = create_menu_mode_card(s_menu_card_clips[1],
                          0,
                          0,
                          NUWAX_CC_ICON_PROVISIONING,
                          "设备配网",
                          &s_provision_state,
                           &s_provision_switch,
                           &s_provision_knob);
    if (!s_menu_cards[1]) return false;
    s_menu_card_clips[2] = make_reveal_clip(
        s_menu_page, CC_CARD_LEFT_X, CC_MENU_ROW1_Y,
        CC_CARD_W, CC_MENU_ACTION_H);
    if (!s_menu_card_clips[2]) return false;
    s_menu_cards[2] = create_menu_action_card(s_menu_card_clips[2],
                            0,
                            0,
                            NUWAX_CC_ICON_QR,
                            "配网二维码");
    if (!s_menu_cards[2]) return false;
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
        !create_icon_image(s_menu_volume, NUWAX_CC_ICON_VOLUME, 10, 15)) {
        return false;
    }
    lv_obj_t *volume_label = make_label(s_menu_volume,
                                        &nuwax_font_control_center_cjk_10,
                                        lv_color_hex(0xf5f7f8),
                                        LV_OPA_COVER,
                                        LV_TEXT_ALIGN_LEFT);
    if (!volume_label) return false;
    lv_obj_set_pos(volume_label, 43, 8);
    lv_obj_set_width(volume_label, 42);
    lv_label_set_text(volume_label, "音量");
    s_volume_text = make_label(s_menu_volume,
                               &lv_font_montserrat_10,
                               lv_color_hex(0xb9c0c5),
                               LV_OPA_COVER,
                               LV_TEXT_ALIGN_RIGHT);
    if (!s_volume_text) return false;
    lv_obj_set_pos(s_volume_text, 196, 8);
    lv_obj_set_width(s_volume_text, 34);
    lv_label_set_text(s_volume_text, "60%");

    s_volume_bar = lv_obj_create(s_menu_page);
    if (!s_volume_bar) return false;
    lv_obj_set_pos(s_volume_bar, CC_VOLUME_BAR_X, CC_VOLUME_BAR_Y);
    lv_obj_set_size(s_volume_bar, CC_VOLUME_BAR_W, CC_VOLUME_BAR_H);
    lv_obj_set_style_radius(s_volume_bar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_volume_bar, lv_color_hex(0x555b63), 0);
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
    lv_obj_set_style_bg_color(s_volume_fill, lv_color_hex(0x8edce8), 0);
    lv_obj_set_style_bg_opa(s_volume_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_volume_fill, 0, 0);
    lv_obj_set_style_pad_all(s_volume_fill, 0, 0);
    lv_obj_remove_flag(s_volume_fill,
                       LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_volume_knob = lv_obj_create(s_menu_page);
    if (!s_volume_knob) return false;
    lv_obj_set_size(s_volume_knob, 11, 11);
    lv_obj_set_style_radius(s_volume_knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_volume_knob, lv_color_hex(0x8edce8), 0);
    lv_obj_set_style_bg_opa(s_volume_knob, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_volume_knob, 0, 0);
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
    lv_obj_set_style_bg_color(s_info_page, lv_color_hex(0x050607), 0);
    lv_obj_set_style_bg_opa(s_info_page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_info_page, 0, 0);
    lv_obj_set_style_pad_all(s_info_page, 0, 0);
    lv_obj_remove_flag(s_info_page,
                       LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *back = make_panel(s_info_page, 43, 43, 42, 42);
    if (!back) return false;
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
    lv_obj_t *back_symbol = make_label(back,
                                       &lv_font_montserrat_14,
                                       lv_color_hex(0xe7edf2),
                                       LV_OPA_COVER,
                                       LV_TEXT_ALIGN_CENTER);
    if (!back_symbol) return false;
    lv_obj_set_size(back_symbol, 42, 28);
    lv_obj_set_pos(back_symbol, 0, 7);
    lv_label_set_text(back_symbol, LV_SYMBOL_LEFT);

    lv_obj_t *title = make_label(s_info_page,
                                 &nuwax_font_control_center_cjk_12,
                                 lv_color_hex(0xf5f7fa),
                                 LV_OPA_COVER,
                                 LV_TEXT_ALIGN_CENTER);
    if (!title) return false;
    lv_obj_set_pos(title, 90, 59);
    lv_obj_set_width(title, 180);
    lv_label_set_text(title, "设备信息");

    s_info_body = make_label(s_info_page,
                             &nuwax_font_control_center_cjk_12,
                             lv_color_hex(0xcbd0d7),
                             220,
                             LV_TEXT_ALIGN_LEFT);
    if (!s_info_body) return false;
    lv_obj_set_pos(s_info_body, 72, 112);
    lv_obj_set_size(s_info_body, 216, 180);
    lv_label_set_long_mode(s_info_body, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_info_body, "--");
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
                                       ? lv_color_hex(0x4fc6d5)
                                       : pending
                                             ? lv_color_hex(0x566b70)
                                             : lv_color_hex(0x454a53);
    lv_obj_set_style_bg_color(track, track_color, 0);
    lv_obj_set_style_opa(track, locked ? 105 : LV_OPA_COVER, 0);
    lv_obj_set_x(knob, enabled ? 15 : 2);
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

static const char *provision_state_text(void) {
    if (s_provision_pending) {
        return s_provision_pending_target ? "启动中" : "关闭中";
    }
    return provisioning_active() ? "运行中" : "已关闭";
}

static void set_volume_visual(uint8_t volume) {
    if (!s_volume_fill || !s_volume_knob || !s_volume_text) {
        return;
    }
    const uint8_t clamped = volume > 100U ? 100U : volume;
    int width = (CC_VOLUME_BAR_W * clamped) / 100;
    if (width < 1) width = 1;
    lv_obj_set_width(s_volume_fill, width);
    int knob_x = CC_VOLUME_BAR_X + width - 5;
    if (knob_x < CC_VOLUME_BAR_X - 4) knob_x = CC_VOLUME_BAR_X - 4;
    if (knob_x > CC_VOLUME_BAR_X + CC_VOLUME_BAR_W - 5) {
        knob_x = CC_VOLUME_BAR_X + CC_VOLUME_BAR_W - 5;
    }
    lv_obj_set_pos(s_volume_knob, knob_x,
                   CC_VOLUME_BAR_Y - (11 - CC_VOLUME_BAR_H) / 2);
    lv_label_set_text_fmt(s_volume_text, "%u%%", (unsigned)clamped);
    transition_cache_mark_page_dirty(CC_PAGE_MENU);
}

static void render_menu(void) {
    if (!s_menu_page) {
        return;
    }
    const bool meeting = meeting_active();
    const bool provisioning = provisioning_active();
    const bool monitor = monitoring_active();
    const bool meeting_locked = monitor || provisioning || s_provision_pending;
    const bool provision_locked = monitor || meeting || s_meeting_pending;
    lv_label_set_text(s_meeting_state, meeting_state_text());
    lv_label_set_text(s_provision_state, provision_state_text());
    set_switch_visual(s_meeting_switch,
                      s_meeting_knob,
                      meeting,
                      s_meeting_pending,
                      meeting_locked);
    set_switch_visual(s_provision_switch,
                      s_provision_knob,
                      s_provision_pending ? s_provision_pending_target
                                          : provisioning,
                      s_provision_pending,
                      provision_locked);
    set_volume_visual(s_have_state ? s_state.volume : 60U);
    s_menu_dirty = false;
}

static void render_info(void) {
    if (!s_info_body) {
        return;
    }
    const esp_app_desc_t *app = esp_app_get_description();
    const char *ssid = s_have_state && s_state.ssid[0]
                           ? s_state.ssid
                           : "--";
    const char *ip = s_have_state && s_state.ip[0]
                         ? s_state.ip
                         : "--";
    lv_label_set_text_fmt(
        s_info_body,
        "设备\n%s\n\n固件版本\n%s\n\n网络\n%s\n%s",
        nuwax_device_uid_string()[0] ? nuwax_device_uid_string() : "--",
        app && app->version[0] ? app->version : "--",
        ssid,
        ip);
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
                         lv_color_hex(wifi_offline ? 0x7d8288 : 0xf8fbfc));
    status_card_set_icon(&s_status_cards[1], NUWAX_CC_ICON_CLOUD,
                         lv_color_hex(cloud_offline ? 0x7d8288 : 0xb9f5ff));
    status_card_set_icon(&s_status_cards[2], NUWAX_CC_ICON_BATTERY,
                         lv_color_hex(s_state.power == NUWAX_POWER_CHARGING
                                          ? 0x8ee2c0
                                          : 0xf8fbfc));
    lv_color_t dialog_color = lv_color_hex(0x8ee2c0);
    if (s_state.monitoring != NUWAX_MONITORING_UI_DISABLED) {
        dialog_color = lv_color_hex(0xb9f5ff);
    } else if (s_state.meeting != NUWAX_MEETING_UI_DISABLED) {
        dialog_color = lv_color_hex(0xe2d5ff);
    } else if (s_state.provisioning_active) {
        dialog_color = lv_color_hex(0x8ee2c0);
    } else {
        switch (s_state.dialog) {
            case NUWAX_DIALOG_LISTENING:
            case NUWAX_DIALOG_RECOGNIZING:
                dialog_color = lv_color_hex(0xb9f5ff);
                break;
            case NUWAX_DIALOG_THINKING:
                dialog_color = lv_color_hex(0xd9d1ff);
                break;
            case NUWAX_DIALOG_SPEAKING:
                dialog_color = lv_color_hex(0xffd4a4);
                break;
            case NUWAX_DIALOG_SLEEP:
                dialog_color = lv_color_hex(0xb8bcf7);
                break;
            case NUWAX_DIALOG_ERROR:
                dialog_color = lv_color_hex(0xff9ba6);
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
    lv_label_set_text(s_status_cards[3].value, dialog_text(&s_state));
    if (s_battery_fill) {
        const uint8_t percent = s_state.battery_valid
                                    ? (s_state.battery_percent > 100U
                                           ? 100U
                                           : s_state.battery_percent)
                                    : 100U;
        lv_obj_set_width(s_battery_fill, (16 * percent + 50) / 100);
        lv_obj_set_style_bg_color(
            s_battery_fill,
            lv_color_hex(s_state.power == NUWAX_POWER_CHARGING
                             ? 0x8ee2c0
                             : 0xf8fbfc), 0);
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
            lv_obj_set_style_bg_color(s_footer_dot,
                                       lv_color_hex(0xf0b764), 0);
        } else if (connecting) {
            lv_label_set_text(s_footer_label, "正在建立连接");
            lv_obj_set_style_bg_color(s_footer_dot,
                                       lv_color_hex(0x8edce8), 0);
        } else {
            lv_label_set_text(s_footer_label, "设备运行正常");
            lv_obj_set_style_bg_color(s_footer_dot,
                                       lv_color_hex(0x78d7ad), 0);
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
    if (pending_changed) {
        s_menu_dirty = true;
        if (s_page == CC_PAGE_MENU || s_page_transition_active) {
            render_menu();
        }
    }
    /* Snapshot one component per idle tick. A page generation is committed
     * only after every component has been captured from the same state. */
    if (!s_open && s_have_state && s_transition_cache_available &&
        !s_transition_cache_active) {
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

static int32_t circle_half_width(int32_t radius, int32_t distance_y) {
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

static void transition_cache_release(void) {
    if (s_transition_cache_arena) {
        heap_caps_free(s_transition_cache_arena);
    }
    memset(s_transition_cache, 0, sizeof(s_transition_cache));
    s_transition_cache_arena = NULL;
    s_transition_cache_arena_size = 0U;
    s_transition_cache_available = false;
    s_transition_cache_active = false;
    s_transition_cache_build_active = false;
    s_transition_cache_build_next = 0U;
    s_transition_cache_build_components = 0U;
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
        s_volume_bar,
        s_volume_knob,
    };
    size_t total_size = 0U;
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
    }
    total_size = align_size(total_size, CC_TRANSITION_CACHE_ALIGNMENT);

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
    s_transition_cache_available = true;
    s_status_cache_dirty = true;
    s_menu_cache_dirty = true;
    ESP_LOGI(TAG,
             "transition cache ready components=%u psram_bytes=%u remaining=%u",
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

static void transition_cache_invalidate_components(cc_page_t page) {
    for (size_t i = 0; i < CC_TRANSITION_CACHE_COMPONENTS; ++i) {
        if (s_transition_cache[i].page == page) {
            s_transition_cache[i].valid = false;
        }
    }
}

static void transition_cache_begin_build(cc_page_t page) {
    transition_cache_invalidate_components(page);
    s_transition_cache_build_active = true;
    s_transition_cache_build_page = page;
    s_transition_cache_build_generation =
        transition_cache_page_generation(page);
    s_transition_cache_build_next = 0U;
    s_transition_cache_build_components = 0U;
    s_transition_cache_build_work_us = 0U;
    s_transition_cache_build_max_component_us = 0U;
    s_transition_cache_build_started_us = esp_timer_get_time();
}

static bool transition_cache_capture_page(cc_page_t page) {
    if (!s_transition_cache_available || !s_surface) return false;
    const uint32_t generation = transition_cache_page_generation(page);
    if (!s_transition_cache_build_active ||
        s_transition_cache_build_page != page ||
        s_transition_cache_build_generation != generation) {
        transition_cache_begin_build(page);
    }

    size_t component_index = s_transition_cache_build_next;
    while (component_index < CC_TRANSITION_CACHE_COMPONENTS &&
           s_transition_cache[component_index].page != page) {
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
    const bool captured =
        lv_snapshot_take_to_draw_buf(component->source,
                                     LV_COLOR_FORMAT_RGB565,
                                     &component->draw_buf) == LV_RESULT_OK;
    component->valid = captured;
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
           s_transition_cache[next].page != page) {
        next++;
    }
    if (next < CC_TRANSITION_CACHE_COMPONENTS) {
        s_transition_cache_build_next = next;
        return false;
    }

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
    if (!component || !component->valid || !component->source || !pixels ||
        lv_obj_has_flag(component->source, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    const lv_opa_t opacity = lv_obj_get_style_opa(
        component->source, LV_PART_MAIN);
    if (opacity <= LV_OPA_MIN) return;

    lv_area_t target;
    lv_obj_get_coords(component->source, &target);
    lv_area_increase(&target, component->ext_draw_size,
                     component->ext_draw_size);
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
        const int32_t left_q4 = s_reveal_mask_rows[global_y].left_q4;
        const int32_t right_q4 = s_reveal_mask_rows[global_y].right_q4;
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
            memcpy(destination, source, count * sizeof(uint16_t));
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
        s_detail != CC_DETAIL_NONE) {
        return;
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
    memcpy(target + (size_t)local_y * target_stride +
               (size_t)(x1 - target_x),
           frozen + (size_t)global_y * frozen_stride + (size_t)x1,
           (size_t)(x2 - x1 + 1) * sizeof(uint16_t));
}

static void control_center_flush_filter(
    int32_t x, int32_t y, int32_t width, int32_t height,
    uint16_t *pixels, size_t stride_pixels, void *user_data) {
    (void)user_data;
    if (!pixels || width <= 0 || height <= 0 || stride_pixels == 0U) {
        return;
    }
    transition_cache_compose(x, y, width, height, pixels, stride_pixels);
    if (!s_backdrop_frozen_face || s_backdrop_progress <= 0 ||
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

    const int32_t radius = backdrop_radius(s_backdrop_progress);
    draw.bg_opa = LV_OPA_TRANSP;
    draw.border_width = 1;
    draw.border_color = lv_color_hex(0xaae6f0);
    if (radius >= 94) {
        const lv_area_t inner = {
            .x1 = 86, .y1 = 86, .x2 = 273, .y2 = 273,
        };
        draw.border_opa = 14;
        lv_draw_rect(layer, &draw, &inner);
    }
    if (radius >= 151) {
        const lv_area_t outer = {
            .x1 = 29, .y1 = 29, .x2 = 330, .y2 = 330,
        };
        draw.border_opa = 20;
        lv_draw_rect(layer, &draw, &outer);
    }
}

static void apply_page_progress(int32_t progress, bool quantize) {
    if (!s_status_page || !s_menu_page) return;
    if (progress < 0) progress = 0;
    if (progress > 1000) progress = 1000;
    if (quantize && progress > 0 && progress < 1000) {
        const int32_t step = (progress * CC_PAGE_ANIM_STEPS + 500) / 1000;
        progress = step * 1000 / CC_PAGE_ANIM_STEPS;
    }
    if (progress == s_page_progress) return;
    s_page_progress = progress;
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

    /* Both page trees remain at fixed coordinates. The shared reveal updater
     * changes only child clips and opacity. During transitions the trees are
     * hidden and these values drive the PSRAM component compositor. */
    if (s_reveal_raw >= 0) {
        apply_reveal_progress(s_reveal_raw, false, true);
    }
}

static void page_progress_exec(void *obj, int32_t value) {
    (void)obj;
    apply_page_progress(value, true);
}

static void start_page_perf(void) {
    s_page_perf_active = true;
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
    if (minimum_met) {
        ESP_LOGI(TAG,
                 "control center page perf frames=%u elapsed_ms=%u fps=%u.%u max_gap_ms=%u minimum_fps=%u target_fps=%u",
                 (unsigned)s_page_perf_frames,
                 (unsigned)(elapsed_us > 0 ? elapsed_us / 1000LL : 0LL),
                 (unsigned)(fps_x10 / 10U), (unsigned)(fps_x10 % 10U),
                 (unsigned)(s_page_perf_max_gap_us / 1000U),
                 (unsigned)CC_OPERATION_MIN_FPS,
                 (unsigned)CONFIG_NUWAX_UI_ANIMATION_TARGET_FPS);
    } else {
        ESP_LOGW(TAG,
                 "control center page below minimum frames=%u elapsed_ms=%u fps=%u.%u max_gap_ms=%u minimum_fps=%u target_fps=%u",
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
    return transition_cache_capture_page(page);
}

static bool transition_cache_activate(bool both_pages) {
    if (s_transition_cache_active) return true;
    const cc_page_t other_page = s_page == CC_PAGE_STATUS
                                     ? CC_PAGE_MENU : CC_PAGE_STATUS;
    const bool current_ready =
        !(s_page == CC_PAGE_STATUS ? s_status_dirty : s_menu_dirty) &&
        !(s_page == CC_PAGE_STATUS ? s_status_cache_dirty
                                   : s_menu_cache_dirty) &&
        transition_cache_page_valid(s_page);
    const bool other_ready =
        !(other_page == CC_PAGE_STATUS ? s_status_dirty : s_menu_dirty) &&
        !(other_page == CC_PAGE_STATUS ? s_status_cache_dirty
                                       : s_menu_cache_dirty) &&
        transition_cache_page_valid(other_page);
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
    set_page_tree_mode(s_page, both_pages);
    ESP_LOGI(TAG, "transition compositor active mode=%s cache_bytes=%u",
             both_pages ? "page" : "reveal",
             (unsigned)s_transition_cache_arena_size);
    return true;
}

static bool transition_cache_deactivate(void) {
    if (!s_transition_cache_active) return false;
    s_transition_cache_active = false;
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
    finish_page_perf();
    settle_page_transition(s_page_transition_settle_page);
}

static bool begin_page_transition(cc_page_t target) {
    if (s_page_transition_active || target == s_page) {
        return false;
    }
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

static void set_page(cc_page_t page, bool animate) {
    if (!s_status_page || !s_menu_page || s_page == page ||
        s_page_transition_active) {
        return;
    }
    const int32_t target = page == CC_PAGE_STATUS ? 0 : 1000;
    lv_anim_delete(s_surface, page_progress_exec);
    if (!animate) {
        switch_page_without_animation(page);
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
    if (!s_open || s_closing || s_detail != CC_DETAIL_NONE || !s_surface) {
        return;
    }
    if (!s_page_dragging) {
        const bool toward_other =
            (s_page == CC_PAGE_STATUS && delta_x < 0) ||
            (s_page == CC_PAGE_MENU && delta_x > 0);
        if (!toward_other) return;
        const cc_page_t target = s_page == CC_PAGE_STATUS
                                     ? CC_PAGE_MENU
                                     : CC_PAGE_STATUS;
        if (!begin_page_transition(target)) {
            return;
        }
        s_page_dragging = true;
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
    s_page_dragging = false;
    s_page_transition_settle_page = target;
    const int32_t target_progress = target == CC_PAGE_STATUS ? 0 : 1000;
    lv_anim_delete(s_surface, page_progress_exec);
    if (s_page_progress == target_progress) {
        settle_page_transition(target);
    } else {
        const uint32_t distance = (uint32_t)(
            s_page_progress > target_progress
                ? s_page_progress - target_progress
                : target_progress - s_page_progress);
        uint32_t duration_ms = (CC_PAGE_ANIM_MS * distance + 999U) / 1000U;
        if (duration_ms < 80U) duration_ms = 80U;
        lv_anim_t page_anim;
        start_page_perf();
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

static bool set_y_if_changed(lv_obj_t *obj, int32_t y) {
    if (obj && lv_obj_get_y(obj) != y) {
        lv_obj_set_y(obj, y);
        return true;
    }
    return false;
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
    if (progress < 0) progress = 0;
    if (progress > 1000) progress = 1000;
    if (quantize && progress > 0 && progress < 1000) {
        const int32_t step = (progress * CC_ANIM_STEPS + 500) / 1000;
        progress = step * 1000 / CC_ANIM_STEPS;
    }
    const bool progress_changed = progress != s_reveal_raw;
    if (!progress_changed && !force_content) return;
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
            s_pager_clip, s_pager, 148, 43, 64, 18,
            0, 0, pager_progress);
        pager_changed |= set_opa_if_changed(
            s_pager, opacity_from_permille(pager_progress));
    }

    const int32_t header_progress = smoothstep_permille(progress, 160, 620);
    const int32_t status_header_progress = multiply_permille(
        header_progress, status_page_visibility(0, 200));
    const int32_t menu_header_progress = multiply_permille(
        header_progress, menu_page_visibility(480, 680));
    if (s_status_header) {
        header_changed |= apply_content_reveal(
            s_status_header_clip, s_status_header,
            CC_HEADER_X, 67, CC_HEADER_W, 28,
            0, 7, status_header_progress);
        header_changed |= set_opa_if_changed(
            s_status_header, opacity_from_permille(status_header_progress));
    }
    if (s_menu_header) {
        header_changed |= apply_content_reveal(
            s_menu_header_clip, s_menu_header,
            CC_HEADER_X, 67, CC_HEADER_W, 28,
            0, 7, menu_header_progress);
        header_changed |= set_opa_if_changed(
            s_menu_header, opacity_from_permille(menu_header_progress));
    }
    const int32_t footer_progress = smoothstep_permille(progress, 580, 920);
    const int32_t status_footer_progress = multiply_permille(
        footer_progress, status_page_visibility(260, 440));
    if (s_footer) {
        footer_changed |= apply_content_reveal(
            s_footer_clip, s_footer, 144, 306, 100, 14,
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
    /* A 180-permille fade gives each card about three samples at 40 FPS.
     * Staggering the windows caps the largest scheduled dirty set at the
     * previous implementation's 38,500 pixels instead of trading smoothness
     * for a larger render spike. */
    static const int16_t status_page_fade_start[4] = {20, 100, 180, 260};
    static const int16_t menu_page_fade_start[4] = {480, 560, 640, 720};
    for (size_t i = 0; i < 4; ++i) {
        const int32_t tile_progress = smoothstep_permille(
            progress, reveal_start[i], reveal_start[i] + 340);
        const int32_t status_tile_progress = multiply_permille(
            tile_progress,
            status_page_visibility(status_page_fade_start[i],
                                   status_page_fade_start[i] + 180));
        const int32_t menu_tile_progress = multiply_permille(
            tile_progress,
            menu_page_visibility(menu_page_fade_start[i],
                                 menu_page_fade_start[i] + 180));
        if (s_status_cards[i].root) {
            card_changed[i] |= apply_content_reveal(
                s_status_cards[i].clip,
                s_status_cards[i].root,
                base_x[i], base_y[i],
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
                base_x[i], menu_y,
                CC_CARD_W, CC_MENU_ACTION_H,
                offset_x[i], offset_y[i], menu_tile_progress);
            card_changed[i] |= set_opa_if_changed(
                s_menu_cards[i], opacity_from_permille(menu_tile_progress));
        }
    }

    const int32_t volume_progress = smoothstep_permille(progress, 880, 980);
    const int32_t menu_volume_progress = multiply_permille(
        volume_progress, menu_page_visibility(820, 1000));
    const lv_opa_t volume_opa = opacity_from_permille(menu_volume_progress);
    if (s_menu_volume) {
        volume_changed |= set_hidden_if_changed(
            s_menu_volume, menu_volume_progress <= 0);
        if (menu_volume_progress > 0) {
            volume_changed |= set_y_if_changed(s_menu_volume, CC_MENU_VOLUME_Y);
        }
        volume_changed |= set_opa_if_changed(s_menu_volume, volume_opa);
    }
    if (s_volume_bar) {
        volume_changed |= set_hidden_if_changed(
            s_volume_bar, menu_volume_progress <= 0);
        if (menu_volume_progress > 0) {
            volume_changed |= set_y_if_changed(s_volume_bar, CC_VOLUME_BAR_Y);
        }
        volume_changed |= set_opa_if_changed(s_volume_bar, volume_opa);
    }
    if (s_volume_knob) {
        volume_changed |= set_hidden_if_changed(
            s_volume_knob, menu_volume_progress <= 0);
        if (menu_volume_progress > 0) {
            volume_changed |= set_y_if_changed(
                s_volume_knob,
                CC_VOLUME_BAR_Y - (11 - CC_VOLUME_BAR_H) / 2);
        }
        volume_changed |= set_opa_if_changed(s_volume_knob, volume_opa);
    }

    resume_display_invalidation(s_surface, content_invalidation_suspended);
    if (content_invalidation_suspended) {
        if (pager_changed) invalidate_surface_region(148, 43, 64, 25);
        if (header_changed) {
            invalidate_surface_region(CC_HEADER_X, 67, CC_HEADER_W, 35);
        }
        if (footer_changed) invalidate_surface_region(144, 306, 100, 21);
        const int32_t cards_x = CC_CARD_LEFT_X - 8;
        const int32_t cards_width =
            CC_CARD_RIGHT_X + CC_CARD_W + 8 - cards_x;
        if (card_changed[0] || card_changed[1]) {
            invalidate_surface_region(cards_x, CC_STATUS_ROW0_Y - 7,
                                      cards_width, 74);
        }
        if (card_changed[2] || card_changed[3]) {
            invalidate_surface_region(cards_x, CC_MENU_ROW1_Y - 7,
                                      cards_width, 82);
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
    (void)anim;
    apply_reveal_progress(s_reveal_anim_target, false, false);
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
    if (planned_step > s_reveal_anim_step + 1U) {
        s_reveal_perf_skipped_steps +=
            planned_step - s_reveal_anim_step - 1U;
    }
    s_reveal_anim_step = planned_step;
    apply_reveal_animation_step(s_reveal_anim_step);
    if (s_reveal_anim_step < s_reveal_anim_steps) return;

    s_reveal_timer = NULL;
    lv_timer_delete(timer);
    reveal_completed(NULL);
}

static void animate_reveal(bool opening) {
    if (!s_reveal_clip) return;
    cancel_reveal_animation();
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
    nuwax_device_qr_view_hide();
    if (s_info_page) {
        lv_obj_add_flag(s_info_page, LV_OBJ_FLAG_HIDDEN);
    }
    s_detail = CC_DETAIL_NONE;
}

static void open_control_center(cc_page_t page) {
    if (!s_root || !s_surface) {
        return;
    }
    const bool reversing_close = s_open && s_closing;
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
    lv_obj_add_event_cb(s_reveal_clip, draw_backdrop_cb,
                        LV_EVENT_DRAW_MAIN, NULL);
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
    lv_obj_set_style_border_width(s_surface, 0, 0);
    lv_obj_set_style_pad_all(s_surface, 0, 0);
    lv_obj_remove_flag(s_surface,
                       LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    const char *failed_stage = NULL;
    if (!create_status_page()) {
        failed_stage = "status_page";
    } else if (!create_menu_page()) {
        failed_stage = "menu_page";
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
    nuwax_lvgl_port_set_flush_filter(control_center_flush_filter, NULL);
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
    nuwax_lvgl_port_set_flush_filter(NULL, NULL);
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
    s_reveal_clip = NULL;
    s_surface = NULL;
    s_status_page = NULL;
    s_menu_page = NULL;
    s_info_page = NULL;
    s_pager = NULL;
    s_pager_clip = NULL;
    s_status_header = NULL;
    s_status_header_clip = NULL;
    s_menu_header = NULL;
    s_menu_header_clip = NULL;
    s_status_dot = NULL;
    s_menu_dot = NULL;
    s_page_name = NULL;
    s_cloud_beacon = NULL;
    s_battery_fill = NULL;
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
    s_provision_state = NULL;
    s_provision_switch = NULL;
    s_provision_knob = NULL;
    s_volume_text = NULL;
    s_menu_volume = NULL;
    s_volume_bar = NULL;
    s_volume_fill = NULL;
    s_volume_knob = NULL;
    s_info_body = NULL;
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
}

void nuwax_drawer_update(const nuwax_device_state_t *state) {
    if (!state) {
        return;
    }
    const bool status_changed =
        !s_have_state || state_affects_status(&s_state, state);
    bool menu_changed =
        !s_have_state || state_affects_menu(&s_state, state);
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
    menu_changed = menu_changed ||
                   meeting_pending_before != s_meeting_pending ||
                   provision_pending_before != s_provision_pending;
    s_menu_dirty = s_menu_dirty || menu_changed;
    if (s_open && !s_closing) {
        if (s_status_dirty &&
            (s_page == CC_PAGE_STATUS || s_page_transition_active)) {
            render_status();
        }
        if (s_menu_dirty &&
            (s_page == CC_PAGE_MENU || s_page_transition_active)) {
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
    if (!s_root || !s_surface || s_open) return;
    hide_detail();
    if (s_page != CC_PAGE_MENU) {
        set_page(CC_PAGE_MENU, false);
    }
    render_page_if_dirty(CC_PAGE_MENU);
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
    if (!s_open || !s_reveal_clip || s_closing) {
        return;
    }
    if (s_page_transition_active) {
        lv_anim_delete(s_surface, page_progress_exec);
        cancel_page_perf();
        settle_page_transition(s_page_transition_settle_page);
        s_page_dragging = false;
    }
    s_closing = true;
    hide_detail();
    stop_local_animations();
    animate_reveal(false);
    ESP_LOGI(TAG, "control center closing");
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
        set_page(CC_PAGE_MENU, true);
    }
}

void nuwax_drawer_prev_page(void) {
    if (s_open && s_detail == CC_DETAIL_NONE) {
        set_page(CC_PAGE_STATUS, true);
    }
}

void nuwax_drawer_back(void) {
    if (!s_open || s_detail == CC_DETAIL_NONE) {
        return;
    }
    hide_detail();
    lv_obj_move_foreground(s_pager_clip);
    render_menu();
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
        if (!qr_encoded) {
            ESP_LOGW(TAG, "device QR detail shown before payload is ready");
        }
    }
}

bool nuwax_drawer_show_provisioning_qr(void) {
    if (!s_root || !s_have_state || !s_state.provisioning_active) {
        return false;
    }
    if (!s_open) {
        open_control_center(CC_PAGE_MENU);
    } else {
        if (s_closing) {
            return false;
        }
        if (s_detail == CC_DETAIL_QR) {
            return true;
        }
        if (s_detail != CC_DETAIL_NONE || s_page != CC_PAGE_MENU) {
            return false;
        }
    }
    show_qr();
    return s_open && s_detail == CC_DETAIL_QR;
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
}

nuwax_drawer_action_result_t nuwax_drawer_tap(uint16_t x, uint16_t y) {
    nuwax_drawer_action_result_t result = {
        .action = NUWAX_DRAWER_ACTION_NONE,
        .value = 0,
    };
    if (!s_open || s_closing) {
        return result;
    }
    if (s_detail != CC_DETAIL_NONE) {
        if (point_in_rect(x, y, 34, 34, 64, 64)) {
            nuwax_drawer_back();
        }
        return result;
    }
    if (s_page != CC_PAGE_MENU) {
        return result;
    }
    if (point_in_rect(x, y, CC_CARD_LEFT_X, CC_MENU_ROW0_Y,
                      CC_CARD_W, CC_MENU_ACTION_H)) {
        if (monitoring_active() || provisioning_active() ||
            meeting_transitioning() ||
            s_meeting_pending || s_provision_pending) {
            return result;
        }
#if CONFIG_NUWAX_UI_DISPLAY_TEST_MODE
        s_test_meeting_active = !s_test_meeting_active;
        s_test_provision_active = false;
        render_menu();
#else
        s_meeting_pending_target = !meeting_active();
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
        if (monitoring_active() || meeting_active() ||
            s_meeting_pending || s_provision_pending) {
            return result;
        }
#if CONFIG_NUWAX_UI_DISPLAY_TEST_MODE
        s_test_provision_active = !s_test_provision_active;
        s_test_meeting_active = false;
        render_menu();
#else
        s_provision_pending_target = !provisioning_active();
        s_provision_pending = true;
        s_provision_pending_deadline_us =
            esp_timer_get_time() +
            (int64_t)CC_PROVISION_PENDING_TIMEOUT_MS * 1000LL;
        render_menu();
        result.action = NUWAX_DRAWER_ACTION_REPROVISION_WIFI;
        result.value = s_provision_pending_target ? 1 : 0;
#endif
        return result;
    }
    if (point_in_rect(x, y, CC_CARD_LEFT_X, CC_MENU_ACTION_Y,
                      CC_MENU_ACTION_W, CC_MENU_ACTION_H)) {
        show_qr();
        return result;
    }
    if (point_in_rect(x, y, CC_CARD_RIGHT_X, CC_MENU_ACTION_Y,
                      CC_MENU_ACTION_W, CC_MENU_ACTION_H)) {
        show_info();
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
    if (action == NUWAX_DRAWER_ACTION_SET_VOLUME) {
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
    if (!s_open || s_detail != CC_DETAIL_NONE || s_page != CC_PAGE_MENU ||
        !point_in_rect(x, y,
                       CC_VOLUME_BAR_X - 12,
                       CC_VOLUME_BAR_Y - 14,
                       CC_VOLUME_BAR_W + 24,
                       34)) {
        return false;
    }
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
    if (!s_open || !s_reveal_clip) {
        return;
    }
    if (y > CC_OPEN_Y) y = CC_OPEN_Y;
    if (y < CC_HIDDEN_Y) y = CC_HIDDEN_Y;
    s_dragging = true;
    s_motion_y = y;
    cancel_reveal_animation();
    s_closing = false;
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
    s_motion_y = CC_OPEN_Y;
    animate_reveal(true);
}

int16_t nuwax_drawer_current_y(void) {
    return s_open ? s_motion_y : CC_HIDDEN_Y;
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
