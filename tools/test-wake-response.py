"""Execute noise policy and real greeting admission with response/failure races."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
out = root / 'build-host-checks/wake-response'
out.mkdir(parents=True, exist_ok=True)

def run(name, source):
    path = out / (name + '.c')
    path.write_text(source, encoding='utf-8')
    exe = out / (name + '.exe')
    subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I', str(root / 'main'), str(path), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    print('PASS:', name)

run('noise_and_shadow', (root/'main/wake_response_host_check.c').read_text())
source = (root/'main/nuwax_ai_partner_main.c').read_text(encoding='utf-8-sig')
start = source.index('static esp_err_t send_wake_word_greeting(')
end = source.index('\nstatic esp_err_t wait_for_mic_start_action', start)
body = source[start:end]
run('greeting_feedback', r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#define ESP_LOGI(tag, ...) ((void)printf(__VA_ARGS__))
#define ESP_LOGE(...) ((void)0)
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 1
#define NUWAX_WAKE_WORD_GREETING_REQUEST_PREFIX "wake_"
#define NUWAX_WAKE_WORD_GREETING_TEXT "hello"
#define NUWAX_CONVERSATION_WAKE_TEXT_TIMEOUT_MS 15000
#define NUWAX_VISUAL_ROUND_WAKE_GREETING 2
#define NUWAX_INTERACTIVE_CONTROL_SEND_TIMEOUT_MS 250
#define NUWAX_APP_STATE_LISTENING 5
typedef int esp_err_t;
typedef int nuwax_ws_client_t;
unsigned s_cloud_text_request_seq;
struct { uint32_t wake_detected_ms; } s_wake_greeting_metrics;
static uint32_t monotonic_ms(void){return 100;}
static int metrics_delta_ms(uint32_t a,uint32_t b){return (int)(b-a);}
static bool guard_ok, visual_ok, guarded, visual;
static unsigned epoch, transport_epoch;
static int sends, feedback, cleared, cancelled, result;
static bool begin_active_turn_guard(const char *r, const char *t, int ms, const char *why) {
    (void)r;(void)t;(void)why; assert(ms==15000); guarded=guard_ok; return guarded;
}
static bool begin_conversation_visual_round(int k,const char *r,const char *why) {
    (void)r;(void)why;assert(k==2 && guarded);visual=visual_ok;return visual;
}
static void wake_greeting_metrics_cancel(const char *s){(void)s;cancelled++;}
static void clear_active_turn_guard(const char *s){(void)s;cleared++;guarded=false;visual=false;}
static unsigned active_ws_epoch_snapshot(void){return epoch;}
static unsigned nuwax_ws_connection_epoch(int *w){(void)w;return transport_epoch;}

static int nuwax_session_send_text_request_with_timeout_if_epoch(int *w,const char *r,
    const char *t,int timeout,unsigned ep){
    (void)w;(void)r;(void)t;assert(timeout==250 && ep==epoch);
    /* A response can arrive inside send: ownership must exist; the face still waits for PCM. */
    assert(guarded && visual && feedback==0);sends++;return result;
}
static void wake_greeting_metrics_mark_sent(const char *r,int err){(void)r;assert(err==result);}
''' + body + r'''
static void reset(void){guard_ok=visual_ok=true;guarded=visual=false;epoch=transport_epoch=2;
    sends=feedback=cleared=cancelled=result=0;}
int main(void){
    int ws=0;
    reset();assert(send_wake_word_greeting(&ws,"word")==0);assert(sends==1 && feedback==0 && guarded);
    reset();guard_ok=false;assert(send_wake_word_greeting(&ws,"word")==1);assert(!feedback && !sends && cancelled==1);
    reset();visual_ok=false;assert(send_wake_word_greeting(&ws,"word")==1);assert(!feedback && !sends && cleared==1);
    reset();transport_epoch=3;assert(send_wake_word_greeting(&ws,"word")==1);assert(!feedback && !sends && cleared==1);
    reset();epoch=0;assert(send_wake_word_greeting(&ws,"word")==1);assert(!feedback && !sends);
    reset();result=7;assert(send_wake_word_greeting(&ws,"word")==7);assert(feedback==0 && sends==1 && cleared==1 && !guarded);
    return 0;
}
''')
detector = (root/'main/wake_word_detector.c').read_text(encoding='utf-8-sig')
assert 'noise_floor_avg_abs = nuwax_wake_noise_observe(' in detector
assert 'nuwax_wake_noise_reset(&noise_estimator)' in detector
assert 'action_eligible = nuwax_wake_detection_accept(' in detector
assert 'action_eligible = nuwax_wake_shadow' not in detector
assert 'shadow=%s enforced=0' in detector
assert 'set_state_deferred_ui(NUWAX_APP_STATE_THINKING)' not in body
assert 'set_state_deferred_ui(' not in body
assert 'start_prelisten_window' not in body
assert 'CONFIG_NUWAX_CUSTOM_WAKE_WORD_THRESHOLD_PERCENT=9' in (root/'sdkconfig.defaults').read_text(encoding='utf-8-sig')
print('PASS: wake feedback ownership/failure/epoch ordering; original audio flow; shadow-only diagnostics')

# Execute the actual cloud settle loop: a queued wake must keep its ownership
# metadata and return before the 2 s admission deadline, including mid-wait hits.
start = source.index('static void wait_for_cloud_updates(')
end = source.index('\nstatic bool mcp_set_emotion', start)
run('cloud_wait_wake_handoff', r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define CONFIG_NUWAX_WAKE_WORD_ENABLE 1
#define CONFIG_NUWAX_AUDIO_SOURCE_I2S_MIC 1
#define CONFIG_NUWAX_VOICE_FRAME_DURATION_MS 20
#define NUWAX_APP_STATE_SLEEPING 4
#define ESP_OK 0
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
typedef int nuwax_ws_client_t;
typedef int esp_err_t;
static int state, queued, slices, ticks, commands, mode;
static void *s_wake_word_events=(void*)1;
static int s_meeting_mode,s_monitoring_mode;
static struct { bool pending,capture_active; } s_prelisten;
static int64_t now_ms_i64(void){return ticks;}
static bool nuwax_ws_is_connected(int *w){(void)w;return true;}
static void app_task_watchdog_checkpoint(void){}
static int conversation_state_current(void){return state;}
static unsigned uxQueueMessagesWaiting(void *q){assert(q);return queued;}
static bool nuwax_audio_terminal_recovery_pending(void){return false;}
static int service_audio_terminal_recovery(int *w){(void)w;return 0;}
static void drain_session_commands(int *w){(void)w;commands++;}
static bool drain_shell_actions_preserving_listen_control(int *w){(void)w;return mode==3;}
static void complete_during_playback_wake_stop(void){}
static bool exclusive_audio_mode_blocks_normal_audio(void){return false;}
static bool nuwax_meeting_mode_has_pending(int *m){(void)m;return false;}
static bool nuwax_monitoring_mode_has_pending(int *m){(void)m;return false;}
static void service_exclusive_audio_sleep_reconcile(void){}
static int service_application_keepalive(int *w,const char *c){(void)w;(void)c;return 0;}
static int run_audio_source(int *w){(void)w;assert(false);return 0;}
static void service_pending_ws_reconnect_audio_recovery(int *w){(void)w;}
static bool wake_detector_request_is_playback(void){return false;}
static void conversation_owner_wait_ms(unsigned ms){
    ticks+=ms;slices++;
    if(mode==1)queued=1;
    if(mode==2)state=4;
}
""" + source[start:end] + r"""
static void reset(void){state=3;queued=slices=ticks=commands=mode=0;}
int main(void){int ws=0;
    reset();state=4;wait_for_cloud_updates(&ws,2500);assert(slices==0);
    reset();queued=1;wait_for_cloud_updates(&ws,2500);assert(slices==0 && queued==1);
    reset();mode=1;wait_for_cloud_updates(&ws,2500);assert(ticks==100 && queued==1);
    reset();mode=2;wait_for_cloud_updates(&ws,2500);assert(ticks==100);
    reset();mode=3;wait_for_cloud_updates(&ws,2500);assert(slices==0 && commands==1);
    reset();wait_for_cloud_updates(&ws,2500);assert(ticks==2500 && commands==25);
    return 0;
}
""")
