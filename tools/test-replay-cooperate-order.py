"""Run the production post-send blocks with deterministic scheduler interleavings.

The 143 block is a negative control: two sequential waits must be reproduced.
Simulated times verify ordering only, not ESP throughput or frame rate.
"""
from pathlib import Path
import argparse, hashlib, json, subprocess

p=argparse.ArgumentParser();p.add_argument('--baseline',type=Path,required=True);a=p.parse_args()
r=Path(__file__).resolve().parents[1];out=r/'build-host-checks/replay-cooperate-order'
out.mkdir(parents=True,exist_ok=True)
path=r/'main/offline_audio_spool.c';src=path.read_text(encoding='utf-8-sig')
old=(a.baseline/'main/offline_audio_spool.c').read_text(encoding='utf-8-sig')
def block(s,marker):
    start=s.index(marker,s.index('static esp_err_t replay_send_windowed('))
    end=s.index('\n            }\n            if (err != ESP_OK)',start)
    return s[start:end]
current=block(src,'/* Fairness first:')
previous=block(old,'/* No transport lock is held here.')
pre=r'''
#define ESP_PLATFORM 1
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "offline_io_schedule.h"
#include "ws_send_metrics.h"
#define portTICK_PERIOD_MS 10U
static uint64_t now_us;
static bool pending,complete_during_wait,idle_during_wait,observed;
static unsigned grant=1,wait_calls,handoff_calls,epoch,ui_epoch;
static uint32_t served_help,ui_handoffs,fairness_idle_wakes,fairness_timer_wakes;
static uint32_t fairness_followup_ui,send_wait_credits;
static uint64_t ui_handoff_us,fairness_wait_us;
static offline_replay_budget_t budget;
static int64_t esp_timer_get_time(void) {return (int64_t)now_us;}
bool nuwax_replay_schedule_snapshot(uint32_t *idle,uint32_t *ui) {
    *idle=epoch;*ui=ui_epoch;return observed;
}
bool nuwax_replay_schedule_yield(void) {
    wait_calls++;now_us+=10000;
    if(complete_during_wait){pending=false;ui_epoch++;}
    if(idle_during_wait)epoch++;
    return idle_during_wait;
}
bool nuwax_replay_ui_handoff(uint32_t *served) {
    if(!pending || *served==grant)return false;
    *served=grant;handoff_calls++;now_us+=8000;
    pending=false;ui_epoch++;return true;
}
static void reset(void) {
    now_us=wait_calls=handoff_calls=epoch=ui_epoch=0;
    pending=complete_during_wait=idle_during_wait=observed=false;grant=1;
    served_help=ui_handoffs=fairness_idle_wakes=fairness_timer_wakes=0;
    fairness_followup_ui=send_wait_credits=0;ui_handoff_us=fairness_wait_us=0;
    budget=(offline_replay_budget_t){0};
}
'''
def wrap(name,body):
    return f'''static void {name}(size_t n,uint64_t work,uint64_t wait) {{
        nuwax_ws_send_metrics_t metrics={{.operation_wait_max_us=wait}};
        const uint64_t packet_us=work,encode_before=0;
        uint32_t tx_groups[4]={{0}};size_t frame_count=1,sent=0;
        int64_t first_payload_us=-1,started=esp_timer_get_time();
        for(unsigned step=0;step<1;step++) {{
{body}
        }}
        assert(sent==n && tx_groups[0]==1 && first_payload_us>=0);
    }}\n'''
tests=r'''
int main(void) {
    // Negative control uses the actual old block, not a rewritten algorithm.
    reset();budget.bytes=32768;pending=complete_during_wait=true;
    old_block(32768,1000,0);
    assert(wait_calls==1 && handoff_calls==1 && now_us==18000);
    reset();budget.bytes=32768;pending=complete_during_wait=true;
    new_block(32768,1000,0);
    assert(wait_calls==1 && handoff_calls==0 && now_us==10000 && !pending);
    assert(fairness_timer_wakes==1 && fairness_followup_ui==0 && budget.yields==1);
    puts("PASS negative control: actual 143 block waits twice; 144 overlaps decode completion with required fairness wait");

    // If the wait does NOT produce a frame, retain the directed UI handoff.
    reset();budget.bytes=32768;pending=true;new_block(32768,1000,0);
    assert(wait_calls==1 && handoff_calls==1 && fairness_followup_ui==1 && now_us==18000);
    reset();pending=true;new_block(8192,1000,0);
    assert(wait_calls==0 && handoff_calls==1 && budget.bytes==8192);
    reset();budget.bytes=32768;pending=true;new_block(32768,1000,10000);
    assert(wait_calls==0 && handoff_calls==1 && send_wait_credits==1 && budget.bytes==0);
    reset();budget.bytes=32768;pending=true;new_block(32768,1000,9999);
    assert(wait_calls==1 && handoff_calls==1 && send_wait_credits==0);
    reset();budget.work_us=19999;new_block(1,1,0);
    assert(wait_calls==1 && budget.yields==1); // work guard retained
    reset();budget.bytes=32768;pending=complete_during_wait=idle_during_wait=true;
    new_block(32768,1000,0);
    assert(fairness_idle_wakes==1 && fairness_timer_wakes==0 && handoff_calls==0);

    // Observed IDLE still credits a previous yield; UI alone cannot erase bytes.
    reset();observed=true;new_block(32768,1000,0);epoch++;
    new_block(32768,1000,0);assert(wait_calls==0 && budget.idle_credits==1);
    ui_epoch++;new_block(32768,1000,0);assert(wait_calls==1);

    reset();complete_during_wait=true;
    for(unsigned i=0;i<10000;i++) {
        pending=true;grant++;new_block(32768,500,0);
        assert(budget.bytes<65536 && !pending);
    }
    assert(wait_calls==5000 && handoff_calls==5000 && budget.yields==5000);
    assert(fairness_followup_ui==0); // every due wait already completed UI work
    puts("PASS actual sender block: UI fallback, socket wait credit, sub-tick rejection, work/byte bounds, IDLE/UI separation and 10000 cycles; payload accounting exactly once");
}
'''
host=pre+wrap('old_block',previous)+wrap('new_block',current)+tests
(out/'check.c').write_text(host,encoding='utf-8')
subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-I',str(r/'main'),
    str(out/'check.c'),'-o',str(out/'check.exe')],check=True)
subprocess.run([str(out/'check.exe')],check=True)
(out/'report.json').write_text(json.dumps({'hardwareBenchmark':False,
    'sourceSha256':hashlib.sha256(path.read_bytes()).hexdigest(),
    'oldBlockSha256':hashlib.sha256(previous.encode()).hexdigest(),
    'newBlockSha256':hashlib.sha256(current.encode()).hexdigest(),
    'negativeControl':'production 143 block incurs handoff then fairness wait',
    'checks':'completion during wait, still-needed fallback, socket wait, work/byte bounds, accounting, 10000 cycles'},indent=2),encoding='utf-8')
