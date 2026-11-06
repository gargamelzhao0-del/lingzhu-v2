"""Exercise the production brightness owner with PWM and NVS fault injection."""
from pathlib import Path
import subprocess
root=Path(__file__).resolve().parents[1]
out=root/'build-host-checks/display-brightness';out.mkdir(parents=True,exist_ok=True)
headers={
 'esp_err.h': '#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_FAIL -1\n#define ESP_ERR_INVALID_ARG 258\n#define ESP_ERR_NVS_NOT_ENOUGH_SPACE 4357\nconst char *esp_err_to_name(int e);\n',
 'esp_timer.h': '#pragma once\n#include <stdint.h>\nint64_t esp_timer_get_time(void);\n',
 'esp_log.h': '#pragma once\n#define ESP_LOGW(tag,...) ((void)(tag))\n',
 'nvs.h': '#pragma once\n#include <stdint.h>\n#include "esp_err.h"\ntypedef int nvs_handle_t;\n#define NVS_READONLY 0\n#define NVS_READWRITE 1\nint nvs_open(const char*,int,nvs_handle_t*);\nint nvs_get_u8(nvs_handle_t,const char*,uint8_t*);\nint nvs_set_u8(nvs_handle_t,const char*,uint8_t);\nint nvs_commit(nvs_handle_t);\nvoid nvs_close(nvs_handle_t);\n'
}
for name,text in headers.items():(out/name).write_text(text,encoding='utf-8')
harness=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "display_brightness.h"
static int64_t time_us;
static int pwm_error,open_error,commit_error,get_error,set_error,guard_error,release_error;
static unsigned guards,ends,releases;
static bool guarded;
static unsigned pwm_calls,writes,commits,closes;
static uint8_t saved=100,staged,pwm;
int64_t esp_timer_get_time(void){return time_us;}
const char *esp_err_to_name(int e){(void)e;return "fault";}
int nuwax_display_backlight_set_percent(uint8_t v){++pwm_calls;if(pwm_error)return -1;pwm=v;return 0;}
int nuwax_wake_word_detector_begin_flash_operation(uint32_t ms){assert(ms==500&&!guarded);++guards;if(guard_error)return guard_error;guarded=true;return 0;}
void nuwax_wake_word_detector_end_flash_operation(void){assert(guarded);guarded=false;++ends;}
int nuwax_device_error_report_release_persisted_storage(void){assert(!guarded);++releases;if(!release_error)set_error=0;return release_error;}
int nvs_open(const char*n,int mode,int*h){if(mode==1)assert(guarded);assert(!strcmp(n,"display_ui"));(void)mode;*h=1;return open_error;}
int nvs_get_u8(int h,const char*k,uint8_t*v){assert(h==1&&!strcmp(k,"brightness"));*v=saved;return get_error;}
int nvs_set_u8(int h,const char*k,uint8_t v){assert(h==1&&!strcmp(k,"brightness"));++writes;staged=v;return set_error;}
int nvs_commit(int h){assert(h==1);++commits;if(commit_error)return commit_error;saved=staged;return 0;}
void nvs_close(int h){assert(h==1);++closes;}
int main(void){
 open_error=-1;nuwax_display_brightness_load();assert(nuwax_display_brightness_percent()==100);
 open_error=0;saved=1;nuwax_display_brightness_load();assert(nuwax_display_brightness_percent()==100);
 saved=70;nuwax_display_brightness_load();assert(nuwax_display_brightness_percent()==70);
 assert(nuwax_display_brightness_apply(0)!=0&&nuwax_display_brightness_apply(255)!=0&&pwm_calls==0);
 assert(nuwax_display_brightness_apply(70)==0&&pwm_calls==0);
 pwm_error=-1;assert(nuwax_display_brightness_apply(40)!=0&&nuwax_display_brightness_percent()==70);
 pwm_error=0;assert(nuwax_display_brightness_apply(40)==0&&pwm==40);
 nuwax_display_brightness_save(false);assert(!writes);
 time_us=500000;assert(nuwax_display_brightness_apply(100)==0);
 time_us=1000001;nuwax_display_brightness_save(false);assert(!writes);
 time_us=1500001;commit_error=-1;nuwax_display_brightness_save(false);assert(writes==1&&saved==70);
 time_us=2000000;nuwax_display_brightness_save(false);assert(writes==1);
 time_us=6500002;commit_error=0;nuwax_display_brightness_save(false);assert(writes==2&&saved==100);
 nuwax_display_brightness_load();assert(nuwax_display_brightness_percent()==100);
 assert(nuwax_display_brightness_apply(40)==0);nuwax_display_brightness_save(true);assert(saved==40);
 nuwax_display_brightness_load();assert(nuwax_display_brightness_percent()==40);
 assert(nuwax_display_brightness_apply(70)==0);assert(nuwax_display_brightness_apply(40)==0);
 unsigned before=writes;nuwax_display_brightness_save(true);assert(writes==before);
 assert(nuwax_display_brightness_apply(70)==0);
 guard_error=-1;before=writes;unsigned old_ends=ends;
 nuwax_display_brightness_save(true);assert(writes==before&&ends==old_ends&&!guarded);
 guard_error=0;nuwax_display_brightness_save(false);assert(writes==before);
 time_us+=5000001;nuwax_display_brightness_save(false);assert(saved==70);
 assert(nuwax_display_brightness_apply(100)==0);set_error=4357;
 nuwax_display_brightness_save(true);assert(saved==100&&releases==1&&!guarded);
 assert(nuwax_display_brightness_apply(40)==0);set_error=4357;release_error=-1;
 before=writes;nuwax_display_brightness_save(true);assert(saved==100&&writes==before+1&&releases==2);
 release_error=0;set_error=0;nuwax_display_brightness_save(true);assert(saved==40);
 assert(nuwax_display_brightness_apply(70)==0);commit_error=4357;before=writes;
 nuwax_display_brightness_save(true);assert(writes==before+2&&saved==40&&releases==3);
 commit_error=0;nuwax_display_brightness_save(true);assert(saved==70&&!guarded&&guards==ends+1);
 assert(nuwax_display_brightness_apply(100)==0);commit_error=-1;
 nuwax_display_brightness_save(true);assert(saved==70);
 assert(nuwax_display_brightness_apply(70)==0);commit_error=0;before=writes;
 nuwax_display_brightness_save(true);assert(writes==before+1&&saved==70);
 get_error=-1;nuwax_display_brightness_load();assert(nuwax_display_brightness_percent()==100);
 puts("PASS: brightness valid levels, immediate PWM, default/corrupt/read-error boot, debounce, PWM failure rollback, commit retry, reboot restore, shutdown flush, return-to-saved no-write");
}
'''
c=out/'test.c';c.write_text(harness,encoding='utf-8');exe=out/'test.exe'
subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(out),'-I'+str(root/'main'),str(c),str(root/'main/display_brightness.c'),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
