#pragma once

/* The replay FILE has not been read yet. Positional metadata reads leave its
 * descriptor position and 16 KiB stdio buffer untouched, so v1 validation and
 * sequential replay keep their existing read-ahead. Only the 96-byte header
 * and 32-byte footer are requested, instead of filling that buffer twice. */
static esp_err_t replay_metadata_read_at(FILE *file, void *destination,
                                         size_t bytes, off_t offset)
{
    uint8_t *out = destination;
    size_t done = 0U;
    while (done < bytes) {
        const ssize_t got = pread(fileno(file), out + done, bytes - done,
                                  offset + (off_t)done);
        if (got < 0) {
            if (errno == EINTR) continue;
            return ESP_FAIL;
        }
        if (got == 0) return ESP_ERR_INVALID_SIZE;
        done += (size_t)got;
    }
    return ESP_OK;
}
