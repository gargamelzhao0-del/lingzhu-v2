"""Actual sender blocks/scheduler with deterministic RTOS interleavings.

Negative control uses the saved 145 source, not an invented slow implementation.
Elapsed times demonstrate wait overlap only; they are not hardware benchmarks.
"""
from pathlib import Path
import argparse, hashlib, json, re, subprocess

p = argparse.ArgumentParser()
p.add_argument('--baseline', type=Path, required=True)
a = p.parse_args()
r = Path(__file__).resolve().parents[1]
out = r / 'build-host-checks/replay-shared-wait'
out.mkdir(parents=True, exist_ok=True)
read = lambda p: p.read_text(encoding='utf-8-sig')
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()

def block(s, marker):
    start = s.index(marker, s.index('static esp_err_t replay_send_windowed('))
    return s[start:s.index('\n            }\n            if (err != ESP_OK)', start)]

support = r'''
#define ESP_PLATFORM 1
#include <assert.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include "offline_io_schedule.h"
#include "ws_send_metrics.h"
#include "nuwax_ui_v28/face/emotion_decode_schedule.h"
#define ESP_OK 0
#define pdTRUE 1
#define pdFALSE 0
#define portTICK_PERIOD_MS 10U
typedef uint32_t TickType_t;
typedef struct { unsigned bits; } StaticEventGroup_t;
typedef StaticEventGroup_t *EventGroupHandle_t;
static uint64_t now_us;
static unsigned priority=5,wait_calls,help_waits,fair_waits,demotions,delays;
static bool finish, cancel_wait, replace_wait, idle_wait, stale_once, clear_complete, unavailable;
static bool (*idle_cb)(void);
static uint32_t served_help,ui_handoffs,fairness_idle_wakes,fairness_timer_wakes;
static uint32_t fairness_shared_ui,fairness_ui_pending,fairness_followup_ui;
static uint32_t fairness_ui_ready,fairness_ui_expired,fairness_ui_cancelled,send_wait_credits;
static uint64_t ui_handoff_us,fairness_wait_us;
static offline_replay_budget_t budget;
static int64_t esp_timer_get_time(void) {return now_us;}
static TickType_t xTaskGetTickCount(void) {return (TickType_t)(now_us/10000);}
static void vTaskDelay(unsigned n) {assert(n==1);now_us+=10000;delays++;}
static int xPortGetCoreID(void) {return 0;}
static unsigned uxTaskPriorityGet(void*t) {(void)t;return priority;}
static void __attribute__((unused)) vTaskPrioritySet(void*t,unsigned p) {
    assert(!t && ((priority==5&&p==4)||(priority==4&&p==5)));priority=p;
    if(p==4) {demotions++;if(finish){now_us+=8000;nuwax_replay_ui_set_help_state(NUWAX_REPLAY_UI_HELP_READY);}}
}
static EventGroupHandle_t xEventGroupCreateStatic(StaticEventGroup_t*s) {s->bits=0;return unavailable?NULL:s;}
static unsigned xEventGroupSetBits(EventGroupHandle_t e,unsigned b) {e->bits|=b;return e->bits;}
static unsigned xEventGroupClearBits(EventGroupHandle_t e,unsigned b) {
    unsigned old=e->bits;e->bits&=~b;
    if(b==2 && clear_complete) {clear_complete=false;nuwax_replay_ui_set_help_state(NUWAX_REPLAY_UI_HELP_READY);}
    return old;
}
static unsigned xEventGroupWaitBits(EventGroupHandle_t e,unsigned b,int clear,int all,unsigned ticks) {
    assert(clear==1&&all==0&&ticks==1);wait_calls++;
    if(b==2)help_waits++;else {assert(b==1);fair_waits++;}
    if(b==2 && stale_once){stale_once=false;return b;} // no state change / no elapsed tick
    if(finish) {
        now_us+=8000;nuwax_replay_ui_set_help_state(NUWAX_REPLAY_UI_HELP_READY);
        if(b==1)now_us+=2000; // UI completion cannot waive the IDLE guard
    } else if(cancel_wait || replace_wait) {
        now_us+=1000;nuwax_replay_ui_set_help_state(NUWAX_REPLAY_UI_HELP_CANCELLED);
        if(replace_wait)nuwax_replay_ui_set_help_state(NUWAX_REPLAY_UI_HELP_REQUESTED);
        if(b==1)now_us+=9000;
    } else now_us+=10000;
    if(b==1 && idle_wait)(void)idle_cb();
    unsigned result=e->bits&b; e->bits&=~b;return result;
}
static int esp_register_freertos_idle_hook_for_cpu(bool(*cb)(void),int core) {assert(core==0);idle_cb=cb;return ESP_OK;}
'''

tests = r'''
static void reset(void) {
    now_us=0;priority=5;wait_calls=help_waits=fair_waits=demotions=delays=0;
    finish=true;cancel_wait=replace_wait=idle_wait=stale_once=clear_complete=false;
    served_help=ui_handoffs=fairness_idle_wakes=fairness_timer_wakes=0;
    fairness_shared_ui=fairness_ui_pending=fairness_followup_ui=0;
    fairness_ui_ready=fairness_ui_expired=fairness_ui_cancelled=send_wait_credits=0;
    ui_handoff_us=fairness_wait_us=0;budget=(offline_replay_budget_t){0};
    atomic_store(&s_idle_epoch,0);atomic_store(&s_ui_epoch,0);
    atomic_store(&s_help_sequence,0);atomic_store(&s_help_generation,0);
    nuwax_replay_ui_set_help_state(NUWAX_REPLAY_UI_HELP_NONE);
    s_idle_event->bits=0;
}
static void grant(void) {nuwax_replay_ui_set_help_state(NUWAX_REPLAY_UI_HELP_REQUESTED);}
int main(void) {
    nuwax_replay_schedule_init();reset();grant();budget.bytes=32768;
    sender(32768,1000,0);
#if OLD
    assert(now_us==18000 && demotions==1 && fair_waits==1);
    puts("PASS negative control: actual 145 block spends 8ms helping then a separate 10ms fairness wait");
#else
    assert(now_us==10000 && demotions==0 && fair_waits==1 && help_waits==0);
    assert(fairness_shared_ui==1 && fairness_ui_ready==1 && fairness_idle_wakes==0);
    assert(budget.yields==1); // same byte bound, even though a frame completed
    puts("PASS actual 146 block: same interleaving overlaps help with the mandatory wait (10ms), no priority change");

    reset();grant();sender(8192,1000,0);
    assert(now_us==8000 && help_waits==1 && fair_waits==0 && ui_handoffs==1);
    assert(!nuwax_replay_ui_handoff(&served_help));

    reset();grant();finish=false;sender(8192,1000,0);
    assert(now_us==10000 && help_waits==1 && ui_handoffs==1);
    assert(!nuwax_replay_ui_handoff(&served_help)); // no unbounded retry of same request

    reset();grant();finish=false;budget.bytes=32768;sender(32768,1000,0);
    assert(fairness_ui_pending==1 && fairness_ui_ready==0 && help_waits==0 && now_us==10000);
    assert(!nuwax_replay_ui_handoff(&served_help)); // pending is not completion

    reset();grant();clear_complete=true;sender(8192,1000,0);
    assert(help_waits==0 && now_us==0); // completion between clearing stale event and predicate

    reset();grant();stale_once=true;sender(8192,1000,0);
    assert(help_waits==2 && now_us==8000); // spurious event does not become completion

    reset();grant();finish=false;cancel_wait=true;sender(8192,1000,0);
    assert(help_waits==1 && now_us==1000 && !nuwax_replay_ui_handoff(&served_help));

    reset();grant();finish=false;replace_wait=true;sender(8192,1000,0);
    assert(help_waits==1 && now_us==1000);
    replace_wait=false;finish=true;sender(8192,1000,0);
    assert(help_waits==2 && ui_handoffs==2); // new grant remains independently serviceable

    reset();grant();finish=false;replace_wait=true;budget.bytes=32768;sender(32768,1000,0);
    assert(fair_waits==1 && help_waits==0 && fairness_ui_ready==0);
    replace_wait=false;finish=true;sender(8192,1000,0);assert(help_waits==1);

    reset();grant();sender(8192,1000,10000);
    assert(send_wait_credits==1 && fair_waits==0 && help_waits==1); // preserve real socket wait credit

    reset();grant();idle_wait=true;budget.bytes=32768;sender(32768,1000,0);
    assert(fairness_idle_wakes==1 && fairness_timer_wakes==0 && help_waits==0);

    reset();grant();now_us=(uint64_t)UINT32_MAX*10000;finish=false;sender(8192,1000,0);
    assert(xTaskGetTickCount()==0 && help_waits==1); // tick wrap keeps timeout bounded

    reset();grant();EventGroupHandle_t saved=s_idle_event;s_idle_event=NULL;
    sender(8192,1000,0);assert(delays==1 && priority==5);s_idle_event=saved;

    for(unsigned i=0;i<10000;i++) {
        reset();grant();finish=(i%3)==0;cancel_wait=(i%3)==1;budget.bytes=32768;
        sender(32768,1000,0);
        assert(wait_calls==1 && help_waits==0 && priority==5 && atomic_load(&s_idle_wait_state)==0);
    }
    puts("PASS: real outcomes, timeout, pending, cancel, replacement, clear/wait races, stale event, IDLE-only early credit, socket wait, tick wrap, missing event and 10000 cycles");
#endif
    return 0;
}
'''

hashes = {}
for label, base, marker in [('old', a.baseline, '/* Serve an existing grant'),
                             ('new', r, '/* Decide fairness before waiting')]:
    body = block(read(base / 'main/offline_audio_spool.c'), marker)
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
    c.write_text(support + module + wrap + tests, encoding='utf-8')
    subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror',
        '-Wno-unused-variable', '-Wno-unused-function', '-DOLD=' + str(int(label=='old')),
        '-I',str(base / 'main'),str(c),'-o',str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    hashes[label + 'BlockSha256'] = hashlib.sha256(body.encode()).hexdigest()
names = ['main/offline_audio_spool.c','main/replay_ui_schedule.c','main/replay_ui_schedule.h',
         'main/offline_io_schedule.h','main/nuwax_ui_v28/face/emotion_decode_schedule.h']
(out / 'report.json').write_text(json.dumps(dict(hardwareValidated=False,
    simulatedWaitBeforeMs=18, simulatedWaitAfterMs=10, sourceHashes={n:sha(r/n) for n in names},
    **hashes), indent=2), encoding='utf-8')
