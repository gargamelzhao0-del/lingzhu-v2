#include "sd_archive_store.h"

#include <errno.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#define SD_ARCHIVE_DEFAULT_SEGMENT_BYTES (4U * 1024U * 1024U)
#define SD_ARCHIVE_HEADER_BYTES 136U
#define SD_ARCHIVE_FOOTER_BYTES 32U
#define SD_ARCHIVE_MAX_FRAME_BYTES (1024U * 1024U)
#define SD_ARCHIVE_STORE_MAGIC 0x53444131U

static const unsigned char k_header_magic[8] = {'N', 'W', 'A', '-', 'S', 'E', 'G', '1'};
static const unsigned char k_footer_magic[8] = {'N', 'W', 'A', '-', 'E', 'N', 'D', '1'};

typedef int (*path_callback_t)(const char *path, const char *name, void *ctx);

static bool has_suffix(const char *name, const char *suffix);
static bool safe_archive_filename(const char *name, bool ready);
static int for_each_file(const char *root, path_callback_t callback, void *ctx);
static int rescan_usage(sd_archive_store_t *store);
static void discard_open_segment(sd_archive_store_t *store);

static int path_join(char *out, size_t out_capacity, const char *dir, const char *name) {
    if (!out || out_capacity == 0U || !dir || !name || dir[0] == '\0' || name[0] == '\0') {
        return SD_ARCHIVE_INVALID_ARGUMENT;
    }
    const size_t dir_len = strlen(dir);
    const bool has_sep = dir[dir_len - 1U] == '/' || dir[dir_len - 1U] == '\\';
    const int written = snprintf(out, out_capacity, "%s%s%s", dir, has_sep ? "" : "/", name);
    return written > 0 && (size_t)written < out_capacity ? SD_ARCHIVE_OK : SD_ARCHIVE_INVALID_ARGUMENT;
}

static int make_directory(const char *path) {
    if (!path || path[0] == '\0') {
        return SD_ARCHIVE_INVALID_ARGUMENT;
    }
#if defined(_WIN32)
    if (_mkdir(path) == 0 || errno == EEXIST) {
        return SD_ARCHIVE_OK;
    }
#else
    if (mkdir(path, 0775) == 0 || errno == EEXIST) {
        return SD_ARCHIVE_OK;
    }
#endif
    return SD_ARCHIVE_IO_ERROR;
}

static bool file_exists(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0;
}

static int sync_file(FILE *file) {
    if (!file || fflush(file) != 0) {
        return SD_ARCHIVE_IO_ERROR;
    }
#if defined(_WIN32)
    return _commit(_fileno(file)) == 0 ? SD_ARCHIVE_OK : SD_ARCHIVE_IO_ERROR;
#else
    return fsync(fileno(file)) == 0 ? SD_ARCHIVE_OK : SD_ARCHIVE_IO_ERROR;
#endif
}

static int sync_parent_directory(const char *path) {
    if (!path || path[0] == '\0') {
        return SD_ARCHIVE_INVALID_ARGUMENT;
    }
#if defined(_WIN32)
    /* The CRT does not expose a portable directory fsync. FATFS will need a
     * platform-specific directory/volume sync in the device adapter. */
    (void)path;
    return SD_ARCHIVE_OK;
#else
    const char *last_separator = strrchr(path, '/');
    if (!last_separator) {
        return SD_ARCHIVE_OK;
    }
    char directory[SD_ARCHIVE_PATH_MAX];
    const size_t length = (size_t)(last_separator - path);
    if (length == 0U || length >= sizeof(directory)) {
        return SD_ARCHIVE_INVALID_ARGUMENT;
    }
    memcpy(directory, path, length);
    directory[length] = '\0';
    const int fd = open(directory, O_RDONLY);
    if (fd < 0) {
        return SD_ARCHIVE_IO_ERROR;
    }
    const int result = fsync(fd) == 0 ? SD_ARCHIVE_OK : SD_ARCHIVE_IO_ERROR;
    close(fd);
    return result;
#endif
}

static int truncate_file(FILE *file, uint64_t length) {
    if (!file || length > (uint64_t)INT64_MAX) {
        return SD_ARCHIVE_INVALID_ARGUMENT;
    }
#if defined(_WIN32)
    return _chsize_s(_fileno(file), (__int64)length) == 0 ? SD_ARCHIVE_OK : SD_ARCHIVE_IO_ERROR;
#else
    return ftruncate(fileno(file), (off_t)length) == 0 ? SD_ARCHIVE_OK : SD_ARCHIVE_IO_ERROR;
#endif
}

static int write_u16(FILE *file, uint16_t value) {
    unsigned char bytes[2] = {(unsigned char)(value & 0xffU), (unsigned char)(value >> 8U)};
    return fwrite(bytes, 1U, sizeof(bytes), file) == sizeof(bytes) ? SD_ARCHIVE_OK : SD_ARCHIVE_IO_ERROR;
}

static int write_u32(FILE *file, uint32_t value) {
    unsigned char bytes[4] = {(unsigned char)(value & 0xffU), (unsigned char)(value >> 8U),
                              (unsigned char)(value >> 16U), (unsigned char)(value >> 24U)};
    return fwrite(bytes, 1U, sizeof(bytes), file) == sizeof(bytes) ? SD_ARCHIVE_OK : SD_ARCHIVE_IO_ERROR;
}

static int read_u16(const unsigned char *bytes, uint16_t *value) {
    if (!bytes || !value) {
        return SD_ARCHIVE_INVALID_ARGUMENT;
    }
    *value = (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8U);
    return SD_ARCHIVE_OK;
}

static int write_u64(FILE *file, uint64_t value) {
    unsigned char bytes[8];
    for (size_t i = 0U; i < sizeof(bytes); ++i) {
        bytes[i] = (unsigned char)(value >> (8U * i));
    }
    return fwrite(bytes, 1U, sizeof(bytes), file) == sizeof(bytes) ? SD_ARCHIVE_OK : SD_ARCHIVE_IO_ERROR;
}

static int read_u32(const unsigned char *bytes, uint32_t *value) {
    if (!bytes || !value) {
        return SD_ARCHIVE_INVALID_ARGUMENT;
    }
    *value = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) |
             ((uint32_t)bytes[2] << 16U) | ((uint32_t)bytes[3] << 24U);
    return SD_ARCHIVE_OK;
}

static int read_u64(const unsigned char *bytes, uint64_t *value) {
    if (!bytes || !value) {
        return SD_ARCHIVE_INVALID_ARGUMENT;
    }
    *value = 0U;
    for (size_t i = 0U; i < 8U; ++i) {
        *value |= (uint64_t)bytes[i] << (8U * i);
    }
    return SD_ARCHIVE_OK;
}

uint32_t sd_archive_crc32_update(uint32_t crc, const void *data, size_t bytes) {
    const unsigned char *input = (const unsigned char *)data;
    crc = ~crc;
    for (size_t i = 0U; i < bytes; ++i) {
        crc ^= input[i];
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ (0xedb88320U & (uint32_t)-(int)(crc & 1U));
        }
    }
    return ~crc;
}

static bool valid_id(const char *value, size_t max_len) {
    if (!value || value[0] == '\0' || strlen(value) > max_len) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.')) {
            return false;
        }
    }
    return true;
}

static int write_header(FILE *file,
                        const sd_archive_metadata_t *metadata,
                        uint32_t segment_index) {
    if (!file || !metadata) {
        return SD_ARCHIVE_INVALID_ARGUMENT;
    }
    unsigned char reserved[6] = {0};
    if (fwrite(k_header_magic, 1U, sizeof(k_header_magic), file) != sizeof(k_header_magic) ||
        write_u16(file, 1U) != SD_ARCHIVE_OK || write_u16(file, SD_ARCHIVE_HEADER_BYTES) != SD_ARCHIVE_OK ||
        write_u64(file, metadata->start_unix_ms) != SD_ARCHIVE_OK ||
        write_u32(file, metadata->sample_rate) != SD_ARCHIVE_OK ||
        write_u16(file, metadata->channels) != SD_ARCHIVE_OK ||
        write_u16(file, metadata->codec) != SD_ARCHIVE_OK ||
        write_u16(file, metadata->frame_duration_ms) != SD_ARCHIVE_OK ||
        write_u32(file, segment_index) != SD_ARCHIVE_OK ||
        fwrite(metadata->archive_id, 1U, SD_ARCHIVE_ID_MAX + 1U, file) != SD_ARCHIVE_ID_MAX + 1U ||
        fwrite(metadata->audio_session_id, 1U, SD_ARCHIVE_SESSION_ID_MAX + 1U, file) != SD_ARCHIVE_SESSION_ID_MAX + 1U ||
        fwrite(reserved, 1U, sizeof(reserved), file) != sizeof(reserved)) {
        return SD_ARCHIVE_IO_ERROR;
    }
    return SD_ARCHIVE_OK;
}

static int write_footer(FILE *file,
                        uint32_t frame_count,
                        uint64_t payload_bytes,
                        uint32_t crc32,
                        bool complete) {
    unsigned char reserved[3] = {0};
    if (!file || fwrite(k_footer_magic, 1U, sizeof(k_footer_magic), file) != sizeof(k_footer_magic) ||
        write_u16(file, 1U) != SD_ARCHIVE_OK || write_u16(file, SD_ARCHIVE_FOOTER_BYTES) != SD_ARCHIVE_OK ||
        write_u32(file, frame_count) != SD_ARCHIVE_OK || write_u64(file, payload_bytes) != SD_ARCHIVE_OK ||
        write_u32(file, crc32) != SD_ARCHIVE_OK ||
        fputc(complete ? 1 : 0, file) == EOF || fwrite(reserved, 1U, sizeof(reserved), file) != sizeof(reserved)) {
        return SD_ARCHIVE_IO_ERROR;
    }
    return SD_ARCHIVE_OK;
}

static int build_segment_paths(const sd_archive_store_t *store,
                               const sd_archive_metadata_t *metadata,
                               uint32_t index,
                               char *open_path,
                               char *ready_path) {
    char base[160];
    const int base_len = snprintf(base, sizeof(base), "archive_%s_%06lu", metadata->archive_id,
                                  (unsigned long)index);
    if (base_len <= 0 || (size_t)base_len >= sizeof(base)) {
        return SD_ARCHIVE_INVALID_ARGUMENT;
    }
    char open_name[180];
    char ready_name[180];
    if (snprintf(open_name, sizeof(open_name), "%s.open", base) <= 0 ||
        snprintf(ready_name, sizeof(ready_name), "%s.ready", base) <= 0) {
        return SD_ARCHIVE_INVALID_ARGUMENT;
    }
    if (path_join(open_path, SD_ARCHIVE_PATH_MAX, store->root_dir, open_name) != SD_ARCHIVE_OK ||
        path_join(ready_path, SD_ARCHIVE_PATH_MAX, store->root_dir, ready_name) != SD_ARCHIVE_OK) {
        return SD_ARCHIVE_INVALID_ARGUMENT;
    }
    return SD_ARCHIVE_OK;
}

static bool safe_archive_filename(const char *name, bool ready) {
    if (!name || name[0] == '\0' || strstr(name, "..") != NULL ||
        strchr(name, '/') != NULL || strchr(name, '\\') != NULL ||
        strncmp(name, "archive_", 8U) != 0) {
        return false;
    }
    return has_suffix(name, ready ? ".ready" : ".open");
}

int sd_archive_store_init(sd_archive_store_t *store,
                          const sd_archive_config_t *config) {
    if (!store || !config || !config->root_dir || config->root_dir[0] == '\0') {
        return SD_ARCHIVE_INVALID_ARGUMENT;
    }
    if (store->instance_magic == SD_ARCHIVE_STORE_MAGIC && store->active) {
        return SD_ARCHIVE_BUSY;
    }
    memset(store, 0, sizeof(*store));
    if (strlen(config->root_dir) >= sizeof(store->root_dir)) {
        return SD_ARCHIVE_INVALID_ARGUMENT;
    }
    strcpy(store->root_dir, config->root_dir);
    store->max_bytes = config->max_bytes;
    store->max_segment_bytes = config->max_segment_bytes ? config->max_segment_bytes
                                                          : SD_ARCHIVE_DEFAULT_SEGMENT_BYTES;
    if (store->max_segment_bytes < SD_ARCHIVE_HEADER_BYTES + SD_ARCHIVE_FOOTER_BYTES) {
        return SD_ARCHIVE_INVALID_ARGUMENT;
    }
    const int directory_result = make_directory(store->root_dir);
    if (directory_result != SD_ARCHIVE_OK) {
        return directory_result;
    }
    store->instance_magic = SD_ARCHIVE_STORE_MAGIC;
    store->initialized = true;
    const int scan_result = rescan_usage(store);
    if (scan_result != SD_ARCHIVE_OK) {
        memset(store, 0, sizeof(*store));
        return scan_result;
    }
    uint32_t recovered_count = 0U;
    const int recovery_result = sd_archive_store_recover_open(store, &recovered_count);
    (void)recovered_count;
    if (recovery_result != SD_ARCHIVE_OK) {
        memset(store, 0, sizeof(*store));
        return recovery_result;
    }
    return SD_ARCHIVE_OK;
}

int sd_archive_store_deinit(sd_archive_store_t *store) {
    if (!store || store->instance_magic != SD_ARCHIVE_STORE_MAGIC || !store->initialized) {
        return SD_ARCHIVE_INVALID_ARGUMENT;
    }
    if (store->active) {
        return SD_ARCHIVE_BUSY;
    }
    memset(store, 0, sizeof(*store));
    return SD_ARCHIVE_OK;
}

int sd_archive_store_begin(sd_archive_store_t *store,
                           const sd_archive_metadata_t *metadata) {
    if (!store || !store->initialized || store->instance_magic != SD_ARCHIVE_STORE_MAGIC ||
        !metadata || store->active || !valid_id(metadata->archive_id, SD_ARCHIVE_ID_MAX) ||
        !valid_id(metadata->audio_session_id, SD_ARCHIVE_SESSION_ID_MAX) ||
        metadata->sample_rate == 0U || metadata->channels == 0U || metadata->frame_duration_ms == 0U) {
        return store && store->active ? SD_ARCHIVE_BUSY : SD_ARCHIVE_INVALID_ARGUMENT;
    }
    const uint64_t empty_segment_bytes = SD_ARCHIVE_HEADER_BYTES + SD_ARCHIVE_FOOTER_BYTES;
    if (store->max_bytes != 0U &&
        (store->used_bytes > store->max_bytes ||
         empty_segment_bytes > store->max_bytes - store->used_bytes)) {
        return SD_ARCHIVE_NO_SPACE;
    }
    uint32_t index = store->next_segment_index;
    for (;;) {
        if (index == UINT32_MAX) {
            return SD_ARCHIVE_NO_SPACE;
        }
        char open_path[SD_ARCHIVE_PATH_MAX];
        char ready_path[SD_ARCHIVE_PATH_MAX];
        if (build_segment_paths(store, metadata, index, open_path, ready_path) != SD_ARCHIVE_OK) {
            return SD_ARCHIVE_INVALID_ARGUMENT;
        }
        if (!file_exists(open_path) && !file_exists(ready_path)) {
            FILE *file = fopen(open_path, "wb");
            if (!file) {
                return SD_ARCHIVE_IO_ERROR;
            }
            if (write_header(file, metadata, index) != SD_ARCHIVE_OK || sync_file(file) != SD_ARCHIVE_OK) {
                fclose(file);
                remove(open_path);
                return SD_ARCHIVE_IO_ERROR;
            }
            store->open_file = file;
            strcpy(store->open_path, open_path);
            strcpy(store->ready_path, ready_path);
            store->metadata = *metadata;
            store->segment_index = index;
            store->frame_count = 0U;
            store->payload_bytes = 0U;
            store->crc32 = 0U;
            store->active = true;
            store->next_segment_index = index + 1U;
            store->used_bytes += SD_ARCHIVE_HEADER_BYTES;
            return SD_ARCHIVE_OK;
        }
        ++index;
    }
}

int sd_archive_store_commit(sd_archive_store_t *store, bool complete) {
    if (!store || !store->initialized || store->instance_magic != SD_ARCHIVE_STORE_MAGIC ||
        !store->active || !store->open_file) {
        return SD_ARCHIVE_INVALID_ARGUMENT;
    }
    const int footer_result = write_footer(store->open_file, store->frame_count,
                                           store->payload_bytes, store->crc32, complete);
    const int sync_result = footer_result == SD_ARCHIVE_OK ? sync_file(store->open_file)
                                                            : SD_ARCHIVE_IO_ERROR;
    if (footer_result != SD_ARCHIVE_OK || sync_result != SD_ARCHIVE_OK) {
        (void)fclose(store->open_file);
        store->open_file = NULL;
        store->active = false;
        (void)rescan_usage(store);
        return footer_result != SD_ARCHIVE_OK ? footer_result : sync_result;
    }
    if (fclose(store->open_file) != 0) {
        store->open_file = NULL;
        store->active = false;
        (void)rescan_usage(store);
        return SD_ARCHIVE_IO_ERROR;
    }
    store->open_file = NULL;
    if (rename(store->open_path, store->ready_path) != 0 ||
        sync_parent_directory(store->ready_path) != SD_ARCHIVE_OK) {
        store->active = false;
        (void)rescan_usage(store);
        return SD_ARCHIVE_IO_ERROR;
    }
    store->active = false;
    store->used_bytes += SD_ARCHIVE_FOOTER_BYTES;
    store->ready_count++;
    store->open_path[0] = '\0';
    store->ready_path[0] = '\0';
    return SD_ARCHIVE_OK;
}

int sd_archive_store_abort(sd_archive_store_t *store) {
    if (!store || !store->initialized || store->instance_magic != SD_ARCHIVE_STORE_MAGIC ||
        !store->active || !store->open_file) {
        return SD_ARCHIVE_INVALID_ARGUMENT;
    }
    const int result = fclose(store->open_file) == 0 ? SD_ARCHIVE_OK : SD_ARCHIVE_IO_ERROR;
    store->open_file = NULL;
    store->active = false;
    (void)rescan_usage(store);
    return result;
}

static void discard_open_segment(sd_archive_store_t *store) {
    if (!store || !store->active) {
        return;
    }
    if (store->open_file) {
        (void)fclose(store->open_file);
        store->open_file = NULL;
    }
    if (store->open_path[0] != '\0') {
        (void)remove(store->open_path);
    }
    store->active = false;
    store->open_path[0] = '\0';
    store->ready_path[0] = '\0';
    (void)rescan_usage(store);
}

int sd_archive_store_append_frame(sd_archive_store_t *store,
                                  const void *frame,
                                  size_t frame_bytes) {
    if (!store || !store->initialized || store->instance_magic != SD_ARCHIVE_STORE_MAGIC ||
        !store->active || !store->open_file || !frame || frame_bytes == 0U ||
        frame_bytes > SD_ARCHIVE_MAX_FRAME_BYTES) {
        return SD_ARCHIVE_INVALID_ARGUMENT;
    }
    const uint64_t record_bytes = sizeof(uint32_t) + frame_bytes;
    const uint64_t projected = SD_ARCHIVE_HEADER_BYTES + store->payload_bytes +
                               (uint64_t)store->frame_count * sizeof(uint32_t) + record_bytes +
                               SD_ARCHIVE_FOOTER_BYTES;
    const bool rotate = projected > store->max_segment_bytes;
    if (store->max_bytes != 0U) {
        uint64_t used = 0U;
        uint32_t ready = 0U;
        const uint64_t additional_bytes = rotate
                                              ? (uint64_t)SD_ARCHIVE_FOOTER_BYTES +
                                                    SD_ARCHIVE_HEADER_BYTES + record_bytes +
                                                    SD_ARCHIVE_FOOTER_BYTES
                                              : record_bytes + SD_ARCHIVE_FOOTER_BYTES;
        if (sd_archive_store_get_usage(store, &used, &ready) != SD_ARCHIVE_OK ||
            used > store->max_bytes || additional_bytes > store->max_bytes - used) {
            (void)ready;
            return SD_ARCHIVE_NO_SPACE;
        }
    }
    if (rotate) {
        const sd_archive_metadata_t metadata = store->metadata;
        if (store->frame_count == 0U) {
            discard_open_segment(store);
            return SD_ARCHIVE_NO_SPACE;
        }
        const int commit_result = sd_archive_store_commit(store, true);
        if (commit_result != SD_ARCHIVE_OK) {
            return commit_result;
        }
        const int begin_result = sd_archive_store_begin(store, &metadata);
        if (begin_result != SD_ARCHIVE_OK) {
            return begin_result;
        }
        if (SD_ARCHIVE_HEADER_BYTES + sizeof(uint32_t) + frame_bytes + SD_ARCHIVE_FOOTER_BYTES >
            store->max_segment_bytes) {
            discard_open_segment(store);
            return SD_ARCHIVE_NO_SPACE;
        }
    }
    if (write_u32(store->open_file, (uint32_t)frame_bytes) != SD_ARCHIVE_OK ||
        fwrite(frame, 1U, frame_bytes, store->open_file) != frame_bytes) {
        (void)sd_archive_store_abort(store);
        return SD_ARCHIVE_IO_ERROR;
    }
    store->frame_count++;
    store->payload_bytes += frame_bytes;
    store->crc32 = sd_archive_crc32_update(store->crc32, frame, frame_bytes);
    store->used_bytes += record_bytes;
    return SD_ARCHIVE_OK;
}

/* Directory iteration is kept inside this standalone prototype. The eventual
 * ESP-IDF port should replace it with a FATFS directory adapter. */
static int for_each_file(const char *root, path_callback_t callback, void *ctx) {
#if defined(_WIN32)
    char pattern[SD_ARCHIVE_PATH_MAX];
    if (path_join(pattern, sizeof(pattern), root, "*") != SD_ARCHIVE_OK) {
        return SD_ARCHIVE_INVALID_ARGUMENT;
    }
    WIN32_FIND_DATAA data;
    HANDLE handle = FindFirstFileA(pattern, &data);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                   ? SD_ARCHIVE_OK
                   : SD_ARCHIVE_IO_ERROR;
    }
    int result = SD_ARCHIVE_OK;
    do {
        if (strcmp(data.cFileName, ".") != 0 && strcmp(data.cFileName, "..") != 0 &&
            !(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            char path[SD_ARCHIVE_PATH_MAX];
            const int callback_result = path_join(path, sizeof(path), root, data.cFileName) != SD_ARCHIVE_OK
                                            ? SD_ARCHIVE_INVALID_ARGUMENT
                                            : callback(path, data.cFileName, ctx);
            if (callback_result != SD_ARCHIVE_OK) {
                result = callback_result;
                break;
            }
        }
    } while (FindNextFileA(handle, &data));
    FindClose(handle);
    return result;
#else
    DIR *dir = opendir(root);
    if (!dir) {
        return errno == ENOENT ? SD_ARCHIVE_OK : SD_ARCHIVE_IO_ERROR;
    }
    int result = SD_ARCHIVE_OK;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char path[SD_ARCHIVE_PATH_MAX];
        const int callback_result = path_join(path, sizeof(path), root, entry->d_name) != SD_ARCHIVE_OK
                                        ? SD_ARCHIVE_INVALID_ARGUMENT
                                        : callback(path, entry->d_name, ctx);
        if (callback_result != SD_ARCHIVE_OK) {
            result = callback_result;
            break;
        }
    }
    closedir(dir);
    return result;
#endif
}

typedef struct {
    uint64_t bytes;
    uint32_t ready_count;
} usage_ctx_t;

static int usage_callback(const char *path, const char *name, void *ctx) {
    usage_ctx_t *usage = (usage_ctx_t *)ctx;
    struct stat st;
    if (!usage || (!safe_archive_filename(name, false) &&
                   !safe_archive_filename(name, true))) {
        return SD_ARCHIVE_OK;
    }
    if (stat(path, &st) != 0) {
        return SD_ARCHIVE_IO_ERROR;
    }
    usage->bytes += (uint64_t)st.st_size;
    const size_t len = strlen(name);
    if (len >= 6U && strcmp(name + len - 6U, ".ready") == 0) {
        usage->ready_count++;
    }
    return SD_ARCHIVE_OK;
}

int sd_archive_store_get_usage(const sd_archive_store_t *store,
                               uint64_t *used_bytes,
                               uint32_t *ready_count) {
    if (!store || !store->initialized || store->instance_magic != SD_ARCHIVE_STORE_MAGIC ||
        !used_bytes || !ready_count) {
        return SD_ARCHIVE_INVALID_ARGUMENT;
    }
    *used_bytes = store->used_bytes;
    *ready_count = store->ready_count;
    return SD_ARCHIVE_OK;
}

static int rescan_usage(sd_archive_store_t *store) {
    if (!store || !store->initialized || store->instance_magic != SD_ARCHIVE_STORE_MAGIC) {
        return SD_ARCHIVE_INVALID_ARGUMENT;
    }
    usage_ctx_t usage = {0};
    const int result = for_each_file(store->root_dir, usage_callback, &usage);
    if (result != SD_ARCHIVE_OK) {
        return result;
    }
    store->used_bytes = usage.bytes;
    store->ready_count = usage.ready_count;
    return SD_ARCHIVE_OK;
}

static bool has_suffix(const char *name, const char *suffix) {
    const size_t name_len = strlen(name);
    const size_t suffix_len = strlen(suffix);
    return name_len >= suffix_len && strcmp(name + name_len - suffix_len, suffix) == 0;
}

typedef struct {
    uint32_t frame_count;
    uint64_t payload_bytes;
    uint32_t crc32;
} parsed_counts_t;

typedef struct {
    parsed_counts_t counts;
    bool complete;
} parsed_footer_t;

static int stream_size(FILE *file, uint64_t *size) {
    if (!file || !size || fseek(file, 0L, SEEK_END) != 0) {
        return SD_ARCHIVE_IO_ERROR;
    }
    const long end = ftell(file);
    if (end < 0L || fseek(file, 0L, SEEK_SET) != 0) {
        return SD_ARCHIVE_IO_ERROR;
    }
    *size = (uint64_t)end;
    return SD_ARCHIVE_OK;
}

static int validate_header(FILE *file) {
    unsigned char header[SD_ARCHIVE_HEADER_BYTES];
    if (!file || fseek(file, 0L, SEEK_SET) != 0 ||
        fread(header, 1U, sizeof(header), file) != sizeof(header)) {
        return SD_ARCHIVE_CORRUPT;
    }
    uint16_t version = 0U;
    uint16_t header_bytes = 0U;
    (void)read_u16(&header[8], &version);
    (void)read_u16(&header[10], &header_bytes);
    return memcmp(header, k_header_magic, sizeof(k_header_magic)) == 0 &&
                   version == 1U && header_bytes == SD_ARCHIVE_HEADER_BYTES
               ? SD_ARCHIVE_OK
               : SD_ARCHIVE_CORRUPT;
}

static int read_footer(FILE *file, uint64_t file_size, parsed_footer_t *footer) {
    unsigned char bytes[SD_ARCHIVE_FOOTER_BYTES];
    if (!file || !footer || file_size < SD_ARCHIVE_HEADER_BYTES + SD_ARCHIVE_FOOTER_BYTES ||
        file_size > (uint64_t)LONG_MAX ||
        fseek(file, (long)(file_size - SD_ARCHIVE_FOOTER_BYTES), SEEK_SET) != 0 ||
        fread(bytes, 1U, sizeof(bytes), file) != sizeof(bytes)) {
        return SD_ARCHIVE_CORRUPT;
    }
    uint16_t version = 0U;
    uint16_t footer_bytes = 0U;
    (void)read_u16(&bytes[8], &version);
    (void)read_u16(&bytes[10], &footer_bytes);
    if (memcmp(bytes, k_footer_magic, sizeof(k_footer_magic)) != 0 || version != 1U ||
        footer_bytes != SD_ARCHIVE_FOOTER_BYTES || bytes[28] > 1U) {
        return SD_ARCHIVE_CORRUPT;
    }
    (void)read_u32(&bytes[12], &footer->counts.frame_count);
    (void)read_u64(&bytes[16], &footer->counts.payload_bytes);
    (void)read_u32(&bytes[24], &footer->counts.crc32);
    footer->complete = bytes[28] != 0U;
    return SD_ARCHIVE_OK;
}

static int scan_records(FILE *file,
                        uint64_t records_end,
                        parsed_counts_t *counts,
                        bool truncate_partial,
                        uint64_t *last_good_offset) {
    if (!file || !counts || records_end < SD_ARCHIVE_HEADER_BYTES ||
        records_end > (uint64_t)LONG_MAX || fseek(file, (long)SD_ARCHIVE_HEADER_BYTES, SEEK_SET) != 0) {
        return SD_ARCHIVE_INVALID_ARGUMENT;
    }
    memset(counts, 0, sizeof(*counts));
    uint64_t offset = SD_ARCHIVE_HEADER_BYTES;
    while (offset < records_end) {
        if (records_end - offset < sizeof(uint32_t)) {
            if (!truncate_partial || truncate_file(file, offset) != SD_ARCHIVE_OK) {
                return SD_ARCHIVE_CORRUPT;
            }
            break;
        }
        unsigned char length_bytes[sizeof(uint32_t)];
        const size_t length_read = fread(length_bytes, 1U, sizeof(length_bytes), file);
        if (length_read != sizeof(length_bytes)) {
            if (ferror(file)) {
                return SD_ARCHIVE_IO_ERROR;
            }
            if (!truncate_partial || truncate_file(file, offset) != SD_ARCHIVE_OK) {
                return SD_ARCHIVE_CORRUPT;
            }
            break;
        }
        uint32_t frame_bytes = 0U;
        (void)read_u32(length_bytes, &frame_bytes);
        if (frame_bytes == 0U || frame_bytes > SD_ARCHIVE_MAX_FRAME_BYTES ||
            (uint64_t)frame_bytes > records_end - offset - sizeof(uint32_t)) {
            if (!truncate_partial || truncate_file(file, offset) != SD_ARCHIVE_OK) {
                return SD_ARCHIVE_CORRUPT;
            }
            break;
        }
        unsigned char buffer[4096];
        uint32_t remaining = frame_bytes;
        uint32_t frame_crc = counts->crc32;
        while (remaining > 0U) {
            const size_t chunk = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
            const size_t read = fread(buffer, 1U, chunk, file);
            if (read != chunk) {
                if (ferror(file)) {
                    return SD_ARCHIVE_IO_ERROR;
                }
                if (!truncate_partial || truncate_file(file, offset) != SD_ARCHIVE_OK) {
                    return SD_ARCHIVE_CORRUPT;
                }
                remaining = 0U;
                frame_bytes = 0U;
                break;
            }
            frame_crc = sd_archive_crc32_update(frame_crc, buffer, read);
            remaining -= (uint32_t)read;
        }
        if (frame_bytes == 0U) {
            break;
        }
        counts->crc32 = frame_crc;
        counts->frame_count++;
        counts->payload_bytes += frame_bytes;
        offset += sizeof(uint32_t) + frame_bytes;
    }
    if (last_good_offset) {
        *last_good_offset = offset;
    }
    return SD_ARCHIVE_OK;
}

static int validate_stream(FILE *file, uint64_t file_size, sd_archive_segment_info_t *info) {
    if (!file || !info || file_size < SD_ARCHIVE_HEADER_BYTES + SD_ARCHIVE_FOOTER_BYTES) {
        return SD_ARCHIVE_CORRUPT;
    }
    if (validate_header(file) != SD_ARCHIVE_OK) {
        return SD_ARCHIVE_CORRUPT;
    }
    parsed_footer_t footer;
    if (read_footer(file, file_size, &footer) != SD_ARCHIVE_OK) {
        return SD_ARCHIVE_CORRUPT;
    }
    parsed_counts_t scanned;
    if (scan_records(file, file_size - SD_ARCHIVE_FOOTER_BYTES, &scanned, false, NULL) != SD_ARCHIVE_OK ||
        scanned.frame_count != footer.counts.frame_count ||
        scanned.payload_bytes != footer.counts.payload_bytes ||
        scanned.crc32 != footer.counts.crc32) {
        return SD_ARCHIVE_CORRUPT;
    }
    info->bytes = file_size;
    info->frame_count = footer.counts.frame_count;
    info->payload_bytes = footer.counts.payload_bytes;
    info->crc32 = footer.counts.crc32;
    info->complete = footer.complete;
    return SD_ARCHIVE_OK;
}

static int quarantine_open_file(const char *root, const char *path, const char *name) {
    char quarantine_dir[SD_ARCHIVE_PATH_MAX];
    char quarantine_path[SD_ARCHIVE_PATH_MAX];
    if (path_join(quarantine_dir, sizeof(quarantine_dir), root, "quarantine") != SD_ARCHIVE_OK ||
        make_directory(quarantine_dir) != SD_ARCHIVE_OK ||
        path_join(quarantine_path, sizeof(quarantine_path), quarantine_dir, name) != SD_ARCHIVE_OK ||
        file_exists(quarantine_path) || rename(path, quarantine_path) != 0 ||
        sync_parent_directory(quarantine_path) != SD_ARCHIVE_OK) {
        return SD_ARCHIVE_IO_ERROR;
    }
    return SD_ARCHIVE_OK;
}

typedef struct {
    sd_archive_store_t *store;
    uint32_t recovered;
} recovery_ctx_t;

static int recover_callback(const char *path, const char *name, void *ctx) {
    recovery_ctx_t *recovery = (recovery_ctx_t *)ctx;
    if (!recovery) {
        return SD_ARCHIVE_OK;
    }
    if (safe_archive_filename(name, true)) {
        FILE *ready_file = fopen(path, "rb");
        uint64_t ready_size = 0U;
        sd_archive_segment_info_t ready_info = {0};
        const int validation = ready_file && stream_size(ready_file, &ready_size) == SD_ARCHIVE_OK
                                   ? validate_stream(ready_file, ready_size, &ready_info)
                                   : SD_ARCHIVE_IO_ERROR;
        if (ready_file) {
            (void)fclose(ready_file);
        }
        return validation == SD_ARCHIVE_OK
                   ? SD_ARCHIVE_OK
                   : quarantine_open_file(recovery->store->root_dir, path, name);
    }
    if (!safe_archive_filename(name, false)) {
        return SD_ARCHIVE_OK;
    }
    FILE *file = fopen(path, "rb+");
    if (!file) {
        return SD_ARCHIVE_IO_ERROR;
    }
    uint64_t file_size = 0U;
    if (stream_size(file, &file_size) != SD_ARCHIVE_OK || validate_header(file) != SD_ARCHIVE_OK) {
        fclose(file);
        return quarantine_open_file(recovery->store->root_dir, path, name);
    }
    if (file_size >= SD_ARCHIVE_HEADER_BYTES + SD_ARCHIVE_FOOTER_BYTES) {
        unsigned char footer_magic[sizeof(k_footer_magic)];
        if (fseek(file, (long)(file_size - SD_ARCHIVE_FOOTER_BYTES), SEEK_SET) != 0 ||
            fread(footer_magic, 1U, sizeof(footer_magic), file) != sizeof(footer_magic)) {
            fclose(file);
            return SD_ARCHIVE_IO_ERROR;
        }
        bool has_footer = memcmp(footer_magic, k_footer_magic, sizeof(k_footer_magic)) == 0;
        if (has_footer) {
            sd_archive_segment_info_t info = {0};
            const int validation = validate_stream(file, file_size, &info);
            fclose(file);
            if (validation != SD_ARCHIVE_OK) {
                return quarantine_open_file(recovery->store->root_dir, path, name);
            }
            char ready_name[SD_ARCHIVE_PATH_MAX];
            char ready_path[SD_ARCHIVE_PATH_MAX];
            const size_t name_len = strlen(name);
            if (name_len <= 5U || snprintf(ready_name, sizeof(ready_name), "%.*s.ready",
                                           (int)(name_len - 5U), name) <= 0 ||
                path_join(ready_path, sizeof(ready_path), recovery->store->root_dir, ready_name) != SD_ARCHIVE_OK ||
                file_exists(ready_path) || rename(path, ready_path) != 0 ||
                sync_parent_directory(ready_path) != SD_ARCHIVE_OK) {
                return SD_ARCHIVE_IO_ERROR;
            }
            recovery->recovered++;
            return SD_ARCHIVE_OK;
        }
    }
    if (fseek(file, 0L, SEEK_SET) != 0) {
        fclose(file);
        return SD_ARCHIVE_IO_ERROR;
    }
    unsigned char header[SD_ARCHIVE_HEADER_BYTES];
    (void)fread(header, 1U, sizeof(header), file);
    uint64_t offset = SD_ARCHIVE_HEADER_BYTES;
    parsed_counts_t counts;
    if (scan_records(file, file_size, &counts, true, &offset) != SD_ARCHIVE_OK ||
        fseek(file, 0L, SEEK_END) != 0 ||
        write_footer(file, counts.frame_count, counts.payload_bytes, counts.crc32, false) != SD_ARCHIVE_OK ||
        sync_file(file) != SD_ARCHIVE_OK) {
        fclose(file);
        return SD_ARCHIVE_IO_ERROR;
    }
    if (fclose(file) != 0) {
        return SD_ARCHIVE_IO_ERROR;
    }
    char ready_path[SD_ARCHIVE_PATH_MAX];
    const size_t name_len = strlen(name);
    if (name_len <= 5U || snprintf(ready_path, sizeof(ready_path), "%s/%.*s.ready", recovery->store->root_dir,
                                    (int)(name_len - 5U), name) <= 0 || rename(path, ready_path) != 0 ||
        sync_parent_directory(ready_path) != SD_ARCHIVE_OK) {
        return SD_ARCHIVE_IO_ERROR;
    }
    recovery->recovered++;
    return SD_ARCHIVE_OK;
}

int sd_archive_store_recover_open(sd_archive_store_t *store,
                                  uint32_t *recovered_count) {
    if (!store || !store->initialized || store->instance_magic != SD_ARCHIVE_STORE_MAGIC ||
        store->active || !recovered_count) {
        return SD_ARCHIVE_INVALID_ARGUMENT;
    }
    recovery_ctx_t recovery = {.store = store, .recovered = 0U};
    const int result = for_each_file(store->root_dir, recover_callback, &recovery);
    *recovered_count = recovery.recovered;
    const int scan_result = rescan_usage(store);
    return result != SD_ARCHIVE_OK ? result : scan_result;
}

int sd_archive_store_inspect_ready(const sd_archive_store_t *store,
                                   const char *filename,
                                   sd_archive_segment_info_t *info) {
    if (!store || !store->initialized || store->instance_magic != SD_ARCHIVE_STORE_MAGIC ||
        !safe_archive_filename(filename, true) || !info) {
        return SD_ARCHIVE_INVALID_ARGUMENT;
    }
    char path[SD_ARCHIVE_PATH_MAX];
    if (path_join(path, sizeof(path), store->root_dir, filename) != SD_ARCHIVE_OK) {
        return SD_ARCHIVE_INVALID_ARGUMENT;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        return errno == ENOENT ? SD_ARCHIVE_NOT_FOUND : SD_ARCHIVE_IO_ERROR;
    }
    if (st.st_size < (off_t)(SD_ARCHIVE_HEADER_BYTES + SD_ARCHIVE_FOOTER_BYTES)) {
        return SD_ARCHIVE_CORRUPT;
    }
    FILE *file = fopen(path, "rb");
    if (!file) {
        return SD_ARCHIVE_IO_ERROR;
    }
    const int validation = validate_stream(file, (uint64_t)st.st_size, info);
    const int close_result = fclose(file);
    if (close_result != 0) {
        return SD_ARCHIVE_IO_ERROR;
    }
    if (validation != SD_ARCHIVE_OK) {
        return validation;
    }
    snprintf(info->filename, sizeof(info->filename), "%s", filename);
    return SD_ARCHIVE_OK;
}

int sd_archive_store_remove_ready(sd_archive_store_t *store,
                                  const char *filename) {
    if (!store || !store->initialized || store->instance_magic != SD_ARCHIVE_STORE_MAGIC ||
        !safe_archive_filename(filename, true)) {
        return SD_ARCHIVE_INVALID_ARGUMENT;
    }
    char path[SD_ARCHIVE_PATH_MAX];
    struct stat st;
    if (path_join(path, sizeof(path), store->root_dir, filename) != SD_ARCHIVE_OK ||
        stat(path, &st) != 0 || remove(path) != 0) {
        return errno == ENOENT ? SD_ARCHIVE_NOT_FOUND : SD_ARCHIVE_IO_ERROR;
    }
    if (sync_parent_directory(path) != SD_ARCHIVE_OK) {
        (void)rescan_usage(store);
        return SD_ARCHIVE_IO_ERROR;
    }
    if (store->used_bytes >= (uint64_t)st.st_size) {
        store->used_bytes -= (uint64_t)st.st_size;
    } else {
        store->used_bytes = 0U;
    }
    if (store->ready_count > 0U) {
        store->ready_count--;
    }
    return SD_ARCHIVE_OK;
}
