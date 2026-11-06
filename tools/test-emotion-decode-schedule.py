"""Exercise the production owner/worker with deterministic preemption points.

No hardware timing or throughput claim. Generated C uses the actual functions,
with SDK calls and decompression replaced by controlled test doubles.
"""
from pathlib import Path
import hashlib, json, re, subprocess

root = Path(__file__).resolve().parents[1]
out = root/'build-host-checks/emotion-decode-schedule'
out.mkdir(parents=True, exist_ok=True)
path = root/'main/nuwax_ui_v28/face/emotion_animation_player.c'
source = path.read_text(encoding='utf-8-sig')

def extract(name):
    m = re.search(r'^(?:static )?[\w* ]+\b'+name+r'\([^;{}]*\)\s*\{', source, re.M)
    assert m, name
    i = source.index('{',m.start())+1
    depth=1
    while depth:
        depth+=(source[i]=='{')-(source[i]=='}')
        i+=1
    return source[m.start():i]

support=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include "emotion_decode_schedule.h"
#include "replay_ui_schedule.h"
typedef int esp_err_t;
typedef void *TaskHandle_t;
enum { ESP_OK=0, DECODE_WORKER_IDLE, DECODE_WORKER_REQUESTED, DECODE_WORKER_RUNNING,
       DECODE_WORKER_READY, DECODE_WORKER_FAILED, DECODE_SLOT_FREE, DECODE_SLOT_FILLING,
       DECODE_SLOT_READY, BRIDGE_DECODE_REQUESTED, BRIDGE_DECODE_RUNNING,
       BRIDGE_DECODE_READY, BRIDGE_DECODE_FAILED };
#define NUWAX_EMOTION_ASSET_PACK_ABI_V2 2
#define DECODE_TASK_PRIORITY 4U
#define DECODE_COOPERATIVE_DELAY_THRESHOLD_US 25000U
#define MIN_ANIMATED_FPS 40U
#define MAX_ANIMATED_FPS 60U
#define FRAME_RGB_BYTES 2U
#define pdTRUE 1
#define portMAX_DELAY 0xffffffffU
typedef struct { atomic_int state; uint32_t generation,sequence,decode_us; } decode_frame_slot_t;
typedef struct { unsigned frame_count; } segment_t;
typedef struct { unsigned frame_index; segment_t *segment; } decoder_t;
typedef struct { unsigned fps; } entry_t;
typedef struct { void *source_rgb565,*visible_rgb565,*target_indices,*target_rgb565;
    unsigned asset_id,segment_index,frame_index,generation,decode_us; int result; } bridge_decode_job_t;
static emotion_decode_help_t s_decode_help;
static atomic_bool s_decode_help_frame,s_decode_paused,s_decode_stop_requested,s_decode_task_exited;
static atomic_uint s_decode_help_serial;
static atomic_uint s_decode_generation,s_queued_asset_id;
static atomic_int s_decode_worker_state,s_bridge_decode_state;
static atomic_uint s_bridge_decode_generation;
static atomic_bool s_bridge_source_ready;
static bridge_decode_job_t s_bridge_decode_job;
static atomic_uint s_health_decode_cooperative_blocks,s_health_decode_switch_aborts;
static atomic_uint s_decode_request_us,s_decode_request_wait_us,s_decode_request_count,s_decode_request_wait_max_us;
static atomic_bool s_decode_request_timed;
static uint32_t s_decode_help_grants,s_decode_help_ready,s_decode_help_expired,s_decode_help_max_us;
static uint64_t s_decode_help_total_us;
static int64_t s_decode_last_publish_us,s_next_frame_us;
static TaskHandle_t s_decode_task;
static bool s_ready,s_background_decode,s_interaction_active,s_holding_frame,s_segment_advance_pending;
static struct { bool active; } s_component_bridge;
static entry_t entry,*s_entry;
static segment_t segment;
static decoder_t s_decoder;
static decode_frame_slot_t slots[2];
static const unsigned s_decode_slot_count=2;
static uint32_t s_next_decode_sequence,s_pending_decode_us;
static int s_decode_worker_error;
static int64_t now_us;
static bool replay, help_pending;
static nuwax_replay_ui_help_state_t help_outcome;
void nuwax_replay_ui_set_help_state(nuwax_replay_ui_help_state_t state, uint32_t until_us) {
    assert(state!=NUWAX_REPLAY_UI_HELP_REQUESTED || until_us==(uint32_t)s_decode_help.until_us);
    assert(state==NUWAX_REPLAY_UI_HELP_REQUESTED || until_us==0U);
    help_outcome=state; help_pending=state==NUWAX_REPLAY_UI_HELP_REQUESTED;
}
static unsigned priority,priority_changes,notifies,wakes,delays,yields,reads,prepares;
static unsigned prepare_cost,hook;
static jmp_buf done;
static int64_t esp_timer_get_time(void) { return now_us; }
static bool nuwax_offline_audio_replay_running(void) { return replay; }
static unsigned nuwax_emotion_asset_pack_abi(void) { return 4; }
static void vTaskPrioritySet(void *task,unsigned p) {
    assert(task==s_decode_task && task); priority=p;priority_changes++;
}
static unsigned ready_decode_slot_count(unsigned generation) {
    unsigned n=0;for(unsigned i=0;i<2;i++)
        if(atomic_load(&slots[i].state)==DECODE_SLOT_READY && slots[i].generation==generation)n++;
    return n;
}
static void xTaskNotifyGive(void *task) { assert(task==s_decode_task);notifies++; }
static unsigned ulTaskNotifyTake(int clear,unsigned timeout) {
    (void)clear;(void)timeout;
    if(reads++)atomic_store(&s_decode_stop_requested,true);
    return 1;
}
static void vTaskDelay(unsigned ticks) { assert(ticks==1);delays++;now_us+=10000; }
static void taskYIELD(void) { yields++; }
static void vTaskSuspend(void *task) { assert(!task);longjmp(done,1); }
static void nuwax_lvgl_port_wake(void) { assert(atomic_load(&s_decode_worker_state)==DECODE_WORKER_READY);wakes++; }
static decode_frame_slot_t *find_free_decode_slot(void) {
    for(unsigned i=0;i<2;i++)if(atomic_load(&slots[i].state)==DECODE_SLOT_FREE){
        atomic_store(&slots[i].state,DECODE_SLOT_FILLING);return &slots[i];}
    return NULL;
}
static void release_decode_slot(decode_frame_slot_t *s) { atomic_store(&s->state,DECODE_SLOT_FREE); }
static int decode_v4_handoff_frame(unsigned a,unsigned b,unsigned c,void*d,void*e) {
    (void)a;(void)b;(void)c;(void)d;(void)e;return ESP_OK;
}
static void decode_help_service(int64_t);
static void decode_worker_request(void);
static int prepare_next_frame(decode_frame_slot_t *slot) {
    (void)slot;prepares++;
    if(hook==1 && prepares==1)decode_help_service(now_us); // owner preempts a running decode
    if(hook==6 && prepares==1)decode_help_service(now_us);
    now_us+=prepare_cost;
    if(hook==2)decode_help_service(now_us); // lease expires during one frame
    if(hook==6)decode_help_service(now_us); // granted AND expired within this frame
    if(hook==3)atomic_store(&s_queued_asset_id,9); // newest emotion cancels publication
    if(hook==4)return -7;
    if(hook==5)atomic_fetch_add(&s_decode_generation,1);
    s_decoder.frame_index++;
    return ESP_OK;
}
'''

tests=r'''
static void reset(void) {
    s_decode_help=(emotion_decode_help_t){0};
    atomic_store(&s_decode_help_frame,false);
    atomic_store(&s_decode_help_serial,0);
    s_ready=s_background_decode=replay=true;s_decode_task=(void*)1;
    s_interaction_active=s_holding_frame=s_segment_advance_pending=s_component_bridge.active=false;
    atomic_store(&s_decode_paused,false);atomic_store(&s_decode_stop_requested,false);
    atomic_store(&s_decode_task_exited,false);atomic_store(&s_decode_generation,3);
    atomic_store(&s_queued_asset_id,0);atomic_store(&s_decode_worker_state,DECODE_WORKER_IDLE);
    atomic_store(&s_bridge_decode_state,0);atomic_store(&s_decode_request_timed,false);
    atomic_store(&s_decode_request_count,0);atomic_store(&s_decode_request_wait_us,0);
    atomic_store(&s_decode_request_wait_max_us,0);
    entry.fps=40;s_entry=&entry;segment.frame_count=100;s_decoder=(decoder_t){0,&segment};
    for(unsigned i=0;i<2;i++){slots[i]=(decode_frame_slot_t){0};atomic_store(&slots[i].state,DECODE_SLOT_FREE);}
    s_decode_help_grants=s_decode_help_ready=s_decode_help_expired=s_decode_help_max_us=0;
    s_decode_help_total_us=0;priority=4;priority_changes=notifies=wakes=delays=yields=reads=prepares=0;
    now_us=100000;s_next_frame_us=now_us+8000;prepare_cost=2000;hook=0;
}
static void run_worker(void) { if(!setjmp(done))decode_worker_task(NULL); }
int main(void) {
    reset();decode_worker_request();replay=false;decode_help_service(now_us);
    assert(priority==4 && !s_decode_help.active);run_worker();
    assert(prepares==2 && wakes==0 && delays==0); // ordinary behavior unchanged

    reset();decode_worker_request();replay=false;prepare_cost=35000;run_worker();
    assert(prepares==2 && delays==2); // keep the ordinary long-work protection

    reset();decode_worker_request();decode_help_service(now_us);
    assert(priority==5 && s_decode_help.active);now_us+=4000;run_worker();
    assert(prepares==1 && wakes==1 && delays==0 && ready_decode_slot_count(3)==1);
    atomic_store(&s_decode_stop_requested,false);nuwax_emotion_animation_player_service_decode();
    assert(priority==4 && s_decode_help_ready==1 && !s_decode_help.active && !help_pending);
    assert(help_outcome==NUWAX_REPLAY_UI_HELP_READY);
    assert(atomic_load(&s_decode_request_wait_us)==4000);
    assert(atomic_load(&s_decode_worker_state)==DECODE_WORKER_REQUESTED);
    assert(notifies==2); // second slot primed before the display deadline
    nuwax_emotion_animation_player_service_decode();assert(notifies==2);
    atomic_store(&s_decode_worker_state,DECODE_WORKER_READY);
    atomic_store(&slots[1].state,DECODE_SLOT_READY);slots[1].generation=3;
    nuwax_emotion_animation_player_service_decode();assert(notifies==2); // both full
    atomic_store(&slots[1].state,DECODE_SLOT_FREE);s_decoder.frame_index=100;
    nuwax_emotion_animation_player_service_decode();assert(notifies==2); // segment end
    s_decoder.frame_index=1;atomic_store(&s_decode_paused,true);
    nuwax_emotion_animation_player_service_decode();assert(notifies==2); // paused
    atomic_store(&s_decode_paused,false);replay=false;
    nuwax_emotion_animation_player_service_decode();assert(notifies==2); // ordinary path

    reset();decode_worker_request();hook=1;prepare_cost=35000;run_worker();
    assert(prepares==1 && wakes==1 && delays==0); // boost arriving mid-frame
    decode_help_cancel();assert(priority==4 && !atomic_load(&s_decode_help_frame));
    assert(help_outcome==NUWAX_REPLAY_UI_HELP_CANCELLED);

    reset();decode_worker_request();hook=6;prepare_cost=35000;run_worker();
    assert(prepares==1 && wakes==1 && delays==0 && priority==4 && s_decode_help_expired==1);

    reset();decode_worker_request();decode_help_service(now_us);hook=2;prepare_cost=13000;run_worker();
    assert(priority==4 && s_decode_help_expired==1 && !s_decode_help.active);
    assert(help_outcome==NUWAX_REPLAY_UI_HELP_EXPIRED);
    assert(prepares==1 && delays==0); // saved hint cannot fill a second slot
    atomic_store(&s_decode_stop_requested,false);atomic_store(&s_decode_worker_state,DECODE_WORKER_REQUESTED);
    for(unsigned i=0;i<2;i++)atomic_store(&slots[i].state,DECODE_SLOT_FREE);
    now_us=s_decode_help.next_grant_us-1;decode_help_service(now_us);assert(!s_decode_help.active);
    now_us++;decode_help_service(now_us);assert(s_decode_help.active);

    // Every eligibility gate must restore an outstanding priority immediately.
    for(unsigned gate=0;gate<9;gate++) {
        reset();decode_worker_request();decode_help_service(now_us);assert(priority==5);
        switch(gate){case 0:replay=false;break;case 1:atomic_store(&s_decode_paused,true);break;
        case 2:atomic_store(&s_queued_asset_id,9);break;case 3:s_component_bridge.active=true;break;
        case 4:atomic_store(&s_decode_stop_requested,true);break;case 5:atomic_fetch_add(&s_decode_generation,1);break;
        case 6:s_segment_advance_pending=true;break;case 7:s_holding_frame=true;break;case 8:entry.fps=0;break;}
        decode_help_service(now_us+1);assert(priority==4 && !s_decode_help.active);
        assert(help_outcome==NUWAX_REPLAY_UI_HELP_CANCELLED);
    }
    reset();decode_worker_request();decode_help_service(now_us);hook=3;run_worker();
    assert(ready_decode_slot_count(3)==0 && prepares==1);decode_help_cancel();assert(priority==4);
    for(unsigned failure=4;failure<=5;failure++) {
        reset();decode_worker_request();decode_help_service(now_us);hook=failure;run_worker();
        assert(prepares==1 && ready_decode_slot_count(3)==0 && wakes==0);
        atomic_store(&s_decode_stop_requested,false);nuwax_emotion_animation_player_service_decode();
        assert(priority==4 && !s_decode_help.active);
    }

    reset();decode_worker_request();s_next_frame_us=now_us+25001;decode_help_service(now_us);assert(priority==4);
    now_us++;decode_help_service(now_us);assert(priority==5);
    decode_help_cancel();assert(s_decode_help.next_grant_us==0 && priority==4);

    // An empty queue immediately after display no longer waits 5 ms for help.
    reset();decode_worker_request();s_next_frame_us=now_us+25000;prepare_cost=9000;
    decode_help_service(now_us);assert(priority==5 && s_decode_help.active);
    run_worker();assert(prepares==1 && wakes==1 && now_us<s_next_frame_us);

    // A 40 FPS frame with 18 ms left used to wait another 6 ms before help.
    // One 9 ms decode now fits before its deadline, without raising priority
    // or extending the 12 ms lease / one-frame-per-period cap.
    reset();decode_worker_request();s_next_frame_us=now_us+18000;prepare_cost=9000;
    decode_help_service(now_us);assert(priority==5 && s_decode_help.until_us==now_us+12000);
    run_worker();assert(prepares==1 && wakes==1 && now_us<s_next_frame_us);

    // The 32-bit request clock wraps after ~71 minutes; elapsed subtraction does not.
    reset();now_us=0xfffffff0LL;decode_worker_request();now_us+=200;run_worker();
    assert(atomic_load(&s_decode_request_wait_us)==200);
    reset();now_us=9LL*3600*1000000;s_next_frame_us=now_us;decode_worker_request();
    decode_help_service(now_us);assert(priority==5 && s_decode_help.until_us>now_us);
    decode_help_service(now_us+12000);assert(priority==4 && s_decode_help_expired==1);

    puts("PASS production decode scheduler: normal batching, one-frame help, mid-frame preemption, expiry, cooldown, pause/switch/stop/generation, late uptime and clock wrap");
    return 0;
}
'''
names=['decode_help_finish','decode_help_cancel','decode_help_service','nuwax_emotion_animation_player_service_decode','decode_worker_request','decode_worker_task']
parts={name:extract(name) for name in names}
generated=out/'schedule.c'
generated.write_text(support+'\n'.join(parts.values())+tests,encoding='utf-8')
exe=out/'schedule.exe'
subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-I',str(path.parent),'-I',str(root/'main'),str(generated),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
(out/'report.json').write_text(json.dumps({'hardwareValidated':False,'productionFunctions':{
    name:hashlib.sha256(code.encode()).hexdigest() for name,code in parts.items()},
    'sourceSha256':hashlib.sha256(path.read_bytes()).hexdigest(),
    'policySha256':hashlib.sha256((path.parent/'emotion_decode_schedule.h').read_bytes()).hexdigest(),
    'priority':5,'leaseUs':12000,'oneFrame':True,'normalBehaviorPreserved':True},indent=2),encoding='utf-8')
