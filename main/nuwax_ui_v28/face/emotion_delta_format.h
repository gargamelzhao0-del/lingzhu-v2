#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NUWAX_EMOTION_DELTA_FRAME_WIDTH 360U
#define NUWAX_EMOTION_DELTA_FRAME_HEIGHT 360U
#define NUWAX_EMOTION_DELTA_FRAME_PIXELS                                      \
    (NUWAX_EMOTION_DELTA_FRAME_WIDTH * NUWAX_EMOTION_DELTA_FRAME_HEIGHT)
#define NUWAX_EMOTION_DELTA_FRAME_FLAG_ABSOLUTE 0x0001U
#define NUWAX_EMOTION_DELTA_MAX_FRAME_RUNS 2048U
#define NUWAX_EMOTION_DELTA_MAX_RECORD_BYTES                                  \
    (8U + NUWAX_EMOTION_DELTA_MAX_FRAME_RUNS * 6U +                          \
     NUWAX_EMOTION_DELTA_FRAME_PIXELS)

typedef struct __attribute__((packed)) {
    uint16_t run_count;
    uint16_t flags;
    uint32_t changed_count;
} nuwax_emotion_delta_frame_header_t;

typedef struct __attribute__((packed)) {
    uint32_t offset;
    uint16_t length;
} nuwax_emotion_delta_run_header_t;

typedef struct {
    uint16_t run_count;
    uint32_t changed_count;
    bool absolute;
} nuwax_emotion_delta_record_info_t;

static inline bool nuwax_emotion_delta_record_validate(
    const uint8_t *record,
    size_t record_size,
    uint16_t palette_count,
    bool expect_absolute,
    nuwax_emotion_delta_record_info_t *info_out)
{
    if (!record || palette_count == 0U || palette_count > UINT8_MAX ||
        record_size < sizeof(nuwax_emotion_delta_frame_header_t) ||
        record_size > NUWAX_EMOTION_DELTA_MAX_RECORD_BYTES) {
        return false;
    }

    nuwax_emotion_delta_frame_header_t header;
    __builtin_memcpy(&header, record, sizeof(header));
    const bool absolute =
        (header.flags & NUWAX_EMOTION_DELTA_FRAME_FLAG_ABSOLUTE) != 0U;
    if ((header.flags & ~NUWAX_EMOTION_DELTA_FRAME_FLAG_ABSOLUTE) != 0U ||
        absolute != expect_absolute ||
        header.run_count > NUWAX_EMOTION_DELTA_MAX_FRAME_RUNS ||
        (absolute && header.run_count == 0U) ||
        header.changed_count > NUWAX_EMOTION_DELTA_FRAME_PIXELS ||
        (header.run_count == 0U && header.changed_count != 0U)) {
        return false;
    }

    size_t cursor = sizeof(header);
    uint32_t previous_end = 0U;
    uint32_t encoded_pixels = 0U;
    for (uint16_t run_index = 0; run_index < header.run_count; ++run_index) {
        if (cursor > record_size ||
            record_size - cursor < sizeof(nuwax_emotion_delta_run_header_t)) {
            return false;
        }
        nuwax_emotion_delta_run_header_t run;
        __builtin_memcpy(&run, record + cursor, sizeof(run));
        cursor += sizeof(run);
        const uint64_t run_end = (uint64_t)run.offset + run.length;
        if (run.length == 0U || run.offset < previous_end ||
            run_end > NUWAX_EMOTION_DELTA_FRAME_PIXELS ||
            (!absolute &&
             run.offset / NUWAX_EMOTION_DELTA_FRAME_WIDTH !=
                 ((uint32_t)run_end - 1U) /
                     NUWAX_EMOTION_DELTA_FRAME_WIDTH) ||
            run.length > record_size - cursor) {
            return false;
        }
        for (uint16_t i = 0; i < run.length; ++i) {
            if (record[cursor + i] >= palette_count) {
                return false;
            }
        }
        cursor += run.length;
        previous_end = (uint32_t)run_end;
        encoded_pixels += run.length;
    }

    if (cursor != record_size || encoded_pixels != header.changed_count ||
        (absolute &&
         (encoded_pixels != NUWAX_EMOTION_DELTA_FRAME_PIXELS ||
          previous_end != NUWAX_EMOTION_DELTA_FRAME_PIXELS))) {
        return false;
    }
    if (info_out) {
        *info_out = (nuwax_emotion_delta_record_info_t){
            .run_count = header.run_count,
            .changed_count = header.changed_count,
            .absolute = absolute,
        };
    }
    return true;
}

_Static_assert(sizeof(nuwax_emotion_delta_frame_header_t) == 8U,
               "v2 frame header layout");
_Static_assert(sizeof(nuwax_emotion_delta_run_header_t) == 6U,
               "v2 run header layout");
