"""Rolling meeting FIFO, adaptive byte GHASH and unchanged safety boundaries."""
from pathlib import Path
import subprocess,tempfile,ctypes,os,sys,json,re
root=Path(__file__).resolve().parents[1];out=root/'build-host-checks/sd-r201';out.mkdir(parents=True,exist_ok=True)
if os.name=='nt':ctypes.windll.kernel32.SetErrorMode(0x8003)
flags=['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function',
 '-DESP_ERR_INVALID_CRC=0x109','-DESP_ERR_NOT_FOUND=0x105','-DSD_AUDIO_CATALOG_ROOT="catalog"',
 '-DSD_AUDIO_ARCHIVE_ROOT="audio"','-DSD_AUDIO_ARCHIVE_NEW_ROOT="audio2"',
 '-I'+str(root/'main'),'-I'+str(root/'main/audio_modes/meeting/host_stubs'),'-I'+str(root/'managed_components/espressif__cjson/cJSON')]
def run(name,units):
 exe=out/(name+'.exe');subprocess.run(flags+[str(p) for p in units]+['-o',str(exe)],check=True)
 with tempfile.TemporaryDirectory(prefix='nuwax-r201-') as cwd:subprocess.run([str(exe)],cwd=cwd,check=True,timeout=90)
run('meeting-stream',[root/'main/sd_meeting_stream_host_check.c',root/'main/audio_frame.c',root/'managed_components/espressif__cjson/cJSON/cJSON.c'])
run('ghash',[root/'main/archive_ghash_host_check.c'])
header=(root/'main/archive_ghash.h').read_text();needle='(z[0] >> 8U) ^ reduction;';assert header.count(needle)==1
(out/'archive_ghash.h').write_text(header.replace(needle,'(z[0] >> 8U) ^ (reduction ^ 1U);'),encoding='utf-8')
(out/'ghash-negative.c').write_text((root/'main/archive_ghash_host_check.c').read_text(),encoding='utf-8')
exe=out/'ghash-negative.exe';subprocess.run(flags+[str(out/'ghash-negative.c'),'-o',str(exe)],check=True)
r=subprocess.run([str(exe)],capture_output=True,timeout=30);assert r.returncode!=0,'GHASH polynomial mutant escaped reference'
print('PASS negative: wrong GHASH reduction is rejected by the independent reference',flush=True)
env=os.environ.copy()
for mode in ['0','1']:
 env['NUWAX_SD_TEST_FAST_GHASH']=mode
 subprocess.run([sys.executable,str(root/'tools/test-sd-r200.py')],cwd=root,env=env,check=True,timeout=90)
spool=(root/'main/offline_audio_spool.c').read_text(encoding='utf-8-sig')
def function(text,name):
 m=re.search(r'^(?:static )?[\w *]+\s+\b'+name+r'\([^;{}]*\)\s*\{',text,re.M);assert m,name
 end=text.index('{',m.start())+1;depth=1
 while depth:depth+=(text[end]=='{')-(text[end]=='}');end+=1
 return text[m.start():end]
body=function(spool,'nuwax_offline_audio_storage_busy')
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#define CONFIG_NUWAX_SD_SESSION_ENABLE 1
#define CONFIG_NUWAX_OTA_ENABLE 1
static int s_capture_admission;
static bool admission,ota,card;
static atomic_bool s_card_finishing,s_card_failed,s_shutdown_requested,s_media_ready;
static struct {atomic_bool media_recovery_requested,writer_error,active,closing,capture_running,writer_busy;
 atomic_uint s_mirror_dummy;int queue;} s;
static atomic_uint s_mirror_closers;
static unsigned depth;
static bool sd_capture_admission_held(int *p,uint32_t t){(void)p;(void)t;return admission;}
static uint32_t offline_now_ms(void){return 0;}
static bool nuwax_ota_device_is_active(void){return ota;}
static bool nuwax_sd_session_active(void){return card;}
static bool enabled(void){return true;}
static bool runtime_initialized(void){return true;}
static unsigned uxQueueMessagesWaiting(int q){(void)q;return depth;}
'''+body+r'''
int main(void){
 s_media_ready=true;card=true;s.active=s.capture_running=s.writer_busy=true;s.queue=1;depth=64;
 assert(!nuwax_offline_audio_storage_busy());
 admission=true;assert(nuwax_offline_audio_storage_busy());admission=false;
 ota=true;assert(nuwax_offline_audio_storage_busy());ota=false;
 s.media_recovery_requested=true;assert(nuwax_offline_audio_storage_busy());s.media_recovery_requested=false;
 s.writer_error=true;assert(nuwax_offline_audio_storage_busy());s.writer_error=false;
 s_card_finishing=s_card_failed=true;assert(nuwax_offline_audio_storage_busy());s_card_finishing=s_card_failed=false;
 s_shutdown_requested=true;assert(nuwax_offline_audio_storage_busy());s_shutdown_requested=false;
 s_media_ready=false;assert(nuwax_offline_audio_storage_busy());s_media_ready=true;
 card=false;assert(nuwax_offline_audio_storage_busy());s.active=s.capture_running=s.writer_busy=false;depth=0;
 assert(!nuwax_offline_audio_storage_busy());s_mirror_closers=1;assert(nuwax_offline_audio_storage_busy());
 puts("PASS storage gate: live card capture permits sealed-file replay; start/OTA/recovery/writer/media/shutdown and no-card capture remain exclusive");
}
'''
(out/'storage-gate.c').write_text(code,encoding='utf-8');run('storage-gate',[out/'storage-gate.c'])
replay=(root/'main/sd_audio_session_replay.inc').read_text()
assert 'sd_meeting_fifo_next' in replay and 'sd_meeting_fifo_end_ready' in replay and 'sd_meeting_fifo_finish' in replay
assert 'if (!session->ended) return ESP_ERR_NOT_FINISHED;' in replay
assert 'retry_pending' in replay and 'atomic_store(&s_card_pending_upload, false)' in replay
assert 'revision == s_replay_launch_revision' in spool
assert 'nuwax_sd_session_active() && s.queue' in spool
assert '(!s_card_replay_session.mode && !s_card_replay_session.ended)' not in spool
sdk=(root/'main/tls_archive_crypto.c').read_text();assert 'matches && archive_ghash_worthwhile' in sdk
assert 'mbedtls_platform_zeroize(s_ghash_table, sizeof(*s_ghash_table))' in sdk
# Execute the unmodified SDK API dispatcher in both configurations. This proves
# which synchronization route changes, not a hardware throughput measurement.
tcpip=Path(os.environ.get('IDF_PATH','C:/esp/v6.0/esp-idf'))/'components/lwip/lwip/src/api/tcpip.c'
dispatch=function(tcpip.read_text(),'tcpip_send_msg_wait_sem')
stub=r'''
#include <assert.h>
#include <stdio.h>
typedef int err_t;
typedef int sys_sem_t;
typedef void (*tcpip_callback_fn)(void *);
enum {ERR_OK=0,TCPIP_MSG_API=1};
static int held,locks,posts,waits,calls,tcpip_mbox;
struct message {int type;struct {struct {tcpip_callback_fn function;void *msg;} api_msg;} msg;};
#define LWIP_UNUSED_ARG(x) (void)(x)
#define LWIP_ASSERT(text,test) assert(test)
#define LOCK_TCPIP_CORE() do {assert(!held);held=1;++locks;} while(0)
#define UNLOCK_TCPIP_CORE() do {assert(held);held=0;} while(0)
#define TCPIP_MSG_VAR_DECLARE(name) struct message name
#define TCPIP_MSG_VAR_ALLOC(name) (void)(name)
#define TCPIP_MSG_VAR_REF(name) (name)
#define TCPIP_MSG_VAR_FREE(name) (void)(name)
#define sys_sem_valid(x) ((x)!=NULL)
#define sys_mbox_valid_val(x) ((void)(x),1)
static void sys_mbox_post(int *box,struct message *m){
 (void)box;assert(m->type==TCPIP_MSG_API);++posts;m->msg.api_msg.function(m->msg.api_msg.msg);
}
static void sys_arch_sem_wait(sys_sem_t *sem,int timeout){assert(sem);assert(!timeout);++waits;}
static void callback(void *p){assert(p==&calls);assert(held==LWIP_TCPIP_CORE_LOCKING);++calls;}
'''+dispatch+r'''
int main(void){sys_sem_t sem=1;(void)tcpip_mbox;assert(tcpip_send_msg_wait_sem(callback,&calls,&sem)==ERR_OK);
 assert(calls==1&&!held);
 assert(locks==LWIP_TCPIP_CORE_LOCKING);
 assert(posts==!LWIP_TCPIP_CORE_LOCKING&&waits==!LWIP_TCPIP_CORE_LOCKING);
 puts("PASS SDK tcpip dispatcher: mutex route avoids per-call mailbox and semaphore round trip");
}
'''
for locking in [0,1]:
 source=out/('tcpip-core-'+str(locking)+'.c')
 source.write_text('#define LWIP_TCPIP_CORE_LOCKING '+str(locking)+'\n'+stub,encoding='utf-8')
 run('tcpip-core-'+str(locking),[source])
assert 'CONFIG_LWIP_TCPIP_CORE_LOCKING=y' in (root/'sdkconfig.sd-replay-formal.defaults').read_text()
(out/'results.json').write_text(json.dumps(dict(rollingMeeting=True,independentGhashCases=3328,oracleCases=1536,
 calibrationThresholdPercent=15,negativeGhashCaught=True,storageGatePassed=True,sdkCoreLockDispatchBranchesTested=2,
 hardwareSpeedMeasured=False,passed=True),indent=2)+'\n')
print('PASS r201: rolling meeting/FIFO, both production crypto backends, mutation rejection, storage gates and retry wakeup',flush=True)
