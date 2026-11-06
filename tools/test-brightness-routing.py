"""Exercise actual UI action mapping, dispatcher, owner callback and PWM service."""
from pathlib import Path
import runpy
import subprocess

root = Path(__file__).resolve().parents[1]
base = runpy.run_path(str(root / 'tools/test-display-brightness.py'))
out = base['out']
(out / 'freertos').mkdir(exist_ok=True)
(out / 'freertos/FreeRTOS.h').write_text('#pragma once\n#include <stdint.h>\n')
(out / 'freertos/queue.h').write_text('#pragma once\ntypedef void *QueueHandle_t;\n')
(out / 'lvgl.h').write_text('#pragma once\ntypedef struct lv_obj_t lv_obj_t;\n')
(out / 'esp_log.h').write_text('#pragma once\n#define ESP_LOGW(tag,...) ((void)(tag))\n#define ESP_LOGI(tag,...) ((void)(tag))\n')
with (out / 'esp_err.h').open('a') as f:
    f.write('#define ESP_ERR_INVALID_STATE 259\n')

def function(path, signature):
    source = (root / path).read_text(encoding='utf-8')
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

mapping = function('main/app_shell.c', 'static nuwax_app_shell_result_t drawer_action_to_shell_action(\n    nuwax_drawer_action_result_t drawer_action) {')
owner = function('main/nuwax_ai_partner_main.c', 'static void shell_dispatch_cycle_brightness(void *ctx) {')
harness = base['harness'].split('int main(void){')[0] + r'''
#include "app_shell.h"
#include "nuwax_drawer.h"
#include "conversation/shell_action_dispatch.h"
#include "esp_log.h"
static const char *TAG="test";
static bool power_transition;
static unsigned rejected,followup_clears;
static uint8_t published;
static bool local_power_transition_active(void){return power_transition;}
void nuwax_app_shell_set_brightness_percent(uint8_t value){published=value;}
void nuwax_app_shell_notify_action_rejected(nuwax_app_shell_action_t action){
 assert(action==NUWAX_APP_SHELL_ACTION_CYCLE_BRIGHTNESS);++rejected;
}
static void clear_followup(void *ctx,const char *reason){(void)ctx;(void)reason;++followup_clears;}
''' + mapping + '\n' + owner + r'''
int main(void){
 saved=100;nuwax_display_brightness_load();
 const nuwax_shell_action_dispatch_callbacks_t cb={
  .cycle_brightness=shell_dispatch_cycle_brightness,.clear_auto_followup=clear_followup};
 nuwax_app_shell_result_t queued[3];
 for(unsigned i=0;i<3;++i){
  queued[i]=drawer_action_to_shell_action((nuwax_drawer_action_result_t){
   .action=NUWAX_DRAWER_ACTION_CYCLE_BRIGHTNESS});
  assert(queued[i].action==NUWAX_APP_SHELL_ACTION_CYCLE_BRIGHTNESS);
  assert(!nuwax_shell_action_is_listen_control(queued[i].action));
 }
 const uint8_t expected[]={40,70,100};
 /* All clicks arrive before any UI refresh: each must still advance once. */
 for(unsigned i=0;i<3;++i){
  nuwax_shell_action_dispatch(NULL,&queued[i],&cb);
  assert(pwm==expected[i]&&published==expected[i]);
 }
 assert(pwm_calls==3&&!rejected&&!followup_clears);
 pwm_error=-1;nuwax_shell_action_dispatch(NULL,&queued[0],&cb);
 assert(rejected==1&&published==100&&nuwax_display_brightness_percent()==100);
 pwm_error=0;nuwax_shell_action_dispatch(NULL,&queued[0],&cb);
 assert(pwm==40&&published==40);
 power_transition=true;nuwax_shell_action_dispatch(NULL,&queued[0],&cb);
 assert(rejected==2&&pwm==40&&published==40);
 puts("PASS: actual drawer mapping -> dispatcher -> main owner -> PWM; three queued clicks cycle 40/70/100; failure retains level; shutdown blocks; conversation preserved");
}
'''
source = out / 'routing.c'
source.write_text(harness, encoding='utf-8')
exe = out / 'routing.exe'
subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
               '-I'+str(out), '-I'+str(root/'main'), '-I'+str(root/'main/nuwax_ui_v28'),
               str(source), str(root/'main/display_brightness.c'),
               str(root/'main/conversation/shell_action_dispatch.c'), '-o', str(exe)], check=True)
subprocess.run([str(exe)], check=True)
