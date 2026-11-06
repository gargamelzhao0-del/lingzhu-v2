"""Read current functions into deterministic host probes; no product edits/device I/O."""
from pathlib import Path
import json
import subprocess
import sys
import tempfile

root=Path(__file__).resolve().parents[1]
ev=root/'build-host-checks/sd-close-lifecycle'
ev.mkdir(parents=True,exist_ok=True)
sys.path.insert(0,str(root/'tools'))
from c_source_extract import function
spool=(root/'main/offline_audio_spool.c').read_text(encoding='utf-8-sig')
voice=(root/'main/offline_audio_voice.c').read_text(encoding='utf-8-sig')
storage=(root/'main/sd_audio_session_storage.inc').read_text(encoding='utf-8-sig')
health_start=spool.index('        const bool media_ready =\n')
health_end=spool.index('        if (health == NUWAX_OFFLINE_CAPTURE_UNAVAILABLE)',health_start)

close_code=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "offline_audio_voice.h"
#define CONFIG_NUWAX_SD_SESSION_ENABLE 1
#define MIRROR_CLOSE_TIMEOUT_MS 20000U
#define MIRROR_STAGING_FRAMES 768U
#define MALLOC_CAP_SPIRAM 1U
#define MALLOC_CAP_8BIT 2U
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define portMUX_TYPE unsigned
#define portMUX_INITIALIZER_UNLOCKED 0U
static void enter_guard(const void *lock);
static void exit_guard(const void *lock);
#define portENTER_CRITICAL(x) enter_guard(x)
#define portEXIT_CRITICAL(x) exit_guard(x)
static struct { atomic_bool capture_complete,capture_stop,active,archive_ready,writer_error; } s;
static struct { char session_id[48]; } s_card_session;
static atomic_bool s_card_failed,s_media_ready,s_card_partial,s_card_active;
static unsigned s_card_network_lock;
static char s_card_mirror_session[48];
static atomic_uint s_mirror_closers,s_mirror_staging_rings,s_capture_cancel_epoch;
static atomic_uint s_capture_recovery_until;
static atomic_int s_capture_health;
static uint32_t clock_ms;
static unsigned drained,lost,deletes;
static void(*pending_task)(void*);static void *pending_arg;
static bool current_active;
static unsigned guard_depth;
static bool publish_on_unlock;
static uint32_t new_epoch;
static void (*after_delay)(void);
static void enter_guard(const void *lock) { (void)lock;++guard_depth; }
static void exit_guard(const void *lock) {
    assert(guard_depth);--guard_depth;
    if(!guard_depth&&lock==&s_card_network_lock&&publish_on_unlock){
        publish_on_unlock=false;
        strcpy(s_card_mirror_session,"monitor_b");
        s_card_failed=s_card_partial=false;s.capture_complete=true;s.capture_stop=false;
        new_epoch=s_capture_cancel_epoch;
    }
}
bool nuwax_sd_session_active(void) { return current_active; }
static bool enabled(void) { return true; }
static bool runtime_initialized(void) { return true; }
static uint32_t offline_now_ms(void) { return clock_ms; }
static bool offline_deadline_elapsed(uint32_t now,uint32_t until) { return (int32_t)(now-until)>=0; }
static bool recording_space_blocked(void) { return false; }
static void *heap_caps_malloc(size_t n,unsigned caps) { (void)caps;return malloc(n); }
static void heap_caps_free(void *p) { free(p); }
static void vTaskDelay(unsigned ms) { assert(!guard_depth);clock_ms+=ms;if(after_delay)after_delay(); }
static void vTaskDeleteWithCaps(void *p) { (void)p;++deletes; }
void nuwax_offline_audio_supervisor_wake(void) {}
esp_err_t nuwax_offline_audio_request_end(bool complete) { (void)complete;return ESP_OK; }
static int xTaskCreatePinnedToCoreWithCaps(void(*task)(void*),const char *name,unsigned stack,
    void *arg,unsigned priority,void *handle,unsigned core,unsigned caps) {
    (void)name;(void)stack;(void)priority;(void)handle;(void)core;(void)caps;
    assert(!pending_task);pending_task=task;pending_arg=arg;return pdPASS;
}
/* Queue is full for the zero-wait close call. The writer is immediately
 * available when the deferred worker runs; this isolates the deadline. */
static void mirror_drain(nuwax_offline_audio_mirror_t *m,bool closing) {
    if(closing){drained+=m->staging_count;m->accepted_frames+=m->staging_count;m->staging_count=0U;}
}
''' + function(storage,'card_note_mirror_result') + '\n' + function(spool,'mirror_note_loss') + '\n' + function(spool,'mirror_finish') + r'''
#include "offline_mirror_close_queue.inc"
static mirror_close_node_t delayed_predecessor;
static uint32_t predecessor_until;
static void release_predecessor(void) {
    if(!offline_deadline_elapsed(clock_ms,predecessor_until))return;
    mirror_close_remove(&delayed_predecessor);after_delay=NULL;
}
''' + function(spool,'mirror_close_task') + '\n' + function(spool,'nuwax_offline_audio_mirror_request_close') + '\n' + function(voice,'nuwax_offline_voice_deinit') + '\n' + function(voice,'nuwax_offline_voice_reset') + '\n' + function(spool,'offline_capture_cancelled') + '\n' + function(spool,'nuwax_offline_audio_request_stop') + r'''
static nuwax_offline_capture_health_t observe_quiet(void) {
    const bool monitoring=true;
    struct { bool recording; } voice_archive={0};
    nuwax_offline_audio_mirror_t empty={0};const nuwax_offline_audio_mirror_t *sink=&empty;
    const uint32_t rejected_before=0U;
    nuwax_offline_capture_watch_t storage_watch={0};
    struct { int64_t start_deadline_us; } cfg={0};
''' + spool[health_start:health_end] + r'''
    (void)cfg;return health;
}
int main(int argc,char **argv) {
    assert(argc==2);clock_ms=!strcmp(argv[1],"wrap")?UINT32_MAX-10000U:120000U;
    current_active=s_card_active=true;s.capture_complete=s_media_ready=s.archive_ready=true;
    strcpy(s_card_session.session_id,"monitor_a");strcpy(s_card_mirror_session,"monitor_a");
    nuwax_offline_voice_t v={0};v.recording=v.policy.start_recording=true;
    nuwax_offline_audio_mirror_t *m=&v.mirror;
    m->started=m->attached=m->complete=true;m->staging=malloc(1U);m->staging_count=16U;
    strcpy(m->session_id,"monitor_a");s_mirror_staging_rings=1U;
    if(!strcmp(argv[1],"owner_race")) {
        publish_on_unlock=true;mirror_finish(m);
        assert(!publish_on_unlock&&!s_card_failed&&s.capture_complete&&!offline_capture_cancelled(new_epoch));
        assert(!strcmp(s_card_mirror_session,"monitor_b")&&!guard_depth);
        puts("PASS old-tail stop and next-owner publication are serialized; late repeated result cannot cancel the new epoch");return 0;
    }
    if(!strcmp(argv[1],"fatal_tail")) {
        mirror_finish(m);
        assert(s_card_failed&&!s.capture_complete&&offline_capture_cancelled(0U));
        assert(observe_quiet()==NUWAX_OFFLINE_CAPTURE_RECORDING);
        assert(nuwax_offline_audio_request_stop(false)==ESP_OK&&offline_capture_cancelled(0U));
        puts("PASS same-scene fatal tail requests stop before a new MIC is published");return 0;
    }
    if(!strcmp(argv[1],"foreign_tail")) {
        strcpy(m->session_id,"previous_scene");mirror_finish(m);
        assert(!s_card_failed&&s.capture_complete&&!offline_capture_cancelled(0U));
        puts("CONTROL foreign tail does not fail or stop a different current scene");return 0;
    }
    const bool queued=!strcmp(argv[1],"queued_budget");
    if(queued){mirror_close_enqueue(&delayed_predecessor);predecessor_until=clock_ms+21000U;after_delay=release_predecessor;}
    if(!strcmp(argv[1],"deinit"))nuwax_offline_voice_deinit(&v,true);
    else if(!strcmp(argv[1],"reset"))nuwax_offline_voice_reset(&v);
    else assert(nuwax_offline_audio_mirror_request_close(m,true,
        !strcmp(argv[1],"stale_timestamp")?clock_ms-25000U:clock_ms)==ESP_OK);
    assert(pending_task&&s_mirror_closers==(queued?2U:1U));
    const uint32_t deadline=((mirror_close_node_t*)pending_arg)->deadline_ms;
    clock_ms+=!strcmp(argv[1],"real_expiry")?20001U:1U;
    void(*task)(void*)=pending_task;pending_task=NULL;task(pending_arg);
    lost=16U-drained;
    assert(!s_mirror_closers&&!s_mirror_staging_rings&&deletes==1U);
    if(strcmp(argv[1],"real_expiry")&&!queued) {
        assert(drained==16U&&!s_card_failed);
        if(!strcmp(argv[1],"reset"))assert(s_card_partial&&!s.capture_complete&&!offline_capture_cancelled(0U));
        else assert(s.capture_complete&&!s_card_partial);
    } else assert(!drained&&lost==16U&&s_card_failed&&!s.capture_complete&&offline_capture_cancelled(0U));
    printf("%s deadline=%u execution_ms=%u drained=%u lost=%u audio_lost_ms=%u failed=%u\n",
        argv[1],deadline,clock_ms,drained,lost,lost*20U,(unsigned)s_card_failed);
    return 0;
}
'''

def compile_run(name,code,cases):
    src=ev/(name+'.c');src.write_text(code,encoding='utf-8');exe=src.with_suffix('.exe')
    command=['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function',
        '-DSD_AUDIO_CATALOG_ROOT="catalog"','-DSD_AUDIO_ARCHIVE_ROOT="audio"','-DSD_AUDIO_ARCHIVE_NEW_ROOT="audio2"',
        '-DESP_ERR_INVALID_CRC=0x109','-DESP_ERR_NOT_FOUND=0x105',
        '-I'+str(root/'main/audio_modes/meeting/host_stubs'),'-I'+str(root/'main'),str(src),'-o',str(exe)]
    subprocess.run(command,check=True)
    for case in cases:
        with tempfile.TemporaryDirectory(prefix='sd-independent-audit-') as directory:
            run=subprocess.run([str(exe),case],cwd=directory,text=True,capture_output=True,timeout=30)
        text=run.stdout+run.stderr
        (ev/(name+'-'+case+'.log')).write_text(text,encoding='utf-8')
        print(text,flush=True)
        results.append(dict(group=name,case=case,exitCode=run.returncode,log=str(ev/(name+'-'+case+'.log'))))
        assert run.returncode==0,(name,case)
results=[]
compile_run('close',close_code,['deinit','reset','stale_timestamp','current','real_expiry','fatal_tail','foreign_tail','owner_race','queued_budget','wrap'])
print('PASS actual voice deinit/reset + storage-owner close clock, partial/fatal stop and old-scene isolation')
