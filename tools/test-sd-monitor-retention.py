"""Real catalog/SD writer and capacity policy, isolated from the user's SD data."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='nuwax-monitor-retention-') as directory:
    work = Path(directory)
    exe = work / 'retention.exe'
    subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-DSD_AUDIO_CATALOG_ROOT="catalog"', '-DSD_AUDIO_ARCHIVE_ROOT="audio"',
        '-DSD_AUDIO_ARCHIVE_NEW_ROOT="audio2"', '-DESP_ERR_INVALID_CRC=0x109', '-DESP_ERR_NOT_FOUND=0x105',
        '-I' + str(root / 'main'), '-I' + str(root / 'main/audio_modes/meeting/host_stubs'),
        str(root / 'main/sd_monitor_retention_host_check.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], cwd=work, check=True)

    # The actual archive writer and actual reclamation loop. Only the FAT
    # capacity query/clock/RTOS peripherals are doubled; file bytes are real.
    spool = (root / 'main/offline_audio_spool.c').read_text(encoding='utf-8')
    storage = (root / 'main/sd_audio_session_storage.inc').read_text(encoding='utf-8')
    room = (root / 'main/sd_monitor_storage.inc').read_text(encoding='utf-8')
    pieces = {
        'types': spool[spool.index('typedef struct {\n    uint8_t type;'):spool.index('static spool_state_t s;')],
        'crc': spool[spool.index('static uint32_t crc32_update('):spool.index('static esp_err_t validate_archive_frame(')],
        'catalog': storage[storage.index('static bool card_recording_locked('):storage.rindex('#endif')],
        'functions': spool[spool.index('static FILE *archive_create_exclusive('):spool.index('static void writer_schedule_retry_locked(')],
        'loss': spool[spool.index('static void mirror_note_loss('):spool.index('static void mirror_drain(')],
    }
    for name, code in pieces.items():
        (work / f'sd_storage_{name}_host.inc').write_text(code, encoding='utf-8')
    source = (root / 'main/sd_audio_storage_host_check.c').read_text(encoding='utf-8')
    source = source[:source.index('int main(void)')]
    source = source.replace('static esp_err_t card_monitor_make_room_locked(bool ahead, uint32_t ms) { (void)ahead; (void)ms; return ESP_OK; }',
        'static esp_err_t card_monitor_make_room_locked(bool ahead, uint32_t ms);')
    source = source.replace('static int esp_vfs_fat_info(',
        'static uint64_t simulated_capacity, protected_bytes;\nstatic atomic_bool s_monitor_space_blocked;\nstatic int esp_vfs_fat_info(')
    source = source.replace('{ (void)p; *total = *available = 1000000000ULL; return ESP_OK; }',
        '{ (void)p; *total = simulated_capacity; assert(simulated_capacity >= protected_bytes + s.used_bytes);\n'
        ' *available = simulated_capacity - protected_bytes - s.used_bytes; return ESP_OK; }')
    source = source.replace('#include "sd_storage_functions_host.inc"',
        'static void archive_remove_empty_bucket(const char *p) { (void)p; }\n' +
        room[room.index('static esp_err_t card_monitor_make_room_locked('):room.index('static esp_err_t card_monitor_prepare_space(')] +
        '\n#include "sd_storage_functions_host.inc"')
    source += r'''
int main(int argc, char **argv)
{
    assert(argc == 2);
    simulated_capacity = (uint64_t)strtoul(argv[1], NULL, 10) * 1024U * 1024U * 1024U;
    begin("writer_ring", 1);
    assert(sd_audio_catalog_enable_retention(s.session_id) == ESP_OK);
    /* Existing protected audio already exceeds the old 1.5 GiB quota. */
    s.used_bytes = 2ULL * 1024U * 1024U * 1024U;
    protected_bytes = simulated_capacity - s.used_bytes - sd_monitor_reserve_bytes(simulated_capacity)
        - 4U * CONFIG_NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES - 65536U;
    const unsigned frames = (CONFIG_NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES - 128U) / 666U;
    const uint64_t origin = clock_ms;
    for (unsigned i = 0; i < frames * 9U; ++i) {
        frame(i, false);
        if ((i + 1U) % frames == 0U && (i + 1U) / frames >= 3U)
            assert(sd_audio_catalog_ack(s.session_id, (i + 1U) / frames - 3U) == ESP_OK);
    }
    assert(!s.file && s_card_session.segment_count == 9U);
    /* The last sealed file consumed space since the last admission snapshot.
     * Run the actual bounded maintenance step before checking the watermark. */
    assert(card_monitor_make_room_locked(false, 50U) == ESP_OK);
    uint64_t total, available;
    assert(esp_vfs_fat_info("/sdcard", &total, &available) == ESP_OK);
    assert(available >= sd_monitor_required_bytes(total, CONFIG_NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES, false));
    /* Used bytes above the old quota must not latch a false capacity fault
     * while the actual card still has its required reserve. */
    assert(!s_space_blocked && !s_monitor_space_blocked);
    sd_audio_session_t saved;
    assert(sd_audio_catalog_load(s.session_id, &saved) == ESP_OK && saved.complete && !saved.ended);
    bool cleaned = false;
    for (unsigned i = 0; i < 9U; ++i) {
        sd_audio_segment_t recorded;struct stat st;
        assert(sd_audio_catalog_read(s.session_id, i, &recorded) == ESP_OK && !recorded.missing);
        assert(recorded.acknowledged == (i < 7U));
        if (stat(recorded.path, &st) == 0) (void)part(i, i * frames);
        else { assert(recorded.acknowledged && errno == ENOENT); cleaned = true; }
    }
    assert(cleaned);
    /* Reconnect uses the same identity/index stream and seals the short tail. */
    frame(frames * 9U, false);
    clock_ms += 9ULL * 3600000U; /* Offline period beyond the old eight-hour limit. */
    frame(frames * 9U + 1U, true);
    assert(commit_segment_locked(true) == ESP_OK);
    assert(s_card_session.segment_count == 11U && !strcmp(s_card_session.session_id, "writer_ring"));
    sd_audio_segment_t offline, online;
    assert(sd_audio_catalog_read(s.session_id, 9, &offline) == ESP_OK);
    assert(sd_audio_catalog_read(s.session_id, 10, &online) == ESP_OK);
    assert(!offline.realtime && online.realtime && online.start_ms > origin + 8ULL * 3600000U);
    s_card_session.ended = 1; s_card_session.ended_ms = clock_ms;
    assert(sd_audio_catalog_save(&s_card_session) == ESP_OK && s_card_session.complete);
    assert(s.used_bytes < 2ULL * 1024U * 1024U * 1024U + 4U * CONFIG_NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES);
    puts("PASS: actual data-first writer: 8/32/64 GiB, >1.5 GiB used, only ACKed files reclaimed, all unconfirmed PCM/CRC retained, offline/online identity and final complete marker");
    return 0;
}
'''
    source_file = work / 'writer.c'
    source_file.write_text(source, encoding='utf-8')
    writer = work / 'writer.exe'
    subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
        '-DCONFIG_NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES=4194304',
        '-DSD_AUDIO_CATALOG_ROOT="catalog"', '-DSD_AUDIO_ARCHIVE_ROOT="."',
        '-DESP_ERR_INVALID_CRC=0x109', '-DESP_ERR_NOT_FOUND=0x105',
        '-I' + str(work), '-I' + str(root / 'main'),
        '-I' + str(root / 'main/audio_modes/meeting/host_stubs'),
        str(source_file), str(root / 'main/sd_audio_catalog.c'), str(root / 'main/audio_frame.c'),
        '-o', str(writer)], check=True)
    for capacity in (8, 32, 64):
        test_dir = work / f'card-{capacity}'
        test_dir.mkdir()
        subprocess.run([str(writer), str(capacity)], cwd=test_dir, check=True)

    limit = spool[spool.index('        uint32_t offline_limit_hours ='):spool.index('        const size_t samples_remaining =')]
    clock_check = r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include "offline_audio_policy.h"
#include "sd_audio_retention_policy.h"
#define CONFIG_NUWAX_SD_SESSION_ENABLE 1
#define CONFIG_NUWAX_MEETING_OFFLINE_MAX_HOURS 8
#define CONFIG_NUWAX_SD_MEETING_MAX_HOURS 8
#define CONFIG_NUWAX_MONITORING_OFFLINE_MAX_HOURS 8
#define NUWAX_OFFLINE_AUDIO_MODE_MEETING 0
#define ESP_LOGI(...) ((void)0)
static atomic_bool s_card_online, s_offline_limit_reached;
static bool card;
static uint64_t now;
static bool nuwax_sd_session_active(void) { return card; }
static uint64_t esp_timer_get_time(void) { return now; }
static bool expired(bool with_card, unsigned mode, bool online, unsigned hours)
{
    card=with_card; s_card_online=online; s_offline_limit_reached=false;
    now=40000000000ULL+(uint64_t)hours*3600000000ULL;
    struct { unsigned mode; uint64_t limit_used_us; } cfg={mode,0};
    bool monitoring=mode==1;
    uint64_t card_meeting_started_us=40000000000ULL, offline_started_us=40000000000ULL;
    for (;;) {
''' + limit + r'''
        return false;
    }
    return s_offline_limit_reached;
}
int main(void)
{
    assert(!expired(true,1,false,9) && !expired(true,1,false,24*30));
    assert(!expired(true,1,true,24*30));
    assert(expired(true,0,false,9) && expired(true,0,true,9));
    assert(!expired(true,0,true,7) && expired(true,0,true,8));
    assert(expired(false,1,false,9) && expired(false,0,false,9));
    puts("PASS: production capture duration branch, simulated 9 hours/30 days; only card monitoring bypasses the offline limit");
}
'''
    (work / 'clock.c').write_text(clock_check, encoding='utf-8')
    clock_exe = work / 'clock.exe'
    subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I' + str(root / 'main'),
        str(work / 'clock.c'), '-o', str(clock_exe)], check=True)
    subprocess.run([str(clock_exe)], cwd=work, check=True)
