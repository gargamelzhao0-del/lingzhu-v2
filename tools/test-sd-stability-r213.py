"""Execute production UI result delivery and cooperative evidence cancellation."""
from pathlib import Path
import ctypes,os,re,subprocess,tempfile
root=Path(__file__).resolve().parents[1];out=root/'build-host-checks/sd-stability-r213';out.mkdir(parents=True,exist_ok=True)
if os.name=='nt':ctypes.windll.kernel32.SetErrorMode(0x8003)
def function(text,signature):
    found=re.search(re.escape(signature)+r'[^;{}]*\{',text);assert found,signature
    start=found.start();at=text.index('{',start)+1;depth=1
    while depth:depth+=(text[at]=='{')-(text[at]=='}');at+=1
    return text[start:at]
def run(name,code,units=()):
    source=out/(name+'.c');source.write_text(code,encoding='utf-8');exe=out/(name+'.exe')
    subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function',
        '-DESP_ERR_INVALID_CRC=0x109','-I'+str(root/'main'),'-I'+str(root/'main/audio_modes/meeting/host_stubs'),
        str(source),*map(str,units),'-o',str(exe)],check=True)
    with tempfile.TemporaryDirectory(prefix='nuwax-r213-') as cwd:
        result=subprocess.run([str(exe)],cwd=cwd,text=True,capture_output=True,check=True,timeout=30)
    (out/(name+'.log')).write_text(result.stdout,encoding='utf-8');print(result.stdout)

core=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "sd_audio_integrity.h"
static uint32_t crc(uint32_t c,const void*v,size_t n){const uint8_t*p=v;c=~c;while(n--){c^=*p++;for(unsigned b=0;b<8;b++)c=(c>>1)^((c&1)?0xedb88320U:0);}return ~c;}
static unsigned calls,stop_at;
static bool yield_requested(void *ctx){assert(ctx==&calls);return ++calls>=stop_at;}
int main(void){uint8_t header[96]={0},frame[662]={0};sd_integrity_t*s=sd_integrity_create(crc);assert(s);
 assert(sd_integrity_begin(s,"meeting","part",1,1,header,96));for(unsigned n=0;n<100;n++)sd_integrity_record(s,662,frame,0,false,0);
 sd_integrity_freeze(s,3,10,s->shadow_crc,0);char path[192];stop_at=1;
 assert(sd_integrity_save_cooperative(s,"before",path,sizeof(path),yield_requested,&calls)==ESP_ERR_NOT_FINISHED&&!s->saved&&!strcmp(s->save_stage,"admission"));
 struct stat st;assert(stat("before",&st)!=0);calls=0;stop_at=5;
 assert(sd_integrity_save_cooperative(s,"evidence",path,sizeof(path),yield_requested,&calls)==ESP_ERR_NOT_FINISHED&&!s->saved&&!strcmp(s->save_stage,"yield"));
 assert(!rename("evidence/evidence_0.part","evidence/retained.part"));
 assert(sd_integrity_save(s,"evidence",path,sizeof(path))==ESP_OK&&s->saved&&!strcmp(s->save_stage,"saved"));
 puts("PASS production cooperative export: early yield does no filesystem work; mid-write yield closes FILE normally; retained frozen evidence saves later");
 sd_integrity_destroy(s);s=sd_integrity_create(crc);assert(sd_integrity_begin(s,"bad","part",1,1,header,96));sd_integrity_freeze(s,3,1,0,0);
 assert(sd_integrity_save(s,"missing/parent/evidence",path,sizeof(path))==ESP_FAIL&&s->save_errno&&!strcmp(s->save_stage,"mkdir"));
 puts("PASS production export: real filesystem failure keeps exact stage/errno and frozen state");sd_integrity_destroy(s);return 0;}
'''
run('cooperative',core,[root/'main/sd_audio_integrity.c'])

main=(root/'main/nuwax_ai_partner_main.c').read_text(encoding='utf-8-sig')
shell=(root/'main/app_shell.c').read_text(encoding='utf-8-sig')
drawer=(root/'main/nuwax_ui_v28/nuwax_drawer.c').read_text(encoding='utf-8-sig')
callback=function(main[main.index('static void meeting_state_changed(void *ctx,',main.index('static void meeting_state_changed(void *ctx,')+1):], 'static void meeting_state_changed(')
notify=function(shell,'void nuwax_app_shell_notify_action_rejected(')
reject=function(drawer,'void nuwax_drawer_reject_action(')
fixture=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define CONFIG_NUWAX_OFFLINE_AUDIO_ENABLE 0
#define NUWAX_TEST_NETWORK_ENABLED 0
#define TAG "host"
static void log_ignore(const char *tag,const char *format,...){(void)tag;(void)format;}
#define ESP_LOGI(...) log_ignore(__VA_ARGS__)
#define ESP_LOGW(...) log_ignore(__VA_ARGS__)
enum {NUWAX_MEETING_STATE_DISABLED,NUWAX_MEETING_STATE_STARTING,NUWAX_MEETING_STATE_ACTIVE,NUWAX_MEETING_STATE_STOPPING,NUWAX_MEETING_STATE_ERROR};
enum{NUWAX_MONITORING_STATE_DISABLED};
enum{NUWAX_APP_STATE_LISTENING,NUWAX_APP_STATE_ERROR};
typedef int nuwax_meeting_state_t;
typedef int nuwax_app_shell_action_t;
typedef int nuwax_drawer_action_t;
enum{NUWAX_APP_SHELL_ACTION_SET_MEETING_MODE=1};
enum{NUWAX_DRAWER_ACTION_CYCLE_BRIGHTNESS,NUWAX_DRAWER_ACTION_SET_VOLUME,NUWAX_DRAWER_ACTION_SET_MEETING_MODE,NUWAX_DRAWER_ACTION_REPROVISION_WIFI};
typedef struct {int unused;}nuwax_ws_client_t;
static unsigned s_drawer_action_reject_mask,s_ui_mailbox_mux;
#define taskENTER_CRITICAL(p) ((void)(p))
#define taskEXIT_CRITICAL(p) ((void)(p))
static uint32_t app_shell_action_bit(int action){return 1U<<action;}
static bool s_exclusive_audio_sleep_reconcile_pending;
static struct{bool capture_active,pending;}s_prelisten;
static int s_monitoring_mode;
static void begin_exclusive_audio_takeover(const char *p){(void)p;}
static void clear_awake_session(const char*p){(void)p;}
static void stop_sleep_wake_word_detector(const char*p){(void)p;}
static void cancel_auto_followup_capture(const char*p){(void)p;}
static void clear_auto_followup(const char*p){(void)p;}
static void discard_session_queue_at_audio_mode_boundary(const char*p){(void)p;}
static void set_state(int state){(void)state;}
static nuwax_ws_client_t*active_ws_snapshot(void*p){(void)p;return NULL;}
static int nuwax_monitoring_mode_state(void*p){(void)p;return 0;}
static bool nuwax_monitoring_mode_has_pending(void*p){(void)p;return false;}
static bool nuwax_ws_is_connected(void*p){(void)p;return false;}
static void service_exclusive_audio_sleep_reconcile(void){}
static int s_brightness_visual;
static bool s_have_state,s_meeting_pending,s_provision_pending,s_open,s_closing;
static int64_t s_pending_deadline_us,s_provision_pending_deadline_us;
static struct {unsigned volume;}s_state;
static void set_volume_visual(unsigned v){(void)v;}
static void render_menu(void){}
'''+notify+callback+reject+r'''
int main(void){s_meeting_pending=true;s_pending_deadline_us=6000000;
 meeting_state_changed(NULL,NUWAX_MEETING_STATE_ERROR,"sd_capture_ended");
 meeting_state_changed(NULL,NUWAX_MEETING_STATE_DISABLED,"capture_error_reset");
 assert(s_drawer_action_reject_mask&app_shell_action_bit(NUWAX_APP_SHELL_ACTION_SET_MEETING_MODE));
 unsigned delivered=s_drawer_action_reject_mask;s_drawer_action_reject_mask=0;
 if(delivered&app_shell_action_bit(NUWAX_APP_SHELL_ACTION_SET_MEETING_MODE))nuwax_drawer_reject_action(NUWAX_DRAWER_ACTION_SET_MEETING_MODE);
 assert(!s_meeting_pending&&!s_pending_deadline_us);
 s_meeting_pending=true;s_pending_deadline_us=9000000;
 assert(!s_drawer_action_reject_mask);meeting_state_changed(NULL,NUWAX_MEETING_STATE_STARTING,"local_sd_enable");
 assert(s_meeting_pending&&!s_drawer_action_reject_mask);
 puts("PASS actual main callback + mailbox + drawer: ERROR/DISABLED coalescing retains terminal failure; next touch starts with drained result and no stale ERROR state");return 0;}
'''
run('ui',fixture)
spool=(root/'main/offline_audio_spool.c').read_text(encoding='utf-8-sig')
recovery=function(spool,'esp_err_t nuwax_offline_audio_recover_if_idle(')
assert 'writer_integrity_save_if_idle' not in recovery and 'writer_integrity_service' not in recovery
supervisor=function(spool,'static void offline_storage_supervisor_task(')
assert 'if (err == ESP_OK) writer_integrity_service();' in supervisor
prepare=function(spool,'esp_err_t nuwax_sd_session_prepare_start(')
assert prepare.index('sd_capture_admission_hold(')<prepare.index('xSemaphoreTake(s.admission_lock')
hooks=(root/'main/sd_audio_integrity_writer.inc').read_text(encoding='utf-8')
assert 'sd_capture_admission_held(&s_capture_admission, offline_now_ms())' in function(hooks,'static void writer_integrity_save_if_idle(')
assert 'nuwax_sd_integrity_io_budget_begin(750U)' not in hooks
assert shell.index('nuwax_drawer_reject_action(NUWAX_DRAWER_ACTION_SET_MEETING_MODE);') < shell.index('const int64_t touch_drain_started_us =')
print('PASS production ownership fences: start/recovery never exports; supervisor-only export respects pending start; LVGL consumes rejection before new touch actions')
intent=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_err.h"
#include "sd_capture_admission.h"
#define CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE 0
#define pdTRUE 1
typedef int nuwax_offline_audio_mode_t;
static sd_capture_admission_t s_capture_admission;
static struct{int admission_lock;}s;
static int64_t clock_us=1000000;
static int64_t esp_timer_get_time(void){return clock_us;}
static uint32_t offline_now_ms(void){return (uint32_t)(clock_us/1000);}
static bool runtime_initialized(void){return true;}
static bool nuwax_offline_audio_replay_running(void){return false;}
static bool nuwax_offline_audio_storage_busy(void){return false;}
static int xSemaphoreTake(int lock,unsigned wait){(void)lock;(void)wait;return 0;}
static void xSemaphoreGive(int lock){(void)lock;}
'''+prepare+function(hooks,'static bool writer_integrity_export_yield(')+r'''
int main(void){int64_t started=clock_us;
 assert(nuwax_sd_session_prepare_start(0,0)==ESP_ERR_NOT_FINISHED);
 assert(sd_capture_admission_held(&s_capture_admission,offline_now_ms())&&writer_integrity_export_yield(&started));
 sd_capture_admission_release(&s_capture_admission,0);assert(!writer_integrity_export_yield(&started));
 assert(nuwax_sd_session_prepare_start(2,0)==ESP_ERR_INVALID_ARG&&!sd_capture_admission_held(&s_capture_admission,offline_now_ms()));
 assert(nuwax_sd_session_prepare_start(0,clock_us)==ESP_ERR_TIMEOUT&&!sd_capture_admission_held(&s_capture_admission,offline_now_ms()));
 puts("PASS actual start admission: busy gate still publishes intent; exporter yields; release, invalid mode and expired deadline cannot leave a stale reservation");return 0;}
'''
run('intent',intent)
classification=r'''
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "sd_audio_integrity.h"
'''+function(hooks,'static bool writer_integrity_export_media_fault(')+r'''
int main(void){sd_integrity_t d={0};d.save_stage="mkdir";
 int local[]={EACCES,ENOTDIR,ENOSPC,ENOENT,EEXIST,ENOMEM};
 for(unsigned i=0;i<sizeof(local)/sizeof(local[0]);i++){d.save_errno=local[i];assert(!writer_integrity_export_media_fault(&d,ESP_FAIL));}
 int physical[]={EIO,ENODEV,ETIMEDOUT,EILSEQ};for(unsigned i=0;i<sizeof(physical)/sizeof(physical[0]);i++){d.save_errno=physical[i];assert(writer_integrity_export_media_fault(&d,ESP_FAIL));}
 d.save_errno=0;d.save_stage="close";assert(writer_integrity_export_media_fault(&d,ESP_FAIL));
 assert(!writer_integrity_export_media_fault(&d,ESP_ERR_NOT_FINISHED));
 puts("PASS production export classification: namespace/space/permission/policy cannot poison audio admission; real I/O or uncertain close still require recovery");return 0;}
'''
run('classification',classification)
close_failure=r'''
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
static int close_fails;
static FILE *fail_fdopen(int fd,const char *mode){(void)fd;(void)mode;errno=ENOMEM;return NULL;}
static int report_close(int fd){int result=close(fd);assert(!result);if(close_fails){errno=EIO;return -1;}return 0;}
#define fdopen fail_fdopen
#define close report_close
#include "sd_audio_integrity.c"
#undef close
#undef fdopen
static uint32_t crc(uint32_t seed,const void*data,size_t n){(void)seed;(void)data;(void)n;return 0;}
int main(void){uint8_t header[96]={0};sd_integrity_t*d=sd_integrity_create(crc);assert(sd_integrity_begin(d,"test","part",1,1,header,96));sd_integrity_freeze(d,3,1,0,0);
 close_fails=1;assert(sd_integrity_save(d,"evidence",NULL,0)==ESP_FAIL&&!strcmp(d->save_stage,"close")&&d->save_errno==EIO);
 close_fails=0;assert(sd_integrity_save(d,"evidence",NULL,0)==ESP_FAIL&&!strcmp(d->save_stage,"fdopen")&&d->save_errno==ENOMEM);
 puts("PASS production cleanup: fdopen allocation failure closes descriptor; an uncertain close supersedes benign allocation error");sd_integrity_destroy(d);return 0;}
'''
run('close_failure',close_failure)
