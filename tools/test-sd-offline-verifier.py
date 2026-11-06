"""Run the production offline worker against real catalog/audio files.

RTOS scheduling, allocation, and the raw read adapter are explicit doubles;
the worker, catalog, NA builder, and streaming validator are real code.
"""
from pathlib import Path
import subprocess
import tempfile
from c_source_extract import function

root = Path(__file__).resolve().parents[1]
out = root / 'build-host-checks/sd-offline-verifier'
out.mkdir(parents=True, exist_ok=True)
spool = (root / 'main/offline_audio_spool.c').read_text(encoding='utf-8-sig')
types = ''
for name in ('archive_header_t', 'archive_footer_t'):
    end = spool.index('} ' + name + ';') + len('} ' + name + ';')
    types += spool[spool.rindex('typedef struct', 0, end):end] + '\n'
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <direct.h>
#include "audio_frame.h"
#include "sd_audio_file_fault.h"
#include "sd_capture_admission.h"
#include "sd_audio_catalog.c"
''' + types + '\n' + function(spool, 'crc32_update') + r'''
#define HEADER_BYTES 96U
#define FOOTER_BYTES 32U
#define MAX_FRAME_BYTES 2048U
''' + (root / 'main/offline_replay_stream.inc').read_text() + r'''
#define CONFIG_NUWAX_SD_SESSION_ENABLE 1
#define ESP_PLATFORM 1
#define TAG "worker-host"
#define MALLOC_CAP_SPIRAM 1U
#define MALLOC_CAP_8BIT 2U
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define REPLAY_ACK_BIT 1U
#define REPLAY_CHUNK_ACK_BIT 2U
static atomic_bool s_offline_verify_running, s_offline_verify_cancel;
static atomic_bool s_card_online, s_shutdown_requested, s_card_finishing, s_media_ready;
static atomic_bool s_replay_media_fault;
static atomic_bool s_formal_replay_cancel_requested;
static atomic_uint s_media_generation;
static sd_capture_admission_t s_capture_admission;
static struct { atomic_bool pending; } s_card_io_recovery;
static struct { int lock, queue, admission_lock, writer_gate, events; FILE *file;
    atomic_bool writer_busy; uint32_t capture_generation; } s;
static sd_audio_session_t s_card_session;
static sd_audio_damage_window_t s_card_damage;
static uint64_t s_offline_verify_nonce=99U;
static uint32_t clock_ms=10000U;
static unsigned allocations, reads, deletes, failures, damage, delays, event;
static bool held, fail_alloc, fail_task;
static unsigned held_mask;
static bool ota_active, capture_active;
static uint8_t owner_byte=0x31;
typedef unsigned nuwax_offline_audio_mode_t;
static unsigned launch_race, creates, created_after_admission;
static esp_err_t admission_result;
static void (*pending_task)(void *);
static void *pending_argument;
esp_err_t nuwax_offline_audio_prepare_shutdown(void);
esp_err_t nuwax_offline_audio_replay_prepare_ota(uint32_t timeout_ms);
esp_err_t nuwax_sd_session_prepare_start(nuwax_offline_audio_mode_t mode, int64_t deadline_us);
static uint32_t offline_now_ms(void) { return clock_ms; }
static int64_t esp_timer_get_time(void) { return (int64_t)clock_ms*1000; }
static bool offline_deadline_elapsed(uint32_t now,uint32_t until) { return (int32_t)(now-until)>=0; }
static void *heap_caps_malloc(size_t n,unsigned caps) { (void)caps;++allocations;return fail_alloc?NULL:malloc(n); }
static void heap_caps_free(void *p) { free(p); }
static bool nuwax_power_ctrl_shutdown_preparing(void) { return false; }
static bool nuwax_offline_audio_replay_running(void) { return false; }
static bool nuwax_sd_session_active(void) { return capture_active; }
static bool replay_ota_active(void) { return ota_active; }
static bool enabled(void) { return true; }
static bool runtime_initialized(void) { return true; }
static bool nuwax_offline_audio_capture_running(void) { return false; }
static bool nuwax_offline_audio_storage_busy(void) { return false; }
static int xEventGroupSetBits(int group,unsigned bits) { (void)group;return (int)bits; }
static unsigned uxQueueMessagesWaiting(int q) { (void)q;return 0U; }
static int xSemaphoreTake(int lock,unsigned ticks) {
    if(held_mask&(1U<<lock)){clock_ms+=ticks;return 0;}
    held_mask|=1U<<lock;held=true;return pdTRUE;
}
static void xSemaphoreGive(int lock) { assert(held_mask&(1U<<lock));held_mask&=~(1U<<lock);held=held_mask!=0U; }
static void vTaskDelay(unsigned ticks) {
    assert(!held);clock_ms+=ticks; ++delays;
    if(delays==1U) {
        if(event==1U)s_offline_verify_cancel=true;
        if(event==2U)atomic_fetch_add(&s_media_generation,1U);
        if(event==3U)s_card_online=true;
        if(event==4U)s_card_finishing=true;
        if(event==5U)s_shutdown_requested=true;
        if(event==9U)ota_active=true;
        if(event==10U)s_offline_verify_running=false;
    }
}
static bool card_owner(uint8_t owner[32]) { memset(owner,owner_byte,32U);return true; }
static void nuwax_offline_audio_supervisor_wake(void) {}
static uint32_t esp_random(void) { return 17U; }
static void vTaskDeleteWithCaps(void *t) { (void)t;++deletes; }
static int xTaskCreateWithCaps(void(*task)(void*),const char *name,unsigned stack,void *arg,unsigned priority,void *handle,unsigned caps) {
    (void)name;(void)handle;(void)caps;assert(stack==8192U&&priority==1U);
    if(fail_task) return 0;
    ++creates;if(admission_result==ESP_OK)++created_after_admission;
    assert(!pending_task);pending_task=task;pending_argument=arg;return pdPASS;
}
static void nuwax_sd_card_mark_io_fault(esp_err_t err) { assert(err!=ESP_OK);++failures; }
static esp_err_t card_confirm_missing_locked(const sd_audio_session_t *scene,const sd_audio_segment_t *part,bool damaged) {
    assert(held&&scene->session_id[0]&&part->sealed&&damaged);++damage;return ESP_ERR_NOT_FINISHED;
}
typedef struct { unsigned unused; } replay_read_stats_t;
static esp_err_t replay_read_rewind(FILE *file) { return fseek(file,0,SEEK_SET)==0?ESP_OK:ESP_FAIL; }
static esp_err_t replay_read_validate(FILE *file,uint8_t *buffer,size_t *n,replay_stream_t *stream,
    uint32_t total,const archive_header_t *header,const archive_footer_t *footer,
    uint64_t *read_us,uint64_t *validation_us,uint64_t *work_us,replay_read_stats_t *stats) {
    (void)read_us;(void)validation_us;(void)work_us;(void)stats;assert(held&&*n<=4096U);++reads;
    if(event==6U)return ESP_ERR_NO_MEM;
    if(event==7U)return ESP_ERR_INVALID_CRC;
    return fread(buffer,1,*n,file)==*n?replay_stream_feed(stream,buffer,*n,total,header,footer):ESP_FAIL;
}
/* Interrupt the creator after its eligibility snapshot but before publication.
 * These are the real admission functions, not an assumed barrier result. */
static uint32_t launch_race_revision(void) {
    const uint32_t revision=sd_audio_catalog_live_revision();
    const unsigned race=launch_race;launch_race=0U;
    if(race==1U)admission_result=nuwax_offline_audio_prepare_shutdown();
    if(race==2U){ota_active=true;admission_result=nuwax_offline_audio_replay_prepare_ota(0U);}
    if(race==3U)admission_result=nuwax_sd_session_prepare_start(0U,esp_timer_get_time()+5000000);
    return revision;
}
#define sd_audio_catalog_live_revision launch_race_revision
#include "sd_audio_offline_verify.inc"
#undef sd_audio_catalog_live_revision
static bool nuwax_offline_audio_replay_blocks_ota(void) { return false; }
''' + function(spool, 'nuwax_offline_audio_prepare_shutdown') + '\n' + function(spool, 'nuwax_offline_audio_replay_prepare_ota') + '\n' + function(spool, 'nuwax_sd_session_prepare_start') + r'''

static sd_audio_segment_t archive_fixture(const char *id,uint8_t owner) {
    s_card_session=(sd_audio_session_t){.segment_count=1U,.complete=1U,.start_ms=1000U,.boot_id=7U};
    strcpy(s_card_session.session_id,id);memset(s_card_session.owner,owner,32);
    assert(sd_audio_catalog_create(&s_card_session)==ESP_OK);
    sd_audio_segment_t part={.sealed=1U,.start_ms=1000U,.end_ms=1640U,.total_bytes=128U+32U*666U};
    snprintf(part.path,sizeof(part.path),"./card_%s_1.ready",id);
    archive_header_t h={0};memcpy(h.magic,"NWXOFF1",7);h.version=1;h.header_bytes=96;
    h.codec=NUWAX_AUDIO_CODEC_PCM16;h.sample_rate=16000;h.channels=1;h.frame_duration_ms=20;
    strcpy(h.session_id,s_card_session.session_id);
    archive_footer_t footer={0};memcpy(footer.magic,"NWXEND1",7);footer.version=1;footer.footer_bytes=32;
    footer.complete=1;footer.frame_count=32;footer.payload_bytes=32U*662U;
    FILE *file=fopen(part.path,"wb");assert(file&&fwrite(&h,1,sizeof(h),file)==sizeof(h));
    for(unsigned sequence=0;sequence<32U;++sequence) {
        int16_t pcm[320];for(unsigned k=0;k<320U;++k)pcm[k]=(int16_t)(sequence*3U+k);
        uint8_t frame[662];size_t n;uint32_t size=662U;
        assert(nuwax_audio_frame_build_pcm16(frame,sizeof(frame),pcm,320U,sequence,1000U+sequence*20U,0U,16000U,1U,20U,&n)==ESP_OK);
        footer.crc32=crc32_update(footer.crc32,frame,n);
        assert(fwrite(&size,1,4,file)==4&&fwrite(frame,1,n,file)==n);
    }
    assert(fwrite(&footer,1,sizeof(footer),file)==sizeof(footer)&&!fclose(file));part.payload_crc=footer.crc32;
    assert(sd_audio_catalog_write(s_card_session.session_id,&part)==ESP_OK);return part;
}
static sd_audio_segment_t fixture(const char *directory) {
    assert(_mkdir(directory)==0&&_chdir(directory)==0&&sd_audio_catalog_init()==ESP_OK);
    return archive_fixture("worker_audio",0x31);
}
static void reset(void) {
    s_media_ready=true;s_offline_verify_cancel=s_card_online=s_card_finishing=s_shutdown_requested=false;
    s_card_io_recovery.pending=false;s_media_generation=3U;s.writer_busy=false;s.capture_generation=1U;
    held=fail_alloc=fail_task=ota_active=false;held_mask=0U;capture_active=true;
    s.lock=1;s.admission_lock=2;s.writer_gate=3;
    s.file=NULL;s_offline_verify_running=false;
    s_capture_admission=(sd_capture_admission_t){0};
    launch_race=creates=created_after_admission=0U;admission_result=ESP_FAIL;owner_byte=0x31;
    pending_task=NULL;pending_argument=NULL;
    s_formal_replay_cancel_requested=true;
    allocations=reads=deletes=failures=damage=delays=event=0U;
    s_verify_retry_ms=s_verify_seen_revision=0U;clock_ms=10000U;
}
int main(void) {
    for(unsigned scenario=0U;scenario<10U;++scenario) {
        reset();char name[40];snprintf(name,sizeof(name),"case_%u",scenario);sd_audio_segment_t original=fixture(name),actual;
        event=scenario;fail_alloc=scenario==8U;s_offline_verify_running=true;
        card_offline_verify_task(NULL);
        assert(!held&&!s_offline_verify_running&&!sd_audio_catalog_replay_pinned()&&deletes==1U);
        assert(sd_audio_catalog_read(s_card_session.session_id,0U,&actual)==ESP_OK&&!actual.acknowledged&&!actual.missing);
        assert(actual.payload_crc==original.payload_crc&&actual.total_bytes==original.total_bytes&&!failures);
        const uint32_t next=catalog_verify_position(&s_card_session,99U,3U);
        if(scenario==0U)assert(next==1U&&reads>1U&&!damage);
        else assert(next==0U&&damage==(scenario==7U));
        if(scenario==8U)assert(!reads&&!damage);
        sd_audio_catalog_verify_reset();assert(_chdir("..")==0);
    }
    reset();fixture("unreadable_inventory");
    char manifest[128];assert(catalog_path(manifest,s_card_session.session_id,"session"));
    FILE *bad=fopen(manifest,"r+b");assert(bad);
    uint8_t torn[2*sizeof(sd_audio_session_t)];memset(torn,0xa5,sizeof(torn));
    assert(fwrite(torn,1,sizeof(torn),bad)==sizeof(torn)&&!fclose(bad));
    s_offline_verify_running=true;card_offline_verify_task(NULL);
    assert(!s_offline_verify_running&&!held&&!reads&&!s_verify_seen_revision);
    assert(!damage&&!failures&&!sd_audio_catalog_replay_pinned());
    assert(_chdir("..")==0);
    reset();fixture("task_oom");fail_task=true;card_offline_verify_service();
    assert(!s_offline_verify_running&&!held&&!sd_audio_catalog_replay_pinned()&&!reads);
    reset();capture_active=false;s_offline_verify_running=true;
    assert(nuwax_offline_audio_prepare_shutdown()==ESP_ERR_NOT_FINISHED&&!held&&s_shutdown_requested);
    s_offline_verify_running=false;
    reset();capture_active=false;fixture("binding_change");
    (void)archive_fixture("binding_b",0x32);
    s_offline_verify_running=true;card_offline_verify_task(NULL);
    assert(s_verify_seen_revision==sd_audio_catalog_live_revision());
    sd_audio_session_t other;assert(sd_audio_catalog_load("binding_b",&other)==ESP_OK);
    assert(catalog_verify_position(&other,99U,3U)==0U);
    owner_byte=0x32;clock_ms=s_verify_retry_ms;
    card_offline_verify_service();
    assert(creates==1U&&pending_task);
    void(*task)(void*)=pending_task;pending_task=NULL;task(pending_argument);
    assert(!s_offline_verify_running&&!held&&!sd_audio_catalog_replay_pinned());
    assert(catalog_verify_position(&other,99U,3U)==1U);
    sd_audio_segment_t preserved;assert(sd_audio_catalog_read("binding_b",0U,&preserved)==ESP_OK&&!preserved.acknowledged&&!preserved.missing);
    assert(_chdir("..")==0);
    for(unsigned race=1U;race<=3U;++race) {
        reset();capture_active=false;launch_race=race;
        card_offline_verify_service();
        if(created_after_admission)fprintf(stderr,"creator admitted after completed barrier: race=%u tasks=%u\n",race,creates);
        assert(!held&&!created_after_admission);
        if(pending_task){void(*task)(void*)=pending_task;pending_task=NULL;task(pending_argument);}
        assert(!s_offline_verify_running&&!held&&!sd_audio_catalog_replay_pinned());
        if(race==1U)assert(nuwax_offline_audio_prepare_shutdown()==ESP_OK);
        if(race==2U)assert(nuwax_offline_audio_replay_prepare_ota(0U)==ESP_OK);
        if(race==3U)assert(nuwax_sd_session_prepare_start(0U,esp_timer_get_time()+5000000)==ESP_OK);
    }
    assert(nuwax_offline_audio_prepare_shutdown()==ESP_OK&&!held);
    reset();ota_active=true;s_offline_verify_running=true;event=10U;
    assert(nuwax_offline_audio_replay_prepare_ota(50U)==ESP_OK&&s_offline_verify_cancel&&!s_offline_verify_running&&delays==1U);
    reset();ota_active=true;s_offline_verify_running=true;event=11U;
    assert(nuwax_offline_audio_replay_prepare_ota(50U)==ESP_ERR_TIMEOUT&&s_offline_verify_running&&delays==3U);
    s_offline_verify_running=false;
    puts("PASS actual offline worker: original NA/CRC files, bounded slices and durable optional cursor; no ACK/deletion");
    puts("PASS actual offline worker: cancel, stale media, online, stop, shutdown, read OOM, CRC suspicion and task/buffer OOM release own FILE/lease without false completeness");
    puts("PASS actual shutdown/OTA barriers: verifier must release before power-off or firmware write; timeout does not force-clear a live worker; formal cancel does not suppress offline validation");
    puts("PASS actual creator/admission interleavings: no verification task can be launched after shutdown, OTA or recording-start barrier has completed");
    puts("PASS actual inventory failure: malformed real manifest does not latch seen_revision or invent media failure, ACK or missing");
    puts("PASS actual binding change: unchanged card/revision cannot reuse another owner's exhausted inventory; newly eligible old audio is verified without ACK/deletion");
    return 0;
}
'''
source=out/'worker.c';source.write_text(code,encoding='utf-8')
exe=out/'worker.exe'
subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function',
    '-DSD_AUDIO_CATALOG_ROOT="catalog"','-DESP_ERR_INVALID_CRC=0x109','-DESP_ERR_NOT_FOUND=0x105',
    '-I'+str(root/'main/audio_modes/meeting/host_stubs'),'-I'+str(root/'main'),
    str(source),str(root/'main/audio_frame.c'),'-o',str(exe)],check=True)
with tempfile.TemporaryDirectory(prefix='sd-offline-worker-') as directory:
    subprocess.run([str(exe)],cwd=directory,check=True,timeout=30)
