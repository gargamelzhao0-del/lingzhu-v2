#pragma once
#include <stdio.h>
#include <limits.h>
#include <string.h>
#include "cloud_time.h"
#include "sd_audio_time.h"
#include "sd_audio_seal_head.h"

/* NWXOFF1 stays 96 bytes. Its former 12 reserved bytes are uint64 UTC
 * milliseconds at 84 and uint32 capture span at 92, both little endian.
 * Zero means unknown; duration is the span, not frame_count * 20. */
static inline uint64_t sd_audio_file_started_at(const sd_audio_session_time_t *time,
    uint64_t session_start_ms, uint64_t file_start_ms)
{
    if (!time || !(time->valid & SD_AUDIO_TIME_STARTED)) return 0;
    const nuwax_cloud_time_anchor_t anchor = {time->started_at_ms, session_start_ms};
    uint64_t utc = 0;
    return nuwax_cloud_time_project(&anchor, file_start_ms, &utc) ? utc : 0;
}

static inline uint32_t sd_audio_file_duration(uint64_t start_ms, uint64_t end_ms)
{
    return end_ms > start_ms && end_ms - start_ms <= UINT32_MAX
        ? (uint32_t)(end_ms - start_ms) : 0;
}

/* Used only before sealing a .part. A ready/attempted file is immutable. */
static inline bool sd_audio_file_time_write(FILE *file, uint64_t started_at_ms,
    uint32_t duration_ms)
{
    const long position = ftell(file);
    uint8_t bytes[12];
    for (unsigned i = 0; i < 8; ++i) bytes[i] = (uint8_t)(started_at_ms >> (8U * i));
    for (unsigned i = 0; i < 4; ++i) bytes[8 + i] = (uint8_t)(duration_ms >> (8U * i));
    return position >= 96 && fseek(file, 84, SEEK_SET) == 0 &&
        fwrite(bytes, 1, sizeof(bytes), file) == sizeof(bytes) &&
        fseek(file, position, SEEK_SET) == 0;
}

/* A live writer owns a CRC-protected original prefix. Rewrite every known byte
 * at offset zero instead of read/modify/writing just 12 bytes in an old FIL
 * sector cache. Short archives write their whole known prefix, never padding
 * over the footer or changing the file size. Keep the caller's append cursor. */
static inline bool sd_audio_file_head_write(FILE *file, const uint8_t *prefix, size_t bytes)
{
    if (!file || !prefix || bytes < 96U || bytes > 512U) return false;
    const long position = ftell(file);
    return position >= (long)bytes && fseek(file, 0L, SEEK_SET) == 0 &&
        fwrite(prefix, 1U, bytes, file) == bytes &&
        fflush(file) == 0 && fseek(file, position, SEEK_SET) == 0;
}

static inline bool sd_audio_file_tail_write(FILE *file, uint64_t body_end,
    uint64_t offset, const uint8_t *bytes, size_t count)
{
    if (!file || !bytes || !count || (offset & 511U) || offset > body_end ||
        body_end > LONG_MAX - 32U || count != body_end + 32U - offset ||
        ftell(file) != (long)body_end) return false;
    return fseek(file, (long)offset, SEEK_SET) == 0 &&
        fwrite(bytes, 1U, count, file) == count &&
        ftell(file) == (long)(body_end + 32U);
}

/* Recovery uses retained frame timestamps, never the new boot's uptime.
 * Per-frame unsigned deltas handle the 32-bit timestamp wrap. */
typedef struct {
    uint64_t duration_ms;
    uint32_t previous_ms;
    bool seen;
    bool invalid;
} sd_audio_file_span_t;

static inline void sd_audio_file_span_add(sd_audio_file_span_t *span,
    uint32_t timestamp_ms, uint64_t start_ms, uint16_t frame_ms)
{
    if (span->invalid) return;
    if (!span->seen) {
        span->invalid = timestamp_ms != (uint32_t)start_ms || !frame_ms;
        span->duration_ms = frame_ms;
        span->seen = true;
    } else {
        const uint32_t delta = timestamp_ms - span->previous_ms;
        span->invalid = delta < frame_ms || delta >= UINT32_C(0x80000000);
        span->duration_ms += delta;
    }
    span->previous_ms = timestamp_ms;
    if (span->duration_ms > UINT32_MAX) span->invalid = true;
}
