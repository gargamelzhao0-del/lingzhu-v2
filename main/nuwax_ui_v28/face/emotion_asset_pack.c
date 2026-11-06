#include "emotion_asset_pack.h"

#include "emotion_segment_policy.h"

#include <string.h>

#include "esp_check.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "psa/crypto.h"

static const char *TAG = "emotion_pack";

#define PACK_VERSION_V1 NUWAX_EMOTION_ASSET_PACK_ABI_V1
#define PACK_VERSION_V2 NUWAX_EMOTION_ASSET_PACK_ABI_V2
#define PACK_VERSION_V3 NUWAX_EMOTION_ASSET_PACK_ABI_V3
#define PACK_VERSION_V4 NUWAX_EMOTION_ASSET_PACK_ABI_V4
#define PACK_WIDTH 360U
#define PACK_HEIGHT 360U
#define PACK_FRAME_PIXELS (PACK_WIDTH * PACK_HEIGHT)
#define PACK_V2_ABSOLUTE_RAW_SIZE (PACK_FRAME_PIXELS + 20U)
#define PACK_V3_MAX_KEY_BYTES 32768U
#define PACK_V3_MAX_PATCH_BYTES 32768U
#define PACK_V4_MAX_RECORD_BYTES 12352U
#define PACK_MIN_ANIMATED_FPS 40U
#define PACK_EXPECTED_SEGMENTS 30U
#define PACK_RELEASE_TOTAL_SIZE 2891300U
#define PACK_RELEASE_PAYLOAD_CRC32 0x78888ec3U
#define PACK_RELEASE_CATALOG_CRC32 0x740d9d26U
#define PACK_SUPPORTED_FLAGS                                                   \
    (NUWAX_EMOTION_ASSET_FLAG_PERSISTENT |                                    \
     NUWAX_EMOTION_ASSET_FLAG_TRANSIENT |                                     \
     NUWAX_EMOTION_ASSET_FLAG_COMPLETION)
#define PACK_PARTITION_LABEL "emotion_0"
#define PACK_PARTITION_SUBTYPE 0x40U
#define PACK_PARTITION_ADDRESS 0xB00000U
#define PACK_PARTITION_SIZE 0x400000U
#define PACK_REQUIRED_FLASH_SIZE 0x1000000U
#define CRC_CHUNK_BYTES 4096U
#define SHA256_HEX_BYTES 64U

#ifndef NUWAX_EMOTION_RELEASE_SHA256
#define NUWAX_EMOTION_RELEASE_SHA256 \
    "c8e9258f5963f6b08089ef80af6190ba1053e3e73dfc990d529d090a6f752897"
#endif
#ifndef NUWAX_EMOTION_COMPATIBLE_SHA256_CSV
#define NUWAX_EMOTION_COMPATIBLE_SHA256_CSV NUWAX_EMOTION_RELEASE_SHA256
#endif

enum {
    PACK_SEGMENT_INTRO = 0,
    PACK_SEGMENT_LOOP = 1,
    PACK_SEGMENT_TRANSIENT = 2,
    PACK_SEGMENT_COMPLETION = 3,
};

typedef struct __attribute__((packed)) {
    uint8_t magic[8];
    uint16_t version;
    uint16_t header_size;
    uint16_t width;
    uint16_t height;
    uint16_t entry_count;
    uint16_t segment_count;
    uint32_t entry_offset;
    uint32_t segment_offset;
    uint32_t palette_offset;
    uint32_t data_offset;
    uint32_t total_size;
    uint32_t payload_crc32;
    uint32_t catalog_crc32;
    char build_id[16];
} pack_header_t;

typedef struct __attribute__((packed)) {
    uint16_t asset_id;
    uint16_t flags;
    uint16_t fps;
    uint16_t palette_count;
    uint16_t intro_segment;
    uint16_t loop_segment;
    uint16_t completion_segment;
    uint16_t reserved;
    uint32_t palette_offset;
    char name[12];
} pack_entry_t;

typedef struct __attribute__((packed)) {
    uint32_t offset;
    uint32_t compressed_size;
    uint32_t raw_size;
    uint32_t raw_crc32;
    uint16_t frame_count;
    uint16_t kind;
} pack_segment_t;

typedef struct __attribute__((packed)) {
    uint8_t magic[4];
    uint16_t header_size;
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
    uint32_t payload_crc32;
} pack_v3_segment_header_t;

typedef struct __attribute__((packed)) {
    uint8_t magic[4];
    uint16_t header_size;
    uint16_t flags;
    uint16_t frame_count;
    uint16_t max_regions;
    uint32_t offset_table_offset;
    uint32_t records_offset;
    uint32_t max_record_size;
    uint32_t payload_crc32;
    uint32_t reserved;
} pack_v4_segment_header_t;

typedef struct __attribute__((packed)) {
    uint16_t compressed_size;
    uint8_t flags;
    uint8_t region_count;
    uint32_t changed_count;
    uint32_t raw_crc32;
} pack_v4_frame_header_t;

typedef struct __attribute__((packed)) {
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
} pack_v4_region_t;

typedef struct {
    const char *name;
    uint16_t flags;
    uint16_t fps;
} asset_contract_t;

_Static_assert(sizeof(pack_header_t) == 64, "emotion pack header layout");
_Static_assert(sizeof(pack_entry_t) == 32, "emotion pack entry layout");
_Static_assert(sizeof(pack_segment_t) == 20, "emotion pack segment layout");
_Static_assert(sizeof(pack_v3_segment_header_t) == 64,
               "emotion pack v3 segment header layout");
_Static_assert(sizeof(pack_v4_segment_header_t) == 32,
               "emotion pack v4 segment header layout");
_Static_assert(sizeof(pack_v4_frame_header_t) == 12,
               "emotion pack v4 frame header layout");
_Static_assert(sizeof(pack_v4_region_t) == 8,
               "emotion pack v4 region layout");
_Static_assert(NUWAX_EMOTION_ASSET_MAX_ENTRIES == 16U,
               "production emotion catalog size");

static const uint8_t PACK_MAGIC_V1[8] = {'N', 'W', 'E', 'M', 'O', '1', 0, 0};
static const uint8_t PACK_MAGIC_V2[8] = {'N', 'W', 'E', 'M', 'O', '2', 0, 0};
static const uint8_t PACK_MAGIC_V3[8] = {'N', 'W', 'E', 'M', 'O', '3', 0, 0};
static const uint8_t PACK_MAGIC_V4[8] = {'N', 'W', 'E', 'M', 'O', '4', 0, 0};
static const uint8_t PACK_V3_SEGMENT_MAGIC[4] = {'S', 'G', 'V', '3'};
static const uint8_t PACK_V4_SEGMENT_MAGIC[4] = {'S', 'G', 'V', '4'};
#define PACK_V4_SEGMENT_FLAG_INDEPENDENT_ZLIB (1U << 0)
#define PACK_V4_SEGMENT_FLAG_REGIONAL_PAYLOAD (1U << 1)
#define PACK_V4_SEGMENT_FLAG_RLE_RECORDS (1U << 2)
#define PACK_V4_SEGMENT_FLAG_HANDOFF_RECORD (1U << 3)
#define PACK_V4_SEGMENT_SUPPORTED_FLAGS                                      \
    (PACK_V4_SEGMENT_FLAG_INDEPENDENT_ZLIB |                                \
     PACK_V4_SEGMENT_FLAG_REGIONAL_PAYLOAD |                                \
     PACK_V4_SEGMENT_FLAG_RLE_RECORDS |                                     \
     PACK_V4_SEGMENT_FLAG_HANDOFF_RECORD)
#define PACK_V4_HANDOFF_OFFSET_MASK 0x00ffffffU
#define PACK_V4_HANDOFF_FRAME_SHIFT 24U
#define PACK_V3_SUPPORTED_FLAGS                                               \
    (NUWAX_EMOTION_ASSET_V3_FLAG_KEY_RLE |                                  \
     NUWAX_EMOTION_ASSET_V3_FLAG_TAIL_ZLIB |                                \
     NUWAX_EMOTION_ASSET_V3_FLAG_TRANSITION |                               \
     NUWAX_EMOTION_ASSET_V3_FLAG_WRAP)
static const asset_contract_t PACK_ASSETS[NUWAX_EMOTION_ASSET_MAX_ENTRIES] = {
    {"dizziness", NUWAX_EMOTION_ASSET_FLAG_PERSISTENT, 40},
    {"happy", NUWAX_EMOTION_ASSET_FLAG_PERSISTENT, 40},
    {"excited", NUWAX_EMOTION_ASSET_FLAG_PERSISTENT, 40},
    {"sleeping", NUWAX_EMOTION_ASSET_FLAG_PERSISTENT, 40},
    {"provisioning", NUWAX_EMOTION_ASSET_FLAG_PERSISTENT, 40},
    {"disconnected", NUWAX_EMOTION_ASSET_FLAG_PERSISTENT, 40},
    {"thinking", NUWAX_EMOTION_ASSET_FLAG_PERSISTENT, 40},
    {"thought_complete", NUWAX_EMOTION_ASSET_FLAG_COMPLETION, 40},
    {"meeting", NUWAX_EMOTION_ASSET_FLAG_PERSISTENT, 40},
    {"monitoring", NUWAX_EMOTION_ASSET_FLAG_PERSISTENT, 40},
    {"touch", NUWAX_EMOTION_ASSET_FLAG_TRANSIENT, 40},
    {"playful", NUWAX_EMOTION_ASSET_FLAG_PERSISTENT, 40},
    {"like", NUWAX_EMOTION_ASSET_FLAG_PERSISTENT, 40},
    {"angry", NUWAX_EMOTION_ASSET_FLAG_PERSISTENT, 40},
    {"listening", NUWAX_EMOTION_ASSET_FLAG_PERSISTENT, 40},
    {"question", NUWAX_EMOTION_ASSET_FLAG_PERSISTENT, 40},
};

static const esp_partition_t *s_partition;
static const uint8_t *s_mapped_pack;
static esp_partition_mmap_handle_t s_mapped_pack_handle;
static pack_header_t s_header;
static nuwax_emotion_asset_entry_t s_entries[NUWAX_EMOTION_ASSET_MAX_ENTRIES];
static nuwax_emotion_asset_segment_t s_segments[NUWAX_EMOTION_ASSET_MAX_SEGMENTS];
static nuwax_emotion_asset_v3_segment_t
    s_v3_segments[NUWAX_EMOTION_ASSET_MAX_SEGMENTS];
static nuwax_emotion_asset_v4_segment_t
    s_v4_segments[NUWAX_EMOTION_ASSET_MAX_SEGMENTS];
static bool s_ready;
static bool s_release_match;

static char hex_digit(uint8_t value)
{
    return (char)(value < 10U ? '0' + value : 'a' + (value - 10U));
}

static void sha256_to_hex(const uint8_t digest[32],
                          char hex[SHA256_HEX_BYTES + 1U])
{
    for (size_t i = 0; i < 32U; ++i) {
        hex[i * 2U] = hex_digit((uint8_t)(digest[i] >> 4U));
        hex[i * 2U + 1U] = hex_digit((uint8_t)(digest[i] & 0x0fU));
    }
    hex[SHA256_HEX_BYTES] = '\0';
}

static bool compatible_sha256(const char digest_hex[SHA256_HEX_BYTES + 1U])
{
    const char *cursor = NUWAX_EMOTION_COMPATIBLE_SHA256_CSV;
    while (*cursor != '\0') {
        const char *separator = strchr(cursor, ',');
        const size_t length = separator ? (size_t)(separator - cursor)
                                        : strlen(cursor);
        if (length == SHA256_HEX_BYTES &&
            memcmp(cursor, digest_hex, SHA256_HEX_BYTES) == 0) {
            return true;
        }
        if (!separator) break;
        cursor = separator + 1;
    }
    return false;
}

static uint32_t align4(uint32_t value)
{
    return (value + 3U) & ~3U;
}

static bool range_valid(uint32_t offset, uint32_t size, uint32_t limit)
{
    return offset <= limit && size <= limit - offset;
}

static bool segment_index_valid(uint16_t index)
{
    return index == NUWAX_EMOTION_ASSET_INVALID_SEGMENT ||
           index < s_header.segment_count;
}

static void reset_pack_state(void)
{
    if (s_mapped_pack) {
        esp_partition_munmap(s_mapped_pack_handle);
    }
    s_mapped_pack = NULL;
    s_mapped_pack_handle = 0U;
    s_ready = false;
    s_release_match = false;
    s_partition = NULL;
    memset(&s_header, 0, sizeof(s_header));
    memset(s_entries, 0, sizeof(s_entries));
    memset(s_segments, 0, sizeof(s_segments));
    memset(s_v3_segments, 0, sizeof(s_v3_segments));
    memset(s_v4_segments, 0, sizeof(s_v4_segments));
}

static esp_err_t read_pack_bytes(uint32_t offset, void *buffer, size_t size)
{
    if (!s_partition || !buffer || size == 0U || size > UINT32_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t limit = s_header.total_size != 0U
        ? s_header.total_size : s_partition->size;
    if (limit > s_partition->size ||
        !range_valid(offset, (uint32_t)size, limit)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_mapped_pack) {
        memcpy(buffer, s_mapped_pack + offset, size);
        return ESP_OK;
    }
    return esp_partition_read(s_partition, offset, buffer, size);
}

static esp_err_t validate_v4_segment(uint16_t segment_index,
                                     const pack_segment_t *source)
{
    pack_v4_segment_header_t header;
    if (!source || source->compressed_size < sizeof(header)) {
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_RETURN_ON_ERROR(
        esp_partition_read(s_partition, source->offset, &header, sizeof(header)),
        TAG, "emotion v4 segment header read failed");
    const uint32_t table_bytes =
        ((uint32_t)source->frame_count + 1U) * sizeof(uint32_t);
    if (memcmp(header.magic, PACK_V4_SEGMENT_MAGIC,
               sizeof(PACK_V4_SEGMENT_MAGIC)) != 0 ||
        header.header_size != sizeof(header) ||
        (header.flags & PACK_V4_SEGMENT_FLAG_INDEPENDENT_ZLIB) == 0U ||
        (header.flags & ~PACK_V4_SEGMENT_SUPPORTED_FLAGS) != 0U ||
        header.frame_count != source->frame_count ||
        header.max_regions != NUWAX_EMOTION_ASSET_V4_MAX_REGIONS ||
        header.offset_table_offset != sizeof(header) ||
        header.records_offset != sizeof(header) + table_bytes ||
        header.max_record_size < sizeof(pack_v4_frame_header_t) +
                                     NUWAX_EMOTION_ASSET_V4_MAX_REGIONS *
                                         sizeof(pack_v4_region_t) ||
        header.max_record_size > PACK_V4_MAX_RECORD_BYTES ||
        header.records_offset > source->compressed_size ||
        header.payload_crc32 == 0U) {
        ESP_LOGE(TAG, "emotion v4 segment contract mismatch index=%u",
                 (unsigned)segment_index);
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t boundaries[2] = {0};
    ESP_RETURN_ON_ERROR(
        esp_partition_read(s_partition,
                           source->offset + header.offset_table_offset,
                           &boundaries[0], sizeof(uint32_t)),
        TAG, "emotion v4 first frame offset read failed");
    ESP_RETURN_ON_ERROR(
        esp_partition_read(s_partition,
                           source->offset + header.offset_table_offset +
                               (uint32_t)source->frame_count * sizeof(uint32_t),
                           &boundaries[1], sizeof(uint32_t)),
        TAG, "emotion v4 last frame offset read failed");
    const bool has_handoff =
        (header.flags & PACK_V4_SEGMENT_FLAG_HANDOFF_RECORD) != 0U;
    const uint16_t handoff_frame_index =
        (uint16_t)(header.reserved >> PACK_V4_HANDOFF_FRAME_SHIFT);
    const uint32_t handoff_relative_offset =
        header.reserved & PACK_V4_HANDOFF_OFFSET_MASK;
    const uint32_t regular_records_end = header.records_offset + boundaries[1];
    uint32_t handoff_record_size = 0U;
    if (boundaries[0] != 0U || regular_records_end > source->compressed_size ||
        (!has_handoff &&
         (header.reserved != 0U || regular_records_end != source->compressed_size))) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (has_handoff) {
        pack_v4_frame_header_t handoff_header;
        const uint32_t fixed_size = sizeof(pack_v4_frame_header_t) +
            NUWAX_EMOTION_ASSET_V4_MAX_REGIONS * sizeof(pack_v4_region_t);
        if ((header.flags & PACK_V4_SEGMENT_FLAG_RLE_RECORDS) == 0U ||
            handoff_frame_index == 0U ||
            handoff_frame_index >= source->frame_count ||
            handoff_relative_offset != regular_records_end ||
            !range_valid(handoff_relative_offset, fixed_size,
                         source->compressed_size)) {
            return ESP_ERR_INVALID_SIZE;
        }
        ESP_RETURN_ON_ERROR(
            esp_partition_read(s_partition,
                               source->offset + handoff_relative_offset,
                               &handoff_header, sizeof(handoff_header)),
            TAG, "emotion v4 handoff header read failed");
        handoff_record_size = fixed_size + handoff_header.compressed_size;
        if (handoff_header.flags !=
                (NUWAX_EMOTION_ASSET_V4_FRAME_FLAG_ABSOLUTE |
                 NUWAX_EMOTION_ASSET_V4_FRAME_FLAG_RLE) ||
            handoff_record_size !=
                source->compressed_size - handoff_relative_offset ||
            handoff_record_size > header.max_record_size) {
            return ESP_ERR_INVALID_SIZE;
        }
    }
    s_v4_segments[segment_index] = (nuwax_emotion_asset_v4_segment_t){
        .records_offset = source->offset + header.records_offset,
        .offset_table_offset = source->offset + header.offset_table_offset,
        .max_record_size = header.max_record_size,
        .payload_crc32 = header.payload_crc32,
        .regional_payload =
            (header.flags & PACK_V4_SEGMENT_FLAG_REGIONAL_PAYLOAD) != 0U,
        .rle_records =
            (header.flags & PACK_V4_SEGMENT_FLAG_RLE_RECORDS) != 0U,
        .handoff_frame_index = has_handoff ? handoff_frame_index : UINT16_MAX,
        .handoff_record_offset = has_handoff
            ? source->offset + handoff_relative_offset : 0U,
        .handoff_record_size = handoff_record_size,
    };
    return ESP_OK;
}

/* Read the emotion partition once and update both integrity primitives from
 * each chunk. The previous implementation scanned the 2.9 MiB partition
 * twice (CRC, then SHA-256), adding avoidable startup latency. */
static esp_err_t validate_pack_integrity(void)
{
    const psa_status_t init_status = psa_crypto_init();
    if (init_status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "PSA crypto initialization failed status=%ld",
                 (long)init_status);
        return ESP_FAIL;
    }
    uint8_t *chunk = heap_caps_malloc(CRC_CHUNK_BYTES,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!chunk) return ESP_ERR_NO_MEM;

    psa_hash_operation_t operation = PSA_HASH_OPERATION_INIT;
    psa_status_t status = psa_hash_setup(&operation, PSA_ALG_SHA_256);
    uint32_t crc = 0U;
    uint32_t offset = 0;
    while (status == PSA_SUCCESS && offset < s_header.total_size) {
        const uint32_t remaining = s_header.total_size - offset;
        const uint32_t bytes = remaining < CRC_CHUNK_BYTES
                                   ? remaining
                                   : CRC_CHUNK_BYTES;
        const esp_err_t read_err = esp_partition_read(s_partition, offset,
                                                      chunk, bytes);
        if (read_err != ESP_OK) {
            (void)psa_hash_abort(&operation);
            heap_caps_free(chunk);
            return read_err;
        }
        status = psa_hash_update(&operation, chunk, bytes);
        if (status == PSA_SUCCESS &&
            offset + bytes > s_header.header_size) {
            const uint32_t crc_skip =
                offset < s_header.header_size
                    ? s_header.header_size - offset
                    : 0U;
            crc = esp_rom_crc32_le(crc, chunk + crc_skip,
                                   bytes - crc_skip);
        }
        offset += bytes;
    }
    uint8_t digest[32] = {0};
    size_t digest_size = 0;
    if (status == PSA_SUCCESS) {
        status = psa_hash_finish(&operation, digest, sizeof(digest),
                                 &digest_size);
    } else {
        (void)psa_hash_abort(&operation);
    }
    heap_caps_free(chunk);
    if (status != PSA_SUCCESS || digest_size != sizeof(digest)) {
        (void)psa_hash_abort(&operation);
        ESP_LOGE(TAG, "pack SHA-256 calculation failed status=%ld",
                 (long)status);
        return ESP_FAIL;
    }

    if (crc != s_header.payload_crc32) {
        ESP_LOGE(TAG,
                 "payload CRC mismatch header=%08lx actual=%08lx",
                 (unsigned long)s_header.payload_crc32,
                 (unsigned long)crc);
        memset(digest, 0, sizeof(digest));
        return ESP_ERR_INVALID_CRC;
    }

    char digest_hex[SHA256_HEX_BYTES + 1U];
    sha256_to_hex(digest, digest_hex);
    const bool compatible = compatible_sha256(digest_hex);
    s_release_match =
        strcmp(digest_hex, NUWAX_EMOTION_RELEASE_SHA256) == 0;
    if (!compatible) {
        ESP_LOGE(TAG,
                 "emotion pack SHA-256 is not in firmware compatibility set actual=%.8s",
                 digest_hex);
        memset(digest_hex, 0, sizeof(digest_hex));
        memset(digest, 0, sizeof(digest));
        return ESP_ERR_INVALID_CRC;
    }
    memset(digest_hex, 0, sizeof(digest_hex));
    memset(digest, 0, sizeof(digest));
    return ESP_OK;
}

static esp_err_t validate_v3_segment(uint16_t segment_index,
                                     const pack_segment_t *source)
{
    pack_v3_segment_header_t header;
    if (source->compressed_size < sizeof(header)) {
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_RETURN_ON_ERROR(
        esp_partition_read(s_partition, source->offset, &header, sizeof(header)),
        TAG, "emotion v3 segment header read failed");

    const bool has_tail =
        (header.flags & NUWAX_EMOTION_ASSET_V3_FLAG_TAIL_ZLIB) != 0U;
    const bool has_transition =
        (header.flags & NUWAX_EMOTION_ASSET_V3_FLAG_TRANSITION) != 0U;
    const bool has_wrap =
        (header.flags & NUWAX_EMOTION_ASSET_V3_FLAG_WRAP) != 0U;
    const bool is_loop = source->kind == PACK_SEGMENT_LOOP;
    if (memcmp(header.magic, PACK_V3_SEGMENT_MAGIC,
               sizeof(PACK_V3_SEGMENT_MAGIC)) != 0 ||
        header.header_size != sizeof(header) ||
        (header.flags & ~PACK_V3_SUPPORTED_FLAGS) != 0U ||
        (header.flags & NUWAX_EMOTION_ASSET_V3_FLAG_KEY_RLE) == 0U ||
        has_tail != (source->frame_count > 1U) ||
        has_transition != is_loop || has_wrap != is_loop ||
        header.key_offset != sizeof(header) || header.key_size == 0U ||
        header.key_size > PACK_V3_MAX_KEY_BYTES ||
        (header.key_size % 3U) != 0U || header.key_crc32 == 0U ||
        header.payload_crc32 == 0U) {
        ESP_LOGE(TAG, "emotion v3 segment contract mismatch index=%u",
                 (unsigned)segment_index);
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t expected_offset = header.key_offset;
    if (!range_valid(expected_offset, header.key_size,
                     source->compressed_size)) {
        return ESP_ERR_INVALID_SIZE;
    }
    expected_offset += header.key_size;
    if (has_tail) {
        const uint32_t expected_tail_raw =
            source->raw_size - PACK_V2_ABSOLUTE_RAW_SIZE;
        if (source->raw_size < PACK_V2_ABSOLUTE_RAW_SIZE ||
            header.tail_offset != expected_offset || header.tail_size == 0U ||
            header.tail_raw_size != expected_tail_raw ||
            header.tail_raw_crc32 == 0U ||
            !range_valid(header.tail_offset, header.tail_size,
                         source->compressed_size)) {
            return ESP_ERR_INVALID_SIZE;
        }
        expected_offset += header.tail_size;
    } else if (header.tail_offset != 0U || header.tail_size != 0U ||
               header.tail_raw_size != 0U || header.tail_raw_crc32 != 0U ||
               source->raw_size != PACK_V2_ABSOLUTE_RAW_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (has_transition) {
        if (header.transition_offset != expected_offset ||
            header.transition_size < 8U || header.transition_crc32 == 0U ||
            header.transition_size > PACK_V3_MAX_PATCH_BYTES ||
            !range_valid(header.transition_offset, header.transition_size,
                         source->compressed_size)) {
            return ESP_ERR_INVALID_SIZE;
        }
        expected_offset += header.transition_size;
    } else if (header.transition_offset != 0U ||
               header.transition_size != 0U ||
               header.transition_crc32 != 0U) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (has_wrap) {
        if (header.wrap_offset != expected_offset || header.wrap_size < 8U ||
            header.wrap_size > PACK_V3_MAX_PATCH_BYTES ||
            header.wrap_crc32 == 0U ||
            !range_valid(header.wrap_offset, header.wrap_size,
                         source->compressed_size)) {
            return ESP_ERR_INVALID_SIZE;
        }
        expected_offset += header.wrap_size;
    } else if (header.wrap_offset != 0U || header.wrap_size != 0U ||
               header.wrap_crc32 != 0U) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (expected_offset != source->compressed_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    const uint32_t base = source->offset;
    s_v3_segments[segment_index] = (nuwax_emotion_asset_v3_segment_t){
        .flags = header.flags,
        .key_offset = base + header.key_offset,
        .key_size = header.key_size,
        .key_crc32 = header.key_crc32,
        .tail_offset = has_tail ? base + header.tail_offset : 0U,
        .tail_size = header.tail_size,
        .tail_raw_size = header.tail_raw_size,
        .tail_raw_crc32 = header.tail_raw_crc32,
        .transition_offset = has_transition ? base + header.transition_offset : 0U,
        .transition_size = header.transition_size,
        .transition_crc32 = header.transition_crc32,
        .wrap_offset = has_wrap ? base + header.wrap_offset : 0U,
        .wrap_size = header.wrap_size,
        .wrap_crc32 = header.wrap_crc32,
    };
    return ESP_OK;
}

static esp_err_t validate_segment_table(
    const pack_segment_t raw_segments[NUWAX_EMOTION_ASSET_MAX_SEGMENTS])
{
    uint32_t previous_end = s_header.data_offset;
    for (uint16_t i = 0; i < s_header.segment_count; ++i) {
        const pack_segment_t *source = &raw_segments[i];
        const bool raw_size_valid = nuwax_emotion_segment_raw_size_valid(
            s_header.version, source->frame_count, source->raw_size,
            PACK_FRAME_PIXELS, 8U);
        if (source->frame_count == 0 || source->compressed_size == 0 ||
            !raw_size_valid ||
            source->kind > PACK_SEGMENT_COMPLETION ||
            (source->offset & 3U) != 0U ||
            !range_valid(source->offset, source->compressed_size,
                         s_header.total_size) ||
            source->offset < previous_end) {
            ESP_LOGE(TAG,
                     "invalid segment index=%u kind=%u offset=%lu compressed=%lu raw=%lu frames=%u previous_end=%lu",
                     (unsigned)i, (unsigned)source->kind,
                     (unsigned long)source->offset,
                     (unsigned long)source->compressed_size,
                     (unsigned long)source->raw_size,
                     (unsigned)source->frame_count,
                     (unsigned long)previous_end);
            return ESP_ERR_INVALID_SIZE;
        }
        previous_end = align4(source->offset + source->compressed_size);
        if (previous_end > s_header.total_size) {
            return ESP_ERR_INVALID_SIZE;
        }
        s_segments[i] = (nuwax_emotion_asset_segment_t){
            .offset = source->offset,
            .compressed_size = source->compressed_size,
            .raw_size = source->raw_size,
            .raw_crc32 = source->raw_crc32,
            .frame_count = source->frame_count,
            .kind = source->kind,
        };
        if (s_header.version == PACK_VERSION_V3) {
            ESP_RETURN_ON_ERROR(validate_v3_segment(i, source), TAG,
                                "emotion v3 segment validation failed");
        }
        if (s_header.version == PACK_VERSION_V4) {
            ESP_RETURN_ON_ERROR(validate_v4_segment(i, source), TAG,
                                "emotion v4 segment validation failed");
        }
    }
    return ESP_OK;
}

static bool segment_kind_is(const pack_segment_t *segments,
                            uint16_t index, uint16_t expected)
{
    return index < s_header.segment_count && segments[index].kind == expected;
}

static esp_err_t validate_entry_table(
    const pack_entry_t raw_entries[NUWAX_EMOTION_ASSET_MAX_ENTRIES],
    const pack_segment_t raw_segments[NUWAX_EMOTION_ASSET_MAX_SEGMENTS])
{
    uint8_t segment_references[NUWAX_EMOTION_ASSET_MAX_SEGMENTS] = {0};
    uint32_t seen_assets = 0;
    for (uint16_t i = 0; i < s_header.entry_count; ++i) {
        const pack_entry_t *source = &raw_entries[i];
        if (source->asset_id == 0 ||
            source->asset_id > NUWAX_EMOTION_ASSET_MAX_ENTRIES) {
            ESP_LOGE(TAG, "invalid asset id index=%u asset=%u",
                     (unsigned)i, (unsigned)source->asset_id);
            return ESP_ERR_INVALID_ARG;
        }
        const uint32_t asset_bit = 1UL << (source->asset_id - 1U);
        const asset_contract_t *contract = &PACK_ASSETS[source->asset_id - 1U];
        const size_t contract_name_len = strlen(contract->name);
        const size_t packed_name_len = contract_name_len < sizeof(source->name) - 1U
                                           ? contract_name_len
                                           : sizeof(source->name) - 1U;
        if ((seen_assets & asset_bit) != 0U || source->reserved != 0U ||
            source->flags != contract->flags ||
            (source->flags & ~PACK_SUPPORTED_FLAGS) != 0U ||
            source->fps != contract->fps ||
            source->fps < PACK_MIN_ANIMATED_FPS ||
            source->palette_count != NUWAX_EMOTION_ASSET_PALETTE_COLORS ||
            source->palette_offset !=
                s_header.palette_offset +
                    (uint32_t)i * NUWAX_EMOTION_ASSET_PALETTE_COLORS *
                        sizeof(uint16_t) ||
            strnlen(source->name, sizeof(source->name)) != packed_name_len ||
            strncmp(source->name, contract->name, packed_name_len) != 0 ||
            !segment_index_valid(source->intro_segment) ||
            !segment_index_valid(source->loop_segment) ||
            !segment_index_valid(source->completion_segment)) {
            ESP_LOGE(TAG, "asset contract mismatch index=%u asset=%u flags=%u fps=%u",
                     (unsigned)i, (unsigned)source->asset_id,
                     (unsigned)source->flags, (unsigned)source->fps);
            return ESP_ERR_INVALID_ARG;
        }

        bool topology_ok = false;
        if (source->flags == NUWAX_EMOTION_ASSET_FLAG_PERSISTENT) {
            topology_ok = source->completion_segment ==
                              NUWAX_EMOTION_ASSET_INVALID_SEGMENT &&
                          segment_kind_is(raw_segments,
                                          source->intro_segment,
                                          PACK_SEGMENT_INTRO) &&
                          segment_kind_is(raw_segments,
                                          source->loop_segment,
                                          PACK_SEGMENT_LOOP);
        } else if (source->flags == NUWAX_EMOTION_ASSET_FLAG_TRANSIENT) {
            topology_ok = source->intro_segment ==
                              NUWAX_EMOTION_ASSET_INVALID_SEGMENT &&
                          source->loop_segment ==
                              NUWAX_EMOTION_ASSET_INVALID_SEGMENT &&
                          segment_kind_is(raw_segments,
                                          source->completion_segment,
                                          PACK_SEGMENT_TRANSIENT);
        } else if (source->flags == NUWAX_EMOTION_ASSET_FLAG_COMPLETION) {
            topology_ok = source->intro_segment ==
                              NUWAX_EMOTION_ASSET_INVALID_SEGMENT &&
                          source->loop_segment ==
                              NUWAX_EMOTION_ASSET_INVALID_SEGMENT &&
                          segment_kind_is(raw_segments,
                                          source->completion_segment,
                                          PACK_SEGMENT_COMPLETION);
        }
        if (!topology_ok) {
            ESP_LOGE(TAG, "asset segment topology mismatch asset=%u",
                     (unsigned)source->asset_id);
            return ESP_ERR_INVALID_ARG;
        }

        const uint16_t indices[] = {
            source->intro_segment,
            source->loop_segment,
            source->completion_segment,
        };
        for (size_t j = 0; j < sizeof(indices) / sizeof(indices[0]); ++j) {
            if (indices[j] != NUWAX_EMOTION_ASSET_INVALID_SEGMENT) {
                if (++segment_references[indices[j]] != 1U) {
                    ESP_LOGE(TAG, "segment shared by multiple assets index=%u",
                             (unsigned)indices[j]);
                    return ESP_ERR_INVALID_ARG;
                }
            }
        }
        seen_assets |= asset_bit;
        s_entries[i] = (nuwax_emotion_asset_entry_t){
            .asset_id = source->asset_id,
            .flags = source->flags,
            .fps = source->fps,
            .palette_count = source->palette_count,
            .intro_segment = source->intro_segment,
            .loop_segment = source->loop_segment,
            .completion_segment = source->completion_segment,
            .palette_offset = source->palette_offset,
        };
        memcpy(s_entries[i].name, source->name, sizeof(source->name));
        s_entries[i].name[sizeof(s_entries[i].name) - 1] = '\0';
    }
    if (seen_assets != 0xffffU) {
        ESP_LOGE(TAG, "emotion asset set incomplete bitmap=%04lx",
                 (unsigned long)seen_assets);
        return ESP_ERR_NOT_FOUND;
    }
    for (uint16_t i = 0; i < s_header.segment_count; ++i) {
        if (segment_references[i] != 1U) {
            ESP_LOGE(TAG, "orphan emotion segment index=%u refs=%u",
                     (unsigned)i, (unsigned)segment_references[i]);
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

esp_err_t nuwax_emotion_asset_pack_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    reset_pack_state();

    uint32_t flash_size = 0;
    esp_err_t err = esp_flash_get_size(NULL, &flash_size);
    if (err != ESP_OK || flash_size < PACK_REQUIRED_FLASH_SIZE) {
        ESP_LOGW(TAG,
                 "emotion pack unavailable: detected flash=%lu required=%lu err=%s",
                 (unsigned long)flash_size,
                 (unsigned long)PACK_REQUIRED_FLASH_SIZE,
                 esp_err_to_name(err));
        return err == ESP_OK ? ESP_ERR_INVALID_SIZE : err;
    }
    s_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        (esp_partition_subtype_t)PACK_PARTITION_SUBTYPE,
        PACK_PARTITION_LABEL);
    if (!s_partition || s_partition->address != PACK_PARTITION_ADDRESS ||
        s_partition->size != PACK_PARTITION_SIZE) {
        ESP_LOGW(TAG,
                 "emotion partition missing or incompatible label=%s address=%08lx size=%lu",
                 s_partition ? s_partition->label : "-",
                 s_partition ? (unsigned long)s_partition->address : 0UL,
                 s_partition ? (unsigned long)s_partition->size : 0UL);
        reset_pack_state();
        return ESP_ERR_NOT_FOUND;
    }

    err = esp_partition_read(s_partition, 0, &s_header, sizeof(s_header));
    if (err != ESP_OK) {
        reset_pack_state();
        return err;
    }
    const uint32_t entries_bytes =
        (uint32_t)NUWAX_EMOTION_ASSET_MAX_ENTRIES * sizeof(pack_entry_t);
    const uint32_t segments_bytes =
        (uint32_t)PACK_EXPECTED_SEGMENTS * sizeof(pack_segment_t);
    const uint32_t expected_palette_offset =
        sizeof(pack_header_t) + entries_bytes + segments_bytes;
    const uint32_t expected_data_offset = align4(
        expected_palette_offset +
        NUWAX_EMOTION_ASSET_MAX_ENTRIES *
            NUWAX_EMOTION_ASSET_PALETTE_COLORS * sizeof(uint16_t));
    const bool abi_v1 =
        s_header.version == PACK_VERSION_V1 &&
        memcmp(s_header.magic, PACK_MAGIC_V1, sizeof(PACK_MAGIC_V1)) == 0;
    const bool abi_v2 =
        s_header.version == PACK_VERSION_V2 &&
        memcmp(s_header.magic, PACK_MAGIC_V2, sizeof(PACK_MAGIC_V2)) == 0;
    const bool abi_v3 =
        s_header.version == PACK_VERSION_V3 &&
        memcmp(s_header.magic, PACK_MAGIC_V3, sizeof(PACK_MAGIC_V3)) == 0;
    const bool abi_v4 =
        s_header.version == PACK_VERSION_V4 &&
        memcmp(s_header.magic, PACK_MAGIC_V4, sizeof(PACK_MAGIC_V4)) == 0;
    if ((!abi_v1 && !abi_v2 && !abi_v3 && !abi_v4) ||
        s_header.header_size != sizeof(pack_header_t) ||
        s_header.width != PACK_WIDTH || s_header.height != PACK_HEIGHT ||
        s_header.entry_count != NUWAX_EMOTION_ASSET_MAX_ENTRIES ||
        s_header.segment_count != PACK_EXPECTED_SEGMENTS ||
        s_header.entry_offset != sizeof(pack_header_t) ||
        s_header.segment_offset != sizeof(pack_header_t) + entries_bytes ||
        s_header.palette_offset != expected_palette_offset ||
        s_header.data_offset != expected_data_offset ||
        s_header.total_size < expected_data_offset ||
        s_header.total_size > s_partition->size ||
        s_header.payload_crc32 == 0U ||
        s_header.catalog_crc32 == 0U) {
        ESP_LOGW(TAG,
                 "emotion pack header incompatible version=%u entries=%u segments=%u bytes=%lu payload=%08lx catalog=%08lx",
                 (unsigned)s_header.version,
                 (unsigned)s_header.entry_count,
                 (unsigned)s_header.segment_count,
                 (unsigned long)s_header.total_size,
                 (unsigned long)s_header.payload_crc32,
                 (unsigned long)s_header.catalog_crc32);
        reset_pack_state();
        return ESP_ERR_INVALID_VERSION;
    }

    pack_entry_t raw_entries[NUWAX_EMOTION_ASSET_MAX_ENTRIES];
    pack_segment_t raw_segments[NUWAX_EMOTION_ASSET_MAX_SEGMENTS];
    memset(raw_segments, 0, sizeof(raw_segments));
    err = esp_partition_read(s_partition, s_header.entry_offset,
                             raw_entries, entries_bytes);
    if (err == ESP_OK) {
        err = esp_partition_read(s_partition, s_header.segment_offset,
                                 raw_segments, segments_bytes);
    }
    if (err == ESP_OK) {
        err = validate_segment_table(raw_segments);
    }
    if (err == ESP_OK) {
        err = validate_entry_table(raw_entries, raw_segments);
    }
    if (err == ESP_OK) {
        err = validate_pack_integrity();
    }
    if (err != ESP_OK) {
        reset_pack_state();
        return err;
    }

    const void *mapped = NULL;
    esp_partition_mmap_handle_t mapped_handle = 0U;
    const esp_err_t mmap_err = esp_partition_mmap(
        s_partition, 0U, s_header.total_size, ESP_PARTITION_MMAP_DATA,
        &mapped, &mapped_handle);
    if (mmap_err == ESP_OK && mapped) {
        s_mapped_pack = (const uint8_t *)mapped;
        s_mapped_pack_handle = mapped_handle;
    } else {
        ESP_LOGW(TAG,
                 "emotion pack mmap unavailable; retaining bounded partition-read fallback err=%s",
                 esp_err_to_name(mmap_err));
    }

    s_header.build_id[sizeof(s_header.build_id) - 1] = '\0';
    s_ready = true;
    s_release_match = s_release_match &&
                      s_header.total_size == PACK_RELEASE_TOTAL_SIZE &&
                      s_header.payload_crc32 == PACK_RELEASE_PAYLOAD_CRC32 &&
                      s_header.catalog_crc32 == PACK_RELEASE_CATALOG_CRC32;
    ESP_LOGI(TAG,
             "emotion asset pack verified partition=%s build=%s bytes=%lu entries=%u segments=%u min_fps=%u release_match=%d integrity=sha256+payload+segment_crc abi=v%u",
             s_partition->label, s_header.build_id,
             (unsigned long)s_header.total_size,
             (unsigned)s_header.entry_count,
             (unsigned)s_header.segment_count,
             (unsigned)PACK_MIN_ANIMATED_FPS,
             s_release_match ? 1 : 0,
             (unsigned)s_header.version);
    ESP_LOGI(TAG, "emotion asset runtime reads=%s",
             s_mapped_pack ? "mmap" : "partition");
    return ESP_OK;
}

void nuwax_emotion_asset_pack_deinit(void)
{
    reset_pack_state();
}

bool nuwax_emotion_asset_pack_ready(void)
{
    return s_ready;
}

uint16_t nuwax_emotion_asset_pack_abi(void)
{
    return s_ready ? s_header.version : 0U;
}

const char *nuwax_emotion_asset_pack_build_id(void)
{
    return s_ready ? s_header.build_id : "unavailable";
}

size_t nuwax_emotion_asset_pack_size(void)
{
    return s_ready ? s_header.total_size : 0U;
}

const nuwax_emotion_asset_entry_t *nuwax_emotion_asset_pack_find(
    nuwax_emotion_asset_id_t asset_id)
{
    if (!s_ready || asset_id < NUWAX_EMOTION_ASSET_DIZZINESS ||
        asset_id > NUWAX_EMOTION_ASSET_QUESTION) {
        return NULL;
    }
    for (uint16_t i = 0; i < s_header.entry_count; ++i) {
        if (s_entries[i].asset_id == (uint16_t)asset_id) {
            return &s_entries[i];
        }
    }
    return NULL;
}

const nuwax_emotion_asset_segment_t *nuwax_emotion_asset_pack_segment(
    uint16_t segment_index)
{
    if (!s_ready || segment_index >= s_header.segment_count) {
        return NULL;
    }
    return &s_segments[segment_index];
}

const nuwax_emotion_asset_v3_segment_t *nuwax_emotion_asset_pack_v3_segment(
    uint16_t segment_index)
{
    if (!s_ready || s_header.version != PACK_VERSION_V3 ||
        segment_index >= s_header.segment_count) {
        return NULL;
    }
    return &s_v3_segments[segment_index];
}

const nuwax_emotion_asset_v4_segment_t *nuwax_emotion_asset_pack_v4_segment(
    uint16_t segment_index)
{
    if (!s_ready || s_header.version != PACK_VERSION_V4 ||
        segment_index >= s_header.segment_count) {
        return NULL;
    }
    return &s_v4_segments[segment_index];
}

static esp_err_t parse_v4_frame_record(
    uint16_t segment_index, uint16_t frame_index, uint32_t record_offset,
    uint32_t record_size, bool handoff,
    nuwax_emotion_asset_v4_frame_t *frame_out)
{
    if (!frame_out || segment_index >= s_header.segment_count) {
        return ESP_ERR_INVALID_ARG;
    }
    const nuwax_emotion_asset_v4_segment_t *v4 = &s_v4_segments[segment_index];
    const uint32_t fixed_size = sizeof(pack_v4_frame_header_t) +
        NUWAX_EMOTION_ASSET_V4_MAX_REGIONS * sizeof(pack_v4_region_t);
    if (record_size < fixed_size ||
        record_size > v4->max_record_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t fixed[sizeof(pack_v4_frame_header_t) +
                  NUWAX_EMOTION_ASSET_V4_MAX_REGIONS *
                      sizeof(pack_v4_region_t)];
    ESP_RETURN_ON_ERROR(
        read_pack_bytes(record_offset, fixed, sizeof(fixed)),
        TAG, "emotion v4 frame metadata read failed");
    pack_v4_frame_header_t header;
    memcpy(&header, fixed, sizeof(header));
    const bool absolute =
        (header.flags & NUWAX_EMOTION_ASSET_V4_FRAME_FLAG_ABSOLUTE) != 0U;
    const bool rle =
        (header.flags & NUWAX_EMOTION_ASSET_V4_FRAME_FLAG_RLE) != 0U;
    const uint8_t supported_frame_flags =
        NUWAX_EMOTION_ASSET_V4_FRAME_FLAG_ABSOLUTE |
        NUWAX_EMOTION_ASSET_V4_FRAME_FLAG_RLE;
    if ((header.flags & ~supported_frame_flags) != 0U ||
        (!handoff && frame_index == 0U && !absolute) ||
        (!handoff && frame_index != 0U && absolute) ||
        (handoff && (!absolute || !rle)) ||
        (rle && !v4->rle_records) ||
        header.region_count > NUWAX_EMOTION_ASSET_V4_MAX_REGIONS ||
        (header.changed_count == 0U) != (header.region_count == 0U) ||
        header.changed_count > PACK_FRAME_PIXELS ||
        header.compressed_size != record_size - fixed_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    *frame_out = (nuwax_emotion_asset_v4_frame_t){
        .compressed_size = header.compressed_size,
        .absolute = absolute,
        .region_count = header.region_count,
        .changed_count = header.changed_count,
        .raw_size = v4->regional_payload ? 0U : PACK_FRAME_PIXELS,
        .raw_crc32 = header.raw_crc32,
        .compressed_offset = record_offset + fixed_size,
        .regional_payload = v4->regional_payload,
        .rle = rle,
    };
    for (uint8_t i = 0; i < NUWAX_EMOTION_ASSET_V4_MAX_REGIONS; ++i) {
        pack_v4_region_t region;
        memcpy(&region, fixed + sizeof(header) + i * sizeof(region),
               sizeof(region));
        if (i < header.region_count) {
            if (region.width == 0U || region.height == 0U ||
                region.x >= PACK_WIDTH || region.y >= PACK_HEIGHT ||
                region.width > PACK_WIDTH - region.x ||
                region.height > PACK_HEIGHT - region.y) {
                return ESP_ERR_INVALID_SIZE;
            }
            frame_out->regions[i] = (nuwax_emotion_asset_region_t){
                .x = region.x, .y = region.y,
                .width = region.width, .height = region.height,
            };
            if (v4->regional_payload) {
                const uint32_t area = (uint32_t)region.width * region.height;
                if (area > PACK_FRAME_PIXELS - frame_out->raw_size) {
                    return ESP_ERR_INVALID_SIZE;
                }
                frame_out->raw_size += area;
            }
        } else if (region.x != 0U || region.y != 0U ||
                   region.width != 0U || region.height != 0U) {
            return ESP_ERR_INVALID_SIZE;
        }
    }
    if (absolute &&
        (header.changed_count != PACK_FRAME_PIXELS ||
         header.region_count != 1U || frame_out->regions[0].x != 0U ||
         frame_out->regions[0].y != 0U ||
         frame_out->regions[0].width != PACK_WIDTH ||
         frame_out->regions[0].height != PACK_HEIGHT ||
         frame_out->raw_size != PACK_FRAME_PIXELS)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t nuwax_emotion_asset_pack_v4_frame(
    uint16_t segment_index, uint16_t frame_index,
    nuwax_emotion_asset_v4_frame_t *frame_out)
{
    if (!s_ready || s_header.version != PACK_VERSION_V4 || !frame_out ||
        segment_index >= s_header.segment_count ||
        frame_index >= s_segments[segment_index].frame_count) {
        return ESP_ERR_INVALID_ARG;
    }
    const nuwax_emotion_asset_v4_segment_t *v4 = &s_v4_segments[segment_index];
    uint32_t offsets[2] = {0};
    ESP_RETURN_ON_ERROR(
        read_pack_bytes(v4->offset_table_offset +
                            (uint32_t)frame_index * sizeof(uint32_t),
                        offsets, sizeof(offsets)),
        TAG, "emotion v4 frame offsets read failed");
    if (offsets[1] < offsets[0]) return ESP_ERR_INVALID_SIZE;
    return parse_v4_frame_record(
        segment_index, frame_index, v4->records_offset + offsets[0],
        offsets[1] - offsets[0], false, frame_out);
}

esp_err_t nuwax_emotion_asset_pack_v4_handoff_frame(
    uint16_t segment_index, uint16_t frame_index,
    nuwax_emotion_asset_v4_frame_t *frame_out)
{
    if (!s_ready || s_header.version != PACK_VERSION_V4 || !frame_out ||
        segment_index >= s_header.segment_count) {
        return ESP_ERR_INVALID_ARG;
    }
    const nuwax_emotion_asset_v4_segment_t *v4 = &s_v4_segments[segment_index];
    if (v4->handoff_record_size == 0U ||
        v4->handoff_frame_index != frame_index) {
        return ESP_ERR_NOT_FOUND;
    }
    return parse_v4_frame_record(
        segment_index, frame_index, v4->handoff_record_offset,
        v4->handoff_record_size, true, frame_out);
}

esp_err_t nuwax_emotion_asset_pack_read(uint32_t offset, void *buffer,
                                        size_t size)
{
    if (!s_ready || !buffer || size == 0 || size > UINT32_MAX ||
        !range_valid(offset, (uint32_t)size, s_header.total_size)) {
        return ESP_ERR_INVALID_ARG;
    }
    return read_pack_bytes(offset, buffer, size);
}

esp_err_t nuwax_emotion_asset_pack_read_palette(
    const nuwax_emotion_asset_entry_t *entry,
    uint16_t palette[NUWAX_EMOTION_ASSET_PALETTE_COLORS])
{
    if (!entry || !palette) {
        return ESP_ERR_INVALID_ARG;
    }
    bool known_entry = false;
    for (uint16_t i = 0; i < s_header.entry_count; ++i) {
        if (entry == &s_entries[i]) {
            known_entry = true;
            break;
        }
    }
    if (!known_entry) {
        return ESP_ERR_INVALID_ARG;
    }
    return nuwax_emotion_asset_pack_read(
        entry->palette_offset, palette,
        NUWAX_EMOTION_ASSET_PALETTE_COLORS * sizeof(uint16_t));
}
