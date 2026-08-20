#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "emotion_asset_pack.h"

typedef struct {
    uint64_t bytes;
    bool exact;
    bool supported;
} nuwax_emotion_segment_size_requirement_t;

static inline nuwax_emotion_segment_size_requirement_t
nuwax_emotion_segment_size_requirement(uint16_t abi, uint16_t frame_count,
                                       uint32_t frame_pixels,
                                       uint32_t framed_record_min_bytes)
{
    nuwax_emotion_segment_size_requirement_t requirement = {0};
    if (frame_count == 0U || frame_pixels == 0U ||
        framed_record_min_bytes == 0U) {
        return requirement;
    }

    switch (abi) {
    case NUWAX_EMOTION_ASSET_PACK_ABI_V1:
    case NUWAX_EMOTION_ASSET_PACK_ABI_V4:
        requirement.bytes = (uint64_t)frame_count * frame_pixels;
        requirement.exact = true;
        requirement.supported = true;
        break;
    case NUWAX_EMOTION_ASSET_PACK_ABI_V2:
    case NUWAX_EMOTION_ASSET_PACK_ABI_V3:
        requirement.bytes =
            (uint64_t)frame_count * framed_record_min_bytes;
        requirement.exact = false;
        requirement.supported = true;
        break;
    default:
        break;
    }
    return requirement;
}

static inline bool nuwax_emotion_segment_raw_size_valid(
    uint16_t abi, uint16_t frame_count, uint32_t raw_size,
    uint32_t frame_pixels, uint32_t framed_record_min_bytes)
{
    const nuwax_emotion_segment_size_requirement_t requirement =
        nuwax_emotion_segment_size_requirement(
            abi, frame_count, frame_pixels, framed_record_min_bytes);
    if (!requirement.supported || requirement.bytes > UINT32_MAX) {
        return false;
    }
    return requirement.exact
               ? raw_size == (uint32_t)requirement.bytes
               : raw_size >= (uint32_t)requirement.bytes;
}
