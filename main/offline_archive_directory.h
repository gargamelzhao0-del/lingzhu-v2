#pragma once
/* Legacy archives stay in place. New files use at most 32 indices per leaf,
 * so FAT directory lookup/rename cost does not grow with recording history. */
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#if CONFIG_NUWAX_SD_DIRECTORY_GUARD_ENABLE
#include "board/sd_card_driver.h"
#endif
#ifndef ARCHIVE_NEW_ROOT
#define ARCHIVE_NEW_ROOT "/sdcard/offline2"
#endif
#define ARCHIVE_BUCKET_SEGMENTS 32U
typedef struct {
    DIR *legacy, *buckets, *leaf;
    struct dirent entry;
    char bucket[9];
    int error;
} archive_dir_t;

static bool archive_bucket_name(const char *name)
{
    if (strlen(name) != 8U) return false;
    for (unsigned i = 0; i < 8U; ++i)
        if (!((name[i] >= '0' && name[i] <= '9') ||
              (name[i] >= 'a' && name[i] <= 'f') ||
              (name[i] >= 'A' && name[i] <= 'F'))) return false;
    return true;
}

static bool compose_child_path(char *dst, size_t size, const char *name)
{
    if (!dst || !size || !name) return false;
    const char *root = ROOT_DIR;
    if (strncmp(name, "v2/", 3U) == 0) {
        name += 3U;
        if (strlen(name) < 10U || name[8] != '/') return false;
        char bucket[9]; memcpy(bucket, name, 8U); bucket[8] = 0;
        if (!archive_bucket_name(bucket) || strchr(name + 9U, '/')) return false;
        root = ARCHIVE_NEW_ROOT;
    } else if (strchr(name, '/')) return false;
    if (strchr(name, '\\') || strstr(name, "..")) return false;
    const int n = snprintf(dst, size, "%s/%s", root, name);
    return n >= 0 && (size_t)n < size;
}

static int archive_check_directory(const char *path, bool allow_missing)
{
#if CONFIG_NUWAX_SD_DIRECTORY_GUARD_ENABLE
    const esp_err_t err = nuwax_sd_card_validate_directory(path, allow_missing);
    if (err != ESP_OK) {
        errno = err == ESP_ERR_NO_MEM ? ENOMEM : err == ESP_ERR_TIMEOUT ? EBUSY : EIO;
        return -1;
    }
#else
    (void)path; (void)allow_missing;
#endif
    return 0;
}

static int archive_make_directory(const char *path)
{
    struct stat st;
    if (archive_check_directory(path, true)) return -1;
    if (mkdir(path, 0775) != 0 &&
        !(errno == EEXIST && stat(path, &st) == 0 && S_ISDIR(st.st_mode))) return -1;
    return archive_check_directory(path, false);
}

static bool archive_new_paths(uint32_t index, const char *session, char *part, size_t part_size,
                              char *ready, size_t ready_size)
{
    /* A missing leaf must not hide a damaged existing parent before mkdir. */
    if (archive_check_directory(ARCHIVE_NEW_ROOT, false)) return false;
    char bucket[96];
    int n = snprintf(bucket, sizeof(bucket), "%s/%08lx", ARCHIVE_NEW_ROOT,
                     (unsigned long)(index / ARCHIVE_BUCKET_SEGMENTS));
    if (n < 0 || (size_t)n >= sizeof(bucket) || archive_make_directory(bucket)) return false;
    n = snprintf(part, part_size, "%s/offline_%s_%08lu.part", bucket, session, (unsigned long)index);
    if (n < 0 || (size_t)n >= part_size) return false;
    n = snprintf(ready, ready_size, "%s/offline_%s_%08lu.ready", bucket, session, (unsigned long)index);
    return n >= 0 && (size_t)n < ready_size;
}

static void archive_dir_close(archive_dir_t *d)
{
    if (d->leaf) closedir(d->leaf);
    if (d->legacy) closedir(d->legacy);
    if (d->buckets) closedir(d->buckets);
    d->leaf = d->legacy = d->buckets = NULL;
}

static archive_dir_t *archive_dir_open(archive_dir_t *d)
{
    memset(d, 0, sizeof(*d));
    if (archive_check_directory(ROOT_DIR, false) || archive_check_directory(ARCHIVE_NEW_ROOT, false)) {
        d->error = errno; return NULL;
    }
    d->legacy = opendir(ROOT_DIR);
    if (d->legacy) d->buckets = opendir(ARCHIVE_NEW_ROOT);
    if (!d->legacy || !d->buckets) {
        d->error = errno ? errno : EIO;
        archive_dir_close(d); errno = d->error; return NULL;
    }
    return d;
}

static struct dirent *archive_dir_read(archive_dir_t *d)
{
    for (;;) {
        struct dirent *e;
        if (d->legacy) {
            errno = 0; e = readdir(d->legacy);
            if (e) return e;
            if (errno) { d->error = errno; return NULL; }
            closedir(d->legacy); d->legacy = NULL;
        }
        if (d->leaf) {
            errno = 0; e = readdir(d->leaf);
            if (e) {
                if (e->d_name[0] == '.') continue;
                const int n = snprintf(d->entry.d_name, sizeof(d->entry.d_name),
                                       "v2/%s/%s", d->bucket, e->d_name);
                if (n < 0 || (size_t)n >= sizeof(d->entry.d_name)) {
                    d->error = ENAMETOOLONG; return NULL;
                }
                return &d->entry;
            }
            if (errno) { d->error = errno; return NULL; }
            closedir(d->leaf); d->leaf = NULL;
        }
        errno = 0; e = readdir(d->buckets);
        if (!e) { if (errno) d->error = errno; return NULL; }
        if (!archive_bucket_name(e->d_name)) continue;
        memcpy(d->bucket, e->d_name, 9U);
        char path[96];
        const int n = snprintf(path, sizeof(path), "%s/%s", ARCHIVE_NEW_ROOT, d->bucket);
        if (n < 0 || (size_t)n >= sizeof(path)) { d->error = ENAMETOOLONG; return NULL; }
        if (archive_check_directory(path, false)) { d->error = errno; return NULL; }
        d->leaf = opendir(path);
        if (!d->leaf) { d->error = errno ? errno : EIO; return NULL; }
    }
}

static void archive_remove_empty_bucket(const char *file)
{
    const size_t root_len = strlen(ARCHIVE_NEW_ROOT);
    if (strncmp(file, ARCHIVE_NEW_ROOT, root_len) || file[root_len] != '/') return;
    const char *leaf = file + root_len + 1U;
    if (strlen(leaf) < 10U || leaf[8] != '/') return;
    char bucket[9]; memcpy(bucket, leaf, 8U); bucket[8] = 0;
    if (!archive_bucket_name(bucket)) return;
    char path[96];
    const int n = snprintf(path, sizeof(path), "%s/%s", ARCHIVE_NEW_ROOT, bucket);
    if (n > 0 && (size_t)n < sizeof(path)) (void)rmdir(path); /* Only succeeds if empty. */
}
