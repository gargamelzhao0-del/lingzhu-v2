"""Run production UI mailbox producers and LVGL consumer through contact boundaries."""
from pathlib import Path
import hashlib, json, re, subprocess
root=Path(__file__).resolve().parents[1]
s=(root/'main/app_shell.c').read_text(encoding='utf-8-sig')
h=(root/'main/app_shell.h').read_text(encoding='utf-8-sig')
d=(root/'main/nuwax_ui_v28/nuwax_drawer.h').read_text(encoding='utf-8-sig')
def function(signature):
    a=s.index(signature)
    while ';' in s[a:s.index('{',a)]:a=s.index(signature,a+len(signature))
    b=s.index('{',a)+1;depth=1
    while depth:depth+=(s[b]=='{')-(s[b]=='}');b+=1
    return s[a:b]
prefix=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include "control_center_input_policy.h"
#include "nuwax_device_state.h"
#include "app_state.h"
typedef void *QueueHandle_t;
typedef void *TaskHandle_t;
typedef int StaticQueue_t,portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define taskENTER_CRITICAL(x) ((void)0)
#define taskEXIT_CRITICAL(x) ((void)0)
#define pdMS_TO_TICKS(x) (x)
#define pdTRUE 1
#define ESP_LOGE(...) ((void)0)
typedef struct {bool touched,cancelled;uint8_t gesture,points,event;uint16_t x,y;uint32_t contact_id,sample_ms;}nuwax_touch_point_t;
'''
prefix+=h[h.index('typedef enum {'):h.index('esp_err_t nuwax_app_shell_init(')]
prefix+=d[d.index('typedef enum {'):d.index('esp_err_t nuwax_drawer_create(')]
prefix+=re.search(r'typedef struct \{\n    uint32_t scene_generation;.*?\} nuwax_drawer_input_state_t;',d,re.S)[0]+'\n'
prefix+='\n'.join(re.findall(r'^#define APP_SHELL_.*$',s,re.M))+'\n'
prefix+=s[s.index('typedef struct {\n    uint32_t contact_sequence;'):s.index('static app_shell_overlay_state_t app_shell_overlay_state_get(')]
stubs=r'''
typedef unsigned TickType_t;
#define ESP_LOGW(...) ((void)0)
static app_shell_touch_event_t fifo[APP_SHELL_TOUCH_EVENT_QUEUE_DEPTH];
static unsigned head,count,drags,finishes,pages,page_finishes,taps,rejections,down_calls,up_calls;
static unsigned business_sends,notifications,last_wait,preview_calls,cancel_calls,note_calls;
static int pose,visible_volume,audio_preview,admitted_volume;
static bool opened,detail,modal,ota,closing,stable,is_menu,renderer_locked,lock_admits,action_admits;
static uint32_t scene,volume_clock,audio_preview_sequence;
static uint32_t clock_us=1000000,clock_step;
static nuwax_app_shell_result_t last_action;
static nuwax_device_state_t visible_state;
static char trace[512];
static TaskHandle_t current_task=(void*)7;
static void log_pose(char kind,int value) {
 size_t n=strlen(trace);snprintf(trace+n,sizeof(trace)-n,"%c%d ",kind,value);
}
static int64_t esp_timer_get_time(void){uint32_t n=clock_us;clock_us+=clock_step;return n;}
static int xQueueSend(QueueHandle_t q,const void *p,unsigned wait) {
 if(q==(void*)9){
    last_wait=wait;
    if(!action_admits)return 0;
    last_action=*(const nuwax_app_shell_result_t*)p;
    business_sends++;
    return pdTRUE;
 }
 assert(q==(void*)1);
 if(count==APP_SHELL_TOUCH_EVENT_QUEUE_DEPTH)return 0;
 fifo[(head+count++)%APP_SHELL_TOUCH_EVENT_QUEUE_DEPTH]=*(const app_shell_touch_event_t*)p;
 return pdTRUE;
}
static int xQueueReceive(QueueHandle_t q,void *p,unsigned wait) {
 (void)q;(void)wait;if(!count)return 0;
 *(app_shell_touch_event_t*)p=fifo[head++%APP_SHELL_TOUCH_EVENT_QUEUE_DEPTH];count--;return pdTRUE;
}
static unsigned uxQueueMessagesWaiting(QueueHandle_t q){(void)q;return count;}
static TaskHandle_t xTaskGetCurrentTaskHandle(void){return current_task;}
static void xTaskNotifyGive(TaskHandle_t task){
 assert(task==(void*)7);
 if(last_action.action==NUWAX_APP_SHELL_ACTION_SET_VOLUME) {
    assert(s_state_mailbox.snapshot.volume==last_action.index);
    assert(admitted_volume==last_action.index);
 }
 notifications++;
}
static void nuwax_lvgl_port_wake(void){}
static bool nuwax_lvgl_port_lock(uint32_t ms){
 assert(ms==500 && !renderer_locked && s_system_closing);
 renderer_locked=lock_admits;
 return lock_admits;
}
static void nuwax_lvgl_port_unlock(void){assert(renderer_locked);renderer_locked=false;}
static void nuwax_face_service_decode(void){}
static bool nuwax_ota_progress_is_visible(void){return ota;}
static void nuwax_drawer_contact(bool down,uint16_t x,uint16_t y){
 (void)x;(void)y;if(down)down_calls++;else up_calls++;
}
static bool nuwax_drawer_is_open(void){return opened&&!modal;}
static bool nuwax_drawer_is_detail(void){return detail;}
static int16_t nuwax_drawer_current_y(void){return pose;}
static void nuwax_drawer_begin_pull(void){opened=true;closing=false;pose=-226;log_pose('B',0);}
static void nuwax_drawer_drag_to(int16_t y){pose=y;closing=false;drags++;log_pose('D',y);}
static void nuwax_drawer_finish_drag(bool close){closing=close;finishes++;log_pose('F',close);}
static void nuwax_drawer_close(void){closing=true;stable=false;scene++;log_pose('C',0);}
static void nuwax_drawer_page_drag_to(int16_t dx){pages++;log_pose('P',dx);}
static void nuwax_drawer_page_finish_drag(bool commit){page_finishes++;log_pose('E',commit);}
static void nuwax_drawer_reject_action(nuwax_drawer_action_t a){(void)a;rejections++;}
static bool nuwax_drawer_show_provisioning_qr(void){opened=true;detail=true;scene++;return true;}
static void nuwax_drawer_hide_provisioning_qr(void){if(detail){detail=false;scene++;}}
void nuwax_device_state_set(const nuwax_device_state_t *state){visible_state=*state;visible_volume=state->volume;}
const nuwax_device_state_t *nuwax_device_state_get(void){return &visible_state;}
static size_t copy_str(char *dest,const char *source,size_t size) {
 size_t n=strlen(source);if(size){size_t m=n<size?n:size-1;memcpy(dest,source,m);dest[m]=0;}return n;
}
#define strlcpy copy_str
static void nuwax_drawer_get_input_state(nuwax_drawer_input_state_t *state){
 *state=(nuwax_drawer_input_state_t){
    .open=opened&&!modal,.detail=detail,.modal_blocked=modal,.scene_generation=scene,
    .tap_allowed=opened&&stable&&!modal,
    .volume_available=opened&&stable&&is_menu&&!detail&&!modal,.page_left=true,.page_right=true
 };
}
static bool nuwax_drawer_volume_value_at(uint16_t x,uint16_t y,bool captured,uint8_t *v){
 if(!captured&&(x<160||x>313||y<258||y>291))return false;
 *v=x<172?0:x>290?100:(x-172)*100/118;
 return true;
}
static nuwax_app_shell_result_t app_shell_handle_touch_event_on_lvgl(
 nuwax_app_shell_event_t e,const nuwax_touch_point_t *p) {
 if(e!=NUWAX_APP_SHELL_EVENT_TAP)return(nuwax_app_shell_result_t){0};
 assert(p && p->x==100 && p->y==110);
 taps++;
 return(nuwax_app_shell_result_t){
    .action=is_menu?NUWAX_APP_SHELL_ACTION_SET_MEETING_MODE:NUWAX_APP_SHELL_ACTION_NONE,.index=1,
 };
}
static uint32_t preview(int volume){
 preview_calls++;audio_preview=volume;return audio_preview_sequence=++volume_clock;
}
static void cancel(uint32_t sequence){
 cancel_calls++;
 if(sequence==audio_preview_sequence){audio_preview=admitted_volume;audio_preview_sequence=0;}
}
static void note_queued(int volume,uint32_t sequence){
 assert(sequence!=0);
 note_calls++;admitted_volume=volume;
}
void nuwax_app_shell_request_close_overlays(void);
static void app_shell_cancel_touch_locked(void);
static void app_shell_cancel_volume_preview_locked(void);
'''
functions=[
 'static uint32_t app_shell_next_revision(', 'bool nuwax_app_shell_is_ui_ready(',
 'void nuwax_app_shell_set_action_queue(', 'void nuwax_app_shell_action_queued(',
 'void nuwax_app_shell_set_volume_callbacks(', 'static uint32_t app_shell_next_state_revision_locked(',
 'static bool app_shell_touch_allowed_locked(', 'static void app_shell_clear_volume_preview_locked(',
 'static void app_shell_restore_volume_locked(', 'static void app_shell_cancel_volume_preview_locked(',
 'static void app_shell_cancel_touch_locked(', 'static uint32_t app_shell_action_bit(',
 'void nuwax_app_shell_notify_action_rejected(', 'static void app_shell_ensure_state_snapshot_locked(',
 'static void app_shell_merge_device_state_locked(', 'static void app_shell_post_volume_locked(',
 'static bool app_shell_queue_result(', 'static void app_shell_overlay_state_sync_from_lvgl(',
 'static void app_shell_lvgl_unlock(', 'static bool app_shell_enqueue_input(',
 'static void app_shell_post_drawer_page_drag(', 'static void app_shell_post_drawer_page_finish(',
 'static bool app_shell_touch_allowed_on_lvgl(', 'static void app_shell_apply_pending_close(',
 'static bool app_shell_apply_panel_motion(', 'static void app_shell_finish_volume_on_lvgl(',
 'static bool app_shell_drain_touch_events(', 'static void app_shell_lvgl_pre_handler(',
 'void nuwax_app_shell_request_close_overlays(', 'void nuwax_app_shell_close_overlays(',
 'void nuwax_app_shell_touch_begin(', 'bool nuwax_app_shell_drawer_can_page_drag(',
 'bool nuwax_app_shell_drawer_preview_volume_drag_at(', 'void nuwax_app_shell_drawer_finish_volume_drag(',
 'void nuwax_app_shell_drawer_contact(', 'void nuwax_app_shell_drawer_drag_by(',
 'void nuwax_app_shell_drawer_finish_drag(', 'void nuwax_app_shell_confirm_volume(',
 'void nuwax_app_shell_set_ota_progress(',
]
checks=r'''
static void reset(void){
 head=count=drags=finishes=pages=page_finishes=taps=rejections=down_calls=up_calls=0;
 business_sends=notifications=last_wait=preview_calls=cancel_calls=note_calls=0;
 pose=48;visible_volume=audio_preview=admitted_volume=60;
 opened=stable=is_menu=lock_admits=action_admits=true;
 detail=modal=ota=closing=renderer_locked=false;
 scene=1;volume_clock=audio_preview_sequence=0;trace[0]=0;clock_step=0;
 s_touch_event_queue=(void*)1;s_ui_ready=true;
 s_touch_contact_sequence=s_touch_cancelled_sequence=s_touch_completed_sequence=0;
 s_panel_anchor_sequence=s_visual_contact_sequence=s_volume_setting_sequence=0;
 s_pending_close_overlays=s_system_closing=s_pending_drawer_page_drag=s_pending_drawer_contact=false;
 s_pending_provisioning_qr=s_pending_provisioning_qr_close=false;
 s_touch_panel_motion=s_pending_panel_motion=(app_shell_panel_motion_t){0};
 s_drawer_input=(nuwax_drawer_input_state_t){0};s_drawer_contact_down=false;
 memset(&s_volume_preview,0,sizeof(s_volume_preview));
 s_state_mailbox=(app_shell_state_mailbox_t){.initialized=true,.snapshot={.volume=60}};
 visible_state=s_state_mailbox.snapshot;s_drawer_action_reject_mask=0;
 nuwax_app_shell_set_volume_callbacks(preview,cancel,note_queued);
 nuwax_app_shell_set_action_queue((void*)9);
 app_shell_overlay_state_sync_from_lvgl();
}
static void frame(void){app_shell_lvgl_pre_handler(NULL);}
static void begin(void){nuwax_app_shell_touch_begin();}
static bool tap(void){
 app_shell_touch_event_t e={
    .kind=APP_SHELL_INPUT_TOUCH,.event=NUWAX_APP_SHELL_EVENT_TAP,
    .has_point=true,.point={.x=100,.y=110},
 };
 return app_shell_enqueue_input(&e);
}
static uint8_t slider(unsigned x,bool captured) {
 uint8_t v=255;
 assert(nuwax_app_shell_drawer_preview_volume_drag_at(x,270,captured,&v));
 return v;
}
static void volume_release(unsigned x){nuwax_app_shell_drawer_finish_volume_drag(slider(x,false));}

int main(void){
 reset();opened=false;app_shell_overlay_state_sync_from_lvgl();begin();
 nuwax_app_shell_drawer_drag_by(206,true);
 nuwax_app_shell_drawer_finish_drag(206,206,0,false);
 frame();assert(!strcmp(trace,"B0 D-20 F0 ")&&drags==1&&finishes==1);
 frame();assert(drags==1&&finishes==1);

 /* Movement consumed after the renderer advances must start at that pose. */
 reset();pose=-116;begin();nuwax_app_shell_drawer_drag_by(0,false);
 pose=-29;frame();assert(pose==-29&&drags==1);
 nuwax_app_shell_drawer_drag_by(24,false);frame();assert(pose==-5&&drags==2);
 nuwax_app_shell_drawer_finish_drag(29,45,0,false);frame();assert(pose==0&&finishes==1&&!closing);

 /* The final pose and close decision are applied before the next contact. */
 reset();begin();nuwax_app_shell_drawer_drag_by(-90,false);
 nuwax_app_shell_drawer_finish_drag(-90,-105,0,false);
 begin();nuwax_app_shell_drawer_drag_by(20,false);frame();
 assert(!strcmp(trace,"D-42 F1 D-22 ")&&!closing);

 reset();for(int i=0;i<3;i++){
    begin();app_shell_post_drawer_page_drag(20+i*10);app_shell_post_drawer_page_finish(i==1);
 }
 frame();assert(pages==3&&page_finishes==3&&!strcmp(trace,"P20 E0 P30 E1 P40 E0 "));

 /* Same-scene rapid taps survive coalesced contact feedback. */
 reset();begin();nuwax_app_shell_drawer_contact(true,100,110);
 nuwax_app_shell_drawer_contact(false,0,0);assert(tap());
 begin();assert(tap());frame();assert(taps==2&&business_sends==2);
 reset();is_menu=false;begin();assert(tap());is_menu=true;scene++;frame();
 assert(taps==0&&business_sends==0);
 reset();stable=false;app_shell_overlay_state_sync_from_lvgl();begin();assert(tap());
 stable=true;frame();assert(!taps&&!business_sends);

 /* Strong close wins over already queued movement and terminal operations. */
 reset();opened=false;app_shell_overlay_state_sync_from_lvgl();begin();
 nuwax_app_shell_drawer_drag_by(170,true);
 nuwax_app_shell_drawer_finish_drag(180,180,450,false);
 nuwax_app_shell_close_overlays();frame();assert(!opened&&!drags&&!finishes);
 reset();begin();volume_release(285);nuwax_app_shell_request_close_overlays();
 assert(audio_preview==60&&cancel_calls==1);
 frame();assert(!business_sends&&closing&&visible_volume==60);
 begin();assert(!tap());opened=false;app_shell_overlay_state_sync_from_lvgl();
 opened=stable=true;scene++;app_shell_overlay_state_sync_from_lvgl();
 assert(!tap());begin();assert(tap());frame();assert(taps==1&&business_sends==1);
 reset();lock_admits=false;begin();nuwax_app_shell_drawer_drag_by(-20,false);
 nuwax_app_shell_close_overlays();assert(s_pending_close_overlays&&!closing);
 frame();assert(closing&&!drags);

 reset();begin();volume_release(290);
 nuwax_app_shell_set_ota_progress(true,20);nuwax_app_shell_set_ota_progress(false,0);
 frame();assert(!business_sends&&audio_preview==60&&visible_volume==60);
 reset();begin();volume_release(290);modal=true;scene++;frame();assert(!business_sends&&audio_preview==60);
 modal=false;scene++;frame();assert(!business_sends&&visible_volume==60);

 /* Direct QR takeover invalidates the press before its release is dispatched. */
 reset();begin();volume_release(285);
 s_pending_provisioning_qr=true;s_pending_provisioning_qr_deadline_us=clock_us-1;
 s_state_mailbox.snapshot.provisioning_active=true;
 frame();assert(detail&&!business_sends&&audio_preview==60);

 /* Canonical state changes only on accepted release or owner confirmation. */
 reset();begin();const uint8_t first=slider(254,false);const uint32_t a=audio_preview_sequence;
 frame();assert(visible_volume==first&&s_state_mailbox.snapshot.volume==60);
 assert(slider(254,true)==first&&preview_calls==1);
 nuwax_app_shell_drawer_finish_volume_drag(first);frame();
 assert(business_sends==1&&notifications==1&&last_action.volume_sequence==a&&note_calls==1);
 begin();const uint8_t second=slider(282,false);const uint32_t b=audio_preview_sequence;
 nuwax_app_shell_confirm_volume(first,a);frame();
 assert(visible_volume==second&&s_state_mailbox.snapshot.volume==first&&audio_preview==second);
 nuwax_app_shell_drawer_finish_volume_drag(second);frame();
 assert(last_action.volume_sequence==b&&note_calls==2&&business_sends==2);
 nuwax_app_shell_confirm_volume(first,a);frame();
 assert(visible_volume==second&&s_state_mailbox.snapshot.volume==second);

 reset();begin();volume_release(290);
 nuwax_app_shell_confirm_volume(25,++volume_clock);
 frame();assert(!business_sends&&visible_volume==25);
 reset();begin();volume_release(290);scene++;is_menu=false;
 frame();assert(!business_sends&&audio_preview==60&&visible_volume==60);

 reset();action_admits=false;begin();volume_release(290);frame();frame();
 assert(!business_sends&&!notifications&&!note_calls&&audio_preview==60&&visible_volume==60&&rejections==1);
 reset();begin();volume_release(250);begin();const uint8_t latest=slider(286,false);frame();
 assert(business_sends==1&&visible_volume==latest&&audio_preview==latest);
 nuwax_app_shell_drawer_finish_volume_drag(-1);frame();
 assert(audio_preview==admitted_volume&&visible_volume==admitted_volume&&business_sends==1);

 reset();for(unsigned i=0;i<APP_SHELL_TOUCH_EVENT_QUEUE_DEPTH;i++){
    begin();app_shell_post_drawer_page_finish(true);
 }
 begin();nuwax_app_shell_drawer_drag_by(0,false);nuwax_app_shell_drawer_finish_drag(0,0,0,false);
 assert(s_pending_close_overlays);frame();assert(!drags&&!finishes&&!page_finishes&&closing);

 reset();for(unsigned i=0;i<4;i++){
    begin();app_shell_post_drawer_page_drag(20+i);app_shell_post_drawer_page_finish(true);
 }
 begin();nuwax_app_shell_drawer_drag_by(-10,false);
 clock_step=750;frame();assert(count>0&&drags==0);
 for(unsigned i=0;i<10&&count;i++)frame();
 frame();assert(page_finishes==4&&drags==1&&pose==38);

 reset();s_touch_contact_sequence=s_touch_completed_sequence=s_touch_cancelled_sequence=UINT32_MAX-1U;
 begin();nuwax_app_shell_drawer_drag_by(-10,false);nuwax_app_shell_drawer_finish_drag(-10,-25,0,false);
 begin();app_shell_post_drawer_page_drag(30);app_shell_post_drawer_page_finish(true);
 frame();assert(finishes==1&&page_finishes==1&&s_touch_completed_sequence==1U);

 reset();begin();nuwax_app_shell_drawer_contact(true,220,110);frame();
 nuwax_app_shell_drawer_contact(false,0,0);begin();nuwax_app_shell_drawer_contact(true,220,110);
 frame();assert(down_calls==2&&up_calls>=2);

 reset();nuwax_app_shell_set_action_queue(NULL);nuwax_app_shell_action_queued();assert(!notifications);
 nuwax_app_shell_set_action_queue((void*)9);current_task=(void*)8;
 nuwax_app_shell_result_t action={.action=NUWAX_APP_SHELL_ACTION_CYCLE_BRIGHTNESS};
 assert(app_shell_queue_result(&action)&&notifications==1&&last_wait==0);
 action_admits=false;assert(!app_shell_queue_result(&action)&&notifications==1);
 action_admits=true;action.action=NUWAX_APP_SHELL_ACTION_STOP_LISTEN;
 assert(app_shell_queue_result(&action)&&notifications==2&&last_wait==100);
 puts("PASS: production mailbox close and modal fences, scene-bound taps, LVGL anchor, ordered volume, FIFO budget/overflow/wrap, business queue wake");
}
'''
out=root/'build-host-checks/drawer-mailbox';out.mkdir(parents=True,exist_ok=True)
c=out/'mailbox.c';c.write_text(prefix+stubs+'\n'.join(function(x) for x in functions)+checks,encoding='utf-8')
exe=out/'mailbox.exe'
cmd=['gcc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable','-I'+str(root/'main/nuwax_ui_v28'),'-I'+str(root/'main'),str(c),'-o',str(exe)]
b=subprocess.run(cmd,capture_output=True,text=True);(out/'compile.log').write_text(b.stdout+b.stderr,encoding='utf-8');assert b.returncode==0,b.stderr
r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=10);(out/'run.log').write_text(r.stdout+r.stderr,encoding='utf-8');assert r.returncode==0,r.stdout+r.stderr
print(r.stdout,end='');(out/'report.json').write_text(json.dumps({'sourceSha256':hashlib.sha256((root/'main/app_shell.c').read_bytes()).hexdigest(),'passed':True,'hardwareValidated':False},indent=2),encoding='utf-8')

