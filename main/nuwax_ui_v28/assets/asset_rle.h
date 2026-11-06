#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* PackBits-style runs, independently bounded per A8 frame. No allocation,
 * filesystem access or shared decoder state; call only on the UI owner. */
typedef struct {
    const uint8_t *data;
    const uint32_t *offsets; /* frame_count + 1 offsets, including end sentinel */
    size_t data_size;
    uint32_t frame_count, frame_bytes;
} nuwax_asset_rle_t;

static inline bool nuwax_asset_rle_decode_frame(const nuwax_asset_rle_t *asset,
                                               unsigned frame, uint8_t *out,
                                               size_t capacity)
{
    if (!asset || !asset->data || !asset->offsets || !out ||
        frame >= asset->frame_count || !asset->frame_bytes ||
        capacity < asset->frame_bytes) return false;
    size_t pos = asset->offsets[frame], end = asset->offsets[frame + 1U], written = 0;
    if (pos >= end || end > asset->data_size) return false;
    while (pos < end) {
        const unsigned tag = asset->data[pos++];
        if (tag == 128U) return false; /* Encoder never emits a no-op. */
        const size_t count = tag < 128U ? tag + 1U : 257U - tag;
        if (count > asset->frame_bytes - written) return false;
        if (tag < 128U) {
            if (count > end - pos) return false;
            memcpy(out + written, asset->data + pos, count);
            pos += count;
        } else {
            if (pos == end) return false;
            memset(out + written, asset->data[pos++], count);
        }
        written += count;
    }
    return written == asset->frame_bytes;
}
