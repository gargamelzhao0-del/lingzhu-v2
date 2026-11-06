"""Exercise production filename admission and media-fault fencing without a device.

Real FatFs dropped-sector coverage is in test-sd-catalog-fatfs.py. This test
extracts the real scanner and replay classifier, rather than a matching model.
"""
from pathlib import Path
import ctypes
import hashlib
import json
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
out = root / 'build-host-checks/sd-integrity'
out.mkdir(parents=True, exist_ok=True)
if os.name == 'nt':
    ctypes.windll.kernel32.SetErrorMode(0x8003)
spool = (root / 'main/offline_audio_spool.c').read_text(encoding='utf-8-sig')
replay = (root / 'main/sd_audio_session_replay.inc').read_text(encoding='utf-8-sig')

def function(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'

fixture = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_FINISHED 0x10c
typedef int esp_err_t;
struct dirent {char d_name[128];};
typedef struct {bool error; unsigned at;} archive_dir_t;
static struct {atomic_bool degraded; uint32_t next_index; atomic_bool capture_complete;} s;
static const char *names[8];
static bool fail_open;
static int fail_at=-1;
static unsigned closes;
static archive_dir_t *archive_dir_open(archive_dir_t *d) {
    if(fail_open)return NULL;memset(d,0,sizeof(*d));return d;
}
static struct dirent *archive_dir_read(archive_dir_t *d) {
    static struct dirent e;
    if(fail_at>=0 && d->at==(unsigned)fail_at){d->error=true;return NULL;}
    if(d->at>=8U || !names[d->at])return NULL;
    snprintf(e.d_name,sizeof(e.d_name),"%s",names[d->at++]);return &e;
}
static void archive_dir_close(archive_dir_t *d){(void)d;++closes;}
'''
fixture += function(spool, 'static esp_err_t refresh_next_segment_index(')
fixture += r'''
#include "sd_audio_file_fault.h"
static sd_audio_file_fault_t s_card_file_fault[2];
typedef struct {char session_id[48];uint8_t mode;} sd_audio_session_t;
typedef struct {uint32_t index;char path[128];} sd_audio_segment_t;
static sd_audio_session_t s_card_session;
static atomic_uint s_media_generation=1U;
static atomic_bool s_media_ready=true,s_replay_media_fault,s_card_failed;
static bool healthy=true,active,stop_requested;
static unsigned missing_marks,wakes;
static bool nuwax_sd_card_is_mounted(void){return healthy;}
static bool nuwax_sd_session_active(void){return active;}
static void nuwax_sd_card_mark_io_fault(esp_err_t err){assert(err==ESP_FAIL);healthy=false;}
static void notify_writer_task(void){++wakes;}
static void nuwax_offline_audio_supervisor_wake(void){++wakes;}
static esp_err_t nuwax_offline_audio_request_stop(bool complete){assert(!complete);stop_requested=true;return ESP_OK;}
static esp_err_t sd_audio_catalog_mark_missing(const char *id,uint32_t index){assert(id[0] && index<100);++missing_marks;return ESP_OK;}
static void ignore_log(const char *tag,const char *fmt,...){(void)tag;(void)fmt;}
#define TAG "integrity"
#define ESP_LOGW(...) ignore_log(__VA_ARGS__)
'''
fixture += function(replay, 'static esp_err_t card_confirm_missing_locked(')
fixture += r'''
int main(void) {
    s.next_index=100U;names[0]="card_scene_00000004.ready";
    assert(refresh_next_segment_index()==ESP_OK && s.next_index==100U);
    names[0]="00000004/card_scene_00000201.part";
    assert(refresh_next_segment_index()==ESP_OK && s.next_index==202U);
    fail_open=true;assert(refresh_next_segment_index()==ESP_FAIL && s.next_index==202U && s.degraded);fail_open=false;
    names[0]="card_scene_00000999.ready";fail_at=1;
    assert(refresh_next_segment_index()==ESP_FAIL && s.next_index==202U);fail_at=-1;
    assert(refresh_next_segment_index()==ESP_OK && s.next_index==1000U);
    names[0]=NULL;assert(refresh_next_segment_index()==ESP_OK && s.next_index==1000U);
    names[0]="card_scene_4294967295.ready";
    assert(refresh_next_segment_index()==ESP_ERR_INVALID_SIZE && s.next_index==1000U);
    names[0]="card_scene_18446744073709551616999.part.bad";
    assert(refresh_next_segment_index()==ESP_ERR_INVALID_SIZE && s.next_index==1000U);
    names[0]="card_scene_-1.ready";names[1]="random_no_index.ready";
    assert(refresh_next_segment_index()==ESP_OK && s.next_index==1000U && closes==8U);
    puts("PASS production filename scan: monotonic after deletion/remount, EIO/open failure preserves high-water, overflow fails closed");
    for(unsigned mode=0;mode<2;mode++) {
        sd_audio_session_t scene={.mode=(uint8_t)mode};strcpy(scene.session_id,mode?"monitor_live":"meeting_live");
        sd_audio_segment_t file={.index=7U};healthy=true;s_media_ready=true;active=true;
        s_card_session=scene;s.capture_complete=true;s_card_failed=false;stop_requested=false;
        const unsigned before=missing_marks;
        assert(card_confirm_missing_locked(&scene,&file,false)==ESP_ERR_NOT_FINISHED && missing_marks==before && !healthy && !s_media_ready && s_replay_media_fault);
        healthy=true;
        assert(card_confirm_missing_locked(&scene,&file,false)==ESP_ERR_NOT_FINISHED && missing_marks==before); /* Same epoch is no proof. */
        healthy=true;atomic_fetch_add(&s_media_generation,1U);
        assert(card_confirm_missing_locked(&scene,&file,false)==ESP_OK && missing_marks==before+1U && s_card_failed && !s.capture_complete && stop_requested);
        healthy=true;file.index++;active=false;stop_requested=false;
        assert(card_confirm_missing_locked(&scene,&file,false)==ESP_ERR_NOT_FINISHED && !stop_requested);
        healthy=true;atomic_fetch_add(&s_media_generation,1U);
        assert(card_confirm_missing_locked(&scene,&file,false)==ESP_OK && !stop_requested);
    }
    sd_audio_file_fault_t fault={0};
    assert(!sd_audio_file_fault_confirm(&fault,"meeting_a",1U,UINT32_MAX));
    assert(sd_audio_file_fault_confirm(&fault,"meeting_a",1U,0U));
    assert(!sd_audio_file_fault_confirm(&fault,"meeting_b",1U,1U));
    assert(!sd_audio_file_fault_confirm(&fault,"meeting_b",2U,2U));
    assert(!sd_audio_file_fault_confirm(&fault,"meeting_b",2U,2U));
    assert(sd_audio_file_fault_confirm(&fault,"meeting_b",2U,3U));
    assert(wakes==12U);
    puts("PASS production file-fault classifier: both modes, same-file remount proof, independent lanes, epoch wrap and active/old-scene stop fences");
}
'''
source = out / 'integrity.c'
source.write_text(fixture, encoding='utf-8')
exe = out / 'integrity.exe'
subprocess.run(['gcc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
    '-Wno-misleading-indentation', '-I' + str(root/'main'), str(source), '-o', str(exe)], check=True)
subprocess.run([str(exe)], check=True, timeout=30)
(out/'sd_file_fault_production_host.inc').write_text(function(replay,'static esp_err_t card_confirm_missing_locked('),encoding='utf-8')
for negative in [True,False]:
    name='quarantine-negative' if negative else 'quarantine-production'
    binary=out/(name+'.exe')
    subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function',
        *(['-DNEGATIVE_KEEP_BAD_READY=1'] if negative else []),
        '-DSD_AUDIO_CATALOG_ROOT="catalog"','-DSD_AUDIO_ARCHIVE_ROOT="."','-DSD_AUDIO_ARCHIVE_NEW_ROOT="audio2"',
        '-DESP_ERR_INVALID_CRC=0x109','-DESP_ERR_NOT_FOUND=0x105',
        '-I'+str(out),'-I'+str(root/'main'),'-I'+str(root/'main/audio_modes/meeting/host_stubs'),
        str(root/'main/sd_audio_quarantine_host_check.c'),str(root/'main/audio_frame.c'),'-o',str(binary)],check=True)
    with tempfile.TemporaryDirectory(prefix='nuwax-'+name+'-') as directory:
        subprocess.run([str(binary)],cwd=directory,check=True,timeout=30)

baseline = Path('C:/tmp/nuwax-sd-flow-release-20261030-r210')
assert baseline.is_dir(), 'R210 source snapshot required for the speed/protocol preservation audit'
preserved = ['main/ws_transport.c','main/ws_transport.h','main/tls_archive_crypto.c',
    'main/sd_tcp_recovery.c','main/ws_archive_flow_policy.h','main/sd_capture_admission.h',
    'main/offline_replay_shared.inc','main/offline_replay_prefetch.inc','main/offline_replay_stream.inc',
    'main/sd_audio_protocol.h','main/sd_audio_scene_protocol.h','main/sd_audio_catalog.c',
    'main/sd_audio_catalog_io.inc','main/sd_audio_scene_replay.inc','main/sd_audio_session_storage.inc',
    'main/nuwax_ai_partner_main.c','main/audio_modes/meeting/meeting_mode.c',
    'main/audio_modes/monitoring/monitoring_mode.c']
for rel in preserved:
    if rel == 'main/nuwax_ai_partner_main.c':
        actual=(root/rel).read_text(encoding='utf-8-sig')
        notification='''    if (state == NUWAX_MEETING_STATE_ERROR) {
        /* ERROR may become DISABLED before the single state publisher runs.
         * Latch the terminal result separately; LVGL consumes this before
         * draining subsequent touch actions, with no transient ERROR frame. */
        nuwax_app_shell_notify_action_rejected(NUWAX_APP_SHELL_ACTION_SET_MEETING_MODE);
    }
'''
        assert actual.count(notification)==1
        assert actual.replace(notification,'')==(baseline/rel).read_text(encoding='utf-8-sig'),rel
    elif rel=='main/sd_audio_catalog.c':
        from sd_source_contracts import catalog_without_directory_guard
        assert catalog_without_directory_guard((root/rel).read_text(encoding='utf-8-sig'))==(baseline/rel).read_text(encoding='utf-8-sig'),rel
    elif rel=='main/offline_replay_shared.inc':
        from sd_source_contracts import shared_without_r216
        assert shared_without_r216((root/rel).read_text(encoding='utf-8-sig'))==(baseline/rel).read_text(encoding='utf-8-sig'),rel
    else:
        assert (root/rel).read_bytes() == (baseline/rel).read_bytes(), rel
previous = (baseline/'main/offline_audio_spool.c').read_text(encoding='utf-8-sig')
for signature in ['static esp_err_t replay_one_ready_file(', 'esp_err_t nuwax_sd_session_prepare_start(',
                  'static esp_err_t append_locked(']:
    actual, old = function(spool,signature), function(previous,signature)
    if signature == 'static esp_err_t append_locked(':
        from sd_source_contracts import append_without_r216
        actual=append_without_r216(actual)
        # R212 deliberately adds independently tested optional diagnostics.
        # With that feature disabled every original C token must remain.
        def preprocess(text):
            return subprocess.check_output(['gcc','-E','-P','-x','c',
                '-DCONFIG_NUWAX_SD_INTEGRITY_DIAGNOSTICS=0',
                '-DCONFIG_NUWAX_SD_SESSION_ENABLE=1','-'],input=text,text=True)
        import re
        assert re.sub(r'\s+','',preprocess(actual)) == re.sub(r'\s+','',preprocess(old)),signature
    else:
        if signature == 'static esp_err_t replay_one_ready_file(':
            from sd_source_contracts import file_sender_without_r216
            actual=file_sender_without_r216(actual)
        if signature == 'esp_err_t nuwax_sd_session_prepare_start(':
            changed='''    /* Publish intent before trying the storage gate. A background evidence
     * exporter holding that gate can then observe the request and yield. */
    sd_capture_admission_hold(&s_capture_admission, (unsigned)mode, (uint32_t)(until / 1000LL));
    if (xSemaphoreTake(s.admission_lock, 0) != pdTRUE) return ESP_ERR_NOT_FINISHED;
'''
            original='''    if (xSemaphoreTake(s.admission_lock, 0) != pdTRUE) return ESP_ERR_NOT_FINISHED;
    sd_capture_admission_hold(&s_capture_admission, (unsigned)mode, (uint32_t)(until / 1000LL));
'''
            assert actual.count(changed)==1
            actual=actual.replace(changed,original)
        assert actual == old, signature
report = dict(productionScanner=True, productionClassifier=True, realCatalogResurrectionNegativeControl=True,
              productionQuarantineCrashBoundary=True, speedAndProtocolPreserved=preserved,
              hardwareValidated=False, hardwareThroughputKiBs=None)
(out/'results.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
print('PASS R210 exact speed, capture/control, wire-format, CRC and durable ACK preservation fences')
