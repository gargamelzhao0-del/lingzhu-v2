#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Kept separately from both FILE caches. Only original accepted bytes enter
 * this prefix; a later card read can never redefine the expected audio. */
typedef uint32_t (*sd_audio_seal_crc_t)(uint32_t, const void *, size_t);
typedef struct {
    uint32_t generation, index, crc32;
    uint16_t count;
    uint8_t data[512];
    uint16_t tail_count;
    uint32_t tail_crc32;
    uint64_t body_end;
    uint8_t tail[1024];
} sd_audio_seal_head_t;

static inline bool sd_audio_seal_head_valid(const sd_audio_seal_head_t *head,
    uint32_t generation, uint32_t index, sd_audio_seal_crc_t crc)
{
    return head && crc && head->count >= 96U && head->count <= sizeof(head->data) &&
        head->generation == generation && head->index == index &&
        crc(0U, head->data, head->count) == head->crc32;
}

static inline bool sd_audio_seal_head_begin(sd_audio_seal_head_t *head,
    const void *header, size_t bytes, uint32_t generation, uint32_t index,
    sd_audio_seal_crc_t crc)
{
    if (!head || !header || bytes != 96U || !crc) return false;
    memset(head, 0, sizeof(*head));
    head->generation = generation; head->index = index; head->count = (uint16_t)bytes;
    memcpy(head->data, header, bytes); head->crc32 = crc(0U, head->data, bytes);
    memcpy(head->tail, header, bytes); head->tail_count = (uint16_t)bytes;
    head->tail_crc32 = head->crc32; head->body_end = bytes;
    return true;
}

static inline void sd_audio_seal_tail_append(sd_audio_seal_head_t *head,
    const void *data, size_t bytes)
{
    const uint8_t *source = data;
    if (bytes >= sizeof(head->tail)) {
        memcpy(head->tail, source + bytes - sizeof(head->tail), sizeof(head->tail));
        head->tail_count = sizeof(head->tail);
    } else {
        size_t keep = head->tail_count;
        if (keep > sizeof(head->tail) - bytes) keep = sizeof(head->tail) - bytes;
        memmove(head->tail, head->tail + head->tail_count - keep, keep);
        memcpy(head->tail + keep, source, bytes);
        head->tail_count = (uint16_t)(keep + bytes);
    }
}

static inline bool sd_audio_seal_head_record(sd_audio_seal_head_t *head,
    const void *frame, uint32_t bytes, uint32_t generation, uint32_t index,
    sd_audio_seal_crc_t crc)
{
    if (!head || !frame || !bytes || !crc || head->generation != generation ||
        head->index != index || head->count < 96U || head->count > sizeof(head->data) ||
        !head->tail_count || head->tail_count > sizeof(head->tail) ||
        head->body_end < head->tail_count ||
        head->body_end > UINT64_MAX - sizeof(bytes) - bytes ||
        crc(0U, head->tail, head->tail_count) != head->tail_crc32) return false;
    if (head->count < sizeof(head->data)) {
        if (!sd_audio_seal_head_valid(head, generation, index, crc)) return false;
        size_t n = sizeof(head->data) - head->count;
        if (n > sizeof(bytes)) n = sizeof(bytes);
        memcpy(head->data + head->count, &bytes, n); head->count += (uint16_t)n;
        n = sizeof(head->data) - head->count;
        if (n > bytes) n = bytes;
        memcpy(head->data + head->count, frame, n); head->count += (uint16_t)n;
        head->crc32 = crc(0U, head->data, head->count);
    }
    sd_audio_seal_tail_append(head, &bytes, sizeof(bytes));
    sd_audio_seal_tail_append(head, frame, bytes);
    head->body_end += sizeof(bytes) + bytes;
    head->tail_crc32 = crc(0U, head->tail, head->tail_count);
    return true;
}

static inline size_t sd_audio_seal_head_prepare(const sd_audio_seal_head_t *head,
    uint32_t generation, uint32_t index, uint64_t start_ms, uint32_t duration_ms,
    uint8_t *output, size_t capacity, sd_audio_seal_crc_t crc)
{
    if (!output || !sd_audio_seal_head_valid(head, generation, index, crc) ||
        capacity < head->count || memcmp(head->data, "NWXOFF1", 7U)) return 0U;
    memcpy(output, head->data, head->count);
    for (unsigned n = 0U; n < 8U; ++n) output[84U+n] = (uint8_t)(start_ms >> (8U*n));
    for (unsigned n = 0U; n < 4U; ++n) output[92U+n] = (uint8_t)(duration_ms >> (8U*n));
    return head->count;
}

/* All bytes from an aligned offset through the original footer are known.
 * No old FILE sector cache or card read may redefine the accepted PCM. The
 * final partial sector is overwritten from byte zero to the exact file end. */
static inline size_t sd_audio_seal_tail_prepare(const sd_audio_seal_head_t *head,
    uint32_t generation, uint32_t index, uint64_t body_end, uint64_t start_ms,
    uint32_t duration_ms, const void *footer, size_t footer_bytes,
    uint8_t *output, size_t capacity, uint64_t *offset, sd_audio_seal_crc_t crc)
{
    if (!output || !offset || !footer || footer_bytes != 32U ||
        memcmp(footer, "NWXEND1", 7U) ||
        !sd_audio_seal_head_valid(head, generation, index, crc) ||
        !head->tail_count || head->tail_count > sizeof(head->tail) ||
        body_end != head->body_end || body_end < head->tail_count ||
        body_end > UINT64_MAX - footer_bytes ||
        crc(0U, head->tail, head->tail_count) != head->tail_crc32) return 0U;
    const uint64_t known = body_end - head->tail_count;
    if (known > UINT64_MAX - 511U) return 0U;
    const uint64_t aligned = (known + 511U) & ~UINT64_C(511);
    if (aligned > body_end) return 0U;
    const size_t bytes = (size_t)(body_end - aligned);
    if (bytes > capacity || footer_bytes > capacity - bytes) return 0U;
    memcpy(output, head->tail + (size_t)(aligned - known), bytes);
    if (aligned == 0U) {
        if (bytes < 96U) return 0U;
        for (unsigned n = 0U; n < 8U; ++n) output[84U+n] = (uint8_t)(start_ms >> (8U*n));
        for (unsigned n = 0U; n < 4U; ++n) output[92U+n] = (uint8_t)(duration_ms >> (8U*n));
    }
    memcpy(output + bytes, footer, footer_bytes);
    *offset = aligned;
    return bytes + footer_bytes;
}
