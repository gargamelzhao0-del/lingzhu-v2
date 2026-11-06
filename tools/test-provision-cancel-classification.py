"""Execute the boot provisioning outcome branch, including cancellation rollback."""
from pathlib import Path
import subprocess
r=Path(__file__).resolve().parents[1];s=(r/'main/nuwax_ai_partner_main.c').read_text(encoding='utf-8')
a=s.index('            const bool provision_completed =',s.index('nuwa'+'x_provision_main_run(300000)',s.index('void app_main(')))
b=s.index('\n        }\n    } else {',a)
branch=s[a:b]
# The owner must finish saving before publishing its park acknowledgement.
a=s.index('const esp_err_t park_close_err = nuwax_ws_owner_close(&ws);')
assert a < s.index('nuwax_display_brightness_save(true);',a) < s.index('atomic_store_explicit(&s_cloud_owner_parked, true,',a)
pre=r'''
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#define ESP_OK 0
#define ESP_FAIL -1
#define NUWAX_PROVISION_OUTCOME_SUCCESS 1
#define NUWAX_PROVISION_OUTCOME_OLD_WIFI_RECOVERED 2
#define NUWAX_PROVISION_OUTCOME_STOPPED 6
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
typedef int esp_err_t;
static int reports,stops,rollbacks,restarts,rollback_error;
static bool active;
static void nuwax_device_error_report_enqueue(const char*a,const char*b,const char*c,int d,const char*e,unsigned f,void*g){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;++reports;}
static void nuwax_provision_main_stop(void){++stops;}
static void nuwax_app_shell_set_provisioning_active(bool a){active=a;}
static void nuwax_face_clear_provisioning_boot_visual(void){}
static int rollback_wifi_credentials_guarded(void){++rollbacks;return rollback_error;}
static void boot_fail_controlled_restart(const char*s){(void)s;++restarts;}
static void run(int provision_outcome,bool reversible_reprovision){
'''
post=r'''
}
int main(void){
 active=true;run(6,true);assert(!reports&&rollbacks==1&&!restarts&&!active);
 run(3,true);assert(reports==1&&rollbacks==2&&!restarts);
 rollback_error=-1;run(6,true);assert(reports==1&&rollbacks==3&&restarts==1);
 rollback_error=0;run(1,true);run(2,true);assert(reports==1&&rollbacks==3&&restarts==1);
 run(3,false);assert(reports==2&&restarts==2&&rollbacks==3&&stops==6);
 puts("PASS: cancel restores without false report; actual failures reported; rollback failure restarts; success unchanged; brightness saved before handoff");
}
'''
out=r/'build-host-checks/provision-cancel';out.mkdir(parents=True,exist_ok=True)
c=out/'check.c';c.write_text(pre+branch+post,encoding='utf-8');exe=out/'check.exe'
subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror',str(c),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
