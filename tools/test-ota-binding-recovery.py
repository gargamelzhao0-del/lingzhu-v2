"""Run the production pending-OTA/rebind decisions with persistence and race faults."""
from pathlib import Path
import re,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'main/nuwax_ota_new/nuwax_ota_device.c').read_text(encoding='utf-8-sig')
def function(signature):
    start=s.rindex(signature);end=s.index('{',start)+1;depth=1
    while depth:
        depth+=(s[end]=='{')-(s[end]=='}');end+=1
    return s[start:end]
enum=re.search(r'typedef enum \{\s*OTA_BOOT_DECISION_NONE.*?\} ota_boot_decision_owner_t;',s,re.S).group()
code=r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <setjmp.h>
#include <stdlib.h>
/* Expected-negative mutations must exit in unattended Windows runs instead
 * of entering a CRT assertion dialog and timing out during temp cleanup. */
#undef assert
#define assert(condition) do { if (!(condition)) { fprintf(stderr,"assertion failed: %s at %d\n",#condition,__LINE__); exit(86); } } while(0)
typedef int esp_err_t;
typedef void *TaskHandle_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 0x103
#define OTA_NOTIFY_BINDING_RECOVERY 4U
#define eSetBits 1
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define portENTER_CRITICAL(p) do { assert(!locked); locked=true; } while(0)
#define portEXIT_CRITICAL(p) do { assert(locked); locked=false; } while(0)
static bool locked, durable, fail_save, rollback_returns, allow_rollback;
static unsigned saves, rollbacks, notifications;
static uint32_t now;
static jmp_buf reboot;
static uint32_t monotonic_ms(void){return now;}
static void vTaskDelay(unsigned ticks){assert(!locked);now+=ticks;}
#define pdMS_TO_TICKS(x) (x)
static int xTaskNotify(TaskHandle_t task,unsigned bits,int action){assert(task && bits==4 && action==1 && !locked);notifications++;return 1;}
static int prepare(void *ctx){(void)ctx;assert(!locked);saves++;if(fail_save)return ESP_FAIL;durable=true;return ESP_OK;}
static int nuwax_ota_main_rollback_running_image_and_reboot(void){assert(!locked && durable && allow_rollback);rollbacks++;if(rollback_returns)return ESP_FAIL;longjmp(reboot,1);}
static TaskHandle_t s_controller_task=(void*)1;
static bool s_boot_pending_verify,s_boot_binding_recovery_requested,s_cloud_authenticated,s_command_pending,s_active;
static uint32_t s_boot_binding_recovery_retry_ms,s_boot_validation_deadline_ms;
static bool s_cloud_ready_event_pending;
static uint32_t s_cloud_ready_event_ms;
static struct {void *ctx;esp_err_t (*prepare_reprovision_boot)(void *);}s_callbacks={NULL,prepare};
"""+enum+'\nstatic ota_boot_decision_owner_t s_boot_decision_owner;\n'
for sig in ['static bool deadline_reached(', 'static bool claim_boot_timeout_decision(',
            'static esp_err_t execute_boot_rollback(', 'static void process_boot_binding_recovery(',
            'bool nuwax_ota_device_request_binding_recovery(']:code+='\n'+function(sig)+'\n'
code+=r"""
static void fresh(void){locked=durable=fail_save=rollback_returns=allow_rollback=false;saves=rollbacks=notifications=0;now=100;
 s_boot_pending_verify=true;s_boot_binding_recovery_requested=false;s_cloud_authenticated=s_command_pending=s_active=false;
 s_boot_binding_recovery_retry_ms=0;s_boot_validation_deadline_ms=120100;
 s_boot_decision_owner=OTA_BOOT_DECISION_NONE;s_cloud_ready_event_pending=true;s_callbacks.prepare_reprovision_boot=prepare;
}
int main(void){
 fresh();process_boot_binding_recovery();assert(!saves&&!rollbacks); /* ordinary loss has no request */
 s_cloud_authenticated=true;assert(!nuwax_ota_device_request_binding_recovery());
 s_cloud_authenticated=false;s_active=true;assert(!nuwax_ota_device_request_binding_recovery());
 s_active=false;s_command_pending=true;assert(!nuwax_ota_device_request_binding_recovery());
 s_command_pending=false;assert(nuwax_ota_device_request_binding_recovery());assert(notifications==1&&!saves&&!rollbacks);
 allow_rollback=true;
 if(!setjmp(reboot)){process_boot_binding_recovery();assert(!"reboot expected");}
 assert(saves==1&&rollbacks==1&&durable&&!s_cloud_ready_event_pending&&s_boot_decision_owner==OTA_BOOT_DECISION_REPROVISION);
 process_boot_binding_recovery();assert(rollbacks==1); /* only one decision owner */
 fresh();s_boot_decision_owner=OTA_BOOT_DECISION_CLOUD_READY;
 assert(nuwax_ota_device_request_binding_recovery());process_boot_binding_recovery();assert(!saves);
 s_boot_pending_verify=false;s_boot_decision_owner=OTA_BOOT_DECISION_COMPLETE;
 process_boot_binding_recovery();assert(!saves&&!rollbacks&&!s_boot_binding_recovery_requested);
 assert(!nuwax_ota_device_request_binding_recovery());
 fresh();fail_save=true;assert(nuwax_ota_device_request_binding_recovery());
 process_boot_binding_recovery();assert(saves==1&&!rollbacks&&!durable&&s_boot_decision_owner==OTA_BOOT_DECISION_NONE);
 assert(s_boot_validation_deadline_ms==120100);now=500;process_boot_binding_recovery();assert(saves==1);
 now=1100;process_boot_binding_recovery();assert(saves==2&&s_boot_validation_deadline_ms==120100);
 now=120100;assert(claim_boot_timeout_decision(now));process_boot_binding_recovery();assert(saves==2&&!rollbacks);
 fresh();rollback_returns=allow_rollback=true;assert(nuwax_ota_device_request_binding_recovery());process_boot_binding_recovery();
 assert(durable&&rollbacks==1&&s_boot_decision_owner==OTA_BOOT_DECISION_NONE);
 process_boot_binding_recovery();assert(rollbacks==1);
 fresh();s_callbacks.prepare_reprovision_boot=NULL;assert(!nuwax_ota_device_request_binding_recovery());
 puts("PASS: pending/revoked-binding recovery only; durable save before rollback; cloud-confirm/timeout races; NVS failure retains deadline; bounded retries; no RX work");
}
"""
with tempfile.TemporaryDirectory(prefix='nuwax-ota-binding-') as folder:
 for name,src,negative in [('actual',code,False),('unsafe-persist',code.replace('if (err == ESP_OK) {','if (err == ESP_OK || err == ESP_FAIL) {',1),True),
                           ('unsafe-owner',code.replace(function('static void process_boot_binding_recovery('),function('static void process_boot_binding_recovery(').replace('s_boot_decision_owner == OTA_BOOT_DECISION_NONE &&','true &&')),True)]:
    path=Path(folder)/(name+'.c');exe=path.with_suffix('.exe');path.write_text(src,encoding='utf-8')
    subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-parameter',str(path),'-o',str(exe)],check=True)
    r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=10)
    if negative:assert r.returncode!=0,name;print('PASS: rejected '+name)
    else:print(r.stdout,end='');assert r.returncode==0,r.stdout+r.stderr
