#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "nuwax_ui_v28/face/emotion_segment_policy.h"

#define TEST_FRAME_PIXELS (360U * 360U)
#define TEST_RECORD_HEADER_BYTES 8U

int main(void)
{
    const uint16_t frames = 42U;
    const uint32_t full_frame_bytes = frames * TEST_FRAME_PIXELS;
    const uint32_t minimum_record_bytes =
        frames * TEST_RECORD_HEADER_BYTES;

    assert(nuwax_emotion_segment_raw_size_valid(
        NUWAX_EMOTION_ASSET_PACK_ABI_V1, frames, full_frame_bytes,
        TEST_FRAME_PIXELS, TEST_RECORD_HEADER_BYTES));
    assert(nuwax_emotion_segment_raw_size_valid(
        NUWAX_EMOTION_ASSET_PACK_ABI_V2, frames, minimum_record_bytes,
        TEST_FRAME_PIXELS, TEST_RECORD_HEADER_BYTES));
    assert(nuwax_emotion_segment_raw_size_valid(
        NUWAX_EMOTION_ASSET_PACK_ABI_V3, frames, minimum_record_bytes + 1U,
        TEST_FRAME_PIXELS, TEST_RECORD_HEADER_BYTES));
    assert(nuwax_emotion_segment_raw_size_valid(
        NUWAX_EMOTION_ASSET_PACK_ABI_V4, frames, full_frame_bytes,
        TEST_FRAME_PIXELS, TEST_RECORD_HEADER_BYTES));

    assert(!nuwax_emotion_segment_raw_size_valid(
        NUWAX_EMOTION_ASSET_PACK_ABI_V4, frames, full_frame_bytes - 1U,
        TEST_FRAME_PIXELS, TEST_RECORD_HEADER_BYTES));
    assert(!nuwax_emotion_segment_raw_size_valid(
        NUWAX_EMOTION_ASSET_PACK_ABI_V4, 0U, 0U, TEST_FRAME_PIXELS,
        TEST_RECORD_HEADER_BYTES));
    assert(!nuwax_emotion_segment_raw_size_valid(
        UINT16_MAX, frames, full_frame_bytes, TEST_FRAME_PIXELS,
        TEST_RECORD_HEADER_BYTES));
    assert(!nuwax_emotion_segment_raw_size_valid(
        NUWAX_EMOTION_ASSET_PACK_ABI_V4, UINT16_MAX, UINT32_MAX,
        UINT32_MAX, TEST_RECORD_HEADER_BYTES));

    puts("emotion_segment_policy_host_check: PASS");
    return 0;
}
