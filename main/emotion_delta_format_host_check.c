#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "nuwax_ui_v28/face/emotion_delta_format.h"

typedef struct {
    uint8_t bytes[NUWAX_EMOTION_DELTA_MAX_RECORD_BYTES];
    size_t size;
} record_builder_t;

static void append(record_builder_t *builder, const void *data, size_t size)
{
    assert(builder->size + size <= sizeof(builder->bytes));
    memcpy(builder->bytes + builder->size, data, size);
    builder->size += size;
}

static void add_run(record_builder_t *builder, uint32_t offset,
                    uint16_t length, uint8_t value)
{
    const nuwax_emotion_delta_run_header_t run = {
        .offset = offset,
        .length = length,
    };
    append(builder, &run, sizeof(run));
    memset(builder->bytes + builder->size, value, length);
    builder->size += length;
}

static record_builder_t make_delta(uint16_t run_count, uint32_t changed_count)
{
    record_builder_t builder = {0};
    const nuwax_emotion_delta_frame_header_t header = {
        .run_count = run_count,
        .flags = 0U,
        .changed_count = changed_count,
    };
    append(&builder, &header, sizeof(header));
    return builder;
}

int main(void)
{
    nuwax_emotion_delta_record_info_t info = {0};

    record_builder_t empty = make_delta(0U, 0U);
    assert(nuwax_emotion_delta_record_validate(
        empty.bytes, empty.size, 64U, false, &info));
    assert(info.run_count == 0U && info.changed_count == 0U && !info.absolute);

    record_builder_t delta = make_delta(2U, 5U);
    add_run(&delta, 360U, 2U, 1U);
    add_run(&delta, 720U, 3U, 2U);
    assert(nuwax_emotion_delta_record_validate(
        delta.bytes, delta.size, 64U, false, &info));

    record_builder_t overlap = make_delta(2U, 4U);
    add_run(&overlap, 100U, 2U, 1U);
    add_run(&overlap, 101U, 2U, 1U);
    assert(!nuwax_emotion_delta_record_validate(
        overlap.bytes, overlap.size, 64U, false, NULL));

    record_builder_t cross_row = make_delta(1U, 2U);
    add_run(&cross_row, 359U, 2U, 1U);
    assert(!nuwax_emotion_delta_record_validate(
        cross_row.bytes, cross_row.size, 64U, false, NULL));

    record_builder_t palette = make_delta(1U, 1U);
    add_run(&palette, 0U, 1U, 64U);
    assert(!nuwax_emotion_delta_record_validate(
        palette.bytes, palette.size, 64U, false, NULL));

    record_builder_t absolute = {0};
    const nuwax_emotion_delta_frame_header_t absolute_header = {
        .run_count = NUWAX_EMOTION_DELTA_FRAME_HEIGHT,
        .flags = NUWAX_EMOTION_DELTA_FRAME_FLAG_ABSOLUTE,
        .changed_count = NUWAX_EMOTION_DELTA_FRAME_PIXELS,
    };
    append(&absolute, &absolute_header, sizeof(absolute_header));
    for (uint32_t row = 0; row < NUWAX_EMOTION_DELTA_FRAME_HEIGHT; ++row) {
        add_run(&absolute, row * NUWAX_EMOTION_DELTA_FRAME_WIDTH,
                NUWAX_EMOTION_DELTA_FRAME_WIDTH, 3U);
    }
    assert(nuwax_emotion_delta_record_validate(
        absolute.bytes, absolute.size, 64U, true, &info));
    assert(!nuwax_emotion_delta_record_validate(
        absolute.bytes, absolute.size - 1U, 64U, true, NULL));
    assert(!nuwax_emotion_delta_record_validate(
        absolute.bytes, absolute.size, 64U, false, NULL));

    /* Production absolute frames use long contiguous runs to avoid 360 row
     * headers. Only sparse delta runs must stay within one display row. */
    record_builder_t compact_absolute = {0};
    const nuwax_emotion_delta_frame_header_t compact_header = {
        .run_count = 2U,
        .flags = NUWAX_EMOTION_DELTA_FRAME_FLAG_ABSOLUTE,
        .changed_count = NUWAX_EMOTION_DELTA_FRAME_PIXELS,
    };
    append(&compact_absolute, &compact_header, sizeof(compact_header));
    add_run(&compact_absolute, 0U, UINT16_MAX, 3U);
    add_run(&compact_absolute, UINT16_MAX,
            (uint16_t)(NUWAX_EMOTION_DELTA_FRAME_PIXELS - UINT16_MAX), 4U);
    assert(nuwax_emotion_delta_record_validate(
        compact_absolute.bytes, compact_absolute.size, 64U, true, &info));

    puts("emotion_delta_format_host_check: PASS");
    return 0;
}
