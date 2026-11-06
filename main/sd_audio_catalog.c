#include "sd_audio_catalog.h"
#include "sd_audio_monitor_fifo.h"
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "board/sd_dma_bulk.h"
#include "board/sd_probe_trace.h"
#include "board/sd_card_driver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define fsync _commit
#define mkdir(path, mode) _mkdir(path)
#endif

#ifndef SD_AUDIO_CATALOG_ROOT
#define SD_AUDIO_CATALOG_ROOT "/sdcard/audio_sessions"
#endif
#define CATALOG_MAGIC 0x3153584eU
#define SEGMENT_MAGIC 0x3147584eU
#define CATALOG_INDEX_SLICE 16U
#define CATALOG_MANIFEST_SLICE 16U

/* Only the replay worker changes these per-pass cursors, under the storage
 * lock. A rejected scene is retried next pass; later scenes can still drain. */
#define CATALOG_DEFERRED_MAX 16U
static char deferred_ids[CATALOG_DEFERRED_MAX][48];
static unsigned deferred_count;
/* When a pass exhausts its bounded skip set, the next pass starts AFTER the
 * failed key and wraps around. This is ordering, never an exclusion boundary:
 * boot-relative timestamps may overlap and every eligible scene stays visible.
 * Kept across worker restarts, reset on media initialization; no disk writes. */
typedef struct {
    char id[48];
    uint8_t owner[32];
    uint64_t ended_ms;
} replay_rotation_t;
static replay_rotation_t replay_rotation[2];
/* Cache identities and scan hints; CRC-protected manifests are read for each
 * selection. New/changed scenes invalidate both picks so chronological and
 * mode fairness cannot be bypassed. No heap, open directory or FILE retained. */
static char replay_pick[2][48];
static uint8_t replay_pick_owner[2][32];
static bool replay_empty[2];
/* Volatile scan hints, valid only with the same CRC-checked manifest
 * generation. The durable per-segment ACK remains authoritative. */
static uint32_t replay_cursor[2], replay_generation[2];
/* Cold selection scans are also sliced so a large SD backlog does not keep
 * the recording writer's lock for a full directory's manifest I/O. No DIR
 * handle survives a yield. Any catalog mutation restarts these snapshots. */
typedef struct {
    bool active, found, has_lower, mode_candidate[2];
    uint8_t owner[32];
    char last_id[48], lower_id[48];
    uint64_t lower_ended_ms;
    sd_audio_session_t best;
} replay_scan_t;
static replay_scan_t replay_scans[3]; /* Meeting, monitoring, unrestricted. */
static uint32_t replay_scan_generation;
/* A single-use selection snapshot. Every catalog mutation/reset invalidates
 * it under the same storage lock. The sealed file is still opened and fully
 * validated by replay; this only avoids reading the same two CRC records
 * again between selection and begin. No FILE or heap allocation is retained. */
static sd_audio_session_t selected_session;
static sd_audio_segment_t selected_segment;
static bool selected_valid;
static char acked_id[48];
static uint32_t acked_index;

/* Completed scenes are immutable for replay. Retain exact IDs, never hashes:
 * a collision must not hide unacknowledged audio. Optional PSRAM only on the
 * device; allocation failure/eviction falls back to CRC-checked disk reads.
 * The storage lock serializes this cache with every catalog operation. */
#define CATALOG_DONE_CACHE_COUNT 128U
static char (*done_ids)[48];
static unsigned done_count, done_replace;
static sd_audio_catalog_stats_t catalog_stats;
/* Storage-lock protected, invalidated before every physical remount. */
static bool metadata_io_qualified;
static uint32_t metadata_probe_ticket;
/* A hint version, never an ACK or authorization decision. */
static atomic_uint live_revision = ATOMIC_VAR_INIT(1U);
uint32_t sd_audio_catalog_live_revision(void)
{
    return atomic_load_explicit(&live_revision, memory_order_acquire);
}
static void invalidate_live_hint(void)
{
    atomic_fetch_add_explicit(&live_revision, 1U, memory_order_release);
}

static void done_cache_reset(void)
{
    done_count = done_replace = 0U;
    if (!done_ids) {
#ifdef ESP_PLATFORM
        done_ids = heap_caps_malloc(CATALOG_DONE_CACHE_COUNT * sizeof(*done_ids),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
        done_ids = malloc(CATALOG_DONE_CACHE_COUNT * sizeof(*done_ids));
#endif
    }
}

static bool done_cache_has(const char *id)
{
    for (unsigned i = 0U; i < done_count; ++i)
        if (!strcmp(done_ids[i], id)) return true;
    return false;
}

static void done_cache_forget(const char *id)
{
    for (unsigned i = 0U; i < done_count; ++i) {
        if (strcmp(done_ids[i], id)) continue;
        --done_count;
        if (i != done_count) memcpy(done_ids[i], done_ids[done_count], sizeof(*done_ids));
        done_replace = 0U;
        break;
    }
}

static void done_cache_remember(const sd_audio_session_t *session)
{
    if (!done_ids || !session->done || done_cache_has(session->session_id)) return;
    const unsigned slot = done_count < CATALOG_DONE_CACHE_COUNT ? done_count++ :
        done_replace++ % CATALOG_DONE_CACHE_COUNT;
    memcpy(done_ids[slot], session->session_id, sizeof(*done_ids));
}

static bool done_cache_skip(const char *id)
{
    if (!done_cache_has(id)) return false;
    ++catalog_stats.completed_hits;
    return true;
}

void sd_audio_catalog_stats(sd_audio_catalog_stats_t *stats)
{
    if (stats) *stats = catalog_stats;
}

static void monitor_fifo_metadata_changed(const char *id);
static esp_err_t catalog_segment_backup_write(const char *id, const sd_audio_segment_t *segment);

static void invalidate_selection(void)
{
    selected_valid = false;
    acked_id[0] = '\0';
}

static void invalidate_replay_picks(void)
{
    invalidate_live_hint();
    invalidate_selection();
    memset(replay_pick, 0, sizeof(replay_pick));
    memset(replay_empty, 0, sizeof(replay_empty));
    memset(replay_cursor, 0, sizeof(replay_cursor));
    memset(replay_generation, 0, sizeof(replay_generation));
    memset(replay_scans, 0, sizeof(replay_scans));
    ++replay_scan_generation;
}

void sd_audio_catalog_replay_reset(void)
{
    deferred_count = 0;
    invalidate_replay_picks();
}

static int session_order(const sd_audio_session_t *a, const sd_audio_session_t *b, bool rotate)
{
    if (rotate && a->mode == b->mode && a->mode < 2U) {
        const replay_rotation_t *rotation = &replay_rotation[a->mode];
        if (rotation->id[0] && !memcmp(a->owner, rotation->owner, 32) &&
            !memcmp(b->owner, rotation->owner, 32)) {
            const bool a_after = a->ended_ms > rotation->ended_ms ||
                (a->ended_ms == rotation->ended_ms && strcmp(a->session_id, rotation->id) > 0);
            const bool b_after = b->ended_ms > rotation->ended_ms ||
                (b->ended_ms == rotation->ended_ms && strcmp(b->session_id, rotation->id) > 0);
            if (a_after != b_after) return a_after ? -1 : 1;
        }
    }
    if (a->ended_ms != b->ended_ms) return a->ended_ms < b->ended_ms ? -1 : 1;
    return strcmp(a->session_id, b->session_id);
}

static bool session_blocked(const char *id);

static bool session_deferred(const sd_audio_session_t *session)
{
    if (session_blocked(session->session_id)) return true;
    for (unsigned i = 0; i < deferred_count; ++i)
        if (!strcmp(session->session_id, deferred_ids[i])) return true;
    return false;
}

esp_err_t sd_audio_catalog_defer(const sd_audio_session_t *session)
{
    if (!session || session->mode > 1U || !session->session_id[0] ||
        !memchr(session->session_id, '\0', sizeof(session->session_id))) return ESP_ERR_INVALID_ARG;
    if (session_deferred(session)) return ESP_OK;
    /* Boot-relative clocks cannot define a skip boundary: a failed old
     * scene must not hide live scenes (ended_ms=0) or a later boot. */
    if (deferred_count == CATALOG_DEFERRED_MAX) {
        replay_rotation_t *rotation = &replay_rotation[session->mode];
        memcpy(rotation->id, session->session_id, sizeof(rotation->id));
        memcpy(rotation->owner, session->owner, sizeof(rotation->owner));
        rotation->ended_ms = session->ended_ms;
        invalidate_replay_picks();
        return ESP_ERR_NO_MEM;
    }
    strcpy(deferred_ids[deferred_count++], session->session_id);
    invalidate_replay_picks();
    return ESP_OK;
}

static uint32_t checksum(const void *data, size_t bytes)
{
    const uint8_t *p = data;
    uint32_t crc = ~0U;
    while (bytes--) {
        crc ^= *p++;
        for (unsigned i = 0; i < 8; ++i)
            crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

static bool valid_id(const char *id)
{
    if (!id || !id[0] || strlen(id) >= 48U) return false;
    for (const char *p = id; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return false;
    return true;
}

static bool catalog_path(char path[128], const char *id, const char *suffix)
{
    if (!valid_id(id)) return false;
    const int n = snprintf(path, 128U, "%s/%s.%s", SD_AUDIO_CATALOG_ROOT, id, suffix);
    return n > 0 && n < 128;
}

/* A rejection marker is deliberately separate: archive/index layouts stay readable
 * after an update. Even a torn marker holds the scene rather than deleting data. */
static bool session_blocked(const char *id)
{
    char path[128]; struct stat st;
    return catalog_path(path, id, "blocked") && stat(path, &st) == 0;
}
static esp_err_t catalog_block_reason(const char *id, const char *reason)
{
    char path[128];
    if (!catalog_path(path, id, "blocked")) return ESP_ERR_INVALID_ARG;
    FILE *file = fopen(path, "wb");
    if (!file) return ESP_FAIL;
    const size_t bytes = strlen(reason);
    const bool ok = fwrite(reason, 1, bytes, file) == bytes &&
        fflush(file) == 0 && fsync(fileno(file)) == 0;
    const int closed = fclose(file);
    invalidate_replay_picks();
    return ok && closed == 0 ? ESP_OK : ESP_FAIL;
}
esp_err_t sd_audio_catalog_block(const char *id)
{
    return catalog_block_reason(id, "protocol_conflict: source retained\n");
}

/* CRC covers the record through crc (not compiler tail padding). */
static bool valid_record(const void *record, size_t crc_offset, uint32_t magic)
{
    uint32_t stored, tag, generation;
    memcpy(&generation, record, 4);
    memcpy(&tag, (const uint8_t *)record + 4, 4);
    memcpy(&stored, (const uint8_t *)record + crc_offset, 4);
    return generation && tag == magic && stored == checksum(record, crc_offset);
}

typedef struct {
    const char *id;
    const sd_audio_session_t *expected;
    bool recovery;
    bool diagnostics;
} catalog_session_key_t;

typedef bool (*catalog_record_match_t)(const void *record, const void *context);
#ifdef ESP_PLATFORM
static __attribute__((noinline)) void catalog_log_original_pair(const uint8_t *copies,
    size_t got, size_t bytes, size_t crc_offset, uint32_t magic, uint64_t offset,
    const catalog_session_key_t *key, esp_err_t err)
{
    ESP_LOGE("sd_audio_catalog", "SD metadata original session=%s offset=%lu record_bytes=%u read_bytes=%u expected_gen=%lu err=%s sample=original",
        key->id, (unsigned long)offset, (unsigned)bytes, (unsigned)got,
        (unsigned long)(key->expected ? key->expected->generation : 0U), esp_err_to_name(err));
    if (bytes < 8U || crc_offset > bytes - sizeof(uint32_t)) return;
    for (unsigned i = 0; i < 2U; ++i) {
        uint32_t generation = 0U, tag = 0U, stored = 0U, computed = 0U;
        const bool full = got >= (i + 1U) * bytes;
        if (full) {
            const uint8_t *record = copies + i * bytes;
            memcpy(&generation, record, 4U); memcpy(&tag, record + 4U, 4U);
            memcpy(&stored, record + crc_offset, 4U); computed = checksum(record, crc_offset);
        }
        ESP_LOGE("sd_audio_catalog", "SD metadata slot=%u full=%u valid=%u gen=%lu magic=%08lx stored_crc=%08lx computed_crc=%08lx sample=original",
            i, full ? 1U : 0U, full && generation && tag == magic && stored == computed ? 1U : 0U,
            (unsigned long)generation, (unsigned long)tag, (unsigned long)stored, (unsigned long)computed);
    }
}
#endif
static esp_err_t read_pair_matching(FILE *file, uint64_t offset, void *record, size_t size,
    size_t crc_offset, uint32_t magic, catalog_record_match_t matches, const void *context)
{
    _Alignas(sd_audio_segment_t) uint8_t copies[2 * sizeof(sd_audio_segment_t)] = {0};
    if (size > sizeof(sd_audio_segment_t) || offset + 2U * size > 0x7fffffffU ||
        offset > 0x7fffffffU) return ESP_ERR_INVALID_SIZE;
    if (fseek(file, (long)offset, SEEK_SET) != 0) return ESP_FAIL;
    /* Both slots are adjacent on disk. One read avoids a second seek and
     * stdio buffer refill; a truncated second slot still permits the first. */
    const size_t got = fread(copies, 1, 2U * size, file);
    if (ferror(file)) {
#ifdef ESP_PLATFORM
        const catalog_session_key_t *key = context;
        if (magic == CATALOG_MAGIC && key && key->diagnostics)
            catalog_log_original_pair(copies, got, size, crc_offset, magic, offset, key, ESP_FAIL);
#endif
        return ESP_FAIL;
    }
    bool valid[2] = {false, false};
    bool crc_valid = false;
    for (unsigned i = 0; i < 2; ++i) {
        valid[i] = got >= (i + 1U) * size && valid_record(copies + i * size, crc_offset, magic);
        crc_valid = crc_valid || valid[i];
        if (valid[i] && matches) valid[i] = matches(copies + i * size, context);
    }
    if (!valid[0] && !valid[1]) {
        const esp_err_t err = crc_valid ? ESP_ERR_INVALID_STATE : ESP_ERR_INVALID_CRC;
#ifdef ESP_PLATFORM
        const catalog_session_key_t *key = context;
        if (magic == CATALOG_MAGIC && key && key->diagnostics)
            catalog_log_original_pair(copies, got, size, crc_offset, magic, offset, key, err);
#endif
        return err;
    }
    uint32_t a = 0, b = 0;
    memcpy(&a, copies, 4); memcpy(&b, copies + size, 4);
    const unsigned slot = !valid[0] || (valid[1] && (int32_t)(b - a) > 0) ? 1 : 0;
#ifdef ESP_PLATFORM
    const catalog_session_key_t *key = context;
    if (magic == CATALOG_MAGIC && key && key->diagnostics && key->expected &&
        (slot ? b : a) != key->expected->generation)
        catalog_log_original_pair(copies, got, size, crc_offset, magic, offset, key, ESP_ERR_INVALID_CRC);
#endif
    memcpy(record, copies + slot * size, size);
    return ESP_OK;
}

static esp_err_t read_pair(FILE *file, uint64_t offset, void *record, size_t size,
                           size_t crc_offset, uint32_t magic)
{
    return read_pair_matching(file, offset, record, size, crc_offset, magic, NULL, NULL);
}

static esp_err_t write_pair(FILE *file, uint64_t offset, void *record, size_t size,
                            size_t crc_offset)
{
    if (size > sizeof(sd_audio_segment_t) || offset > 0x7fffffffU ||
        2U * size > 0x7fffffffU - offset) return ESP_ERR_INVALID_SIZE;
    /* FatFs f_lseek beyond EOF allocates sectors without erasing their old
     * contents. A first generation writes slot 1; slot 0 must not contain a
     * CRC-valid manifest/index belonging to a previously deleted file. Also
     * initialize gaps when an index grows, while preserving existing slots. */
    struct stat st;
    if (fstat(fileno(file), &st) != 0 || st.st_size < 0) return ESP_FAIL;
    uint64_t length = (uint64_t)st.st_size;
    const uint64_t end = offset + 2U * size;
    if (length < end) {
        if (fseek(file, (long)length, SEEK_SET) != 0) return ESP_FAIL;
        const uint8_t zero[128] = {0};
        while (length < end) {
            const size_t bytes = end - length < sizeof(zero) ? (size_t)(end - length) : sizeof(zero);
            if (fwrite(zero, 1U, bytes, file) != bytes) return ESP_FAIL;
            length += bytes;
        }
    }
    uint32_t generation;
    memcpy(&generation, record, 4);
    if (++generation == 0U) generation = 1U;
    memcpy(record, &generation, 4);
    const uint32_t crc = checksum(record, crc_offset);
    memcpy((uint8_t *)record + crc_offset, &crc, 4);
    const uint64_t position = offset + (generation & 1U) * size;
    if (position > 0x7fffffffU || fseek(file, (long)position, SEEK_SET) != 0 ||
        fwrite(record, 1, size, file) != size || fflush(file) != 0 ||
        fsync(fileno(file)) != 0) return ESP_FAIL;
    return ESP_OK;
}

#include "sd_audio_timestamp_storage.inc"
#include "sd_audio_catalog_pages.inc"
static esp_err_t finish_retirement(const char *id, sd_audio_segment_t *segment, uint64_t *removed_bytes);
static void retention_scan_reset(void);
static void confirmed_scan_reset(void);

static esp_err_t ensure_catalog_directory(void)
{
    struct stat st;
#if defined(ESP_PLATFORM) && CONFIG_NUWAX_SD_DIRECTORY_GUARD_ENABLE
    esp_err_t err = nuwax_sd_card_validate_directory(SD_AUDIO_CATALOG_ROOT, true);
    if (err != ESP_OK) return err;
    if (mkdir(SD_AUDIO_CATALOG_ROOT, 0775) != 0 &&
        !(errno == EEXIST && stat(SD_AUDIO_CATALOG_ROOT, &st) == 0 && S_ISDIR(st.st_mode))) return ESP_FAIL;
    return nuwax_sd_card_validate_directory(SD_AUDIO_CATALOG_ROOT, false);
#else
    if (mkdir(SD_AUDIO_CATALOG_ROOT, 0775) == 0) return ESP_OK;
    return errno == EEXIST && stat(SD_AUDIO_CATALOG_ROOT, &st) == 0 &&
        S_ISDIR(st.st_mode) ? ESP_OK : ESP_FAIL;
#endif
}

esp_err_t sd_audio_catalog_init(void)
{
    if (sd_audio_catalog_replay_pinned()) return ESP_ERR_NOT_FINISHED;
    const esp_err_t repair_closed = sd_audio_catalog_repair_reset();
    if (repair_closed != ESP_OK) return repair_closed;
    sd_audio_catalog_verify_reset();
    sd_monitor_fifo_reset();
    retention_scan_reset();
    confirmed_scan_reset();
    memset(page_cache, 0, sizeof(page_cache));
    sd_audio_catalog_unpin();
    memset(replay_rotation, 0, sizeof(replay_rotation));
    sd_audio_catalog_replay_reset();
    done_cache_reset(); /* Mount/recovery boundary: never trust a previous card. */
    const esp_err_t err = ensure_catalog_directory();
    if (err != ESP_OK) return err;
    return metadata_io_qualified ? ESP_OK : sd_audio_catalog_qualify_storage();
}

esp_err_t sd_audio_catalog_check_new(const char *id)
{
    /* An orphaned index/time or an interrupted create still owns the ID. */
    static const char *const suffixes[] = {"session", "creating", "repairing", "index", "time", "blocked", "paged", "paging"};
    char path[128];
    struct stat st;
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i) {
        if (!catalog_path(path, id, suffixes[i])) return ESP_ERR_INVALID_ARG;
        if (stat(path, &st) == 0) return ESP_ERR_INVALID_STATE;
        if (errno != ENOENT) return ESP_FAIL;
    }
    /* Quarantined manifests also reserve the identity. Enumerate once instead
     * of probing 100 possible backup names; this runs only at scene creation.
     * FAT names are case insensitive, including quarantined names. */
    DIR *dir = opendir(SD_AUDIO_CATALOG_ROOT);
    if (!dir) return errno == ENOENT ? ESP_OK : ESP_FAIL;
    char prefix[72];
    snprintf(prefix, sizeof(prefix), "%s.session.bad.", id);
    const size_t length = strlen(prefix);
    struct dirent *entry;
    esp_err_t result = ESP_OK;
    errno = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (!strncasecmp(entry->d_name, prefix, length)) {
            result = ESP_ERR_INVALID_STATE;
            break;
        }
    }
    if (result == ESP_OK && errno != 0) result = ESP_FAIL;
    if (closedir(dir) != 0 && result == ESP_OK) result = ESP_FAIL;
    return result;
}

static bool session_record_matches(const void *record, const void *context)
{
    const sd_audio_session_t *session = record;
    const catalog_session_key_t *key = context;
    if (!memchr(session->session_id, 0, sizeof(session->session_id)) ||
        strcmp(session->session_id, key->id) || session->mode > 1U ||
        session->ended > 1U || session->complete > 1U || session->done > 1U ||
        session->segment_count > 1000000U ||
        (!key->recovery && session->first_pending > session->segment_count)) return false;
    const sd_audio_session_t *expected = key->expected;
    return !expected || (expected->mode == session->mode &&
        expected->boot_id == session->boot_id && expected->start_ms == session->start_ms &&
        !memcmp(expected->owner, session->owner, sizeof(session->owner)));
}

esp_err_t sd_audio_catalog_load(const char *id, sd_audio_session_t *session)
{
    char path[128];
    if (!session || !catalog_path(path, id, "session")) return ESP_ERR_INVALID_ARG;
    ++catalog_stats.manifest_reads;
    FILE *file = fopen(path, "rb");
    if (!file) {
        const esp_err_t err = errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
        done_cache_forget(id);
        return err;
    }
    const catalog_session_key_t key = {.id = id};
    esp_err_t err = read_pair_matching(file, 0, session, sizeof(*session),
        offsetof(sd_audio_session_t, crc), CATALOG_MAGIC, session_record_matches, &key);
    fclose(file);
    if (err == ESP_OK && (!memchr(session->session_id, 0, sizeof(session->session_id)) ||
        strcmp(id, session->session_id) || session->mode > 1U || session->first_pending > session->segment_count))
        err = ESP_ERR_INVALID_STATE;
    if (err == ESP_OK && session->done) done_cache_remember(session);
    else done_cache_forget(id);
    return err;
}

#include "sd_audio_catalog_io.inc"

/* A successful write/fsync does not prove that the next reader can recover
 * the record. Verify through a fresh handle, including its exact generation. */
static esp_err_t catalog_verify_session_file(const char *path,
                                             const sd_audio_session_t *expected)
{
    FILE *file = fopen(path, "rb");
    if (!file) return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    sd_audio_session_t actual = {0};
    const catalog_session_key_t key = {.id = expected->session_id, .expected = expected, .diagnostics = true};
    esp_err_t err = read_pair_matching(file, 0U, &actual, sizeof(actual),
        offsetof(sd_audio_session_t, crc), CATALOG_MAGIC, session_record_matches, &key);
    const bool selected = err == ESP_OK;
    if (selected && memcmp(&actual, expected,
        offsetof(sd_audio_session_t, crc) + sizeof(actual.crc))) err = ESP_ERR_INVALID_CRC;
    if (err != ESP_OK) {
        metadata_io_qualified = false;
        catalog_report_readback(file, path, expected->session_id, 0U, expected, &actual, selected,
            sizeof(actual), offsetof(sd_audio_session_t, crc), CATALOG_MAGIC, err);
    }
    const bool close_failed = fclose(file) != 0;
    if (close_failed && err == ESP_OK) err = ESP_FAIL;
    if (err != ESP_OK) metadata_io_qualified = false;
#ifdef ESP_PLATFORM
    if (err != ESP_OK) ESP_LOGE("sd_audio_catalog", "scene readback failed stage=%s session=%s expected_generation=%lu selected_generation=%lu close_failed=%u err=%s",
        catalog_metadata_phase(path), expected->session_id, (unsigned long)expected->generation,
        (unsigned long)actual.generation, close_failed ? 1U : 0U, esp_err_to_name(err));
#endif
    return err;
}

static esp_err_t catalog_seed_session(const char *path, sd_audio_session_t *session)
{
    FILE *file = fopen(path, "w+b");
    if (!file) return ESP_FAIL;
    session->magic = CATALOG_MAGIC; session->generation = 0U;
    esp_err_t err = write_pair(file, 0U, session, sizeof(*session), offsetof(sd_audio_session_t, crc));
    /* Seed both independent CRC slots before publishing a brand-new scene. */
    if (err == ESP_OK) err = write_pair(file, 0U, session, sizeof(*session), offsetof(sd_audio_session_t, crc));
    if (fclose(file) != 0 && err == ESP_OK) err = ESP_FAIL;
    return err == ESP_OK ? catalog_verify_session_file(path, session) : err;
}

esp_err_t sd_audio_catalog_save(sd_audio_session_t *session)
{
    invalidate_selection();
    char path[128];
    if (!session || !catalog_path(path, session->session_id, "session")) return ESP_ERR_INVALID_ARG;
    done_cache_forget(session->session_id);
    FILE *file = fopen(path, "r+b");
    if (!file) return ESP_FAIL;
    sd_audio_session_t latest;
    const catalog_session_key_t key = {.id = session->session_id, .expected = session};
    const esp_err_t loaded = read_pair_matching(file, 0, &latest, sizeof(latest),
        offsetof(sd_audio_session_t, crc), CATALOG_MAGIC, session_record_matches, &key);
    if (loaded != ESP_OK) {
        fclose(file); return loaded;
    }
    if (session->first_pending > session->segment_count ||
        session->segment_count < latest.segment_count ||
        (!session->ended && latest.ended)) {
        /* Stale snapshots must not hide newly reserved files or reopen a
         * sealed scene. Recovery uses its separate evidence-checked writer. */
        fclose(file); return ESP_ERR_INVALID_STATE;
    }
    session->generation = latest.generation;
    if (session->first_pending < latest.first_pending) session->first_pending = latest.first_pending;
    session->complete = session->complete && latest.complete;
    session->done = session->done || latest.done;
    /* Cursor-only persistence does not change which scene is eligible.
     * Invalidate before writing: an I/O error can leave either slot current. */
    if (session->mode != latest.mode || session->done != latest.done ||
        session->ended != latest.ended || session->ended_ms != latest.ended_ms ||
        session->segment_count != latest.segment_count ||
        memcmp(session->owner, latest.owner, sizeof(session->owner)))
        invalidate_replay_picks();
    const esp_err_t err = write_pair(file, 0, session, sizeof(*session), offsetof(sd_audio_session_t, crc));
    const int closed = fclose(file);
    if (err == ESP_OK && closed == 0) done_cache_remember(session);
    if (err == ESP_OK && closed == 0 && session->first_pending > latest.first_pending)
        catalog_prune_pages(session, latest.first_pending, false);
    if (err == ESP_OK && closed == 0 && session->done) {
        const int previous_errno = errno;
        if (catalog_path(path, session->session_id, "verify")) (void)unlink(path);
        if (catalog_path(path, session->session_id, "verify.tmp")) (void)unlink(path);
        errno = previous_errno;
    }
    return err == ESP_OK && closed != 0 ? ESP_FAIL : err;
}

esp_err_t sd_audio_catalog_create(sd_audio_session_t *session)
{
    retention_scan_reset();
    invalidate_replay_picks();
    char path[128], temporary[128];
    if (!session || !catalog_path(path, session->session_id, "session")) return ESP_ERR_INVALID_ARG;
    /* A new scene resets per-pass selection, not knowledge of already
     * completed scenes on this mounted card. Recovery/init resets both. */
    sd_audio_catalog_replay_reset();
    done_cache_forget(session->session_id);
    if (ensure_catalog_directory() != ESP_OK) return ESP_FAIL;
    const esp_err_t available = sd_audio_catalog_check_new(session->session_id);
    if (available != ESP_OK) return available;
    if (!metadata_io_qualified) {
        const esp_err_t qualified = sd_audio_catalog_qualify_storage();
        if (qualified != ESP_OK) return qualified;
    }
    if (!catalog_path(temporary, session->session_id, "creating")) return ESP_ERR_INVALID_ARG;
    const esp_err_t err = catalog_seed_session(temporary, session);
    if (err != ESP_OK) return err;
    /* Audio creation starts only after this durable metadata is published. */
    if (rename(temporary, path) != 0) return ESP_FAIL;
    return catalog_verify_session_file(path, session);
}

/* A torn/invalid manifest is a local fault, not a failed card. Preserve it
 * and its audio for inspection; an unknown owner is never uploaded. Physical
 * read/rename errors remain fatal and enter the normal SD recovery path. */
static esp_err_t catalog_quarantine_manifest(const char *id)
{
    char path[128], bad[128];
    if (!catalog_path(path, id, "session")) return ESP_FAIL;
    for (unsigned i = 0; i < 100U; ++i) {
        char suffix[24];
        snprintf(suffix, sizeof(suffix), "session.bad.%u", i);
        if (!catalog_path(bad, id, suffix)) return ESP_FAIL;
        struct stat st;
        if (stat(bad, &st) == 0) continue;
        if (errno != ENOENT || rename(path, bad) != 0) return ESP_FAIL;
        invalidate_replay_picks();
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_FAIL;
}

static esp_err_t load_replay_session(const char *id, sd_audio_session_t *session)
{
    esp_err_t err = sd_audio_catalog_load(id, session);
    if (err == ESP_ERR_INVALID_CRC || err == ESP_ERR_INVALID_STATE || err == ESP_ERR_INVALID_SIZE)
        err = sd_audio_catalog_load(id, session);
    if (err != ESP_ERR_INVALID_CRC && err != ESP_ERR_INVALID_STATE && err != ESP_ERR_INVALID_SIZE) return err;
    return catalog_quarantine_manifest(id);
}

esp_err_t sd_audio_catalog_mark_missing(const char *id, uint32_t index)
{
    sd_audio_session_t session;
    esp_err_t err = sd_audio_catalog_load(id, &session);
    if (err != ESP_OK) return err;
    if (session.done) return ESP_ERR_INVALID_STATE;
    sd_audio_segment_t segment;
    err = sd_audio_catalog_read(id, index, &segment);
    if (err != ESP_OK) return err;
    if (segment.acknowledged) return ESP_ERR_INVALID_STATE;
    /* Persist incompleteness first: a reset must never publish a full scene
     * while its segment has been skipped. Raw audio is retained, never ACKed. */
    session.complete = 0;
    err = sd_audio_catalog_save(&session);
    if (err != ESP_OK) return err;
    segment.missing = 1;
    return sd_audio_catalog_write(id, &segment);
}

esp_err_t sd_audio_catalog_mark_missing_expected(const sd_audio_session_t *expected,
                                               const sd_audio_segment_t *part)
{
    if (!expected || !part || !(part->sealed & 1U) || !part->path[0] ||
        part->acknowledged) return ESP_ERR_INVALID_ARG;
    sd_audio_session_t session;
    esp_err_t err = sd_audio_catalog_load(expected->session_id, &session);
    if (err != ESP_OK) return err;
    if (session.done || session.mode != expected->mode || session.boot_id != expected->boot_id ||
        session.start_ms != expected->start_ms ||
        memcmp(session.owner, expected->owner, sizeof(session.owner)))
        return ESP_ERR_INVALID_STATE;
    sd_audio_segment_t current;
    err = sd_audio_catalog_read(session.session_id, part->index, &current);
    if (err != ESP_OK) return err;
    if (current.acknowledged || !(current.sealed & 1U) ||
        current.total_bytes != part->total_bytes || current.payload_crc != part->payload_crc ||
        current.start_ms != part->start_ms || current.end_ms != part->end_ms ||
        strcmp(current.path, part->path)) return ESP_ERR_INVALID_STATE;
    session.complete = 0U;
    err = sd_audio_catalog_save(&session);
    if (err != ESP_OK) return err;
    if (current.missing) return ESP_OK;
    current.missing = 1U;
    return sd_audio_catalog_write(session.session_id, &current);
}

static bool segment_record_matches(const void *record, const void *context)
{
    const sd_audio_segment_t *segment = record;
    return segment->index == *(const uint32_t *)context &&
        memchr(segment->path, 0, sizeof(segment->path)) &&
        !(segment->sealed & ~(1U | SD_AUDIO_SEGMENT_ATTEMPTED)) &&
        segment->acknowledged <= 1U && segment->realtime <= 1U &&
        segment->missing <= SD_AUDIO_SEGMENT_RETIRED;
}

esp_err_t sd_audio_catalog_read(const char *id, uint32_t index, sd_audio_segment_t *segment)
{
    char path[128];
    uint64_t offset;
    if (!segment) return ESP_ERR_INVALID_ARG;
    esp_err_t location = catalog_index_location(id, index, path, &offset);
    if (location != ESP_OK) return location;
    FILE *file = fopen(path, "rb");
    if (!file) return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    const esp_err_t err = read_pair_matching(file, offset,
        segment, sizeof(*segment), offsetof(sd_audio_segment_t, crc), SEGMENT_MAGIC,
        segment_record_matches, &index);
    fclose(file);
    if (err == ESP_OK && (segment->index != index ||
        !memchr(segment->path, 0, sizeof(segment->path)))) return ESP_ERR_INVALID_STATE;
    return err;
}

esp_err_t sd_audio_catalog_write(const char *id, sd_audio_segment_t *segment)
{
    monitor_fifo_metadata_changed(id);
    invalidate_selection();
    char path[128];
    uint64_t offset;
    if (!segment) return ESP_ERR_INVALID_ARG;
    esp_err_t location = catalog_index_location(id, segment->index, path, &offset);
    if (location != ESP_OK) return location;
    if (!segment->acknowledged) invalidate_replay_picks();
    FILE *file = fopen(path, "r+b");
    if (!file && errno == ENOENT) file = fopen(path, "w+b");
    if (!file) return ESP_FAIL;
    segment->magic = SEGMENT_MAGIC;
    esp_err_t err = write_pair(file, offset,
        segment, sizeof(*segment), offsetof(sd_audio_segment_t, crc));
    const int closed = fclose(file);
    if (err == ESP_OK && closed != 0) err = ESP_FAIL;
    if (err == ESP_OK && (segment->sealed & 1U) && !segment->missing)
        err = catalog_segment_backup_write(id, segment);
    return err;
}

esp_err_t sd_audio_catalog_reserve(sd_audio_session_t *session, sd_audio_segment_t *segment)
{
    /* The existing wire contract accepts indices 0..999999. Do not keep
     * recording archives that neither the sender nor the peer can represent. */
    if (!session || !segment || session->ended || session->done || session->segment_count >= 1000000U)
        return ESP_ERR_INVALID_STATE;
    segment->index = session->segment_count;
    /* Reserve count before creating audio: a crash can produce a reported gap,
     * never an uncounted file that makes a partial meeting look complete. */
    session->segment_count++;
    esp_err_t err = sd_audio_catalog_save(session);
    if (err == ESP_OK) {
        char path[128];
        if (!catalog_path(path, session->session_id, "session")) return ESP_ERR_INVALID_ARG;
        err = catalog_verify_session_file(path, session);
    }
    if (err == ESP_OK) err = sd_audio_catalog_write(session->session_id, segment);
    if (err == ESP_OK) {
        sd_audio_segment_t actual;
        err = sd_audio_catalog_read(session->session_id, segment->index, &actual);
        if (err == ESP_OK && memcmp(&actual, segment,
            offsetof(sd_audio_segment_t, crc) + sizeof(actual.crc))) err = ESP_ERR_INVALID_CRC;
    }
    return err;
}

esp_err_t sd_audio_catalog_mark_attempted(const char *id, uint32_t index)
{
    invalidate_selection();
    char path[128];
    uint64_t offset;
    esp_err_t location = catalog_index_location(id, index, path, &offset);
    if (location != ESP_OK) return location;
    FILE *file = fopen(path, "r+b");
    if (!file) return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    sd_audio_segment_t segment;
    esp_err_t err = read_pair(file, offset, &segment, sizeof(segment),
        offsetof(sd_audio_segment_t, crc), SEGMENT_MAGIC);
    if (err == ESP_OK && (segment.index != index || !(segment.sealed & 1U) ||
        segment.acknowledged || segment.missing ||
        !memchr(segment.path, 0, sizeof(segment.path)))) err = ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) {
        /* No eligibility, ordering or manifest generation changes. The
         * general writer invalidates picks for sealing/new/missing records;
         * using it for this retry hint forced a full scan before EVERY file.
         * Still write a fresh CRC slot on retry: a previously failed sync
         * can leave the attempted bit visible without durable confirmation. */
        segment.sealed |= SD_AUDIO_SEGMENT_ATTEMPTED;
        err = write_pair(file, offset, &segment, sizeof(segment), offsetof(sd_audio_segment_t, crc));
    }
    if (fclose(file) != 0 && err == ESP_OK) err = ESP_FAIL;
    return err;
}

esp_err_t sd_audio_catalog_ack(const char *id, uint32_t index)
{
    monitor_fifo_metadata_changed(id);
    invalidate_live_hint();
    invalidate_selection();
    char path[128];
    uint64_t offset;
    esp_err_t location = catalog_index_location(id, index, path, &offset);
    if (location != ESP_OK) return location;
    FILE *file = fopen(path, "r+b");
    if (!file) return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    sd_audio_segment_t segment;
    esp_err_t err = read_pair(file, offset, &segment, sizeof(segment),
        offsetof(sd_audio_segment_t, crc), SEGMENT_MAGIC);
    if (err == ESP_OK && (segment.index != index || !segment.sealed || segment.missing ||
        !memchr(segment.path, 0, sizeof(segment.path)))) err = ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) {
        /* A failed earlier sync/close can leave an ACK visible in caches.
         * Rewrite it in a new CRC slot, including on retry: fsync on a newly
         * opened, unmodified FatFs handle need not flush that earlier write. */
        segment.acknowledged = 1;
        err = write_pair(file, offset, &segment, sizeof(segment), offsetof(sd_audio_segment_t, crc));
    }
    if (fclose(file) != 0 && err == ESP_OK) err = ESP_FAIL;
    if (err == ESP_OK) err = catalog_segment_backup_write(id, &segment);
    if (err == ESP_OK) {
        strcpy(acked_id, id);
        acked_index = index;
    }
    return err;
}

static esp_err_t remove_acknowledged(const char *id, const sd_audio_segment_t *segment)
{
    if (!segment->path[0]) return ESP_OK;
    struct stat st;
    if (stat(segment->path, &st) != 0)
        return errno == ENOENT ? sd_audio_catalog_drop_backup(segment->path) : ESP_FAIL;
    /* Usually deletion already happened in replay_one_ready. The exceptional
     * cleanup path must establish the same durable ACK before removing audio.
     * Reuse proof only while no catalog mutation/reset has invalidated it. */
    if (strcmp(acked_id, id) || acked_index != segment->index) {
        const esp_err_t err = sd_audio_catalog_ack(id, segment->index);
        if (err != ESP_OK) return err;
    }
    if (remove(segment->path) != 0 && errno != ENOENT) return ESP_FAIL;
    return sd_audio_catalog_drop_backup(segment->path);
}

#include "sd_audio_catalog_retention.inc"
#include "sd_audio_verify_catalog.inc"

esp_err_t sd_audio_catalog_prepare_remount(void)
{
    if (sd_audio_catalog_replay_pinned()) return ESP_ERR_NOT_FINISHED;
    esp_err_t err = sd_audio_catalog_repair_reset();
    sd_audio_catalog_verify_reset();
    metadata_io_qualified = false;
    if (retention_scan.dir && closedir(retention_scan.dir) != 0) err = ESP_FAIL;
    retention_scan.dir = NULL;
    if (confirmed_scan.dir && closedir(confirmed_scan.dir) != 0) err = ESP_FAIL;
    confirmed_scan.dir = NULL;
    retention_scan_reset(); confirmed_scan_reset();
    return err;
}

void sd_audio_catalog_note_deleted(const char *id, uint32_t index)
{
    /* Called only AFTER successful unlink following a durable ACK. An
     * intervening writer clears the proof, so a miss falls back to the
     * ordinary CRC/ACK scan. Failed unlink never advances the cursor. */
    if (!id || !acked_id[0] || strcmp(id, acked_id) || index != acked_index) return;
    for (unsigned mode = 0; mode < 2U; ++mode) {
        if (!strcmp(replay_pick[mode], id) && replay_cursor[mode] == index && index != UINT32_MAX)
            replay_cursor[mode] = index + 1U;
    }
    acked_id[0] = '\0';
}

esp_err_t sd_audio_catalog_bind(const char *id, uint32_t index,
    sd_audio_session_t *session, sd_audio_segment_t *segment, bool *reused)
{
    if (reused) *reused = false;
    const bool hit = selected_valid && valid_id(id) && index == selected_segment.index &&
        !strcmp(id, selected_session.session_id);
    selected_valid = false; // A retry must obtain a fresh selection.
    if (!session || !segment) return ESP_ERR_INVALID_ARG;
    if (hit) {
        *session = selected_session; *segment = selected_segment;
        if (reused) *reused = true;
        return ESP_OK;
    }
    esp_err_t err = sd_audio_catalog_load(id, session);
    if (err == ESP_OK) err = sd_audio_catalog_read(id, index, segment);
    return err;
}

static uint32_t u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Archives have already undergone record/CRC recovery by the spool. Only
 * reconcile immutable metadata here; no damaged/temporary file is deleted. */
static esp_err_t recover_record_sample(sd_audio_segment_t *segment, bool *complete)
{
    FILE *file = fopen(segment->path, "rb");
    if (!file) {
        if (errno != ENOENT) return ESP_FAIL;
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t footer[32], last[26];
    esp_err_t err = ESP_FAIL;
    if (fseek(file, 0, SEEK_END) != 0) goto closed;
    const long size = ftell(file);
    if (size < 0) goto closed;
    if (size < 96 + 666 + 32) { err = ESP_ERR_INVALID_SIZE; goto closed; }
    if (fseek(file, -32, SEEK_END) != 0) goto closed;
    if (fread(footer, 1, 32, file) != 32) { err = ESP_ERR_INVALID_SIZE; goto closed; }
    if (memcmp(footer, "NWXEND1", 7)) { err = ESP_ERR_INVALID_CRC; goto closed; }
    /* A sealed index is the original immutable size/CRC witness. Never
     * replace it with a newly read footer, even if that footer is parseable. */
    if ((segment->sealed & 1U) && ((uint64_t)size != segment->total_bytes ||
        u32(footer + 24) != segment->payload_crc)) { err = ESP_ERR_INVALID_CRC; goto closed; }
    if (fseek(file, -(32 + 666), SEEK_END) != 0) goto closed;
    if (fread(last, 1, 26, file) != 26) { err = ESP_ERR_INVALID_SIZE; goto closed; }
    if (u32(last) != 662U || last[4] != 'N' || last[5] != 'A') { err = ESP_ERR_INVALID_CRC; goto closed; }
    segment->sealed |= 1U;
    segment->total_bytes = (uint32_t)size;
    segment->payload_crc = u32(footer + 24);
    if (!footer[28]) *complete = false;
    const uint32_t timestamp = u32(last + 4 + 14);
    segment->end_ms = segment->start_ms + (uint32_t)(timestamp - (uint32_t)segment->start_ms) + 20U;
    err = ESP_OK;
closed:
    if (ferror(file)) err = ESP_FAIL;
    if (fclose(file) != 0 && err == ESP_OK) err = ESP_FAIL;
    return err;
}

static esp_err_t recover_record(const char *id, sd_audio_segment_t *segment, bool *complete)
{
    sd_audio_segment_t sampled = *segment;
    bool sampled_complete = *complete;
    esp_err_t err = recover_record_sample(&sampled, &sampled_complete);
#ifdef ESP_PLATFORM
    if (err == ESP_ERR_INVALID_CRC || err == ESP_ERR_INVALID_SIZE) {
        if (!nuwax_sd_bulk_read_recovery_begin(1500U)) return ESP_ERR_NOT_FINISHED;
        sampled = *segment; sampled_complete = *complete;
        err = recover_record_sample(&sampled, &sampled_complete);
        /* This proves metadata only, not the whole payload CRC. */
        nuwax_sd_bulk_read_recovery_end(false);
    }
#endif
    if (err == ESP_OK) err = sd_audio_catalog_write(id, &sampled);
    if (err == ESP_OK) { *segment = sampled; *complete = sampled_complete; }
    return err;
}

#include "sd_audio_catalog_repair.inc"
#include "sd_audio_catalog_archives.inc"
#include "sd_audio_catalog_active.inc"

static esp_err_t catalog_recover_impl(uint32_t boot_id, const char *active_id)
{
    sd_audio_catalog_replay_reset();
    (void)boot_id;
    const esp_err_t initialized = sd_audio_catalog_init();
    if (initialized != ESP_OK) return initialized;
    esp_err_t restored = catalog_restore_pending_repairs(active_id);
    if (restored == ESP_OK) restored = active_id ? restore_quarantined_sessions_filtered(active_id)
                                               : restore_quarantined_sessions();
    if (restored != ESP_OK) return restored;
    bool archives_repaired = false;
    catalog_absence_clear();
    recovery_absence.active = true;
    recovery_absence.live = active_id != NULL;
    const esp_err_t archives = catalog_recover_ready_archives(active_id, UINT32_MAX, &archives_repaired);
    recovery_absence.active = false;
    recovery_absence.complete = archives == ESP_OK;
    if (archives != ESP_OK) { catalog_absence_clear(); return archives; }
#ifdef ESP_PLATFORM
    if (archives_repaired) ESP_LOGW("sd_audio_catalog", "retained archive indexes restored; durable ACK and owner proof preserved, completeness not promoted");
#endif
    DIR *dir = opendir(SD_AUDIO_CATALOG_ROOT);
    if (!dir) { catalog_absence_clear(); return ESP_FAIL; }
    esp_err_t result = ESP_OK;
    struct dirent *entry;
    for (;;) {
        errno = 0; entry = readdir(dir);
        if (!entry) { if (errno) result = ESP_FAIL; break; }
        size_t n = strlen(entry->d_name);
        if (n <= 8U || strcmp(entry->d_name + n - 8U, ".session")) continue;
        char id[48] = {0};
        if (n - 8U >= sizeof(id)) continue;
        memcpy(id, entry->d_name, n - 8U);
        if (active_id && strcmp(active_id, id)) continue;
        sd_audio_session_t session;
        char manifest[128];
        if (!catalog_path(manifest, id, "session")) continue;
        esp_err_t err = load_recovery_session(manifest, id, &session);
        if (!active_id && (err == ESP_ERR_INVALID_STATE || err == ESP_ERR_INVALID_CRC || err == ESP_ERR_INVALID_SIZE))
            err = load_replay_session(id, &session);
        if (err == ESP_ERR_NOT_FOUND) continue;
        if (err != ESP_OK) { result = err; continue; }
        bool repaired = false;
        err = repair_pending_session(&session, false, &repaired);
        if (err == ESP_OK && repaired) err = persist_repaired_session(manifest, &session);
        if (err != ESP_OK) { result = err; continue; }
        if (session.first_pending > session.segment_count) {
            /* No valid archive proved how to repair this cursor. Preserve the
             * manifest/audio through the existing quarantine path. */
            (void)load_replay_session(id, &session);
            continue;
        }
        catalog_prune_pages(&session, 0U, true);
        if (session.done) {
            done_cache_remember(&session);
            continue;
        }
        bool local_file_block = false;
        err = catalog_discard_local_block(id, &local_file_block);
        if (err != ESP_OK) { result = err; continue; }
        if (session_blocked(id) && !local_file_block) {
            /* A retained historical identity conflict is not a whole-card
             * failure. Never turn its unknown indexes into missing/ACKs. */
            if (active_id) result = ESP_ERR_INVALID_STATE;
            else if (session.complete) {
                session.complete = 0U;
                if (sd_audio_catalog_save(&session) != ESP_OK) result = ESP_FAIL;
            }
            continue;
        }
        if (!session.ended && (!active_id || strcmp(active_id, id))) {
            session.ended = 1; session.complete = 0;
        }
        uint32_t recovery_first = 0U;
        err = first_existing_index(&session, &recovery_first);
        if (err != ESP_OK) { result = err; continue; }
        /* Recreated pages can contain zero padding before the restored file.
         * Reconcile retained entries, including this prefix, as explicit holes
         * rather than inventing ACKs or letting end verification stall. */
        for (uint32_t i = recovery_first; i < session.segment_count; ++i) {
            sd_audio_segment_t segment;
            err = sd_audio_catalog_read(id, i, &segment);
#ifdef ESP_PLATFORM
            if (err == ESP_ERR_INVALID_CRC || err == ESP_ERR_INVALID_SIZE) {
                if (!nuwax_sd_bulk_read_recovery_begin(1500U)) err = ESP_ERR_NOT_FINISHED;
                else {
                    err = sd_audio_catalog_read(id, i, &segment);
                    nuwax_sd_bulk_read_recovery_end(false);
                }
            }
#endif
            if (err == ESP_ERR_NOT_FINISHED || err == ESP_ERR_NO_MEM || err == ESP_ERR_TIMEOUT) {
                result = err; break;
            }
            if (err == ESP_FAIL) { result = err; break; }
            if (err != ESP_OK) {
                /* Only the complete, trustworthy source walk above can prove
                 * a legacy hole. Ordinary replay never creates empty tuples. */
                memset(&segment, 0, sizeof(segment));
                segment.index = i; segment.missing = 1;
                session.complete = 0;
                err = catalog_mark_proven_absent(id, i);
                if (err == ESP_ERR_INVALID_STATE) {
                    err = catalog_discard_record(&session, i, NULL, &segment);
                }
            } else if (segment.missing == SD_AUDIO_SEGMENT_RETIRING) {
                session.complete = 0;
                err = finish_retirement(id, &segment, NULL);
            } else if (segment.missing) {
                /* Also repair a manifest that fell back to its older CRC slot.
                 * A retained hole can never be promoted to a complete scene. */
                session.complete = 0;
                if (segment.missing == SD_AUDIO_SEGMENT_RETIRED)
                    catalog_discard_unlink(&session, &segment);
            } else if (!segment.acknowledged && !segment.missing) {
                bool complete = true;
                err = recover_record(id, &segment, &complete);
                if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_SIZE || err == ESP_ERR_INVALID_CRC) {
                    session.complete = 0;
                    err = sd_audio_catalog_mark_missing(id, i);
                }
                if (segment.missing || !complete) session.complete = 0;
            }
            if (err != ESP_OK) { result = err; break; }
        }
        if (err == ESP_OK) err = sd_audio_catalog_save(&session);
        if (err != ESP_OK) result = err;
        if (err == ESP_OK && local_file_block &&
            catalog_discard_clear_local_block(id) != ESP_OK) result = ESP_FAIL;
    }
    if (closedir(dir) != 0) result = ESP_FAIL;
    catalog_absence_clear();
    return result;
}

esp_err_t sd_audio_catalog_recover(uint32_t boot_id)
{
    return catalog_recover_impl(boot_id, NULL);
}

esp_err_t sd_audio_catalog_recover_active(uint32_t boot_id, const char *active_id)
{
    if (!valid_id(active_id)) return ESP_ERR_INVALID_ARG;
    /* Never quarantine the live identity and report a successful recovery.
     * The storage owner must first restore it from its fenced checkpoint. */
    sd_audio_session_t active;
    const esp_err_t err = sd_audio_catalog_load(active_id, &active);
    if (err != ESP_OK) return err;
    if (active.ended || active.done) return ESP_ERR_INVALID_STATE;
    return catalog_recover_impl(boot_id, active_id);
}

bool sd_audio_segment_realtime(const sd_audio_session_t *session,
    const sd_audio_segment_t *segment, uint32_t boot_id, uint64_t now_ms, uint64_t live_since_ms)
{
    /* Wire class describes recording origin, never current age or priority.
     * Selection/bind validate eligibility independently before sending. */
    (void)boot_id; (void)now_ms; (void)live_since_ms;
    return session && segment && session->mode == 1U && segment->realtime != 0U;
}

esp_err_t sd_audio_catalog_has_pending(bool *pending)
{
    if (!pending) return ESP_ERR_INVALID_ARG;
    *pending = false;
    DIR *dir = opendir(SD_AUDIO_CATALOG_ROOT);
    if (!dir) return errno == ENOENT ? ESP_OK : ESP_FAIL;
    esp_err_t result = ESP_OK;
    struct dirent *entry;
    for (;;) {
        errno = 0; entry = readdir(dir);
        if (!entry) { if (errno) result = ESP_FAIL; break; }
        const size_t n = strlen(entry->d_name);
        if (n <= 8U || strcmp(entry->d_name + n - 8U, ".session")) continue;
        char id[48] = {0};
        if (n - 8U >= sizeof(id)) continue;
        memcpy(id, entry->d_name, n - 8U);
        if (done_cache_skip(id)) continue;
        sd_audio_session_t session;
        result = load_replay_session(id, &session);
        if (result == ESP_ERR_NOT_FOUND) { result = ESP_OK; continue; }
        if (result != ESP_OK) break;
        if (!session.done && !session_blocked(session.session_id) && (session.ended ||
            session.first_pending < session.segment_count)) {
            *pending = true;
            break;
        }
    }
    if (closedir(dir) != 0) result = ESP_FAIL;
    return result;
}

static bool replay_eligible(const sd_audio_session_t *session, const uint8_t owner[32])
{
    return !session->done && !memcmp(owner, session->owner, 32) &&
        (session->mode != 0U || session->ended) && !session_deferred(session);
}

/* Select the next manifest BEFORE spending the bounded index-read budget.
 * Directory/FAT order is unrelated to ended_ms, especially across reboots.
 * Reading the first sixteen directory entries and sorting just those can
 * repeatedly jump over a later entry, even with a persisted rotation key. */
static esp_err_t find_replay_session(const uint8_t owner[32], int preferred_mode,
    const sd_audio_session_t *lower, unsigned *budget, sd_audio_session_t *out, bool *found)
{
    *found = false;
    replay_scan_t *scan = &replay_scans[preferred_mode >= 0 && preferred_mode < 2 ? preferred_mode : 2];
    if (!scan->active || memcmp(scan->owner, owner, 32) ||
        (lower && (!scan->has_lower || strcmp(scan->lower_id, lower->session_id) ||
                   scan->lower_ended_ms != lower->ended_ms))) {
        memset(scan, 0, sizeof(*scan));
        scan->active = true;
        memcpy(scan->owner, owner, 32);
        if (lower) {
            scan->has_lower = true;
            memcpy(scan->lower_id, lower->session_id, sizeof(scan->lower_id));
            scan->lower_ended_ms = lower->ended_ms;
        }
    }
    DIR *dir = opendir(SD_AUDIO_CATALOG_ROOT);
    if (!dir) { scan->active = false; return errno == ENOENT ? ESP_OK : ESP_FAIL; }
    const uint32_t generation = replay_scan_generation;
    esp_err_t result = ESP_OK;
    bool reached_cursor = !scan->last_id[0];
    sd_audio_session_t lower_key = {0};
    if (scan->has_lower) {
        memcpy(lower_key.session_id, scan->lower_id, sizeof(lower_key.session_id));
        memcpy(lower_key.owner, owner, 32);
        lower_key.ended_ms = scan->lower_ended_ms;
        lower_key.mode = preferred_mode == 1 ? 1U : 0U;
    }
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        size_t n = strlen(entry->d_name);
        if (n <= 8U || strcmp(entry->d_name + n - 8U, ".session")) continue;
        char id[48] = {0};
        if (n - 8U >= sizeof(id)) continue;
        memcpy(id, entry->d_name, n - 8U);
        if (!reached_cursor) {
            if (!strcmp(id, scan->last_id)) reached_cursor = true;
            continue;
        }
        if (done_cache_skip(id)) continue;
        if (!*budget) { result = ESP_ERR_NOT_FINISHED; break; }
        --*budget;
        memcpy(scan->last_id, id, sizeof(id));
        sd_audio_session_t session;
        result = load_replay_session(id, &session);
        /* Quarantining a torn manifest invalidates cached selection too. */
        if (generation != replay_scan_generation) { result = ESP_ERR_NOT_FINISHED; break; }
        if (result == ESP_ERR_NOT_FOUND) { result = ESP_OK; continue; }
        if (result != ESP_OK) break;
        if (!session.done && memcmp(session.owner, owner, sizeof(session.owner))) {
            ++catalog_stats.owner_skips;
            memcpy(catalog_stats.last_owner_blocked, session.session_id, sizeof(session.session_id));
        }
        if (!replay_eligible(&session, owner)) continue;
        scan->mode_candidate[session.mode] = true;
        if (preferred_mode >= 0 && session.mode != preferred_mode) continue;
        const bool rotate = preferred_mode >= 0 && preferred_mode < 2;
        if (scan->has_lower && session_order(&session, &lower_key, rotate) <= 0) continue;
        if (!scan->found || session_order(&session, &scan->best, rotate) < 0) {
            scan->best = session;
            scan->found = true;
        }
    }
    closedir(dir);
    if (!reached_cursor) { scan->active = false; return ESP_ERR_NOT_FINISHED; }
    if (result == ESP_ERR_NOT_FINISHED) return result;
    scan->active = false;
    if (result == ESP_OK && scan->found) { *found = true; *out = scan->best; }
    if (result == ESP_OK && !scan->has_lower) {
        /* This scan observes both modes. Only a complete manifest scan can
         * prove an empty mode; index budget exhaustion never can. */
        for (unsigned mode = 0; mode < 2U; ++mode) {
            if (scan->mode_candidate[mode]) continue;
            memcpy(replay_pick_owner[mode], owner, 32);
            replay_empty[mode] = true;
            replay_pick[mode][0] = '\0';
            replay_cursor[mode] = replay_generation[mode] = 0U;
        }
    }
    return result;
}

static void remember_replay_pick(int mode, const uint8_t owner[32], const sd_audio_session_t *session)
{
    if (mode < 0 || mode > 1) return;
    memcpy(replay_pick_owner[mode], owner, 32);
    replay_empty[mode] = false;
    memcpy(replay_pick[mode], session->session_id, sizeof(session->session_id));
    replay_cursor[mode] = session->first_pending;
    replay_generation[mode] = session->generation;
}

static esp_err_t catalog_next_impl(const uint8_t owner[32], uint32_t boot_id,
    uint64_t now_ms, uint64_t live_since_ms, const char *active_id, bool realtime_only, int preferred_mode,
    sd_audio_session_t *out, sd_audio_segment_t *part, bool *found_session, bool *found_file)
{
    *found_session = *found_file = false;
    /* Live lookup must not walk thousands of historical scene manifests. */
    if (realtime_only) {
        if (!active_id || !active_id[0]) return ESP_OK;
        esp_err_t err = load_replay_session(active_id, out);
        if (err == ESP_ERR_NOT_FOUND) return ESP_OK;
        if (err != ESP_OK) return err;
        if (out->done || out->mode != 1U || memcmp(owner, out->owner, 32) ||
            session_deferred(out)) return ESP_OK;
        uint32_t first = out->first_pending;
        if (out->segment_count > 8U && first < out->segment_count - 8U) first = out->segment_count - 8U;
        for (uint32_t i = first; i < out->segment_count; ++i) {
            err = sd_audio_catalog_read(active_id, i, part);
            if (err == ESP_FAIL) return err;
            if (err != ESP_OK) continue; // Background cursor repairs only this record.
            if (sd_audio_segment_realtime(out, part, boot_id, now_ms, live_since_ms)) {
                *found_session = *found_file = true;
                break;
            }
        }
        return ESP_OK;
    }
    const bool cache_mode = preferred_mode >= 0 && preferred_mode < 2;
    const bool cache_owner = cache_mode && !memcmp(owner, replay_pick_owner[preferred_mode], 32);
    if (cache_owner && replay_empty[preferred_mode]) return ESP_OK;
    bool cached = cache_owner && replay_pick[preferred_mode][0];
    char cached_id[48] = {0};
    if (cached) memcpy(cached_id, replay_pick[preferred_mode], sizeof(cached_id));
    /* One budget covers stale cache misses and every candidate in this call.
     * Keep a partial cursor even when fewer than sixteen ACKs were scanned. */
    unsigned budget = CATALOG_INDEX_SLICE;
    unsigned manifest_budget = CATALOG_MANIFEST_SLICE;
    sd_audio_session_t lower;
    bool have_lower = false;
    for (;;) {
        sd_audio_session_t session;
        esp_err_t result = ESP_OK;
        if (cached) {
            --manifest_budget;
            result = load_replay_session(cached_id, &session);
            if (result != ESP_OK && result != ESP_ERR_NOT_FOUND) return result;
            if (result == ESP_ERR_NOT_FOUND || !replay_eligible(&session, owner) ||
                session.mode != preferred_mode) {
                replay_pick[preferred_mode][0] = '\0';
                cached = false;
                continue;
            }
        } else {
            bool candidate = false;
            result = find_replay_session(owner, preferred_mode, have_lower ? &lower : NULL,
                                         &manifest_budget, &session, &candidate);
            if (result != ESP_OK) return result;
            if (!candidate) {
                if (cache_mode) {
                    memcpy(replay_pick_owner[preferred_mode], owner, 32);
                    replay_pick[preferred_mode][0] = '\0';
                    replay_empty[preferred_mode] = true;
                }
                return ESP_OK;
            }
        }
        const char *id = session.session_id;
        sd_audio_segment_t segment = {0};
        bool found = false;
        const uint32_t previous_first_pending = session.first_pending;
        if (cached &&
            replay_generation[preferred_mode] == session.generation &&
            replay_cursor[preferred_mode] >= session.first_pending &&
            replay_cursor[preferred_mode] <= session.segment_count)
            session.first_pending = replay_cursor[preferred_mode];
        for (uint32_t i = session.first_pending; i < session.segment_count; ++i) {
            if (!budget) { result = ESP_ERR_NOT_FINISHED; break; }
            --budget;
            esp_err_t err = catalog_read_reconciled(id, i, &segment);
            if (err != ESP_OK) {
                result = err;
                break; /* Incomplete scans/unknown records never create holes. */
            }
            if (segment.acknowledged || segment.missing) {
                if (segment.missing) session.complete = 0;
                if (segment.missing == SD_AUDIO_SEGMENT_RETIRING) {
                    result = finish_retirement(id, &segment, NULL);
                    if (result != ESP_OK) break;
                }
                /* A reset/delete failure after durable ACK must not leave an
                 * unreachable audio file taking up the card forever. */
                if (segment.acknowledged) {
                    result = remove_acknowledged(id, &segment);
                    if (result != ESP_OK) break;
                }
                if (i == session.first_pending) session.first_pending++;
                continue;
            }
            if (segment.sealed) {
                found = true; break;
            }
        }
        if (result != ESP_OK && result != ESP_ERR_NOT_FINISHED) return result;
        /* ACKs were fsynced before deletion. Persist the scan cursor every
         * bounded slice, not after every file. For a closed scene's final
         * cursor, session_end ACK will persist cursor + done together. A
         * lost ACK/reboot safely rechecks the durable segment ACKs. */
        if (previous_first_pending != session.first_pending &&
            !(session.ended && session.first_pending == session.segment_count) &&
            (session.first_pending - previous_first_pending >= CATALOG_INDEX_SLICE ||
             session.first_pending == session.segment_count ||
             (!cache_mode && result == ESP_ERR_NOT_FINISHED)) &&
            sd_audio_catalog_save(&session) != ESP_OK) {
            return ESP_FAIL;
        }
        if (result == ESP_ERR_NOT_FINISHED) {
            remember_replay_pick(preferred_mode, owner, &session);
            return result;
        }
        const bool end_due = session.ended && session.first_pending == session.segment_count;
        if (found || end_due) {
            *out = session; *part = segment; *found_session = true; *found_file = found;
            remember_replay_pick(preferred_mode, owner, &session);
            return ESP_OK;
        }
        /* An active monitor may have only an unsealed tail. Continue after
         * this key, never restart at the directory head or reset the budget. */
        lower = session; have_lower = true; cached = false;
        if (cache_mode) replay_pick[preferred_mode][0] = '\0';
    }
}

esp_err_t sd_audio_catalog_next(const uint8_t owner[32], uint32_t boot_id,
    uint64_t now_ms, uint64_t live_since_ms, const char *active_id, bool realtime_only, int preferred_mode,
    sd_audio_session_t *out, sd_audio_segment_t *part, bool *found_session, bool *found_file)
{
    selected_valid = false;
    const esp_err_t err = catalog_next_impl(owner, boot_id, now_ms, live_since_ms,
        active_id, realtime_only, preferred_mode, out, part, found_session, found_file);
    if (err == ESP_OK && *found_session && *found_file && part->sealed &&
        !part->acknowledged && !part->missing) {
        selected_session = *out; selected_segment = *part; selected_valid = true;
    }
    return err;
}

#include "sd_audio_monitor_fifo.inc"
