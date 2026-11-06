"""Production sender blocks + observer + lease policy, with a simulated RTOS.

The old block is a negative control. Times describe an interleaving only, not
ESP throughput/FPS. The separate decoder test exercises actual owner/worker.
"""
from pathlib import Path
import argparse, hashlib, json, re, subprocess
p = argparse.ArgumentParser()
p.add_argument('--baseline', type=Path, required=True)
a = p.parse_args()
r = Path(__file__).resolve().parents[1]
out = r / 'build-host-checks/replay-ui-lease'
out.mkdir(parents=True, exist_ok=True)
source = (r / 'main/offline_audio_spool.c').read_text(encoding='utf-8-sig')
old = (a.baseline / 'main/offline_audio_spool.c').read_text(encoding='utf-8-sig')
def block(s, marker):
    start = s.index(marker, s.index('static esp_err_t replay_send_windowed('))
    end = s.index('\n            }\n            if (err != ESP_OK)', start)
    return s[start:end]
current = block(source, '/* Serve an existing grant')
previous = block(old, '/* Fairness first:')
module = re.sub(r'^#include.*$', '', (r / 'main/replay_ui_schedule.c').read_text(), flags=re.M)
pre = r'''
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
static bool frame_ready, eligible, finish_on_handoff, complete_during_wait;
static bool cancel_during_wait, new_grant_during_wait, idle_during_wait;
static unsigned priority=5, wait_calls, handoff_calls, expired, ready;
static uint32_t served_help, ui_handoffs, fairness_idle_wakes, fairness_timer_wakes;
static uint32_t fairness_followup_ui, fairness_ui_ready, fairness_ui_expired, fairness_ui_cancelled;
static uint32_t send_wait_credits;
static uint64_t ui_handoff_us, fairness_wait_us;
static offline_replay_budget_t budget;
static emotion_decode_help_t help;
static bool (*idle_cb)(void);
static void owner_tick(void) {
    emotion_decode_help_action_t a=emotion_decode_help_update(&help,now_us,now_us+8000,25000,1,
                                                            eligible,true,frame_ready);
    if(a==EMOTION_DECODE_HELP_GRANT) nuwax_replay_ui_set_help_state(NUWAX_REPLAY_UI_HELP_REQUESTED);
    if(a==EMOTION_DECODE_HELP_READY) {ready++;nuwax_replay_ui_set_help_state(NUWAX_REPLAY_UI_HELP_READY);}
    if(a==EMOTION_DECODE_HELP_EXPIRED) {expired++;nuwax_replay_ui_set_help_state(NUWAX_REPLAY_UI_HELP_EXPIRED);}
    if(a==EMOTION_DECODE_HELP_CANCEL) nuwax_replay_ui_set_help_state(NUWAX_REPLAY_UI_HELP_CANCELLED);
}
static int64_t esp_timer_get_time(void) {return now_us;}
static TickType_t xTaskGetTickCount(void) {return now_us/10000;}
static void vTaskDelay(unsigned ticks) {assert(ticks==1);now_us+=10000;}
static EventGroupHandle_t xEventGroupCreateStatic(StaticEventGroup_t *s) {s->bits=0;return s;}
static unsigned xEventGroupClearBits(EventGroupHandle_t e,unsigned b) {unsigned old=e->bits;e->bits&=~b;return old;}
static unsigned xEventGroupSetBits(EventGroupHandle_t e,unsigned b) {e->bits|=b;return e->bits;}
static unsigned xEventGroupWaitBits(EventGroupHandle_t e,unsigned b,int clear,int all,unsigned ticks) {
    assert(clear==1 && all==0 && ticks==1);wait_calls++;
    now_us+=5000;
    if(new_grant_during_wait) {
        help=(emotion_decode_help_t){0};frame_ready=false;eligible=true;
        nuwax_replay_ui_set_help_state(NUWAX_REPLAY_UI_HELP_CANCELLED);owner_tick();
    }
    now_us+=5000;
    if(complete_during_wait) {frame_ready=true;nuwax_replay_ui_frame_done();}
    if(cancel_during_wait) eligible=false;
    owner_tick();
    if(idle_during_wait) (void)idle_cb();
    unsigned result=e->bits;e->bits&=~b;return result;
}
static int xPortGetCoreID(void) {return 0;}
static unsigned uxTaskPriorityGet(void *t) {(void)t;return priority;}
static void vTaskPrioritySet(void *t,unsigned p) {
    assert(!t && ((priority==5 && p==4)||(priority==4 && p==5)));priority=p;
    if(p==4) {
        handoff_calls++;
        if(finish_on_handoff) {
            now_us+=8000;frame_ready=true;nuwax_replay_ui_frame_done();owner_tick();
        }
    }
}
static int esp_register_freertos_idle_hook_for_cpu(bool(*cb)(void),int core) {assert(core==0);idle_cb=cb;return ESP_OK;}
'''
def wrap(name, body):
    return f'''static void {name}(size_t n,uint64_t work,uint64_t wait) {{
        nuwax_ws_send_metrics_t metrics={{.operation_wait_max_us=wait}};
        const uint64_t packet_us=work,encode_before=0;
        uint32_t tx_groups[4]={{0}};size_t frame_count=1,sent=0;
        int64_t first_payload_us=-1,started=esp_timer_get_time();
        for(unsigned step=0;step<1;step++) {{
{body}
        }}
        assert(sent==n && tx_groups[0]==1 && first_payload_us>=0 && priority==5);
    }}\n'''
tests = r'''
static void reset(void) {
    now_us=0;priority=5;wait_calls=handoff_calls=expired=ready=0;
    frame_ready=eligible=complete_during_wait=cancel_during_wait=new_grant_during_wait=idle_during_wait=false;
    finish_on_handoff=true;
    served_help=ui_handoffs=fairness_idle_wakes=fairness_timer_wakes=fairness_followup_ui=0;
    fairness_ui_ready=fairness_ui_expired=fairness_ui_cancelled=send_wait_credits=0;
    ui_handoff_us=fairness_wait_us=0;budget=(offline_replay_budget_t){0};help=(emotion_decode_help_t){0};
    atomic_store(&s_idle_epoch,0);atomic_store(&s_ui_epoch,0);
    atomic_store(&s_help_generation,0);atomic_store(&s_help_sequence,0);
    nuwax_replay_ui_set_help_state(NUWAX_REPLAY_UI_HELP_NONE);
}
static void start_grant(void) {eligible=true;owner_tick();assert(help.active);now_us+=3000;budget.bytes=32768;}
int main(void) {
    nuwax_replay_schedule_init();
    reset();start_grant();old_block(32768,1000,0);
    assert(expired==1 && !frame_ready && handoff_calls==0 && wait_calls==1);
    reset();start_grant();new_block(32768,1000,0);
    assert(expired==0 && frame_ready && handoff_calls==1 && wait_calls==1 && ready==1);
    assert(fairness_followup_ui==0 && fairness_ui_expired==0);
    puts("PASS negative control: production 144 misses an existing 12ms grant; 145 serves it before mandatory fairness without extending its lease");

    // A later grant is serviced after waiting, without waiting for another send.
    reset();budget.bytes=32768;new_grant_during_wait=true;new_block(32768,1000,0);
    assert(frame_ready && fairness_followup_ui==1 && handoff_calls==1 && wait_calls==1);
    reset();start_grant();new_grant_during_wait=true;new_block(32768,1000,0);
    assert(handoff_calls==2 && fairness_followup_ui==1); // distinct grants

    // Already-served grants cannot request a second demotion, even if unfinished.
    reset();start_grant();finish_on_handoff=false;new_block(32768,1000,0);
    assert(handoff_calls==1 && expired==1 && fairness_ui_expired==1 && fairness_followup_ui==0);
    reset();start_grant();finish_on_handoff=false;complete_during_wait=true;new_block(32768,1000,0);
    assert(handoff_calls==1 && fairness_ui_ready==1 && fairness_ui_expired==0);
    reset();start_grant();finish_on_handoff=false;cancel_during_wait=true;new_block(32768,1000,0);
    assert(handoff_calls==1 && fairness_ui_cancelled==1 && fairness_ui_ready==0);
    for(unsigned state=NUWAX_REPLAY_UI_HELP_READY;state<=NUWAX_REPLAY_UI_HELP_CANCELLED;state++) {
        reset();nuwax_replay_ui_set_help_state((nuwax_replay_ui_help_state_t)state);
        budget.bytes=32768;new_block(32768,1000,0);assert(handoff_calls==0 && wait_calls==1);
    }
    reset();start_grant();new_block(32768,1000,10000);
    assert(handoff_calls==1 && wait_calls==0 && send_wait_credits==1 && budget.bytes==0);
    reset();start_grant();new_block(32768,1000,9999);
    assert(wait_calls==1 && send_wait_credits==0);
    reset();budget.work_us=19999;new_block(1,1,0);assert(wait_calls==1);
    reset();budget.bytes=32768;idle_during_wait=true;new_block(32768,1000,0);
    assert(fairness_idle_wakes==1 && fairness_timer_wakes==0);
    reset();new_block(32768,1000,0);(void)idle_cb();new_block(32768,1000,0);
    assert(wait_calls==0 && budget.idle_credits==1);
    nuwax_replay_ui_frame_done();new_block(32768,1000,0);assert(wait_calls==1);
    reset();
    for(unsigned i=0;i<10000;i++) {
        eligible=false;help=(emotion_decode_help_t){0};frame_ready=false;
        nuwax_replay_ui_set_help_state(NUWAX_REPLAY_UI_HELP_CANCELLED);
        eligible=true;owner_tick();now_us+=3000;new_block(32768,1000,0);
        assert(frame_ready && expired==0 && budget.bytes<65536);
    }
    assert(handoff_calls==10000 && wait_calls==5000 && fairness_followup_ui==0);
    puts("PASS actual sender/observer/lease: new grants, once-only demotion, ready/expired/cancel separation, real socket wait credit, work/byte guards, IDLE/UI separation, exact accounting and 10000 cycles");
}
'''
(out / 'check.c').write_text(pre + module + wrap('old_block', previous) + wrap('new_block', current) + tests, encoding='utf-8')
subprocess.run(['gcc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-I', str(r / 'main'),
               str(out / 'check.c'), '-o', str(out / 'check.exe')], check=True)
subprocess.run([str(out / 'check.exe')], check=True)
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
report = dict(hardwareBenchmark=False, sourceSha256=sha(r / 'main/offline_audio_spool.c'),
    oldBlockSha256=hashlib.sha256(previous.encode()).hexdigest(),
    newBlockSha256=hashlib.sha256(current.encode()).hexdigest(),
    sourceHashes={n: sha(r / n) for n in ['main/replay_ui_schedule.c', 'main/replay_ui_schedule.h',
        'main/offline_io_schedule.h', 'main/nuwax_ui_v28/face/emotion_decode_schedule.h']},
    checks='old negative control, current expiry regression, outcome separation, new grants, fairness and 10000 cycles')
(out / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
