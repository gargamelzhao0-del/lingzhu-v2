"""Replay motion into the production policy and fault-inject its real worker."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root/'main/board/shake_detector.c').read_text(encoding='utf-8-sig')
header = (root/'main/board/shake_detector.h').read_text(encoding='utf-8-sig')
policy = (root/'main/board/shake_motion_policy.h').read_text(encoding='utf-8-sig')
policy_test = (root/'main/shake_motion_policy_host_check.c').read_text(encoding='utf-8-sig')
start = source.index('static void shake_detector_task(')
end = source.index('{', start)+1
depth = 1
while depth:
    depth += (source[end]=='{') - (source[end]=='}')
    end += 1
worker = source[start:end]
worker = 'static void simulate(void) {\n' + worker[worker.index('    nuwax_shake_motion_t motion = {0};'):]
definitions = '\n'.join(re.findall(r'^#define NUWAX_.*$', source+'\n'+header, re.M))
common = r'''
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <stdio.h>
#include <setjmp.h>
#include <assert.h>
#include "board/shake_motion_policy.h"
#define ESP_OK 0
#define ESP_ERR_NOT_FINISHED 2
#define ESP_LOGI(...) ((void)0)
#define pdMS_TO_TICKS(ms) ((ms)/10U)
#define NUWAX_INTERACTION_GATE_SHAKE 1
typedef uint32_t TickType_t;
typedef int esp_err_t;
typedef struct {int16_t accel_x,accel_y,accel_z,gyro_x,gyro_y,gyro_z;
 float accel_scale,gyro_scale;} qmi8658a_sensor_data_t;
static uint32_t ticks,cycles,events,mode,first_event,last_event,claims,last_read_end;
static bool last_read_slow;
static jmp_buf end;
static TickType_t xTaskGetTickCount(void){return ticks;}
static void vTaskDelayUntil(TickType_t *wake,uint32_t period){
 if(++cycles>200)longjmp(end,1);
 *wake+=period; if(ticks<*wake)ticks=*wake;
 if(mode==2 && cycles%40==0)ticks+=5;
 if(mode==4 && cycles%4==0)ticks+=100;
}
static uint64_t esp_timer_get_time(void){return (uint64_t)ticks*10000;}
static esp_err_t qmi8658a_read_data(qmi8658a_sensor_data_t *v){
 if(mode==9||mode==10){
  if(last_read_slow)assert(ticks-last_read_end>=5);
  last_read_slow=mode==9 || cycles%40==0;
  if(last_read_slow)ticks+=6;
  last_read_end=ticks;
 }
 float a=800*sinf(6.2831853f*2*(float)(ticks*10U)/1000);
 if(mode==5 || (mode==6 && cycles%20>=4) ||
    ((mode==11||mode==12||mode==13) && cycles>40 && cycles<=120)) a=0;
 *v=(qmi8658a_sensor_data_t){.accel_x=(int16_t)a,.accel_z=1000,
     .gyro_x=mode==5?160:0,.gyro_scale=1,.accel_scale=1};
 if(mode==14 || (mode==12 && cycles%5==0)) return ESP_ERR_NOT_FINISHED;
 return ((mode==1 && cycles%40==0)||mode==3||(mode==8 && cycles<=100)||
    (mode==13 && cycles>40 && cycles<=120 && cycles%5==0))?1:0;
}
static bool interaction_gate_claim(int owner,uint32_t t){(void)owner;(void)t;claims++;return mode!=7;}
static void interaction_gate_release(int owner){(void)owner;}
static void hit(void *ctx){
 (void)ctx; uint32_t now=ticks*10U;
 if(events)assert((uint32_t)(now-last_event)>=NUWAX_SHAKE_INTERACTION_COOLDOWN_MS);
 else first_event=now;
 last_event=now;events++;
}
static struct {void (*callback)(void*);void *ctx;} s_runtime={hit,0};
'''
tests = r'''
int main(void){
 for(mode=0;mode<15;mode++){
  ticks=cycles=events=claims=first_event=last_event=last_read_end=0; last_read_slow=false;
  if(setjmp(end)==0)simulate();
  bool expect=mode==0||mode==1||mode==2||mode==8||mode==10||mode==11||mode==12||mode==13;
  printf("worker mode=%u events=%u first_ms=%u claims=%u\n",mode,events,first_event,claims);
  assert(expect ? events>0 : events==0);
  if(expect)assert(first_event>=NUWAX_SHAKE_MIN_SPAN_MS);
  if(mode==0)assert(events==1);
  if(mode==8)assert(first_event>=5000+NUWAX_SHAKE_MIN_SPAN_MS);
  if(mode==7)assert(claims>0);
  if(mode==11)assert(events==2);
  if(mode==12)assert(events==2);
  if(mode==13)assert(events==1);
 }
 puts("PASS: real worker read failures, scheduling gaps, stale reads, no catch-up, gyro-only rejection, gate and quiet re-arm");
}
'''
code = definitions + '\n' + common + worker + tests
with tempfile.TemporaryDirectory(prefix='nuwax-shake-pattern-') as tmp:
    folder=Path(tmp)
    (folder/'board').mkdir()
    local_header=folder/'board/shake_motion_policy.h'
    local_header.write_text(policy,encoding='utf-8')
    path=folder/'check.c'
    exe=folder/'check.exe'
    def run(text, expected=True):
        path.write_text(text,encoding='utf-8')
        subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror',
            '-Wno-unused-but-set-variable','-Wno-unused-variable',
            '-I',str(folder),str(path),'-lm','-o',str(exe)],check=True)
        result=subprocess.run([str(exe)],capture_output=True,text=True)
        assert (result.returncode==0)==expected, result.stdout+result.stderr
        if expected: print(result.stdout,end='')
    run(policy_test)
    mutations={
        'missing_reversals': ('if (sign == s->sign) return false;', '/* any direction counts */'),
        'too_few_peaks': ('REQUIRED_PEAKS 4U','REQUIRED_PEAKS 3U'),
        'no_fixed_window': ('> NUWAX_SHAKE_WINDOW_MS', '> UINT32_MAX'),
        'fast_rebound_accepted': ('MIN_PEAK_INTERVAL_MS 100U','MIN_PEAK_INTERVAL_MS 1U'),
        'no_quiet_rearm': ('if (s->inhibited) {','if (false && s->inhibited) {'),
        'busy_erases_baseline': ('s->skipped_sample = true;\n}', 's->initialized = false; s->quiet_seen = false; s->skipped_sample = true;\n}'),
        'count_unobserved_quiet': ('quiet && !quiet_gap', 'quiet'),
        'unbounded_quiet_window': ('> NUWAX_SHAKE_REARM_WINDOW_MS', '> UINT32_MAX'),
        'gyro_is_quiet': ('gyro_sum_dps <= NUWAX_SHAKE_QUIET_GYRO_DPS','true'),
        'no_cooldown': ('>= cooldown_ms', '>= cooldown_ms / 4000U'),
        'no_gap_reset': ('> NUWAX_SHAKE_MAX_SAMPLE_GAP_MS','> UINT32_MAX'),
        'fault_releases_inhibition': ('s->initialized = false;', 's->inhibited=false; s->initialized = false;'),
        'single_gap_erases_valid_peaks': ('s->initialized = false;', 'nuwax_shake_candidate_reset(s); s->initialized = false;'),
    }
    for name,(before,after) in mutations.items():
        assert before in policy, name
        local_header.write_text(policy.replace(before,after),encoding='utf-8')
        run(policy_test,False)
        print('PASS: policy mutant rejected',name)
    local_header.write_text(policy,encoding='utf-8')
    run(code)
    mutants={
        'catch_up_after_slow_read': ('            wake_tick = xTaskGetTickCount();','            /* no rebase */'),
        'consume_failed_read': ('if (read_err != ESP_OK)', 'if (false && read_err != ESP_OK)'),
        'skip_interaction_gate': ('if (!interaction_gate_claim(', 'if (false && !interaction_gate_claim('),
        'busy_treated_as_sensor_fault': ('read_err == ESP_ERR_NOT_FINISHED && !stale_sample', 'false && read_err == ESP_ERR_NOT_FINISHED && !stale_sample'),
    }
    for name,(before,after) in mutants.items():
        assert before in code,name
        run(code.replace(before,after),False)
        print('PASS: worker mutant rejected',name)
print('PASS: shake pattern policy and actual worker regression checks')
