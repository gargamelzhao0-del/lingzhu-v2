"""Long-run storage integrity, recoverable backpressure and bounded replay launch.

Executes the actual writer against files; injected corrupt reads must be
detected before a whole-file upload. No serial or device operation.
"""
from pathlib import Path
import ctypes,os,subprocess,tempfile,re,json,argparse
root=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--long',action='store_true',help='Exercise eight hours of 20 ms PCM frames (about 1 GiB on host disk)')
args=parser.parse_args()
if os.name=='nt':ctypes.windll.kernel32.SetErrorMode(0x8003)
out=root/'build-host-checks/sd-r203';out.mkdir(parents=True,exist_ok=True)
spool=(root/'main/offline_audio_spool.c').read_text(encoding='utf-8-sig')
storage=(root/'main/sd_audio_session_storage.inc').read_text(encoding='utf-8-sig')
app=(root/'main/nuwax_ai_partner_main.c').read_text(encoding='utf-8-sig')
def function(text,name):
    m=re.search(r'^(?:static )?[\w *]+\b'+name+r'\([^;{}]*\)\s*\{',text,re.M);assert m,name
    at=text.index('{',m.start())+1;depth=1
    while depth:depth+=(text[at]=='{')-(text[at]=='}');at+=1
    return text[m.start():at]
pieces={
 'types':spool[spool.index('typedef struct {\n    uint8_t type;'):spool.index('static spool_state_t s;')],
 'crc':spool[spool.index('static uint32_t crc32_update('):spool.index('static esp_err_t validate_archive_frame(')],
 'catalog':storage[storage.index('static bool card_recording_locked('):storage.rindex('#endif')],
 'functions':spool[spool.index('static FILE *archive_create_exclusive('):spool.index('static void writer_schedule_retry_locked(')],
 'loss':spool[spool.index('static void mirror_note_loss('):spool.index('static void mirror_drain(')]}
for name,text in pieces.items():
    if name=='functions':text=text.replace('fread(block, 1U, n, readback)','host_readback_fread(block, 1U, n, readback)')
    (out/('sd_storage_'+name+'_host.inc')).write_text(text,encoding='utf-8')
flags=['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function',
 '-DESP_ERR_INVALID_CRC=0x109','-DESP_ERR_NOT_FOUND=0x105',
 '-DSD_AUDIO_CATALOG_ROOT="catalog"','-DSD_AUDIO_ARCHIVE_ROOT="audio"','-DSD_AUDIO_ARCHIVE_NEW_ROOT="audio2"',
 '-I'+str(out),'-I'+str(root/'main/audio_modes/meeting/host_stubs'),'-I'+str(root/'main')]
def run(name,code,units=(),long=False):
    source=out/(name+'.c');source.write_text(code,encoding='utf-8');exe=out/(name+'.exe')
    subprocess.run(flags+[str(source)]+[str(p) for p in units]+['-o',str(exe)],check=True)
    with tempfile.TemporaryDirectory(prefix='nuwax-r203-'+name+'-') as cwd:
        subprocess.run([str(exe)]+(['long'] if long else []),cwd=cwd,check=True,timeout=600 if long else 90)
prefix=r'''
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
static bool corrupt_readback;
static unsigned readbacks;
static size_t host_readback_fread(void *dst,size_t size,size_t count,FILE *file){
 size_t got=fread(dst,size,count,file);readbacks++;
 if(corrupt_readback&&got)((uint8_t*)dst)[0]^=0x40U;
 return got;
}
#define CONFIG_NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES 4194304U
#define main old_storage_main
#include "sd_audio_storage_host_check.c"
#undef main
'''
code=prefix+r'''
int main(int argc,char **argv){(void)argv;assert(sd_audio_catalog_init()==ESP_OK);
 begin("readback_bad_first",0U);corrupt_readback=true;
 int16_t pcm[320]={0};spool_item_t item={.type=1,.generation=1};
 assert(nuwax_audio_frame_build_pcm16(item.data,sizeof(item.data),pcm,320U,0U,(uint32_t)clock_ms,0,16000,1,20,&item.bytes)==ESP_OK);
 assert(append_locked(&item)==ESP_FAIL&&s.writer_frame_written&&!s.archive_ready);
 assert(!strcmp(s_writer_fault_stage,"record_readback_crc"));
 corrupt_readback=false;assert(commit_segment_locked(false)==ESP_OK);
 puts("PASS actual writer: corrupt successful read is detected at first durable frame, cannot publish recording");
 begin("readback_bad_batch",1U);s_writer_fault_stage=NULL;frame(0U,true);
 for(unsigned i=1;i<99;i++)frame(i,true);
 corrupt_readback=true;item.generation=s.capture_generation;item.realtime=true;
 assert(nuwax_audio_frame_build_pcm16(item.data,sizeof(item.data),pcm,320U,99U,(uint32_t)clock_ms,0,16000,1,20,&item.bytes)==ESP_OK);
 assert(append_locked(&item)==ESP_FAIL&&s.writer_frame_written&&!strcmp(s_writer_fault_stage,"record_readback_crc"));
 corrupt_readback=false;assert(commit_segment_locked(false)==ESP_OK);
 puts("PASS actual writer: periodic corruption caught within 100 frames; bytes retained and no false success");
 begin("readback_bad_tail",0U);frame(0U,true);frame(1U,true);corrupt_readback=true;
 assert(commit_segment_locked(true)==ESP_FAIL&&!s.file&&path_exists(s.open_path));
 assert(!s_card_pending_upload);corrupt_readback=false;
 puts("PASS actual writer: corrupt tail never sealed or published for upload, .part retained");
 begin("repair_buffered_tail",0U);frame(0U,true);
 char ready_path[128];strcpy(ready_path,s.ready_path);
 uint8_t torn[1024];memset(torn,0xa5,sizeof(torn));
 assert(fwrite(torn,1,sizeof(torn),s.file)==sizeof(torn));s.degraded=true;
 assert(commit_segment_locked(false)==ESP_OK);
 struct stat tail_stat;assert(!stat(ready_path,&tail_stat)&&tail_stat.st_size==96+666+32);
 puts("PASS actual writer: buffered torn tail flushed before truncation; exact one-frame archive with no trailing bytes");
 begin("filename_exhausted",0U);s.next_index=UINT32_MAX;
 item.generation=s.capture_generation;item.realtime=true;
 assert(append_locked(&item)==ESP_ERR_INVALID_SIZE&&!s.file&&!s_card_pending_upload&&s.next_index==UINT32_MAX);
 puts("PASS actual writer: exhausted filename counter cannot wrap, reserve a catalog entry or overwrite an old archive");
 if(argc>1)for(unsigned mode=0;mode<2U;mode++){
   begin(mode?"monitor_eight_hours":"meeting_eight_hours",mode);unsigned cursor=0,checked=0;
   const unsigned total=8U*60U*60U*50U;
   for(unsigned i=0;i<total;i++){
     unsigned previous=s_card_session.segment_count;frame(i,(i/(60U*60U*50U))%2U==0U);
     if(s_card_session.segment_count>previous&&i){
       sd_audio_segment_t got=part(checked,cursor);
       cursor+=(got.total_bytes-HEADER_BYTES-FOOTER_BYTES)/666U;checked++;
       assert(cursor==i);
     }
   }
   assert(commit_segment_locked(true)==ESP_OK);sd_audio_segment_t tail=part(checked,cursor);
   cursor+=(tail.total_bytes-HEADER_BYTES-FOOTER_BYTES)/666U;checked++;
   assert(cursor==total&&checked==s_card_session.segment_count&&readbacks>10000U&&!s.dropped_frames);
   printf("PASS eight-hour logical PCM mode=%u: %u frames, %u files, every sequence/PCM/timestamp/footer CRC exact; no dropped frames\n",mode,cursor,checked);
 }
}
'''
run('writer',code,[root/'main/sd_audio_catalog.c',root/'main/audio_frame.c'],long=args.long)
# Run the actual append admission prefix. A recoverable fault must retain an
# attached mirror, not return INVALID_STATE and force an end transaction.
append=function(spool,'append_frame')
start=append.index('    size_t expected_samples = 0U;');end=append.index('    if (!runtime_initialized()',start)
admission=append[start:end].replace('    size_t expected_samples = 0U;','')
run('backpressure',r'''
#include <assert.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#define CONFIG_NUWAX_SD_SESSION_ENABLE 1
#define ESP_ERR_NOT_FINISHED 7
static struct{int admission_lock;}s;
static struct{atomic_bool pending;}s_card_io_recovery;
static atomic_bool s_replay_media_fault,s_media_ready;
static bool active;static unsigned released;
static bool nuwax_sd_session_active(void){return active;}
static void xSemaphoreGive(int lock){(void)lock;released++;}
static int prefix(void){
'''+admission+r'''
return 0;}
int main(void){
 active=true;s_media_ready=true;assert(prefix()==0&&!released);
 s_card_io_recovery.pending=true;assert(prefix()==7&&released==1);
 s_card_io_recovery.pending=false;s_replay_media_fault=true;assert(prefix()==7&&released==2);
 s_replay_media_fault=false;s_media_ready=false;assert(prefix()==7&&released==3);
 active=false;assert(prefix()==0&&released==3);
 puts("PASS real append admission: transient recovery retains attached PCM ring; healthy/cardless behavior unchanged");
}
''')
poll=function(app,'exclusive_audio_poll_controls')
assert 'nuwax_offline_audio_replay_formal_start' not in poll
assert poll.count('service_offline_audio_replay(ws);')==1
start=function(spool,'replay_formal_start_locked')
assert start.index('!nuwax_sd_card_is_mounted()')<start.index('xTaskCreatePinnedToCoreWithCaps')
finish=spool[spool.index('    if (result == ESP_ERR_NOT_FINISHED) {',spool.index('static void formal_replay_task(')):spool.index('static esp_err_t replay_formal_start_locked(')]
assert 's_replay_launch_epoch = 0' not in finish and 'offline_now_ms() + 100U' in finish
print('PASS launch gates: single dispatcher, healthy admission and bounded deferred retry',flush=True)
finish_branch=function(spool[spool.index('    if (result == ESP_ERR_NOT_FINISHED) {',spool.index('static void formal_replay_task(')):].replace('    if (result == ESP_ERR_NOT_FINISHED) {','static void deferred(void) {',1),'deferred')
run('launch',r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#define CONFIG_NUWAX_SD_SESSION_ENABLE 1
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 1
#define ESP_ERR_NOT_FINISHED 2
#define ESP_ERR_NO_MEM 3
#define pdPASS 1
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define OFFLINE_REPLAY_FORMAL_TASK_STACK_BYTES 16384
#define OFFLINE_REPLAY_FORMAL_TASK_PRIORITY 5
#define ESP_LOGI(...) ((void)0)
typedef int esp_err_t;typedef int BaseType_t;typedef int nuwax_ws_client_t;
static struct{atomic_bool media_recovery_requested;}s;
static atomic_bool s_shutdown_requested,s_replay_retry_required,s_formal_replay_running;
static atomic_bool s_media_ready,s_replay_media_fault,s_formal_replay_cancel_requested;
static struct{nuwax_ws_client_t *ws;unsigned epoch,core;}s_formal_replay_context;
static uint32_t s_replay_launch_epoch,s_replay_launch_revision,s_replay_launch_deadline_ms,clock_ms;
static unsigned spawned;static bool healthy=true;
static bool nuwax_power_ctrl_shutdown_preparing(void){return false;}
static bool nuwax_offline_audio_storage_busy(void){return false;}
static bool enabled(void){return true;}
static bool runtime_initialized(void){return true;}
static bool nuwax_ws_is_connected(nuwax_ws_client_t *w){(void)w;return true;}
static unsigned nuwax_ws_connection_epoch(nuwax_ws_client_t *w){(void)w;return 4;}
static bool nuwax_offline_audio_has_ready(void){return true;}
static uint32_t offline_now_ms(void){return clock_ms;}
static uint32_t sd_audio_catalog_live_revision(void){return 10;}
static bool nuwax_audio_uplink_background_ready(void){return true;}
static bool nuwax_sd_card_is_mounted(void){return healthy;}
static bool offline_deadline_elapsed(uint32_t n,uint32_t d){return (int32_t)(n-d)>=0;}
static bool replay_ota_active(void){return false;}
static unsigned replay_formal_target_core(void){return 1;}
static void formal_replay_task(void *p){(void)p;}
static int xTaskCreatePinnedToCoreWithCaps(void(*f)(void*),const char *n,unsigned z,void *p,unsigned pri,void *h,int c,unsigned cap){
 (void)f;(void)n;(void)z;(void)p;(void)pri;(void)h;(void)c;(void)cap;spawned++;return pdPASS;
}
'''+finish_branch+'\n'+start+r'''
int main(void){nuwax_ws_client_t w=0;s_media_ready=true;
 healthy=false;
 for(clock_ms=0;clock_ms<120000U;clock_ms++)assert(replay_formal_start_locked(&w,4)==ESP_ERR_NOT_FINISHED);
 assert(spawned==0);healthy=true;
 for(clock_ms=120000U;clock_ms<180000U;clock_ms++){
   if(replay_formal_start_locked(&w,4)==ESP_OK&&s_formal_replay_running){deferred();s_formal_replay_running=false;}
 }
 assert(spawned==600); /* One bounded retry per 100 ms, not per MIC/control poll. */
 clock_ms=UINT32_MAX-10U;s_replay_launch_epoch=0;
 assert(replay_formal_start_locked(&w,4)==ESP_OK);deferred();s_formal_replay_running=false;
 unsigned before=spawned;clock_ms=20;assert(replay_formal_start_locked(&w,4)==ESP_ERR_NOT_FINISHED&&spawned==before);
 clock_ms=100;assert(replay_formal_start_locked(&w,4)==ESP_OK&&spawned==before+1U);
 puts("PASS actual launch: 120,000 unhealthy polls create zero tasks; deferred retry bounded across uptime wrap");
}
''')
run('time-cache',r'''
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "sd_audio_catalog.h"
#define ESP_LOGW(...) ((void)0)
static sd_audio_session_t s_card_session;
static uint32_t s_card_boot_id;
static sd_audio_session_time_t s_card_time;
static int result;
bool nuwax_cloud_time_snapshot(nuwax_cloud_time_anchor_t *a){(void)a;return false;}
static bool card_stop_time(uint64_t *m){*m=2000;return true;}
esp_err_t sd_audio_catalog_session_time(const sd_audio_session_t *s,uint32_t b,const nuwax_cloud_time_anchor_t *a,bool end,uint64_t m,sd_audio_session_time_t *out){
 (void)s;(void)b;(void)a;(void)end;(void)m;*out=(sd_audio_session_time_t){0};
 if(result==0){*out=(sd_audio_session_time_t){1800000000000ULL,2000,3};}
 return result;
}
'''+function(storage,'card_save_time_locked')+r'''
int main(void){s_card_time=(sd_audio_session_time_t){1800000000000ULL,1000,3};result=-1;
 card_save_time_locked();assert(s_card_time.started_at_ms==1800000000000ULL&&s_card_time.duration_ms==1000&&s_card_time.valid==3);
 result=0;card_save_time_locked();assert(s_card_time.duration_ms==2000);
 puts("PASS time cache: failed I/O cannot erase last durable time or publish an unpersisted duration");}
''')
(out/'results.json').write_text(json.dumps(dict(passed=True,eightHourLogicalRun=args.long,hardwareValidated=False),indent=2)+'\n',encoding='utf-8')
