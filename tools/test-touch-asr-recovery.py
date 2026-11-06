"""Execute real capture arbitration, owner service and ASR dispatch boundaries."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
app = (root / 'main/nuwax_ai_partner_main.c').read_text(encoding='utf-8-sig')


def function(signature):
    # Definitions contain an opening brace before the next semicolon.
    pos = 0
    while True:
        start = app.index(signature, pos)
        brace = app.index('{', start)
        if ';' not in app[start:brace]:
            break
        pos = start + len(signature)
    end, depth = brace + 1, 1
    while depth:
        depth += (app[end] == '{') - (app[end] == '}')
        end += 1
    return app[start:end]


frame = function('static bool head_touch_agent_consume_capture_frame(')
service = function('static void service_head_touch_agent_intent(')
terminal_service = function('static esp_err_t service_audio_terminal_recovery(')
handler = function('static void handle_session_event(')
asr = handler[handler.index('    const bool audio_input_event ='):
              handler.index('    switch (command->event)')]
common = r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "conversation/head_touch_agent_intent.h"
#include "conversation/listen_resume_state.h"
#define CONFIG_NUWAX_SPEAKER_ENABLE 0
#define CONFIG_NUWAX_OTA_ENABLE 0
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE -1
#define NUWAX_WS_CLOSE_CAUSE_TRANSPORT_INTEGRITY 1
#define NUWAX_SHAKE_AGENT_REQUEST_PREFIX "shake_"
#define NUWAX_HEAD_TOUCH_AGENT_REQUEST_PREFIX "touch_"
#define NUWAX_SHAKE_AGENT_TEXT "shake"
#define NUWAX_HEAD_TOUCH_AGENT_TEXT "touch"
#define NUWAX_CONVERSATION_TEXT_TIMEOUT_MS 15000U
#define NUWAX_INTERACTIVE_CONTROL_SEND_TIMEOUT_MS 500U
#define NUWAX_POST_END_RESPONSE_TIMEOUT_MS 15000U
#define CONFIG_NUWAX_CONVERSATION_AWAKE_IDLE_TIMEOUT_MS 60000U
#define NUWAX_VISUAL_ROUND_SHAKE 1
#define NUWAX_VISUAL_ROUND_HEAD_TOUCH 2
typedef int esp_err_t;
typedef struct { bool connected; uint32_t epoch; } nuwax_ws_client_t;
static nuwax_ws_client_t ws = {true, 10U};
static uint32_t epoch=10U, now=100U, s_awake_session=6U;
static nuwax_head_touch_agent_intent_t s_head_touch_agent_intent;
static nuwax_listen_resume_state_t s_listen_resume;
static struct {bool active; char request_id[48];} s_turn_guard;
static struct {bool active;} s_notification_transaction;
static struct {bool capture_active;} s_prelisten;
static struct {bool active; uint32_t ws_epoch,token;} s_sleep_transaction;
static atomic_bool s_conversation_capture_runtime_active;
static bool blocked, recovery_pending;
static unsigned sends, resumes, normal_asr, shake_visuals;
static uint32_t s_cloud_text_request_seq, s_consecutive_post_end_response_timeouts;
static char clear_reason[80];
static uint32_t monotonic_ms(void) {return now;}
static uint32_t active_ws_epoch_snapshot(void) {return epoch;}
static uint32_t nuwax_awake_session_state_generation(const uint32_t *s) {return *s;}
static bool nuwax_awake_session_state_is_active(const uint32_t *s) {return *s!=0;}
static bool nuwax_ws_is_connected(nuwax_ws_client_t *s) {return s->connected;}
static uint32_t nuwax_ws_connection_epoch(nuwax_ws_client_t *s) {return s->epoch;}
static bool nuwax_ws_request_close_if_epoch(nuwax_ws_client_t *s, uint32_t e) {
    if(s->epoch==e) s->connected=false; return true;
}
static bool head_touch_agent_is_blocked(void) {return blocked;}
static void clear_head_touch_agent_intent(const char *reason) {
    snprintf(clear_reason,sizeof(clear_reason),"%s",reason);
    nuwax_head_touch_agent_intent_clear(&s_head_touch_agent_intent);
}
static void clear_active_turn_guard(const char *s) {(void)s;s_turn_guard.active=false;}
static bool resume_prelisten_after_no_speech(void) {resumes++;return true;}
static bool begin_head_touch_agent_quiesce(nuwax_ws_client_t *s) {
    (void)s;return s_head_touch_agent_intent.state==NUWAX_HEAD_TOUCH_AGENT_STATE_QUIESCING;
}
static bool nuwax_audio_terminal_recovery_pending(void) {return recovery_pending;}
typedef struct {
    bool success,quarantined; uint32_t ws_epoch,failures,age_ms; char audio_session_id[48];
} nuwax_audio_terminal_recovery_completion_t;
static nuwax_audio_terminal_recovery_completion_t completion;
static int nuwax_audio_terminal_recovery_service(nuwax_ws_client_t *s) {(void)s;return 0;}
static bool nuwax_audio_terminal_recovery_take_completion(nuwax_audio_terminal_recovery_completion_t *c) {
    if(!recovery_pending)return false;*c=completion;recovery_pending=false;return true;
}
#define nuwax_sleep_transaction_mark_protocol_closed(...) (0)
#define service_sleep_transaction(...) ((void)0)
static bool nuwax_ws_close_if_epoch(nuwax_ws_client_t *s,uint32_t e,int cause) {
    (void)cause;return nuwax_ws_request_close_if_epoch(s,e);
}
static bool begin_active_turn_guard(const char *r, const char *t, uint32_t ms, const char *s) {
    (void)t;(void)ms;(void)s;assert(!s_turn_guard.active);
    s_turn_guard.active=true;snprintf(s_turn_guard.request_id,48,"%s",r);return true;
}
static bool begin_conversation_visual_round(int kind,const char *r,const char *s) {
    (void)kind;(void)r;(void)s;return true;
}
static void clear_last_listen_resume(void) {nuwax_listen_resume_state_clear(&s_listen_resume);}
static int nuwax_session_send_text_request_with_timeout_if_epoch(
    nuwax_ws_client_t *s,const char *r,const char *text,uint32_t ms,uint32_t e) {
    (void)r;(void)text;(void)ms; assert(s->connected && s->epoch==e);
    assert(!atomic_load(&s_conversation_capture_runtime_active));
    assert(!s_prelisten.capture_active);sends++;return ESP_OK;
}
static bool resume_awake_conversation_after_inactivity(const char *s) {(void)s;return true;}
#define enter_sleeping_conversation_state(...) ((void)0)
#define metrics_begin_prelisten(...) ((void)0)
#define invalidate_all_pending_semantic_emotions(...) (0)
#define nuwax_state_bridge_clear_waiting_question(...) ((void)0)
#define nuwax_state_bridge_pin_shake(...) ((void)0)
#define nuwax_state_bridge_pin_touch(...) ((void)0)
#define pause_awake_waiting(...) ((void)0)
#define set_conversation_status_guarded(...) ((void)0)
static void nuwax_state_bridge_trigger_shake(void) {shake_visuals++;}
enum { NUWAX_SESSION_EVENT_AUDIO_PROGRESS, NUWAX_SESSION_EVENT_ASR_PARTIAL,
       NUWAX_SESSION_EVENT_ASR_FINAL, NUWAX_SESSION_EVENT_ASR_RESULT };
typedef struct {int event; bool ok; char audio_session_id[48]; char error[40]; int sequence; char text[128];} command_t;
static bool is_no_speech_asr_error(const char *s) {return strcmp(s,"no_speech")==0;}
'''
dispatch = 'static void dispatch_asr(const command_t *command, uint32_t event_ws_epoch) {\n' + asr + '\nnormal_asr++;\n}\n'
tests = r'''
static void start(void) {
    ws.connected=true;ws.epoch=epoch=10U;s_awake_session=6U;now=100U;
    blocked=recovery_pending=false;sends=resumes=normal_asr=shake_visuals=0;
    s_turn_guard.active=false;s_notification_transaction.active=false;
    s_prelisten.capture_active=true;atomic_store(&s_conversation_capture_runtime_active,true);
    nuwax_head_touch_agent_intent_clear(&s_head_touch_agent_intent);
    nuwax_listen_resume_state_clear(&s_listen_resume);
    s_listen_resume.awake_generation=6U;strcpy(s_listen_resume.audio_session_id,"a1");
    assert(nuwax_head_touch_agent_intent_arm(&s_head_touch_agent_intent,
        NUWAX_HEAD_TOUCH_AGENT_SOURCE_LISTENING,10U,6U,0U,true,false,NULL,now,60U));
}
static void contact_noise(void) {
    assert(!head_touch_agent_consume_capture_frame(&ws,true,true,"a1",NULL,true,true));
    assert(s_head_touch_agent_intent.asr_deferred);
    service_head_touch_agent_intent(&ws);
    assert(!sends && atomic_load(&s_conversation_capture_runtime_active));
}
static void release(void) {
    assert(nuwax_head_touch_agent_intent_mark_terminal_released(&s_head_touch_agent_intent,10U,"a1"));
    assert(nuwax_head_touch_agent_intent_mark_capture_released(&s_head_touch_agent_intent,10U,6U,"a1"));
    atomic_store(&s_conversation_capture_runtime_active,false);s_prelisten.capture_active=false;
    assert(nuwax_listen_resume_state_arm_post_end_asr(&s_listen_resume,10U,15000U));
}
static void tick(void) { for(int i=0;i<5;i++)service_head_touch_agent_intent(&ws); }
int main(void) {
    command_t no={.event=NUWAX_SESSION_EVENT_ASR_RESULT,.ok=false,.audio_session_id="a1",.error="no_speech"};
    start();contact_noise();release();
    dispatch_asr(&no,10U);assert(s_head_touch_agent_intent.asr_no_speech && !normal_asr);
    service_head_touch_agent_intent(&ws); /* enters quiesce */
    dispatch_asr(&no,10U); /* duplicate must not cancel recovered touch */
    tick();assert(sends==1 && !resumes);

    start();contact_noise();release();
    command_t real={.event=NUWAX_SESSION_EVENT_ASR_FINAL,.ok=true,.audio_session_id="a1",.sequence=1,.text="hello"};
    dispatch_asr(&real,10U);tick();assert(!sends && normal_asr==1);
    assert(!strcmp(clear_reason,"matching_cloud_speech_won"));

    start();contact_noise();release();strcpy(no.audio_session_id,"old");
    dispatch_asr(&no,10U);tick();assert(!sends && !normal_asr);
    assert(!s_head_touch_agent_intent.asr_no_speech);strcpy(no.audio_session_id,"a1");
    now=4100U;tick();assert(!sends && !resumes);
    assert(!strcmp(clear_reason,"deferred_asr_timeout"));
    dispatch_asr(&no,10U);assert(normal_asr==1); /* normal ASR recovery still runs */

    start();contact_noise();dispatch_asr(&no,10U); /* pre-END result */
    assert(!sends && normal_asr==1 && s_head_touch_agent_intent.state==0);

    start();contact_noise();release();dispatch_asr(&no,9U);
    assert(!sends && !s_head_touch_agent_intent.asr_no_speech);

    for(int scenario=0;scenario<5;scenario++) {
        start();contact_noise();release();dispatch_asr(&no,10U);
        if(scenario==0)blocked=true;
        if(scenario==1)s_notification_transaction.active=true;
        if(scenario==2)ws.epoch=epoch=11U;
        if(scenario==3)s_awake_session=7U;
        if(scenario==4)strcpy(s_listen_resume.audio_session_id,"new_capture");
        tick();assert(!sends && s_head_touch_agent_intent.state==0);
    }
    for(int stage=1;stage<=3;stage++) {
        start();contact_noise();release();dispatch_asr(&no,10U);
        for(int i=0;i<stage;i++)service_head_touch_agent_intent(&ws);
        strcpy(s_listen_resume.audio_session_id,"new_capture");
        tick();assert(!sends && s_head_touch_agent_intent.state==0);
        assert(!strcmp(s_listen_resume.audio_session_id,"new_capture"));
    }
    start();contact_noise();release();dispatch_asr(&no,10U);now=4100U;
    tick();assert(!sends && resumes==1);

    start();contact_noise();release();tick(); /* completion alone is insufficient */
    assert(!sends && !s_head_touch_agent_intent.asr_no_speech);
    s_turn_guard.active=true;tick();assert(!sends && s_head_touch_agent_intent.state==0);

    /* A delayed END/STOP completion must retain the ordinary ASR watchdog,
     * including after the shorter physical-interaction wait expires. */
    start();contact_noise();
    atomic_store(&s_conversation_capture_runtime_active,false);s_prelisten.capture_active=false;
    assert(nuwax_head_touch_agent_intent_mark_capture_released(&s_head_touch_agent_intent,10U,6U,"a1"));
    completion=(nuwax_audio_terminal_recovery_completion_t){.success=true,.ws_epoch=10U};
    strcpy(completion.audio_session_id,"a1");recovery_pending=true;
    assert(service_audio_terminal_recovery(&ws)==0);
    assert(s_head_touch_agent_intent.terminal_released);
    assert(nuwax_listen_resume_state_is_waiting_post_end_asr(&s_listen_resume));
    now=4100U;tick();assert(!sends && s_head_touch_agent_intent.state==0);
    assert(nuwax_listen_resume_state_is_waiting_post_end_asr(&s_listen_resume));

    start();now=161U;
    assert(head_touch_agent_consume_capture_frame(&ws,true,true,"a1",NULL,false,false));
    assert(s_head_touch_agent_intent.state==NUWAX_HEAD_TOUCH_AGENT_STATE_QUIESCING);
    assert(!s_head_touch_agent_intent.asr_deferred); /* normal touch still immediate */

    start();s_head_touch_agent_intent.interaction=NUWAX_AGENT_INTERACTION_SHAKE;
    assert(head_touch_agent_consume_capture_frame(&ws,true,true,"a1",NULL,true,true));
    assert(shake_visuals==1 && !s_head_touch_agent_intent.asr_deferred);
    puts("PASS: production touch ASR arbitration, release barriers, real speech, stale results, lifecycle, timeout, shake");
}
'''
with tempfile.TemporaryDirectory(prefix='touch-asr-') as temp:
    src = Path(temp) / 'check.c'
    exe = Path(temp) / 'check.exe'
    def run(frame_source, service_source, terminal_source, expect_pass=True):
        src.write_text(common + frame_source + service_source + terminal_source + dispatch + tests, encoding='utf-8')
        subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-variable', '-Wno-misleading-indentation',
                        '-I' + str(root / 'main'), str(src),
                        str(root / 'main/conversation/head_touch_agent_intent.c'),
                        str(root / 'main/conversation/listen_resume_state.c'), '-o', str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        assert (result.returncode == 0) == expect_pass, result.stdout + result.stderr
        if expect_pass:
            print(result.stdout.strip())

    run(frame, service, terminal_service)
    # Reproduce the original irreversible VAD cancellation in the actual
    # arbitration function, then prove the regression scenario detects it.
    start = frame.index('    if (user_speech_confirmed && listen_started && upload_started &&')
    end = frame.index('    const bool shake_listening_bypasses_chat_guard', start)
    run(frame[:start] + frame[end:], service, terminal_service, False)
    print('PASS: original local-VAD cancellation detected at runtime')
    old_watchdog = terminal_service.replace(
        '(!head_touch_round_matches || s_head_touch_agent_intent.asr_deferred)',
        '!head_touch_round_matches')
    assert old_watchdog != terminal_service
    run(frame, service, old_watchdog, False)
    print('PASS: missing deferred-ASR terminal watchdog detected at runtime')
