"""Production sender + scheduler, with a saved 146 negative control.

Exercise tick phase and owner outcomes. This is not a hardware speed benchmark.
"""
from pathlib import Path
import argparse, hashlib, json, re, subprocess

p = argparse.ArgumentParser()
p.add_argument('--baseline', type=Path, required=True)
a = p.parse_args()
r = Path(__file__).resolve().parents[1]
out = r / 'build-host-checks/replay-pending-help'
out.mkdir(parents=True, exist_ok=True)
read = lambda p: p.read_text(encoding='utf-8-sig')
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()

def block(s):
    start = s.index('/* Decide fairness before waiting', s.index('static esp_err_t replay_send_windowed('))
    return s[start:s.index('\n            }\n            if (err != ESP_OK)', start)]

support = r'''
#define ESP_PLATFORM 1
#include <assert.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include "offline_io_schedule.h"
#include "ws_send_metrics.h"
#define ESP_OK 0
#define pdTRUE 1
#define pdFALSE 0
#define portTICK_PERIOD_MS 10U
typedef uint32_t TickType_t;
typedef struct { unsigned bits; } StaticEventGroup_t;
typedef StaticEventGroup_t *EventGroupHandle_t;
static uint64_t now_us, ready_at, cancel_at, replace_at;
static unsigned priority=5,wait_calls,help_waits,fair_waits,delays;
static bool idle_wait,stale_once,clear_complete;
static bool (*idle_cb)(void);
static uint32_t served_help,ui_handoffs,fairness_idle_wakes,fairness_timer_wakes;
static uint32_t fairness_shared_ui,fairness_ui_pending;
static uint32_t fairness_ui_ready,fairness_ui_expired,fairness_ui_cancelled,send_wait_credits;
static uint64_t ui_handoff_us,fairness_wait_us;
static offline_replay_budget_t budget;
static int64_t esp_timer_get_time(void) {return now_us;}
static TickType_t xTaskGetTickCount(void) {return (TickType_t)(now_us/10000);}
static void vTaskDelay(unsigned n) {assert(n==1);now_us=(now_us/10000+1)*10000;delays++;}
static int xPortGetCoreID(void) {return 0;}
static unsigned uxTaskPriorityGet(void*t) {(void)t;return priority;}
static void publish(nuwax_replay_ui_help_state_t state) {
#if OLD
    nuwax_replay_ui_set_help_state(state);
#else
    nuwax_replay_ui_set_help_state(state, state==NUWAX_REPLAY_UI_HELP_REQUESTED?(uint32_t)(now_us+12000):0);
#endif
}
static unsigned snapshot(uint32_t *g, uint32_t *deadline) {
#if OLD
    (void)deadline;return nuwax_replay_ui_help_snapshot(g);
#else
    return nuwax_replay_ui_help_snapshot(g,deadline);
#endif
}
static EventGroupHandle_t xEventGroupCreateStatic(StaticEventGroup_t*s) {s->bits=0;return s;}
static unsigned xEventGroupSetBits(EventGroupHandle_t e,unsigned b) {e->bits|=b;return e->bits;}
static unsigned xEventGroupClearBits(EventGroupHandle_t e,unsigned b) {
    unsigned old=e->bits;e->bits&=~b;
    if(b==2 && clear_complete) {clear_complete=false;publish(NUWAX_REPLAY_UI_HELP_READY);}
    return old;
}
static unsigned xEventGroupWaitBits(EventGroupHandle_t e,unsigned b,int clear,int all,unsigned ticks) {
    assert(clear==1&&all==0&&ticks==1);wait_calls++;
    if(b==2)help_waits++;else {assert(b==1);fair_waits++;}
    if(b==2 && stale_once){stale_once=false;return b;}
    const uint64_t tick_end=(now_us/10000+1)*10000;
    uint64_t event_at=ready_at;
    if(cancel_at<event_at) event_at=cancel_at;
    if(replace_at<event_at) event_at=replace_at;
    if(event_at<=tick_end) {
        now_us=event_at;
        if(ready_at==event_at){ready_at=UINT64_MAX;publish(NUWAX_REPLAY_UI_HELP_READY);}
        if(cancel_at==event_at){cancel_at=UINT64_MAX;publish(NUWAX_REPLAY_UI_HELP_CANCELLED);}
        if(replace_at==event_at){replace_at=UINT64_MAX;publish(NUWAX_REPLAY_UI_HELP_CANCELLED);publish(NUWAX_REPLAY_UI_HELP_REQUESTED);}
        if(b==1) now_us=tick_end; // UI alone cannot waive mandatory fairness
    } else now_us=tick_end;
    if(b==1 && idle_wait)(void)idle_cb();
    unsigned result=e->bits&b; e->bits&=~b;return result;
}
static int esp_register_freertos_idle_hook_for_cpu(bool(*cb)(void),int core) {assert(core>=0 && core<2);idle_cb=cb;return ESP_OK;}
'''

tests = r'''
static void reset(uint64_t start) {
    now_us=start;priority=5;wait_calls=help_waits=fair_waits=delays=0;
    ready_at=cancel_at=replace_at=UINT64_MAX;
    idle_wait=stale_once=clear_complete=false;
    served_help=ui_handoffs=fairness_idle_wakes=fairness_timer_wakes=0;
    fairness_shared_ui=fairness_ui_pending=0;
    fairness_ui_ready=fairness_ui_expired=fairness_ui_cancelled=send_wait_credits=0;
    ui_handoff_us=fairness_wait_us=0;budget=(offline_replay_budget_t){0};
    atomic_store(&s_idle_observers[0].epoch,0);atomic_store(&s_ui_epoch,0);
    atomic_store(&s_help_sequence,0);atomic_store(&s_help_generation,0);
    publish(NUWAX_REPLAY_UI_HELP_NONE);s_idle_observers[0].event->bits=0;
}
int main(void) {
    nuwax_replay_schedule_init();
    // First tick ends after only 1ms. The frame needs 5ms from the grant.
    for(unsigned shared=0;shared<2;shared++) {
        reset(9000);publish(NUWAX_REPLAY_UI_HELP_REQUESTED);ready_at=14000;
        if(shared)budget.bytes=32768;
        sender(shared?32768:8192,1000,0);
        assert(now_us==10000 && wait_calls==1);
        assert(shared ? fairness_ui_pending==1 && help_waits==0 : help_waits==1);
        now_us=11000;sender(8192,1000,0);
#if OLD
        assert(now_us==11000 && wait_calls==1 && served_help==1);
        uint32_t g;assert(snapshot(&g,NULL)==NUWAX_REPLAY_UI_HELP_REQUESTED);
#else
        assert(now_us==14000 && wait_calls==2 && served_help==1);
        uint32_t g;assert(snapshot(&g,NULL)==NUWAX_REPLAY_UI_HELP_READY);
#endif
    }
#if OLD
    puts("PASS negative control: actual 146 consumes unfinished grants after both a short help wait and a shared fairness wait");
#else
    puts("PASS: actual 147 continues both unfinished paths within the ORIGINAL lease; no second wait in a shared batch");
    reset(0);publish(NUWAX_REPLAY_UI_HELP_REQUESTED);ready_at=8000;budget.bytes=32768;
    sender(32768,1000,0);
    assert(now_us==10000 && fair_waits==1 && help_waits==0 && fairness_ui_ready==1);
    assert(!nuwax_replay_ui_handoff(&served_help)); // preserve merged completed path
    reset(0);publish(NUWAX_REPLAY_UI_HELP_REQUESTED);ready_at=8000;sender(8192,1000,0);
    assert(now_us==8000 && help_waits==1 && fair_waits==0 && served_help==1);
    reset(0);publish(NUWAX_REPLAY_UI_HELP_REQUESTED);sender(8192,1000,0);
    assert(now_us==10000 && served_help==0);
    now_us=12000;assert(!nuwax_replay_ui_handoff(&served_help) && served_help==1);
    now_us+=UINT32_MAX+1ULL;assert(!nuwax_replay_ui_handoff(&served_help)); // stale grant cannot revive
    reset(0);publish(NUWAX_REPLAY_UI_HELP_REQUESTED);sender(8192,1000,0);
    now_us=11000;publish(NUWAX_REPLAY_UI_HELP_REQUESTED); // duplicate publish must not renew
    uint32_t g,until;assert(snapshot(&g,&until)==NUWAX_REPLAY_UI_HELP_REQUESTED && until==12000 && g==1);
    now_us=12001;assert(!nuwax_replay_ui_handoff(&served_help));
    reset(0);publish(NUWAX_REPLAY_UI_HELP_REQUESTED);sender(8192,1000,0);sender(8192,1000,0);
    assert(now_us==20000 && help_waits==2 && served_help==1); // tick quantization, not lease extension
    for(unsigned i=0;i<100;i++) assert(!nuwax_replay_ui_handoff(&served_help));
    reset(0);publish(NUWAX_REPLAY_UI_HELP_REQUESTED);clear_complete=true;sender(8192,1000,0);
    assert(help_waits==0 && now_us==0 && served_help==1);
    reset(0);publish(NUWAX_REPLAY_UI_HELP_REQUESTED);stale_once=true;ready_at=8000;sender(8192,1000,0);
    assert(help_waits==2 && now_us==8000 && served_help==1);
    reset(9000);publish(NUWAX_REPLAY_UI_HELP_REQUESTED);cancel_at=9500;sender(8192,1000,0);
    assert(help_waits==1 && now_us==9500 && served_help==1 && !nuwax_replay_ui_handoff(&served_help));
    reset(9000);publish(NUWAX_REPLAY_UI_HELP_REQUESTED);replace_at=9500;sender(8192,1000,0);
    assert(help_waits==1 && now_us==9500 && served_help!=2);
    ready_at=9800;sender(8192,1000,0);assert(help_waits==2 && now_us==9800 && served_help==2);
    reset(9000);publish(NUWAX_REPLAY_UI_HELP_REQUESTED);replace_at=9500;budget.bytes=32768;
    sender(32768,1000,0);assert(fair_waits==1 && help_waits==0 && fairness_ui_ready==0);
    ready_at=11000;sender(8192,1000,0);assert(help_waits==1 && served_help==2);
    reset(0);publish(NUWAX_REPLAY_UI_HELP_REQUESTED);ready_at=8000;sender(8192,1000,10000);
    assert(send_wait_credits==1 && fair_waits==0 && help_waits==1);
    reset(0);publish(NUWAX_REPLAY_UI_HELP_REQUESTED);idle_wait=true;budget.bytes=32768;
    sender(32768,1000,0);assert(fairness_idle_wakes==1 && fairness_timer_wakes==0 && help_waits==0);
    reset(0);publish(NUWAX_REPLAY_UI_HELP_REQUESTED);atomic_fetch_add(&s_help_sequence,1);
    assert(!nuwax_replay_ui_handoff(&served_help) && served_help==0);atomic_fetch_add(&s_help_sequence,1);
    reset(0);publish(NUWAX_REPLAY_UI_HELP_REQUESTED);EventGroupHandle_t saved=s_idle_observers[0].event;s_idle_observers[0].event=NULL;
    sender(8192,1000,0);assert(delays==1 && served_help==0);
    now_us=12000;assert(!nuwax_replay_ui_handoff(&served_help));s_idle_observers[0].event=saved;
    // All 100Hz tick phases and both low-microsecond/tick counter wraps.
    uint64_t starts[]={0,UINT32_MAX-9999ULL,(uint64_t)UINT32_MAX*10000-9999ULL};
    for(unsigned wrap=0;wrap<3;wrap++) for(unsigned phase=0;phase<10000;phase++) {
        const uint64_t start=starts[wrap]+phase;reset(start);publish(NUWAX_REPLAY_UI_HELP_REQUESTED);
        unsigned waits=0;
        while(nuwax_replay_ui_handoff(&served_help)) {
            assert(++waits<=3 && now_us<start+22000); // original 12ms lease plus <1 tick quantization
        }
        assert(now_us>=start+12000 && served_help==1 && priority==5);
    }
    // Pending UI never erases the byte guard, and waiting never holds a task pointer.
    for(unsigned i=0;i<10000;i++) {
        reset(i);publish(NUWAX_REPLAY_UI_HELP_REQUESTED);budget.bytes=32768;sender(32768,1000,0);
        assert(wait_calls==1 && help_waits==0 && priority==5 && atomic_load(&s_idle_observers[0].wait_state)==0);
    }
    puts("PASS: completion/cancel/replacement, fixed expiry, duplicate publish, stale/clear race, torn snapshot, absent event, socket/IDLE credit; 30000 phase/wrap and 10000 fairness cases");
#endif
    return 0;
}
'''

hashes = {}
for label, base in [('old', a.baseline), ('new', r)]:
    body = block(read(base / 'main/offline_audio_spool.c'))
    module = re.sub(r'^#include.*$', '', read(base / 'main/replay_ui_schedule.c'), flags=re.M)
    wrap = '''static void sender(size_t n,uint64_t work,uint64_t wait) {
        nuwax_ws_send_metrics_t metrics={.operation_wait_max_us=wait};
        const uint64_t packet_us=work,encode_before=0;
        uint32_t tx_groups[4]={0};size_t frame_count=1,sent=0;
        int64_t first_payload_us=-1,started=esp_timer_get_time();
        for(unsigned step=0;step<1;step++) {
    ''' + body + '''
        }
        assert(sent==n && tx_groups[0]==1 && first_payload_us>=0 && priority==5);
    }
    '''
    c = out / (label + '.c'); exe = c.with_suffix('.exe')
    cases = tests
    if label == 'old':
        for current, old_name in [('s_idle_observers[0].wait_state','s_idle_wait_state'),
                                  ('s_idle_observers[0].epoch','s_idle_epoch'),
                                  ('s_idle_observers[0].state','s_observer_state'),
                                  ('s_idle_observers[0].event','s_idle_event')]:
            cases = cases.replace(current, old_name)
    c.write_text(support + module + wrap + cases, encoding='utf-8')
    subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror',
        '-Wno-unused-function', '-DOLD=' + str(int(label=='old')),
        '-I',str(base / 'main'),str(c),'-o',str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    hashes[label + 'BlockSha256'] = hashlib.sha256(body.encode()).hexdigest()
names = ['main/offline_audio_spool.c','main/replay_ui_schedule.c','main/replay_ui_schedule.h',
         'main/offline_io_schedule.h','main/nuwax_ui_v28/face/emotion_decode_schedule.h',
         'main/nuwax_ui_v28/face/emotion_animation_player.c']
(out / 'report.json').write_text(json.dumps(dict(hardwareValidated=False,
    negativeControlVersion='7.0.146', tickPhaseCases=30000, fairnessCases=10000,
    sourceHashes={n:sha(r/n) for n in names}, **hashes), indent=2), encoding='utf-8')
