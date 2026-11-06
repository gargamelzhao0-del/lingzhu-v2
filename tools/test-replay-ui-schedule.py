"""Production scheduling/wakeup tests. Does not claim device timing or speed."""
from pathlib import Path
import hashlib, json, re, subprocess

root = Path(__file__).resolve().parents[1]
out = root/'build-host-checks/replay-ui-schedule'
out.mkdir(parents=True, exist_ok=True)

def extract(source, name):
    m = re.search(r'^(?:static )?[\w* ]+\b'+name+r'\([^;{}]*\)\s*\{', source, re.M)
    assert m, name
    at = source.index('{', m.start())+1
    depth=1
    while depth:
        depth+=(source[at]=='{')-(source[at]=='}'); at+=1
    return source[m.start():at]

def run(name, source):
    path=out/(name+'.c'); path.write_text(source,encoding='utf-8')
    exe=out/(name+'.exe')
    subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror',
        '-I',str(root/'main'),str(path),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)

module=(root/'main/replay_ui_schedule.c').read_text(encoding='utf-8')
module=re.sub(r'^#include.*$', '', module, flags=re.M)
run('progress',r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include "offline_io_schedule.h"
#include "replay_ui_schedule.h"
#include "ui_memory_pressure.h"
#define ESP_OK 0
#define pdTRUE 1
#define pdFALSE 0
typedef uint32_t TickType_t;
typedef struct { unsigned bits; } StaticEventGroup_t;
typedef StaticEventGroup_t *EventGroupHandle_t;
static int core, registration_error;
static unsigned priority=5, registrations,yields;
static unsigned priority_changes;
static bool (*idle_cb)(void);
static uint32_t tick;
static unsigned wait_case,wait_calls,delay_calls,event_sets;
static uint64_t elapsed_us;
static int64_t esp_timer_get_time(void) {return elapsed_us;}
static bool create_failure;
static TickType_t xTaskGetTickCount(void) {return tick;}
static void vTaskDelay(unsigned ticks) {
    assert(ticks==1);delay_calls++;tick++;elapsed_us+=9000;
}
static EventGroupHandle_t xEventGroupCreateStatic(StaticEventGroup_t *s) {
    s->bits=0;return create_failure?NULL:s;
}
static unsigned xEventGroupClearBits(EventGroupHandle_t e,unsigned b) {
    unsigned old=e->bits;e->bits&=~b;
    if(wait_case==1){wait_case=0;assert(idle_cb());} // drain before arm
    return old;
}
static unsigned xEventGroupSetBits(EventGroupHandle_t e,unsigned b) {
    event_sets++;e->bits|=b;return e->bits;
}
static unsigned xEventGroupWaitBits(EventGroupHandle_t e,unsigned b,int clear,int all,unsigned ticks) {
    assert(clear==pdTRUE && all==pdFALSE && ticks==1);wait_calls++;
    if(wait_case==2) {elapsed_us+=600;assert(!idle_cb());} // actual idle -> early wake
    else if(wait_case==3) {nuwax_replay_ui_frame_done();tick++;elapsed_us+=9000;}
    else if(wait_case==4) {e->bits|=b;} // stale signal, no IDLE progress
    else {tick++;elapsed_us+=9000;} // overloaded CPU, original timeout
    unsigned result=e->bits;e->bits&=~b;return result;
}
static int xPortGetCoreID(void) {return core;}
static unsigned uxTaskPriorityGet(void *t) {(void)t;return priority;}
static int esp_register_freertos_idle_hook_for_cpu(bool(*cb)(void),int c) {
    assert(c>=0 && c<2);registrations++;idle_cb=cb;return registration_error;
}
'''+module+r'''
int main(void) {
    nuwax_ui_memory_pressure_t pressure={0};
    assert(nuwax_replay_dma_loan_bytes(0)==0);
    nuwax_replay_dma_loan_begin(8192,1000);
    assert(nuwax_replay_dma_loan_bytes(1001)==8192);
    assert(!nuwax_ui_memory_release_due(&pressure,1001,18435,24000,8192));
    assert(!nuwax_ui_memory_release_due(&pressure,11001,18435,24000,8192));
    // Critical and unrelated pressure never waits for the grace period.
    assert(nuwax_ui_memory_release_due(&pressure,11002,16383,24000,8192));
    assert(nuwax_ui_memory_release_due(&pressure,11003,17000,24000,4096));
    assert(nuwax_ui_memory_release_due(&pressure,11004,18435,24000,0));
    assert(!nuwax_ui_memory_release_due(&pressure,20000,18435,24000,8192));
    assert(!nuwax_ui_memory_release_due(&pressure,69999,18435,24000,8192));
    assert(nuwax_ui_memory_release_due(&pressure,70000,18435,24000,8192));
    // Renewing a DMA loan cannot reset the owner's 50 ms deadline.
    nuwax_replay_dma_loan_begin(8192,69999);
    assert(nuwax_ui_memory_release_due(&pressure,70001,18435,24000,nuwax_replay_dma_loan_bytes(70001)));
    assert(!nuwax_ui_memory_release_due(&pressure,70002,26627,24000,0) && !pressure.deadline_us);
    assert(nuwax_replay_dma_loan_bytes(119999)==0); // stale SD operation
    nuwax_replay_dma_loan_end();assert(nuwax_replay_dma_loan_bytes(0)==0);
    nuwax_replay_dma_loan_begin(8192,UINT32_MAX-99);
    assert(nuwax_replay_dma_loan_bytes(100)==8192);
    assert(nuwax_replay_dma_loan_bytes(50000)==0);
    nuwax_replay_dma_loan_begin(8193,0);assert(nuwax_replay_dma_loan_bytes(1)==0);
    atomic_fetch_add(&s_dma_loan_sequence,1);assert(nuwax_replay_dma_loan_bytes(1)==0);
    atomic_fetch_add(&s_dma_loan_sequence,1);nuwax_replay_dma_loan_end();
    puts("PASS: bounded DMA pressure hint, critical/unrelated/stale rejection, 50ms deadline cannot renew, recovery and clock wrap");
    uint32_t idle,ui;
    assert(!nuwax_replay_schedule_snapshot(&idle,&ui));
    nuwax_replay_schedule_init();nuwax_replay_schedule_init();assert(registrations==2);
    assert(nuwax_replay_schedule_snapshot(&idle,&ui)&&idle==0&&ui==0);
    assert(idle_cb());nuwax_replay_ui_frame_done();
    assert(nuwax_replay_schedule_snapshot(&idle,&ui)&&idle==1&&ui==1);
    // Actual sender policy: only observed IDLE progress permits early return.
    wait_case=2;elapsed_us=0;
    assert(nuwax_replay_schedule_yield() && elapsed_us==600 && delay_calls==0);
    assert(wait_calls==1 && event_sets==1 && atomic_load(&s_idle_observers[0].wait_state)==0);
    assert(idle_cb() && event_sets==1); // inactive hook never sets an event
    wait_case=1;
    assert(nuwax_replay_schedule_yield() && wait_calls==1); // no lost wake before arm
    wait_case=0;
    assert(!nuwax_replay_schedule_yield() && wait_calls==2 && delay_calls==0);
    wait_case=3;
    assert(!nuwax_replay_schedule_yield() && wait_calls==3 && delay_calls==0); // UI alone insufficient
    wait_case=4;
    assert(!nuwax_replay_schedule_yield() && delay_calls==1); // stale event cannot bypass yield
    tick=UINT32_MAX;wait_case=0;
    assert(!nuwax_replay_schedule_yield() && tick==0 && delay_calls==1);
    atomic_store(&s_idle_observers[0].epoch,UINT32_MAX);wait_case=2;
    assert(nuwax_replay_schedule_yield() && atomic_load(&s_idle_observers[0].epoch)==0);
    core=2;assert(!nuwax_replay_schedule_yield() && delay_calls==2);core=0;
    priority=6;assert(!nuwax_replay_schedule_yield() && delay_calls==3);priority=5;
    atomic_store(&s_idle_observers[0].wait_state,1);
    assert(!nuwax_replay_schedule_yield() && delay_calls==4 && atomic_load(&s_idle_observers[0].wait_state)==1);
    atomic_store(&s_idle_observers[0].wait_state,0);
    EventGroupHandle_t saved_event=s_idle_observers[0].event;s_idle_observers[0].event=NULL;
    assert(!nuwax_replay_schedule_yield() && delay_calls==5);s_idle_observers[0].event=saved_event;
    // Repeated success/timeout/shutdown leaves no task handle or waiter state.
    for(unsigned i=0;i<10000;i++) {
        wait_case=(i&1)?0:2;
        assert(nuwax_replay_schedule_yield()==!(i&1));
        assert(atomic_load(&s_idle_observers[0].wait_state)==0 && !s_idle_observers[0].event->bits && priority==5);
    }
    puts("PASS production idle yield: real idle wakes early; UI/stale event do not; before-arm drain, timer and epoch wrap, wrong core/priority, missing event, duplicate waiter, 10000 repeated cycles");
    core=1;assert(nuwax_replay_schedule_snapshot(&idle,&ui) && ui==0);core=0;
    uint32_t served=0;
    assert(!nuwax_replay_ui_handoff(&served));
    nuwax_replay_ui_set_help_state(NUWAX_REPLAY_UI_HELP_REQUESTED, (uint32_t)elapsed_us + 12000U);
    assert(nuwax_replay_ui_handoff(&served));
    assert(priority==5 && priority_changes==0 && yields==0);
    assert(served==0); // one tick ended while the original lease still has time
    assert(nuwax_replay_ui_handoff(&served));
    for(unsigned i=0;i<100;i++) assert(!nuwax_replay_ui_handoff(&served));
    nuwax_replay_ui_set_help_state(NUWAX_REPLAY_UI_HELP_REQUESTED, (uint32_t)elapsed_us + 12000U); // same grant is not renewed
    assert(!nuwax_replay_ui_handoff(&served));
    nuwax_replay_ui_set_help_state(NUWAX_REPLAY_UI_HELP_CANCELLED, 0U);nuwax_replay_ui_set_help_state(NUWAX_REPLAY_UI_HELP_REQUESTED, (uint32_t)elapsed_us + 12000U);
    core=1;assert(!nuwax_replay_ui_handoff(&served));core=0;
    priority=6;assert(!nuwax_replay_ui_handoff(&served));priority=5;
    assert(!nuwax_replay_ui_handoff(NULL));
    assert(nuwax_replay_ui_handoff(&served));
    assert(priority==5 && priority_changes==0 && yields==0);
    nuwax_replay_ui_set_help_state(NUWAX_REPLAY_UI_HELP_CANCELLED, 0U);atomic_store(&s_help_generation,UINT32_MAX);
    served=UINT32_MAX;nuwax_replay_ui_set_help_state(NUWAX_REPLAY_UI_HELP_REQUESTED, (uint32_t)elapsed_us + 12000U);
    assert(nuwax_replay_ui_handoff(&served) && served==UINT32_MAX && priority==5);
    assert(nuwax_replay_ui_handoff(&served) && served==0);
    assert(!nuwax_replay_ui_handoff(&served));
    nuwax_replay_ui_set_help_state(NUWAX_REPLAY_UI_HELP_CANCELLED, 0U);
    uint32_t help_generation;
    for(unsigned state=NUWAX_REPLAY_UI_HELP_READY;state<=NUWAX_REPLAY_UI_HELP_CANCELLED;state++) {
        nuwax_replay_ui_set_help_state((nuwax_replay_ui_help_state_t)state, 0U);
        assert(nuwax_replay_ui_help_snapshot(&help_generation, NULL)==state);
        assert(!nuwax_replay_ui_handoff(&served));
    }
    assert(nuwax_replay_ui_help_snapshot(NULL, NULL)==NUWAX_REPLAY_UI_HELP_NONE);
    nuwax_replay_ui_set_help_state(NUWAX_REPLAY_UI_HELP_REQUESTED, (uint32_t)elapsed_us + 12000U);
    atomic_fetch_add(&s_help_sequence,1); // interrupted owner update
    assert(nuwax_replay_ui_help_snapshot(&help_generation, NULL)==NUWAX_REPLAY_UI_HELP_NONE);
    assert(!nuwax_replay_ui_handoff(&served));
    atomic_fetch_add(&s_help_sequence,1);
    assert(nuwax_replay_ui_handoff(&served));
    nuwax_replay_ui_set_help_state(NUWAX_REPLAY_UI_HELP_CANCELLED, 0U);
    atomic_store(&s_idle_observers[0].epoch,UINT32_MAX);assert(idle_cb());
    assert(nuwax_replay_schedule_snapshot(&idle,&ui)&&idle==0);
    atomic_store(&s_idle_observers[0].state,0);registration_error=-1;
    nuwax_replay_schedule_init();nuwax_replay_schedule_init();assert(registrations==3);
    assert(!nuwax_replay_schedule_snapshot(&idle,&ui));
    assert(!nuwax_replay_schedule_yield() && delay_calls==6); // hook registration failure

    // CPU1 works independently even if CPU0 registration failed. CPU0 UI or
    // IDLE progress must never be credited to the CPU1 TLS sender.
    core=1;wait_case=2;
    uint32_t idle1,ui1;
    assert(nuwax_replay_schedule_snapshot(&idle1,&ui1) && ui1==0);
    core=0;assert(idle_cb());nuwax_replay_ui_frame_done();core=1;
    uint32_t after1;
    assert(nuwax_replay_schedule_snapshot(&after1,&ui1) && after1==idle1 && ui1==0);
    assert(nuwax_replay_schedule_yield());
    assert(nuwax_replay_schedule_snapshot(&after1,&ui1) && after1!=idle1 && ui1==0);
    wait_case=4;unsigned old_delays=delay_calls;
    assert(!nuwax_replay_schedule_yield() && delay_calls==old_delays+1);
    assert(!nuwax_replay_schedule_snapshot(NULL,&ui1));
    // A failed event allocation is permanent and does not register a hook.
    atomic_store(&s_idle_observers[1].state,0);create_failure=true;
    unsigned old_registrations=registrations;nuwax_replay_schedule_init();
    assert(registrations==old_registrations);
    assert(!nuwax_replay_schedule_snapshot(&idle1,&ui1));
    assert(!nuwax_replay_schedule_yield());core=0;

    offline_replay_budget_t b={0};
    assert(!offline_replay_budget_account(&b,32768,1000,true,1,1));
    // UI progress may erase preempted wall time, NEVER the byte bound.
    assert(offline_replay_budget_account(&b,32768,50000,true,1,2));
    assert(b.yields==1&&b.ui_credits==1&&b.idle_credits==0);
    assert(!offline_replay_budget_account(&b,32768,1000,true,1,2));
    assert(!offline_replay_budget_account(&b,32768,1000,true,2,2));
    assert(b.bytes==32768&&b.idle_credits==1);
    assert(offline_replay_budget_account(&b,32768,1000,true,2,2));
    // Neither stale UI progress nor old IDLE progress can be spent twice.
    assert(offline_replay_budget_account(&b,8192,20000,true,2,2));
    assert(b.ui_credits==1&&b.idle_credits==1);
    b=(offline_replay_budget_t){0};
    assert(!offline_replay_budget_account(&b,8192,1000,true,UINT32_MAX,UINT32_MAX));
    assert(!offline_replay_budget_account(&b,8192,50000,true,0,0));
    assert(b.idle_credits==1&&b.ui_credits==1&&b.bytes==8192&&b.work_us==0);
    // Actual same-core idle within a 45ms TLS call waives its wall-time
    // charge, but not the current frame's bytes; the credit is single-use.
    offline_replay_budget_t tls_budget={0};
    assert(offline_replay_budget_account(&tls_budget,8064,45000,true,10,0));
    assert(!offline_replay_budget_account(&tls_budget,8064,45000,true,11,0));
    assert(tls_budget.bytes==8064 && tls_budget.work_us==0 && tls_budget.idle_credits==1);
    assert(offline_replay_budget_account(&tls_budget,8064,45000,true,11,0));
    // Missing hook/unsupported core retains the original bounded policy.
    assert(offline_replay_budget_account(&b,8192,20000,false,0,0));
    assert(!offline_replay_budget_account(&b,8192,1000,true,10,10));
    assert(b.ui_credits==1&&b.idle_credits==1);
    b=(offline_replay_budget_t){0};
    for(unsigned i=0;i<10000;i++)
        assert(offline_replay_budget_account(&b,8192,40000,true,0,i)==(i==0||i%8==0));
    assert(b.bytes<65536&&b.yields==1250);
    puts("PASS production observer: once-only init, failure fallback, CPU affinity, sender-only handoff; UI never erases 64KiB guard, idle credit consumed once, wrap and 10000-step fairness");
}
''')

lvgl=(root/'main/nuwax_lvgl_port.c').read_text(encoding='utf-8-sig')
run('wake',r'''
#include <assert.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#define ESP_OK 0
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#define LVGL_EXTERNAL_WAKE_STREAK_LIMIT 8U
typedef void *TaskHandle_t;
static TaskHandle_t s_lvgl_task=(void*)1,s_wake_timer=(void*)2;
static atomic_uint s_external_wake_generation;
static unsigned s_external_wake_serviced,s_external_wake_streak;
static atomic_bool s_wake_timer_fired;
static unsigned tokens,blocks,starts,delays,inject;
static int timer_error;
void nuwax_lvgl_port_wake(void);
static void xTaskNotifyGive(void *task) {assert(task==s_lvgl_task);tokens++;}
static unsigned ulTaskNotifyTake(int clear,unsigned timeout) {
    assert(clear==1);
    unsigned n=tokens;tokens=0;
    if(!timeout) {
        if(inject==1){inject=0;nuwax_lvgl_port_wake();}
        return n;
    }
    blocks++;
    if(!n)atomic_store(&s_wake_timer_fired,true); // finite one-shot timer
    return n?n:1;
}
static int esp_timer_stop(void *t){assert(t==s_wake_timer);return 0;}
static int esp_timer_start_once(void*t,uint64_t us){
    assert(t==s_wake_timer&&us==4000);starts++;
    if(inject==2){inject=0;nuwax_lvgl_port_wake();}
    return timer_error;
}
static unsigned delay_ticks_min1(unsigned ms){assert(ms==4);return 1;}
static void vTaskDelay(unsigned ticks){assert(ticks==1);delays++;}
'''+extract(lvgl,'nuwax_lvgl_port_wake')+extract(lvgl,'lvgl_wait_until_due')+r'''
int main(void) {
    nuwax_lvgl_port_wake(); // produced after the owner drained the previous state
    lvgl_wait_until_due(4);assert(blocks==0&&starts==0);
    s_external_wake_serviced=atomic_load(&s_external_wake_generation);
    lvgl_wait_until_due(4);assert(blocks==1&&starts==1);
    inject=1;lvgl_wait_until_due(4);assert(blocks==1&&starts==1);
    s_external_wake_serviced=atomic_load(&s_external_wake_generation);
    inject=2;lvgl_wait_until_due(4);assert(blocks==2&&starts==2);
    assert(atomic_load(&s_external_wake_generation)!=s_external_wake_serviced);
    lvgl_wait_until_due(4);assert(blocks==2);
    s_external_wake_serviced=UINT32_MAX;atomic_store(&s_external_wake_generation,UINT32_MAX);
    nuwax_lvgl_port_wake();lvgl_wait_until_due(4);assert(blocks==2);
    s_external_wake_streak=0;
    for(unsigned i=0;i<8;i++)lvgl_wait_until_due(4);
    assert(delays==1); // event storms still give IDLE0 a bounded opportunity
    s_external_wake_serviced=atomic_load(&s_external_wake_generation);
    timer_error=-1;lvgl_wait_until_due(4);assert(delays==2);
    s_wake_timer=NULL;lvgl_wait_until_due(4);assert(delays==3);
    puts("PASS production LVGL wait: completion during handler/drain/arm, persistent generation, wrap, timer failure and notification-storm fairness");
}
''')

files=['main/replay_ui_schedule.c','main/replay_ui_schedule.h','main/offline_io_schedule.h',
       'main/nuwax_lvgl_port.c']
(out/'report.json').write_text(json.dumps({'hardwareValidated':False,'checks':['progress','wake'],
    'sourceHashes':{p:hashlib.sha256((root/p).read_bytes()).hexdigest() for p in files}},indent=2),encoding='utf-8')
