#include "sd_audio_integrity.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#if CONFIG_NUWAX_SD_DIRECTORY_GUARD_ENABLE
#include "board/sd_card_driver.h"
#endif
#elif defined(_WIN32)
#include <io.h>
#include <direct.h>
#endif

sd_integrity_t *sd_integrity_create(sd_integrity_crc_t crc)
{
    if (!crc) return NULL;
#ifdef ESP_PLATFORM
    sd_integrity_t *state = heap_caps_calloc(1U, sizeof(*state), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    sd_integrity_t *state = calloc(1U, sizeof(*state));
#endif
    if (state) state->crc = crc;
    return state;
}
void sd_integrity_destroy(sd_integrity_t *state)
{
#ifdef ESP_PLATFORM
    heap_caps_free(state);
#else
    free(state);
#endif
}

static void retain_edges(sd_integrity_t *state, const uint8_t *bytes, size_t count)
{
    size_t head = sizeof(state->head) - state->head_bytes;
    if (head > count) head = count;
    memcpy(state->head + state->head_bytes, bytes, head); state->head_bytes += head;
    if (count >= sizeof(state->tail)) {
        memcpy(state->tail, bytes + count - sizeof(state->tail), sizeof(state->tail));
        state->tail_bytes = sizeof(state->tail);
    } else {
        const size_t keep = state->tail_bytes < sizeof(state->tail) - count
            ? state->tail_bytes : sizeof(state->tail) - count;
        memmove(state->tail, state->tail + state->tail_bytes - keep, keep);
        memcpy(state->tail + keep, bytes, count); state->tail_bytes = keep + count;
    }
}

bool sd_integrity_begin(sd_integrity_t *state, const char *session, const char *path,
    uint32_t generation, uint32_t index, const void *header, size_t bytes)
{
    if (!state || !header || bytes > sizeof(state->shadow) || (state->frozen && !state->saved)) return false;
    const sd_integrity_crc_t crc = state->crc;
    memset(state, 0, offsetof(sd_integrity_t, shadow)); state->crc = crc;
    state->active = state->shadow_covered = true;
    state->generation = generation; state->index = index; state->first_difference = UINT32_MAX;
    snprintf(state->session_id, sizeof(state->session_id), "%s", session);
    snprintf(state->path, sizeof(state->path), "%s", path);
    memcpy(state->shadow, header, bytes); state->shadow_bytes = (uint32_t)bytes;
    state->shadow_crc = crc(0U, state->shadow, bytes); state->body_end = bytes;
    retain_edges(state, header, bytes);
    return true;
}

void sd_integrity_record(sd_integrity_t *state, uint32_t length, const void *frame,
    uint32_t queued_crc, bool queued_valid, uint32_t before_crc)
{
    if (!state || !state->active || state->frozen) return;
    state->queue_crc = queued_crc; state->queue_valid = queued_valid;
    state->before_crc = before_crc; state->after_crc = before_crc;
    uint32_t sequence = 0U;
    if (length >= 14U) memcpy(&sequence, (const uint8_t *)frame + 10U, sizeof(sequence));
    if (!state->records) state->first_sequence = sequence;
    state->last_sequence = sequence; ++state->records;
    state->body_end += sizeof(length) + length;
    retain_edges(state, (const uint8_t *)&length, sizeof(length));
    retain_edges(state, frame, length);
    if (length > sizeof(state->shadow) - sizeof(length) ||
        state->shadow_bytes > sizeof(state->shadow) - sizeof(length) - length) {
        state->shadow_covered = false; return;
    }
    uint8_t *record = state->shadow + state->shadow_bytes;
    memcpy(record, &length, sizeof(length)); memcpy(record + sizeof(length), frame, length);
    state->shadow_crc = state->crc(state->shadow_crc, record, sizeof(length) + length);
    state->shadow_bytes += (uint32_t)sizeof(length) + length;
}

void sd_integrity_read_attempt(sd_integrity_t *state)
{
    if (!state || state->frozen) return;
    state->first_difference = UINT32_MAX; state->differing_bytes = state->observed_bytes = state->observed_relative = 0U;
}
void sd_integrity_compare(sd_integrity_t *state, const void *opaque, size_t bytes, uint32_t relative)
{
    if (!state || !state->active || state->frozen || !state->shadow_covered ||
        relative > state->shadow_bytes || bytes > state->shadow_bytes - relative) return;
    const uint8_t *observed = opaque, *expected = state->shadow + relative;
    for (size_t n = 0U; n < bytes; ++n) if (expected[n] != observed[n]) {
        if (state->first_difference == UINT32_MAX) {
            state->first_difference = relative + (uint32_t)n;
            state->observed_relative = relative;
            const size_t keep = bytes < sizeof(state->observed) ? bytes : sizeof(state->observed);
            memcpy(state->observed, observed, keep); state->observed_bytes = (uint32_t)keep;
        }
        ++state->differing_bytes;
    }
}
void sd_integrity_verified(sd_integrity_t *state, uint64_t end)
{
    if (!state || !state->active || state->frozen) return;
    state->offset = state->body_end = end;
    state->shadow_bytes = state->shadow_crc = state->records = 0U;
    state->first_difference = UINT32_MAX; state->differing_bytes = state->observed_bytes = 0U;
    state->shadow_covered = true;
    state->repair_attempted = false;
}
void sd_integrity_freeze(sd_integrity_t *state, uint32_t stage, uint32_t tick,
    uint32_t expected, uint32_t observed)
{
    if (!state || !state->active || state->frozen) return;
    state->stage = stage; state->tick_ms = tick; state->expected_crc = expected;
    state->observed_crc = observed; state->frozen = true;
    /* Recompute the retained copy independently; never replace application CRC. */
    state->snapshot_crc = state->crc(0U, state->shadow, state->shadow_bytes);
}

static void little32(uint8_t *out, uint32_t value)
{ for (unsigned i = 0U; i < 4U; ++i) out[i] = (uint8_t)(value >> (i * 8U)); }
static void little64(uint8_t *out, uint64_t value)
{ for (unsigned i = 0U; i < 8U; ++i) out[i] = (uint8_t)(value >> (i * 8U)); }

void sd_integrity_prepare_seal(sd_integrity_t *state, uint64_t start, uint32_t duration,
    const void *footer, size_t bytes)
{
    if (!state || !state->active || state->frozen || state->head_bytes < 96U ||
        state->tail_bytes != 512U || bytes != 32U) return;
    little64(state->head + 84U, start); little32(state->head + 92U, duration);
    if (!state->seal_prepared) memmove(state->tail, state->tail + bytes, sizeof(state->tail) - bytes);
    memcpy(state->tail + sizeof(state->tail) - bytes, footer, bytes);
    state->seal_prepared = true;
}
void sd_integrity_seal_difference(sd_integrity_t *state, uint64_t offset,
    const void *expected, const void *observed, size_t bytes)
{
    if (!state || !state->active || state->frozen || bytes > 512U) return;
    memcpy(state->shadow, expected, bytes); state->shadow_bytes = (uint32_t)bytes;
    state->offset = offset; state->shadow_crc = state->crc(0U, expected, bytes);
    state->shadow_covered = true; sd_integrity_read_attempt(state);
    sd_integrity_compare(state, observed, bytes, 0U);
}

static bool sync_evidence(FILE *file)
{
    if (fflush(file) != 0) return false;
#ifdef _WIN32
    return _commit(fileno(file)) == 0;
#else
    return fsync(fileno(file)) == 0;
#endif
}

static esp_err_t evidence_write(FILE *file, const void *data, size_t bytes,
    sd_integrity_yield_t yield_requested, void *ctx)
{
    const uint8_t *p = data;
    while (bytes) {
        if (yield_requested && yield_requested(ctx)) return ESP_ERR_NOT_FINISHED;
        const size_t count = bytes < 4096U ? bytes : 4096U;
        if (fwrite(p, 1U, count, file) != count) return ESP_FAIL;
        p += count; bytes -= count;
    }
    return ESP_OK;
}

esp_err_t sd_integrity_save_cooperative(sd_integrity_t *state, const char *directory,
    char *saved_path, size_t capacity, sd_integrity_yield_t yield_requested, void *ctx)
{
    if (!state || !state->frozen || state->saved || !directory) return ESP_ERR_INVALID_STATE;
    state->save_stage = "admission"; state->save_errno = 0;
    if (yield_requested && yield_requested(ctx)) return ESP_ERR_NOT_FINISHED;
    /* Failure export must not create evidence inside a structurally damaged
     * directory, even if the FAT mount and free-space query succeeded. */
#if defined(ESP_PLATFORM) && CONFIG_NUWAX_SD_DIRECTORY_GUARD_ENABLE
    state->save_stage = "directory_preflight";
    esp_err_t directory_err = nuwax_sd_card_validate_evidence_directory(directory, true);
    if (directory_err != ESP_OK) return directory_err;
#endif
    state->save_stage = "mkdir";
#ifdef _WIN32
    if (_mkdir(directory) != 0 && errno != EEXIST) { state->save_errno = errno; return ESP_FAIL; }
#else
    if (mkdir(directory, 0700) != 0 && errno != EEXIST) { state->save_errno = errno; return ESP_FAIL; }
#endif
#if defined(ESP_PLATFORM) && CONFIG_NUWAX_SD_DIRECTORY_GUARD_ENABLE
    directory_err = nuwax_sd_card_validate_evidence_directory(directory, false);
    if (directory_err != ESP_OK) return directory_err;
#endif
    char part[192], final[192]; int fd = -1;
    for (unsigned slot = 0U; slot < 4U; ++slot) {
        state->save_stage = "reserve";
        if (yield_requested && yield_requested(ctx)) return ESP_ERR_NOT_FINISHED;
        const int a = snprintf(part, sizeof(part), "%s/evidence_%u.part", directory, slot);
        const int b = snprintf(final, sizeof(final), "%s/evidence_%u.bin", directory, slot);
        if (a < 0 || a >= (int)sizeof(part) || b < 0 || b >= (int)sizeof(final)) return ESP_ERR_INVALID_SIZE;
        struct stat st;
        if (stat(final, &st) == 0) continue;
        if (errno != ENOENT) { state->save_errno = errno; return ESP_FAIL; }
        int flags = O_CREAT | O_EXCL | O_RDWR;
#ifdef O_BINARY
        flags |= O_BINARY;
#endif
        fd = open(part, flags, 0600);
        if (fd >= 0) break;
        if (errno != EEXIST) { state->save_errno = errno; return ESP_FAIL; }
    }
    if (fd < 0) return ESP_ERR_NO_MEM; /* All four evidence slots occupied. */
    FILE *file = fdopen(fd, "w+b");
    if (!file) {
        const int error = errno;
        if (close(fd) != 0) { state->save_stage = "close"; state->save_errno = errno; return ESP_FAIL; }
        state->save_stage = "fdopen"; state->save_errno = error; errno = error; return ESP_FAIL;
    }
    uint8_t header[512] = {0}; memcpy(header, "NWXDIA2", 7U);
    little32(header + 8U, 1U); little32(header + 12U, sizeof(header));
    little64(header + 16U, state->offset);
    const uint32_t values[] = {state->shadow_bytes, state->observed_bytes, state->expected_crc,
        state->observed_crc, state->shadow_crc, state->queue_crc, state->before_crc,
        state->after_crc, state->first_difference, state->differing_bytes, state->stage,
        (state->shadow_covered ? 1U : 0U) | (state->queue_valid ? 2U : 0U), state->generation,
        state->index, state->tick_ms};
    for (unsigned i = 0U; i < sizeof(values)/sizeof(values[0]); ++i) little32(header + 24U + i*4U, values[i]);
    little32(header + 84U, state->snapshot_crc);
    little64(header + 88U, state->body_end);
    little64(header + 96U, state->observed_file_size);
    little32(header + 104U, state->observed_relative);
    memcpy(header + 128U, state->session_id, sizeof(state->session_id));
    memcpy(header + 176U, state->path, sizeof(state->path));
    little64(header + 304U, state->io.bad_lba);
    const uint32_t io[] = {state->io.expected_crc, state->io.observed_crc, state->io.raw_crc,
        (uint32_t)state->io.raw_error, state->io.covered_sectors, state->io.unknown_sectors,
        state->io.evictions, state->io.foreign_writes, state->io.sdk_errors, state->first_sequence,
        state->last_sequence, state->records, state->io.source_mutations, state->io.write_calls,
        state->io.read_calls, state->io.matched_sectors, state->io.mismatched_sectors,
        state->io.raw_attempts, state->io.raw_valid ? 1U : 0U,
        state->crc(0U, state->shadow, state->shadow_bytes),
        state->crc(0U, state->observed, state->observed_bytes)};
    for (unsigned i = 0U; i < sizeof(io)/sizeof(io[0]); ++i) little32(header + 312U + i*4U, io[i]);
    little32(header + 508U, state->crc(0U, header, 508U));
    state->save_stage = "write";
    esp_err_t result = evidence_write(file, header, sizeof(header), yield_requested, ctx);
    if (result == ESP_OK) result = evidence_write(file, state->shadow, state->shadow_bytes, yield_requested, ctx);
    if (result == ESP_OK) result = evidence_write(file, state->observed, state->observed_bytes, yield_requested, ctx);
    if (result == ESP_OK) {
        state->save_stage = "sync";
        if (yield_requested && yield_requested(ctx)) result = ESP_ERR_NOT_FINISHED;
        else if (!sync_evidence(file)) result = ESP_FAIL;
    }
    if (result == ESP_FAIL) state->save_errno = errno;
    /* Never expire an SDK scope with a dirty FILE still alive. A cooperative
     * yield stops new writes, then completes normal FatFs close/cleanup. */
    const int closed = fclose(file);
    if (closed != 0) { state->save_stage = "close"; state->save_errno = errno; return ESP_FAIL; }
    if (result != ESP_OK) {
        if (result == ESP_ERR_NOT_FINISHED) state->save_stage = "yield";
        return result;
    }
    state->save_stage = "rename";
    if (rename(part, final) != 0) { state->save_errno = errno; return ESP_FAIL; }
    state->saved = true;
    state->save_stage = "saved";
    if (saved_path && capacity) snprintf(saved_path, capacity, "%s", final);
    return ESP_OK;
}

esp_err_t sd_integrity_save(sd_integrity_t *state, const char *directory, char *saved_path, size_t capacity)
{
    return sd_integrity_save_cooperative(state, directory, saved_path, capacity, NULL, NULL);
}
