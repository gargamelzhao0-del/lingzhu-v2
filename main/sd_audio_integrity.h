#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "board/sd_integrity_io.h"

#define SD_INTEGRITY_SHADOW_BYTES (96U + 100U * (4U + 662U))
#define SD_INTEGRITY_BLOCK_BYTES 512U
typedef uint32_t (*sd_integrity_crc_t)(uint32_t, const void *, size_t);
typedef bool (*sd_integrity_yield_t)(void *);
enum { SD_INTEGRITY_QUEUE = 1, SD_INTEGRITY_WRITE = 2, SD_INTEGRITY_RECORD = 3,
       SD_INTEGRITY_SEAL = 4, SD_INTEGRITY_SEAL_SIZE = 5 };
typedef struct {
    sd_integrity_crc_t crc;
    bool active, frozen, saved, shadow_covered, queue_valid, seal_prepared;
    bool repair_attempted, repaired_file;
    const char *save_stage;
    int save_errno;
    uint32_t generation, index, stage, tick_ms;
    uint32_t queue_crc, before_crc, after_crc, expected_crc, observed_crc, shadow_crc, snapshot_crc;
    uint32_t shadow_bytes, observed_bytes, observed_relative, first_difference, differing_bytes;
    uint32_t records, first_sequence, last_sequence, save_attempts;
    uint64_t offset, body_end, observed_file_size;
    size_t head_bytes, tail_bytes;
    char session_id[48], path[128];
    nuwax_sd_integrity_io_report_t io;
    uint8_t head[512], tail[512], observed[512];
    uint8_t shadow[SD_INTEGRITY_SHADOW_BYTES];
} sd_integrity_t;

sd_integrity_t *sd_integrity_create(sd_integrity_crc_t crc);
void sd_integrity_destroy(sd_integrity_t *state);
bool sd_integrity_begin(sd_integrity_t *state, const char *session, const char *path,
    uint32_t generation, uint32_t index, const void *header, size_t bytes);
void sd_integrity_record(sd_integrity_t *state, uint32_t length, const void *frame,
    uint32_t queued_crc, bool queued_valid, uint32_t before_crc);
void sd_integrity_read_attempt(sd_integrity_t *state);
void sd_integrity_compare(sd_integrity_t *state, const void *bytes, size_t count, uint32_t relative);
void sd_integrity_verified(sd_integrity_t *state, uint64_t end);
void sd_integrity_freeze(sd_integrity_t *state, uint32_t stage, uint32_t tick_ms,
    uint32_t expected_crc, uint32_t observed_crc);
void sd_integrity_prepare_seal(sd_integrity_t *state, uint64_t started_at_ms,
    uint32_t duration_ms, const void *footer, size_t bytes);
void sd_integrity_seal_difference(sd_integrity_t *state, uint64_t offset,
    const void *expected, const void *observed, size_t bytes);
/* Frozen evidence only. Fixed four slots, exclusive create, no deletion of
 * existing evidence/audio; caller owns a bounded diagnostic SDK I/O scope. */
esp_err_t sd_integrity_save(sd_integrity_t *state, const char *directory, char *saved_path, size_t capacity);
esp_err_t sd_integrity_save_cooperative(sd_integrity_t *state, const char *directory,
    char *saved_path, size_t capacity, sd_integrity_yield_t yield_requested, void *ctx);
