"""Signal-driven SD handoff and file-boundary routing, not a hardware speed test."""
from pathlib import Path
import subprocess,tempfile,os,ctypes,json,re
root=Path(__file__).resolve().parents[1];out=root/'build-host-checks/sd-r202';out.mkdir(parents=True,exist_ok=True)
if os.name=='nt':ctypes.windll.kernel32.SetErrorMode(0x8003)
spool=(root/'main/offline_audio_spool.c').read_text(encoding='utf-8-sig')
start=spool.index('static void replay_storage_signal(');end=spool.index('static StaticSemaphore_t s_writer_gate_storage;',start)
(out/'sd-writer-idle.inc').write_text(spool[start:end],encoding='utf-8')
flags=['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-misleading-indentation',
 '-I'+str(root/'main'),'-I'+str(out)]
exe=out/'handoff.exe';subprocess.run(flags+[str(root/'main/sd_storage_handoff_host_check.c'),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True,timeout=20)
# Negative control: suppressing writer publication must fail the event wake test.
bad=(out/'sd-writer-idle.inc').read_text().replace('replay_storage_signal();','/* missing signal */')
(out/'sd-writer-idle.inc').write_text(bad,encoding='utf-8')
exe=out/'no-signal.exe';subprocess.run(flags+[str(root/'main/sd_storage_handoff_host_check.c'),'-o',str(exe)],check=True)
result=subprocess.run([str(exe)],capture_output=True,timeout=20);assert result.returncode!=0,'missing release wake escaped test'
(out/'sd-writer-idle.inc').write_text(spool[start:end],encoding='utf-8')
print('PASS negative: missing writer wake is detected',flush=True)
header=(root/'main/offline_io_schedule.h').read_text();a=header.index('static inline unsigned offline_replay_formal_core(');b=header.index('typedef struct',a)
for single in [0,1]:
 source=out/('route-'+str(single)+'.c');source.write_text('#include <stdbool.h>\n#include <assert.h>\n#define CONFIG_FREERTOS_UNICORE '+str(single)+'\n'+header[a:b]+
 'int main(void){assert(offline_replay_formal_core(false)==0);assert(offline_replay_formal_core(true)=='+str(0 if single else 1)+');}\n',encoding='utf-8')
 exe=out/('route-'+str(single)+'.exe');subprocess.run(flags+[str(source),'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
loop=spool[spool.index('static esp_err_t replay_ready_impl('):spool.index('static uint32_t s_formal_retry_backoff_ms;')]
assert loop.index('replay_formal_core_handoff()')<loop.index('card_replay_next(')
assert '(uintptr_t)xTaskGetCurrentTaskHandle()' in spool and 's_formal_worker_identity, 0U' in spool
assert 's_formal_replay_context.core = replay_formal_target_core();' in spool
writer=spool[spool.index('static void writer_task('):spool.index('static void destroy_unpublished_runtime(')]
assert 'atomic_store_explicit(&s.writer_busy, false' not in writer and 'atomic_store(&s.writer_busy, false' not in writer
assert writer.count('writer_storage_idle();')==9
shared=(root/'main/offline_replay_shared.inc').read_text();prefetch=(root/'main/offline_replay_prefetch.inc').read_text()
assert 'replay_prefetch_join_profiled' in shared and 'SD formal reader admission revision=202' in shared
assert '*storage_stats = p->storage_stats' in prefetch
assert 'offline_replay_wait_relaxes(p->pressure_us - pressure_before' in prefetch
assert shared.index('nuwax_replay_ui_handoff')<shared.index('const int64_t packet_started')
assert shared.index('replay_shared_check',shared.index('nuwax_replay_ui_handoff'))<shared.index('nuwax_ws_send_archive_parts_if_epoch')
# Execute the real replay-loop selection boundary with a completed-file double.
# The existing WS suite separately verifies the real in-flight TLS drain.
def function(text,name):
 m=re.search(r'^(?:static )?[\w *]+\s+\b'+name+r'\([^;{}]*\)\s*\{',text,re.M);assert m,name
 at=text.index('{',m.start())+1;depth=1
 while depth:depth+=(text[at]=='{')-(text[at]=='}');at+=1
 return text[m.start():at]
begin=loop.index('    for (;;) {');end=loop.index('        char next_name[128]',begin)
boundary=loop[begin:end]+'        break;\n    }\n    return result;\n}'
core_test=r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <assert.h>
#include <stdio.h>
#define ESP_PLATFORM 1
#define CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE 1
#define CONFIG_NUWAX_SD_SESSION_ENABLE 1
#define ESP_OK 0
#define ESP_ERR_NOT_FINISHED 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_LOGI(...) ((void)0)
typedef int esp_err_t;typedef int nuwax_ws_client_t;
static struct {atomic_bool capture_running;} s;
static atomic_bool s_formal_replay_cancel_requested;
static atomic_uintptr_t s_formal_worker_identity;
static esp_err_t s_card_pass_error;
static unsigned core,commits,next_file,wire_frames;static bool in_frame,pinned,flip;
static uintptr_t xTaskGetCurrentTaskHandle(void){return 123;}
static unsigned xPortGetCoreID(void){return core;}
'''+header[a:b]+function(spool,'replay_formal_target_core')+function(spool,'replay_formal_core_handoff')+r'''
static esp_err_t card_replay_next(nuwax_ws_client_t *ws,unsigned epoch,bool independent,bool *worked){
 (void)ws;(void)epoch;(void)independent;assert(!in_frame&&!pinned);
 *worked=next_file<2;if(!*worked)return ESP_OK;
 in_frame=pinned=true;wire_frames++;
 if(flip){s.capture_running=false;flip=false;}
 /* State may change while the frame is pending: boundary guard runs only
  * after this whole call has drained/committed/released its file lease. */
 commits++;next_file++;in_frame=pinned=false;return ESP_OK;
}
static esp_err_t drive(nuwax_ws_client_t *ws,unsigned expected_epoch,bool independent_transport){
 esp_err_t result=ESP_OK;
'''+boundary+r'''
int main(void){nuwax_ws_client_t ws=0;
 core=1;s.capture_running=true;s_formal_worker_identity=123;flip=true;
 assert(drive(&ws,7,false)==ESP_ERR_NOT_FINISHED);
 assert(commits==1&&next_file==1&&wire_frames==1&&!in_frame&&!pinned);
 core=0;assert(drive(&ws,7,false)==ESP_OK);
 assert(commits==2&&next_file==2&&wire_frames==2);
 /* Synchronous caller on another task is never subjected to worker routing. */
 next_file=commits=wire_frames=0;core=1;s_formal_worker_identity=999;
 assert(drive(&ws,7,false)==ESP_OK&&commits==2);
 next_file=commits=wire_frames=0;s_formal_worker_identity=123;
 assert(drive(&ws,7,true)==ESP_OK&&commits==2);
 puts("PASS actual replay boundary: phase changes finish current frame/file first, relaunch core0 resumes next file without duplicate; synchronous/independent callers retained");
}
'''
source=out/'core-boundary.c';source.write_text(core_test,encoding='utf-8');exe=out/'core-boundary.exe'
subprocess.run(flags+[str(source),'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True,timeout=20)
(out/'results.json').write_text(json.dumps(dict(passed=True,signalHandoff=True,writerLockQueueWakeRaces=True,
 cancellationAndEpochAfterLock=True,negativeMissingSignalCaught=True,phaseAwareWorkerCore=True,
 uniprocessorPolicyTested=True,readerMetricsExported=True,actualFileBoundaryHandoffTested=True,hardwareSpeedMeasured=False),indent=2)+'\n')
print('PASS r202: signalled handoff, bounded waits, fences, reader diagnostics, phase-aware core selection and frame-boundary placement',flush=True)
