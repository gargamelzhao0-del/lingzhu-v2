#ifndef SD_ARCHIVE_STORE_H
#define SD_ARCHIVE_STORE_H

/*
 * Host-side preparation API for the future SD monitoring archive.
 *
 * This header deliberately has no ESP-IDF or product include dependency. It
 * is not registered as an ESP-IDF component and is safe to test in isolation.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define SD_ARCHIVE_ID_MAX 47U
#define SD_ARCHIVE_SESSION_ID_MAX 47U
#define SD_ARCHIVE_PATH_MAX 512U

enum {
    SD_ARCHIVE_OK = 0,
    SD_ARCHIVE_INVALID_ARGUMENT = -1,
    SD_ARCHIVE_IO_ERROR = -2,
    SD_ARCHIVE_NO_SPACE = -3,
    SD_ARCHIVE_CORRUPT = -4,
    SD_ARCHIVE_BUSY = -5,
    SD_ARCHIVE_NOT_FOUND = -6,
};

typedef struct {
    const char *root_dir;
    uint64_t max_bytes;          /* 0 means no local quota. */
    uint32_t max_segment_bytes;  /* 0 uses the default 4 MiB limit. */
} sd_archive_config_t;

typedef struct {
    char archive_id[SD_ARCHIVE_ID_MAX + 1U];
    char audio_session_id[SD_ARCHIVE_SESSION_ID_MAX + 1U];
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t codec;
    uint16_t frame_duration_ms;
    uint64_t start_unix_ms;
} sd_archive_metadata_t;

typedef struct {
    char filename[SD_ARCHIVE_PATH_MAX];
    uint64_t bytes;
    uint32_t frame_count;
    uint64_t payload_bytes;
    uint32_t crc32;
    bool complete;
} sd_archive_segment_info_t;

typedef struct {
    char root_dir[SD_ARCHIVE_PATH_MAX];
    uint64_t max_bytes;
    uint32_t max_segment_bytes;
    uint32_t next_segment_index;
    FILE *open_file;
    char open_path[SD_ARCHIVE_PATH_MAX];
    char ready_path[SD_ARCHIVE_PATH_MAX];
    sd_archive_metadata_t metadata;
    uint32_t segment_index;
    uint32_t frame_count;
    uint64_t payload_bytes;
    uint32_t crc32;
    uint64_t used_bytes;
    uint32_t ready_count;
    uint32_t instance_magic;
    bool active;
    bool initialized;
} sd_archive_store_t;

int sd_archive_store_init(sd_archive_store_t *store,
                          const sd_archive_config_t *config);

/* Releases the store handle. The store must not have an active segment. */
int sd_archive_store_deinit(sd_archive_store_t *store);

/* Starts one segment. A later append automatically rotates when the size
 * limit is reached. Only one segment may be open per store instance. */
int sd_archive_store_begin(sd_archive_store_t *store,
                           const sd_archive_metadata_t *metadata);

/* Appends one complete NA-v1 binary frame. The frame is stored byte-for-byte
 * after a length prefix; this prototype does not reinterpret audio payloads.
 * A segment is committed and a new one is opened when the segment limit is
 * reached. */
int sd_archive_store_append_frame(sd_archive_store_t *store,
                                  const void *frame,
                                  size_t frame_bytes);

/* Writes a footer, flushes the file, and atomically renames .open to .ready.
 * complete=false is used for a network/power-loss boundary. */
int sd_archive_store_commit(sd_archive_store_t *store, bool complete);

/* Closes an active .open file without making it uploadable. A later recovery
 * pass can validate and promote it to .ready with complete=false. */
int sd_archive_store_abort(sd_archive_store_t *store);

/* Scans the root for orphan .open segments, truncates an incomplete final
 * record, writes an incomplete footer, and promotes each file to .ready. */
int sd_archive_store_recover_open(sd_archive_store_t *store,
                                  uint32_t *recovered_count);

int sd_archive_store_get_usage(const sd_archive_store_t *store,
                               uint64_t *used_bytes,
                               uint32_t *ready_count);

/* Validates the footer of one committed segment and returns its counters. */
int sd_archive_store_inspect_ready(const sd_archive_store_t *store,
                                   const char *filename,
                                   sd_archive_segment_info_t *info);

/* Deletes a segment only after the cloud has acknowledged its archive_id and
 * segment index. The caller must pass the exact filename returned by listing. */
int sd_archive_store_remove_ready(sd_archive_store_t *store,
                                  const char *filename);

uint32_t sd_archive_crc32_update(uint32_t crc,
                                 const void *data,
                                 size_t bytes);

#endif
