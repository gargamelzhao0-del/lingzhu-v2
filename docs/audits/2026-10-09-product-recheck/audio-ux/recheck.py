"""Second independent audit harness: real wake-wait loop; no production edits."""
from pathlib import Path
import ast
import hashlib
import json
import re
import subprocess

OUT = Path(__file__).resolve().parent
ROOT = next(p for p in Path(__file__).resolve().parents if (p / 'main/nuwax_ai_partner_main.c').is_file())
source = (ROOT / 'main/nuwax_ai_partner_main.c').read_text(encoding='utf-8-sig')
previous = ROOT / 'docs/audits/2026-10-09-product-release/audio-ux/reproduce.py'
tree = ast.parse(previous.read_text())
prefix = next(ast.literal_eval(n.value) for n in tree.body if isinstance(n, ast.Assign) and
              any(isinstance(t, ast.Name) and t.id == 'prefix' for t in n.targets))


def extract(name, text=source):
    match = re.search(r'^static [^;{}]*\b' + name + r'\([^;{}]*\)\s*\{', text, re.M)
    assert match, name
    pos = text.index('{', match.start()) + 1
    depth = 1
    while depth:
        if text[pos] == '{': depth += 1
        if text[pos] == '}': depth -= 1
        pos += 1
    return text[match.start():pos]


for name in ['clear_active_turn_guard', 'start_prelisten_window',
             'return_to_listening_after_cancel', 'nuwax_prelisten_state_mode_is',
             'clear_last_listen_resume','nuwax_listen_resume_state_holds_idle_expiry_boundary']:
    prefix = prefix.replace(extract(name, prefix), '')
prefix = prefix.replace('static struct { bool pending, capture_active; } s_prelisten;',
                       'static nuwax_prelisten_state_t s_prelisten;')
prefix = prefix.replace('static int s_listen_resume, s_notification_transaction;',
                       'static nuwax_listen_resume_state_t s_listen_resume; static int s_notification_transaction;')
prefix = prefix.replace('static void set_state(int next) { state = next; }',
                       'static void set_state(int next) { state = next; if(next==NUWAX_APP_STATE_LISTENING) capture_starts++; }')
prefix = prefix.replace('static unsigned uxQueueMessagesWaiting(void *p) { (void)p;return 0; }',
                       'static unsigned uxQueueMessagesWaiting(void *p);')
prefix = prefix.replace('#include "conversation/awake_session_state.h"',
    '#include "conversation/awake_session_state.h"\n#include "conversation/prelisten_state.h"\n'
    '#include "conversation/listen_resume_state.h"\n#include "conversation/conversation_visual_round.h"')
extra = r'''
#define CONFIG_NUWAX_VOICE_MIC_TOUCH_TRIGGER 1
#define CONFIG_NUWAX_TOUCH_INTERRUPT_ACTION_ENABLE 1
#define CONFIG_NUWAX_TOUCH_WAKE_ACTION_ENABLE 0
#define CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE 0
#define CONFIG_NUWAX_CONVERSATION_AWAKE_IDLE_TIMEOUT_MS 60000
#define CONFIG_NUWAX_WAKE_WORD_LISTEN_GRACE_MS 650
#define NUWAX_CONVERSATION_WAKE_TEXT_TIMEOUT_MS 15000
#define NUWAX_WAKE_WORD_GREETING_REQUEST_PREFIX "wake_"
#define NUWAX_WAKE_WORD_GREETING_TEXT "hello"
#define NUWAX_INTERACTIVE_CONTROL_SEND_TIMEOUT_MS 250
#define NUWAX_WAKE_EVENT_MAX_AGE_MS 3000
#define pdTRUE 1
#define memory_order_release memory_order_relaxed
typedef int nuwax_app_state_t;
enum { NUWAX_WAKE_MODE_SLEEP, NUWAX_WAKE_MODE_PLAYBACK_INTERRUPT };
enum { NUWAX_SLEEP_TRANSACTION_IDLE, NUWAX_SLEEP_TRANSACTION_SLEEPING };
enum { NUWAX_APP_SHELL_ACTION_NONE, NUWAX_APP_SHELL_ACTION_START_LISTEN, NUWAX_APP_SHELL_ACTION_STOP_LISTEN };
typedef struct { int action, index; } nuwax_app_shell_result_t;
typedef struct { struct { int mode; uint32_t ws_epoch; } arm; uint32_t detected_ms; char wake_word[32]; } wake_word_event_t;
typedef struct { bool start_gate_pending,start_gate_authorized; uint32_t start_gate_generation,submitted_generation; } nuwax_speaker_playback_stats_t;
static nuwax_conversation_visual_round_t s_visual_round;
static atomic_uint s_pending_speaker_start_generation;
static bool s_deferred_speaker_start_valid;
static struct { uint32_t speaker_generation; } s_deferred_speaker_start_event;
static uint32_t s_deferred_speaker_start_epoch;
static bool s_sleep_request_pending,s_sleep_request_requires_idle_expiry,s_awake_resume_after_ws_reconnect;
static uint32_t s_awake_disconnect_ms,s_consecutive_post_end_response_timeouts;
static char s_active_turn_id[48];
static struct { bool active; int state; } s_sleep_transaction;
static struct { bool active; } s_deferred_sleep_intent;
static bool s_power_shutdown_requested;
static struct { uint32_t wake_detected_ms; } s_wake_greeting_metrics;
static uint32_t s_cloud_text_request_seq;
static void *s_shell_actions=(void*)2;
static bool queued_wake,wake_desired,tts_injected;
static unsigned uxQueueMessagesWaiting(void *p) { return p==s_wake_word_events && queued_wake ? 1U : 0U; }
static unsigned real_loop_iterations,sends;
static int send_result,scenario;
static uint32_t harness_end_ms;
static void check_conversation_turn_timeout(nuwax_ws_client_t *ws);
static uint32_t active_ws_epoch_snapshot(void) { return 1; }
static void clear_head_touch_agent_intent(const char *r) { (void)r; }
static void clear_touch_cancel_recovery(const char *r) { (void)r; }
static void clear_ws_reconnect_audio_recovery(void) {}
static void clear_sleep_request(void) { s_sleep_request_pending=false; }
static void nuwax_state_bridge_cancel_touch(void) {}
static void inhibit_sleep_wake_word_detector(const char *r) { (void)r;wake_desired=false; }
static void stop_sleep_wake_word_detector(const char *r) { inhibit_sleep_wake_word_detector(r); }
static bool wake_detector_request_is_playback(void) { return false; }
static bool nuwax_wake_word_detector_is_running(void) { return false; }
static void nuwax_speaker_playback_get_stats(nuwax_speaker_playback_stats_t *s) { memset(s,0,sizeof(*s)); }
static bool nuwax_speaker_playback_abort_metadata_only_session(const char *a,const char *b,const char *r) { (void)a;(void)b;(void)r;return false; }
static bool nuwax_speaker_playback_reject_start_generation(uint32_t g,const char *r) { (void)g;(void)r;return false; }
static void clear_tts_skip_emotion_window(const char *r) { (void)r; }
static void nuwax_speaker_playback_release_idle_output(const char *r) { (void)r; }
static bool sleep_lifecycle_blocks_new_capture(void) { return s_sleep_request_pending||s_sleep_transaction.active||s_deferred_sleep_intent.active; }
static void clear_last_listen_resume(void) { nuwax_listen_resume_state_clear(&s_listen_resume); }
static uint32_t conversation_listen_window_ms(void) { return 60000; }
static void app_task_watchdog_checkpoint(void) { real_loop_iterations++; }
static void service_power_key_events(void *ws) { (void)ws; }
static bool service_music_url_test(bool b) { (void)b;return false; }
static bool nuwax_ws_is_connected(void *ws) { (void)ws;return now<harness_end_ms; }
static void complete_during_playback_wake_stop(void) {}
static void service_ota_wake_detector_recovery(void) {}
static bool nuwax_sleep_transaction_requires_local_sleep_reconcile(void *t,bool sleeping) { (void)t;(void)sleeping;return false; }
static void request_sleeping_conversation_state(const char *r) { enter_sleeping_conversation_state(r); }
static void service_wake_interrupt_cancel(void *ws) { (void)ws; }
static int64_t now_ms_i64(void) { return now; }
/* Transport heartbeat stays healthy; no cloud business output is fabricated. */
static int service_idle_wait_application_keepalive(void *ws) { (void)ws;return ESP_OK; }
static bool elapsed_ms(uint32_t n,uint32_t d) { return (int32_t)(n-d)>=0; }
static void clear_auto_followup(const char *r) { (void)r;nuwax_prelisten_state_clear(&s_prelisten); }
static uint32_t activate_pending_prelisten_capture(uint32_t n,const char *r) { (void)r;return nuwax_prelisten_state_activate_pending(&s_prelisten,n); }
static bool handle_playback_wake_event(void *ws,void *e) { (void)ws;(void)e;return false; }
static void nuwax_sleep_transaction_reset_for_wake(void *t) { (void)t;s_sleep_transaction.state=NUWAX_SLEEP_TRANSACTION_IDLE; }
static void wake_greeting_metrics_begin(const char *w,uint32_t t) { (void)w;s_wake_greeting_metrics.wake_detected_ms=t; }
static void wake_greeting_metrics_mark_sent(const char *r,int e) { (void)r;(void)e; }
static int nuwax_session_send_text_request_with_timeout_if_epoch(void *ws,const char *r,const char *t,uint32_t ms,uint32_t e) { (void)ws;(void)r;(void)t;assert(ms==250&&e==1);sends++;return send_result; }
static int conversation_live_aec_pool_init(void) { return ESP_OK; }
static void restart_if_post_reprovision_aec_failed(const char *r) { (void)r; }
static bool take_deferred_shell_listen_action(void *p) { (void)p;return false; }
static int xQueueReceive(void *q,void *p,int timeout) { (void)timeout;if(q==s_wake_word_events&&queued_wake){queued_wake=false;wake_word_event_t *e=p;e->arm.ws_epoch=1;e->arm.mode=NUWAX_WAKE_MODE_SLEEP;e->detected_ms=now;strcpy(e->wake_word,"word");return pdTRUE;}return 0; }
static void conversation_owner_wait_ms(uint32_t ms) { now+=ms; }
static void start_prelisten_capture_now(const char *m,uint32_t w,const char *r) { (void)r;nuwax_prelisten_state_start_capture(&s_prelisten,m,now,w,false);state=NUWAX_APP_STATE_LISTENING;capture_starts++; }
static void handle_shell_action(void *ws,void *a) { (void)ws;(void)a; }
static void drain_session_commands(nuwax_ws_client_t *ws) {
    if(scenario==1&&s_turn_guard.active&&!tts_injected&&now>=1100) {
        /* Same transition as TEXT_RESPONSE(ok,tts_pending) at production line 30150. */
        assert(nuwax_turn_guard_transition(&s_turn_guard,NUWAX_TURN_PHASE_WAIT_TEXT,NUWAX_TURN_PHASE_WAIT_TTS,now,30000));
        tts_injected=true;
    }
    check_conversation_turn_timeout(ws);
}
'''
names = ['clear_conversation_visual_round','clear_active_turn_guard','begin_active_turn_guard',
         'begin_awake_session','begin_conversation_visual_round','start_prelisten_window',
         'return_to_listening_after_cancel','recover_normal_conversation_after_playback_abort',
         'awake_idle_sleep_admitted','maybe_enter_idle_sleep','ensure_sleep_wake_word_detector',
         'check_conversation_turn_timeout','send_wake_word_greeting','wait_for_mic_start_action']
funcs = {name: extract(name) for name in names}
tests = r'''
static void reset(int kind) {
    memset(&s_awake_session,0,sizeof(s_awake_session));nuwax_prelisten_state_clear(&s_prelisten);
    nuwax_turn_guard_clear(&s_turn_guard);nuwax_playback_terminal_guard_clear(&s_playback_terminal_guard);
    nuwax_conversation_visual_round_clear(&s_visual_round);clear_last_listen_resume();
    scenario=kind;now=100;harness_end_ms=600100;state=NUWAX_APP_STATE_SLEEPING;
    send_result=kind==2?ESP_ERR_INVALID_STATE:ESP_OK;
    queued_wake=true;wake_desired=false;tts_injected=false;s_wake_word_events=(void*)1;
    real_loop_iterations=sends=0;capture_starts=wake_rearms=cleared=status_updates=close_requests=0;
    s_sleep_transaction.state=NUWAX_SLEEP_TRANSACTION_SLEEPING;
}
int main(void) {
    int ws=0;
    for(int kind=0;kind<3;kind++) {
        reset(kind);
        const int result=wait_for_mic_start_action(&ws);
        assert(sends==1);
        if(kind<2) {
            assert(result==ESP_ERR_INVALID_STATE&&now==harness_end_ms);
            assert(state==NUWAX_APP_STATE_SLEEPING&&s_awake_session.active&&!s_awake_session.waiting);
            assert(!s_turn_guard.active&&!s_prelisten.pending&&!s_prelisten.capture_active&&!wake_desired&&!capture_starts&&!wake_rearms);
            assert(!nuwax_listen_resume_state_can_bootstrap_cloud_turn(&s_listen_resume,s_awake_session.generation,1,false));
            assert(nuwax_playback_terminal_guard_apply_cancel_ack(&s_playback_terminal_guard,1,"late_cancel","old_turn","old_play",true)==NUWAX_PLAYBACK_CANCEL_ACK_UNMATCHED);
            printf("BASELINE REPRODUCED scenario=%d real_wait_iterations=%u sends=%u simulated_ms=600000 capture=0 wake=0 late_turn_bootstrap=rejected late_cancel_ack=unmatched\n",kind,real_loop_iterations,sends);
        } else {
            assert(result==ESP_OK&&s_prelisten.capture_active&&state==NUWAX_APP_STATE_LISTENING);
            printf("CONTROL PASSED send_failure: real_wait returns ESP_OK at simulated_ms=%lu capture=1\n",(unsigned long)(now-100));
        }
    }
    return 0;
}
'''
generated = OUT/'real_wait_baseline.c'
generated.write_text(prefix+extra+'\n\n'.join(funcs.values())+tests,encoding='utf-8')
cmd=['gcc','-std=c11','-Wall','-Wextra','-Wno-unused-parameter','-Wno-unused-variable','-I',str(ROOT/'main'),str(generated),
     *[str(ROOT/'main/conversation'/f) for f in ['awake_session_state.c','turn_guard.c','playback_terminal_guard.c','prelisten_state.c','listen_resume_state.c']],
     '-o',str(OUT/'real_wait_baseline.exe')]
built=subprocess.run(cmd,capture_output=True,text=True)
log=built.stdout+built.stderr
if built.returncode:
    (OUT/'real-wait-output.txt').write_text(log);print(log);raise SystemExit(built.returncode)
result=subprocess.run([str(OUT/'real_wait_baseline.exe')],capture_output=True,text=True)
log+=result.stdout+result.stderr
(OUT/'real-wait-output.txt').write_text(log,encoding='utf-8');print(log)
assert result.returncode==0
(OUT/'source-evidence.json').write_text(json.dumps({
    'candidate':'R1-RECHECK-REAL-WAIT-v1','sourceSha256':hashlib.sha256((ROOT/'main/nuwax_ai_partner_main.c').read_bytes()).hexdigest(),
    'extractedFunctionSha256Utf8LF':{n:hashlib.sha256(b.encode()).hexdigest() for n,b in funcs.items()},
    'hardwareValidated':False,'simulationMillisecondsPerBaseline':600000,
    'limitations':['RTOS queues/time, cloud transport, hardware speaker and unrelated idle services are stubbed.',
                   'No response case executes real wait, wake send, awake begin, visual ownership, timeout, clear guard, recovery, idle and wake-supervisor admission functions.',
                   'TTS-pending case injects the same production turn transition; does not run the entire cloud event decoder.',
                   'Harness ends baseline by explicitly disconnecting only after 600000 simulated ms; it is not a device timeout.']
},indent=2),encoding='utf-8')
