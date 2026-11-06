"""Compile audit-only scoped candidate and original reconnect state functions."""
from pathlib import Path
import subprocess,re,hashlib,json,difflib
OUT=Path(__file__).resolve().parent
ROOT = next(p for p in Path(__file__).resolve().parents if (p / 'main/nuwax_ai_partner_main.c').is_file())
source=(ROOT/'main/nuwax_ai_partner_main.c').read_text(encoding='utf-8-sig')
base=(OUT/'real_wait_baseline.c').read_text()

def extract(name,text=source):
    m=re.search(r'^static [^;{}]*\b'+name+r'\([^;{}]*\)\s*\{',text,re.M);assert m,name
    p=text.index('{',m.start())+1;depth=1
    while depth:
        if text[p]=='{':depth+=1
        if text[p]=='}':depth-=1
        p+=1
    return text[m.start():p]

# Retain reset(), replace only test main and audited timeout routing.
base=base[:base.index('\nint main(void) {')]
base=base.replace('#include <assert.h>','#include <assert.h>\n#include "candidate_policy.h"\nstatic unsigned dynamic_epoch=1;\nstatic bool transport_closed, menu_open, mode_owner, mode_pending;')
base=base.replace('return 1; }\nstatic uint32_t update_normal_downlink_admission', 'return dynamic_epoch; }\nstatic uint32_t update_normal_downlink_admission')
base=base.replace('static uint32_t active_ws_epoch_snapshot(void) { return 1; }','static uint32_t active_ws_epoch_snapshot(void) { return dynamic_epoch; }')
base=base.replace('return now<harness_end_ms;', 'return !transport_closed && now<harness_end_ms;')
base=base.replace('static bool nuwax_app_shell_blocks_auto_followup(void) { return false; }','static bool nuwax_app_shell_blocks_auto_followup(void) { return menu_open; }')
base=base.replace('static bool exclusive_audio_mode_blocks_normal_audio(void) { return false; }','static bool exclusive_audio_mode_blocks_normal_audio(void) { return mode_pending||mode_owner; }')
base=base.replace('static void clear_ws_reconnect_audio_recovery(void) {}','static void clear_ws_reconnect_audio_recovery(void);')
base=base.replace('assert(ms==250&&e==1);','assert(ms==250&&e==dynamic_epoch);')
addition=r'''
static wg_ticket_t pending_greeting_recovery;
static bool ota_active;
static bool candidate_service_recovery(nuwax_ws_client_t *ws) {
    (void)ws;
    const wg_action_t action=wg_decide(&pending_greeting_recovery,dynamic_epoch,
        s_awake_session.generation,s_turn_guard.epoch,s_turn_guard.active,
        !transport_closed,s_awake_session.active,
        s_sleep_transaction.active||s_sleep_transaction.state==NUWAX_SLEEP_TRANSACTION_SLEEPING||s_sleep_request_pending||s_deferred_sleep_intent.active,
        s_power_shutdown_requested,ota_active,atomic_load(&s_reprovision_quiesce_requested),false,mode_owner||mode_pending);
    if(action==WG_ROTATE_EPOCH) {
        assert(wg_candidate_close_admits(WG_CLOSE_UNRESOLVED_GREETING,true));
        close_requests++;transport_closed=true;pending_greeting_recovery.valid=false;return true;
    }
    if(action==WG_YIELD_NEW_OWNER)pending_greeting_recovery.valid=false;
    return action==WG_DEFER_OTA||action==WG_YIELD_NEW_OWNER;
}
'''
marker='static void check_conversation_turn_timeout(nuwax_ws_client_t *ws) {'
base=base.replace(marker,addition+'\n'+marker)
needle='    const uint32_t epoch = s_turn_guard.epoch;'
replacement='''    const wg_failure_t recovery_failure = first_pcm_timeout ? WG_FAILURE_FIRST_PCM_TIMEOUT :
        s_turn_guard.phase==NUWAX_TURN_PHASE_WAIT_TEXT ? WG_FAILURE_TEXT_TIMEOUT :
        s_turn_guard.phase==NUWAX_TURN_PHASE_WAIT_TTS ? WG_FAILURE_TTS_TIMEOUT : WG_FAILURE_NONE;
    const wg_ticket_t greeting_ticket=wg_capture(&s_visual_round,&s_turn_guard,
        active_ws_epoch_snapshot(),s_awake_session.generation,recovery_failure);
'''+needle
assert needle in base;base=base.replace(needle,replacement,1)
needle='''        nuwax_speaker_playback_request_fade_cancel(timeout_reason, 0U);
        recover_normal_conversation_after_playback_abort('''
replacement='''        nuwax_speaker_playback_request_fade_cancel(timeout_reason, 0U);
        if(greeting_ticket.valid) {
            pending_greeting_recovery=greeting_ticket;
            if(candidate_service_recovery(ws))return;
        }
        recover_normal_conversation_after_playback_abort('''
assert needle in base;base=base.replace(needle,replacement,1)
base=base.replace('        service_ota_wake_detector_recovery();','        service_ota_wake_detector_recovery();\n        (void)candidate_service_recovery(ws);')
support=r'''
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define NUWAX_AWAKE_WS_RESUME_GRACE_MS 30000
#define NUWAX_WS_RECONNECT_AUDIO_RECOVERY_TIMEOUT_MS 5000
typedef enum {NUWAX_WS_RECOVERY_CONVERGED,NUWAX_WS_RECOVERY_WAIT_MODE_QUEUE,
 NUWAX_WS_RECOVERY_TRANSFERRED_TO_OWNER,NUWAX_WS_RECOVERY_MENU_BLOCKED,
 NUWAX_WS_RECOVERY_FALLBACK_SLEEP} nuwax_ws_recovery_result_t;
static struct {bool pending;uint32_t ws_epoch,awake_generation,deadline_ms;} s_ws_audio_recovery;
static bool s_during_playback_handoff_pending,s_during_playback_handoff_playback_started,s_during_playback_handoff_seamless;
static bool s_during_playback_wake_stop_pending,s_during_playback_wake_stop_playback_started;
static int s_wake_stop_handoff_lock;
static bool exclusive_audio_owner_active(void){return mode_owner;}
static void clear_awake_session(const char *r){(void)r;nuwax_awake_session_state_clear(&s_awake_session);clear_ws_reconnect_audio_recovery();}
'''
names=['pause_awake_waiting','clear_ws_reconnect_audio_recovery','preserve_awake_session_for_ws_reconnect',
       'arm_ws_reconnect_audio_recovery','recover_awake_conversation_after_ws_reconnect']
funcs={n:extract(n) for n in names}
tests=r'''
static void candidate_reset(int scenario) {
    transport_closed=menu_open=mode_owner=mode_pending=ota_active=false;dynamic_epoch=1;
    pending_greeting_recovery=(wg_ticket_t){0};reset(scenario);
}
static void reconnect_on_new_epoch(void) { transport_closed=false;dynamic_epoch=2;now+=1000; }
int main(void) {
    int ws=0;
    assert(!nuwax_ws_close_policy_admits(NUWAX_WS_CLOSE_CAUSE_BUSINESS_RECOVERY,true));
    assert(!nuwax_ws_close_policy_admits(NUWAX_WS_CLOSE_CAUSE_BUSINESS_STALLED,true));
    assert(wg_candidate_close_admits(WG_CLOSE_UNRESOLVED_GREETING,true));
    puts("PASS generic close stays suppressed with healthy heartbeat; new scoped cause is required");
    for(int scenario=0;scenario<2;scenario++) {
        candidate_reset(scenario);
        assert(wait_for_mic_start_action(&ws)==ESP_ERR_INVALID_STATE);
        assert(close_requests==1&&transport_closed&&capture_starts==0);
        const uint32_t elapsed=now-100;
        assert(elapsed==((scenario==0)?15100U:31100U));
        assert(preserve_awake_session_for_ws_reconnect("candidate_epoch_rotation"));
        assert(s_awake_resume_after_ws_reconnect&&s_awake_session.active&&!s_awake_session.waiting);
        reconnect_on_new_epoch();
        assert(arm_ws_reconnect_audio_recovery(&ws,"candidate"));
        assert(recover_awake_conversation_after_ws_reconnect(&ws,"candidate")==NUWAX_WS_RECOVERY_CONVERGED);
        assert(s_prelisten.pending&&state==NUWAX_APP_STATE_LISTENING);
        printf("CANDIDATE scenario=%d real_wait_ms=%lu requests_one_epoch_rotation; original preserve+reconnect functions arm listening on epoch=2\n",scenario,(unsigned long)elapsed);
    }
    candidate_reset(0);assert(wait_for_mic_start_action(&ws)==ESP_ERR_INVALID_STATE);
    assert(preserve_awake_session_for_ws_reconnect("menu"));reconnect_on_new_epoch();menu_open=true;
    assert(arm_ws_reconnect_audio_recovery(&ws,"menu"));
    assert(recover_awake_conversation_after_ws_reconnect(&ws,"menu")==NUWAX_WS_RECOVERY_MENU_BLOCKED);
    assert(state==NUWAX_APP_STATE_IDLE&&!s_prelisten.pending);menu_open=false;maybe_enter_idle_sleep();
    assert(s_prelisten.pending);puts("PASS reconnect preserves open menu; closing menu resumes via original idle code");

    candidate_reset(0);begin_awake_session("policy");assert(begin_active_turn_guard("wake_99",NULL,15000,"policy"));
    assert(begin_conversation_visual_round(NUWAX_VISUAL_ROUND_WAKE_GREETING,"wake_99","policy"));
    wg_ticket_t t=wg_capture(&s_visual_round,&s_turn_guard,1,s_awake_session.generation,WG_FAILURE_TEXT_TIMEOUT);
    assert(t.valid);s_visual_round.first_pcm_seen=true;
    assert(!wg_capture(&s_visual_round,&s_turn_guard,1,s_awake_session.generation,WG_FAILURE_TEXT_TIMEOUT).valid);
    assert(wg_decide(&t,2,t.awake_generation,t.turn_epoch,false,true,true,false,false,false,false,false,false)==WG_YIELD_NEW_OWNER);
    assert(wg_decide(&t,1,t.awake_generation+1,t.turn_epoch,false,true,true,false,false,false,false,false,false)==WG_YIELD_NEW_OWNER);
    assert(wg_decide(&t,1,t.awake_generation,t.turn_epoch+1,false,true,true,false,false,false,false,false,false)==WG_YIELD_NEW_OWNER);
    assert(wg_decide(&t,1,t.awake_generation,t.turn_epoch,false,true,true,true,false,false,false,false,false)==WG_YIELD_NEW_OWNER);
    assert(wg_decide(&t,1,t.awake_generation,t.turn_epoch,false,true,true,false,true,false,false,false,false)==WG_YIELD_NEW_OWNER);
    assert(wg_decide(&t,1,t.awake_generation,t.turn_epoch,false,true,true,false,false,true,false,false,false)==WG_DEFER_OTA);
    assert(wg_decide(&t,1,t.awake_generation,t.turn_epoch,false,true,true,false,false,false,true,false,false)==WG_YIELD_NEW_OWNER);
    assert(wg_decide(&t,1,t.awake_generation,t.turn_epoch,false,true,true,false,false,false,false,true,false)==WG_YIELD_NEW_OWNER);
    assert(wg_decide(&t,1,t.awake_generation,t.turn_epoch,false,true,true,false,false,false,false,false,true)==WG_YIELD_NEW_OWNER);
    assert(wg_decide(&t,1,t.awake_generation,t.turn_epoch,false,false,true,false,false,false,false,false,false)==WG_YIELD_NEW_OWNER);
    puts("PASS ticket identity/first-PCM/active sleep/shutdown/OTA/reprovision/notification/exclusive/disconnect policy fences");
    assert(!nuwax_ws_close_requested_for_epoch(2,1));
    puts("PASS an old-epoch close does not affect the replacement connection");
    candidate_reset(0);assert(wait_for_mic_start_action(&ws)==ESP_ERR_INVALID_STATE);
    assert(preserve_awake_session_for_ws_reconnect("grace_failure"));
    now=s_awake_disconnect_ms+30001;
    /* Exact outer-owner condition at production lines 34314-34320.
       enter_sleeping stub models its state/wake intent, not hardware readiness. */
    if(s_awake_resume_after_ws_reconnect&&elapsed_ms(now,s_awake_disconnect_ms+NUWAX_AWAKE_WS_RESUME_GRACE_MS))
        enter_sleeping_conversation_state("ws_reconnect_grace_expired");
    assert(!s_awake_session.active&&state==NUWAX_APP_STATE_SLEEPING&&wake_rearms==1);
    puts("PASS reconnect grace expiry condition releases awake owner and requests wake-ready sleep (hardware wake startup not simulated)");
    return 0;
}
'''
candidate=base+support+'\n\n'.join(funcs.values())+tests
(OUT/'real_wait_candidate.c').write_text(candidate)
(OUT/'candidate-vs-baseline.patch').write_text(''.join(difflib.unified_diff((OUT/'real_wait_baseline.c').read_text().splitlines(True),candidate.splitlines(True),fromfile='audit/real_wait_baseline.c',tofile='audit/real_wait_candidate.c')))
cmd=['gcc','-std=c11','-Wall','-Wextra','-Wno-unused-parameter','-Wno-unused-variable','-I',str(ROOT/'main'),str(OUT/'real_wait_candidate.c'),
     *[str(ROOT/'main/conversation'/f) for f in ['awake_session_state.c','turn_guard.c','playback_terminal_guard.c','prelisten_state.c','listen_resume_state.c']],'-o',str(OUT/'real_wait_candidate.exe')]
built=subprocess.run(cmd,capture_output=True,text=True);log=built.stdout+built.stderr
if not built.returncode:
    result=subprocess.run([str(OUT/'real_wait_candidate.exe')],capture_output=True,text=True);log+=result.stdout+result.stderr
else:result=built
(OUT/'candidate-output.txt').write_text(log);print(log);assert result.returncode==0
(OUT/'candidate-evidence.json').write_text(json.dumps({'candidate':'WG-EPOCH-RECOVERY-v1','productionModified':False,'hardwareValidated':False,
    'sourceSha256':hashlib.sha256((ROOT/'main/nuwax_ai_partner_main.c').read_bytes()).hexdigest(),
    'candidatePolicySha256':hashlib.sha256((OUT/'candidate_policy.h').read_bytes()).hexdigest(),
    'additionalRealFunctions':{n:hashlib.sha256(b.encode()).hexdigest() for n,b in funcs.items()},
    'limitations':['Candidate close flag simulates transport close mailbox; no live socket, physical teardown, authentication or network retry executed.',
                   'Reconnect success is injected after 1000ms to exercise original recovery functions. This is not a latency prediction.',
                   'Lifecycle fence cases exercise candidate policy; they are not full OTA/provisioning/mode integration tests.']},indent=2))
