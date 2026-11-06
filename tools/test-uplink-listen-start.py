"""Execute production listen-start ordering and cancellation with transport doubles.

This does not access a device. A 500ms control write advances an independent
20ms capture producer; the production sender prefix must retain all 25 frames.
"""
from pathlib import Path
import ctypes
import re
import subprocess
import tempfile
from c_source_extract import function

if hasattr(ctypes, 'windll'):
    ctypes.windll.kernel32.SetErrorMode(0x8003)

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'main/conversation/audio_uplink.c').read_text(encoding='utf-8-sig')
HEADER = (ROOT / 'main/conversation/audio_uplink.h').read_text(encoding='utf-8-sig')
MAIN = (ROOT/'main/nuwax_ai_partner_main.c').read_text(encoding='utf-8-sig')
enums = '\n'.join(re.findall(r'typedef enum \{[^}]*\} nuwax_audio_uplink_(?:sender_stage|listen_start_state)_t;', HEADER))
names = ['nuwax_audio_uplink_listen_start_committed',
         'audio_uplink_commit_listen_start',
         'nuwax_audio_uplink_schedule_listen_start',
         'audio_uplink_arm_terminal_recovery',
         'nuwax_audio_uplink_preempt_for_local_power',
         'nuwax_audio_uplink_discard_uncommitted',
         'nuwax_audio_uplink_bind_terminal_recovery']
functions = '\n'.join(function(SOURCE, name) for name in names)
prepare = function(MAIN, 'prepare_mic_cloud_upload')
worker = function(SOURCE, 'audio_uplink_send_task')
prefix_end = worker.index('        audio_uplink_sender_stage_store(\n            ctx, NUWAX_AUDIO_UPLINK_SENDER_PREPARE_PACKET,')
# Execute the actual control/admission portion. Binary send internals already
# have separate regressions; here the transport sink verifies its prerequisite.
worker = worker[:prefix_end] + r'''
        assert(nuwax_audio_uplink_listen_start_committed(ctx));
        pcm++;
        audio_uplink_release_packet_slot(ctx, packet.slot);
    }
    ctx->finished = true;
}
'''
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_TIMEOUT 3
#define ESP_ERR_NOT_FINISHED 4
#define ESP_FAIL 5
#define ESP_ERR_INVALID_SIZE 6
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define TAG "test"
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
#define NUWAX_AUDIO_UPLINK_IDLE_FALLBACK_WAIT_MS 500
#define NUWAX_AUDIO_UPLINK_LOCAL_POWER_PREEMPT_WAIT_MS 1500
#define NUWAX_AUDIO_UPLINK_ABORT_WAIT_MS 1000
#define NUWAX_AUDIO_UPLINK_PROFILE_REALTIME 1
#define NUWAX_AUDIO_TERMINAL_RECOVERY_DEFAULT_ATTEMPT_TIMEOUT_MS 1000
typedef struct { uint32_t epoch; bool connected; } nuwax_ws_client_t;
typedef struct {
    void *task, *ready_packets;
    nuwax_ws_client_t *ws;
    uint32_t ws_connection_epoch, next_sequence, queued_batches, batch_count;
    size_t batch_len;
    bool terminal_recovery_bound, frame_seen, end_enqueued, terminal_slot_claimed;
    _Atomic bool task_started, capture_done, finished, send_failed;
    _Atomic int listen_start_state;
    int result;
    int64_t listen_start_deadline_us;
    char terminal_recovery_mode[32], terminal_recovery_audio_session_id[48];
    char terminal_recovery_request_id[48];
    uint32_t terminal_recovery_attempt_timeout_ms;
} nuwax_audio_uplink_t;
typedef struct { unsigned slot; } nuwax_audio_uplink_packet_t;
static nuwax_audio_uplink_t *active;
static int64_t clock_us;
static int start_calls, pcm, recovered, closes, notifications, queued, produced;
static int send_result, send_delay_ms, destroyed;
static bool fail_partial, cancel_during_send, schedule_on_receive, run_on_delay;
static bool capture_busy, recovery_pending;
static unsigned recovery_services, local_prepares;
static void audio_uplink_send_task(void *arg);
static esp_err_t nuwax_audio_uplink_schedule_listen_start(nuwax_audio_uplink_t *, uint32_t);
static esp_err_t audio_uplink_arm_terminal_recovery(nuwax_audio_uplink_t *, const uint8_t *, size_t, bool);
static bool audio_uplink_flag_load(const _Atomic bool *p) { return atomic_load(p); }
static void audio_uplink_flag_store(_Atomic bool *p, bool v) { atomic_store(p,v); }
static int64_t esp_timer_get_time(void) {return clock_us;}
static bool nuwax_ws_is_connected(nuwax_ws_client_t *w) {return w->connected;}
static uint32_t nuwax_ws_connection_epoch(nuwax_ws_client_t *w) {return w->epoch;}
static const char *esp_err_to_name(int e) {(void)e;return "error";}
static void audio_uplink_sender_stage_store(nuwax_audio_uplink_t *c,int s,unsigned q,size_t z) {(void)c;(void)s;(void)q;(void)z;}
static void audio_uplink_notify_sender(nuwax_audio_uplink_t *c) {(void)c;notifications++;}
static void audio_uplink_result_store_first_error(nuwax_audio_uplink_t *c,int e) {if(!c->result)c->result=e;}
static void audio_uplink_release_packet_slot(nuwax_audio_uplink_t *c,unsigned s) {(void)c;(void)s;}
static int xQueueReceive(void *q,void *packet,int wait) {
    (void)q;(void)packet;(void)wait;
    if(schedule_on_receive) {
        schedule_on_receive=false;
        assert(nuwax_audio_uplink_schedule_listen_start(active,500)==ESP_OK);
        queued=1;active->capture_done=true;
    }
    if(queued>0){queued--;return pdTRUE;}return 0;
}
static unsigned uxQueueMessagesWaiting(void *q) {(void)q;return (unsigned)queued;}
static void ulTaskNotifyTake(int clear,unsigned ms) {(void)clear;(void)ms;active->capture_done=true;}
static void vTaskDelay(unsigned ms) {
    clock_us+=(int64_t)ms*1000;
    if(run_on_delay){run_on_delay=false;audio_uplink_send_task(active);}
}
static esp_err_t nuwax_session_send_listen_with_audio_session_until_if_epoch(
    nuwax_ws_client_t *w,const char *state,const char *mode,const char *session,
    const char *request,int64_t deadline,uint32_t epoch) {
    assert(w==active->ws && epoch==42 && !strcmp(state,"start"));
    assert(!strcmp(mode,"awake_session") && !strcmp(session,"session_42"));
    assert(request && !strcmp(request,"request_42"));
    assert(deadline==501000 && pcm==0);
    start_calls++;
    for(int ms=0;ms<send_delay_ms;ms+=20){
        clock_us+=20000;queued++;produced++;assert(queued<=64);
    }
    if(cancel_during_send) active->send_failed=true;
    if(fail_partial){w->connected=false;closes++;}
    active->capture_done=true;
    return send_result;
}
static esp_err_t nuwax_audio_terminal_recovery_arm(
    nuwax_ws_client_t *w,uint32_t epoch,const uint8_t *p,size_t n,bool end,
    const char *mode,const char *session,const char *request,uint32_t timeout) {
    (void)p;(void)n;(void)end;(void)mode;(void)session;(void)request;(void)timeout;
    assert(w==active->ws && epoch==42);recovered++;return ESP_OK;
}
static bool nuwax_audio_uplink_end_committed(nuwax_audio_uplink_t *c) {(void)c;return false;}
static esp_err_t audio_uplink_arm_synthesized_terminal_recovery(nuwax_audio_uplink_t *c,uint32_t seq) {
    (void)seq;return audio_uplink_arm_terminal_recovery(c,NULL,0,false);
}
static bool audio_uplink_request_integrity_close(nuwax_audio_uplink_t *c,const char *reason) {(void)c;(void)reason;closes++;return true;}
static void nuwax_audio_uplink_destroy(nuwax_audio_uplink_t *c) {c->task=NULL;destroyed++;}
static bool nuwax_audio_uplink_sender_busy(void) {return false;}
static bool nuwax_audio_terminal_recovery_pending(void) {return recovery_pending;}
static bool conversation_capture_busy_for_sleep(void) {return capture_busy;}
static int service_audio_terminal_recovery(nuwax_ws_client_t *ws) {
    (void)ws;recovery_services++;recovery_pending=false;return ESP_OK;
}
static int nuwax_audio_uplink_start_with_profile(nuwax_audio_uplink_t *c,nuwax_ws_client_t *ws,int profile) {
    assert(c==active && ws==c->ws && profile==NUWAX_AUDIO_UPLINK_PROFILE_REALTIME);
    local_prepares++;return ESP_OK;
}
static int nuwax_audio_uplink_set_low_latency(nuwax_audio_uplink_t *c,bool enabled) {
    assert(c==active && enabled);return ESP_OK;
}
'''
tests = r'''
static void reset(nuwax_audio_uplink_t *c,nuwax_ws_client_t *w) {
    memset(c,0,sizeof(*c));w->epoch=42;w->connected=true;
    c->task=(void*)1;c->ready_packets=(void*)1;c->ws=w;c->ws_connection_epoch=42;
    c->terminal_recovery_bound=true;
    strcpy(c->terminal_recovery_mode,"awake_session");
    strcpy(c->terminal_recovery_audio_session_id,"session_42");
    strcpy(c->terminal_recovery_request_id,"request_42");
    active=c;clock_us=1000;start_calls=pcm=recovered=closes=notifications=queued=produced=destroyed=0;
    send_result=ESP_OK;send_delay_ms=0;
    fail_partial=cancel_during_send=schedule_on_receive=run_on_delay=false;
    capture_busy=recovery_pending=false;recovery_services=local_prepares=0;
}
int main(void) {
    nuwax_audio_uplink_t c; nuwax_ws_client_t w;
    reset(&c,&w);send_delay_ms=500;
    assert(nuwax_audio_uplink_schedule_listen_start(&c,500)==ESP_OK);
    assert(clock_us==1000 && start_calls==0); /* owner never blocks */
    audio_uplink_send_task(&c);
    assert(start_calls==1 && produced==25 && pcm==25 && c.finished);
    assert(nuwax_audio_uplink_listen_start_committed(&c));
    assert(audio_uplink_arm_terminal_recovery(&c,NULL,0,false)==ESP_OK && recovered==1);

    reset(&c,&w);schedule_on_receive=true;
    audio_uplink_send_task(&c);
    assert(start_calls==1 && pcm==1); /* producer publication race */

    reset(&c,&w);send_result=ESP_ERR_TIMEOUT;
    assert(nuwax_audio_uplink_schedule_listen_start(&c,500)==ESP_OK);
    queued=1;audio_uplink_send_task(&c);
    assert(!pcm && c.send_failed && !nuwax_audio_uplink_listen_start_committed(&c));
    assert(audio_uplink_arm_terminal_recovery(&c,NULL,0,false)==ESP_ERR_INVALID_STATE);
    assert(!closes && !recovered); /* safe unsent start preserves transport */

    reset(&c,&w);send_result=ESP_FAIL;fail_partial=true;
    assert(nuwax_audio_uplink_schedule_listen_start(&c,500)==ESP_OK);
    queued=1;audio_uplink_send_task(&c);
    assert(!pcm && !recovered && closes==1 && !w.connected);

    reset(&c,&w);assert(nuwax_audio_uplink_schedule_listen_start(&c,500)==ESP_OK);
    w.epoch=43;audio_uplink_send_task(&c);
    assert(!start_calls && !pcm && c.send_failed);

    reset(&c,&w);assert(nuwax_audio_uplink_schedule_listen_start(&c,500)==ESP_OK);
    clock_us=501000;audio_uplink_send_task(&c);
    assert(!start_calls && !pcm && c.result==ESP_ERR_TIMEOUT);

    reset(&c,&w);assert(nuwax_audio_uplink_schedule_listen_start(&c,500)==ESP_OK);
    run_on_delay=true;
    assert(nuwax_audio_uplink_preempt_for_local_power(&c)==ESP_OK);
    assert(!start_calls && !pcm && !recovered && !closes && c.finished);

    reset(&c,&w);cancel_during_send=true;
    assert(nuwax_audio_uplink_schedule_listen_start(&c,500)==ESP_OK);
    assert(audio_uplink_commit_listen_start(&c)==ESP_OK);
    assert(c.send_failed && nuwax_audio_uplink_listen_start_committed(&c));
    run_on_delay=true;
    assert(nuwax_audio_uplink_preempt_for_local_power(&c)==ESP_OK);
    assert(recovered==1 && !pcm && !closes); /* committed start still needs END/stop */

    reset(&c,&w);assert(nuwax_audio_uplink_schedule_listen_start(&c,500)==ESP_OK);
    run_on_delay=true;
    assert(nuwax_audio_uplink_discard_uncommitted(&c)==ESP_OK);
    assert(!start_calls && !pcm && !recovered && destroyed==1);

    reset(&c,&w);c.listen_start_state=NUWAX_AUDIO_UPLINK_LISTEN_START_SENDING;
    assert(nuwax_audio_uplink_discard_uncommitted(&c)==ESP_ERR_INVALID_STATE && !destroyed);
    reset(&c,&w);c.frame_seen=true;
    assert(nuwax_audio_uplink_schedule_listen_start(&c,500)==ESP_ERR_INVALID_STATE);
    reset(&c,&w);recovery_pending=capture_busy=true;
    assert(prepare_mic_cloud_upload(&w,&c,"awake_session","session_42","request_42")==ESP_ERR_NOT_FINISHED);
    assert(!recovery_services && !local_prepares && recovery_pending);
    capture_busy=false;
    assert(prepare_mic_cloud_upload(&w,&c,"awake_session","session_42","request_42")==ESP_OK);
    assert(recovery_services==1 && local_prepares==1 && !recovery_pending);
    puts("PASS: ordered start, 500ms/25-frame capture continuity, publication race, unsent/partial/epoch failures, pre/during-send cancellation, terminal obligation and nonblocking active-capture prepare");
    return 0;
}
'''
# Header declaration is non-static, matching the production definition.
prefix = prefix.replace('static esp_err_t nuwax_audio_uplink_schedule_listen_start(',
                        'esp_err_t nuwax_audio_uplink_schedule_listen_start(')
with tempfile.TemporaryDirectory(prefix='uplink-start-') as directory:
    p = Path(directory)/'test.c'
    exe = p.with_suffix('.exe')
    for name, variant, checked_functions, checked_prepare, should_pass in [
        ('production', worker, functions, prepare, True),
        ('missing_post_dequeue_barrier', worker.replace(
            'const esp_err_t packet_start_err = audio_uplink_commit_listen_start(ctx);',
            'const esp_err_t packet_start_err = ESP_OK;'), functions, prepare, False),
        ('lost_commit_on_cancel', worker, functions.replace(
            'result == ESP_OK ? NUWAX_AUDIO_UPLINK_LISTEN_START_COMMITTED',
            'result == ESP_OK && !audio_uplink_flag_load(&ctx->send_failed) ? NUWAX_AUDIO_UPLINK_LISTEN_START_COMMITTED'), prepare, False),
        ('orphan_terminal_on_unsent_start', worker, functions.replace(
            '        !nuwax_audio_uplink_listen_start_committed(ctx) ||\n', ''), prepare, False),
        ('sync_recovery_in_live_capture', worker, functions, prepare.replace(
            'if (!conversation_capture_busy_for_sleep()) {', 'if (true) {'), False),
    ]:
        p.write_text(prefix+enums+checked_functions+variant+checked_prepare+tests,encoding='utf-8')
        build=subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror',
                              '-Wno-unused-function','-Wno-unused-variable',
                              '-Wno-unused-but-set-variable',str(p),'-o',str(exe)],
                             capture_output=True,text=True)
        if build.returncode: raise RuntimeError(build.stdout+build.stderr)
        result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=10)
        if (result.returncode==0)!=should_pass:
            raise RuntimeError(name+result.stdout+result.stderr)
        print(result.stdout.strip() if should_pass else 'PASS: rejected '+name)

start=function(MAIN,'start_mic_cloud_upload')
assert 'nuwax_session_send_listen_with_audio_session_until_if_epoch' not in start
assert 'nuwax_audio_uplink_schedule_listen_start' in start
capture=function(MAIN,'stream_microphone_audio')
assert 'nuwax_audio_uplink_listen_start_committed(send_ctx)' in capture
print('PASS: capture owner admits start without network call and resolves real terminal commit')
