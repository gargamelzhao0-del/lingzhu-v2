#include "sd_audio_protocol.h"
#include "sd_audio_read_recovery_host.h"
#include "sd_audio_file_time.h"
#include "esp_heap_caps.h"
#include "sd_audio_retention_policy.h"
#include "offline_audio_policy.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <io.h>
#include "sd_audio_catalog.h"
#include "audio_frame.h"
#define CONFIG_NUWAX_SD_SESSION_ENABLE 1
#ifndef CONFIG_NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES
#define CONFIG_NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES 40000U
#endif
#undef NUWAX_OFFLINE_AUDIO_FREE_RESERVE_BYTES
#define NUWAX_OFFLINE_AUDIO_FREE_RESERVE_BYTES 100000U
#define HEADER_BYTES 96U
#define FOOTER_BYTES 32U
#define MAX_FRAME_BYTES 2048U
#define ARCHIVE_STDIO_BUFFER_BYTES 4096U
#define ARCHIVE_QUOTA_BYTES 100000000U
#define DURABILITY_SYNC_INTERVAL_FRAMES 100U
#define TAG "storage-host"
typedef void *QueueHandle_t;
typedef void *EventGroupHandle_t;
typedef void *SemaphoreHandle_t;
typedef void *TaskHandle_t;
typedef enum { NUWAX_OFFLINE_AUDIO_MODE_MEETING, NUWAX_OFFLINE_AUDIO_MODE_MONITORING } nuwax_offline_audio_mode_t;
#include "sd_storage_types_host.inc"
static spool_state_t s;
static sd_audio_session_t s_card_session;
static sd_audio_segment_t s_card_segment;
static sd_audio_session_time_t s_card_time;
static nuwax_cloud_time_anchor_t test_anchor = {UINT64_C(1800000000000), 0};
static atomic_bool s_card_created, s_card_failed, s_card_partial, s_card_pending_upload;
static char s_card_mirror_session[48];
static unsigned s_card_network_lock;
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
static esp_err_t nuwax_offline_audio_request_stop(bool complete)
{ (void)complete; return ESP_OK; }
static atomic_bool s_space_blocked, s_directory_dirty;
static uint64_t s_card_frame_start, clock_ms = 10000U;
static bool s_card_frame_realtime, card_active = true;
static char buffer[2U*ARCHIVE_STDIO_BUFFER_BYTES];
static bool nuwax_sd_session_active(void) { return card_active; }
static esp_err_t card_monitor_make_room_locked(bool ahead, uint32_t ms) { (void)ahead; (void)ms; return ESP_OK; }
static int64_t esp_timer_get_time(void) { return (int64_t)clock_ms * 1000; }
static void log_ignore(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
#define ESP_LOGE(...) log_ignore(__VA_ARGS__)
#define ESP_LOGW(...) log_ignore(__VA_ARGS__)
static void log_sd_step(const char *name, int64_t stamp) { (void)name; (void)stamp; }
static const char *s_writer_fault_stage;
static int s_writer_fault_errno;
static void writer_note_fault(const char *stage, int error)
{ if (!s_writer_fault_stage) { s_writer_fault_stage = stage; s_writer_fault_errno = error; } }
static bool nuwax_sd_card_is_mounted(void) { return true; }
static void nuwax_sd_card_mark_io_fault(int error) { (void)error; }
static int nuwax_device_error_report_enqueue(const char *a, const char *b, const char *c,
    int d, const char *e, uint32_t f, const char *g) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g; return 0; }
static int esp_vfs_fat_info(const char *p, uint64_t *total, uint64_t *available)
{ (void)p; *total = *available = 1000000000ULL; return ESP_OK; }
static bool path_exists(const char *path) { struct stat st; return stat(path, &st) == 0; }
static size_t strlcpy(char *to, const char *from, size_t size) { snprintf(to, size, "%s", from); return strlen(from); }
static bool archive_new_paths(uint32_t index, const char *session, char *part, size_t psize, char *ready, size_t rsize)
{
    int a = snprintf(part, psize, "./offline_%s_%08lu.part", session, (unsigned long)index);
    int b = snprintf(ready, rsize, "./offline_%s_%08lu.ready", session, (unsigned long)index);
    return a > 0 && b > 0 && (size_t)a < psize && (size_t)b < rsize;
}
static bool fail_first_frame_sync;
static bool sync_archive_file(FILE *f, const char *step) {
    if (fail_first_frame_sync && !strcmp(step,"first_frame_sync")) { errno=EIO;return false; }
    return fflush(f) == 0 && _commit(fileno(f)) == 0;
}
typedef struct {
    uint32_t rejected_frames, last_drop_log_ms, staging_count;
    bool complete;
    char session_id[48];
} nuwax_offline_audio_mirror_t;
static uint32_t offline_now_ms(void) { return (uint32_t)clock_ms; }
#include "sd_storage_loss_host.inc"
#include "sd_storage_crc_host.inc"
static void card_save_time_locked(void)
{
    assert(sd_audio_catalog_session_time(&s_card_session, 123, &test_anchor,
        false, 0, &s_card_time) == ESP_OK);
}
#include "sd_storage_catalog_host.inc"
#include "sd_storage_functions_host.inc"

static void begin(const char *id, unsigned mode)
{
    static unsigned owner = 0;
    memset(&s, 0, sizeof(s)); memset(&s_card_session, 0, sizeof(s_card_session));
    fail_first_frame_sync=false;
    memset(&s_card_time, 0, sizeof(s_card_time));
    strcpy(s_card_session.session_id, id); strcpy(s_card_mirror_session, id);
    memset(s_card_session.owner, ++owner, 32);
    s_card_session.start_ms = clock_ms; s_card_session.mode = mode;
    s_card_session.boot_id = 123; s_card_session.complete = 1;
    assert(sd_audio_catalog_create(&s_card_session) == ESP_OK);
    s_card_created = true; s.active = true; s.mode = mode; s.capture_generation = 1;
    s_card_pending_upload = false; s_card_failed = false;
    s.sample_rate = 16000; s.channels = 1; s.frame_duration_ms = 20;
    s.write_buffer = buffer; strcpy(s.session_id, id);
}
static void frame(unsigned sequence, bool live)
{
    int16_t pcm[320] = {0};
    spool_item_t item = {.type=1, .generation=1, .realtime=live};
    pcm[0] = (int16_t)sequence;
    assert(nuwax_audio_frame_build_pcm16(item.data, sizeof(item.data), pcm, 320,
        sequence, (uint32_t)clock_ms, 0, 16000, 1, 20, &item.bytes) == ESP_OK);
    assert(append_locked(&item) == ESP_OK && s.writer_frame_written);
    clock_ms += 20;
}
static sd_audio_segment_t part(unsigned index, unsigned first_sequence)
{
    sd_audio_segment_t segment;
    assert(sd_audio_catalog_read(s.session_id, index, &segment) == ESP_OK);
    assert(segment.sealed && strstr(segment.path, "/card_"));
    FILE *f = fopen(segment.path, "rb"); assert(f);
    archive_header_t header; archive_footer_t footer;
    assert(fread(&header, 1, sizeof(header), f) == sizeof(header));
    assert(!strcmp(header.session_id, s.session_id) && header.segment_index == index);
    assert(header.start_uptime_ms == segment.start_ms && (header.reserved0 & 2));
    assert(header.file_started_at_ms == sd_audio_file_started_at(&s_card_time,
        s_card_session.start_ms, segment.start_ms));
    assert(header.file_duration_ms == segment.end_ms - segment.start_ms);
    assert(fseek(f, -32, SEEK_END) == 0 && fread(&footer, 1, 32, f) == 32);
    assert(footer.crc32 == segment.payload_crc && footer.complete);
    assert((uint32_t)ftell(f) == segment.total_bytes);
    assert(segment.end_ms - segment.start_ms == (uint64_t)footer.frame_count * 20U);
    assert(fseek(f, sizeof(header), SEEK_SET) == 0);
    uint32_t crc = 0;
    uint64_t payload_bytes = 0;
    for (uint32_t n = 0; n < footer.frame_count; ++n) {
        uint32_t length = 0, sequence = 0, timestamp = 0;
        uint16_t raw[MAX_FRAME_BYTES / 2U], rate = 0;
        uint8_t *data = (uint8_t *)raw, channels = 0;
        const int16_t *pcm = NULL;
        size_t samples = 0;
        int16_t expected[320] = {0};
        assert(fread(&length, 1, sizeof(length), f) == sizeof(length));
        assert(length == 662U && fread(data, 1, length, f) == length);
        assert(nuwax_audio_frame_parse_pcm16(data, length, &pcm, &samples,
            &sequence, &rate, &channels) == ESP_OK);
        assert(samples == 320U && rate == 16000U && channels == 1U);
        memcpy(&timestamp, data + 14U, sizeof(timestamp));
        assert(sequence == first_sequence + n);
        assert(timestamp == (uint32_t)(segment.start_ms + n * 20U));
        expected[0] = (int16_t)sequence;
        assert(memcmp(pcm, expected, sizeof(expected)) == 0);
        crc = crc32_update(crc, data, length);
        payload_bytes += length;
    }
    assert(crc == footer.crc32 && payload_bytes == footer.payload_bytes);
    assert((uint32_t)ftell(f) + FOOTER_BYTES == segment.total_bytes);
    fclose(f); return segment;
}

static void monitor_full_files(bool live)
{
    begin(live ? "monitor_full_online" : "monitor_full_offline", 1);
    const uint64_t started = clock_ms;
    const unsigned per_file = (CONFIG_NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES -
        HEADER_BYTES - FOOTER_BYTES) / 666U;
    for (unsigned i = 0; i < per_file - 1U; ++i) {
        frame(i, live);
        assert(s.file && !s_card_pending_upload);
    }
    sd_audio_session_t found;
    sd_audio_segment_t selected;
    bool have = false, file = false;
    assert(sd_audio_catalog_next(s_card_session.owner, 123, clock_ms, started,
        s.session_id, false, 1, &found, &selected, &have, &file) == ESP_OK && !have);
    frame(per_file - 1U, live);
    assert(!s.file && s_card_pending_upload && s_card_session.segment_count == 1U);
    sd_audio_segment_t a = part(0, 0);
    assert(a.total_bytes == HEADER_BYTES + FOOTER_BYTES + per_file * 666U);
    assert(a.total_bytes <= CONFIG_NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES &&
        a.total_bytes + 666U > CONFIG_NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES);
    assert(a.realtime == live && !s_card_session.ended);
    assert(commit_segment_locked(true) == ESP_OK && s_card_session.segment_count == 1U);
    /* A full file is selectable while the next file is still being recorded. */
    frame(per_file, live);
    assert(s.file && s_card_session.segment_count == 2U);
    assert(sd_audio_catalog_next(s_card_session.owner, 123, clock_ms, started,
        s.session_id, live, 1, &found, &selected, &have, &file) == ESP_OK && have && file);
    assert(selected.index == 0U && !found.ended);
    for (unsigned i = per_file + 1U; i < 2U * per_file; ++i) frame(i, live);
    assert(!s.file && s_card_session.segment_count == 2U);
    sd_audio_segment_t b = part(1, per_file);
    for (unsigned i = 2U * per_file; i < 2U * per_file + 7U; ++i) frame(i, live);
    assert(s.file && commit_segment_locked(true) == ESP_OK);
    sd_audio_segment_t c = part(2, 2U * per_file);
    assert(b.total_bytes == a.total_bytes && c.total_bytes == HEADER_BYTES + FOOTER_BYTES + 7U * 666U);
    assert(a.end_ms == b.start_ms && b.end_ms == c.start_ms &&
        c.end_ms == started + (2U * per_file + 7U) * 20U);
    assert(commit_segment_locked(true) == ESP_OK && s_card_session.segment_count == 3U);
}

int main(void)
{
    begin("meeting_storage", 0);
    const unsigned per_file=(CONFIG_NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES-HEADER_BYTES-FOOTER_BYTES)/666U;
    const unsigned frames=2U*per_file+7U;
    for (unsigned i=0; i<frames; ++i) frame(i, true);
    assert(commit_segment_locked(true) == ESP_OK);
    assert(atomic_load(&s.ready_count) == 0); // Card catalog alone owns its pending work.
    assert(s_card_session.segment_count == 3);
    sd_audio_segment_t a=part(0,0), b=part(1,per_file), c=part(2,2U*per_file);
    assert(a.start_ms == 10000 && a.end_ms == b.start_ms && b.end_ms == c.start_ms && c.end_ms == 10000U+20U*frames);
    assert(a.total_bytes<=CONFIG_NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES &&
        b.total_bytes==a.total_bytes && c.total_bytes==HEADER_BYTES+FOOTER_BYTES+7U*666U);
    sd_audio_session_t found; sd_audio_segment_t selected; bool have, file;
    assert(sd_audio_catalog_next(s_card_session.owner,123,clock_ms,0,NULL,false,-1,&found,&selected,&have,&file) == ESP_OK && !have);
    s_card_session.ended=1; assert(sd_audio_catalog_save(&s_card_session)==ESP_OK);
    assert(sd_audio_catalog_next(s_card_session.owner,123,clock_ms,0,NULL,false,-1,&found,&selected,&have,&file) == ESP_OK && file);
    begin("monitor_storage", 1);
    for (unsigned i=0; i<48; ++i) frame(i, true);
    assert(s.file && !s_card_pending_upload);
    for (unsigned i=48; i<60; ++i) frame(i, false);
    a=part(0,0); assert(a.end_ms-a.start_ms == 960);
    frame(60, true); b=part(1,48); assert(!b.realtime && b.end_ms-b.start_ms == 240);
    assert(commit_segment_locked(true)==ESP_OK); c=part(2,60);
    assert(a.end_ms==b.start_ms && b.end_ms==c.start_ms && c.realtime);
    assert(sd_audio_catalog_ack(s.session_id,0)==ESP_OK);
    assert(path_exists(a.path));
    assert(sd_audio_catalog_next(s_card_session.owner,123,clock_ms,0,NULL,false,1,&found,&selected,&have,&file)==ESP_OK && file);
    assert(!path_exists(a.path) && path_exists(b.path));
    /* A pause seals a short tail; resuming preserves the scene and real gap. */
    clock_ms += 1000U;
    frame(61, true);
    assert(commit_segment_locked(true) == ESP_OK);
    sd_audio_segment_t resumed = part(3,61);
    assert(resumed.start_ms == c.end_ms + 1000U && s_card_session.segment_count == 4U);
    monitor_full_files(true);
    monitor_full_files(false);
    nuwax_offline_audio_mirror_t tail = {.complete=true};
    s_card_failed=false; s.capture_complete=true;
    mirror_note_loss(&tail, 1);
    assert(!s_card_failed && s.capture_complete && !tail.complete);
    strcpy(tail.session_id, s_card_session.session_id);
    tail.complete = true;
    mirror_note_loss(&tail, 1);
    assert(s_card_failed && !s.capture_complete && !tail.complete);
    puts("PASS: actual SD writer: full-file online/offline monitoring, immediate seal, next-file capture, short tails, no empty files, every PCM/sequence/timestamp/CRC, pause/live boundaries, meeting barrier and ACK-before-delete cleanup");
    return 0;
}
