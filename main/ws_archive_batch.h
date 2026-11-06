#pragma once
#include <stddef.h>
#include <stdint.h>

#define NUWAX_WS_ARCHIVE_BATCH_MAX_FRAMES 4U
typedef struct {
    const uint8_t *header; /* Existing 64-byte archive chunk header. */
    const uint8_t *data;
    size_t bytes;
} nuwax_ws_archive_frame_t;
