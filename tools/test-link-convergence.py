"""Execute production UI convergence, absent-media backoff and NVS write path."""
from pathlib import Path
import re
import subprocess
import argparse
parser=argparse.ArgumentParser()
parser.add_argument('--sd-only',action='store_true',help='Run UI/SD convergence without unrelated NVS probes')
args=parser.parse_args()
root=Path(__file__).resolve().parents[1]
out=root/'build-host-checks/link-convergence'; out.mkdir(parents=True,exist_ok=True)
def extract(source,signature):
    a=source.index(signature); b=source.index('{',a); depth=1; end=b+1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}'); end+=1
    return source[a:end]
def run(name,code,negative=False):
    src=out/(name+'.c'); exe=out/(name+'.exe');src.write_text(code,encoding='utf-8')
    subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-parameter',
                    '-I'+str(root/'main/audio_modes/meeting/host_stubs'),str(src),'-o',str(exe)],check=True)
    result=subprocess.run([str(exe)],capture_output=negative)
    assert (result.returncode!=0) if negative else (result.returncode==0), name
    print('PASS: '+name+(' mutant rejected' if negative else ''))
common=r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#define ESP_OK 0
#define ESP_FAIL 1
#define ESP_ERR_TIMEOUT 2
#define ESP_LOGW(...) ((void)0)
typedef int esp_err_t;
"""
shell=(root/'main/app_shell.c').read_text(encoding='utf-8-sig')
ui=common+r"""
enum {NUWAX_WIFI_CONNECTED, NUWAX_WIFI_CONNECTING, NUWAX_WIFI_WEAK,NUWAX_WIFI_DISCONNECTED,NUWAX_CLOUD_DISCONNECTED};
typedef struct {int wifi;bool provisioning_active,provisioning_starting;char ssid[33],ip[16];} nuwax_device_state_t;
static nuwax_device_state_t state;
static bool connected,provisioning,fail_status,drop_during_status;
static unsigned writes,cloud_writes;
static uint32_t s_wifi_signal_last_poll_ms;
#define APP_SHELL_WIFI_SIGNAL_POLL_MS 1000U
static void nuwax_state_bridge_service(uint32_t t){}
static bool nuwax_app_shell_is_ui_ready(void){return true;}
static bool nuwax_app_shell_get_device_state_snapshot(nuwax_device_state_t *s,void *p){*s=state;return true;}
static bool nuwax_wifi_is_connected(void){return connected;}
static bool nuwax_wifi_provisioning_active(void){return provisioning;}
static void nuwax_app_shell_set_wifi_status(const char *s,const char *ip){writes++;state.wifi=s?NUWAX_WIFI_CONNECTED:NUWAX_WIFI_DISCONNECTED;snprintf(state.ssid,sizeof state.ssid,"%s",s?s:"");snprintf(state.ip,sizeof state.ip,"%s",ip?ip:"");}
static void nuwax_app_shell_set_cloud_state(int s){cloud_writes++;}
static int nuwax_wifi_get_status(char *s,size_t n,char *ip,size_t m){snprintf(s,n,"ap");snprintf(ip,m,"192.0.2.1");if(drop_during_status)connected=false;return fail_status?ESP_FAIL:ESP_OK;}
static int nuwax_wifi_get_rssi(int8_t *r){*r=-42;return ESP_OK;}
static void nuwax_app_shell_set_wifi_signal(bool v,int8_t r){assert(v&&r==-42);}
"""+extract(shell,'void nuwax_app_shell_update(uint32_t now_ms) {')+r"""
int main(void){
 state.wifi=NUWAX_WIFI_CONNECTING;connected=true;
 nuwax_app_shell_update(1000); assert(writes==1 && state.wifi==NUWAX_WIFI_CONNECTED && !cloud_writes);
 nuwax_app_shell_update(2000);assert(writes==1);
 strcpy(state.ip,"old");nuwax_app_shell_update(3000);assert(writes==2);
 connected=false;nuwax_app_shell_update(4000);assert(writes==3&&cloud_writes==1);
 connected=true;provisioning=true;nuwax_app_shell_update(5000);assert(writes==3);
 provisioning=false;state.provisioning_starting=true;nuwax_app_shell_update(6000);assert(writes==3);
 state.provisioning_starting=false;state.provisioning_active=true;nuwax_app_shell_update(7000);assert(writes==3);
 state.provisioning_active=false;fail_status=true;nuwax_app_shell_update(8000);assert(writes==3);
 fail_status=false;drop_during_status=true;nuwax_app_shell_update(9000);assert(writes==3);
 drop_during_status=false;connected=true;nuwax_app_shell_update(10000);assert(writes==4&&cloud_writes==1);
}
"""
run('ui_bidirectional',ui)
run('ui_missing_connected_repair',ui.replace('ssid[0] && ip[0] && nuwax_wifi_is_connected()', 'false && ssid[0] && ip[0] && nuwax_wifi_is_connected()'),True)
spool=(root/'main/offline_audio_spool.c').read_text(encoding='utf-8-sig')
sd=common+'\n'.join(re.findall(r'^#define MEDIA_(?:RECOVERY|UNAVAILABLE)_RETRY_.*$',spool,re.M))+r"""
static uint32_t now;
static unsigned checkpoint_generation;
static atomic_bool s_replay_media_fault;
static atomic_uint s_media_generation;
static void replay_checkpoint_invalidate(void){checkpoint_generation++;}
static struct {uint32_t media_recovery_backoff_ms,media_recovery_deadline_ms;bool media_was_ready;int media_recovery_last_error;} s;
static uint32_t offline_now_ms(void){return now;}
"""+extract(spool,'static void schedule_media_recovery_locked(esp_err_t err)')+'\n'+extract(spool,'static void reset_media_recovery_locked(bool contents_may_have_changed)')+r"""
int main(void){
 const uint32_t absent[]={5000,10000,20000,40000,80000,120000,120000};
 for(unsigned i=0;i<7;i++){schedule_media_recovery_locked(ESP_ERR_TIMEOUT);assert(s.media_recovery_backoff_ms==absent[i]);now=s.media_recovery_deadline_ms;}
 unsigned before=checkpoint_generation;
 s_replay_media_fault=true;
 const unsigned media_before=atomic_load(&s_media_generation);
 reset_media_recovery_locked(false);assert(checkpoint_generation==before&&!s_replay_media_fault&&s_media_generation==media_before);
 reset_media_recovery_locked(true);assert(checkpoint_generation==before+1&&s_media_generation==media_before+1U);
 assert(s.media_was_ready&&!s.media_recovery_deadline_ms&&!s.media_recovery_backoff_ms);
 const uint32_t removed[]={1000,2000,4000,8000,16000,30000,30000};
 now=UINT32_MAX-300;
 for(unsigned i=0;i<7;i++){schedule_media_recovery_locked(ESP_ERR_TIMEOUT);assert(s.media_recovery_backoff_ms==removed[i]);assert((uint32_t)(s.media_recovery_deadline_ms-now)==removed[i]);now=s.media_recovery_deadline_ms;}
}
"""
run('sd_absent_and_removed',sd)
run('sd_fast_absent_mutant',sd.replace('#define MEDIA_UNAVAILABLE_RETRY_INITIAL_MS 5000U','#define MEDIA_UNAVAILABLE_RETRY_INITIAL_MS 1000U'),True)
if args.sd_only:
    raise SystemExit(0)
report=(root/'main/device_error_report.c').read_text(encoding='utf-8-sig')
persist=extract(report,'static void persist_snapshot_if_dirty(void)')
a=persist.index('    nvs_handle_t handle = 0;'); b=persist.index('    nuwax_wake_word_detector_end_flash_operation();',a)
write=persist[a:b]
report_header=(root/'main/device_error_report.h').read_text(encoding='utf-8-sig')
result_bytes=re.search(r'^#define NUWAX_DEVICE_ERROR_RESULT_BYTES\s+.+$',report_header,re.M).group()
record_constants='\n'.join(re.findall(r'^#define ERROR_REPORT_(?:QUEUE_CAPACITY|[A-Z_]+_BYTES)\s+.+$',report,re.M))
record_types=report[report.index('typedef struct {'):report.index('static error_report_event_t s_events')]
nvs=common+'\n#include <stddef.h>\n'+result_bytes+'\n'+record_constants+'\n'+record_types+'\n'+extract(report,'static size_t compact_snapshot_size(uint32_t count)')+r"""
#define ESP_ERR_NVS_NOT_FOUND 3
#define ESP_ERR_NVS_NOT_ENOUGH_SPACE 4
#define NVS_READWRITE 1
#define ERROR_REPORT_NVS_NAMESPACE "diag"
#define ERROR_REPORT_NVS_KEY "queue"
typedef unsigned nvs_handle_t;
static error_report_persisted_queue_t snapshot_value,*snapshot=&snapshot_value;
static unsigned opens,sets,erases,commits,closes;
static int open_error,set_error,commit_error;
static int nvs_open(const char *n,int m,nvs_handle_t *h){opens++;*h=1;return open_error;}
static int nvs_set_blob(nvs_handle_t h,const char *k,const void *p,size_t n){assert(p==snapshot&&n==compact_snapshot_size(snapshot->count)&&n<sizeof(*snapshot));sets++;return set_error;}
static int nvs_erase_key(nvs_handle_t h,const char *k){erases++;return ESP_ERR_NVS_NOT_FOUND;}
static int nvs_commit(nvs_handle_t h){commits++;return commit_error;}
static void nvs_close(nvs_handle_t h){closes++;}
static int write_snapshot(void){
"""+write+r"""
 return err;
}
int main(void){
 snapshot->count=1;set_error=ESP_ERR_NVS_NOT_ENOUGH_SPACE;
 assert(write_snapshot()==ESP_ERR_NVS_NOT_ENOUGH_SPACE&&sets==1&&commits==0&&erases==0&&closes==1);
 set_error=0;assert(write_snapshot()==ESP_OK&&sets==2&&commits==1&&closes==2);
 snapshot->count=0;assert(write_snapshot()==ESP_OK&&erases==1&&commits==2&&closes==3);
 open_error=ESP_FAIL;assert(write_snapshot()==ESP_FAIL&&closes==3);
 open_error=0;commit_error=ESP_FAIL;assert(write_snapshot()==ESP_FAIL&&closes==4);
}
"""
run('nvs_no_space_no_compaction',nvs)
run('nvs_retry_mutant',nvs.replace('/* Diagnostics are optional.', 'nvs_set_blob(handle, ERROR_REPORT_NVS_KEY, snapshot, sizeof(*snapshot));\n                /* Diagnostics are optional.'),True)
main=(root/'main/nuwax_ai_partner_main.c').read_text(encoding='utf-8-sig')
assert re.search(r'WIFI_RECOVERY_DONE\) \{\s*\(void\)recover_wifi_if_needed\(\);\s*\}\s*drain_wifi_status_reports\(ws\)',main)
assert 'nvs_erase_all' not in persist
assert '#define ERROR_REPORT_PERSIST_NO_SPACE_MAX_FAILURES 1U' in report
assert 's_persist_storage_blocked = true;' in persist
print('PASS: connected owner consumes DONE before reports; NVS blocks retries while RAM delivery remains separate')
start=main.index('/* Single-slot mailbox:')
end=main.index('static void drain_wifi_status_reports(',start)
production=main[start:end]
include=out/'wifi_background_recovery_host.inc'
include.write_text(production,encoding='utf-8')
harness=(root/'main/wifi_background_recovery_host_check.c').read_text(encoding='utf-8-sig')
run('wifi_completion_interleavings',harness)
try:
    include.write_text(production.replace('if (!fenced && connected &&',
                                         'if (false && !fenced && connected &&'),encoding='utf-8')
    run('wifi_early_admission_mutant',harness,True)
finally:
    include.write_text(production,encoding='utf-8')
