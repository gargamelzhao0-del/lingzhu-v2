/* Exercise the production writer and .part recovery against real files. */
#define main original_storage_main
#include "sd_audio_storage_host_check.c"
#undef main
#include <dirent.h>
#define fsync _commit
#define ESP_LOGI(...) log_ignore(__VA_ARGS__)
typedef struct { DIR *dir; int error; } archive_dir_t;
static archive_dir_t *archive_dir_open(archive_dir_t *walk)
{ memset(walk, 0, sizeof(*walk)); walk->dir = opendir("."); return walk->dir ? walk : NULL; }
static struct dirent *archive_dir_read(archive_dir_t *walk) { return readdir(walk->dir); }
static void archive_dir_close(archive_dir_t *walk) { closedir(walk->dir); }
static bool compose_child_path(char *out, size_t size, const char *name)
{ const int n = snprintf(out, size, "./%s", name); return n > 0 && (size_t)n < size; }
#include "sd_file_validator_host.inc"
#include "sd_file_recovery_host.inc"

static const uint64_t epoch = UINT64_C(1800000000000);
static const char *fixture_dir;
static unsigned checks;
static void check_file(unsigned index, uint64_t utc, uint32_t span, unsigned frames,
    bool complete, bool export)
{
    sd_audio_segment_t segment;
    assert(sd_audio_catalog_read(s.session_id, index, &segment) == ESP_OK);
    FILE *file = fopen(segment.path, "rb"); assert(file);
    archive_header_t header; archive_footer_t footer;
    assert(fread(&header, 1, 96, file) == 96);
    assert(header.file_started_at_ms == utc && header.file_duration_ms == span);
    assert(fseek(file, -32, SEEK_END) == 0 && fread(&footer, 1, 32, file) == 32);
    assert(footer.complete == complete && footer.frame_count == frames);
    const long bytes = ftell(file); assert(bytes == 128 + (long)frames * 666);
    if (export && fixture_dir) {
        char output[1024]; snprintf(output, sizeof(output), "%s/%s_%u.nwx", fixture_dir, s.session_id, index);
        FILE *copy = fopen(output, "wb"); assert(copy && fseek(file, 0, SEEK_SET) == 0);
        uint8_t block[4096]; size_t n;
        while ((n = fread(block, 1, sizeof(block), file))) assert(fwrite(block, 1, n, copy) == n);
        assert(!ferror(file) && fclose(copy) == 0);
    }
    fclose(file); checks++;
}
static void reset(const char *id, unsigned mode, uint64_t uptime, bool calibrated)
{
    clock_ms = uptime; test_anchor = (nuwax_cloud_time_anchor_t){calibrated ? epoch : 0, 0};
    begin(id, mode);
}
static void crash(bool torn_record)
{
    assert(s.file);
    if (torn_record) {
        uint32_t len = 662; uint8_t partial[30] = {0};
        assert(fwrite(&len, 1, 4, s.file) == 4 && fwrite(partial, 1, sizeof(partial), s.file) == sizeof(partial));
    }
    assert(sync_archive_file(s.file, "test_crash") && fclose(s.file) == 0); s.file = NULL;
    /* A different boot clock must never be used for this old archive. */
    clock_ms = 500; test_anchor = (nuwax_cloud_time_anchor_t){epoch + 90000000, 500};
    assert(recover_open_segments() == ESP_OK);
}
int main(int argc, char **argv)
{
    fixture_dir = argc > 1 ? argv[1] : NULL;
    reset("time_online", 0, 10000, true);
    frame(0, true); frame(1, true); assert(commit_segment_locked(true) == ESP_OK);
    check_file(0, epoch + 10000, 40, 2, true, true);
    /* Heartbeat adjustments cannot move later files away from the fixed scene clock. */
    test_anchor.utc_ms += 600000; frame(2, true); assert(commit_segment_locked(true) == ESP_OK);
    check_file(1, epoch + 10040, 20, 1, true, true);

    reset("time_offline", 0, UINT64_C(28810000), true);
    frame(0, false); frame(1, false); assert(commit_segment_locked(true) == ESP_OK);
    check_file(0, epoch + 28810000, 40, 2, true, true);
    reset("time_gap", 1, 70000, true);
    frame(0, true); clock_ms += 120000; frame(1, true); assert(commit_segment_locked(true) == ESP_OK);
    check_file(0, epoch + 70000, 120040, 2, true, true);
    reset("time_wrap", 0, UINT64_C(0x100000000) - 10, true);
    frame(0, false); frame(1, false); assert(commit_segment_locked(true) == ESP_OK);
    check_file(0, epoch + UINT64_C(0x100000000) - 10, 40, 2, true, true);

    reset("time_unknown", 0, 1000, false);
    frame(0, false); assert(commit_segment_locked(true) == ESP_OK);
    check_file(0, 0, 20, 1, true, true);
    reset("time_late", 0, 3000, false);
    frame(0, false); test_anchor = (nuwax_cloud_time_anchor_t){epoch + clock_ms, clock_ms};
    frame(1, false); assert(commit_segment_locked(true) == ESP_OK);
    check_file(0, epoch + 3000, 40, 2, true, true);

    reset("time_crash", 1, UINT64_C(0x100000000) - 10, true);
    frame(0, false); frame(1, false); crash(true);
    check_file(0, epoch + UINT64_C(0x100000000) - 10, 40, 2, false, true);
    reset("time_clock_recover", 0, 9000, false);
    frame(0, false); frame(1, false);
    const nuwax_cloud_time_anchor_t saved = {epoch + clock_ms, clock_ms};
    assert(sd_audio_catalog_clock_save(123, s_card_session.owner, &saved) == ESP_OK);
    crash(false); check_file(0, epoch + 9000, 40, 2, false, true);
    reset("time_crash_unknown", 0, 12000, false);
    frame(0, false); crash(false); check_file(0, 0, 20, 1, false, true);

    reset("time_torn_time", 0, 20000, true);
    frame(0, false); frame(1, false);
    assert(sd_audio_file_time_write(s.file, epoch + 123, 999));
    crash(false); check_file(0, epoch + 20000, 40, 2, false, true);

    reset("time_rename", 0, 30000, true);
    frame(0, false); assert(commit_segment_locked(true) == ESP_OK);
    sd_audio_segment_t segment; assert(sd_audio_catalog_read(s.session_id, 0, &segment) == ESP_OK);
    char part_path[160]; snprintf(part_path, sizeof(part_path), "%.*s.part", (int)strlen(segment.path) - 6, segment.path);
    uint8_t before[794], after[794]; FILE *f = fopen(segment.path, "rb"); assert(f);
    assert(fread(before, 1, sizeof(before), f) == sizeof(before)); fclose(f);
    assert(rename(segment.path, part_path) == 0 && recover_open_segments() == ESP_OK);
    assert(recover_open_segments() == ESP_OK);
    f = fopen(segment.path, "rb"); assert(f && fread(after, 1, sizeof(after), f) == sizeof(after)); fclose(f);
    assert(memcmp(before, after, sizeof(before)) == 0);
    check_file(0, epoch + 30000, 20, 1, true, true);

    reset("time_full", 1, 40000, true);
    for (unsigned i = 0; i < 6297; ++i) frame(i, true);
    assert(!s.file && s_card_pending_upload);
    check_file(0, epoch + 40000, 125940, 6297, true, true);
    assert(sd_audio_file_duration(0, UINT32_MAX) == UINT32_MAX);
    assert(!sd_audio_file_duration(0, UINT64_C(0x100000000)));
    assert(!sd_audio_file_duration(30, 20));
    printf("PASS: %u actual-writer file timestamps, offline projection, clock drift, gaps, wrap, unknown/late sync, power loss, partial records, torn time, rename immutability and full 4MiB file\n", checks);
    return 0;
}
