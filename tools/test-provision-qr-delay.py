"""Execute production QR scheduling and polling deadlines with a simulated clock."""
from pathlib import Path
import re, subprocess
r=Path(__file__).resolve().parents[1];s=(r/'main/app_shell.c').read_text(encoding='utf-8-sig')
def block(start):
 a=s.index(start);b=s.index('{',a)+1;depth=1
 while depth:depth+=(s[b]=='{')-(s[b]=='}');b+=1
 return s[a:b]
fn=block('void nuwax_app_shell_schedule_provisioning_qr(uint32_t delay_ms)')
poll=block('    if (s_pending_provisioning_qr &&\n')
defines='\n'.join(re.findall(r'^#define APP_SHELL_PROVISIONING_(?:QR_RETRY|FACE_HOLD)_MS .*$',s,re.M))
pre=r'''
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
static bool s_ui_ready=true,s_pending_provisioning_qr;
static int64_t s_pending_provisioning_qr_deadline_us,clock_us;
static struct {bool initialized;struct {bool provisioning_active;} snapshot;} s_state_mailbox;
#define taskENTER_CRITICAL(x) ((void)0)
#define taskEXIT_CRITICAL(x) ((void)0)
static void app_shell_ensure_state_snapshot_locked(void){s_state_mailbox.initialized=true;}
static int64_t esp_timer_get_time(void){return clock_us;}
static void nuwax_lvgl_port_wake(void){}
'''
checks=r'''
int main(void){
 clock_us=100000;s_state_mailbox.snapshot.provisioning_active=true;
 nuwax_app_shell_schedule_provisioning_qr(0);assert(s_pending_provisioning_qr_deadline_us==3100000);
 clock_us=3099999;assert(!check());clock_us=3100000;assert(check());
 assert(s_pending_provisioning_qr_deadline_us==3600000);
 clock_us=3599999;assert(!check());clock_us=3600000;assert(check());
 /* A failed view admission retries in 500 ms, without restarting the hold. */
 assert(s_pending_provisioning_qr_deadline_us==4100000);
 nuwax_app_shell_schedule_provisioning_qr(5000);assert(s_pending_provisioning_qr_deadline_us==8600000);
 s_state_mailbox.snapshot.provisioning_active=false;clock_us=8600000;
 assert(!check()&&!s_pending_provisioning_qr&&!s_pending_provisioning_qr_deadline_us);
 nuwax_app_shell_schedule_provisioning_qr(0);assert(!s_pending_provisioning_qr);
 s_state_mailbox.snapshot.provisioning_active=true;s_ui_ready=false;
 nuwax_app_shell_schedule_provisioning_qr(0);assert(!s_pending_provisioning_qr);
 puts("PASS: 3000 ms minimum hold, exact deadline, 500 ms readiness retry, longer requested hold, inactive/unready rejection");
}
'''
c=r/'build-host-checks/qr-reveal-delay/check.c';c.parent.mkdir(parents=True,exist_ok=True)
c.write_text(pre+defines+'\n'+fn+'\nstatic bool check(void){bool do_provisioning_qr=false;int64_t provisioning_qr_retry_deadline_us=0;const int64_t now_us=clock_us;\n'+poll+'\nreturn do_provisioning_qr;}\n'+checks,encoding='utf-8')
exe=c.with_suffix('.exe');subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror',str(c),'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
