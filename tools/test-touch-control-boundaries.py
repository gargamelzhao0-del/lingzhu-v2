"""Run the real physical-touch callback against lifecycle and gesture owners."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root/'main/conversation/head_touch_control.c').read_text(encoding='utf-8-sig')
# Globals + the complete callback, without board initialization.
start = source.index('static nuwax_head_touch_state_cb_t s_get_state;')
end = source.index('esp_err_t nuwax_head_touch_control_init(', start)
body = source[start:end]
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "conversation/head_touch_control_policy.h"
#include "board/board_gpio_inputs.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define CONFIG_NUWAX_HEAD_TOUCH_LONG_PRESS_MS 650
#define CONFIG_NUWAX_HEAD_TOUCH_MAX_VALID_DURATION_MS 5000
#define CONFIG_NUWAX_TOUCH_WAKE_ACTION_ENABLE 0
#define CONFIG_NUWAX_TOUCH_INTERRUPT_ACTION_ENABLE 1
enum { NUWAX_APP_SHELL_ACTION_START_LISTEN, NUWAX_APP_SHELL_ACTION_CANCEL,
       NUWAX_APP_SHELL_ACTION_HEAD_TOUCH_AGENT, NUWAX_APP_SHELL_ACTION_STOP_LISTEN };
typedef nuwax_app_state_t (*nuwax_head_touch_state_cb_t)(void*);
typedef bool (*nuwax_head_touch_allowed_cb_t)(void*);
typedef bool (*nuwax_head_touch_media_interrupt_cb_t)(void*);
static nuwax_app_state_t current;
static bool allowed=true, menu=false, music=false, claimed=false, queue_ok=true;
static unsigned animations, requests, claims, releases;
static int last_action;
static bool nuwax_app_shell_can_start_listen(void) {return !menu;}
static void nuwax_app_shell_trigger_touch_animation(void) {animations++;}
static bool nuwax_shake_detector_claim_head_touch(void) {
    if(claimed)return false;claimed=true;claims++;return true;
}
static void nuwax_shake_detector_release_head_touch(void) {claimed=false;releases++;}
static bool nuwax_app_shell_request_action(int action,int value) {
    (void)value;requests++;last_action=action;return queue_ok;
}
static nuwax_app_state_t get(void *s) {(void)s;return current;}
static bool can(void *s) {(void)s;return allowed;}
static bool stop_music(void *s) {(void)s;bool was=music;music=false;return was;}
'''
tests = r'''
static void reset(nuwax_app_state_t state) {
    current=state;allowed=queue_ok=true;menu=music=claimed=false;
    animations=requests=claims=releases=0;
    s_get_state=get;s_interaction_allowed=can;s_interrupt_media=stop_music;
    nuwax_head_touch_event_t press={.type=NUWAX_HEAD_TOUCH_EVENT_PRESS};
    head_touch_event_callback(&press,NULL);
}
static void qualify(void) {
    nuwax_head_touch_event_t event={.type=NUWAX_HEAD_TOUCH_EVENT_LONG_PRESS,.duration_ms=650};
    head_touch_event_callback(&event,NULL);
}
static void release(void) {
    nuwax_head_touch_event_t event={.type=NUWAX_HEAD_TOUCH_EVENT_RELEASE,.duration_ms=1100};
    head_touch_event_callback(&event,NULL);
}
int main(void) {
    reset(NUWAX_APP_STATE_SLEEPING);qualify();release();
    assert(animations==1 && requests==0 && !claimed);
    assert(current==NUWAX_APP_STATE_SLEEPING); /* no wake or cloud command */
    reset(NUWAX_APP_STATE_LISTENING);qualify();release();
    assert(animations==1 && requests==1 && last_action==NUWAX_APP_SHELL_ACTION_HEAD_TOUCH_AGENT);
    assert(claimed); /* async transaction owns the gate after finger-up */
    reset(NUWAX_APP_STATE_SPEAKING);current=NUWAX_APP_STATE_LISTENING;qualify();release();
    assert(animations==1 && requests==0 && !claimed); /* finished playback */
    reset(NUWAX_APP_STATE_SLEEPING);allowed=false;qualify();release();
    assert(animations==0 && requests==0 && claims==0); /* strong lifecycle */
    reset(NUWAX_APP_STATE_LISTENING);claimed=true;qualify();release();
    assert(animations==0 && requests==0 && claimed); /* other interaction owns gate */
    reset(NUWAX_APP_STATE_LISTENING);queue_ok=false;qualify();release();
    assert(animations==1 && requests==1 && !claimed); /* enqueue failure cleanup */
    reset(NUWAX_APP_STATE_LISTENING);menu=true;qualify();release();
    assert(animations==1 && requests==0 && !claimed);
    reset(NUWAX_APP_STATE_SLEEPING);music=true;menu=true;claimed=true;qualify();release();
    assert(!music && animations==1 && requests==0 && claimed); /* explicit music stop */
    puts("PASS: physical touch sleep feedback, speech completion, lifecycle, queue ownership and music stop");
}
'''
with tempfile.TemporaryDirectory(prefix='touch-control-') as temp:
    src, exe = Path(temp)/'test.c', Path(temp)/'test.exe'
    src.write_text(fixture + body + tests, encoding='utf-8')
    subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror',
                    '-Wno-misleading-indentation',
                    '-I'+str(root/'main'), '-I'+str(root/'main/audio_modes/meeting/host_stubs'),
                    str(src), str(root/'main/conversation/head_touch_control_policy.c'),
                    '-o', str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
