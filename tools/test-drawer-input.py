"""Replay real touch gestures against user-visible click, slider and swipe contracts."""
from pathlib import Path
import hashlib
import json
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build-host-checks/drawer-input'
OUT.mkdir(parents=True, exist_ok=True)
FILES = ['main/touch_input.c', 'main/touch_input.h', 'main/touch_read_policy.h',
         'main/conversation/touch_gesture.c', 'main/app_shell.c',
         'main/nuwax_ui_v28/nuwax_drawer.c', 'main/nuwax_ui_v28/control_center_timing_policy.h',
         'main/nuwax_lvgl_port.c', 'main/conversation/speaker_playback.c', 'sdkconfig']
hashes = {p: hashlib.sha256((ROOT / p).read_bytes()).hexdigest() for p in FILES}
gesture = (ROOT / FILES[3]).read_text(encoding='utf-8-sig')
drawer = (ROOT / FILES[5]).read_text(encoding='utf-8-sig')
shell_header = (ROOT / 'main/app_shell.h').read_text(encoding='utf-8-sig')


def function(text, signature):
    start = text.index(signature)
    while ';' in text[start:text.index('{', start)]:
        start = text.index(signature, start + len(signature))
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


events = shell_header[shell_header.index('typedef enum {'):shell_header.index('typedef void (*nuwax_app_shell_reprovision_cb_t)')]
prefix = r'''
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <setjmp.h>
#include <stdlib.h>
#include "control_center_input_policy.h"
typedef void *QueueHandle_t;
typedef int esp_err_t;
#define ESP_OK 0
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
typedef struct {bool touched,cancelled;uint8_t gesture,points,event;uint16_t x,y;uint32_t contact_id,sample_ms;}nuwax_touch_point_t;
'''
prefix += events
prefix += r'''
typedef struct {int ms;bool down;uint16_t x,y;uint32_t id;bool timeout,cancelled;}sample_t;
static sample_t input[32];static unsigned input_count,input_at;
static jmp_buf done;static int64_t now_us;
static unsigned taps,longs,page_drags,panel_drags,volume_previews,volume_commits;
static int last_volume,last_page_delta;static unsigned volume_finishes,cancels,panel_finishes;
static bool last_page_commit,last_panel_close,panel_pull,drawer_open=true;
static int16_t panel_anchor=48;
static int64_t esp_timer_get_time(void){return now_us;}
static void vTaskDelay(unsigned ms){now_us+=(int64_t)ms*1000;}
static esp_err_t nuwax_touch_poll_event(nuwax_touch_point_t *p,unsigned ms){
 (void)ms;if(input_at==input_count)longjmp(done,1);
 sample_t f=input[input_at++];now_us=(1000LL+f.ms)*1000;
 *p=(nuwax_touch_point_t){.touched=f.down,.x=f.x,.y=f.y,.contact_id=f.id,.sample_ms=(uint32_t)(now_us/1000),.cancelled=f.cancelled};return f.timeout?1:0;
}
static void nuwax_app_shell_drawer_contact(bool d,uint16_t x,uint16_t y){(void)d;(void)x;(void)y;}
static void nuwax_app_shell_notify_action_rejected(int a){(void)a;}
static bool nuwax_app_shell_drawer_is_open(void){return drawer_open;}
static bool nuwax_app_shell_is_menu_active(void){return drawer_open;}
static bool nuwax_app_shell_can_start_listen(void){return false;}
static bool nuwax_app_shell_focus_menu_main_is_open(void){return false;}
static bool nuwax_app_shell_focus_menu_detail_is_open(void){return false;}
static void nuwax_app_shell_touch_begin(void){}
static void nuwax_app_shell_action_queued(void){}
static bool nuwax_app_shell_drawer_can_page_drag(int16_t dx){return dx>0;}
static void nuwax_app_shell_drawer_finish_volume_drag(int v){
 volume_finishes++;if(v<0)cancels++;else{volume_commits++;last_volume=v;}
}
static void nuwax_app_shell_drawer_drag_by(int16_t delta_y,bool pull){
 (void)delta_y;panel_drags++;panel_pull=pull;
}
static void nuwax_app_shell_drawer_finish_drag(int16_t delta_y,int16_t dy,int16_t velocity,bool canceled){
 last_panel_close=canceled?panel_pull:nuwax_cc_drawer_release_close(
    panel_pull,dy,velocity,(panel_pull?-226:panel_anchor)+delta_y);
 panel_finishes++;
}
static void nuwax_app_shell_drawer_page_drag_to(int16_t dx){last_page_delta=dx;page_drags++;}
static void nuwax_app_shell_drawer_page_finish_drag(bool commit){last_page_commit=commit;}
static void nuwax_app_shell_menu_drag_to(int16_t y){(void)y;}
static void nuwax_app_shell_menu_finish_drag(void){}
static void nuwax_app_shell_menu_detail_adjust(int dir){(void)dir;}
bool nuwax_drawer_volume_value_at(uint16_t x,uint16_t y,bool captured,uint8_t *v);
static bool nuwax_app_shell_drawer_preview_volume_drag_at(uint16_t x,uint16_t y,bool captured,uint8_t *v){
 uint8_t value;
 if(!nuwax_drawer_volume_value_at(x,y,captured,&value))return false;
 if(v)*v=value;
 volume_previews++;return true;
}
static nuwax_app_shell_result_t nuwax_app_shell_handle_touch_event(nuwax_app_shell_event_t e,const nuwax_touch_point_t *p){
 (void)p;if(e==NUWAX_APP_SHELL_EVENT_TAP)taps++;if(e==NUWAX_APP_SHELL_EVENT_LONG_PRESS)longs++;
 return (nuwax_app_shell_result_t){0};
}
static int xQueueSend(QueueHandle_t q,const void *ptr,unsigned ticks){
 (void)q;(void)ticks;const nuwax_app_shell_result_t *r=ptr;
 assert(r->action!=NUWAX_APP_SHELL_ACTION_SET_VOLUME);
 return pdTRUE;
}
'''
gesture_code = '\n'.join(line for line in gesture.splitlines() if not line.startswith('#include'))
gesture_macros = '\n'.join(re.findall(r'^#define NUWAX_GESTURE_.*$', (ROOT / 'main/nuwax_ui_v28/nuwax_gesture.h').read_text(), re.M))
volume_boundary = r'''
static bool s_open=true,s_page_transition_active,s_sheet_active;
enum{CC_DETAIL_NONE,CC_PAGE_MENU};static int s_detail=CC_DETAIL_NONE,s_page=CC_PAGE_MENU;
static bool reveal_owns_motion(void){return false;}
'''
volume_boundary += '\n'.join(re.search(r'^#define ' + n + r' .*$', drawer, re.M)[0]
                             for n in ['CC_VOLUME_BAR_X', 'CC_VOLUME_BAR_Y', 'CC_VOLUME_BAR_W']) + '\n'
volume_boundary += function(drawer, 'static bool point_in_rect(') + '\n'
volume_boundary += function(drawer, 'bool nuwax_drawer_volume_value_at(') + '\n'
tests = r'''
static void replay(sample_t *samples,unsigned n){
 memcpy(input,samples,n*sizeof(*samples));input_count=n;input_at=0;
 taps=longs=page_drags=panel_drags=volume_previews=volume_commits=volume_finishes=cancels=panel_finishes=0;
 last_volume=last_page_delta=0;last_page_commit=last_panel_close=panel_pull=false;panel_anchor=48;
 nuwax_touch_gesture_set_action_queue((void*)1);
 if(!setjmp(done))nuwax_touch_gesture_task(NULL);
}
#define RUN(name,...) do{sample_t seq[]={__VA_ARGS__};replay(seq,sizeof(seq)/sizeof(*seq));printf("%s taps=%u page_drags=%u page_dx=%d page_commit=%d panel_drags=%u panel_close=%d previews=%u committed_volume=%d longs=%u\n",name,taps,page_drags,last_page_delta,last_page_commit,panel_drags,last_panel_close,volume_previews,last_volume,longs);}while(0)
int main(void){
 RUN("clean_tap",{0,1,230,110},{30,1,230,110},{60,0,230,110});assert(taps==1);
 RUN("eight_px_left",{0,1,230,110},{30,1,222,110},{60,0,222,110});assert(taps==1&&page_drags==0);
 RUN("eleven_px_diagonal",{0,1,230,110},{40,1,241,121},{80,0,241,121});assert(taps==1&&page_drags==0&&panel_drags==0);
 RUN("long_touch",{0,1,230,110},{1000,1,230,110},{1020,0,230,110});assert(taps==1&&longs==0);
 RUN("short_flick",{0,1,180,140},{40,1,210,140},{60,0,210,140});assert(last_page_commit);
 RUN("hold_then_same_flick",{0,1,180,140},{1000,1,180,140},{1040,1,210,140},{1060,0,210,140});assert(last_page_commit);
 RUN("fast_up_32px",{0,1,180,190},{20,1,180,158},{40,0,180,158});assert(panel_drags>0&&last_panel_close);
 RUN("up_65px",{0,1,180,190},{180,1,180,125},{200,0,180,125});assert(last_panel_close);
 RUN("volume_final_sample_throttled",{0,1,175,272},{10,1,180,272},{50,1,230,272},{80,1,290,272},{85,0,290,272});assert(volume_commits==1&&last_volume==100&&volume_finishes==1);
 RUN("volume_drag_exits_hitbox",{0,1,175,272},{10,1,200,272},{120,1,290,295},{140,0,290,295});assert(volume_commits==1&&last_volume==100&&volume_finishes==1);
 RUN("button_cannot_steal_slider",{0,1,230,240},{25,1,230,275},{55,0,230,275});assert(volume_commits==0);
 RUN("page_small_move_held",{0,1,180,140},{100,1,210,140},{500,1,210,140},{530,0,210,140});assert(!last_page_commit);
 RUN("stationary_after_swipe",{0,1,180,140},{40,1,210,140},{220,0,210,140});assert(!last_page_commit);
 RUN("slider_stationary_tap",{0,1,290,272},{20,0,290,272});assert(last_volume==100&&volume_finishes==1);
 RUN("lost_release_new_contact",{0,1,175,272,1},{20,1,230,110,2},{45,0,230,110,1},{60,0,230,110,2});assert(taps==1&&volume_commits==0&&cancels==1);
 RUN("read_cancel_not_commit",{0,1,175,272,1},{20,1,280,272,1},{40,0,280,272,1,false,true});assert(cancels==1&&volume_commits==0);
 RUN("fresh_samples_prevent_guard",{0,1,180,190},{20,1,180,160},{250,1,180,160},{360,0,0,0,0,true},{400,1,180,150},{420,0,0,0});assert(panel_finishes==1&&!last_panel_close);
 RUN("stale_slider_cancel",{0,1,175,272},{100,0,0,0,0,true},{300,0,0,0,0,true});assert(cancels==1&&volume_commits==0);
 RUN("timed_out_contact_cannot_rearm",{0,1,175,272,1},{300,0,0,0,0,true},{310,1,250,272,1},{330,0,250,272,1},{345,1,290,272,2},{365,0,290,272,2});assert(cancels==1&&volume_commits==1&&last_volume==100);
 RUN("exact_slop_remains_click",{0,1,230,110},{40,1,246,110},{65,0,246,110});assert(taps==1&&page_drags==0);
 RUN("page_flick_back_returns",{0,1,180,140},{100,1,260,140},{150,1,260,140},{200,1,230,140},{215,0,230,140});assert(page_drags>0&&!last_page_commit);
 RUN("page_immediate_reverse",{0,1,180,140},{40,1,260,140},{60,1,230,140},{65,0,230,140});assert(page_drags>0&&!last_page_commit);
 RUN("page_micro_jitter_keeps_flick",{0,1,180,140},{40,1,212,140},{60,1,210,140},{65,0,210,140});assert(last_page_commit);
 RUN("dense_hold_then_flick",{0,1,180,140},{15,1,180,140},{30,1,180,140},{45,1,180,140},{60,1,180,140},{75,1,195,140},{85,1,209,140},{100,0,209,140});assert(last_page_commit);
 RUN("drawer_reverse_flick_closes",{0,1,180,140},{100,1,180,240},{150,1,180,240},{200,1,180,190},{215,0,180,190});assert(panel_finishes==1&&last_panel_close);
 RUN("drawer_immediate_reverse",{0,1,180,140},{40,1,180,240},{60,1,180,210},{65,0,180,210});assert(panel_finishes==1&&last_panel_close);
 drawer_open=false;
 RUN("fast_open_before_ui_frame",{0,1,180,12},{20,1,180,45},{40,1,180,75},{50,0,180,75});assert(panel_pull&&panel_drags==2&&panel_finishes==1&&!last_panel_close);
 puts("PASS: production gestures preserve taps, recent flick velocity, captured slider final value, contact boundaries, timeout and rejection recovery");
 return 0;
}
'''


code = prefix + volume_boundary + gesture_macros + '\n' + gesture_code + tests
path = OUT/'gesture.c'
path.write_text(code,encoding='utf-8')
exe = OUT/'gesture.exe'
cmd = ['gcc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-variable','-Wno-unused-parameter','-Wno-unused-function','-Wno-misleading-indentation','-Wno-missing-field-initializers','-I'+str(ROOT/'main/nuwax_ui_v28'),str(path),'-o',str(exe)]
build = subprocess.run(cmd,capture_output=True,text=True)
(OUT/'compile.log').write_text(build.stdout+build.stderr,encoding='utf-8')
assert build.returncode==0,build.stderr
run = subprocess.run([str(exe)],capture_output=True,text=True,timeout=10)
(OUT/'run.log').write_text(run.stdout+run.stderr,encoding='utf-8')
assert run.returncode==0,run.stdout+run.stderr
print(run.stdout,end='')
(OUT/'report.json').write_text(json.dumps({'sources':hashes,'productionGestures':True,'hardwareValidated':False,'passed':True},indent=2),encoding='utf-8')
