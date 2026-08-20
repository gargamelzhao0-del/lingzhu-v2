#include "emotion_animation_player.h"

#include <limits.h>
#include <stdatomic.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "miniz.h"
#include "emotion_component_bridge_policy.h"
#include "emotion_delta_format.h"
#include "emotion_segment_policy.h"
#include "nuwax_lvgl_port.h"

#ifndef CONFIG_NUWAX_UI_DISPLAY_TEST_MODE
#define CONFIG_NUWAX_UI_DISPLAY_TEST_MODE 0
#endif
#ifndef CONFIG_NUWAX_UI_BACKGROUND_DECODE_ENABLE
#define CONFIG_NUWAX_UI_BACKGROUND_DECODE_ENABLE 1
#endif

static const char *TAG = "emotion_player";

#define FRAME_WIDTH 360U
#define FRAME_HEIGHT 360U
#define FRAME_PIXELS (FRAME_WIDTH * FRAME_HEIGHT)
#define FRAME_RGB_BYTES (FRAME_PIXELS * sizeof(uint16_t))
#define INFLATE_INPUT_BYTES 16384U
#define DECODE_PROGRESS_LIMIT 4096U
#define HEALTH_LOG_INTERVAL_US                                                \
    (CONFIG_NUWAX_UI_DISPLAY_TEST_MODE ? 10000000LL : 30000000LL)
#define MIN_ANIMATED_FPS 40U
#define MAX_ANIMATED_FPS 60U
#define DECODE_BUDGET_PERCENT 60U
#define DIRTY_BAND_HEIGHT 20U
#define DIRTY_BAND_COUNT \
    ((FRAME_HEIGHT + DIRTY_BAND_HEIGHT - 1U) / DIRTY_BAND_HEIGHT)
#define DIRTY_ROW_MERGE_GAP 8U
#define MAX_DIRTY_REGIONS 24U
#define TARGET_DIRTY_REGIONS 12U
#define DIRTY_REGION_FIXED_COST_PIXELS 256U
#define CHANGED_WORD_COUNT ((FRAME_PIXELS + 31U) / 32U)
#define V2_MAX_PUBLISH_RECORD_BYTES                                      \
    (sizeof(v2_frame_header_t) +                                         \
     NUWAX_EMOTION_DELTA_MAX_FRAME_RUNS * sizeof(v2_run_header_t) +      \
     FRAME_PIXELS * sizeof(uint16_t))
#define DECODE_TASK_STACK_SIZE 4096U
/* Decode is a producer, not the real-time display owner. Keep it below LVGL
 * so a decompression burst cannot delay a frame deadline; it remains bounded
 * by two frame slots and yields at every frame boundary. */
#define DECODE_TASK_PRIORITY 4U
#define DECODE_TASK_CORE 0
#define DECODE_TASK_STOP_TIMEOUT_MS 1000U
#define DECODE_QUIESCE_TIMEOUT_MS 150U
#define V2_DECODE_SLOT_COUNT 2U
#define V2_SECOND_SLOT_PSRAM_RESERVE_BYTES (1024U * 1024U)
#define COMPONENT_BRIDGE_PSRAM_RESERVE_BYTES (1024U * 1024U)
#define V3_KEY_RUN_BYTES 3U
#define V4_MAX_COMPRESSED_FRAME_BYTES (12U * 1024U)
#define V4_DECODE_SLOT_BYTES (FRAME_PIXELS + V4_MAX_COMPRESSED_FRAME_BYTES)

_Static_assert(FRAME_PIXELS > TINFL_LZ_DICT_SIZE,
               "emotion frames must support multiple dictionary windows");
_Static_assert((FRAME_PIXELS % TINFL_LZ_DICT_SIZE) != 0U,
               "emotion frames must support partial final dictionary windows");
_Static_assert(DIRTY_BAND_COUNT <= 24U,
               "leave LVGL invalidation slots for concurrent UI overlays");

typedef struct {
    const nuwax_emotion_asset_segment_t *segment;
    nuwax_emotion_asset_segment_t stream_segment;
    uint16_t segment_index;
    uint32_t compressed_loaded;
    uint32_t input_pos;
    uint32_t input_size;
    uint32_t dictionary_pos;
    uint32_t pending_pos;
    uint32_t pending_size;
    uint32_t raw_bytes;
    uint32_t raw_crc32;
    uint32_t frame_bytes;
    uint16_t frame_index;
    tinfl_status status;
} decoder_state_t;

typedef nuwax_emotion_delta_frame_header_t v2_frame_header_t;
typedef nuwax_emotion_delta_run_header_t v2_run_header_t;

typedef struct {
    uint16_t min_x;
    uint16_t min_y;
    uint16_t max_x;
    uint16_t max_y;
} dirty_region_t;

typedef enum {
    DECODE_WORKER_IDLE = 0,
    DECODE_WORKER_REQUESTED,
    DECODE_WORKER_RUNNING,
    DECODE_WORKER_READY,
    DECODE_WORKER_FAILED,
} decode_worker_state_t;

typedef enum {
    DECODE_SLOT_FREE = 0,
    DECODE_SLOT_FILLING,
    DECODE_SLOT_READY,
    DECODE_SLOT_APPLYING,
} decode_slot_state_t;

typedef struct {
    atomic_int state;
    uint8_t *data;
    uint32_t size;
    uint32_t decode_us;
    uint32_t encoded_pixels;
    uint32_t generation;
    uint32_t sequence;
    uint16_t source_segment_index;
    uint16_t source_frame_index;
    uint16_t source_frame_count;
    bool bootstrap_delta;
    bool v4_frame;
    bool absolute;
    bool regional_payload;
    uint8_t region_count;
    nuwax_emotion_asset_region_t
        regions[NUWAX_EMOTION_ASSET_V4_MAX_REGIONS];
} decode_frame_slot_t;

typedef enum {
    V3_BOOTSTRAP_KEY = 0,
    V3_BOOTSTRAP_TRANSITION,
    V3_BOOTSTRAP_WRAP,
} v3_bootstrap_t;

static lv_obj_t *s_image;
static lv_image_dsc_t s_image_dsc;
#define FRAME_PATCH_OBJECT_COUNT NUWAX_EMOTION_ASSET_V4_MAX_REGIONS
#define COMPONENT_BRIDGE_FPS 40U
#define COMPONENT_BRIDGE_MIN_FPS 30U
#define COMPONENT_BRIDGE_PERIOD_US (1000000U / COMPONENT_BRIDGE_FPS)
#define COMPONENT_BRIDGE_DIRECT_PRESENT_TIMEOUT_MS 2U
#define COMPONENT_BRIDGE_FINAL_PRESENT_TIMEOUT_MS 8U
#define BRIDGE_REGION_WORK_CAPACITY 32U

typedef struct {
    nuwax_emotion_asset_region_t region;
    bool active;
} frame_patch_object_t;

static frame_patch_object_t s_frame_patches[FRAME_PATCH_OBJECT_COUNT];
static lv_obj_t *s_frame_patch_compositor;

typedef struct {
    int16_t eye_y;
    int16_t eye_spread;
    int16_t eye_width;
    int16_t eye_height;
    uint16_t close_ms;
    uint16_t reshape_ms;
    uint16_t open_ms;
    bool sleep_curve;
} component_pose_t;

typedef struct {
    bool active;
    nuwax_emotion_asset_id_t source_asset;
    nuwax_emotion_asset_id_t target_asset;
    component_pose_t source_pose;
    component_pose_t target_pose;
    uint16_t close_ms;
    uint16_t reshape_ms;
    uint16_t open_ms;
    uint16_t *source_rgb565;
    uint16_t *target_rgb565;
    uint8_t *target_indices;
    uint16_t target_segment;
    uint16_t target_frame_index;
    bool target_ready;
    bool timeline_started;
    bool source_cleared;
    uint32_t decode_generation;
    int64_t started_us;
    int64_t next_frame_us;
    int64_t pause_started_us;
    int64_t first_frame_us;
    int64_t last_frame_us;
    uint32_t visual_elapsed_us;
    uint32_t frames;
    uint32_t max_gap_us;
    uint64_t total_render_us;
    uint32_t max_render_us;
    uint32_t target_decode_us;
    uint32_t direct_present_frames;
    uint32_t fallback_present_frames;
    uint64_t direct_present_total_us;
    uint32_t direct_present_max_us;
} component_bridge_t;

typedef enum {
    BRIDGE_DECODE_IDLE = 0,
    BRIDGE_DECODE_REQUESTED,
    BRIDGE_DECODE_RUNNING,
    BRIDGE_DECODE_READY,
    BRIDGE_DECODE_FAILED,
} bridge_decode_state_t;

typedef struct {
    nuwax_emotion_asset_id_t asset_id;
    uint16_t segment_index;
    uint16_t frame_index;
    uint32_t generation;
    const uint16_t *visible_rgb565;
    uint16_t *source_rgb565;
    uint16_t *target_rgb565;
    uint8_t *target_indices;
    uint32_t decode_us;
    esp_err_t result;
} bridge_decode_job_t;

typedef struct {
    nuwax_emotion_asset_region_t parts[FRAME_PATCH_OBJECT_COUNT];
    uint8_t part_count;
    uint8_t mouth_index;
} component_bounds_t;

#define COMPONENT_PART_NONE UINT8_MAX

static component_bridge_t s_component_bridge;
static uint16_t *s_bridge_source_work;
static uint16_t *s_bridge_target_work;
static uint8_t *s_bridge_target_indices_work;
static bridge_decode_job_t s_bridge_decode_job;
static atomic_int s_bridge_decode_state;
static atomic_bool s_bridge_source_ready;
static atomic_uint s_bridge_decode_generation;

static esp_err_t load_asset(nuwax_emotion_asset_id_t asset_id);
static void cancel_component_bridge(void);
static esp_err_t start_segment(uint16_t segment_index);
static void decode_worker_request(void);
static esp_err_t decode_v4_handoff_frame(
    nuwax_emotion_asset_id_t asset_id, uint16_t segment_index,
    uint16_t target_frame_index, uint8_t *indices_out, uint16_t *rgb_out);
static uint8_t *s_indices;
static uint16_t *s_rgb565;
static uint8_t *s_dictionary;
static uint8_t *s_input;
static tinfl_decompressor *s_inflater;
static uint32_t *s_changed_bits;
static decode_frame_slot_t s_decode_slots[V2_DECODE_SLOT_COUNT];
static uint8_t s_decode_slot_count;
static uint32_t s_decode_slot_bytes;
static atomic_uint s_decode_generation;
static uint32_t s_next_decode_sequence;
static uint32_t s_next_publish_sequence;
static TaskHandle_t s_decode_task;
static StaticTask_t *s_decode_task_tcb;
static StackType_t *s_decode_task_stack;
static atomic_int s_decode_worker_state;
static atomic_bool s_decode_stop_requested;
static atomic_bool s_decode_task_exited;
static atomic_uint s_queued_asset_id;
static bool s_background_decode;
static bool s_cleanup_pending;
static int64_t s_cleanup_started_us;
static int64_t s_cleanup_last_log_us;
static esp_err_t s_decode_worker_error;
static uint32_t s_pending_decode_us;
static uint32_t s_pending_dirty_pixels;
static uint16_t s_band_min_x[DIRTY_BAND_COUNT];
static uint16_t s_band_min_y[DIRTY_BAND_COUNT];
static uint16_t s_band_max_x[DIRTY_BAND_COUNT];
static uint16_t s_band_max_y[DIRTY_BAND_COUNT];
static uint16_t s_row_min_x[FRAME_HEIGHT];
static uint16_t s_row_max_x[FRAME_HEIGHT];
static uint16_t s_palette[NUWAX_EMOTION_ASSET_PALETTE_COLORS];
static const nuwax_emotion_asset_entry_t *s_entry;
static decoder_state_t s_decoder;
static nuwax_emotion_asset_id_t s_asset_id;
static nuwax_emotion_asset_id_t s_return_asset_id;
static bool s_ready;
static bool s_interaction_active;
static bool s_transient_active;
static bool s_holding_frame;
static bool s_compare_rgb_on_absolute_frame;
static bool s_segment_advance_pending;
static v3_bootstrap_t s_v3_bootstrap;
static int64_t s_next_frame_us;
static int64_t s_asset_started_us;
static uint32_t s_frames_rendered;
static uint32_t s_full_frame_updates;
static uint32_t s_deadline_resyncs;
static uint32_t s_decode_budget_overruns;
static uint64_t s_decode_total_us;
static uint32_t s_decode_max_us;
static uint64_t s_dirty_pixels_total;
static uint64_t s_invalidated_pixels_total;
static int64_t s_last_health_log_us;
static uint64_t s_health_frames_total;
static uint64_t s_health_decode_total_us;
static uint64_t s_health_dirty_pixels_total;
static uint64_t s_health_invalidated_pixels_total;
static uint64_t s_health_full_frame_updates;
static uint64_t s_health_deadline_resyncs;
static uint64_t s_health_decode_budget_overruns;
static atomic_uint s_health_decode_cooperative_blocks;
static atomic_uint s_health_decode_switch_aborts;
static uint64_t s_last_health_frames;
static uint64_t s_last_health_decode_total_us;
static uint64_t s_last_health_dirty_pixels_total;
static uint64_t s_last_health_invalidated_pixels_total;
static uint64_t s_last_health_full_frame_updates;
static uint64_t s_last_health_deadline_resyncs;
static uint64_t s_last_health_decode_budget_overruns;
static uint32_t s_last_health_decode_cooperative_blocks;
static uint32_t s_last_health_decode_switch_aborts;

static decode_frame_slot_t *find_free_decode_slot(void)
{
    for (uint32_t i = 0U; i < s_decode_slot_count; ++i) {
        int expected = DECODE_SLOT_FREE;
        if (atomic_compare_exchange_strong_explicit(
                &s_decode_slots[i].state, &expected, DECODE_SLOT_FILLING,
                memory_order_acq_rel, memory_order_acquire)) {
            return &s_decode_slots[i];
        }
    }
    return NULL;
}

static decode_frame_slot_t *find_ready_decode_slot(uint32_t generation,
                                                   uint32_t sequence)
{
    for (uint32_t i = 0U; i < s_decode_slot_count; ++i) {
        decode_frame_slot_t *slot = &s_decode_slots[i];
        if (atomic_load_explicit(&slot->state, memory_order_acquire) ==
                DECODE_SLOT_READY &&
            slot->generation == generation && slot->sequence == sequence) {
            int expected = DECODE_SLOT_READY;
            if (atomic_compare_exchange_strong_explicit(
                    &slot->state, &expected, DECODE_SLOT_APPLYING,
                    memory_order_acq_rel, memory_order_acquire)) {
                return slot;
            }
        }
    }
    return NULL;
}

static uint32_t ready_decode_slot_count(uint32_t generation)
{
    uint32_t count = 0U;
    for (uint32_t i = 0U; i < s_decode_slot_count; ++i) {
        const decode_frame_slot_t *slot = &s_decode_slots[i];
        if (atomic_load_explicit(&slot->state, memory_order_acquire) ==
                DECODE_SLOT_READY &&
            slot->generation == generation) {
            count++;
        }
    }
    return count;
}

static void release_decode_slot(decode_frame_slot_t *slot)
{
    if (!slot) return;
    slot->size = 0U;
    slot->decode_us = 0U;
    slot->encoded_pixels = 0U;
    slot->bootstrap_delta = false;
    slot->v4_frame = false;
    slot->absolute = false;
    slot->region_count = 0U;
    memset(slot->regions, 0, sizeof(slot->regions));
    atomic_store_explicit(&slot->state, DECODE_SLOT_FREE,
                          memory_order_release);
}

static void discard_ready_decode_slots(void)
{
    for (uint32_t i = 0U; i < s_decode_slot_count; ++i) {
        decode_frame_slot_t *slot = &s_decode_slots[i];
        const int state = atomic_load_explicit(&slot->state,
                                               memory_order_acquire);
        if (state == DECODE_SLOT_READY) {
            release_decode_slot(slot);
        }
    }
}

static void clear_decoder(void)
{
    memset(&s_decoder, 0, sizeof(s_decoder));
    s_decoder.segment_index = NUWAX_EMOTION_ASSET_INVALID_SEGMENT;
    s_decoder.status = TINFL_STATUS_NEEDS_MORE_INPUT;
}

static void free_player_buffers(void)
{
    heap_caps_free(s_indices);
    heap_caps_free(s_rgb565);
    heap_caps_free(s_dictionary);
    heap_caps_free(s_input);
    heap_caps_free(s_inflater);
    heap_caps_free(s_changed_bits);
    heap_caps_free(s_bridge_source_work);
    heap_caps_free(s_bridge_target_work);
    heap_caps_free(s_bridge_target_indices_work);
    for (uint32_t i = 0U; i < V2_DECODE_SLOT_COUNT; ++i) {
        heap_caps_free(s_decode_slots[i].data);
        memset(&s_decode_slots[i], 0, sizeof(s_decode_slots[i]));
        atomic_store_explicit(&s_decode_slots[i].state, DECODE_SLOT_FREE,
                              memory_order_release);
    }
    s_indices = NULL;
    s_rgb565 = NULL;
    s_dictionary = NULL;
    s_input = NULL;
    s_inflater = NULL;
    s_changed_bits = NULL;
    s_bridge_source_work = NULL;
    s_bridge_target_work = NULL;
    s_bridge_target_indices_work = NULL;
    s_decode_slot_count = 0U;
    s_decode_slot_bytes = 0U;
}

static void reset_player_state(void)
{
    cancel_component_bridge();
    free_player_buffers();
    nuwax_emotion_asset_pack_deinit();
    memset(&s_image_dsc, 0, sizeof(s_image_dsc));
    memset(s_palette, 0, sizeof(s_palette));
    s_entry = NULL;
    s_ready = false;
    s_interaction_active = false;
    s_transient_active = false;
    s_holding_frame = false;
    s_compare_rgb_on_absolute_frame = false;
    s_segment_advance_pending = false;
    s_v3_bootstrap = V3_BOOTSTRAP_KEY;
    s_asset_id = 0;
    s_return_asset_id = 0;
    s_next_frame_us = 0;
    s_asset_started_us = 0;
    s_pending_decode_us = 0;
    s_pending_dirty_pixels = 0;
    s_decode_worker_error = ESP_OK;
    atomic_store_explicit(&s_decode_generation, 0U, memory_order_release);
    memset(&s_bridge_decode_job, 0, sizeof(s_bridge_decode_job));
    atomic_store_explicit(&s_bridge_decode_state, BRIDGE_DECODE_IDLE,
                          memory_order_release);
    atomic_store_explicit(&s_bridge_source_ready, false,
                          memory_order_release);
    atomic_store_explicit(&s_bridge_decode_generation, 0U,
                          memory_order_release);
    s_next_decode_sequence = 1U;
    s_next_publish_sequence = 1U;
    atomic_store_explicit(&s_queued_asset_id, 0U, memory_order_release);
    s_background_decode = false;
    s_cleanup_pending = false;
    s_cleanup_started_us = 0;
    s_cleanup_last_log_us = 0;
    s_health_frames_total = 0;
    s_health_decode_total_us = 0;
    s_health_dirty_pixels_total = 0;
    s_health_invalidated_pixels_total = 0;
    s_health_full_frame_updates = 0;
    s_health_deadline_resyncs = 0;
    s_health_decode_budget_overruns = 0;
    atomic_store_explicit(&s_health_decode_cooperative_blocks, 0U,
                          memory_order_release);
    atomic_store_explicit(&s_health_decode_switch_aborts, 0U,
                          memory_order_release);
    s_last_health_log_us = 0;
    s_last_health_frames = 0;
    s_last_health_decode_total_us = 0;
    s_last_health_dirty_pixels_total = 0;
    s_last_health_invalidated_pixels_total = 0;
    s_last_health_full_frame_updates = 0;
    s_last_health_deadline_resyncs = 0;
    s_last_health_decode_budget_overruns = 0;
    s_last_health_decode_cooperative_blocks = 0;
    s_last_health_decode_switch_aborts = 0;
    clear_decoder();
}

static void *allocate_decode_work(size_t bytes, bool zeroed)
{
    void *buffer = NULL;
#if CONFIG_NUWAX_UI_DISPLAY_TEST_MODE
    buffer = zeroed
                 ? heap_caps_calloc(1, bytes,
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
                 : heap_caps_malloc(bytes,
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
    if (!buffer) {
        buffer = zeroed
                     ? heap_caps_calloc(1, bytes,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                     : heap_caps_malloc(bytes,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    return buffer;
}

static void init_image_descriptor(void)
{
    s_image_dsc = (lv_image_dsc_t){
        .header.magic = LV_IMAGE_HEADER_MAGIC,
        .header.cf = LV_COLOR_FORMAT_RGB565,
        .header.flags = 0,
        .header.w = FRAME_WIDTH,
        .header.h = FRAME_HEIGHT,
        .header.stride = FRAME_WIDTH * sizeof(uint16_t),
        .data_size = FRAME_RGB_BYTES,
        .data = (const uint8_t *)s_rgb565,
    };
}

static void frame_patch_event_cb(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_COVER_CHECK) {
        /* Every displayed pixel comes from an opaque RGB565 frame.  Marking
         * the patch as a cover lets LVGL stop before traversing the 360x360
         * backing image for normal animation damage. */
        const lv_area_t *area = lv_event_get_cover_area(event);
        lv_obj_t *object = lv_event_get_current_target(event);
        lv_area_t coords;
        lv_obj_get_coords(object, &coords);
        const bool covered = area &&
            area->x1 >= coords.x1 && area->y1 >= coords.y1 &&
            area->x2 <= coords.x2 && area->y2 <= coords.y2;
        lv_event_set_cover_res(
            event, covered ? LV_COVER_RES_COVER : LV_COVER_RES_NOT_COVER);
        return;
    }
    if (code != LV_EVENT_DRAW_MAIN || !s_rgb565 || !s_image) return;

    lv_layer_t *layer = lv_event_get_layer(event);
    if (!layer) return;
    lv_area_t image_area;
    lv_obj_get_coords(s_image, &image_area);
    lv_draw_image_dsc_t draw;
    lv_draw_image_dsc_init(&draw);
    draw.base.layer = layer;
    draw.src = &s_image_dsc;
    draw.opa = LV_OPA_COVER;
    draw.scale_x = LV_SCALE_NONE;
    draw.scale_y = LV_SCALE_NONE;
    draw.image_area = image_area;
    lv_draw_image(layer, &draw, &image_area);
}

static void hide_frame_patches(void)
{
    nuwax_emotion_asset_region_t damage[FRAME_PATCH_OBJECT_COUNT];
    uint8_t damage_count = 0U;
    for (uint32_t i = 0U; i < FRAME_PATCH_OBJECT_COUNT; ++i) {
        frame_patch_object_t *patch = &s_frame_patches[i];
        if (patch->active) {
            damage[damage_count++] = patch->region;
        }
        patch->active = false;
        memset(&patch->region, 0, sizeof(patch->region));
    }
    for (uint8_t i = 0U; i < damage_count && s_frame_patch_compositor; ++i) {
        const nuwax_emotion_asset_region_t region = damage[i];
        const lv_area_t area = {
            .x1 = region.x,
            .y1 = region.y,
            .x2 = (int32_t)region.x + region.width - 1,
            .y2 = (int32_t)region.y + region.height - 1,
        };
        (void)lv_obj_invalidate_area(s_frame_patch_compositor, &area);
    }
}

static uint8_t update_frame_patch_state(
    const nuwax_emotion_asset_region_t *regions, uint8_t region_count,
    nuwax_emotion_asset_region_t *damage, uint32_t *invalidated_pixels)
{
    if (!s_image || !s_frame_patch_compositor ||
        !damage || !invalidated_pixels ||
        region_count > FRAME_PATCH_OBJECT_COUNT) return 0U;
    uint8_t damage_count = 0U;
    *invalidated_pixels = 0U;
    for (uint32_t i = 0U; i < FRAME_PATCH_OBJECT_COUNT; ++i) {
        frame_patch_object_t *patch = &s_frame_patches[i];
        const bool next_active = i < region_count && regions[i].width > 0U &&
            regions[i].height > 0U;
        if (patch->active) {
            damage[damage_count++] = patch->region;
        }
        if (next_active) {
            const nuwax_emotion_asset_region_t next = regions[i];
            damage[damage_count++] = next;
            patch->region = next;
            patch->active = true;
            *invalidated_pixels += (uint32_t)next.width * next.height;
        } else {
            patch->active = false;
            memset(&patch->region, 0, sizeof(patch->region));
        }
    }
    damage_count = nuwax_emotion_component_regions_compact(
        damage, damage_count, FRAME_PATCH_OBJECT_COUNT);
    return damage_count;
}

static void invalidate_frame_patch_damage(
    const nuwax_emotion_asset_region_t *damage, uint8_t damage_count)
{
    if (!s_frame_patch_compositor || !damage) return;
    for (uint8_t i = 0U; i < damage_count; ++i) {
        const nuwax_emotion_asset_region_t region = damage[i];
        const lv_area_t area = {
            .x1 = region.x,
            .y1 = region.y,
            .x2 = (int32_t)region.x + region.width - 1,
            .y2 = (int32_t)region.y + region.height - 1,
        };
        (void)lv_obj_invalidate_area(s_frame_patch_compositor, &area);
    }
}

static uint32_t update_frame_patches(
    const nuwax_emotion_asset_region_t *regions, uint8_t region_count)
{
    nuwax_emotion_asset_region_t damage[FRAME_PATCH_OBJECT_COUNT * 2U];
    uint32_t invalidated_pixels = 0U;
    const uint8_t damage_count = update_frame_patch_state(
        regions, region_count, damage, &invalidated_pixels);
    invalidate_frame_patch_damage(damage, damage_count);
    return invalidated_pixels;
}

static uint32_t present_frame_patches_direct(
    const nuwax_emotion_asset_region_t *regions, uint8_t region_count,
    uint32_t timeout_ms, bool *presented_directly, uint32_t *present_us,
    esp_err_t *present_result)
{
    if (presented_directly) *presented_directly = false;
    if (present_us) *present_us = 0U;
    if (present_result) *present_result = ESP_OK;

    if (!s_image || !s_frame_patch_compositor) {
        if (present_result) *present_result = ESP_ERR_INVALID_STATE;
        return 0U;
    }
    if ((region_count > 0U && !regions) ||
        region_count > FRAME_PATCH_OBJECT_COUNT) {
        if (present_result) *present_result = ESP_ERR_INVALID_ARG;
        return 0U;
    }

    nuwax_emotion_asset_region_t damage[FRAME_PATCH_OBJECT_COUNT * 2U];
    uint32_t invalidated_pixels = 0U;
    const uint8_t damage_count = update_frame_patch_state(
        regions, region_count, damage, &invalidated_pixels);
    if (damage_count == 0U) {
        if (presented_directly) *presented_directly = true;
        return invalidated_pixels;
    }

    nuwax_lvgl_port_rgb565_region_t
        direct_regions[FRAME_PATCH_OBJECT_COUNT];
    for (uint8_t index = 0U; index < damage_count; ++index) {
        direct_regions[index] = (nuwax_lvgl_port_rgb565_region_t){
            .x = (int16_t)damage[index].x,
            .y = (int16_t)damage[index].y,
            .width = (int16_t)damage[index].width,
            .height = (int16_t)damage[index].height,
        };
    }

    const int64_t started_us = esp_timer_get_time();
    const esp_err_t err = nuwax_lvgl_port_present_rgb565_regions(
        s_rgb565, FRAME_WIDTH, direct_regions, damage_count,
        timeout_ms);
    if (present_result) *present_result = err;
    if (err == ESP_OK) {
        if (presented_directly) *presented_directly = true;
        if (present_us) {
            *present_us = (uint32_t)(esp_timer_get_time() - started_us);
        }
        return invalidated_pixels;
    }

    /* Busy or failed direct presentation remains a performance event only.
     * LVGL redraws the exact same old/new damage union on this handler pass. */
    invalidate_frame_patch_damage(damage, damage_count);
    nuwax_lvgl_port_wake();
    return invalidated_pixels;
}

static bool create_frame_patches(lv_obj_t *parent)
{
    memset(s_frame_patches, 0, sizeof(s_frame_patches));
    s_frame_patch_compositor = lv_obj_create(parent);
    if (!s_frame_patch_compositor) return false;
    lv_obj_remove_style_all(s_frame_patch_compositor);
    lv_obj_set_size(s_frame_patch_compositor, FRAME_WIDTH, FRAME_HEIGHT);
    lv_obj_set_pos(s_frame_patch_compositor, 0, 0);
    lv_obj_remove_flag(s_frame_patch_compositor,
                       LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_frame_patch_compositor, frame_patch_event_cb,
                        LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(s_frame_patch_compositor, frame_patch_event_cb,
                        LV_EVENT_COVER_CHECK, NULL);
    return true;
}

static void delete_frame_patches(void)
{
    if (s_frame_patch_compositor) {
        lv_obj_delete(s_frame_patch_compositor);
        s_frame_patch_compositor = NULL;
    }
    memset(s_frame_patches, 0, sizeof(s_frame_patches));
}

static component_pose_t component_pose_for_asset(
    nuwax_emotion_asset_id_t asset_id)
{
    static const component_pose_t poses[NUWAX_EMOTION_ASSET_MAX_ENTRIES] = {
        [NUWAX_EMOTION_ASSET_DIZZINESS - 1] =
            {154, 72, 74, 74, 140, 70, 230, false},
        [NUWAX_EMOTION_ASSET_HAPPY - 1] =
            {150, 74, 64, 64, 140, 70, 230, false},
        [NUWAX_EMOTION_ASSET_EXCITED - 1] =
            {146, 72, 70, 70, 140, 70, 230, false},
        [NUWAX_EMOTION_ASSET_SLEEPING - 1] =
            {174, 74, 78, 48, 170, 90, 280, true},
        [NUWAX_EMOTION_ASSET_PROVISIONING - 1] =
            {147, 72, 62, 62, 150, 80, 250, false},
        [NUWAX_EMOTION_ASSET_DISCONNECTED - 1] =
            {151, 74, 58, 58, 150, 80, 250, false},
        [NUWAX_EMOTION_ASSET_THINKING - 1] =
            {150, 72, 58, 78, 160, 90, 280, false},
        [NUWAX_EMOTION_ASSET_THOUGHT_COMPLETE - 1] =
            {150, 72, 58, 78, 160, 90, 280, false},
        [NUWAX_EMOTION_ASSET_MEETING - 1] =
            {146, 68, 82, 82, 150, 85, 270, false},
        [NUWAX_EMOTION_ASSET_MONITORING - 1] =
            {154, 68, 70, 70, 150, 85, 270, false},
        [NUWAX_EMOTION_ASSET_TOUCH - 1] =
            {157, 72, 62, 62, 110, 45, 190, false},
        [NUWAX_EMOTION_ASSET_PLAYFUL - 1] =
            {150, 72, 62, 62, 140, 70, 230, false},
        [NUWAX_EMOTION_ASSET_LIKE - 1] =
            {151, 70, 68, 62, 140, 70, 230, false},
        [NUWAX_EMOTION_ASSET_ANGRY - 1] =
            {146, 70, 66, 76, 140, 70, 230, false},
        [NUWAX_EMOTION_ASSET_LISTENING - 1] =
            {150, 76, 78, 58, 140, 70, 230, false},
        [NUWAX_EMOTION_ASSET_QUESTION - 1] =
            {158, 72, 76, 76, 140, 70, 230, false},
    };
    if (asset_id == 0U || asset_id > NUWAX_EMOTION_ASSET_MAX_ENTRIES) {
        return poses[NUWAX_EMOTION_ASSET_LISTENING - 1U];
    }
    return poses[asset_id - 1U];
}

#define PART_BOUNDS(_x, _y, _w, _h) \
    { .x = (_x), .y = (_y), .width = (_w), .height = (_h) }

static component_bounds_t component_bounds_for_asset(
    nuwax_emotion_asset_id_t asset_id)
{
    /* Bounds come from connected non-black components in each v4 loop key
     * frame, then include a small antialiasing margin.  Keeping these separate
     * prevents an accessory animation from inflating the eye refresh region. */
    static const component_bounds_t bounds[NUWAX_EMOTION_ASSET_MAX_ENTRIES] = {
        [NUWAX_EMOTION_ASSET_DIZZINESS - 1] = {{
            PART_BOUNDS(50, 95, 106, 117), PART_BOUNDS(197, 95, 106, 117),
            PART_BOUNDS(122, 226, 98, 42),
        }, 3, 2},
        [NUWAX_EMOTION_ASSET_HAPPY - 1] = {{
            PART_BOUNDS(82, 101, 74, 91), PART_BOUNDS(210, 93, 89, 103),
            PART_BOUNDS(135, 206, 108, 80), PART_BOUNDS(20, 186, 123, 101),
        }, 4, 2},
        [NUWAX_EMOTION_ASSET_EXCITED - 1] = {{
            PART_BOUNDS(56, 91, 102, 104), PART_BOUNDS(204, 91, 102, 104),
            PART_BOUNDS(147, 196, 74, 82),
        }, 3, 2},
        [NUWAX_EMOTION_ASSET_SLEEPING - 1] = {{
            PART_BOUNDS(47, 157, 110, 57), PART_BOUNDS(195, 157, 110, 57),
            PART_BOUNDS(152, 238, 57, 51), PART_BOUNDS(198, 59, 88, 91),
        }, 4, 2},
        [NUWAX_EMOTION_ASSET_PROVISIONING - 1] = {{
            PART_BOUNDS(80, 101, 75, 90), PART_BOUNDS(205, 101, 75, 90),
            PART_BOUNDS(58, 205, 119, 95), PART_BOUNDS(220, 214, 72, 76),
        }, 4, COMPONENT_PART_NONE},
        [NUWAX_EMOTION_ASSET_DISCONNECTED - 1] = {{
            PART_BOUNDS(66, 109, 81, 83), PART_BOUNDS(216, 108, 81, 84),
            PART_BOUNDS(125, 221, 109, 50), PART_BOUNDS(136, 30, 84, 70),
        }, 4, 2},
        [NUWAX_EMOTION_ASSET_THINKING - 1] = {{
            PART_BOUNDS(60, 80, 90, 128), PART_BOUNDS(208, 80, 90, 128),
            PART_BOUNDS(146, 210, 91, 45), PART_BOUNDS(135, 20, 92, 83),
            PART_BOUNDS(98, 237, 128, 123),
        }, 5, 2},
        [NUWAX_EMOTION_ASSET_THOUGHT_COMPLETE - 1] = {{
            PART_BOUNDS(60, 80, 90, 128), PART_BOUNDS(208, 80, 90, 128),
            PART_BOUNDS(146, 210, 91, 45), PART_BOUNDS(140, 14, 80, 82),
            PART_BOUNDS(98, 237, 128, 123),
        }, 5, 2},
        [NUWAX_EMOTION_ASSET_MEETING - 1] = {{
            PART_BOUNDS(32, 78, 148, 133), PART_BOUNDS(180, 78, 148, 133),
            PART_BOUNDS(149, 214, 71, 56), PART_BOUNDS(27, 207, 122, 108),
        }, 4, 2},
        [NUWAX_EMOTION_ASSET_MONITORING - 1] = {{
            PART_BOUNDS(42, 104, 101, 105), PART_BOUNDS(177, 104, 101, 105),
            PART_BOUNDS(120, 228, 81, 55), PART_BOUNDS(287, 122, 73, 111),
        }, 4, 2},
        [NUWAX_EMOTION_ASSET_TOUCH - 1] = {{
            PART_BOUNDS(52, 116, 97, 82), PART_BOUNDS(201, 113, 97, 82),
            PART_BOUNDS(133, 212, 100, 76), PART_BOUNDS(182, 28, 113, 81),
        }, 4, 2},
        [NUWAX_EMOTION_ASSET_PLAYFUL - 1] = {{
            PART_BOUNDS(73, 105, 75, 91), PART_BOUNDS(211, 94, 84, 102),
            PART_BOUNDS(155, 226, 71, 56), PART_BOUNDS(260, 185, 90, 102),
        }, 4, 2},
        [NUWAX_EMOTION_ASSET_LIKE - 1] = {{
            PART_BOUNDS(63, 109, 95, 86), PART_BOUNDS(200, 109, 95, 86),
            PART_BOUNDS(148, 209, 67, 40), PART_BOUNDS(162, 14, 70, 113),
        }, 4, 2},
        [NUWAX_EMOTION_ASSET_ANGRY - 1] = {{
            PART_BOUNDS(58, 60, 115, 130), PART_BOUNDS(199, 61, 103, 129),
            PART_BOUNDS(125, 167, 117, 99),
        }, 3, 2},
        [NUWAX_EMOTION_ASSET_LISTENING - 1] = {{
            PART_BOUNDS(44, 101, 120, 88), PART_BOUNDS(196, 101, 120, 88),
            PART_BOUNDS(137, 228, 87, 53),
        }, 3, 2},
        [NUWAX_EMOTION_ASSET_QUESTION - 1] = {{
            PART_BOUNDS(46, 101, 110, 110), PART_BOUNDS(186, 101, 110, 110),
            PART_BOUNDS(127, 210, 81, 42), PART_BOUNDS(228, 27, 80, 113),
            PART_BOUNDS(84, 231, 101, 110),
        }, 5, 2},
    };
    if (asset_id == 0U || asset_id > NUWAX_EMOTION_ASSET_MAX_ENTRIES) {
        return bounds[NUWAX_EMOTION_ASSET_LISTENING - 1U];
    }
    return bounds[asset_id - 1U];
}

#undef PART_BOUNDS

static uint32_t smooth_q10(uint32_t value_q10)
{
    if (value_q10 >= 1024U) return 1024U;
    const uint64_t square = (uint64_t)value_q10 * value_q10;
    return (uint32_t)((square * (3072U - 2U * value_q10) +
                       (1ULL << 19U)) >> 20U);
}

static uint32_t timed_progress_q10(uint32_t elapsed_ms,
                                   uint32_t start_ms,
                                   uint32_t duration_ms)
{
    if (elapsed_ms <= start_ms) return 0U;
    if (duration_ms == 0U || elapsed_ms >= start_ms + duration_ms) {
        return 1024U;
    }
    return smooth_q10(((elapsed_ms - start_ms) * 1024U) / duration_ms);
}

static int32_t lerp_i32(int32_t from, int32_t to, uint32_t progress_q10)
{
    return from + ((to - from) * (int32_t)progress_q10 + 512) / 1024;
}

static uint32_t multiply_q10(uint32_t left, uint32_t right)
{
    return (left * right + 512U) / 1024U;
}

static uint32_t spring_q10(uint32_t progress_q10)
{
    if (progress_q10 >= 1024U) return 1024U;
    const int32_t delta = (int32_t)progress_q10 - 1024;
    const int64_t square = (int64_t)delta * delta;
    const int64_t cube = square * delta;
    int64_t value = 1024LL +
        (14LL * cube) / (10LL * 1024LL * 1024LL) +
        (4LL * square) / (10LL * 1024LL);
    if (value < 0LL) value = 0LL;
    if (value > 1280LL) value = 1280LL;
    return (uint32_t)value;
}

static uint32_t overshoot_q10(uint32_t progress_q10)
{
    if (progress_q10 >= 1024U) return 1024U;
    const int32_t delta = (int32_t)progress_q10 - 1024;
    const int64_t square = (int64_t)delta * delta;
    const int64_t cube = square * delta;
    int64_t value = 1024LL +
        (22LL * cube) / (10LL * 1024LL * 1024LL) +
        (12LL * square) / (10LL * 1024LL);
    if (value < 0LL) value = 0LL;
    if (value > 1280LL) value = 1280LL;
    return (uint32_t)value;
}

static inline uint16_t scale_rgb565_over_black(uint16_t over,
                                                uint32_t alpha_q10)
{
    if (over == 0U || alpha_q10 == 0U) return 0U;
    if (alpha_q10 >= 1024U) return over;
    const uint32_t r =
        ((((uint32_t)(over >> 11U) & 0x1fU) * alpha_q10) + 512U) >> 10U;
    const uint32_t g =
        ((((uint32_t)(over >> 5U) & 0x3fU) * alpha_q10) + 512U) >> 10U;
    const uint32_t b =
        ((((uint32_t)over & 0x1fU) * alpha_q10) + 512U) >> 10U;
    return (uint16_t)((r << 11U) | (g << 5U) | b);
}

static inline uint16_t blend_rgb565(uint16_t under, uint16_t over,
                                     uint32_t alpha_q10)
{
    if (over == 0U || alpha_q10 == 0U) return under;
    if (alpha_q10 >= 1024U || under == over) return over;
    if (under == 0U) {
        /* Transition components are normally drawn onto the cleared black
         * backdrop. Avoid three subtracts and the under-color expansion on
         * this dominant path while preserving the same rounded RGB565 value. */
        return scale_rgb565_over_black(over, alpha_q10);
    }
    const int32_t under_r = (under >> 11U) & 0x1fU;
    const int32_t under_g = (under >> 5U) & 0x3fU;
    const int32_t under_b = under & 0x1fU;
    /* Algebraically equivalent to under*(1-a)+over*a, but one multiply per
     * RGB565 channel instead of two. The positive base term preserves the
     * original rounding for both rising and falling channel values. */
    const uint32_t r = (uint32_t)((under_r * 1024 +
        ((int32_t)((over >> 11U) & 0x1fU) - under_r) *
         (int32_t)alpha_q10 + 512) >> 10U);
    const uint32_t g = (uint32_t)((under_g * 1024 +
        ((int32_t)((over >> 5U) & 0x3fU) - under_g) *
         (int32_t)alpha_q10 + 512) >> 10U);
    const uint32_t b = (uint32_t)((under_b * 1024 +
        ((int32_t)(over & 0x1fU) - under_b) *
         (int32_t)alpha_q10 + 512) >> 10U);
    return (uint16_t)((r << 11U) | (g << 5U) | b);
}

static bool render_bridge_opaque_scaled_rows(
    const uint16_t *source, nuwax_emotion_asset_region_t sample_region,
    int32_t x0, int32_t y0, int32_t width, int32_t height)
{
    if (!source || width != sample_region.width || width <= 0 || height <= 0 ||
        x0 < 0 || y0 < 0 || x0 + width > FRAME_WIDTH ||
        y0 + height > FRAME_HEIGHT ||
        (uint32_t)sample_region.x + sample_region.width > FRAME_WIDTH ||
        (uint32_t)sample_region.y + sample_region.height > FRAME_HEIGHT) {
        return false;
    }
    int32_t source_y = sample_region.y;
    int32_t source_y_error = 0;
    for (int32_t row = 0; row < height; ++row) {
        memcpy(&s_rgb565[(y0 + row) * FRAME_WIDTH + x0],
               &source[source_y * FRAME_WIDTH + sample_region.x],
               (size_t)width * sizeof(uint16_t));
        source_y_error += sample_region.height;
        while (source_y_error >= height) {
            source_y++;
            source_y_error -= height;
        }
    }
    return true;
}

static void bridge_include_area(nuwax_emotion_asset_region_t *regions,
                                uint8_t *region_count,
                                int32_t x, int32_t y,
                                int32_t width, int32_t height)
{
    if (*region_count >= BRIDGE_REGION_WORK_CAPACITY ||
        width <= 0 || height <= 0) {
        return;
    }
    const int32_t x0 = x < 0 ? 0 : x;
    const int32_t y0 = y < 0 ? 0 : y;
    const int32_t x1 = x + width > FRAME_WIDTH ? FRAME_WIDTH : x + width;
    const int32_t y1 = y + height > FRAME_HEIGHT ? FRAME_HEIGHT : y + height;
    if (x0 >= x1 || y0 >= y1) return;
    regions[(*region_count)++] = (nuwax_emotion_asset_region_t){
        .x = (uint16_t)x0,
        .y = (uint16_t)y0,
        .width = (uint16_t)(x1 - x0),
        .height = (uint16_t)(y1 - y0),
    };
}

static void render_bridge_eye(const uint16_t *source,
                              nuwax_emotion_asset_region_t sample_region,
                              nuwax_emotion_asset_region_t source_geometry,
                              nuwax_emotion_asset_region_t target_geometry,
                              uint32_t geometry_q10,
                              uint32_t openness_q10,
                              uint32_t alpha_q10,
                              int32_t bob,
                              nuwax_emotion_asset_region_t *regions,
                              uint8_t *region_count)
{
    if (!source || sample_region.width == 0U || sample_region.height == 0U ||
        openness_q10 <= 10U || alpha_q10 == 0U) {
        return;
    }
    const int32_t source_center_x =
        source_geometry.x + source_geometry.width / 2;
    const int32_t source_center_y =
        source_geometry.y + source_geometry.height / 2;
    const int32_t target_center_x =
        target_geometry.x + target_geometry.width / 2;
    const int32_t target_center_y =
        target_geometry.y + target_geometry.height / 2;
    const int32_t center_x = lerp_i32(source_center_x, target_center_x,
                                      geometry_q10);
    const int32_t center_y = lerp_i32(source_center_y, target_center_y,
                                      geometry_q10) + bob;
    const int32_t width = lerp_i32(source_geometry.width,
                                   target_geometry.width,
                                   geometry_q10);
    const int32_t full_height = lerp_i32(source_geometry.height,
                                         target_geometry.height,
                                         geometry_q10);
    const int32_t height = (full_height * (int32_t)openness_q10 + 512) /
        1024;
    const int32_t bounded_height = height < 4 ? 4 : height;
    const int32_t x0 = center_x - width / 2;
    const int32_t y0 = center_y - bounded_height / 2;
    bridge_include_area(regions, region_count, x0 - 2, y0 - 2,
                        width + 4, bounded_height + 4);
    if (alpha_q10 >= 1024U &&
        render_bridge_opaque_scaled_rows(source, sample_region, x0, y0,
                                         width, bounded_height)) {
        return;
    }
    int32_t source_y = sample_region.y;
    int32_t source_y_error = 0;
    for (int32_t y = y0; y < y0 + bounded_height; ++y) {
        if (source_y >= FRAME_HEIGHT) source_y = FRAME_HEIGHT - 1;
        int32_t source_x = sample_region.x;
        int32_t source_x_error = 0;
        for (int32_t x = x0; x < x0 + width; ++x) {
            if (source_x >= FRAME_WIDTH) source_x = FRAME_WIDTH - 1;
            if (x >= 0 && x < FRAME_WIDTH && y >= 0 && y < FRAME_HEIGHT) {
                const uint16_t sampled = source[
                    source_y * FRAME_WIDTH + source_x];
                const uint32_t pixel =
                    (uint32_t)y * FRAME_WIDTH + (uint32_t)x;
                /* Eye regions are cleared before this pass and do not overlap
                 * one another. Avoid a PSRAM read/modify/write for every pixel;
                 * the closed-eye stroke is composited afterwards. */
                s_rgb565[pixel] =
                    scale_rgb565_over_black(sampled, alpha_q10);
            }
            source_x_error += sample_region.width;
            while (source_x_error >= width) {
                source_x++;
                source_x_error -= width;
            }
        }
        source_y_error += sample_region.height;
        while (source_y_error >= bounded_height) {
            source_y++;
            source_y_error -= bounded_height;
        }
    }
}

static void render_bridge_static_part(const uint16_t *source,
                                      nuwax_emotion_asset_region_t sample_region,
                                      nuwax_emotion_asset_region_t source_geometry,
                                      nuwax_emotion_asset_region_t target_geometry,
                                      uint32_t geometry_q10,
                                      uint32_t scale_q10,
                                      uint32_t alpha_q10, bool base_is_black,
                                      int32_t bob,
                                      nuwax_emotion_asset_region_t *regions,
                                      uint8_t *region_count)
{
    if (!source || sample_region.width == 0U || sample_region.height == 0U ||
        scale_q10 == 0U) {
        return;
    }
    int32_t destination_width =
        ((uint32_t)lerp_i32(source_geometry.width, target_geometry.width,
                            geometry_q10) * scale_q10 + 512U) / 1024U;
    int32_t destination_height =
        ((uint32_t)lerp_i32(source_geometry.height, target_geometry.height,
                            geometry_q10) * scale_q10 + 512U) / 1024U;
    if (destination_width < 2) destination_width = 2;
    if (destination_height < 2) destination_height = 2;
    const int32_t source_center_x =
        source_geometry.x + source_geometry.width / 2;
    const int32_t source_center_y =
        source_geometry.y + source_geometry.height / 2;
    const int32_t target_center_x =
        target_geometry.x + target_geometry.width / 2;
    const int32_t target_center_y =
        target_geometry.y + target_geometry.height / 2;
    const int32_t center_x = lerp_i32(source_center_x, target_center_x,
                                      geometry_q10);
    const int32_t center_y = lerp_i32(source_center_y, target_center_y,
                                      geometry_q10) + bob;
    const int32_t x0 = center_x - destination_width / 2;
    const int32_t y0 = center_y - destination_height / 2;
    bridge_include_area(regions, region_count, x0, y0,
                        destination_width, destination_height);
    if (alpha_q10 >= 1024U &&
        render_bridge_opaque_scaled_rows(
            source, sample_region, x0, y0,
            destination_width, destination_height)) {
        return;
    }
    int32_t source_y = sample_region.y;
    int32_t source_y_error = 0;
    for (int32_t y = y0; y < y0 + destination_height; ++y) {
        if (source_y >= FRAME_HEIGHT) source_y = FRAME_HEIGHT - 1;
        int32_t source_x = sample_region.x;
        int32_t source_x_error = 0;
        for (int32_t x = x0; x < x0 + destination_width; ++x) {
            if (source_x >= FRAME_WIDTH) source_x = FRAME_WIDTH - 1;
            if (x >= 0 && x < FRAME_WIDTH && y >= 0 && y < FRAME_HEIGHT) {
                const uint16_t sampled = source[
                    source_y * FRAME_WIDTH + source_x];
                const uint32_t pixel =
                    (uint32_t)y * FRAME_WIDTH + (uint32_t)x;
                s_rgb565[pixel] = base_is_black
                    ? scale_rgb565_over_black(sampled, alpha_q10)
                    : blend_rgb565(s_rgb565[pixel], sampled, alpha_q10);
            }
            source_x_error += sample_region.width;
            while (source_x_error >= destination_width) {
                source_x++;
                source_x_error -= destination_width;
            }
        }
        source_y_error += sample_region.height;
        while (source_y_error >= destination_height) {
            source_y++;
            source_y_error -= destination_height;
        }
    }
}

static void render_bridge_rounded_curve(
    int32_t center_x, int32_t center_y, int32_t width, int32_t curve,
    int32_t radius, uint32_t alpha_q10,
    nuwax_emotion_asset_region_t *regions, uint8_t *region_count)
{
    if (alpha_q10 == 0U || width < 4 || radius < 1) return;
    const int32_t half = width / 2;
    const int32_t outer_radius = radius + 2;
    bridge_include_area(regions, region_count,
                        center_x - half - outer_radius,
                        center_y - outer_radius,
                        width + outer_radius * 2 + 1,
                        curve + outer_radius * 2 + 1);
    const int32_t inner_squared = radius * radius;
    const int32_t outer_squared = outer_radius * outer_radius;
    const int32_t edge_span = outer_squared - inner_squared;
    for (int32_t x = center_x - half - outer_radius;
         x <= center_x + half + outer_radius; ++x) {
        if (x < 0 || x >= FRAME_WIDTH) continue;
        int32_t step = x - (center_x - half);
        if (step < 0) step = 0;
        if (step > width) step = width;
        const int32_t path_x = center_x - half + step;
        const int32_t normalized = step * 1024 / width;
        const int32_t path_y = center_y +
            ((4 * normalized * (1024 - normalized) * curve +
              (1 << 19)) >> 20);
        for (int32_t y = path_y - outer_radius;
             y <= path_y + outer_radius; ++y) {
            if (y < 0 || y >= FRAME_HEIGHT) continue;
            const int32_t dx = x - path_x;
            const int32_t dy = y - path_y;
            const int32_t distance_squared = dx * dx + dy * dy;
            if (distance_squared > outer_squared) continue;
            uint32_t coverage_q10 = 1024U;
            if (distance_squared > inner_squared) {
                coverage_q10 = (uint32_t)(
                    (outer_squared - distance_squared) * 1024 +
                    edge_span / 2) / (uint32_t)edge_span;
            }
            const uint32_t pixel_alpha_q10 =
                multiply_q10(alpha_q10, coverage_q10);
            if (pixel_alpha_q10 == 0U) continue;
            const uint32_t pixel = (uint32_t)y * FRAME_WIDTH + (uint32_t)x;
            s_rgb565[pixel] = blend_rgb565(s_rgb565[pixel], 0xffffU,
                                           pixel_alpha_q10);
        }
    }
}

static void render_bridge_closed_eye(int32_t center_x, int32_t center_y,
                                     int32_t width, int32_t curve,
                                     uint32_t alpha_q10,
                                     nuwax_emotion_asset_region_t *regions,
                                     uint8_t *region_count)
{
    /* A 9 px radius plus the antialiased edge is 21 px high, matching the
     * listening expression's source eye stroke instead of the old thin bar. */
    render_bridge_rounded_curve(center_x, center_y, width, curve, 9,
                                alpha_q10, regions, region_count);
}

static void render_bridge_neutral_mouth(
    int32_t center_x, int32_t center_y, uint32_t alpha_q10, int32_t bob,
    nuwax_emotion_asset_region_t *regions, uint8_t *region_count)
{
    if (alpha_q10 == 0U) return;
    render_bridge_rounded_curve(center_x, center_y + bob, 56, 11, 6,
                                alpha_q10, regions, region_count);
}

static bool commit_component_bridge_target(int64_t now_us)
{
    component_bridge_t *bridge = &s_component_bridge;
    if (!bridge->active || !bridge->target_ready ||
        !bridge->target_rgb565 ||
        !bridge->target_indices) return false;
    const nuwax_emotion_asset_id_t target = bridge->target_asset;
    const uint16_t target_frame_index = bridge->target_frame_index;
    uint16_t *target_rgb565 = bridge->target_rgb565;
    uint8_t *target_indices = bridge->target_indices;
    const uint32_t frames = bridge->frames;
    const int64_t measured_us =
        bridge->last_frame_us > bridge->first_frame_us
            ? bridge->last_frame_us - bridge->first_frame_us : 0LL;
    const uint32_t intervals = frames > 0U ? frames - 1U : 0U;
    const uint32_t fps_x10 = measured_us > 0
        ? (uint32_t)(((uint64_t)intervals * 10000000ULL) /
                     (uint64_t)measured_us)
        : 0U;
    const uint32_t average_render_us = frames > 0U
        ? (uint32_t)(bridge->total_render_us / frames) : 0U;
    const uint32_t average_direct_present_us =
        bridge->direct_present_frames > 0U
            ? (uint32_t)(bridge->direct_present_total_us /
                         bridge->direct_present_frames)
            : 0U;
    if (frames > 1U && fps_x10 < COMPONENT_BRIDGE_MIN_FPS * 10U) {
        ESP_LOGW(TAG,
                 "component bridge below minimum source=%u target=%u frames=%u fps=%u.%u max_gap_ms=%u avg_render_us=%u max_render_us=%u direct=%u fallback=%u direct_avg_us=%u direct_max_us=%u target_decode_us=%u minimum_fps=%u target_fps=%u",
                 (unsigned)bridge->source_asset, (unsigned)target,
                 (unsigned)frames, (unsigned)(fps_x10 / 10U),
                 (unsigned)(fps_x10 % 10U),
                 (unsigned)(bridge->max_gap_us / 1000U),
                 (unsigned)average_render_us,
                 (unsigned)bridge->max_render_us,
                 (unsigned)bridge->direct_present_frames,
                 (unsigned)bridge->fallback_present_frames,
                 (unsigned)average_direct_present_us,
                 (unsigned)bridge->direct_present_max_us,
                 (unsigned)bridge->target_decode_us,
                 (unsigned)COMPONENT_BRIDGE_MIN_FPS,
                 (unsigned)COMPONENT_BRIDGE_FPS);
    } else {
        ESP_LOGI(TAG,
                 "component bridge perf source=%u target=%u frames=%u fps=%u.%u max_gap_ms=%u avg_render_us=%u max_render_us=%u direct=%u fallback=%u direct_avg_us=%u direct_max_us=%u target_decode_us=%u minimum_fps=%u target_fps=%u",
                 (unsigned)bridge->source_asset, (unsigned)target,
                 (unsigned)frames, (unsigned)(fps_x10 / 10U),
                 (unsigned)(fps_x10 % 10U),
                 (unsigned)(bridge->max_gap_us / 1000U),
                 (unsigned)average_render_us,
                 (unsigned)bridge->max_render_us,
                 (unsigned)bridge->direct_present_frames,
                 (unsigned)bridge->fallback_present_frames,
                 (unsigned)average_direct_present_us,
                 (unsigned)bridge->direct_present_max_us,
                 (unsigned)bridge->target_decode_us,
                 (unsigned)COMPONENT_BRIDGE_MIN_FPS,
                 (unsigned)COMPONENT_BRIDGE_FPS);
    }
    bridge->target_rgb565 = NULL;
    bridge->target_indices = NULL;
    cancel_component_bridge();
    if (load_asset(target) != ESP_OK) {
        s_decode_worker_error = ESP_FAIL;
        return false;
    }
    const uint16_t loop_segment = s_entry->loop_segment !=
        NUWAX_EMOTION_ASSET_INVALID_SEGMENT
            ? s_entry->loop_segment : s_entry->completion_segment;
    if (loop_segment == NUWAX_EMOTION_ASSET_INVALID_SEGMENT ||
        start_segment(loop_segment) != ESP_OK) {
        s_decode_worker_error = ESP_FAIL;
        return false;
    }
    memcpy(s_rgb565, target_rgb565, FRAME_RGB_BYTES);
    memcpy(s_indices, target_indices, FRAME_PIXELS);
    atomic_store_explicit(&s_bridge_decode_state, BRIDGE_DECODE_IDLE,
                          memory_order_release);
    atomic_store_explicit(&s_bridge_source_ready, false,
                          memory_order_release);
    s_decoder.frame_index = (uint16_t)(target_frame_index + 1U);
    const component_bounds_t target_bounds =
        component_bounds_for_asset(target);
    bool presented_directly = false;
    uint32_t present_us = 0U;
    esp_err_t present_result = ESP_OK;
    const uint32_t invalidated_pixels = present_frame_patches_direct(
        target_bounds.parts, target_bounds.part_count,
        COMPONENT_BRIDGE_FINAL_PRESENT_TIMEOUT_MS,
        &presented_directly, &present_us, &present_result);
    if (invalidated_pixels == 0U && target_bounds.part_count > 0U) {
        /* A valid target must damage at least one component. Force a bounded
         * LVGL redraw if compositor state was unavailable or inconsistent. */
        if (s_frame_patch_compositor) {
            lv_obj_invalidate(s_frame_patch_compositor);
        } else if (s_image) {
            lv_obj_invalidate(s_image);
        }
        nuwax_lvgl_port_wake();
        ESP_LOGE(TAG,
                 "component bridge target commit had no damage target=%u regions=%u present_err=%s; forced redraw",
                 (unsigned)target,
                 (unsigned)target_bounds.part_count,
                 esp_err_to_name(present_result));
    } else if (!presented_directly) {
        ESP_LOGW(TAG,
                 "component bridge target commit queued through LVGL target=%u pixels=%lu present_err=%s",
                 (unsigned)target,
                 (unsigned long)invalidated_pixels,
                 esp_err_to_name(present_result));
    } else {
        ESP_LOGI(TAG,
                 "component bridge target committed target=%u pixels=%lu direct_us=%lu",
                 (unsigned)target,
                 (unsigned long)invalidated_pixels,
                 (unsigned long)present_us);
    }
    s_next_frame_us = now_us + COMPONENT_BRIDGE_PERIOD_US;
    if (s_background_decode) decode_worker_request();
    return true;
}

static void clear_bridge_damage(void)
{
    for (uint32_t i = 0U; i < FRAME_PATCH_OBJECT_COUNT; ++i) {
        const frame_patch_object_t *patch = &s_frame_patches[i];
        if (!patch->active) continue;
        for (uint32_t y = patch->region.y;
             y < patch->region.y + patch->region.height; ++y) {
            memset(&s_rgb565[y * FRAME_WIDTH + patch->region.x], 0,
                   patch->region.width * sizeof(uint16_t));
        }
    }
}

static void clear_component_bounds(component_bounds_t bounds)
{
    for (uint8_t part = 0U; part < bounds.part_count; ++part) {
        const nuwax_emotion_asset_region_t region = bounds.parts[part];
        for (uint32_t y = region.y; y < region.y + region.height; ++y) {
            memset(&s_rgb565[y * FRAME_WIDTH + region.x], 0,
                   region.width * sizeof(uint16_t));
        }
    }
}

static void cancel_component_bridge(void)
{
    memset(&s_component_bridge, 0, sizeof(s_component_bridge));
}

static bool request_component_bridge_decode(component_bridge_t *bridge)
{
    if (!bridge || !s_background_decode || !s_decode_task ||
        !bridge->source_rgb565 || !bridge->target_rgb565 ||
        !bridge->target_indices || !s_rgb565) {
        return false;
    }

    int worker_state = atomic_load_explicit(&s_decode_worker_state,
                                            memory_order_acquire);
    if (worker_state == DECODE_WORKER_READY) {
        int expected = DECODE_WORKER_READY;
        if (!atomic_compare_exchange_strong_explicit(
                &s_decode_worker_state, &expected, DECODE_WORKER_IDLE,
                memory_order_acq_rel, memory_order_acquire)) {
            return false;
        }
        worker_state = DECODE_WORKER_IDLE;
    }
    if (worker_state != DECODE_WORKER_IDLE) return false;

    const uint32_t generation = atomic_fetch_add_explicit(
        &s_bridge_decode_generation, 1U, memory_order_acq_rel) + 1U;
    s_bridge_decode_job = (bridge_decode_job_t){
        .asset_id = bridge->target_asset,
        .segment_index = bridge->target_segment,
        .frame_index = bridge->target_frame_index,
        .generation = generation,
        .visible_rgb565 = s_rgb565,
        .source_rgb565 = bridge->source_rgb565,
        .target_rgb565 = bridge->target_rgb565,
        .target_indices = bridge->target_indices,
        .result = ESP_OK,
    };
    bridge->decode_generation = generation;
    atomic_store_explicit(&s_bridge_source_ready, false,
                          memory_order_release);
    atomic_store_explicit(&s_bridge_decode_state, BRIDGE_DECODE_REQUESTED,
                          memory_order_release);

    int expected = DECODE_WORKER_IDLE;
    if (!atomic_compare_exchange_strong_explicit(
            &s_decode_worker_state, &expected, DECODE_WORKER_REQUESTED,
            memory_order_acq_rel, memory_order_acquire)) {
        atomic_store_explicit(&s_bridge_decode_state, BRIDGE_DECODE_IDLE,
                              memory_order_release);
        return false;
    }
    xTaskNotifyGive(s_decode_task);
    return true;
}

static void restore_component_bridge_source(int64_t now_us)
{
    component_bridge_t *bridge = &s_component_bridge;
    if (!bridge->active || !bridge->source_rgb565 || !s_rgb565) return;
    const bool snapshot_ready = atomic_load_explicit(
        &s_bridge_source_ready, memory_order_acquire);
    if (snapshot_ready) {
        memcpy(s_rgb565, bridge->source_rgb565, FRAME_RGB_BYTES);
    }
    cancel_component_bridge();
    if (snapshot_ready) {
        hide_frame_patches();
        if (s_image) lv_obj_invalidate(s_image);
    }
    s_next_frame_us = now_us + COMPONENT_BRIDGE_PERIOD_US;
}

static bool ensure_component_bridge_work(void)
{
    if (s_bridge_source_work && s_bridge_target_work &&
        s_bridge_target_indices_work) {
        return true;
    }
    heap_caps_free(s_bridge_source_work);
    heap_caps_free(s_bridge_target_work);
    heap_caps_free(s_bridge_target_indices_work);
    s_bridge_source_work = NULL;
    s_bridge_target_work = NULL;
    s_bridge_target_indices_work = NULL;

    const size_t work_bytes = FRAME_RGB_BYTES * 2U + FRAME_PIXELS;
    const size_t psram_free = heap_caps_get_free_size(
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t psram_largest = heap_caps_get_largest_free_block(
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (psram_free < work_bytes + COMPONENT_BRIDGE_PSRAM_RESERVE_BYTES ||
        psram_largest < FRAME_RGB_BYTES) {
        return false;
    }
    s_bridge_source_work = heap_caps_malloc(
        FRAME_RGB_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_bridge_target_work = heap_caps_malloc(
        FRAME_RGB_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_bridge_target_indices_work = heap_caps_malloc(
        FRAME_PIXELS, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_bridge_source_work || !s_bridge_target_work ||
        !s_bridge_target_indices_work) {
        heap_caps_free(s_bridge_source_work);
        heap_caps_free(s_bridge_target_work);
        heap_caps_free(s_bridge_target_indices_work);
        s_bridge_source_work = NULL;
        s_bridge_target_work = NULL;
        s_bridge_target_indices_work = NULL;
        return false;
    }
    ESP_LOGI(TAG, "component bridge workspace reserved bytes=%u",
             (unsigned)work_bytes);
    return true;
}

static bool begin_component_bridge(nuwax_emotion_asset_id_t target_asset)
{
    if (nuwax_emotion_asset_pack_abi() != NUWAX_EMOTION_ASSET_PACK_ABI_V4 ||
        !s_rgb565 || !s_image || s_asset_id == 0U ||
        !nuwax_emotion_component_bridge_allowed(s_asset_id, target_asset)) {
        return false;
    }
    cancel_component_bridge();
    if (!ensure_component_bridge_work()) {
        ESP_LOGW(TAG, "component bridge workspace unavailable; direct switch");
        return false;
    }
    const nuwax_emotion_asset_entry_t *target_entry =
        nuwax_emotion_asset_pack_find(target_asset);
    uint16_t target_segment = target_entry
        ? target_entry->loop_segment : NUWAX_EMOTION_ASSET_INVALID_SEGMENT;
    if (target_segment == NUWAX_EMOTION_ASSET_INVALID_SEGMENT && target_entry) {
        target_segment = target_entry->completion_segment;
    }
    nuwax_emotion_asset_v4_frame_t target_frame;
    const uint16_t target_frame_index =
        nuwax_emotion_component_handoff_frame(target_asset);
    const nuwax_emotion_asset_segment_t *segment =
        nuwax_emotion_asset_pack_segment(target_segment);
    if (target_segment == NUWAX_EMOTION_ASSET_INVALID_SEGMENT ||
        !segment || target_frame_index >= segment->frame_count ||
        nuwax_emotion_asset_pack_v4_frame(target_segment, 0U,
                                           &target_frame) != ESP_OK ||
        !target_frame.absolute) {
        ESP_LOGW(TAG,
                 "component bridge target key unavailable asset=%u; direct switch",
                 (unsigned)target_asset);
        return false;
    }
    const component_pose_t source_pose = component_pose_for_asset(s_asset_id);
    const component_pose_t target_pose = component_pose_for_asset(target_asset);
    const nuwax_emotion_component_bridge_timing_t timing =
        nuwax_emotion_component_bridge_timing(
            s_asset_id, target_asset, source_pose.close_ms,
            target_pose.reshape_ms, target_pose.open_ms);
    s_component_bridge = (component_bridge_t){
        .active = true,
        .source_asset = s_asset_id,
        .target_asset = target_asset,
        .source_pose = source_pose,
        .target_pose = target_pose,
        .close_ms = timing.close_ms,
        .reshape_ms = timing.reshape_ms,
        .open_ms = timing.open_ms,
        .source_rgb565 = s_bridge_source_work,
        .target_rgb565 = s_bridge_target_work,
        .target_indices = s_bridge_target_indices_work,
        .target_segment = target_segment,
        .target_frame_index = target_frame_index,
        .started_us = esp_timer_get_time(),
        .next_frame_us = esp_timer_get_time(),
    };
    if (!request_component_bridge_decode(&s_component_bridge)) {
        ESP_LOGW(TAG,
                 "component bridge async prepare unavailable asset=%u; direct switch",
                 (unsigned)target_asset);
        cancel_component_bridge();
        return false;
    }
    return true;
}

static bool tick_component_bridge(int64_t now_us)
{
    if (!s_component_bridge.active || !s_component_bridge.source_rgb565) {
        return false;
    }
    component_bridge_t *bridge = &s_component_bridge;
    if (bridge->decode_generation != s_bridge_decode_job.generation) {
        ESP_LOGE(TAG, "component bridge decode generation mismatch");
        s_decode_worker_error = ESP_ERR_INVALID_STATE;
        restore_component_bridge_source(now_us);
        return false;
    }
    const bridge_decode_state_t decode_state = (bridge_decode_state_t)
        atomic_load_explicit(&s_bridge_decode_state, memory_order_acquire);
    if (!bridge->target_ready && decode_state == BRIDGE_DECODE_READY) {
        bridge->target_ready = true;
        bridge->target_decode_us = s_bridge_decode_job.decode_us;
    } else if (!bridge->target_ready &&
               decode_state == BRIDGE_DECODE_FAILED) {
        const esp_err_t err = s_bridge_decode_job.result;
        ESP_LOGE(TAG,
                 "component bridge target decode failed asset=%u err=%s",
                 (unsigned)bridge->target_asset, esp_err_to_name(err));
        s_decode_worker_error = err != ESP_OK ? err : ESP_FAIL;
        restore_component_bridge_source(now_us);
        return false;
    }

    if (!bridge->timeline_started) {
        const bool source_ready = atomic_load_explicit(
            &s_bridge_source_ready, memory_order_acquire);
        if (!nuwax_emotion_component_bridge_ready_to_animate(
                source_ready, bridge->target_ready)) {
            return true;
        }
        /* Decode and palette expansion contend with LVGL for CPU0 and PSRAM.
         * Start the visual clock only when both immutable frames are ready so
         * the close/open motion never stalls halfway through a blink. */
        bridge->timeline_started = true;
        bridge->started_us = now_us;
        bridge->next_frame_us = now_us;
        bridge->visual_elapsed_us = 0U;
    }

    if (now_us < bridge->next_frame_us) return true;
    const uint32_t close_ms = bridge->close_ms;
    const uint32_t reshape_ms = bridge->reshape_ms;
    const uint32_t open_ms = bridge->open_ms;
    const uint32_t open_start_ms = close_ms + reshape_ms;
    const uint32_t duration_ms = open_start_ms + open_ms;
    const uint32_t duration_us = duration_ms * 1000U;
    const uint32_t elapsed_ms = bridge->visual_elapsed_us / 1000U;

    if (elapsed_ms >= duration_ms) {
        if (bridge->target_ready) {
            (void)commit_component_bridge_target(now_us);
        }
        return true;
    }

    const int64_t render_started_us = esp_timer_get_time();
    if (bridge->first_frame_us == 0) {
        bridge->first_frame_us = render_started_us;
    } else if (render_started_us > bridge->last_frame_us) {
        const uint32_t gap_us =
            (uint32_t)(render_started_us - bridge->last_frame_us);
        if (gap_us > bridge->max_gap_us) bridge->max_gap_us = gap_us;
    }
    bridge->last_frame_us = render_started_us;
    const component_bounds_t source_bounds =
        component_bounds_for_asset(bridge->source_asset);
    const component_bounds_t target_bounds =
        component_bounds_for_asset(bridge->target_asset);
    nuwax_emotion_asset_region_t regions[BRIDGE_REGION_WORK_CAPACITY] = {0};
    uint8_t region_count = 0U;
    const bool first_frame = bridge->frames == 0U;
    if (first_frame) {
        clear_component_bounds(source_bounds);
        for (uint8_t part = 0U; part < source_bounds.part_count; ++part) {
            const nuwax_emotion_asset_region_t region =
                source_bounds.parts[part];
            bridge_include_area(regions, &region_count,
                                region.x, region.y,
                                region.width, region.height);
        }
    } else if (bridge->target_ready && !bridge->source_cleared) {
        /* The component unions cover every animated source pixel. Clear and
         * invalidate those unions while both eyes are closed; the stable black
         * background and circular edge never need a full-screen transfer. */
        clear_bridge_damage();
        clear_component_bounds(source_bounds);
        for (uint8_t part = 0U; part < source_bounds.part_count; ++part) {
            const nuwax_emotion_asset_region_t region =
                source_bounds.parts[part];
            bridge_include_area(regions, &region_count,
                                region.x, region.y,
                                region.width, region.height);
        }
        bridge->source_cleared = true;
    } else {
        clear_bridge_damage();
    }
    const uint32_t close_q10 = timed_progress_q10(elapsed_ms, 0U, close_ms);
    const uint32_t geometry_q10 = timed_progress_q10(
        elapsed_ms, close_ms, reshape_ms);
    const uint32_t open_q10 = timed_progress_q10(
        elapsed_ms, open_start_ms, open_ms);
    const uint32_t target_reveal_q10 = timed_progress_q10(
        elapsed_ms,
        close_ms + (reshape_ms * 38U) / 100U,
        (reshape_ms * 62U) / 100U + (open_ms * 28U) / 100U);
    const uint32_t target_alpha_q10 = target_reveal_q10;
    const uint32_t accessory_source_exit_q10 = timed_progress_q10(
        elapsed_ms, (close_ms * 8U) / 100U, (close_ms * 76U) / 100U);
    const uint32_t accessory_target_q10 = timed_progress_q10(
        elapsed_ms, open_start_ms + (open_ms * 18U) / 100U,
        (open_ms * 72U) / 100U);
    const uint32_t mouth_target_q10 = multiply_q10(target_reveal_q10,
                                                   open_q10);
    const int32_t anticipation = (int32_t)(
        (uint64_t)close_q10 * (1024U - close_q10) * 7U >> 20U);
    const int32_t settle = (int32_t)(
        (uint64_t)open_q10 * (1024U - open_q10) * 10U >> 20U);
    const int32_t bob = anticipation - settle;
    for (uint8_t eye = 0U; eye < 2U; ++eye) {
        const uint32_t side_offset_ms = eye == 0U ? 0U :
            (close_ms * 14U / 100U < 18U ? close_ms * 14U / 100U : 18U);
        const uint32_t source_open_q10 = 1024U - timed_progress_q10(
            elapsed_ms, side_offset_ms,
            close_ms > side_offset_ms ? close_ms - side_offset_ms : 1U);
        const uint32_t target_open_q10 = overshoot_q10(timed_progress_q10(
            elapsed_ms, open_start_ms + side_offset_ms,
            open_ms > side_offset_ms ? open_ms - side_offset_ms : 1U));
        if (elapsed_ms < close_ms) {
            render_bridge_eye(bridge->source_rgb565,
                              source_bounds.parts[eye],
                              source_bounds.parts[eye],
                              source_bounds.parts[eye], 0U,
                              source_open_q10, 1024U, bob,
                              regions, &region_count);
        } else if (bridge->target_ready && elapsed_ms >= open_start_ms) {
            render_bridge_eye(bridge->target_rgb565,
                              target_bounds.parts[eye],
                              target_bounds.parts[eye],
                              target_bounds.parts[eye], 1024U,
                              target_open_q10, target_alpha_q10, bob,
                              regions, &region_count);
        }
        const uint32_t source_closed_q10 = source_open_q10 >= 1024U
            ? 0U : 1024U - source_open_q10;
        const uint32_t target_closed_q10 = target_open_q10 >= 1024U
            ? 0U : 1024U - target_open_q10;
        const uint32_t side_closed_q10 =
            (source_closed_q10 < target_closed_q10
                 ? source_closed_q10 : target_closed_q10) * 118U / 100U;
        const uint32_t global_closed_base = close_q10 < 1024U - open_q10
            ? close_q10 : 1024U - open_q10;
        const uint32_t global_closed_q10 = global_closed_base * 118U / 100U;
        uint32_t closed_alpha_q10 = side_closed_q10 > global_closed_q10
            ? side_closed_q10 : global_closed_q10;
        if (closed_alpha_q10 > 1024U) closed_alpha_q10 = 1024U;
        const int32_t eye_center_x = lerp_i32(
            source_bounds.parts[eye].x + source_bounds.parts[eye].width / 2,
            target_bounds.parts[eye].x + target_bounds.parts[eye].width / 2,
            geometry_q10);
        const int32_t eye_center_y = lerp_i32(
            source_bounds.parts[eye].y + source_bounds.parts[eye].height / 2,
            target_bounds.parts[eye].y + target_bounds.parts[eye].height / 2,
            geometry_q10) + bob;
        const int32_t eye_width = lerp_i32(
            source_bounds.parts[eye].width,
            target_bounds.parts[eye].width, geometry_q10);
        render_bridge_closed_eye(
            eye_center_x, eye_center_y, (eye_width * 64 + 50) / 100,
            lerp_i32(2, bridge->target_pose.sleep_curve ? 18 : 5,
                     geometry_q10),
            closed_alpha_q10,
            regions, &region_count);
    }

    if (elapsed_ms < close_ms &&
        source_bounds.mouth_index != COMPONENT_PART_NONE) {
        const uint8_t source_mouth = source_bounds.mouth_index;
        render_bridge_static_part(
            bridge->source_rgb565, source_bounds.parts[source_mouth],
            source_bounds.parts[source_mouth],
            source_bounds.parts[source_mouth], 0U,
            1024U - close_q10 / 10U, 1024U - close_q10,
            true, bob, regions, &region_count);
    }
    if (bridge->target_ready && elapsed_ms >= open_start_ms &&
        target_bounds.mouth_index != COMPONENT_PART_NONE) {
        const uint8_t target_mouth = target_bounds.mouth_index;
        render_bridge_static_part(
            bridge->target_rgb565, target_bounds.parts[target_mouth],
            target_bounds.parts[target_mouth],
            target_bounds.parts[target_mouth], 1024U,
            spring_q10(open_q10), mouth_target_q10,
            true, bob, regions, &region_count);
    }

    const uint32_t neutral_window_q10 = timed_progress_q10(
        elapsed_ms, (close_ms * 48U) / 100U,
        close_ms - (close_ms * 48U) / 100U);
    const uint32_t neutral_exit_q10 = 1024U - timed_progress_q10(
        elapsed_ms, open_start_ms, (open_ms * 52U) / 100U);
    const uint32_t neutral_alpha_q10 =
        multiply_q10(neutral_window_q10, neutral_exit_q10) * 48U / 100U;
    if (neutral_alpha_q10 > 0U) {
        int32_t source_mouth_x = FRAME_WIDTH / 2;
        int32_t source_mouth_y = 238;
        int32_t target_mouth_x = source_mouth_x;
        int32_t target_mouth_y = source_mouth_y;
        if (source_bounds.mouth_index != COMPONENT_PART_NONE) {
            const nuwax_emotion_asset_region_t mouth =
                source_bounds.parts[source_bounds.mouth_index];
            source_mouth_x = mouth.x + mouth.width / 2;
            source_mouth_y = mouth.y + mouth.height / 2;
        }
        if (target_bounds.mouth_index != COMPONENT_PART_NONE) {
            const nuwax_emotion_asset_region_t mouth =
                target_bounds.parts[target_bounds.mouth_index];
            target_mouth_x = mouth.x + mouth.width / 2;
            target_mouth_y = mouth.y + mouth.height / 2;
        }
        render_bridge_neutral_mouth(
            lerp_i32(source_mouth_x, target_mouth_x, geometry_q10),
            lerp_i32(source_mouth_y, target_mouth_y, geometry_q10),
            neutral_alpha_q10, bob, regions, &region_count);
    }

    for (uint8_t part = 2U;
         elapsed_ms < close_ms && part < source_bounds.part_count; ++part) {
        if (part == source_bounds.mouth_index) continue;
        render_bridge_static_part(
            bridge->source_rgb565, source_bounds.parts[part],
            source_bounds.parts[part], source_bounds.parts[part], 0U,
            1024U - (accessory_source_exit_q10 * 16U) / 100U,
            1024U - accessory_source_exit_q10,
            false, bob, regions, &region_count);
    }
    for (uint8_t part = 2U;
         bridge->target_ready && elapsed_ms >= open_start_ms &&
         part < target_bounds.part_count; ++part) {
        if (part == target_bounds.mouth_index) continue;
        render_bridge_static_part(
            bridge->target_rgb565, target_bounds.parts[part],
            target_bounds.parts[part], target_bounds.parts[part], 1024U,
            spring_q10(accessory_target_q10),
            accessory_target_q10, false, bob, regions, &region_count);
    }
    region_count = nuwax_emotion_component_regions_compact(
        regions, region_count, FRAME_PATCH_OBJECT_COUNT);
    bool presented_directly = false;
    uint32_t present_us = 0U;
    (void)present_frame_patches_direct(regions, region_count,
        COMPONENT_BRIDGE_DIRECT_PRESENT_TIMEOUT_MS,
        &presented_directly, &present_us, NULL);
    if (presented_directly) {
        bridge->direct_present_frames++;
        bridge->direct_present_total_us += present_us;
        if (present_us > bridge->direct_present_max_us) {
            bridge->direct_present_max_us = present_us;
        }
    } else {
        bridge->fallback_present_frames++;
    }
    const uint32_t render_us =
        (uint32_t)(esp_timer_get_time() - render_started_us);
    bridge->total_render_us += render_us;
    if (render_us > bridge->max_render_us) {
        bridge->max_render_us = render_us;
    }
    bridge->frames++;
    bridge->visual_elapsed_us =
        nuwax_emotion_component_bridge_next_elapsed_us(
            bridge->visual_elapsed_us, duration_us,
            COMPONENT_BRIDGE_PERIOD_US);
    /* Do not catch up by skipping visual poses after a long render. A late
     * frame extends the transition but advances exactly one 40 FPS pose, which
     * removes the visible position/scale jump reported on device. */
    bridge->next_frame_us = render_started_us + COMPONENT_BRIDGE_PERIOD_US;
    return true;
}

static esp_err_t refill_input(void)
{
    if (!s_decoder.segment ||
        s_decoder.compressed_loaded >= s_decoder.segment->compressed_size) {
        s_decoder.input_pos = 0;
        s_decoder.input_size = 0;
        return ESP_OK;
    }

    const uint32_t remaining = s_decoder.segment->compressed_size -
                               s_decoder.compressed_loaded;
    const uint32_t bytes = remaining < INFLATE_INPUT_BYTES
                               ? remaining
                               : INFLATE_INPUT_BYTES;
    esp_err_t err = nuwax_emotion_asset_pack_read(
        s_decoder.segment->offset + s_decoder.compressed_loaded,
        s_input, bytes);
    if (err != ESP_OK) {
        return err;
    }
    s_decoder.compressed_loaded += bytes;
    s_decoder.input_pos = 0;
    s_decoder.input_size = bytes;
    return ESP_OK;
}

static esp_err_t inflate_pending_output(void)
{
    if (!s_decoder.segment) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_decoder.pending_size != 0U) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_decoder.input_pos == s_decoder.input_size) {
        ESP_RETURN_ON_ERROR(refill_input(), TAG, "emotion input read failed");
    }

    const size_t dictionary_remaining = TINFL_LZ_DICT_SIZE -
                                        s_decoder.dictionary_pos;
    size_t out_bytes = dictionary_remaining;
    size_t in_bytes = s_decoder.input_size - s_decoder.input_pos;
    const bool more_input_after_buffer =
        s_decoder.compressed_loaded < s_decoder.segment->compressed_size;
    const mz_uint32 flags = TINFL_FLAG_PARSE_ZLIB_HEADER |
                            TINFL_FLAG_COMPUTE_ADLER32 |
                            (more_input_after_buffer
                                 ? TINFL_FLAG_HAS_MORE_INPUT
                                 : 0U);
    const uint32_t dictionary_start = s_decoder.dictionary_pos;
    const tinfl_status status = tinfl_decompress(
        s_inflater,
        s_input + s_decoder.input_pos,
        &in_bytes,
        s_dictionary,
        s_dictionary + dictionary_start,
        &out_bytes,
        flags);
    s_decoder.input_pos += (uint32_t)in_bytes;
    s_decoder.dictionary_pos =
        (s_decoder.dictionary_pos + (uint32_t)out_bytes) % TINFL_LZ_DICT_SIZE;
    s_decoder.pending_pos = dictionary_start;
    s_decoder.pending_size = (uint32_t)out_bytes;
    s_decoder.status = status;

    if (status < TINFL_STATUS_DONE) {
        ESP_LOGE(TAG,
                 "inflate failed asset=%u segment=%u status=%d compressed=%lu/%lu raw=%lu",
                 (unsigned)s_asset_id,
                 (unsigned)s_decoder.segment_index,
                 (int)status,
                 (unsigned long)s_decoder.compressed_loaded,
                 (unsigned long)s_decoder.segment->compressed_size,
                 (unsigned long)s_decoder.raw_bytes);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (out_bytes == 0 && status != TINFL_STATUS_DONE &&
        s_decoder.input_pos == s_decoder.input_size &&
        s_decoder.compressed_loaded >= s_decoder.segment->compressed_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t decoder_read_raw(void *destination, uint32_t bytes)
{
    uint8_t *output = (uint8_t *)destination;
    while (bytes > 0U) {
        if (s_decoder.pending_size == 0U) {
            if (s_decoder.status == TINFL_STATUS_DONE) {
                return ESP_ERR_INVALID_SIZE;
            }
            ESP_RETURN_ON_ERROR(inflate_pending_output(), TAG,
                                "emotion raw stream inflate failed");
            if (s_decoder.pending_size == 0U) {
                continue;
            }
        }
        const uint32_t take = s_decoder.pending_size < bytes
                                  ? s_decoder.pending_size
                                  : bytes;
        const uint8_t *source = s_dictionary + s_decoder.pending_pos;
        if (s_decoder.raw_bytes > s_decoder.segment->raw_size ||
            take > s_decoder.segment->raw_size - s_decoder.raw_bytes) {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(output, source, take);
        s_decoder.raw_crc32 = esp_rom_crc32_le(s_decoder.raw_crc32,
                                                source, take);
        s_decoder.raw_bytes += take;
        s_decoder.pending_pos += take;
        s_decoder.pending_size -= take;
        if (s_decoder.pending_size == 0U) {
            s_decoder.pending_pos = 0U;
        }
        output += take;
        bytes -= take;
    }
    return ESP_OK;
}

static esp_err_t start_segment(uint16_t segment_index)
{
    const nuwax_emotion_asset_segment_t *segment =
        nuwax_emotion_asset_pack_segment(segment_index);
    const uint16_t abi = nuwax_emotion_asset_pack_abi();
    const uint16_t frame_count = segment ? segment->frame_count : 0U;
    const uint32_t raw_size = segment ? segment->raw_size : 0U;
    const nuwax_emotion_segment_size_requirement_t requirement =
        nuwax_emotion_segment_size_requirement(
            abi, frame_count, FRAME_PIXELS, sizeof(v2_frame_header_t));
    const bool raw_size_valid =
        segment && nuwax_emotion_segment_raw_size_valid(
                       abi, frame_count, raw_size, FRAME_PIXELS,
                       sizeof(v2_frame_header_t));
    if (!segment || segment->frame_count == 0 || !raw_size_valid) {
        ESP_LOGE(TAG,
                 "segment contract rejected abi=v%u segment=%u frames=%u raw=%lu requirement=%s%llu",
                 (unsigned)abi, (unsigned)segment_index,
                 (unsigned)frame_count, (unsigned long)raw_size,
                 !requirement.supported
                     ? "unsupported:"
                     : (requirement.exact ? "exact:" : "minimum:"),
                 (unsigned long long)requirement.bytes);
        return ESP_ERR_INVALID_SIZE;
    }
    clear_decoder();
    s_decoder.segment = segment;
    s_decoder.segment_index = segment_index;
    if (abi == NUWAX_EMOTION_ASSET_PACK_ABI_V3) {
        const nuwax_emotion_asset_v3_segment_t *v3 =
            nuwax_emotion_asset_pack_v3_segment(segment_index);
        if (!v3 || v3->key_size == 0U) {
            return ESP_ERR_INVALID_SIZE;
        }
        s_decoder.stream_segment = *segment;
        s_decoder.stream_segment.offset = v3->tail_offset;
        s_decoder.stream_segment.compressed_size = v3->tail_size;
        s_decoder.stream_segment.raw_size = v3->tail_raw_size;
        s_decoder.stream_segment.raw_crc32 = v3->tail_raw_crc32;
        s_decoder.segment = &s_decoder.stream_segment;
        s_decoder.frame_index = 0U;
        s_v3_bootstrap = V3_BOOTSTRAP_KEY;
    } else if (abi == NUWAX_EMOTION_ASSET_PACK_ABI_V4) {
        s_decoder.frame_index = 0U;
        s_decoder.status = TINFL_STATUS_DONE;
    }
    if (s_inflater) tinfl_init(s_inflater);
    if (s_dictionary) memset(s_dictionary, 0, TINFL_LZ_DICT_SIZE);
    return ESP_OK;
}

static esp_err_t finish_segment(void)
{
    const uint16_t abi = nuwax_emotion_asset_pack_abi();
    if (abi == NUWAX_EMOTION_ASSET_PACK_ABI_V4) {
        return s_decoder.segment &&
                       s_decoder.frame_index == s_decoder.segment->frame_count
                   ? ESP_OK : ESP_ERR_INVALID_SIZE;
    }
    if (abi == NUWAX_EMOTION_ASSET_PACK_ABI_V3 &&
        s_decoder.segment && s_decoder.segment->compressed_size == 0U) {
        return s_decoder.frame_index == s_decoder.segment->frame_count
                   ? ESP_OK
                   : ESP_ERR_INVALID_SIZE;
    }
    if (s_decoder.pending_size != 0U) {
        ESP_LOGE(TAG,
                 "segment has pending bytes asset=%u segment=%u bytes=%lu",
                 (unsigned)s_asset_id,
                 (unsigned)s_decoder.segment_index,
                 (unsigned long)s_decoder.pending_size);
        return ESP_ERR_INVALID_SIZE;
    }
    for (uint32_t attempt = 0;
         s_decoder.status != TINFL_STATUS_DONE &&
         attempt < DECODE_PROGRESS_LIMIT;
         ++attempt) {
        ESP_RETURN_ON_ERROR(inflate_pending_output(),
                            TAG, "emotion stream finalization failed");
        if (s_decoder.pending_size != 0U) {
            ESP_LOGE(TAG,
                     "segment produced extra bytes asset=%u segment=%u bytes=%lu",
                     (unsigned)s_asset_id,
                     (unsigned)s_decoder.segment_index,
                     (unsigned long)s_decoder.pending_size);
            return ESP_ERR_INVALID_SIZE;
        }
    }
    if (s_decoder.status != TINFL_STATUS_DONE ||
        s_decoder.compressed_loaded != s_decoder.segment->compressed_size ||
        s_decoder.input_pos != s_decoder.input_size ||
        s_decoder.frame_index != s_decoder.segment->frame_count ||
        s_decoder.raw_bytes != s_decoder.segment->raw_size ||
        s_decoder.raw_crc32 != s_decoder.segment->raw_crc32) {
        ESP_LOGE(TAG,
                 "segment validation failed asset=%u segment=%u status=%d compressed=%lu/%lu input=%lu/%lu frames=%u/%u raw=%lu/%lu crc=%08lx/%08lx",
                 (unsigned)s_asset_id,
                 (unsigned)s_decoder.segment_index,
                 (int)s_decoder.status,
                 (unsigned long)s_decoder.compressed_loaded,
                 (unsigned long)s_decoder.segment->compressed_size,
                 (unsigned long)s_decoder.input_pos,
                 (unsigned long)s_decoder.input_size,
                 (unsigned)s_decoder.frame_index,
                 (unsigned)s_decoder.segment->frame_count,
                 (unsigned long)s_decoder.raw_bytes,
                 (unsigned long)s_decoder.segment->raw_size,
                 (unsigned long)s_decoder.raw_crc32,
                 (unsigned long)s_decoder.segment->raw_crc32);
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}

static void invalidate_dirty_area(uint32_t min_x, uint32_t min_y,
                                  uint32_t max_x, uint32_t max_y)
{
    if (!s_image || min_x > max_x || min_y > max_y) {
        return;
    }
    lv_area_t object_area;
    lv_obj_get_coords(s_image, &object_area);
    const lv_area_t dirty = {
        .x1 = object_area.x1 + (lv_coord_t)min_x,
        .y1 = object_area.y1 + (lv_coord_t)min_y,
        .x2 = object_area.x1 + (lv_coord_t)max_x,
        .y2 = object_area.y1 + (lv_coord_t)max_y,
    };
    (void)lv_obj_invalidate_area(s_image, &dirty);
}

static void reset_dirty_areas(void)
{
    for (uint32_t band = 0; band < DIRTY_BAND_COUNT; ++band) {
        s_band_min_x[band] = FRAME_WIDTH;
        s_band_min_y[band] = FRAME_HEIGHT;
        s_band_max_x[band] = 0U;
        s_band_max_y[band] = 0U;
    }
    for (uint32_t y = 0; y < FRAME_HEIGHT; ++y) {
        s_row_min_x[y] = FRAME_WIDTH;
        s_row_max_x[y] = 0U;
    }
}

static void include_dirty_pixel(uint32_t pixel)
{
    const uint32_t x = pixel % FRAME_WIDTH;
    const uint32_t y = pixel / FRAME_WIDTH;
    const uint32_t band = y / DIRTY_BAND_HEIGHT;
    if (x < s_band_min_x[band]) s_band_min_x[band] = (uint16_t)x;
    if (x > s_band_max_x[band]) s_band_max_x[band] = (uint16_t)x;
    if (y < s_band_min_y[band]) s_band_min_y[band] = (uint16_t)y;
    if (y > s_band_max_y[band]) s_band_max_y[band] = (uint16_t)y;
    if (x < s_row_min_x[y]) s_row_min_x[y] = (uint16_t)x;
    if (x > s_row_max_x[y]) s_row_max_x[y] = (uint16_t)x;
}

static void include_dirty_run(uint32_t offset, uint32_t length)
{
    while (length > 0U) {
        const uint32_t y = offset / FRAME_WIDTH;
        const uint32_t x = offset % FRAME_WIDTH;
        const uint32_t row_length =
            length < FRAME_WIDTH - x ? length : FRAME_WIDTH - x;
        const uint32_t max_x = x + row_length - 1U;
        const uint32_t band = y / DIRTY_BAND_HEIGHT;
        if (x < s_band_min_x[band]) s_band_min_x[band] = (uint16_t)x;
        if (max_x > s_band_max_x[band]) s_band_max_x[band] = (uint16_t)max_x;
        if (y < s_band_min_y[band]) s_band_min_y[band] = (uint16_t)y;
        if (y > s_band_max_y[band]) s_band_max_y[band] = (uint16_t)y;
        if (x < s_row_min_x[y]) s_row_min_x[y] = (uint16_t)x;
        if (max_x > s_row_max_x[y]) s_row_max_x[y] = (uint16_t)max_x;
        offset += row_length;
        length -= row_length;
    }
}

static esp_err_t decode_frame_prepare(void)
{
    if (!s_decoder.segment ||
        s_decoder.frame_index >= s_decoder.segment->frame_count) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_changed_bits) {
        memset(s_changed_bits, 0,
               CHANGED_WORD_COUNT * sizeof(*s_changed_bits));
    }
    reset_dirty_areas();
    uint32_t dirty_pixels = 0;
    uint32_t attempts = 0;

    while (s_decoder.frame_bytes < FRAME_PIXELS) {
        if (++attempts > DECODE_PROGRESS_LIMIT) {
            return ESP_ERR_TIMEOUT;
        }
        if (s_decoder.pending_size == 0U) {
            if (s_decoder.status == TINFL_STATUS_DONE) {
                return ESP_ERR_INVALID_SIZE;
            }
            ESP_RETURN_ON_ERROR(inflate_pending_output(), TAG,
                                "emotion frame inflate failed");
        }
        if (s_decoder.pending_size == 0U) {
            if (s_decoder.status == TINFL_STATUS_DONE) {
                return ESP_ERR_INVALID_SIZE;
            }
            continue;
        }

        if (s_decoder.pending_pos >= TINFL_LZ_DICT_SIZE ||
            s_decoder.pending_size >
                TINFL_LZ_DICT_SIZE - s_decoder.pending_pos) {
            return ESP_ERR_INVALID_SIZE;
        }
        const uint32_t remaining = FRAME_PIXELS - s_decoder.frame_bytes;
        const uint32_t raw_size = s_decoder.pending_size < remaining
                                      ? s_decoder.pending_size
                                      : remaining;
        const uint8_t *raw = s_dictionary + s_decoder.pending_pos;
        if (s_decoder.raw_bytes > s_decoder.segment->raw_size ||
            raw_size > s_decoder.segment->raw_size - s_decoder.raw_bytes) {
            return ESP_ERR_INVALID_SIZE;
        }
        s_decoder.raw_crc32 = esp_rom_crc32_le(
            s_decoder.raw_crc32, raw, raw_size);
        s_decoder.raw_bytes += raw_size;
        const bool absolute_frame = s_decoder.frame_index == 0;
        for (uint32_t i = 0; i < raw_size; ++i) {
            const uint8_t encoded = raw[i];
            if (!absolute_frame && encoded == 0U) {
                continue;
            }
            const uint32_t pixel = s_decoder.frame_bytes + i;
            const uint8_t previous = s_indices[pixel];
            const uint8_t next = absolute_frame
                                     ? encoded
                                     : (uint8_t)(previous ^ encoded);
            if (next >= s_entry->palette_count) {
                ESP_LOGE(TAG,
                         "palette index invalid asset=%u frame=%u pixel=%lu index=%u colors=%u",
                         (unsigned)s_asset_id,
                         (unsigned)s_decoder.frame_index,
                         (unsigned long)pixel,
                         (unsigned)next,
                         (unsigned)s_entry->palette_count);
                return ESP_ERR_INVALID_ARG;
            }
            const bool changed =
                s_compare_rgb_on_absolute_frame
                    ? s_rgb565[pixel] != s_palette[next]
                    : next != previous;
            if (changed) {
                include_dirty_pixel(pixel);
                if (s_changed_bits) {
                    s_changed_bits[pixel >> 5U] |= 1UL << (pixel & 31U);
                } else {
                    s_rgb565[pixel] = s_palette[next];
                }
                dirty_pixels++;
            }
            s_indices[pixel] = next;
        }
        s_decoder.frame_bytes += raw_size;
        s_decoder.pending_pos += raw_size;
        s_decoder.pending_size -= raw_size;
        if (s_decoder.pending_size == 0U) {
            s_decoder.pending_pos = 0U;
        }
    }

    s_decoder.frame_bytes = 0;
    s_decoder.frame_index++;
    s_compare_rgb_on_absolute_frame = false;
    s_pending_dirty_pixels = dirty_pixels;
    return ESP_OK;
}

static esp_err_t decode_v2_frame_prepare(decode_frame_slot_t *slot)
{
    if (!s_decoder.segment || !slot || !slot->data ||
        s_decoder.frame_index >= s_decoder.segment->frame_count) {
        return ESP_ERR_INVALID_STATE;
    }

    v2_frame_header_t header;
    ESP_RETURN_ON_ERROR(decoder_read_raw(&header, sizeof(header)), TAG,
                        "emotion v2 frame header read failed");
    const bool absolute =
        (header.flags & NUWAX_EMOTION_DELTA_FRAME_FLAG_ABSOLUTE) != 0U;
    if ((header.flags & ~NUWAX_EMOTION_DELTA_FRAME_FLAG_ABSOLUTE) != 0U ||
        absolute != (s_decoder.frame_index == 0U) ||
        header.run_count > NUWAX_EMOTION_DELTA_MAX_FRAME_RUNS ||
        (absolute && header.run_count == 0U) ||
        header.changed_count > FRAME_PIXELS ||
        (header.run_count == 0U && header.changed_count != 0U)) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(slot->data, &header, sizeof(header));
    uint32_t record_size = sizeof(header);
    uint32_t previous_end = 0U;
    uint32_t encoded_pixels = 0U;
    for (uint16_t run_index = 0; run_index < header.run_count; ++run_index) {
        v2_run_header_t run;
        ESP_RETURN_ON_ERROR(decoder_read_raw(&run, sizeof(run)), TAG,
                            "emotion v2 run header read failed");
        const uint64_t run_end = (uint64_t)run.offset + run.length;
        if (run.length == 0U || run.offset < previous_end ||
            run_end > FRAME_PIXELS ||
            (!absolute &&
             run.offset / FRAME_WIDTH !=
                 ((uint32_t)run_end - 1U) / FRAME_WIDTH) ||
            record_size > V2_MAX_PUBLISH_RECORD_BYTES - sizeof(run) ||
            record_size + sizeof(run) > V2_MAX_PUBLISH_RECORD_BYTES -
                (uint32_t)run.length * sizeof(uint16_t)) {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(slot->data + record_size, &run, sizeof(run));
        record_size += sizeof(run);
        ESP_RETURN_ON_ERROR(
            decoder_read_raw(slot->data + record_size, run.length), TAG,
            "emotion v2 run payload read failed");
        /* Expand backwards in place so the LVGL owner can publish each run
         * with one memcpy and no palette lookup. The source byte for index i
         * is consumed before its two-byte destination can overlap it. */
        for (uint32_t i = run.length; i > 0U; --i) {
            const uint8_t palette_index =
                slot->data[record_size + i - 1U];
            if (palette_index >= s_entry->palette_count) {
                return ESP_ERR_INVALID_ARG;
            }
            const uint16_t rgb = s_palette[palette_index];
            const uint32_t destination = record_size + (i - 1U) * 2U;
            slot->data[destination] = (uint8_t)rgb;
            slot->data[destination + 1U] = (uint8_t)(rgb >> 8U);
        }
        record_size += (uint32_t)run.length * sizeof(uint16_t);
        previous_end = (uint32_t)run_end;
        encoded_pixels += run.length;
    }
    if (encoded_pixels != header.changed_count ||
        (absolute && (encoded_pixels != FRAME_PIXELS ||
                      previous_end != FRAME_PIXELS))) {
        return ESP_ERR_INVALID_SIZE;
    }

    slot->size = record_size;
    slot->encoded_pixels = encoded_pixels;
    slot->source_segment_index = s_decoder.segment_index;
    slot->source_frame_index = s_decoder.frame_index;
    slot->source_frame_count = s_decoder.segment->frame_count;
    s_decoder.frame_index++;
    return ESP_OK;
}

static esp_err_t v3_read_checked(uint32_t offset, uint32_t size,
                                 uint32_t expected_crc32)
{
    if (size == 0U || size > TINFL_LZ_DICT_SIZE || expected_crc32 == 0U) {
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_RETURN_ON_ERROR(
        nuwax_emotion_asset_pack_read(offset, s_dictionary, size), TAG,
        "emotion v3 bootstrap read failed");
    if (esp_rom_crc32_le(0U, s_dictionary, size) != expected_crc32) {
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}

static esp_err_t v3_expand_key(decode_frame_slot_t *slot,
                               const nuwax_emotion_asset_v3_segment_t *v3)
{
    ESP_RETURN_ON_ERROR(v3_read_checked(v3->key_offset, v3->key_size,
                                        v3->key_crc32), TAG,
                        "emotion v3 key validation failed");
    v2_frame_header_t header = {
        .run_count = 2U,
        .flags = NUWAX_EMOTION_DELTA_FRAME_FLAG_ABSOLUTE,
        .changed_count = FRAME_PIXELS,
    };
    memcpy(slot->data, &header, sizeof(header));
    const v2_run_header_t first = {.offset = 0U, .length = UINT16_MAX};
    const v2_run_header_t second = {
        .offset = UINT16_MAX,
        .length = (uint16_t)(FRAME_PIXELS - UINT16_MAX),
    };
    const uint32_t first_payload = sizeof(header) + sizeof(first);
    const uint32_t second_header = first_payload +
                                   UINT16_MAX * sizeof(uint16_t);
    const uint32_t second_payload = second_header + sizeof(second);
    memcpy(slot->data + sizeof(header), &first, sizeof(first));
    memcpy(slot->data + second_header, &second, sizeof(second));
    uint32_t pixel = 0U;
    for (uint32_t cursor = 0U; cursor < v3->key_size;
         cursor += V3_KEY_RUN_BYTES) {
        const uint16_t length = (uint16_t)s_dictionary[cursor] |
                                (uint16_t)s_dictionary[cursor + 1U] << 8U;
        const uint8_t index = s_dictionary[cursor + 2U];
        if (length == 0U || index >= s_entry->palette_count ||
            length > FRAME_PIXELS - pixel) {
            return ESP_ERR_INVALID_SIZE;
        }
        const uint16_t rgb = s_palette[index];
        for (uint32_t i = 0U; i < length; ++i) {
            const uint32_t absolute_pixel = pixel + i;
            const uint32_t destination =
                absolute_pixel < UINT16_MAX
                    ? first_payload + absolute_pixel * sizeof(uint16_t)
                    : second_payload +
                          (absolute_pixel - UINT16_MAX) * sizeof(uint16_t);
            slot->data[destination] = (uint8_t)rgb;
            slot->data[destination + 1U] = (uint8_t)(rgb >> 8U);
        }
        pixel += length;
    }
    if (pixel != FRAME_PIXELS) {
        return ESP_ERR_INVALID_SIZE;
    }
    slot->size = sizeof(header) + 2U * sizeof(v2_run_header_t) +
                 FRAME_RGB_BYTES;
    slot->encoded_pixels = FRAME_PIXELS;
    slot->bootstrap_delta = false;
    return ESP_OK;
}

static esp_err_t v3_expand_patch(decode_frame_slot_t *slot, uint32_t offset,
                                 uint32_t size, uint32_t crc32)
{
    ESP_RETURN_ON_ERROR(v3_read_checked(offset, size, crc32), TAG,
                        "emotion v3 patch validation failed");
    if (!nuwax_emotion_delta_record_validate(
            s_dictionary, size, s_entry->palette_count, false, NULL)) {
        return ESP_ERR_INVALID_SIZE;
    }
    v2_frame_header_t header;
    memcpy(&header, s_dictionary, sizeof(header));
    memcpy(slot->data, &header, sizeof(header));
    uint32_t source = sizeof(header);
    uint32_t output = sizeof(header);
    for (uint16_t run_index = 0U; run_index < header.run_count; ++run_index) {
        v2_run_header_t run;
        memcpy(&run, s_dictionary + source, sizeof(run));
        source += sizeof(run);
        memcpy(slot->data + output, &run, sizeof(run));
        output += sizeof(run);
        for (uint32_t i = 0U; i < run.length; ++i) {
            const uint8_t index = s_dictionary[source + i];
            const uint16_t rgb = s_palette[index];
            slot->data[output + i * 2U] = (uint8_t)rgb;
            slot->data[output + i * 2U + 1U] = (uint8_t)(rgb >> 8U);
        }
        source += run.length;
        output += (uint32_t)run.length * sizeof(uint16_t);
    }
    if (source != size || output > V2_MAX_PUBLISH_RECORD_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    slot->size = output;
    slot->encoded_pixels = header.changed_count;
    slot->bootstrap_delta = true;
    return ESP_OK;
}

static esp_err_t decode_v3_frame_prepare(decode_frame_slot_t *slot)
{
    const nuwax_emotion_asset_v3_segment_t *v3 =
        nuwax_emotion_asset_pack_v3_segment(s_decoder.segment_index);
    if (!slot || !v3 || !s_decoder.segment ||
        s_decoder.frame_index >= s_decoder.segment->frame_count) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_decoder.frame_index == 0U) {
        esp_err_t err = ESP_ERR_INVALID_STATE;
        if (s_v3_bootstrap == V3_BOOTSTRAP_TRANSITION) {
            err = v3_expand_patch(slot, v3->transition_offset,
                                  v3->transition_size,
                                  v3->transition_crc32);
        } else if (s_v3_bootstrap == V3_BOOTSTRAP_WRAP) {
            err = v3_expand_patch(slot, v3->wrap_offset, v3->wrap_size,
                                  v3->wrap_crc32);
        } else {
            err = v3_expand_key(slot, v3);
        }
        if (err != ESP_OK) return err;
        slot->source_segment_index = s_decoder.segment_index;
        slot->source_frame_index = 0U;
        slot->source_frame_count = s_decoder.segment->frame_count;
        s_decoder.frame_index = 1U;
        return ESP_OK;
    }
    const esp_err_t err = decode_v2_frame_prepare(slot);
    slot->bootstrap_delta = false;
    return err;
}

static size_t inflate_v4_record(void *output, size_t output_capacity,
                                const void *compressed,
                                size_t compressed_size)
{
    if (!output || output_capacity == 0U || !compressed ||
        compressed_size == 0U || !s_inflater) {
        return TINFL_DECOMPRESS_MEM_TO_MEM_FAILED;
    }

    /* tinfl_decompress_mem_to_mem() places its roughly 11 KiB decompressor
     * object on the caller's stack. Both the LVGL owner and background decoder
     * use this path, so keep that object in the preallocated decode workspace
     * and call the low-level ROM routine directly. */
    tinfl_init(s_inflater);
    size_t input_bytes = compressed_size;
    size_t output_bytes = output_capacity;
    const tinfl_status status = tinfl_decompress(
        s_inflater, (const mz_uint8 *)compressed, &input_bytes,
        (mz_uint8 *)output, (mz_uint8 *)output, &output_bytes,
        TINFL_FLAG_PARSE_ZLIB_HEADER |
            TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    return status == TINFL_STATUS_DONE
               ? output_bytes : TINFL_DECOMPRESS_MEM_TO_MEM_FAILED;
}

static size_t decode_v4_rle_record(uint8_t *output, size_t output_capacity,
                                   const uint8_t *encoded,
                                   size_t encoded_size)
{
    if (!output || !encoded) return TINFL_DECOMPRESS_MEM_TO_MEM_FAILED;
    size_t input_cursor = 0U;
    size_t output_cursor = 0U;
    while (input_cursor < encoded_size) {
        const uint8_t control = encoded[input_cursor++];
        const size_t count = (size_t)(control & 0x7fU) + 1U;
        if (count > output_capacity - output_cursor) {
            return TINFL_DECOMPRESS_MEM_TO_MEM_FAILED;
        }
        if ((control & 0x80U) != 0U) {
            if (input_cursor >= encoded_size) {
                return TINFL_DECOMPRESS_MEM_TO_MEM_FAILED;
            }
            memset(output + output_cursor, encoded[input_cursor++], count);
        } else {
            if (count > encoded_size - input_cursor) {
                return TINFL_DECOMPRESS_MEM_TO_MEM_FAILED;
            }
            memcpy(output + output_cursor, encoded + input_cursor, count);
            input_cursor += count;
        }
        output_cursor += count;
    }
    return output_cursor;
}

static esp_err_t decode_v4_record_payload(
    const nuwax_emotion_asset_v4_frame_t *frame, uint8_t *output,
    size_t output_capacity, uint8_t *encoded_work)
{
    if (!frame || !output || !encoded_work || frame->compressed_size == 0U ||
        frame->compressed_size > V4_MAX_COMPRESSED_FRAME_BYTES ||
        frame->raw_size > output_capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_RETURN_ON_ERROR(
        nuwax_emotion_asset_pack_read(frame->compressed_offset, encoded_work,
                                      frame->compressed_size),
        TAG, "emotion v4 record read failed");
    const size_t output_bytes = frame->rle
        ? decode_v4_rle_record(output, output_capacity, encoded_work,
                               frame->compressed_size)
        : inflate_v4_record(output, output_capacity, encoded_work,
                            frame->compressed_size);
    if (output_bytes != frame->raw_size) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return esp_rom_crc32_le(0U, output, frame->raw_size) == frame->raw_crc32
        ? ESP_OK : ESP_ERR_INVALID_CRC;
}

static esp_err_t decode_v4_frame_prepare(decode_frame_slot_t *slot)
{
    if (!slot || !slot->data || !s_decoder.segment ||
        s_decoder.frame_index >= s_decoder.segment->frame_count) {
        return ESP_ERR_INVALID_STATE;
    }
    nuwax_emotion_asset_v4_frame_t frame;
    ESP_RETURN_ON_ERROR(
        nuwax_emotion_asset_pack_v4_frame(
            s_decoder.segment_index, s_decoder.frame_index, &frame),
        TAG, "emotion v4 frame metadata failed");
    if (frame.compressed_size == 0U ||
        frame.compressed_size > V4_MAX_COMPRESSED_FRAME_BYTES ||
        frame.region_count > NUWAX_EMOTION_ASSET_V4_MAX_REGIONS) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *compressed = slot->data + FRAME_PIXELS;
    const esp_err_t decode_err = decode_v4_record_payload(
        &frame, slot->data, FRAME_PIXELS, compressed);
    if (decode_err != ESP_OK) {
        ESP_LOGE(TAG,
                 "emotion v4 frame decode failed segment=%u frame=%u err=%s",
                 (unsigned)s_decoder.segment_index,
                 (unsigned)s_decoder.frame_index,
                 esp_err_to_name(decode_err));
        return decode_err;
    }
    slot->size = frame.raw_size;
    slot->encoded_pixels = frame.changed_count;
    slot->source_segment_index = s_decoder.segment_index;
    slot->source_frame_index = s_decoder.frame_index;
    slot->source_frame_count = s_decoder.segment->frame_count;
    slot->v4_frame = true;
    slot->absolute = frame.absolute;
    slot->regional_payload = frame.regional_payload;
    slot->region_count = frame.region_count;
    memcpy(slot->regions, frame.regions, sizeof(slot->regions));
    s_decoder.frame_index++;
    return ESP_OK;
}

static esp_err_t decode_v4_handoff_frame(
    nuwax_emotion_asset_id_t asset_id, uint16_t segment_index,
    uint16_t target_frame_index, uint8_t *indices_out, uint16_t *rgb_out)
{
    if (!indices_out || !rgb_out ||
        nuwax_emotion_asset_pack_abi() != NUWAX_EMOTION_ASSET_PACK_ABI_V4) {
        return ESP_ERR_INVALID_ARG;
    }
    const nuwax_emotion_asset_entry_t *entry =
        nuwax_emotion_asset_pack_find(asset_id);
    const nuwax_emotion_asset_segment_t *segment =
        nuwax_emotion_asset_pack_segment(segment_index);
    if (!entry || !segment || segment->frame_count == 0U ||
        target_frame_index >= segment->frame_count) {
        return ESP_ERR_NOT_FOUND;
    }
    uint16_t palette[NUWAX_EMOTION_ASSET_PALETTE_COLORS];
    ESP_RETURN_ON_ERROR(nuwax_emotion_asset_pack_read_palette(entry, palette),
                        TAG, "emotion bridge palette read failed");
    nuwax_emotion_asset_v4_frame_t frame;
    bool fast_handoff = false;
    if (target_frame_index != 0U) {
        const esp_err_t handoff_err =
            nuwax_emotion_asset_pack_v4_handoff_frame(
                segment_index, target_frame_index, &frame);
        if (handoff_err == ESP_OK) {
            fast_handoff = true;
        } else if (handoff_err != ESP_ERR_NOT_FOUND) {
            return handoff_err;
        }
    }
    if (!fast_handoff) {
        ESP_RETURN_ON_ERROR(
            nuwax_emotion_asset_pack_v4_frame(segment_index, 0U, &frame), TAG,
            "emotion bridge key metadata failed");
    }
    if (!frame.absolute || frame.compressed_size == 0U ||
        frame.raw_size != FRAME_PIXELS ||
        frame.compressed_size > V4_MAX_COMPRESSED_FRAME_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    /* RGB is populated only after the requested handoff frame is complete.
     * Its first half is a full-size inflate workspace; the second half holds
     * the bounded compressed record. This preserves the source snapshot and
     * avoids another long-lived PSRAM allocation. */
    uint8_t *raw = (uint8_t *)rgb_out;
    uint8_t *compressed = raw + FRAME_PIXELS;
    esp_err_t err = decode_v4_record_payload(
        &frame, indices_out, FRAME_PIXELS, compressed);
    if (err != ESP_OK) return err;

    /* A fast absolute handoff is already the requested pose. Legacy v4 packs
     * retain the replay path below so firmware-only OTA remains compatible. */
    if (fast_handoff) {
        goto convert_handoff_palette;
    }

    for (uint16_t frame_index = 1U; frame_index <= target_frame_index;
         ++frame_index) {
        ESP_RETURN_ON_ERROR(
            nuwax_emotion_asset_pack_v4_frame(segment_index, frame_index,
                                               &frame),
            TAG, "emotion bridge handoff metadata failed");
        if (frame.absolute || frame.compressed_size == 0U ||
            frame.compressed_size > V4_MAX_COMPRESSED_FRAME_BYTES ||
            frame.raw_size == 0U || frame.raw_size > FRAME_PIXELS ||
            frame.region_count > NUWAX_EMOTION_ASSET_V4_MAX_REGIONS) {
            return ESP_ERR_INVALID_SIZE;
        }
        ESP_RETURN_ON_ERROR(
            decode_v4_record_payload(&frame, raw, frame.raw_size, compressed),
            TAG, "emotion bridge handoff payload decode failed");

        uint32_t payload_cursor = 0U;
        uint32_t changed_count = 0U;
        for (uint8_t region_index = 0U;
             region_index < frame.region_count; ++region_index) {
            const nuwax_emotion_asset_region_t region =
                frame.regions[region_index];
            for (uint32_t y = region.y; y < region.y + region.height; ++y) {
                const uint32_t row_start = y * FRAME_WIDTH + region.x;
                for (uint32_t x = 0U; x < region.width; ++x) {
                    const uint32_t pixel = row_start + x;
                    const uint8_t encoded = raw[
                        frame.regional_payload ? payload_cursor++ : pixel];
                    if (encoded == 0U) continue;
                    const uint8_t next =
                        (uint8_t)(indices_out[pixel] ^ encoded);
                    if (next >= entry->palette_count) {
                        return ESP_ERR_INVALID_ARG;
                    }
                    indices_out[pixel] = next;
                    changed_count++;
                }
            }
        }
        if ((frame.regional_payload && payload_cursor != frame.raw_size) ||
            changed_count != frame.changed_count) {
            return ESP_ERR_INVALID_SIZE;
        }
    }

convert_handoff_palette:
    for (uint32_t pixel = 0U; pixel < FRAME_PIXELS; ++pixel) {
        const uint8_t index = indices_out[pixel];
        if (index >= entry->palette_count) return ESP_ERR_INVALID_ARG;
        rgb_out[pixel] = palette[index];
    }
    return ESP_OK;
}

static esp_err_t prepare_next_frame(decode_frame_slot_t *slot)
{
    const uint16_t abi = nuwax_emotion_asset_pack_abi();
    if (abi == NUWAX_EMOTION_ASSET_PACK_ABI_V4) {
        return decode_v4_frame_prepare(slot);
    }
    if (abi == NUWAX_EMOTION_ASSET_PACK_ABI_V3) {
        return decode_v3_frame_prepare(slot);
    }
    return abi == NUWAX_EMOTION_ASSET_PACK_ABI_V2
               ? decode_v2_frame_prepare(slot) : decode_frame_prepare();
}

static esp_err_t publish_v2_frame(decode_frame_slot_t *slot)
{
    if (!slot || !slot->data || slot->size < sizeof(v2_frame_header_t)) {
        return ESP_ERR_INVALID_STATE;
    }
    v2_frame_header_t header;
    memcpy(&header, slot->data, sizeof(header));
    const bool absolute =
        (header.flags & NUWAX_EMOTION_DELTA_FRAME_FLAG_ABSOLUTE) != 0U;
    if (absolute !=
            (slot->source_frame_index == 0U && !slot->bootstrap_delta) ||
        header.run_count > NUWAX_EMOTION_DELTA_MAX_FRAME_RUNS ||
        header.changed_count > FRAME_PIXELS) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint32_t cursor = sizeof(header);
    uint32_t changed_pixels = 0U;
    reset_dirty_areas();
    for (uint16_t run_index = 0; run_index < header.run_count; ++run_index) {
        if (cursor > slot->size - sizeof(v2_run_header_t)) {
            return ESP_ERR_INVALID_SIZE;
        }
        v2_run_header_t run;
        memcpy(&run, slot->data + cursor, sizeof(run));
        cursor += sizeof(run);
        const uint32_t payload_bytes =
            (uint32_t)run.length * sizeof(uint16_t);
        if (run.length == 0U || run.offset >= FRAME_PIXELS ||
            run.length > FRAME_PIXELS - run.offset ||
            payload_bytes > slot->size - cursor) {
            return ESP_ERR_INVALID_SIZE;
        }
        const uint8_t *payload = slot->data + cursor;
        uint32_t changed_start = UINT32_MAX;
        for (uint32_t i = 0U; i < run.length; ++i) {
            const uint16_t rgb = (uint16_t)payload[i * 2U] |
                                 (uint16_t)payload[i * 2U + 1U] << 8U;
            const uint32_t pixel = run.offset + i;
            if (s_rgb565[pixel] != rgb) {
                s_rgb565[pixel] = rgb;
                changed_pixels++;
                if (changed_start == UINT32_MAX) {
                    changed_start = pixel;
                }
            } else if (changed_start != UINT32_MAX) {
                include_dirty_run(changed_start, pixel - changed_start);
                changed_start = UINT32_MAX;
            }
        }
        if (changed_start != UINT32_MAX) {
            include_dirty_run(changed_start,
                              run.offset + run.length - changed_start);
        }
        cursor += payload_bytes;
    }
    if (cursor != slot->size) {
        return ESP_ERR_INVALID_SIZE;
    }
    s_pending_dirty_pixels = changed_pixels;
    slot->size = 0U;
    return ESP_OK;
}

static esp_err_t publish_v4_frame(decode_frame_slot_t *slot)
{
    if (!slot || !slot->data || !slot->v4_frame || !s_indices ||
        (!slot->regional_payload && slot->size != FRAME_PIXELS) ||
        slot->region_count > NUWAX_EMOTION_ASSET_V4_MAX_REGIONS) {
        return ESP_ERR_INVALID_STATE;
    }
    reset_dirty_areas();
    uint32_t changed_pixels = 0U;
    uint32_t payload_cursor = 0U;
    const uint8_t region_count = slot->absolute ? 1U : slot->region_count;
    for (uint8_t region_index = 0U; region_index < region_count; ++region_index) {
        const nuwax_emotion_asset_region_t region = slot->absolute
            ? (nuwax_emotion_asset_region_t){
                  .x = 0U, .y = 0U,
                  .width = FRAME_WIDTH, .height = FRAME_HEIGHT,
              }
            : slot->regions[region_index];
        if (region.width == 0U || region.height == 0U ||
            region.x >= FRAME_WIDTH || region.y >= FRAME_HEIGHT ||
            region.width > FRAME_WIDTH - region.x ||
            region.height > FRAME_HEIGHT - region.y) {
            return ESP_ERR_INVALID_SIZE;
        }
        for (uint32_t y = region.y; y < region.y + region.height; ++y) {
            const uint32_t row_start = y * FRAME_WIDTH + region.x;
            uint32_t changed_start = UINT32_MAX;
            for (uint32_t x = 0U; x < region.width; ++x) {
                const uint32_t pixel = row_start + x;
                const uint8_t encoded = slot->data[
                    slot->regional_payload ? payload_cursor++ : pixel];
                if (!slot->absolute && encoded == 0U) {
                    if (changed_start != UINT32_MAX) {
                        include_dirty_run(changed_start, pixel - changed_start);
                        changed_start = UINT32_MAX;
                    }
                    continue;
                }
                const uint8_t previous = s_indices[pixel];
                const uint8_t next = slot->absolute
                    ? encoded : (uint8_t)(previous ^ encoded);
                if (next >= s_entry->palette_count) {
                    return ESP_ERR_INVALID_ARG;
                }
                const uint16_t rgb = s_palette[next];
                if (s_rgb565[pixel] != rgb) {
                    s_rgb565[pixel] = rgb;
                    changed_pixels++;
                    if (changed_start == UINT32_MAX) changed_start = pixel;
                } else if (changed_start != UINT32_MAX) {
                    include_dirty_run(changed_start, pixel - changed_start);
                    changed_start = UINT32_MAX;
                }
                s_indices[pixel] = next;
            }
            if (changed_start != UINT32_MAX) {
                include_dirty_run(changed_start,
                                  row_start + region.width - changed_start);
            }
        }
    }
    if (slot->regional_payload && payload_cursor != slot->size) {
        return ESP_ERR_INVALID_SIZE;
    }
    s_pending_dirty_pixels = changed_pixels;
    slot->size = 0U;
    return ESP_OK;
}

static uint32_t dirty_band_pixels(void)
{
    uint32_t pixels = 0U;
    for (uint32_t band = 0; band < DIRTY_BAND_COUNT; ++band) {
        if (s_band_min_x[band] > s_band_max_x[band] ||
            s_band_min_y[band] > s_band_max_y[band]) {
            continue;
        }
        pixels +=
            ((uint32_t)s_band_max_x[band] - s_band_min_x[band] + 1U) *
            ((uint32_t)s_band_max_y[band] - s_band_min_y[band] + 1U);
    }
    return pixels;
}

static bool collect_dirty_row_regions(dirty_region_t *regions,
                                      uint32_t *region_count,
                                      uint32_t *region_pixels)
{
    uint32_t count = 0U;
    uint32_t pixels = 0U;
    dirty_region_t current = {0};
    bool active = false;

    for (uint32_t y = 0; y < FRAME_HEIGHT; ++y) {
        if (s_row_min_x[y] > s_row_max_x[y]) {
            if (active) {
                if (count >= MAX_DIRTY_REGIONS) return false;
                regions[count++] = current;
                pixels +=
                    ((uint32_t)current.max_x - current.min_x + 1U) *
                    ((uint32_t)current.max_y - current.min_y + 1U);
                active = false;
            }
            continue;
        }

        if (!active) {
            current = (dirty_region_t){
                .min_x = s_row_min_x[y],
                .min_y = (uint16_t)y,
                .max_x = s_row_max_x[y],
                .max_y = (uint16_t)y,
            };
            active = true;
            continue;
        }

        const bool horizontally_close =
            (uint32_t)s_row_min_x[y] <=
                (uint32_t)current.max_x + DIRTY_ROW_MERGE_GAP &&
            (uint32_t)current.min_x <=
                (uint32_t)s_row_max_x[y] + DIRTY_ROW_MERGE_GAP;
        if (y == (uint32_t)current.max_y + 1U && horizontally_close) {
            if (s_row_min_x[y] < current.min_x) current.min_x = s_row_min_x[y];
            if (s_row_max_x[y] > current.max_x) current.max_x = s_row_max_x[y];
            current.max_y = (uint16_t)y;
            continue;
        }

        if (count >= MAX_DIRTY_REGIONS) return false;
        regions[count++] = current;
        pixels += ((uint32_t)current.max_x - current.min_x + 1U) *
                  ((uint32_t)current.max_y - current.min_y + 1U);
        current = (dirty_region_t){
            .min_x = s_row_min_x[y],
            .min_y = (uint16_t)y,
            .max_x = s_row_max_x[y],
            .max_y = (uint16_t)y,
        };
    }

    if (active) {
        if (count >= MAX_DIRTY_REGIONS) return false;
        regions[count++] = current;
        pixels += ((uint32_t)current.max_x - current.min_x + 1U) *
                  ((uint32_t)current.max_y - current.min_y + 1U);
    }
    *region_count = count;
    *region_pixels = pixels;
    return true;
}

static uint32_t dirty_region_pixels(const dirty_region_t *region)
{
    return ((uint32_t)region->max_x - region->min_x + 1U) *
           ((uint32_t)region->max_y - region->min_y + 1U);
}

static uint32_t compact_dirty_regions(dirty_region_t *regions,
                                      uint32_t region_count)
{
    while (region_count > TARGET_DIRTY_REGIONS) {
        uint32_t best = 0U;
        uint32_t best_penalty = UINT32_MAX;
        for (uint32_t i = 0; i + 1U < region_count; ++i) {
            const dirty_region_t merged = {
                .min_x = regions[i].min_x < regions[i + 1U].min_x
                             ? regions[i].min_x : regions[i + 1U].min_x,
                .min_y = regions[i].min_y,
                .max_x = regions[i].max_x > regions[i + 1U].max_x
                             ? regions[i].max_x : regions[i + 1U].max_x,
                .max_y = regions[i + 1U].max_y,
            };
            const uint32_t penalty = dirty_region_pixels(&merged) -
                dirty_region_pixels(&regions[i]) -
                dirty_region_pixels(&regions[i + 1U]);
            if (penalty < best_penalty) {
                best_penalty = penalty;
                best = i;
            }
        }
        if (regions[best + 1U].min_x < regions[best].min_x) {
            regions[best].min_x = regions[best + 1U].min_x;
        }
        if (regions[best + 1U].max_x > regions[best].max_x) {
            regions[best].max_x = regions[best + 1U].max_x;
        }
        regions[best].max_y = regions[best + 1U].max_y;
        memmove(&regions[best + 1U], &regions[best + 2U],
                (region_count - best - 2U) * sizeof(*regions));
        region_count--;
    }
    return region_count;
}

static uint32_t invalidate_dirty_regions(void)
{
    const uint32_t band_pixels = dirty_band_pixels();
    dirty_region_t regions[MAX_DIRTY_REGIONS];
    uint32_t region_count = 0U;
    uint32_t region_pixels = 0U;
    const bool rows_valid =
        collect_dirty_row_regions(regions, &region_count, &region_pixels);
    if (rows_valid && region_count > TARGET_DIRTY_REGIONS) {
        region_count = compact_dirty_regions(regions, region_count);
        region_pixels = 0U;
        for (uint32_t i = 0; i < region_count; ++i) {
            region_pixels += dirty_region_pixels(&regions[i]);
        }
    }
    const uint32_t row_cost = region_pixels +
        region_count * DIRTY_REGION_FIXED_COST_PIXELS;
    uint32_t band_count = 0U;
    for (uint32_t band = 0; band < DIRTY_BAND_COUNT; ++band) {
        if (s_band_min_x[band] <= s_band_max_x[band] &&
            s_band_min_y[band] <= s_band_max_y[band]) {
            band_count++;
        }
    }
    const uint32_t band_cost = band_pixels +
        band_count * DIRTY_REGION_FIXED_COST_PIXELS;
    const bool use_rows = rows_valid && row_cost <= band_cost;

    if (use_rows) {
        for (uint32_t i = 0; i < region_count; ++i) {
            invalidate_dirty_area(regions[i].min_x, regions[i].min_y,
                                  regions[i].max_x, regions[i].max_y);
        }
        return region_pixels;
    }

    for (uint32_t band = 0; band < DIRTY_BAND_COUNT; ++band) {
        if (s_band_min_x[band] > s_band_max_x[band] ||
            s_band_min_y[band] > s_band_max_y[band]) {
            continue;
        }
        invalidate_dirty_area(s_band_min_x[band], s_band_min_y[band],
                              s_band_max_x[band], s_band_max_y[band]);
    }
    return band_pixels;
}

static void publish_decoded_frame(decode_frame_slot_t *slot)
{
    const bool v4_frame = slot && slot->v4_frame;
    if (nuwax_emotion_asset_pack_abi() >= NUWAX_EMOTION_ASSET_PACK_ABI_V2) {
        if (!slot) {
            s_decode_worker_error = ESP_ERR_INVALID_STATE;
            return;
        }
        s_pending_decode_us = slot->decode_us;
        s_pending_dirty_pixels = slot->encoded_pixels;
        const esp_err_t err = slot->v4_frame
                                  ? publish_v4_frame(slot)
                                  : publish_v2_frame(slot);
        if (err != ESP_OK) {
            s_decode_worker_error = err;
            ESP_LOGE(TAG, "emotion frame publish failed: %s",
                     esp_err_to_name(err));
            return;
        }
    } else if (s_changed_bits) {
        for (uint32_t word_index = 0;
             word_index < CHANGED_WORD_COUNT;
             ++word_index) {
            uint32_t bits = s_changed_bits[word_index];
            while (bits != 0U) {
                const uint32_t bit = (uint32_t)__builtin_ctz(bits);
                const uint32_t pixel = word_index * 32U + bit;
                if (pixel < FRAME_PIXELS) {
                    s_rgb565[pixel] = s_palette[s_indices[pixel]];
                }
                bits &= bits - 1U;
            }
        }
    }

    uint32_t invalidated_pixels = 0U;
    if (v4_frame && s_frame_patch_compositor) {
        if (slot->absolute) {
            hide_frame_patches();
            invalidated_pixels = invalidate_dirty_regions();
        } else {
            invalidated_pixels = update_frame_patches(slot->regions,
                                                       slot->region_count);
        }
    } else {
        invalidated_pixels = invalidate_dirty_regions();
    }
    s_frames_rendered++;
    s_decode_total_us += s_pending_decode_us;
    s_health_frames_total++;
    s_health_decode_total_us += s_pending_decode_us;
    if (s_pending_decode_us > s_decode_max_us) {
        s_decode_max_us = s_pending_decode_us;
    }
    s_dirty_pixels_total += s_pending_dirty_pixels;
    s_invalidated_pixels_total += invalidated_pixels;
    s_health_dirty_pixels_total += s_pending_dirty_pixels;
    s_health_invalidated_pixels_total += invalidated_pixels;
    const uint32_t target_fps = s_entry ? s_entry->fps : 0U;
    if (target_fps >= MIN_ANIMATED_FPS && target_fps <= MAX_ANIMATED_FPS &&
        s_pending_decode_us >
            ((1000000U / target_fps) * DECODE_BUDGET_PERCENT) / 100U) {
        s_decode_budget_overruns++;
        s_health_decode_budget_overruns++;
    }
    if (invalidated_pixels >= FRAME_PIXELS) {
        s_full_frame_updates++;
        s_health_full_frame_updates++;
    }
}

static void decode_worker_task(void *argument)
{
    (void)argument;
    while (!atomic_load_explicit(&s_decode_stop_requested,
                                 memory_order_acquire)) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (atomic_load_explicit(&s_decode_stop_requested,
                                 memory_order_acquire)) {
            break;
        }
        int expected = DECODE_WORKER_REQUESTED;
        if (!atomic_compare_exchange_strong_explicit(
                &s_decode_worker_state, &expected, DECODE_WORKER_RUNNING,
                memory_order_acq_rel, memory_order_acquire)) {
            continue;
        }

        int bridge_expected = BRIDGE_DECODE_REQUESTED;
        if (atomic_compare_exchange_strong_explicit(
                &s_bridge_decode_state, &bridge_expected,
                BRIDGE_DECODE_RUNNING, memory_order_acq_rel,
                memory_order_acquire)) {
            const bridge_decode_job_t job = s_bridge_decode_job;
            memcpy(job.source_rgb565, job.visible_rgb565, FRAME_RGB_BYTES);
            atomic_store_explicit(&s_bridge_source_ready, true,
                                  memory_order_release);

            const int64_t started_us = esp_timer_get_time();
            const esp_err_t bridge_err = decode_v4_handoff_frame(
                job.asset_id, job.segment_index, job.frame_index,
                job.target_indices, job.target_rgb565);
            const uint32_t decode_us =
                (uint32_t)(esp_timer_get_time() - started_us);
            if (job.generation == atomic_load_explicit(
                                      &s_bridge_decode_generation,
                                      memory_order_acquire)) {
                s_bridge_decode_job.decode_us = decode_us;
                s_bridge_decode_job.result = bridge_err;
                atomic_store_explicit(
                    &s_bridge_decode_state,
                    bridge_err == ESP_OK ? BRIDGE_DECODE_READY
                                         : BRIDGE_DECODE_FAILED,
                    memory_order_release);
            }
            atomic_store_explicit(&s_decode_worker_state,
                                  DECODE_WORKER_READY,
                                  memory_order_release);
            continue;
        }

        esp_err_t err = ESP_OK;
        if (nuwax_emotion_asset_pack_abi() >=
            NUWAX_EMOTION_ASSET_PACK_ABI_V2) {
            uint32_t decoded_in_batch = 0U;
            while (!atomic_load_explicit(&s_decode_stop_requested,
                                         memory_order_acquire) &&
                   atomic_load_explicit(&s_queued_asset_id,
                                        memory_order_acquire) == 0U &&
                   s_decoder.frame_index < s_decoder.segment->frame_count) {
                if (decoded_in_batch > 0U) {
                    /* Decode shares CPU0 with LVGL and the display flush worker
                     * so CPU1 remains reserved for wake/AEC/audio. Its lower
                     * priority already lets LVGL and flush preempt it. A fixed
                     * one-tick delay is 10 ms at CONFIG_FREERTOS_HZ=100 and
                     * makes a two-slot 40 FPS pipeline miss its deadline even
                     * when both frame decodes are within budget. Yield without
                     * imposing that artificial latency. */
                    taskYIELD();
                    (void)atomic_fetch_add_explicit(
                        &s_health_decode_cooperative_blocks, 1U,
                        memory_order_relaxed);
                    if (atomic_load_explicit(&s_decode_stop_requested,
                                             memory_order_acquire) ||
                        atomic_load_explicit(&s_queued_asset_id,
                                             memory_order_acquire) != 0U) {
                        break;
                    }
                }
                decode_frame_slot_t *slot = find_free_decode_slot();
                if (!slot) break;
                slot->generation = atomic_load_explicit(
                    &s_decode_generation, memory_order_acquire);
                slot->sequence = s_next_decode_sequence++;
                const int64_t started_us = esp_timer_get_time();
                err = prepare_next_frame(slot);
                slot->decode_us =
                    (uint32_t)(esp_timer_get_time() - started_us);
                const bool switch_pending =
                    atomic_load_explicit(&s_queued_asset_id,
                                         memory_order_acquire) != 0U;
                if (switch_pending) {
                    /* The partially advanced decoder belongs to an asset that
                     * has just been superseded. load_asset() resets it after
                     * this worker reaches READY, so publishing the stale frame
                     * only delays the requested expression and creates a visual
                     * flash. */
                    release_decode_slot(slot);
                    (void)atomic_fetch_add_explicit(
                        &s_health_decode_switch_aborts, 1U,
                        memory_order_relaxed);
                    err = ESP_OK;
                    break;
                }
                if (err != ESP_OK ||
                    slot->generation != atomic_load_explicit(
                        &s_decode_generation, memory_order_acquire)) {
                    release_decode_slot(slot);
                    break;
                }
                atomic_store_explicit(&slot->state, DECODE_SLOT_READY,
                                      memory_order_release);
                decoded_in_batch++;
                /* A new emotion command is newest-wins. Finish the record
                 * already being decoded, then yield the shared decoder at a
                 * frame boundary instead of filling more old-asset slots. */
                if (atomic_load_explicit(&s_queued_asset_id,
                                         memory_order_acquire) != 0U) {
                    break;
                }
            }
        } else {
            const int64_t started_us = esp_timer_get_time();
            err = prepare_next_frame(NULL);
            s_pending_decode_us =
                (uint32_t)(esp_timer_get_time() - started_us);
        }
        if (atomic_load_explicit(&s_decode_stop_requested,
                                 memory_order_acquire)) {
            break;
        }
        s_decode_worker_error = err;
        atomic_store_explicit(&s_decode_worker_state,
                              err == ESP_OK ? DECODE_WORKER_READY
                                            : DECODE_WORKER_FAILED,
                              memory_order_release);
    }

    atomic_store_explicit(&s_decode_task_exited, true, memory_order_release);
    vTaskSuspend(NULL);
    while (true) {
        vTaskSuspend(NULL);
    }
}

static esp_err_t decode_worker_init(void)
{
    if (s_decode_task) return ESP_OK;
    s_decode_task_tcb = heap_caps_calloc(
        1, sizeof(*s_decode_task_tcb),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_decode_task_stack = heap_caps_malloc(
        DECODE_TASK_STACK_SIZE,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_decode_task_tcb || !s_decode_task_stack) {
        heap_caps_free(s_decode_task_tcb);
        heap_caps_free(s_decode_task_stack);
        s_decode_task_tcb = NULL;
        s_decode_task_stack = NULL;
        return ESP_ERR_NO_MEM;
    }

    atomic_store_explicit(&s_decode_worker_state, DECODE_WORKER_IDLE,
                          memory_order_release);
    atomic_store_explicit(&s_decode_stop_requested, false,
                          memory_order_release);
    atomic_store_explicit(&s_decode_task_exited, false,
                          memory_order_release);
    s_decode_worker_error = ESP_OK;
    s_decode_task = xTaskCreateStaticPinnedToCore(
        decode_worker_task, "emotion_decode", DECODE_TASK_STACK_SIZE,
        NULL, DECODE_TASK_PRIORITY, s_decode_task_stack,
        s_decode_task_tcb, DECODE_TASK_CORE);
    if (!s_decode_task) {
        heap_caps_free(s_decode_task_tcb);
        heap_caps_free(s_decode_task_stack);
        s_decode_task_tcb = NULL;
        s_decode_task_stack = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t decode_worker_stop(void)
{
    if (!s_decode_task) return ESP_OK;
    atomic_store_explicit(&s_decode_stop_requested, true,
                          memory_order_release);
    xTaskNotifyGive(s_decode_task);
    const int64_t deadline_us = esp_timer_get_time() +
        (int64_t)DECODE_TASK_STOP_TIMEOUT_MS * 1000LL;
    bool stopped = false;
    while (esp_timer_get_time() < deadline_us) {
        if (atomic_load_explicit(&s_decode_task_exited,
                                 memory_order_acquire) &&
            eTaskGetState(s_decode_task) == eSuspended) {
            stopped = true;
            break;
        }
        vTaskDelay(1);
    }
    if (!stopped) {
        ESP_LOGE(TAG,
                 "emotion decode task did not quiesce timeout_ms=%u state=%d; preserving task and frame buffers",
                 (unsigned)DECODE_TASK_STOP_TIMEOUT_MS,
                 (int)eTaskGetState(s_decode_task));
        return ESP_ERR_TIMEOUT;
    }
    vTaskDelete(s_decode_task);
    s_decode_task = NULL;
    heap_caps_free(s_decode_task_tcb);
    heap_caps_free(s_decode_task_stack);
    s_decode_task_tcb = NULL;
    s_decode_task_stack = NULL;
    s_background_decode = false;
    return ESP_OK;
}

static esp_err_t decode_worker_reap(void)
{
    if (!s_decode_task) return ESP_OK;
    if (!atomic_load_explicit(&s_decode_task_exited, memory_order_acquire) ||
        eTaskGetState(s_decode_task) != eSuspended) {
        return ESP_ERR_INVALID_STATE;
    }

    vTaskDelete(s_decode_task);
    s_decode_task = NULL;
    heap_caps_free(s_decode_task_tcb);
    heap_caps_free(s_decode_task_stack);
    s_decode_task_tcb = NULL;
    s_decode_task_stack = NULL;
    s_background_decode = false;
    return ESP_OK;
}

static esp_err_t decode_worker_quiesce(void)
{
    if (!s_background_decode) return ESP_OK;
    const int64_t deadline_us = esp_timer_get_time() +
        (int64_t)DECODE_QUIESCE_TIMEOUT_MS * 1000LL;
    while (esp_timer_get_time() < deadline_us) {
        const decode_worker_state_t state = (decode_worker_state_t)
            atomic_load_explicit(&s_decode_worker_state,
                                 memory_order_acquire);
        if (state == DECODE_WORKER_IDLE) return ESP_OK;
        if (state == DECODE_WORKER_READY || state == DECODE_WORKER_FAILED) {
            atomic_store_explicit(&s_decode_worker_state,
                                  DECODE_WORKER_IDLE,
                                  memory_order_release);
            return ESP_OK;
        }
        vTaskDelay(1);
    }
    ESP_LOGE(TAG,
             "emotion decode worker quiesce timeout state=%d timeout_ms=%u",
             atomic_load_explicit(&s_decode_worker_state,
                                  memory_order_acquire),
             (unsigned)DECODE_QUIESCE_TIMEOUT_MS);
    return ESP_ERR_TIMEOUT;
}

static void decode_worker_request(void)
{
    if (!s_background_decode || !s_decode_task) return;
    int expected = DECODE_WORKER_IDLE;
    if (atomic_compare_exchange_strong_explicit(
            &s_decode_worker_state, &expected, DECODE_WORKER_REQUESTED,
            memory_order_acq_rel, memory_order_acquire)) {
        xTaskNotifyGive(s_decode_task);
    }
}

static uint16_t first_segment_for_entry(
    const nuwax_emotion_asset_entry_t *entry)
{
    if (!entry) return NUWAX_EMOTION_ASSET_INVALID_SEGMENT;
    if (entry->intro_segment != NUWAX_EMOTION_ASSET_INVALID_SEGMENT) {
        return entry->intro_segment;
    }
    if (entry->loop_segment != NUWAX_EMOTION_ASSET_INVALID_SEGMENT) {
        return entry->loop_segment;
    }
    return entry->completion_segment;
}

static uint32_t target_fps_for_entry(
    const nuwax_emotion_asset_entry_t *entry)
{
    return entry ? entry->fps : 0U;
}

static esp_err_t load_asset(nuwax_emotion_asset_id_t asset_id)
{
    ESP_RETURN_ON_ERROR(decode_worker_quiesce(), TAG,
                        "emotion decode worker still owns previous asset");
    (void)atomic_fetch_add_explicit(&s_decode_generation, 1U,
                                    memory_order_acq_rel);
    discard_ready_decode_slots();
    s_next_decode_sequence = 1U;
    s_next_publish_sequence = 1U;
    s_pending_dirty_pixels = 0U;
    s_pending_decode_us = 0U;
    s_decode_worker_error = ESP_OK;
    s_segment_advance_pending = false;
    const nuwax_emotion_asset_entry_t *entry =
        nuwax_emotion_asset_pack_find(asset_id);
    if (!entry) {
        return ESP_ERR_NOT_FOUND;
    }
    if (entry->fps < MIN_ANIMATED_FPS || entry->fps > MAX_ANIMATED_FPS) {
        ESP_LOGE(TAG, "asset frame rate outside runtime contract id=%u fps=%u",
                 (unsigned)asset_id, (unsigned)entry->fps);
        return ESP_ERR_INVALID_VERSION;
    }
    ESP_RETURN_ON_ERROR(nuwax_emotion_asset_pack_read_palette(entry, s_palette),
                        TAG, "emotion palette read failed");
    const uint16_t segment = first_segment_for_entry(entry);
    if (segment == NUWAX_EMOTION_ASSET_INVALID_SEGMENT) {
        return ESP_ERR_INVALID_SIZE;
    }

    s_entry = entry;
    s_asset_id = asset_id;
    s_holding_frame = false;
    s_compare_rgb_on_absolute_frame = true;
    if (s_indices) {
        memset(s_indices, 0, FRAME_PIXELS);
    }
    ESP_RETURN_ON_ERROR(start_segment(segment), TAG,
                        "emotion segment start failed");
    s_asset_started_us = esp_timer_get_time();
    s_next_frame_us = s_asset_started_us;
    s_frames_rendered = 0;
    s_full_frame_updates = 0;
    s_deadline_resyncs = 0;
    s_decode_budget_overruns = 0;
    s_decode_total_us = 0;
    s_decode_max_us = 0;
    s_dirty_pixels_total = 0;
    s_invalidated_pixels_total = 0;
    ESP_LOGI(TAG,
             "asset selected id=%u name=%s fps=%u flags=0x%02x segment=%u",
             (unsigned)asset_id, entry->name, (unsigned)entry->fps,
             (unsigned)entry->flags, (unsigned)segment);
    return ESP_OK;
}

static esp_err_t advance_after_segment(void)
{
    ESP_RETURN_ON_ERROR(finish_segment(), TAG,
                        "emotion segment verification failed");

    if (s_entry->intro_segment == s_decoder.segment_index &&
        s_entry->loop_segment != NUWAX_EMOTION_ASSET_INVALID_SEGMENT) {
        const esp_err_t err = start_segment(s_entry->loop_segment);
        if (err == ESP_OK && nuwax_emotion_asset_pack_abi() ==
                                 NUWAX_EMOTION_ASSET_PACK_ABI_V3) {
            s_v3_bootstrap = V3_BOOTSTRAP_TRANSITION;
        }
        return err;
    }
    if (s_entry->loop_segment == s_decoder.segment_index) {
        const esp_err_t err = start_segment(s_entry->loop_segment);
        if (err == ESP_OK && nuwax_emotion_asset_pack_abi() ==
                                 NUWAX_EMOTION_ASSET_PACK_ABI_V3) {
            s_v3_bootstrap = V3_BOOTSTRAP_WRAP;
        }
        return err;
    }
    if (s_transient_active) {
        const nuwax_emotion_asset_id_t restore = s_return_asset_id;
        s_transient_active = false;
        s_return_asset_id = 0;
        return load_asset(restore ? restore : NUWAX_EMOTION_ASSET_HAPPY);
    }
    const int64_t now_us = esp_timer_get_time();
    const int64_t active_us = now_us - s_asset_started_us;
    const uint32_t active_fps_x10 = active_us > 0
                                        ? (uint32_t)(
                                              (uint64_t)s_frames_rendered *
                                              10000000ULL /
                                              (uint64_t)active_us)
                                        : 0U;
    s_holding_frame = true;
    ESP_LOGI(TAG,
             "asset one-shot complete id=%u frames=%lu active_fps=%lu.%lu holding=1",
             (unsigned)s_asset_id,
             (unsigned long)s_frames_rendered,
             (unsigned long)(active_fps_x10 / 10U),
             (unsigned long)(active_fps_x10 % 10U));
    return ESP_OK;
}

static void log_health_if_due(int64_t now_us)
{
    if (s_last_health_log_us == 0) {
        s_last_health_log_us = now_us;
        s_last_health_frames = s_health_frames_total;
        s_last_health_decode_total_us = s_health_decode_total_us;
        s_last_health_dirty_pixels_total = s_health_dirty_pixels_total;
        s_last_health_invalidated_pixels_total =
            s_health_invalidated_pixels_total;
        s_last_health_full_frame_updates = s_health_full_frame_updates;
        s_last_health_deadline_resyncs = s_health_deadline_resyncs;
        s_last_health_decode_budget_overruns =
            s_health_decode_budget_overruns;
        s_last_health_decode_cooperative_blocks = atomic_load_explicit(
            &s_health_decode_cooperative_blocks, memory_order_relaxed);
        s_last_health_decode_switch_aborts = atomic_load_explicit(
            &s_health_decode_switch_aborts, memory_order_relaxed);
        return;
    }
    const int64_t elapsed_us = now_us - s_last_health_log_us;
    if (elapsed_us < HEALTH_LOG_INTERVAL_US) {
        return;
    }
    const uint64_t frame_delta =
        s_health_frames_total - s_last_health_frames;
    const uint64_t decode_delta =
        s_health_decode_total_us - s_last_health_decode_total_us;
    const uint64_t dirty_delta =
        s_health_dirty_pixels_total - s_last_health_dirty_pixels_total;
    const uint64_t invalidated_delta =
        s_health_invalidated_pixels_total -
        s_last_health_invalidated_pixels_total;
    const uint64_t full_delta =
        s_health_full_frame_updates - s_last_health_full_frame_updates;
    const uint64_t resync_delta =
        s_health_deadline_resyncs - s_last_health_deadline_resyncs;
    const uint64_t budget_overrun_delta =
        s_health_decode_budget_overruns -
        s_last_health_decode_budget_overruns;
    const uint32_t cooperative_blocks = atomic_load_explicit(
        &s_health_decode_cooperative_blocks, memory_order_relaxed);
    const uint32_t switch_aborts = atomic_load_explicit(
        &s_health_decode_switch_aborts, memory_order_relaxed);
    const uint32_t cooperative_block_delta =
        cooperative_blocks -
        s_last_health_decode_cooperative_blocks;
    const uint32_t switch_abort_delta =
        switch_aborts -
        s_last_health_decode_switch_aborts;
    const uint32_t fps_x10 = elapsed_us > 0
                                 ? (uint32_t)(((uint64_t)frame_delta *
                                               10000000ULL) /
                                              (uint64_t)elapsed_us)
                                 : 0U;
    const uint32_t average_changed = frame_delta == 0
                                         ? 0
                                         : (uint32_t)(dirty_delta /
                                                      frame_delta);
    const uint32_t average_invalidated = frame_delta == 0
                                             ? 0
                                             : (uint32_t)(invalidated_delta /
                                                          frame_delta);
    const uint32_t average_decode_us = frame_delta == 0
                                           ? 0
                                           : (uint32_t)(decode_delta /
                                                        frame_delta);
    ESP_LOGI(TAG,
             "health asset=%u asset_frames=%lu window_frames=%llu fps=%lu.%lu target_fps=%u decode_avg_us=%lu decode_max_us=%lu decode_budget_overruns=%llu cooperative_blocks=%llu switch_aborts=%llu avg_changed=%lu avg_invalidated=%lu/%u full=%llu deadline_resyncs=%llu holding=%d paused=%d decode=%s decode_state=%d queue_ready=%lu/%u decode_stack_hw=%u psram_free=%lu",
             (unsigned)s_asset_id,
             (unsigned long)s_frames_rendered,
             (unsigned long long)frame_delta,
             (unsigned long)(fps_x10 / 10U),
             (unsigned long)(fps_x10 % 10U),
             (unsigned)target_fps_for_entry(s_entry),
             (unsigned long)average_decode_us,
             (unsigned long)s_decode_max_us,
             (unsigned long long)budget_overrun_delta,
             (unsigned long long)cooperative_block_delta,
             (unsigned long long)switch_abort_delta,
             (unsigned long)average_changed,
             (unsigned long)average_invalidated,
             (unsigned)FRAME_PIXELS,
             (unsigned long long)full_delta,
             (unsigned long long)resync_delta,
             s_holding_frame ? 1 : 0,
             s_interaction_active ? 1 : 0,
             s_background_decode ? "background" : "synchronous",
              s_background_decode
                  ? atomic_load_explicit(&s_decode_worker_state,
                                         memory_order_acquire)
                  : DECODE_WORKER_IDLE,
              (unsigned long)ready_decode_slot_count(
                  atomic_load_explicit(&s_decode_generation,
                                       memory_order_acquire)),
              (unsigned)s_decode_slot_count,
              s_decode_task
                 ? (unsigned)uxTaskGetStackHighWaterMark(s_decode_task)
                 : 0U,
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    s_last_health_log_us = now_us;
    s_last_health_frames = s_health_frames_total;
    s_last_health_decode_total_us = s_health_decode_total_us;
    s_last_health_dirty_pixels_total = s_health_dirty_pixels_total;
    s_last_health_invalidated_pixels_total =
        s_health_invalidated_pixels_total;
    s_last_health_full_frame_updates = s_health_full_frame_updates;
    s_last_health_deadline_resyncs = s_health_deadline_resyncs;
    s_last_health_decode_budget_overruns = s_health_decode_budget_overruns;
    s_last_health_decode_cooperative_blocks = cooperative_blocks;
    s_last_health_decode_switch_aborts = switch_aborts;
}

esp_err_t nuwax_emotion_animation_player_init(lv_obj_t *parent)
{
    if (!parent) return ESP_ERR_INVALID_ARG;
    if (s_cleanup_pending) {
        const esp_err_t reap_err = nuwax_emotion_animation_player_reap();
        if (reap_err != ESP_OK) {
            return ESP_ERR_INVALID_STATE;
        }
    }
    if (s_ready) return ESP_OK;

    esp_err_t err = nuwax_emotion_asset_pack_init();
    if (err != ESP_OK) {
        return err;
    }
    const uint16_t pack_abi = nuwax_emotion_asset_pack_abi();
    if (pack_abi < NUWAX_EMOTION_ASSET_PACK_ABI_V2 ||
        pack_abi == NUWAX_EMOTION_ASSET_PACK_ABI_V4) {
        s_indices = heap_caps_calloc(FRAME_PIXELS, 1,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    s_rgb565 = heap_caps_calloc(FRAME_PIXELS, sizeof(uint16_t),
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_inflater = allocate_decode_work(sizeof(*s_inflater), true);
    if (pack_abi < NUWAX_EMOTION_ASSET_PACK_ABI_V4) {
        s_dictionary = allocate_decode_work(TINFL_LZ_DICT_SIZE, true);
        s_input = allocate_decode_work(INFLATE_INPUT_BYTES, false);
    }
    if (pack_abi >= NUWAX_EMOTION_ASSET_PACK_ABI_V2) {
        s_decode_slot_bytes =
            pack_abi == NUWAX_EMOTION_ASSET_PACK_ABI_V4
                ? V4_DECODE_SLOT_BYTES : V2_MAX_PUBLISH_RECORD_BYTES;
        s_decode_slots[0].data = heap_caps_malloc(
            s_decode_slot_bytes,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_decode_slots[0].data) {
            s_decode_slot_count = 1U;
            atomic_store_explicit(&s_decode_slots[0].state,
                                  DECODE_SLOT_FREE, memory_order_release);
            const size_t largest = heap_caps_get_largest_free_block(
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (largest >= s_decode_slot_bytes +
                           V2_SECOND_SLOT_PSRAM_RESERVE_BYTES) {
                s_decode_slots[1].data = heap_caps_malloc(
                    s_decode_slot_bytes,
                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (s_decode_slots[1].data) {
                    s_decode_slot_count = 2U;
                    atomic_store_explicit(&s_decode_slots[1].state,
                                          DECODE_SLOT_FREE,
                                          memory_order_release);
                }
            }
        }
    } else if (CONFIG_NUWAX_UI_BACKGROUND_DECODE_ENABLE) {
        s_changed_bits = heap_caps_calloc(
            CHANGED_WORD_COUNT, sizeof(*s_changed_bits),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if ((pack_abi < NUWAX_EMOTION_ASSET_PACK_ABI_V2 && !s_indices) ||
        !s_rgb565 ||
        !s_inflater ||
        (pack_abi < NUWAX_EMOTION_ASSET_PACK_ABI_V4 &&
         (!s_dictionary || !s_input)) ||
        (pack_abi >= NUWAX_EMOTION_ASSET_PACK_ABI_V2 &&
         s_decode_slot_count == 0U)) {
        free_player_buffers();
        nuwax_emotion_asset_pack_deinit();
        return ESP_ERR_NO_MEM;
    }

    /* Reserve the reusable transition workspace while PSRAM is still
     * contiguous. This removes the first cloud/local emotion switch's three
     * heap allocations from the visible transition path. Low-memory systems
     * retain the direct-switch fallback and may retry this reservation later. */
    if (pack_abi == NUWAX_EMOTION_ASSET_PACK_ABI_V4 &&
        !ensure_component_bridge_work()) {
        ESP_LOGW(TAG,
                 "component bridge workspace not prewarmed; transitions retain bounded direct-switch fallback");
    }

    init_image_descriptor();
    s_image = lv_image_create(parent);
    if (!s_image) {
        free_player_buffers();
        nuwax_emotion_asset_pack_deinit();
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_size(s_image, FRAME_WIDTH, FRAME_HEIGHT);
    lv_obj_set_pos(s_image, 0, 0);
    lv_obj_set_style_transform_pivot_x(s_image, FRAME_WIDTH / 2, 0);
    lv_obj_set_style_transform_pivot_y(s_image, FRAME_HEIGHT / 2, 0);
    lv_obj_remove_flag(s_image, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(s_image, &s_image_dsc);
    if (pack_abi == NUWAX_EMOTION_ASSET_PACK_ABI_V4 &&
        !create_frame_patches(parent)) {
        lv_obj_delete(s_image);
        s_image = NULL;
        free_player_buffers();
        nuwax_emotion_asset_pack_deinit();
        return ESP_ERR_NO_MEM;
    }

    clear_decoder();
    s_background_decode = false;
    if (CONFIG_NUWAX_UI_BACKGROUND_DECODE_ENABLE &&
        (s_changed_bits || s_decode_slot_count > 0U)) {
        err = decode_worker_init();
        if (err == ESP_OK) {
            s_background_decode = true;
        } else {
            ESP_LOGW(TAG,
                     "background decode unavailable err=%s; using synchronous fallback",
                     esp_err_to_name(err));
            heap_caps_free(s_changed_bits);
            s_changed_bits = NULL;
        }
    } else if (CONFIG_NUWAX_UI_BACKGROUND_DECODE_ENABLE) {
        ESP_LOGW(TAG,
                 "background decode bitmap allocation failed; using synchronous fallback");
    }
    s_ready = true;
    s_frames_rendered = 0;
    s_full_frame_updates = 0;
    s_deadline_resyncs = 0;
    s_decode_budget_overruns = 0;
    s_dirty_pixels_total = 0;
    s_invalidated_pixels_total = 0;
    s_last_health_log_us = 0;
    s_last_health_frames = 0;
    err = load_asset(NUWAX_EMOTION_ASSET_HAPPY);
    if (err != ESP_OK) {
        (void)nuwax_emotion_animation_player_deinit();
        return err;
    }
    ESP_LOGI(TAG,
             "player ready abi=v%u frame=%u bytes indices=%u dictionary=%u/%s input=%u/%s inflater=%s decode=%s changed_bitmap=%u decode_slots=%u slot_bytes=%u build=%s",
             (unsigned)pack_abi,
             (unsigned)FRAME_RGB_BYTES, (unsigned)FRAME_PIXELS,
             (unsigned)TINFL_LZ_DICT_SIZE,
             esp_ptr_internal(s_dictionary) ? "internal" : "psram",
             (unsigned)INFLATE_INPUT_BYTES,
             esp_ptr_internal(s_input) ? "internal" : "psram",
             esp_ptr_internal(s_inflater) ? "internal" : "psram",
             s_background_decode ? "background" : "synchronous",
             s_changed_bits
                 ? (unsigned)(CHANGED_WORD_COUNT * sizeof(*s_changed_bits))
                 : 0U,
             (unsigned)s_decode_slot_count,
              s_decode_slot_count > 0U ? (unsigned)s_decode_slot_bytes : 0U,
             nuwax_emotion_asset_pack_build_id());
    return ESP_OK;
}

esp_err_t nuwax_emotion_animation_player_deinit(void)
{
    if (s_cleanup_pending) {
        return nuwax_emotion_animation_player_reap();
    }
    const esp_err_t worker_err = decode_worker_stop();
    if (worker_err != ESP_OK) {
        /* The worker owns the decoder and frame work buffers until it reports
         * EXITED. Detach the LVGL image now so a static fallback can render,
         * then let the LVGL owner reap the suspended task asynchronously. */
        delete_frame_patches();
        if (s_image) {
            lv_obj_delete(s_image);
            s_image = NULL;
        }
        s_ready = false;
        s_background_decode = false;
        s_cleanup_pending = true;
        s_cleanup_started_us = esp_timer_get_time();
        s_cleanup_last_log_us = s_cleanup_started_us;
        return worker_err;
    }
    delete_frame_patches();
    if (s_image) {
        lv_obj_delete(s_image);
        s_image = NULL;
    }
    reset_player_state();
    return ESP_OK;
}

esp_err_t nuwax_emotion_animation_player_reap(void)
{
    if (!s_cleanup_pending) return ESP_OK;

    const esp_err_t err = decode_worker_reap();
    if (err != ESP_OK) {
        const int64_t now_us = esp_timer_get_time();
        if (now_us - s_cleanup_last_log_us >= 60000000LL) {
            ESP_LOGE(TAG,
                     "emotion cleanup still quarantined elapsed_ms=%lld state=%d; buffers remain owned by worker",
                     (long long)((now_us - s_cleanup_started_us) / 1000LL),
                     s_decode_task ? (int)eTaskGetState(s_decode_task) : -1);
            s_cleanup_last_log_us = now_us;
        }
        return err;
    }

    reset_player_state();
    ESP_LOGI(TAG, "emotion quarantined worker reaped; resources released");
    return ESP_OK;
}

esp_err_t nuwax_emotion_animation_player_set_asset(
    nuwax_emotion_asset_id_t asset_id)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (asset_id == 0) return ESP_ERR_INVALID_ARG;
    const unsigned queued = atomic_load_explicit(
        &s_queued_asset_id, memory_order_acquire);
    if (!s_transient_active && !s_holding_frame &&
        nuwax_emotion_component_asset_request_is_satisfied(
            asset_id, (nuwax_emotion_asset_id_t)queued, s_asset_id,
            s_component_bridge.active, s_component_bridge.source_asset)) {
        return ESP_OK;
    }
    s_transient_active = false;
    s_return_asset_id = 0;
    atomic_store_explicit(&s_queued_asset_id, (unsigned)asset_id,
                          memory_order_release);
    if (s_background_decode && s_decode_task) {
        /* Wake a requested/idle worker so it can observe the newest-wins
         * switch gate promptly. The LVGL task still owns the actual asset
         * commit and all frame publication. */
        xTaskNotifyGive(s_decode_task);
    }
    return ESP_OK;
}

esp_err_t nuwax_emotion_animation_player_trigger_touch(void)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (s_transient_active) {
        /* A second physical touch restarts the one-shot while preserving the
         * original return asset. The display policy owns the two-second
         * deadline and may cancel the restarted animation earlier. */
        atomic_store_explicit(&s_queued_asset_id,
                              (unsigned)NUWAX_EMOTION_ASSET_TOUCH,
                              memory_order_release);
        if (s_background_decode && s_decode_task) {
            xTaskNotifyGive(s_decode_task);
        }
        return ESP_OK;
    }
    const unsigned queued = atomic_load_explicit(
        &s_queued_asset_id, memory_order_acquire);
    s_return_asset_id = queued ? (nuwax_emotion_asset_id_t)queued
                               : (s_asset_id ? s_asset_id
                                             : NUWAX_EMOTION_ASSET_HAPPY);
    s_transient_active = true;
    atomic_store_explicit(&s_queued_asset_id,
                          (unsigned)NUWAX_EMOTION_ASSET_TOUCH,
                          memory_order_release);
    if (s_background_decode && s_decode_task) {
        xTaskNotifyGive(s_decode_task);
    }
    return ESP_OK;
}

static esp_err_t apply_queued_asset_if_stable(void)
{
    const unsigned queued = atomic_load_explicit(
        &s_queued_asset_id, memory_order_acquire);
    if (queued == 0U || s_interaction_active) return ESP_OK;
    bool restored_bridge_source = false;
    if (s_component_bridge.active) {
        if (queued == (unsigned)s_component_bridge.target_asset) {
            unsigned expected = queued;
            (void)atomic_compare_exchange_strong_explicit(
                &s_queued_asset_id, &expected, 0U,
                memory_order_acq_rel, memory_order_acquire);
            return ESP_OK;
        }
        const int64_t now_us = esp_timer_get_time();
        const component_bridge_t *bridge = &s_component_bridge;
        const uint32_t duration_ms = (uint32_t)bridge->close_ms +
            bridge->reshape_ms + bridge->open_ms;
        const uint32_t elapsed_ms = bridge->visual_elapsed_us / 1000U;
        const bool prefer_target =
            nuwax_emotion_component_bridge_prefers_target(elapsed_ms,
                                                           duration_ms);
        if (prefer_target && bridge->target_ready) {
            if (!commit_component_bridge_target(now_us)) {
                return s_decode_worker_error != ESP_OK
                    ? s_decode_worker_error : ESP_FAIL;
            }
        } else {
            restore_component_bridge_source(now_us);
            restored_bridge_source = true;
        }
    }
    if (restored_bridge_source && queued == (unsigned)s_asset_id) {
        unsigned expected = queued;
        (void)atomic_compare_exchange_strong_explicit(
            &s_queued_asset_id, &expected, 0U,
            memory_order_acq_rel, memory_order_acquire);
        return ESP_OK;
    }
    if (s_background_decode) {
        decode_worker_state_t state = (decode_worker_state_t)
            atomic_load_explicit(&s_decode_worker_state,
                                 memory_order_acquire);
        if (state == DECODE_WORKER_REQUESTED ||
            state == DECODE_WORKER_RUNNING) {
            return ESP_OK;
        }
    }
    const nuwax_emotion_asset_id_t target = (nuwax_emotion_asset_id_t)queued;
    if (begin_component_bridge(target)) {
        unsigned expected = queued;
        (void)atomic_compare_exchange_strong_explicit(
            &s_queued_asset_id, &expected, 0U,
            memory_order_acq_rel, memory_order_acquire);
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(load_asset(target), TAG,
                        "queued emotion asset load failed");
    unsigned expected = queued;
    (void)atomic_compare_exchange_strong_explicit(
        &s_queued_asset_id, &expected, 0U,
        memory_order_acq_rel, memory_order_acquire);
    return ESP_OK;
}

esp_err_t nuwax_emotion_animation_player_tick(void)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    const int64_t now_us = esp_timer_get_time();
    log_health_if_due(now_us);
    ESP_RETURN_ON_ERROR(apply_queued_asset_if_stable(), TAG,
                        "emotion asset switch failed");
    if (s_interaction_active) {
        return ESP_OK;
    }
    if (s_component_bridge.active) {
        (void)tick_component_bridge(now_us);
        return s_decode_worker_error;
    }
    if (s_holding_frame) {
        return ESP_OK;
    }

    const uint32_t target_fps = target_fps_for_entry(s_entry);
    if (target_fps < MIN_ANIMATED_FPS || target_fps > MAX_ANIMATED_FPS) {
        return ESP_ERR_INVALID_VERSION;
    }
    const uint32_t frame_period_us = 1000000U / target_fps;

    if (s_background_decode) {
        const decode_worker_state_t state = (decode_worker_state_t)
            atomic_load_explicit(&s_decode_worker_state,
                                 memory_order_acquire);
        if (state == DECODE_WORKER_FAILED) {
            const esp_err_t err = s_decode_worker_error;
            atomic_store_explicit(&s_decode_worker_state,
                                  DECODE_WORKER_IDLE,
                                  memory_order_release);
            return err;
        }
        if (s_segment_advance_pending) {
            if (state == DECODE_WORKER_REQUESTED ||
                state == DECODE_WORKER_RUNNING) {
                return ESP_OK;
            }
            if (state == DECODE_WORKER_READY) {
                atomic_store_explicit(&s_decode_worker_state,
                                      DECODE_WORKER_IDLE,
                                      memory_order_release);
            }
            ESP_RETURN_ON_ERROR(advance_after_segment(), TAG,
                                "emotion segment advance failed");
            s_segment_advance_pending = false;
            if (!s_holding_frame && !s_interaction_active) {
                decode_worker_request();
            }
            return ESP_OK;
        }
        if (now_us < s_next_frame_us) {
            return ESP_OK;
        }
        decode_frame_slot_t *slot = NULL;
        if (nuwax_emotion_asset_pack_abi() >=
            NUWAX_EMOTION_ASSET_PACK_ABI_V2) {
            slot = find_ready_decode_slot(
                atomic_load_explicit(&s_decode_generation,
                                     memory_order_acquire),
                s_next_publish_sequence);
            if (!slot) {
                if (state == DECODE_WORKER_IDLE ||
                    state == DECODE_WORKER_READY) {
                    atomic_store_explicit(&s_decode_worker_state,
                                          DECODE_WORKER_IDLE,
                                          memory_order_release);
                    decode_worker_request();
                }
                return ESP_OK;
            }
        } else {
            if (state == DECODE_WORKER_IDLE) {
                decode_worker_request();
                return ESP_OK;
            }
            if (state != DECODE_WORKER_READY) {
                return ESP_OK;
            }
        }
        publish_decoded_frame(slot);
        if (s_decode_worker_error != ESP_OK) {
            release_decode_slot(slot);
            return s_decode_worker_error;
        }
        const bool segment_complete = slot &&
            slot->source_frame_index + 1U == slot->source_frame_count;
        if (slot) {
            s_next_publish_sequence++;
            release_decode_slot(slot);
        }
        if (state == DECODE_WORKER_READY) {
            atomic_store_explicit(&s_decode_worker_state, DECODE_WORKER_IDLE,
                                  memory_order_release);
        }
        int64_t next_frame_us = s_next_frame_us + frame_period_us;
        const int64_t published_us = esp_timer_get_time();
        if (next_frame_us <= published_us) {
            s_deadline_resyncs++;
            s_health_deadline_resyncs++;
            const int64_t missed =
                (published_us - next_frame_us) / frame_period_us + 1LL;
            next_frame_us += missed * frame_period_us;
        }
        s_next_frame_us = next_frame_us;
        if (segment_complete ||
            (!slot && s_decoder.frame_index >= s_decoder.segment->frame_count)) {
            s_segment_advance_pending = true;
        }
        if (!s_segment_advance_pending && !s_holding_frame &&
            !s_interaction_active) {
            decode_worker_request();
        }
        return ESP_OK;
    }

    if (now_us < s_next_frame_us) {
        return ESP_OK;
    }
    const int64_t decode_started_us = esp_timer_get_time();
    decode_frame_slot_t *slot = NULL;
    if (nuwax_emotion_asset_pack_abi() >=
        NUWAX_EMOTION_ASSET_PACK_ABI_V2) {
        slot = find_free_decode_slot();
        if (!slot) return ESP_ERR_INVALID_STATE;
        slot->generation = atomic_load_explicit(&s_decode_generation,
                                                memory_order_acquire);
        slot->sequence = s_next_decode_sequence++;
    }
    const esp_err_t decode_err = prepare_next_frame(slot);
    const int64_t decode_finished_us = esp_timer_get_time();
    const uint32_t decode_us = (uint32_t)(decode_finished_us - decode_started_us);
    if (slot) slot->decode_us = decode_us;
    else s_pending_decode_us = decode_us;
    if (decode_err != ESP_OK) {
        release_decode_slot(slot);
        return decode_err;
    }
    publish_decoded_frame(slot);
    if (s_decode_worker_error != ESP_OK) {
        release_decode_slot(slot);
        return s_decode_worker_error;
    }
    const bool segment_complete = slot &&
        slot->source_frame_index + 1U == slot->source_frame_count;
    if (slot) {
        s_next_publish_sequence++;
        release_decode_slot(slot);
    }
    int64_t next_frame_us = s_next_frame_us + frame_period_us;
    if (next_frame_us <= decode_finished_us) {
        s_deadline_resyncs++;
        s_health_deadline_resyncs++;
        const int64_t missed =
            (decode_finished_us - next_frame_us) / frame_period_us + 1LL;
        next_frame_us += missed * frame_period_us;
    }
    s_next_frame_us = next_frame_us;
    if (segment_complete ||
        (!slot && s_decoder.frame_index >= s_decoder.segment->frame_count)) {
        ESP_RETURN_ON_ERROR(advance_after_segment(), TAG,
                            "emotion segment advance failed");
    }
    return ESP_OK;
}

void nuwax_emotion_animation_player_set_interaction_active(bool active)
{
    if (!s_ready) return;
    if (active) {
        if (s_component_bridge.active &&
            s_component_bridge.pause_started_us == 0) {
            s_component_bridge.pause_started_us = esp_timer_get_time();
        }
        (void)nuwax_emotion_animation_player_freeze();
        return;
    }
    if (!s_interaction_active) return;
    s_interaction_active = false;
    const int64_t resumed_us = esp_timer_get_time();
    if (s_component_bridge.active &&
        s_component_bridge.pause_started_us > 0) {
        const int64_t paused_us = resumed_us -
            s_component_bridge.pause_started_us;
        s_component_bridge.started_us += paused_us;
        s_component_bridge.next_frame_us += paused_us;
        s_component_bridge.pause_started_us = 0;
    }
    s_next_frame_us = resumed_us;
}

esp_err_t nuwax_emotion_animation_player_freeze(void)
{
    if (!s_ready || !s_image || !s_rgb565) return ESP_ERR_INVALID_STATE;
    s_interaction_active = true;
    /* The worker only owns compressed decoder state and the publish patch; it
     * never mutates the visible RGB frame. Freezing therefore has no reason to
     * stall the LVGL owner while a low-priority decode finishes. */
    return ESP_OK;
}

void nuwax_emotion_animation_player_set_overlay_progress(
    uint16_t progress_permille)
{
    if (!s_ready || !s_image) return;
    /* Keep the paused frame resident under the overlay. Toggling HIDDEN on a
     * 360x360 image invalidates the complete display; incremental backdrop
     * bands instead redraw only the pixels that become visible or covered. */
    (void)progress_permille;
}

bool nuwax_emotion_animation_player_frozen_frame_ready(void)
{
    return s_ready && s_interaction_active && s_image && s_rgb565;
}

const uint16_t *nuwax_emotion_animation_player_frozen_rgb565(
    size_t *stride_pixels)
{
    if (stride_pixels) *stride_pixels = 0U;
    if (!nuwax_emotion_animation_player_frozen_frame_ready()) return NULL;
    if (stride_pixels) *stride_pixels = FRAME_WIDTH;
    return s_rgb565;
}

bool nuwax_emotion_animation_player_draw_frozen(void *layer_ptr)
{
    lv_layer_t *layer = (lv_layer_t *)layer_ptr;
    if (!layer || !nuwax_emotion_animation_player_frozen_frame_ready()) {
        return false;
    }
    lv_area_t area;
    lv_obj_get_coords(s_image, &area);
    lv_draw_image_dsc_t draw;
    lv_draw_image_dsc_init(&draw);
    draw.base.layer = layer;
    draw.src = &s_image_dsc;
    draw.opa = LV_OPA_COVER;
    draw.scale_x = LV_SCALE_NONE;
    draw.scale_y = LV_SCALE_NONE;
    draw.image_area = area;
    lv_draw_image(layer, &draw, &area);
    return true;
}

bool nuwax_emotion_animation_player_ready(void)
{
    return s_ready;
}
