#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define NUWAX_EMOTION_ASSET_INVALID_SEGMENT UINT16_MAX
#define NUWAX_EMOTION_ASSET_MAX_ENTRIES 16U
#define NUWAX_EMOTION_ASSET_MAX_SEGMENTS 32U
#define NUWAX_EMOTION_ASSET_PALETTE_COLORS 64U

enum {
    NUWAX_EMOTION_ASSET_PACK_ABI_V1 = 1U,
    NUWAX_EMOTION_ASSET_PACK_ABI_V2 = 2U,
    NUWAX_EMOTION_ASSET_PACK_ABI_V3 = 3U,
    NUWAX_EMOTION_ASSET_PACK_ABI_V4 = 4U,
};

enum {
    NUWAX_EMOTION_ASSET_FLAG_PERSISTENT = 1U << 0,
    NUWAX_EMOTION_ASSET_FLAG_TRANSIENT = 1U << 1,
    NUWAX_EMOTION_ASSET_FLAG_COMPLETION = 1U << 2,
};

typedef enum {
    NUWAX_EMOTION_ASSET_DIZZINESS = 1,
    NUWAX_EMOTION_ASSET_HAPPY,
    NUWAX_EMOTION_ASSET_EXCITED,
    NUWAX_EMOTION_ASSET_SLEEPING,
    NUWAX_EMOTION_ASSET_PROVISIONING,
    NUWAX_EMOTION_ASSET_DISCONNECTED,
    NUWAX_EMOTION_ASSET_THINKING,
    NUWAX_EMOTION_ASSET_THOUGHT_COMPLETE,
    NUWAX_EMOTION_ASSET_MEETING,
    NUWAX_EMOTION_ASSET_MONITORING,
    NUWAX_EMOTION_ASSET_TOUCH,
    NUWAX_EMOTION_ASSET_PLAYFUL,
    NUWAX_EMOTION_ASSET_LIKE,
    NUWAX_EMOTION_ASSET_ANGRY,
    NUWAX_EMOTION_ASSET_LISTENING,
    NUWAX_EMOTION_ASSET_QUESTION,
} nuwax_emotion_asset_id_t;

typedef struct {
    uint16_t asset_id;
    uint16_t flags;
    uint16_t fps;
    uint16_t palette_count;
    uint16_t intro_segment;
    uint16_t loop_segment;
    uint16_t completion_segment;
    uint32_t palette_offset;
    char name[12];
} nuwax_emotion_asset_entry_t;

typedef struct {
    uint32_t offset;
    uint32_t compressed_size;
    uint32_t raw_size;
    uint32_t raw_crc32;
    uint16_t frame_count;
    uint16_t kind;
} nuwax_emotion_asset_segment_t;

enum {
    NUWAX_EMOTION_ASSET_V3_FLAG_KEY_RLE = 1U << 0,
    NUWAX_EMOTION_ASSET_V3_FLAG_TAIL_ZLIB = 1U << 1,
    NUWAX_EMOTION_ASSET_V3_FLAG_TRANSITION = 1U << 2,
    NUWAX_EMOTION_ASSET_V3_FLAG_WRAP = 1U << 3,
};

typedef struct {
    uint16_t flags;
    uint32_t key_offset;
    uint32_t key_size;
    uint32_t key_crc32;
    uint32_t tail_offset;
    uint32_t tail_size;
    uint32_t tail_raw_size;
    uint32_t tail_raw_crc32;
    uint32_t transition_offset;
    uint32_t transition_size;
    uint32_t transition_crc32;
    uint32_t wrap_offset;
    uint32_t wrap_size;
    uint32_t wrap_crc32;
} nuwax_emotion_asset_v3_segment_t;

#define NUWAX_EMOTION_ASSET_V4_MAX_REGIONS 5U

enum {
    NUWAX_EMOTION_ASSET_V4_FRAME_FLAG_ABSOLUTE = 1U << 0,
    NUWAX_EMOTION_ASSET_V4_FRAME_FLAG_RLE = 1U << 1,
};

typedef struct {
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
} nuwax_emotion_asset_region_t;

typedef struct {
    uint32_t records_offset;
    uint32_t offset_table_offset;
    uint32_t max_record_size;
    uint32_t payload_crc32;
    bool regional_payload;
    bool rle_records;
    uint16_t handoff_frame_index;
    uint32_t handoff_record_offset;
    uint32_t handoff_record_size;
} nuwax_emotion_asset_v4_segment_t;

typedef struct {
    uint16_t compressed_size;
    bool absolute;
    uint8_t region_count;
    uint32_t changed_count;
    uint32_t raw_size;
    uint32_t raw_crc32;
    uint32_t compressed_offset;
    bool regional_payload;
    bool rle;
    nuwax_emotion_asset_region_t regions[NUWAX_EMOTION_ASSET_V4_MAX_REGIONS];
} nuwax_emotion_asset_v4_frame_t;

esp_err_t nuwax_emotion_asset_pack_init(void);
void nuwax_emotion_asset_pack_deinit(void);
bool nuwax_emotion_asset_pack_ready(void);
uint16_t nuwax_emotion_asset_pack_abi(void);
const char *nuwax_emotion_asset_pack_build_id(void);
size_t nuwax_emotion_asset_pack_size(void);

const nuwax_emotion_asset_entry_t *nuwax_emotion_asset_pack_find(
    nuwax_emotion_asset_id_t asset_id);
const nuwax_emotion_asset_segment_t *nuwax_emotion_asset_pack_segment(
    uint16_t segment_index);
const nuwax_emotion_asset_v3_segment_t *nuwax_emotion_asset_pack_v3_segment(
    uint16_t segment_index);
const nuwax_emotion_asset_v4_segment_t *nuwax_emotion_asset_pack_v4_segment(
    uint16_t segment_index);
esp_err_t nuwax_emotion_asset_pack_v4_frame(
    uint16_t segment_index, uint16_t frame_index,
    nuwax_emotion_asset_v4_frame_t *frame_out);
esp_err_t nuwax_emotion_asset_pack_v4_handoff_frame(
    uint16_t segment_index, uint16_t frame_index,
    nuwax_emotion_asset_v4_frame_t *frame_out);
esp_err_t nuwax_emotion_asset_pack_read(uint32_t offset, void *buffer, size_t size);
esp_err_t nuwax_emotion_asset_pack_read_palette(
    const nuwax_emotion_asset_entry_t *entry,
    uint16_t palette[NUWAX_EMOTION_ASSET_PALETTE_COLORS]);
