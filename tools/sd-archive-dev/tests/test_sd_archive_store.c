#include "sd_archive_store.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

static void make_dir(const char *path) {
#if defined(_WIN32)
    (void)_mkdir(path);
#else
    (void)mkdir(path, 0775);
#endif
}

static void remove_file(const char *path) {
    (void)remove(path);
}

static void cleanup(void) {
    remove_file("sd_archive_test_root/archive_archive_test_000000.ready");
    remove_file("sd_archive_test_root/archive_archive_test_000001.ready");
    remove_file("sd_archive_test_root/archive_archive_test_000002.ready");
    remove_file("sd_archive_test_root/archive_archive_test_000003.ready");
    remove_file("sd_archive_test_root/archive_archive_test_000000.open");
    remove_file("sd_archive_test_root/archive_archive_test_000001.open");
    remove_file("sd_archive_test_root/archive_archive_test_000002.open");
    remove_file("sd_archive_test_root/archive_archive_test_000003.open");
    remove_file("sd_archive_test_root/archive_small_000000.ready");
    remove_file("sd_archive_test_root/archive_small_000001.ready");
    remove_file("sd_archive_test_root/archive_small_000000.open");
    remove_file("sd_archive_test_root/archive_small_000001.open");
    remove_file("sd_archive_test_root/foreign.open");
    remove_file("sd_archive_test_root/archive_bad_000000.open");
    remove_file("sd_archive_test_root/quarantine/archive_bad_000000.open");
    remove_file("sd_archive_test_root/quarantine/archive_archive_test_000001.ready");
#if defined(_WIN32)
    (void)_rmdir("sd_archive_test_root/quarantine");
    (void)_rmdir("sd_archive_test_root");
#else
    (void)rmdir("sd_archive_test_root/quarantine");
    (void)rmdir("sd_archive_test_root");
#endif
}

static sd_archive_metadata_t metadata(const char *archive_id) {
    sd_archive_metadata_t value;
    memset(&value, 0, sizeof(value));
    snprintf(value.archive_id, sizeof(value.archive_id), "%s", archive_id);
    snprintf(value.audio_session_id, sizeof(value.audio_session_id), "monitoring_session_01");
    value.sample_rate = 16000U;
    value.channels = 1U;
    value.codec = 1U;
    value.frame_duration_ms = 20U;
    value.start_unix_ms = 1724000000000ULL;
    return value;
}

int main(void) {
    cleanup();
    make_dir("sd_archive_test_root");

    sd_archive_store_t store;
    const sd_archive_config_t config = {
        .root_dir = "sd_archive_test_root",
        .max_bytes = 1024U * 1024U,
        .max_segment_bytes = 512U,
    };
    assert(sd_archive_store_init(&store, &config) == SD_ARCHIVE_OK);

    const unsigned char frame_a[64] = {1, 2, 3, 4};
    const unsigned char frame_b[96] = {9, 8, 7, 6};
    const unsigned char frame_c[300] = {5, 6, 7, 8};
    const sd_archive_metadata_t first = metadata("archive_test");
    assert(sd_archive_store_begin(&store, &first) == SD_ARCHIVE_OK);
    assert(sd_archive_store_append_frame(&store, frame_a, sizeof(frame_a)) == SD_ARCHIVE_OK);
    assert(sd_archive_store_append_frame(&store, frame_b, sizeof(frame_b)) == SD_ARCHIVE_OK);
    assert(sd_archive_store_commit(&store, true) == SD_ARCHIVE_OK);

    uint64_t used = 0U;
    uint32_t ready = 0U;
    assert(sd_archive_store_get_usage(&store, &used, &ready) == SD_ARCHIVE_OK);
    assert(used > 0U && ready == 1U);

    assert(sd_archive_store_begin(&store, &first) == SD_ARCHIVE_OK);
    assert(sd_archive_store_append_frame(&store, frame_a, sizeof(frame_a)) == SD_ARCHIVE_OK);
    /* The second frame forces an automatic .ready -> new .open rotation. */
    assert(sd_archive_store_append_frame(&store, frame_c, sizeof(frame_c)) == SD_ARCHIVE_OK);
    assert(sd_archive_store_commit(&store, true) == SD_ARCHIVE_OK);
    assert(sd_archive_store_get_usage(&store, &used, &ready) == SD_ARCHIVE_OK);
    assert(ready == 3U);

    assert(sd_archive_store_begin(&store, &first) == SD_ARCHIVE_OK);
    assert(sd_archive_store_append_frame(&store, frame_a, sizeof(frame_a)) == SD_ARCHIVE_OK);
    assert(sd_archive_store_abort(&store) == SD_ARCHIVE_OK);
    uint32_t recovered = 0U;
    assert(sd_archive_store_recover_open(&store, &recovered) == SD_ARCHIVE_OK);
    assert(recovered == 1U);
    assert(sd_archive_store_get_usage(&store, &used, &ready) == SD_ARCHIVE_OK);
    assert(ready == 4U);

    sd_archive_segment_info_t info;
    assert(sd_archive_store_inspect_ready(&store,
                                          "archive_archive_test_000000.ready",
                                          &info) == SD_ARCHIVE_OK);
    assert(info.complete && info.frame_count == 2U && info.payload_bytes ==
           sizeof(frame_a) + sizeof(frame_b));
    assert(sd_archive_store_inspect_ready(&store,
                                          "archive_archive_test_000003.ready",
                                          &info) == SD_ARCHIVE_OK);
    assert(!info.complete && info.frame_count == 1U);

    FILE *corrupt = fopen("sd_archive_test_root/archive_archive_test_000001.ready", "rb+");
    assert(corrupt != NULL);
    assert(fseek(corrupt, -8L, SEEK_END) == 0);
    assert(fputc(0xff, corrupt) != EOF);
    assert(fclose(corrupt) == 0);
    assert(sd_archive_store_inspect_ready(&store,
                                          "archive_archive_test_000001.ready",
                                          &info) == SD_ARCHIVE_CORRUPT);

    assert(sd_archive_store_remove_ready(&store, "archive_archive_test_000000.ready") == SD_ARCHIVE_OK);
    assert(sd_archive_store_remove_ready(&store, "../archive_archive_test_000001.ready") ==
           SD_ARCHIVE_INVALID_ARGUMENT);
    assert(sd_archive_store_deinit(&store) == SD_ARCHIVE_OK);

    /* A foreign temporary file must not block archive recovery. */
    make_dir("sd_archive_test_root");
    FILE *foreign = fopen("sd_archive_test_root/foreign.open", "wb");
    assert(foreign != NULL);
    assert(fwrite("not an archive", 1U, 14U, foreign) == 14U);
    assert(fclose(foreign) == 0);
    assert(sd_archive_store_init(&store, &config) == SD_ARCHIVE_OK);
    recovered = 99U;
    assert(sd_archive_store_recover_open(&store, &recovered) == SD_ARCHIVE_OK);
    assert(recovered == 0U);

    /* A malformed archive is quarantined and does not abort the scan. */
    FILE *bad = fopen("sd_archive_test_root/archive_bad_000000.open", "wb");
    assert(bad != NULL);
    assert(fwrite("bad", 1U, 3U, bad) == 3U);
    assert(fclose(bad) == 0);
    assert(sd_archive_store_recover_open(&store, &recovered) == SD_ARCHIVE_OK);
#if defined(_WIN32)
    assert(_access("sd_archive_test_root/quarantine/archive_bad_000000.open", 0) == 0);
#else
    assert(access("sd_archive_test_root/quarantine/archive_bad_000000.open", F_OK) == 0);
#endif
    assert(recovered == 0U);
    assert(sd_archive_store_deinit(&store) == SD_ARCHIVE_OK);

    /* A frame too large for a segment must not leave the store permanently busy. */
    const sd_archive_config_t small_config = {
        .root_dir = "sd_archive_test_root",
        .max_bytes = 1024U * 1024U,
        .max_segment_bytes = 200U,
    };
    assert(sd_archive_store_init(&store, &small_config) == SD_ARCHIVE_OK);
    const sd_archive_metadata_t small = metadata("small");
    assert(sd_archive_store_begin(&store, &small) == SD_ARCHIVE_OK);
    assert(sd_archive_store_append_frame(&store, frame_a, sizeof(frame_a)) == SD_ARCHIVE_NO_SPACE);
    assert(!store.active);
    assert(sd_archive_store_begin(&store, &small) == SD_ARCHIVE_OK);
    assert(sd_archive_store_abort(&store) == SD_ARCHIVE_OK);
    assert(sd_archive_store_deinit(&store) == SD_ARCHIVE_OK);
    cleanup();
    puts("sd_archive_store tests: PASS");
    return 0;
}
