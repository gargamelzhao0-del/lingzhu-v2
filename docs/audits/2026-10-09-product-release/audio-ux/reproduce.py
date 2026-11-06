"""Read-only source audit. Writes only this audit directory; requires Python/GCC."""
from pathlib import Path
import hashlib
import json
import subprocess

OUT = Path(__file__).resolve().parent
ROOT = next(p for p in Path(__file__).resolve().parents if (p / "main/nuwax_ai_partner_main.c").is_file())
MAIN = ROOT / "main/nuwax_ai_partner_main.c"
source = MAIN.read_text(encoding="utf-8-sig")
shell = (ROOT / "main/app_shell.c").read_text(encoding="utf-8-sig")
begin = shell.index("static nuwax_app_shell_action_t app_shell_hold_action(void) {")
end = shell.index("\nnuwax_app_shell_result_t nuwax_app_shell_handle_touch_event", begin)
assert shell[begin:end].strip() in (OUT / "wake_greeting_touch.c").read_text(encoding="utf-8"), "refresh extracted touch function before rerunning"


def extract(name):
    # Find definition (not forward declaration), retaining its exact source.
    import re
    match = re.search(r"^static [^;{}]*\b" + name + r"\([^;{}]*\)\s*\{", source, re.M)
    assert match, name
    start = match.start()
    pos = source.index("{", start)
    depth = 1
    pos += 1
    while depth:
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
        pos += 1
    return source[start:pos]


names = ["recover_normal_conversation_after_playback_abort", "awake_idle_sleep_admitted",
         "maybe_enter_idle_sleep", "ensure_sleep_wake_word_detector",
         "check_conversation_turn_timeout"]
functions = {name: extract(name) for name in names}
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "conversation/awake_session_state.h"
#include "conversation/turn_guard.h"
#include "conversation/playback_terminal_guard.h"
#define CONFIG_NUWAX_SPEAKER_ENABLE 1
#define CONFIG_NUWAX_AUDIO_SOURCE_I2S_MIC 1
#define CONFIG_NUWAX_WAKE_WORD_ENABLE 1
#define CONFIG_NUWAX_OFFLINE_AUDIO_ENABLE 0
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 1
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define NUWAX_CONVERSATION_FIRST_PCM_TIMEOUT_MS 15000U
#define NUWAX_CONVERSATION_PLAYBACK_INGRESS_IDLE_TIMEOUT_MS 60000U
#define NUWAX_PLAYBACK_TIMEOUT_CANCEL_SEND_MS 250U
#define NUWAX_PLAYBACK_TERMINAL_WAIT_TIMEOUT_MS 15000U
#define NUWAX_PLAYBACK_FAULT_CANCEL_ACK_TIMEOUT_MS 3000U
enum { NUWAX_APP_STATE_SLEEPING, NUWAX_APP_STATE_IDLE, NUWAX_APP_STATE_LISTENING };
typedef int nuwax_ws_client_t;
typedef int esp_err_t;
static nuwax_awake_session_state_t s_awake_session;
static nuwax_turn_guard_t s_turn_guard;
static nuwax_playback_terminal_guard_t s_playback_terminal_guard;
static struct { bool pending, capture_active; } s_prelisten;
static int s_listen_resume, s_notification_transaction;
static atomic_bool s_reprovision_quiesce_requested;
static bool s_post_playback_resume_pending;
static uint32_t s_cancel_turn_request_seq, s_speaker_fault_recovery_epoch, s_speaker_fault_recovery_generation;
static void *s_wake_word_events;
static uint32_t now;
static int state, capture_starts, wake_rearms, cleared, status_updates, close_requests;
static uint32_t monotonic_ms(void) { return now; }
static int64_t esp_timer_get_time(void) { return (int64_t)now * 1000; }
static int conversation_state_current(void) { return state; }
static bool local_power_transition_active(void) { return false; }
static bool exclusive_audio_mode_blocks_normal_audio(void) { return false; }
static bool nuwax_app_shell_blocks_auto_followup(void) { return false; }
static bool sleep_transaction_blocks_new_audio(void) { return false; }
static bool meeting_or_monitoring_mode_blocks_sleep(void) { return false; }
static bool ota_blocks_wake_detector(void) { return false; }
static bool nuwax_notification_blocks_conversation(void *p) { (void)p;return false; }
static unsigned uxQueueMessagesWaiting(void *p) { (void)p;return 0; }
static bool nuwax_speaker_playback_has_pending(void) { return false; }
static bool nuwax_listen_resume_state_holds_idle_expiry_boundary(void *p) { (void)p;return false; }
static void set_state(int next) { state = next; }
static bool nuwax_prelisten_state_mode_is(void *p,const char *m) { (void)p;(void)m;return false; }
static bool nuwax_capture_phase_cancel_recovery_needs_resume(bool a,bool b,bool c) { return a||b||c; }
static void reset_awake_wait_budget_paused(const char *r) { (void)r;nuwax_awake_session_state_reset_paused(&s_awake_session); }
static uint32_t awake_session_listen_window_ms(uint32_t t) { return nuwax_awake_session_state_remaining_ms(&s_awake_session,t); }
static void start_prelisten_window(const char *r,uint32_t w,uint32_t g) { (void)r;(void)w;(void)g;capture_starts++;s_prelisten.pending=true;state=NUWAX_APP_STATE_LISTENING; }
static void return_to_listening_after_cancel(const char *r) { start_prelisten_window(r,60000,0); }
static void enter_sleeping_conversation_state(const char *r) { (void)r;state=NUWAX_APP_STATE_SLEEPING;nuwax_awake_session_state_clear(&s_awake_session);wake_rearms++; }
static void request_sleep_wake_word_detector(const char *r) { (void)r;wake_rearms++; }
static bool recover_post_end_cloud_response_timeout(void *p,uint32_t t) { (void)p;(void)t;return false; }
static bool finalize_playback_terminal_if_ready(const char *r) { (void)r;return false; }
static bool begin_playback_terminal_local_recovery(const char *r) { (void)r;return false; }
static bool nuwax_speaker_playback_expire_metadata_only_session(const char *a,const char *b,uint32_t t) { (void)a;(void)b;(void)t;return false; }
static void clear_last_listen_resume(void) {}
static void wake_greeting_metrics_cancel(const char *r) { (void)r; }
static uint32_t nuwax_ws_connection_epoch(void *p) { (void)p;return 1; }
static uint32_t update_normal_downlink_admission(bool b) { (void)b;return 1; }
static int nuwax_session_send_cancel_turn_until_if_epoch(void *p,const char *a,const char *b,const char *c,int64_t t,uint32_t e) { (void)p;(void)a;(void)b;(void)c;(void)t;(void)e;return 0; }
static bool nuwax_ws_request_close_if_epoch(void *p,uint32_t e) { (void)p;(void)e;close_requests++;return true; }
static bool send_playback_fault_cancel(const char *r) { (void)r;return true; }
static bool set_active_turn_phase(int p,uint32_t t) { (void)p;(void)t;return true; }
static bool nuwax_speaker_playback_request_cancel_generation(uint32_t g,const char *r,uint32_t t) { (void)g;(void)r;(void)t;return true; }
static void clear_active_turn_guard(const char *r) { (void)r;cleared++;nuwax_turn_guard_clear(&s_turn_guard); }
static void nuwax_speaker_playback_request_fade_cancel(const char *r,uint32_t t) { (void)r;(void)t; }
static void set_conversation_status_guarded(const char *a,const char *b,const char *c,bool d) { (void)a;(void)b;(void)c;(void)d;status_updates++; }
static size_t strlcpy(char *d,const char *s,size_t n) { size_t l=strlen(s);if(n){size_t c=l<n-1?l:n-1;memcpy(d,s,c);d[c]=0;}return l; }
'''
suffix = r'''
static void reproduce(int initial_state,uint32_t timeout,int phase) {
    memset(&s_prelisten,0,sizeof(s_prelisten));
    nuwax_awake_session_state_clear(&s_awake_session);
    nuwax_turn_guard_clear(&s_turn_guard);
    nuwax_playback_terminal_guard_clear(&s_playback_terminal_guard);
    state=initial_state;now=100;capture_starts=wake_rearms=cleared=status_updates=close_requests=0;
    nuwax_awake_session_state_begin(&s_awake_session,60000);
    assert(nuwax_turn_guard_begin(&s_turn_guard,s_awake_session.generation,"wake_1",NULL,now,timeout));
    s_turn_guard.phase=phase;
    now+=timeout;
    int ws=0;
    check_conversation_turn_timeout(&ws);
    assert(!s_turn_guard.active && cleared==1 && status_updates==1);
    for (unsigned i=0;i<100;++i) {
        now+=60000;
        maybe_enter_idle_sleep();
        ensure_sleep_wake_word_detector();
    }
    if(initial_state==NUWAX_APP_STATE_SLEEPING) {
        assert(state==NUWAX_APP_STATE_SLEEPING);
        assert(!capture_starts && !wake_rearms && !close_requests);
        assert(s_awake_session.active && !s_awake_session.waiting);
        assert(!s_prelisten.pending && !s_prelisten.capture_active);
        printf("REPRODUCED phase=%d timeout=%lu: after 100 idle minutes state=SLEEPING awake=active/paused capture=0 wake_rearm=0 websocket_close=0\n",phase,(unsigned long)timeout);
    } else {
        assert(state==NUWAX_APP_STATE_LISTENING && capture_starts>0);
        puts("CONTROL PASSED: the same timeout starting in an awake state schedules listening");
    }
}
int main(void) {
    reproduce(NUWAX_APP_STATE_SLEEPING,15000,NUWAX_TURN_PHASE_WAIT_TEXT);
    reproduce(NUWAX_APP_STATE_SLEEPING,30000,NUWAX_TURN_PHASE_WAIT_TTS);
    reproduce(NUWAX_APP_STATE_IDLE,15000,NUWAX_TURN_PHASE_WAIT_TEXT);
    return 0;
}
'''
generated = OUT / "wake_timeout_recovery.c"
generated.write_text(prefix + "\n\n".join(functions.values()) + suffix, encoding="utf-8")
logs = []
for name, inputs in [
    ("wake_greeting_touch", [OUT / "wake_greeting_touch.c"]),
    ("wake_timeout_recovery", [generated, ROOT / "main/conversation/awake_session_state.c",
                               ROOT / "main/conversation/turn_guard.c",
                               ROOT / "main/conversation/playback_terminal_guard.c"]),
]:
    exe = OUT / (name + ".exe")
    command = ["gcc", "-std=c11", "-Wall", "-Wextra", "-Wno-unused-variable",
               "-I", str(ROOT / "main"), *map(str, inputs), "-o", str(exe)]
    built = subprocess.run(command, text=True, capture_output=True)
    logs.append("BUILD " + name + "\n" + built.stdout + built.stderr)
    if built.returncode:
        (OUT / "reproduction-output.txt").write_text("\n".join(logs), encoding="utf-8")
        raise SystemExit(built.returncode)
    result = subprocess.run([str(exe)], text=True, capture_output=True)
    logs.append(result.stdout + result.stderr)
    print(result.stdout, end="")
    assert result.returncode == 0, result.stderr

(OUT / "reproduction-output.txt").write_text("\n".join(logs), encoding="utf-8")
hashes = {str(p.relative_to(ROOT)).replace("\\", "/"): hashlib.sha256(p.read_bytes()).hexdigest()
          for p in [MAIN, ROOT / "main/app_shell.c", ROOT / "main/conversation/awake_session_state.c",
                    ROOT / "main/conversation/turn_guard.c", ROOT / "main/conversation/playback_terminal_guard.c"]}
function_hashes = {name: hashlib.sha256(body.encode()).hexdigest() for name, body in functions.items()}
function_hashes["app_shell_hold_action"] = hashlib.sha256(shell[begin:end].encode()).hexdigest()
config_path = Path("C:/tmp/nuwax-sd-formal-build-20260921/config/sdkconfig.h")
config = config_path.read_text()
enabled = {name: ("#define CONFIG_" + name + " 1") in config for name in [
    "NUWAX_BARGE_IN_ENABLE", "NUWAX_WAKE_WORD_INTERRUPT_ENABLE", "NUWAX_LIVE_AEC_ENABLE",
    "NUWAX_TOUCH_INTERRUPT_ACTION_ENABLE", "NUWAX_TOUCH_WAKE_ACTION_ENABLE"]}
assert enabled["NUWAX_TOUCH_INTERRUPT_ACTION_ENABLE"] and not enabled["NUWAX_TOUCH_WAKE_ACTION_ENABLE"]
binary = ROOT / "artifacts/release-7.0.113/ota/NuwaxAIPartner-7.0.113-ota.bin"
(OUT / "source-evidence.json").write_text(json.dumps({
    "sourceHashes": hashes, "extractedFunctionSha256Utf8LF": function_hashes,
    "firmwareImageSha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
    "buildConfig": str(config_path), "buildConfigSha256": hashlib.sha256(config_path.read_bytes()).hexdigest(),
    "enabledProductFlags": enabled,
    "generatedC": str(generated), "command": "python tmp/product-launch-review-20261009/audio-ux/reproduce.py",
    "scope": "Real extracted production recovery, timeout and idle/wake supervision functions; mocked transport/speaker hardware; actual awake/turn/terminal state modules. Cloud/WebSocket assumed remains healthy while greeting output is missing. No hardware result claimed."
}, ensure_ascii=False, indent=2), encoding="utf-8")
