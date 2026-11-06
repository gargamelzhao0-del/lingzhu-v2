"""Active-manifest, persisted-start and real blocking-wait regressions."""
from pathlib import Path
import ctypes,os,re,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
out=root/'build-host-checks/sd-r198';out.mkdir(parents=True,exist_ok=True)
if os.name=='nt':ctypes.windll.kernel32.SetErrorMode(0x8003)
base=['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function',
 '-DESP_ERR_INVALID_CRC=0x109','-DESP_ERR_NOT_FOUND=0x105',
 '-DSD_AUDIO_CATALOG_ROOT="catalog"','-DSD_AUDIO_ARCHIVE_ROOT="audio"','-DSD_AUDIO_ARCHIVE_NEW_ROOT="audio2"',
 '-I'+str(out),'-I'+str(root/'main/audio_modes/meeting/host_stubs'),'-I'+str(root/'main')]
def run(name,units,timeout=90):
 exe=out/(name+'.exe');subprocess.run(base+[str(p) for p in units]+['-o',str(exe)],check=True)
 with tempfile.TemporaryDirectory(prefix='nuwax-r198-'+name+'-') as directory:
  subprocess.run([str(exe)],cwd=directory,check=True,timeout=timeout)
run('active-manifest',[root/'main/sd_audio_active_manifest_host_check.c',root/'main/audio_frame.c'],120)
spool=(root/'main/offline_audio_spool.c').read_text(encoding='utf-8-sig')
storage=(root/'main/sd_audio_session_storage.inc').read_text(encoding='utf-8')
pieces={
 'types':spool[spool.index('typedef struct {\n    uint8_t type;'):spool.index('static spool_state_t s;')],
 'crc':spool[spool.index('static uint32_t crc32_update('):spool.index('static esp_err_t validate_archive_frame(')],
 'catalog':storage[storage.index('static bool card_recording_locked('):storage.rindex('#endif')],
 'functions':spool[spool.index('static FILE *archive_create_exclusive('):spool.index('static void writer_schedule_retry_locked(')],
 'loss':spool[spool.index('static void mirror_note_loss('):spool.index('static void mirror_drain(')]}
for name,text in pieces.items():(out/('sd_storage_'+name+'_host.inc')).write_text(text,encoding='utf-8')
run('first-frame',[root/'main/sd_audio_first_frame_host_check.c',root/'main/sd_audio_catalog.c',root/'main/audio_frame.c'])
app=(root/'main/nuwax_ai_partner_main.c').read_text(encoding='utf-8-sig')
def function(source,name):
 m=re.search(r'^static [\w *]+\b'+name+r'\([^;{}]*\)\s*\{',source,re.M);assert m,name
 end=source.index('{',m.start())+1;depth=1
 while depth:depth+=(source[end]=='{')-(source[end]=='}');end+=1
 return source[m.start():end]
(out/'audio_mode_exit_host.inc').write_text('\n'.join(function(app,name) for name in [
 'meeting_or_monitoring_mode_blocks_sleep','exclusive_audio_controllers_released','service_exclusive_audio_sleep_reconcile']),encoding='utf-8')
wait=function(app,'wait_for_mic_start_action')
prefix=wait[wait.index('        app_task_watchdog_checkpoint();'):wait.index('        /* OTA commands are consumed')]
(out/'mode_idle_prefix_host.inc').write_text('static esp_err_t mode_idle_prefix(nuwax_ws_client_t *ws){\n'+prefix+'\nreturn ESP_ERR_NOT_FINISHED;\n}\n',encoding='utf-8')
run('idle-prefix',[root/'main/audio_mode_idle_reconcile_host_check.c'])
old=(out/'mode_idle_prefix_host.inc').read_text()
assert 'service_exclusive_audio_sleep_reconcile();' in old
(out/'mode_idle_prefix_host.inc').write_text(old.replace('service_exclusive_audio_sleep_reconcile();','',1),encoding='utf-8')
exe=out/'idle-prefix-negative.exe';subprocess.run(base+[str(root/'main/audio_mode_idle_reconcile_host_check.c'),'-o',str(exe)],check=True)
r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=10);assert r.returncode!=0
(out/'mode_idle_prefix_host.inc').write_text(old,encoding='utf-8')
print('PASS negative: omitting mode-exit service inside the real wake wait is caught at runtime')
bridge=(root/'main/nuwax_ui_v28/nuwax_state_bridge.c').read_text()
mapping=function(bridge,'default_emotion_for_app_state')
code='#include <assert.h>\n#include "app_state.h"\n#include "nuwax_ui_v28/nuwax_device_state.h"\n'+mapping+'''
int main(void){
 assert(default_emotion_for_app_state(NUWAX_APP_STATE_ERROR,NUWAX_EMOTION_SAD)==NUWAX_EMOTION_NEUTRAL);
 assert(default_emotion_for_app_state(NUWAX_APP_STATE_SLEEPING,NUWAX_EMOTION_SAD)==NUWAX_EMOTION_SLEEPING_FACE);
 assert(default_emotion_for_app_state(NUWAX_APP_STATE_LISTENING,NUWAX_EMOTION_NEUTRAL)==NUWAX_EMOTION_FOCUSED);
 assert(default_emotion_for_app_state(NUWAX_APP_STATE_IDLE,NUWAX_EMOTION_SAD)==NUWAX_EMOTION_SAD);
 return 0;
}
'''
p=out/'error-face.c';p.write_text(code,encoding='utf-8');run('error-face',[p])
print('PASS system error fallback: neutral error face, normal sleep/listening and cloud emotion semantics retained')
