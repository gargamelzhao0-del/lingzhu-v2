"""Check actual GPIO timing, config compatibility, shared gate and shake bounds."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
gpio = (ROOT / 'main/board/board_gpio_inputs.c').read_text(encoding='utf-8-sig')
shake = (ROOT / 'main/board/shake_detector.c').read_text(encoding='utf-8-sig')
shake_header = (ROOT / 'main/board/shake_detector.h').read_text(encoding='utf-8-sig')


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


prefix = r'''
#include <assert.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "board/board_gpio_inputs.h"
#include "board/shake_detector.h"
#include "board/shake_motion_policy.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define portENTER_CRITICAL(...) ((void)0)
#define portEXIT_CRITICAL(...) ((void)0)
#define taskENTER_CRITICAL(...) ((void)0)
#define taskEXIT_CRITICAL(...) ((void)0)
#define pdMS_TO_TICKS(ms) (ms)
_Static_assert(NUWAX_HEAD_TOUCH_INTERACTION_COOLDOWN_MS == 1000U,"head cooldown");
_Static_assert(NUWAX_SHAKE_INTERACTION_COOLDOWN_MS == 2000U,"shake cooldown");
_Static_assert(NUWAX_SHAKE_MIN_SPAN_MS == 400U,"shake span");
static struct{int gpio,active_level;const char *name;}s_inputs[]={{8,0,"head"},{0,0,"boot"}};
static nuwax_board_input_callback_t s_callback;
static void *s_user_ctx,*s_stopped;
static atomic_bool s_stop_requested;
static unsigned now,mode,qualified,expired,releases,first_qualified;
'''
prefix += '\n'.join(re.findall(
    r'^static uint32_t s_head_touch_(?:long_press_ms|max_valid_duration_ms) = .*?;', gpio, re.M))
prefix += r'''
static uint64_t esp_timer_get_time(void){return (uint64_t)now*1000;}
static int gpio_get_level(int pin){
 if(pin!=8)return 1;
 bool active=false;
 if(mode==0)active=now>=100&&now<400;
 if(mode==1)active=now>=100&&now<700;
 if(mode==2)active=now>=100&&now<1250;
 if(mode==3)active=(now>=100&&now<450)||(now>=600&&now<950)||(now>=1100&&now<1450);
 if(mode==4)active=now>=100&&now<1300&&!(now>=500&&now<540);
 if(mode==5)active=now>=100&&now<1300&&!(now>=500&&now<510);
 if(mode==6)active=now>=100&&now<6200;
 return !active;
}
static void emit_head_touch_event(nuwax_head_touch_event_type_t type,uint64_t duration,bool active){
 (void)active;
 if(type==NUWAX_HEAD_TOUCH_EVENT_LONG_PRESS){qualified++;first_qualified=now;assert(duration==650U);}
 if(type==NUWAX_HEAD_TOUCH_EVENT_EXPIRED)expired++;
 if(type==NUWAX_HEAD_TOUCH_EVENT_RELEASE)releases++;
}
static void vTaskDelay(unsigned ms){now+=ms;if(now>=(mode==6?6500U:1600U))atomic_store(&s_stop_requested,true);}
static void xSemaphoreGive(void *s){(void)s;}
static void vTaskSuspend(void *s){(void)s;}
'''
body = function(gpio, 'static void input_task(')
body += function(gpio, 'esp_err_t nuwax_head_touch_set_config(')
gate_enum = shake[shake.index('typedef enum {'):shake.index('} nuwax_interaction_gate_owner_t;') + len('} nuwax_interaction_gate_owner_t;')]
gate = gate_enum + '\n' + shake[shake.index('static nuwax_interaction_gate_owner_t s_interaction_gate_owner;'):shake.index('static void shake_detector_task(')]
gate = re.search(r'^#define NUWAX_INTERACTION_GATE_FAILSAFE_MS .*$', shake, re.M)[0] + '\n' + gate
tests = r'''
static void reset_gate(void){s_interaction_gate_owner=NUWAX_INTERACTION_GATE_NONE;
 s_interaction_gate_deadline_ms=s_interaction_cooldown_until_ms=0;}
int main(void){
 assert(NUWAX_SHAKE_DYNAMIC_PEAK_MG==380.0f);
 assert(s_head_touch_long_press_ms==650U);
 nuwax_head_touch_config_t config={650U,750U,5000U};
 assert(nuwax_head_touch_set_config(&config)==ESP_OK);
 assert(nuwax_head_touch_set_config(NULL)==ESP_ERR_INVALID_ARG);
 config.long_press_ms=99;assert(nuwax_head_touch_set_config(&config)==ESP_ERR_INVALID_ARG);
 config.long_press_ms=10001;assert(nuwax_head_touch_set_config(&config)==ESP_ERR_INVALID_ARG);
 config.long_press_ms=650;config.gap_tolerance_ms=2001;
 assert(nuwax_head_touch_set_config(&config)==ESP_ERR_INVALID_ARG);
 config.gap_tolerance_ms=750;config.max_valid_duration_ms=650;
 assert(nuwax_head_touch_set_config(&config)==ESP_ERR_INVALID_ARG);
 config.max_valid_duration_ms=5000;assert(nuwax_head_touch_set_config(&config)==ESP_OK);
 for(mode=0;mode<7;mode++){
  now=qualified=expired=releases=first_qualified=0;atomic_store(&s_stop_requested,false);
  input_task(NULL);
  assert(qualified==(mode==2||mode==4||mode==5||mode==6));
  assert(expired==(mode==6));
  if(qualified)assert(first_qualified==(mode==4?1190U:750U));
 }
 puts("PASS: real GPIO task: 650ms qualification, short strokes rejected, debounce, release, held/stuck input; legacy 750ms gap config accepted");
 reset_gate();assert(interaction_gate_claim(NUWAX_INTERACTION_GATE_HEAD_TOUCH,100));
 interaction_gate_release(NUWAX_INTERACTION_GATE_SHAKE);
 assert(s_interaction_gate_owner==NUWAX_INTERACTION_GATE_HEAD_TOUCH);
 assert(!interaction_gate_claim(NUWAX_INTERACTION_GATE_SHAKE,1099));
 interaction_gate_release(NUWAX_INTERACTION_GATE_HEAD_TOUCH);
 assert(!interaction_gate_claim(NUWAX_INTERACTION_GATE_SHAKE,1099));
 assert(interaction_gate_claim(NUWAX_INTERACTION_GATE_SHAKE,1100));
 interaction_gate_release(NUWAX_INTERACTION_GATE_HEAD_TOUCH);
 assert(s_interaction_gate_owner==NUWAX_INTERACTION_GATE_SHAKE);
 interaction_gate_release(NUWAX_INTERACTION_GATE_SHAKE);
 assert(!interaction_gate_claim(NUWAX_INTERACTION_GATE_HEAD_TOUCH,3099));
 assert(interaction_gate_claim(NUWAX_INTERACTION_GATE_HEAD_TOUCH,3100));
 reset_gate();assert(interaction_gate_claim(NUWAX_INTERACTION_GATE_HEAD_TOUCH,100));
 assert(!interaction_gate_claim(NUWAX_INTERACTION_GATE_SHAKE,1100));
 assert(!interaction_gate_claim(NUWAX_INTERACTION_GATE_SHAKE,1400));
 interaction_gate_release(NUWAX_INTERACTION_GATE_HEAD_TOUCH);
 assert(interaction_gate_claim(NUWAX_INTERACTION_GATE_SHAKE,1400));
 reset_gate();assert(interaction_gate_claim(NUWAX_INTERACTION_GATE_HEAD_TOUCH,UINT32_MAX-500U));
 interaction_gate_release(NUWAX_INTERACTION_GATE_HEAD_TOUCH);
 assert(!interaction_gate_claim(NUWAX_INTERACTION_GATE_SHAKE,498U));
 assert(interaction_gate_claim(NUWAX_INTERACTION_GATE_SHAKE,499U));
 reset_gate();assert(interaction_gate_claim(NUWAX_INTERACTION_GATE_SHAKE,10));
 assert(!interaction_gate_claim(NUWAX_INTERACTION_GATE_HEAD_TOUCH,35009));
 assert(interaction_gate_claim(NUWAX_INTERACTION_GATE_HEAD_TOUCH,35010));
 puts("PASS: actual shared gate: 1s/2s boundaries, cross-action cooldown, wrong-owner release, in-flight ownership, timestamp wrap and failsafe");
 nuwax_shake_motion_t motion={0};float a[3]={0,0,1000};
 assert(!nuwax_shake_motion_observe(&motion,0,a,0,2000));
 a[0]=450;assert(!nuwax_shake_motion_observe(&motion,50,a,0,2000));
 assert(motion.peaks==1); /* Filtered 400mg: above new 380, below old 450. */
 motion=(nuwax_shake_motion_t){.initialized=true,.sample_seen=true,.last_sample_ms=350,
  .candidate=true,.candidate_started_ms=0,.last_peak_ms=200,.peaks=3,.sign=1,
  .direction={1,0,0},.gravity={0,0,1000}};
 a[0]=-1000;
 assert(!nuwax_shake_motion_observe(&motion,399,a,0,2000));
 motion=(nuwax_shake_motion_t){.initialized=true,.sample_seen=true,.last_sample_ms=350,
  .candidate=true,.candidate_started_ms=0,.last_peak_ms=200,.peaks=3,.sign=1,
  .direction={1,0,0},.gravity={0,0,1000}};
 assert(nuwax_shake_motion_observe(&motion,400,a,0,2000));
 puts("PASS: real shake policy: lighter acceleration admitted and four-peak span boundary at 400ms");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='nuwax-sensitivity-') as tmp:
    folder = Path(tmp)
    (folder / 'esp_err.h').write_text('typedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_ERR_INVALID_ARG -1\n', encoding='utf-8')
    path = folder / 'check.c'
    executable = folder / 'check.exe'

    def run(code, negative=False):
        path.write_text(code, encoding='utf-8')
        compiled = subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                                   '-Wno-unused-variable', '-Wno-unused-parameter',
                                   '-Wno-misleading-indentation', '-I' + str(folder),
                                   '-I' + str(ROOT / 'main'), str(path), '-lm', '-o', str(executable)],
                                  capture_output=True, text=True)
        assert compiled.returncode == 0, compiled.stderr
        result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
        assert (result.returncode != 0) == negative, result.stdout + result.stderr
        if not negative:
            print(result.stdout, end='')

    code = '\n'.join([prefix, body, gate, tests])
    run(code)
    mutations = {
        'legacy_gap_rejects_shorter_qualification': ('config->gap_tolerance_ms > 2000U ||', 'config->gap_tolerance_ms > 2000U || config->gap_tolerance_ms > config->long_press_ms ||'),
        'touch_fires_before_qualification': ('press_duration_ms >= long_press_ms', 'press_duration_ms >= long_press_ms / 2U'),
        'release_skips_cooldown': ('static void interaction_gate_release(nuwax_interaction_gate_owner_t owner) {', 'static void interaction_gate_release(nuwax_interaction_gate_owner_t owner) { s_interaction_cooldown_until_ms = 0U;'),
        'wrong_owner_unlocks': ('if (s_interaction_gate_owner == owner)', 'if (true)'),
    }
    for name, (old, new) in mutations.items():
        assert old in code, name
        run(code.replace(old, new), negative=True)
        print('PASS: compiled negative mutant rejected:', name)
