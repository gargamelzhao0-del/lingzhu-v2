"""Current reliability policy and catalog ownership checks; no device access."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
out = root / 'build-host-checks/sd-reliability'
out.mkdir(parents=True, exist_ok=True)
for name in ('sd_audio_reliability_policy_host_check', 'sd_audio_lease_host_check', 'sd_card_deadline_host_check'):
    exe = out / (name + '.exe')
    subprocess.run(['gcc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
        '-DSD_AUDIO_CATALOG_ROOT="catalog"', '-DESP_ERR_INVALID_CRC=0x109', '-DESP_ERR_NOT_FOUND=0x105',
        '-I' + str(root / 'main/audio_modes/meeting/host_stubs'), '-I' + str(root / 'main'),
        str(root / 'main' / (name + '.c')), '-o', str(exe)], check=True)
    with tempfile.TemporaryDirectory(prefix='sd-reliability-') as directory:
        subprocess.run([str(exe)], cwd=directory, check=True, timeout=30)

# Compile the actual capture-loop health/publication block with explicit MIC,
# queue and clock doubles. Quiet monitoring must not create a storage gap,
# while resumed speech without staging must still stop truthfully.
spool = (root / 'main/offline_audio_spool.c').read_text(encoding='utf-8-sig')
start = spool.index('        const bool media_ready =\n')
end = spool.index('        if (nuwax_offline_capture_health_is_recording(health))', start)
capture = spool[start:end]
source = r'''
#include <assert.h>
#include <stdio.h>
#include <stdatomic.h>
#include "offline_capture_health.h"
#define CONFIG_NUWAX_SD_SESSION_ENABLE 1
#define MIRROR_STAGING_FRAMES 768U
typedef struct { bool started,attached; unsigned staging_count,rejected_frames; } sink_t;
static struct { atomic_bool active,archive_ready,writer_error; } s;
static atomic_bool s_media_ready;
static atomic_uint s_capture_recovery_until;
static atomic_int s_capture_health;
static uint32_t clock_ms;
static uint32_t offline_now_ms(void) { return clock_ms; }
static bool recording_space_blocked(void) { return false; }
static bool nuwax_sd_session_active(void) { return true; }
static nuwax_offline_capture_health_t observe(bool monitoring,bool quiet,sink_t *sink,
    nuwax_offline_capture_watch_t *watch) {
    struct { bool recording; } voice_archive = {.recording=!quiet};
    unsigned rejected_before=0U;
    nuwax_offline_capture_watch_t storage_watch=*watch;
''' + capture + r'''
    *watch=storage_watch;
    return health;
}
int main(void) {
    sink_t sink={.started=true,.attached=true};
    nuwax_offline_capture_watch_t watch={0};
    s_media_ready=s.archive_ready=true;s.active=true;clock_ms=100U;
    assert(observe(false,false,&sink,&watch)==NUWAX_OFFLINE_CAPTURE_RECORDING);
    s_media_ready=false;s.writer_error=true;sink.staging_count=100U;
    assert(observe(false,false,&sink,&watch)==NUWAX_OFFLINE_CAPTURE_BUFFERING);
    clock_ms=14100U;
    assert(observe(false,false,&sink,&watch)==NUWAX_OFFLINE_CAPTURE_BUFFERING);
    assert((uint32_t)s_capture_recovery_until==15100U);
    clock_ms=15100U;
    assert(observe(false,false,&sink,&watch)==NUWAX_OFFLINE_CAPTURE_UNAVAILABLE);
    sink=(sink_t){0};clock_ms=17000U;s.archive_ready=false;
    assert(observe(true,true,&sink,&watch)==NUWAX_OFFLINE_CAPTURE_RECORDING&&!watch.buffering);
    assert((uint32_t)s_capture_recovery_until==27000U);
    assert(observe(true,false,&sink,&watch)==NUWAX_OFFLINE_CAPTURE_UNAVAILABLE);
    assert((uint32_t)s_capture_recovery_until==clock_ms);
    watch=(nuwax_offline_capture_watch_t){0};sink=(sink_t){.started=true,.attached=true};
    s_media_ready=true;s.writer_error=false;
    assert(observe(false,false,&sink,&watch)==NUWAX_OFFLINE_CAPTURE_STARTING);
    s.archive_ready=true;sink.rejected_frames=1U;
    assert(observe(false,false,&sink,&watch)==NUWAX_OFFLINE_CAPTURE_UNAVAILABLE);
    puts("PASS actual capture health: original grace bounds recovery; silent monitoring keeps listening; speech without staging/lost frames stop; no enable ACK before durable first frame");
    return 0;
}
'''
file=out/'capture-health.c';file.write_text(source,encoding='utf-8')
exe=out/'capture-health.exe'
subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror',
    '-I'+str(root/'main'),str(file),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True,timeout=30)

replay=(root/'main/sd_audio_session_replay.inc').read_text(encoding='utf-8')
(out/'sd_file_fault_production_host.inc').write_text(
    replay[replay.index('static esp_err_t card_confirm_missing_locked('):replay.index('/* Signature retained')],
    encoding='utf-8')
exe=out/'quarantine.exe'
subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function',
    '-DSD_AUDIO_CATALOG_ROOT="catalog"','-DSD_AUDIO_ARCHIVE_ROOT="."','-DSD_AUDIO_ARCHIVE_NEW_ROOT="unused_archives"',
    '-DESP_ERR_INVALID_CRC=0x109','-DESP_ERR_NOT_FOUND=0x105',
    '-I'+str(out),'-I'+str(root/'main/audio_modes/meeting/host_stubs'),'-I'+str(root/'main'),
    str(root/'main/sd_audio_quarantine_host_check.c'),str(root/'main/audio_frame.c'),'-o',str(exe)],check=True)
with tempfile.TemporaryDirectory(prefix='sd-quarantine-') as directory:
    subprocess.run([str(exe)],cwd=directory,check=True,timeout=30)
