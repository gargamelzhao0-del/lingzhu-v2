"""Run production wake request, hit binding and cloud-ready predicates on host."""
from pathlib import Path
import re
import subprocess

root = Path(__file__).resolve().parents[1]
out = root / 'build-host-checks/cloud-resume'
out.mkdir(parents=True, exist_ok=True)
source = (root / 'main/nuwax_ai_partner_main.c').read_text(encoding='utf-8-sig')


def function(text, signature):
    start = text.rindex(signature)
    pos = text.index('{', start)
    depth, end = 1, pos + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


common = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#define CONFIG_NUWAX_WAKE_WORD_ENABLE 1
#define CONFIG_NUWAX_OFFLINE_AUDIO_ENABLE 1
#define CONFIG_NUWAX_OTA_ENABLE 1
#define TAG "host"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define NUWAX_WAKE_MODE_SLEEP 1
#define NUWAX_WAKE_MODE_PLAYBACK_INTERRUPT 2
#define NUWAX_WAKE_EVENT_MAX_AGE_MS 3000U
#define pdTRUE 1
static bool ota_busy, cloud_ready=true, running=true;
static bool ota_blocks_wake_detector(void){return ota_busy;}
static bool cloud_service_is_ready(void){return cloud_ready;}
static uint32_t active_epoch, clock_ms=100, next_token;
static uint32_t active_ws_epoch_snapshot(void){return active_epoch;}
static uint32_t monotonic_ms(void){return clock_ms;}
static bool elapsed_ms(uint32_t now,uint32_t deadline){return (int32_t)(now-deadline)>=0;}
typedef struct {int mode;uint32_t mode_token,ws_epoch,speaker_generation;} arm_t;
static arm_t s_wake_detector_request;
static atomic_bool s_wake_detector_desired;
static atomic_uint s_wake_detector_request_ms;
static int s_wake_detector_request_lock, locked;
static int64_t s_wake_interrupt_first_pcm_us;
static unsigned stops, notifications;
#define portENTER_CRITICAL(p) do{assert(p==&s_wake_detector_request_lock && !locked);locked=1;}while(0)
#define portEXIT_CRITICAL(p) do{assert(p==&s_wake_detector_request_lock && locked);locked=0;}while(0)
static uint32_t next_wake_detector_token_locked(void){return ++next_token;}
static bool nuwax_wake_word_detector_is_running(void){return running;}
static void nuwax_wake_word_detector_request_stop(void){stops++;}
static void notify_wake_supervisor(void){notifications++;}
static void notify_conversation_owner(void){}
static void inhibit_sleep_wake_word_detector(const char *r){atomic_store(&s_wake_detector_desired,false);}
typedef struct {arm_t arm;bool probe_requested;uint32_t detected_ms;char wake_word[32];} nuwax_wake_word_result_t;
typedef nuwax_wake_word_result_t wake_word_event_t;
static int s_wake_word_events=1;
static bool queued;
static wake_word_event_t saved;
static int xQueueSend(int q,const void *p,unsigned t){if(queued)return 0;saved=*(const wake_word_event_t*)p;queued=true;return 1;}
static int xQueueReceive(int q,void *p,unsigned t){if(!queued)return 0;*(wake_word_event_t*)p=saved;queued=false;return 1;}
'''
wake = common + function(source, 'static void request_sleep_wake_word_detector(const char *reason) {')
wake += '\n' + function(source, 'static void wake_word_detected_callback(')
predicate = re.search(r'if \((wake_event\.arm\.ws_epoch == 0U.*?)\) \{', source, re.S).group(1)
wake += '\nstatic bool rejected(wake_word_event_t wake_event,uint32_t active_epoch,uint32_t now32){return ' + predicate + ';}\n'
wake += r'''
int main(void){
 request_sleep_wake_word_detector("boot");
 assert(stops==1 && s_wake_detector_request.ws_epoch==0);
 const uint32_t token=s_wake_detector_request.mode_token;
 for(unsigned epoch=1;epoch<10;epoch++){
  active_epoch=epoch;request_sleep_wake_word_detector("cloud_ready");
  assert(stops==1 && s_wake_detector_request.mode_token==token);
 }
 // A real playback->sleep transition still revokes and restarts its old arm.
 s_wake_detector_request.mode=NUWAX_WAKE_MODE_PLAYBACK_INTERRUPT;
 s_wake_detector_request.ws_epoch=9;request_sleep_wake_word_detector("playback_done");
 assert(stops==2 && s_wake_detector_request.mode_token!=token);
 ota_busy=true;atomic_store(&s_wake_detector_desired,false);
 request_sleep_wake_word_detector("ota");assert(!atomic_load(&s_wake_detector_desired) && stops==2);
 ota_busy=false;
 nuwax_wake_word_result_t hit={.arm={.mode=NUWAX_WAKE_MODE_SLEEP,.ws_epoch=0},.detected_ms=100};
 active_epoch=9;wake_word_detected_callback(&hit,NULL);
 assert(queued && saved.arm.ws_epoch==9 && !rejected(saved,9,100));
 assert(rejected(saved,10,100)); // reconnect after publication must reject the queued old hit
 assert(rejected(saved,9,3100)); // age fence remains
 saved.arm.ws_epoch=0;assert(rejected(saved,9,100));queued=false;
 active_epoch=0;wake_word_detected_callback(&hit,NULL);assert(!queued);
 cloud_ready=false;active_epoch=10;wake_word_detected_callback(&hit,NULL);assert(!queued);
 cloud_ready=true;hit.arm.mode=NUWAX_WAKE_MODE_PLAYBACK_INTERRUPT;hit.arm.ws_epoch=7;
 wake_word_detected_callback(&hit,NULL);assert(queued && saved.arm.ws_epoch==7); // never rebind playback hits
 puts("PASS: local sleep arm survives nine connection epochs; playback/OTA boundaries; current hit binding and stale-hit rejection");
}
'''

claims = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#define CONFIG_NUWAX_OFFLINE_AUDIO_ENABLE 1
#define CONFIG_NUWAX_OTA_ENABLE 1
#define NUWAX_MEETING_STATE_DISABLED 0
#define NUWAX_MONITORING_STATE_DISABLED 0
#define NUWAX_APP_STATE_WS_CONNECTING 1
#define NUWAX_APP_STATE_SLEEPING 2
#define TAG "test"
#define ESP_LOGW(...) ((void)0)
typedef struct {bool initialized;int state;atomic_uint queued_enable_events,queued_sync_acks;void *events,*request_acks;} nuwax_meeting_mode_t;
typedef nuwax_meeting_mode_t nuwax_monitoring_mode_t;
typedef int nuwax_monitoring_state_t;
static nuwax_meeting_mode_t s_meeting_mode={.initialized=true};
static nuwax_monitoring_mode_t s_monitoring_mode={.initialized=true};
static bool lease,capture,busy,ready=true,notification,ota,awake;
static int state=NUWAX_APP_STATE_WS_CONNECTING, sleeps;
static bool s_awake_resume_after_ws_reconnect;
static struct {bool active;} s_turn_guard;
static struct {bool pending,capture_active;} s_prelisten;
static int s_awake_session,s_notification_transaction;
static atomic_bool s_reprovision_quiesce_requested;
static bool offline_mode_lease_pending(void){return lease;}
static bool nuwax_offline_audio_capture_running(void){return capture;}
static int conversation_state_current(void){return state;}
static bool cloud_service_is_ready(void){return ready;}
static bool nuwax_awake_session_state_is_active(void *p){return awake;}
static bool conversation_capture_busy_for_sleep(void){return busy;}
static bool nuwax_notification_blocks_conversation(void *p){return notification;}
static bool nuwax_ota_device_blocks_audio(void){return ota;}
static unsigned uxQueueMessagesWaiting(void *p){return p?1:0;}
static void request_sleeping_conversation_state(const char *r){sleeps++;state=NUWAX_APP_STATE_SLEEPING;}
'''
for mode in ['meeting', 'monitoring']:
    text = (root / f'main/audio_modes/{mode}/{mode}_mode.c').read_text(encoding='utf-8-sig')
    if mode == 'monitoring':
        claims += '\n' + function(text, 'nuwax_monitoring_state_t nuwax_monitoring_mode_state(')
    claims += '\n' + function(text, f'bool nuwax_{mode}_mode_has_audio_claim(')
    claims += '\n' + function(text, f'bool nuwax_{mode}_mode_has_pending(')
claims += '\n' + function(source, 'static bool meeting_or_monitoring_mode_blocks_sleep(void) {')
claims += '\n' + function(source, 'static bool exclusive_audio_mode_blocks_normal_audio(void) {')
claims += '\n' + function(source, 'static bool reconcile_cloud_ready_sleep_state(const char *reason) {')
claims += r'''
int main(void){
 s_meeting_mode.events=(void*)1;s_monitoring_mode.request_acks=(void*)1;
 assert(exclusive_audio_mode_blocks_normal_audio()); // normal capture must drain pending controls first
 assert(!meeting_or_monitoring_mode_blocks_sleep());
 assert(reconcile_cloud_ready_sleep_state("cloud_ready") && sleeps==1);
 assert(!reconcile_cloud_ready_sleep_state("again") && sleeps==1);
 assert(s_meeting_mode.events && s_monitoring_mode.request_acks); // no queue purging/reordering
 // Passive state report ACKs are queued but do not reserve audio or sleep.
 s_monitoring_mode.events=(void*)1;
 atomic_store(&s_meeting_mode.queued_sync_acks,1);
 atomic_store(&s_monitoring_mode.queued_sync_acks,1);
 assert(!exclusive_audio_mode_blocks_normal_audio());
 assert(!meeting_or_monitoring_mode_blocks_sleep());
 atomic_store(&s_meeting_mode.queued_enable_events,1);
 assert(exclusive_audio_mode_blocks_normal_audio());
 atomic_store(&s_meeting_mode.queued_enable_events,0);
 atomic_store(&s_meeting_mode.queued_sync_acks,0);
 atomic_store(&s_monitoring_mode.queued_sync_acks,0);
 s_monitoring_mode.events=NULL;
 for(unsigned scenario=0;scenario<11;scenario++){
  state=NUWAX_APP_STATE_WS_CONNECTING;
  atomic_store(&s_meeting_mode.queued_enable_events,scenario==0);
  atomic_store(&s_monitoring_mode.queued_enable_events,scenario==1);
  s_meeting_mode.state=scenario==2;s_monitoring_mode.state=scenario==3;
  lease=scenario==4;capture=scenario==5;busy=scenario==6;notification=scenario==7;
  ota=scenario==8;atomic_store(&s_reprovision_quiesce_requested,scenario==9);awake=scenario==10;
  assert(!reconcile_cloud_ready_sleep_state("blocked") && sleeps==1);
 }
 puts("PASS: pending disabled/ACK controls do not steal sleep; queued enable, active modes, SD, capture, notification, OTA and BLE still block");
}
'''


def run(name, code, negative=False):
    src, exe = out / (name + '.c'), out / (name + '.exe')
    src.write_text(code, encoding='utf-8')
    subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                    str(src), '-o', str(exe)], check=True)
    result = subprocess.run([str(exe)], capture_output=negative)
    assert (result.returncode != 0) if negative else (result.returncode == 0), name
    if negative:
        print('PASS: negative mutant rejected ' + name)


run('wake', wake)
run('claims', claims)
run('epoch_restart', wake.replace('s_wake_detector_request.mode == NUWAX_WAKE_MODE_SLEEP;',
    's_wake_detector_request.mode == NUWAX_WAKE_MODE_SLEEP && s_wake_detector_request.ws_epoch == active_ws_epoch_snapshot();'), True)
run('wrong_hit_epoch', wake.replace('event.arm.ws_epoch = connection_epoch;', 'event.arm.ws_epoch = 0;'), True)
run('pending_as_capture', claims.replace('meeting_or_monitoring_mode_blocks_sleep() ||',
                                       'exclusive_audio_mode_blocks_normal_audio() ||'), True)

setup = source[source.index('            bool resume_awake = false;'):source.index('            while (nuwax_ws_is_connected(&ws))', source.index('            bool resume_awake = false;'))]
def verify_setup(text):
    assert 'vTaskDelay(' not in text, 'blind wait reintroduced into cloud-ready setup'
    assert 'drain_speaker_owner_events();' in text
    assert 'conversation_state_current() != NUWAX_APP_STATE_SLEEPING' in text
verify_setup(setup)
try:
    verify_setup(setup + '\nvTaskDelay(pdMS_TO_TICKS(500));')
except AssertionError:
    print('PASS: blind 500ms delay mutation rejected; immediate speaker owner drain; sleeping transition idempotent')
else:
    raise AssertionError('blind delay mutant escaped')
