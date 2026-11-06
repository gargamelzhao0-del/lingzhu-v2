"""Execute production startup wait/metadata retirement with timed RX races.

Hardware I/O is stubbed; the decisions, locking and fault publication are
extracted verbatim from speaker_playback.c. Negative mutants prove coverage.
"""
from pathlib import Path
import re
import subprocess

root = Path(__file__).resolve().parents[1]
source = (root / 'main/conversation/speaker_playback.c').read_text(encoding='utf-8-sig')
main_source = (root / 'main/nuwax_ai_partner_main.c').read_text(encoding='utf-8-sig')
out = root / 'build-host-checks/playback-startup'
out.mkdir(parents=True, exist_ok=True)


def function(signature, text=source):
    start = text.index(signature)
    while ';' in text[start:text.index('{', start)]:
        start = text.index(signature, start + len(signature))
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def run(name, code, negative=False):
    c = out / (name + '.c')
    exe = out / (name + '.exe')
    c.write_text(code, encoding='utf-8')
    subprocess.run(['gcc', '-std=c11', '-O0', '-Wall', '-Werror=implicit-function-declaration',
                    '-Wno-unused-function', '-Wno-unused-variable', '-I', str(root / 'main'),
                    str(c), '-o', str(exe)], check=True, capture_output=True)
    result = subprocess.run([str(exe)], capture_output=True, timeout=10)
    assert (result.returncode != 0) == negative, (name, result.stderr.decode(errors='replace'))
    print('PASS:', name, '(mutant rejected)' if negative else '')


common = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "conversation/speaker_jitter_policy.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_OK 0
#define ESP_ERR_TIMEOUT 1
#define ESP_ERR_INVALID_RESPONSE 2
#define pdTRUE 1
#define NUWAX_SPEAKER_PLAYBACK_OWNER_CONVERSATION 1
#define NUWAX_SPEAKER_PLAYBACK_OWNER_NOTIFICATION 2
#define NUWAX_SPEAKER_PLAYBACK_OWNER_NONE 0
#define NUWAX_AUDIO_FLAG_END 2
#define NUWAX_SPEAKER_JITTER_STARTUP_BASE_MS 800U
#define NUWAX_SPEAKER_JITTER_RESUME_MIN_MS 640U
#define NUWAX_SPEAKER_JITTER_RESUME_MAX_MS 960U
#define NUWAX_SPEAKER_JITTER_RESUME_GAP_CUSHION_MS 160U
#define NUWAX_SPEAKER_JITTER_POLL_MS 20U
#define NUWAX_SPEAKER_REBUFFER_STALL_WARN_MS 5000U
#define NUWAX_SPEAKER_REBUFFER_STALL_TIMEOUT_MS 15000U
#define NUWAX_SPEAKER_STREAM_STALL_WARN_MS 12000U
#define NUWAX_SPEAKER_STREAM_STALL_TIMEOUT_MS 60000U
typedef int esp_err_t;
typedef struct {uint32_t connection_epoch,generation,sample_rate;} speaker_playback_packet_t;
typedef struct {uint32_t connection_epoch,generation;} speaker_generation_ref_t;
static struct {
 struct {bool active,end_accepted;uint32_t generation,next_sequence,pcm_frames;uint64_t pcm_bytes;} stream;
 bool stream_protocol_faulted,drop_until_end,playback_active,jitter_rebuffering;
 bool playback_session_active,start_gate_pending,start_gate_authorized;
 bool stop_fault_active,fade_cancel_active,runtime_reset_in_progress,downlink_admission_allowed;
 bool jitter_resume_fade_pending;
 uint32_t submitted_connection_epoch,submitted_generation,playback_connection_epoch,playback_generation;
 uint32_t protocol_fault_pending_generation,protocol_fault_pending_connection_epoch;
 uint32_t protocol_fault_notified_generation,protocol_fault_notified_connection_epoch;
 uint32_t stream_protocol_faults,stream_stall_faults;
 uint32_t jitter_startup_target_ms,jitter_resume_target_ms,jitter_rebuffer_started_ms;
 uint32_t jitter_rebuffer_entry_pcm_frames,jitter_rebuffer_events,ingress_max_gap_ms;
 uint32_t jitter_rebuffer_wait_ms,jitter_rebuffer_max_wait_ms,jitter_tail_wait_events,jitter_tail_wait_ms;
 uint32_t jitter_empty_started_ms,completion_claim_generation,cancel_pending_generation;
 uint32_t output_stop_owner_generation,io_claim_generation,maintenance_claim_generation;
 uint32_t metadata_expected_frames,metadata_expected_bytes,fade_cancel_deadline_ms;
 int active_owner;
 int64_t first_pcm_rx_us,startup_watermark_ready_us,playback_begin_rx_us;
 int64_t start_gate_authorized_us,first_i2s_write_us;
 char fade_cancel_source[48],active_turn_id[48],active_playback_id[48],active_notify_id[48];
} s_speaker_ctx;
static uint64_t now_us;
static uint32_t start_ms,records,samples,notices,cancels,polls,mode;
static bool locked,fade;
static uint32_t monotonic_ms(void){return (uint32_t)(now_us/1000);}
static int64_t esp_timer_get_time(void){return (int64_t)now_us;}
static void speaker_lock(void){assert(!locked);locked=true;}
static void speaker_unlock(void){assert(locked);locked=false;}
static uint32_t speaker_packet_ring_count(void){return records;}
static uint32_t speaker_packet_ring_samples(void){return samples;}
static void speaker_notify_playback_fault(uint32_t e,uint32_t g,int result){
 assert(!locked&&e==2&&g==9&&result!=0);notices++;
}
static bool speaker_restore_probe_duck_if_due(uint32_t t){return false;}
static uint32_t speaker_positive_delay_ticks(uint32_t ms){return ms;}
static bool speaker_fade_cancel_due(uint32_t t,char *s,size_t n,uint32_t *g){
 if(!fade)return false;*g=9;return true;
}
static bool speaker_cancel_now_if_current_epoch(uint32_t e,uint32_t g,const char *reason){
 assert(!locked);
 if(e!=s_speaker_ctx.submitted_connection_epoch||g!=s_speaker_ctx.submitted_generation)return false;
 cancels++;s_speaker_ctx.stream.active=false;s_speaker_ctx.drop_until_end=true;return true;
}
static void speaker_clear_end_seen_locked(void){assert(locked);}
static void speaker_reset_stream_runtime_locked(bool clear){
 assert(locked);memset(&s_speaker_ctx.stream,0,sizeof(s_speaker_ctx.stream));
}
static size_t strlcpy(char *d,const char *s,size_t n){size_t l=strlen(s);if(n){snprintf(d,n,"%s",s);}return l;}
static bool nuwax_speaker_stream_bind_expected(void *s,uint32_t g,uint32_t f,uint32_t b){return true;}
static void ulTaskNotifyTake(int clear,uint32_t ms){
 assert(!locked);assert(++polls<4000);now_us+=(uint64_t)ms*1000;
 uint32_t age=monotonic_ms()-start_ms;
 if(mode==1 && age%1000==0 && age<=4000){records++;samples+=320;s_speaker_ctx.stream.pcm_frames++;}
 if(mode==2 && age>=5000)s_speaker_ctx.stream.end_accepted=true;
 if(mode==3 && age>=5000){records=39;samples=39*320;}
 if(mode==4 && age>=1000)s_speaker_ctx.submitted_generation=10;
 if(mode==5 && age>=1000)s_speaker_ctx.submitted_connection_epoch=3;
 if(mode==6 && age>=1000)fade=true;
 if(mode==7 && age>=1000)s_speaker_ctx.drop_until_end=true;
 if(mode==8 && age>=5000)s_speaker_ctx.stream_protocol_faulted=true;
 if(mode==9 && age>=6000)s_speaker_ctx.stream.end_accepted=true;
 if(mode==10 && age%9000==0){records++;samples+=320;s_speaker_ctx.stream.pcm_frames++;}
 if(mode==11 && age>=10000)s_speaker_ctx.stream.end_accepted=true;
 if(mode==12 && age>=10000){records=39;samples=39*320;}
}
'''

notice_end = source.index('} speaker_fault_notice_t;') + len('} speaker_fault_notice_t;')
notice = source[source.rfind('typedef struct {', 0, notice_end):notice_end]
production = '\n'.join([
    function('static bool speaker_generation_source_matches_locked('), notice,
    function('static speaker_fault_notice_t speaker_claim_protocol_fault_locked('),
    function('static void speaker_publish_protocol_fault('),
    function('static void speaker_schedule_protocol_fault('),
    function('static bool speaker_process_pending_protocol_fault('),
    function('void nuwax_speaker_playback_note_playback_begin('),
    function('static bool speaker_abort_metadata_only_session('),
    function('bool nuwax_speaker_playback_abort_metadata_only_session('),
    function('bool nuwax_speaker_playback_expire_metadata_only_session('),
    function('static bool speaker_wait_for_jitter_watermark('),
])

tests = r'''
static void reset(void){
 memset(&s_speaker_ctx,0,sizeof(s_speaker_ctx));
 now_us=1000000;start_ms=monotonic_ms();records=17;samples=17*320;
 notices=cancels=polls=mode=0;fade=locked=false;
 s_speaker_ctx.submitted_generation=9;s_speaker_ctx.submitted_connection_epoch=2;
 s_speaker_ctx.active_owner=NUWAX_SPEAKER_PLAYBACK_OWNER_CONVERSATION;
 s_speaker_ctx.stream.active=true;s_speaker_ctx.stream.generation=9;
 s_speaker_ctx.stream.pcm_frames=18;s_speaker_ctx.stream.pcm_bytes=18*640;
 s_speaker_ctx.first_pcm_rx_us=now_us;
 s_speaker_ctx.jitter_startup_target_ms=800;s_speaker_ctx.jitter_resume_target_ms=640;
}
static bool wait_start(void){
 speaker_playback_packet_t p={.connection_epoch=2,.generation=9,.sample_rate=16000};
 uint32_t waited=0;return speaker_wait_for_jitter_watermark(&p,false,320,&waited);
}
static void metadata(void){
 reset();records=samples=0;memset(&s_speaker_ctx.stream,0,sizeof(s_speaker_ctx.stream));
 s_speaker_ctx.first_pcm_rx_us=0;s_speaker_ctx.drop_until_end=true;
 s_speaker_ctx.downlink_admission_allowed=true;
 nuwax_speaker_playback_note_playback_begin("turn","pb",0,0);
}
int main(void){
 reset();assert(!wait_start());assert(monotonic_ms()-start_ms==5000&&notices==1);
 assert(cancels==0&&s_speaker_ctx.stream_protocol_faulted);
 assert(speaker_process_pending_protocol_fault()&&cancels==1);
 reset();mode=1;assert(!wait_start());assert(monotonic_ms()-start_ms==5000&&notices==1);
 reset();mode=2;assert(wait_start()&&notices==0); /* END exactly at deadline */
 reset();mode=3;assert(wait_start()&&notices==0); /* watermark exactly at deadline */
 reset();records=39;samples=39*320;assert(wait_start()&&polls==0&&notices==0);
 reset();s_speaker_ctx.stream.end_accepted=true;assert(wait_start()&&polls==0);
 for(unsigned m=4;m<=8;m++){reset();mode=m;assert(!wait_start()&&notices==0);assert(cancels==(m==6));}
 reset();mode=9;s_speaker_ctx.active_owner=2;assert(wait_start()&&notices==0);
 assert(monotonic_ms()-start_ms==6000); /* preserve non-conversation startup policy */
 reset();mode=10;s_speaker_ctx.active_owner=2;assert(!wait_start()&&notices==1);
 assert(monotonic_ms()-start_ms==10000); /* notification trickle cannot renew startup */
 assert(speaker_process_pending_protocol_fault()&&cancels==1);
 for(unsigned m=11;m<=12;m++){reset();mode=m;s_speaker_ctx.active_owner=2;
  assert(wait_start()&&notices==0&&monotonic_ms()-start_ms==10000);}
 reset();mode=11;s_speaker_ctx.active_owner=3;assert(wait_start()&&notices==0);
 reset();now_us=((uint64_t)UINT32_MAX-2000)*1000;start_ms=monotonic_ms();
 s_speaker_ctx.first_pcm_rx_us=now_us;assert(!wait_start()&&notices==1);
 assert(monotonic_ms()-start_ms==5000); /* tick wrap */
 reset();now_us+=4000000;start_ms=monotonic_ms();assert(!wait_start());
 assert(monotonic_ms()-start_ms==1000); /* START authorization time counts */
 reset();speaker_schedule_protocol_fault(2,9,1,"test",true);
 speaker_schedule_protocol_fault(2,9,1,"test",true);assert(notices==1);
 s_speaker_ctx.submitted_generation=10;assert(speaker_process_pending_protocol_fault()&&cancels==0);
 reset();s_speaker_ctx.playback_active=true;s_speaker_ctx.playback_generation=9;
 s_speaker_ctx.playback_connection_epoch=2;s_speaker_ctx.jitter_rebuffering=true;
 records=samples=0;speaker_playback_packet_t p={2,9,16000};
 assert(!speaker_wait_for_jitter_watermark(&p,true,0,0));
 assert(monotonic_ms()-start_ms==15000&&notices==1); /* existing rebuffer budget */
 metadata();now_us+=14999000;
 assert(!nuwax_speaker_playback_expire_metadata_only_session("turn","pb",15000));
 nuwax_speaker_playback_note_playback_begin("turn","pb",0,0); /* duplicate cannot extend */
 now_us+=1000;assert(nuwax_speaker_playback_expire_metadata_only_session("turn","pb",15000));
 assert(!s_speaker_ctx.playback_session_active&&!s_speaker_ctx.downlink_admission_allowed);
 assert(!nuwax_speaker_playback_expire_metadata_only_session("turn","pb",15000));
 metadata();assert(!nuwax_speaker_playback_expire_metadata_only_session("turn","pb",0));
 assert(!nuwax_speaker_playback_abort_metadata_only_session("wrong","pb","test"));
 assert(!nuwax_speaker_playback_abort_metadata_only_session("turn","wrong","test"));
 assert(nuwax_speaker_playback_abort_metadata_only_session("turn","pb","sleep"));
 assert(s_speaker_ctx.downlink_admission_allowed); /* retry API doesn't alter admission */
#define BLOCK(field,value) do {metadata();s_speaker_ctx.field=value;now_us+=20000000; \
 assert(!nuwax_speaker_playback_expire_metadata_only_session("turn","pb",15000)); \
 assert(s_speaker_ctx.playback_session_active&&s_speaker_ctx.downlink_admission_allowed);} while(0)
 BLOCK(start_gate_pending,true);BLOCK(start_gate_authorized,true);BLOCK(playback_active,true);
 BLOCK(stream.active,true);BLOCK(stop_fault_active,true);BLOCK(fade_cancel_active,true);
 BLOCK(io_claim_generation,9);BLOCK(maintenance_claim_generation,9);
 BLOCK(completion_claim_generation,9);BLOCK(cancel_pending_generation,9);
 BLOCK(output_stop_owner_generation,9);BLOCK(runtime_reset_in_progress,true);BLOCK(active_owner,2);
 metadata();records=1;now_us+=20000000;
 assert(!nuwax_speaker_playback_expire_metadata_only_session("turn","pb",15000));
 puts("startup, boundary races, epoch/generation fences, metadata, physical claims: PASS");
}
'''

run('production_startup', common + production + tests)
for name, old, new in [
    ('unbounded_startup', 'startup_expired = generation_current &&', 'startup_expired = false && generation_current &&'),
    ('ignore_ready', 'nuwax_speaker_jitter_startup_expired(playback_started, ready,', 'nuwax_speaker_jitter_startup_expired(playback_started, false,'),
    ('metadata_start_race', '!s_speaker_ctx.start_gate_pending &&', 'true &&'),
    ('metadata_stale_id', 'strcmp(playback, s_speaker_ctx.active_playback_id) == 0', 'true'),
    ('late_rx_fence', 's_speaker_ctx.downlink_admission_allowed = false;', '(void)0;'),
    ('unscoped_startup', 's_speaker_ctx.active_owner == NUWAX_SPEAKER_PLAYBACK_OWNER_CONVERSATION &&', 'true &&'),
    ('notification_unbounded', 'notification_startup_expired = generation_current &&', 'notification_startup_expired = false && generation_current &&'),
]:
    assert old in production
    run(name, common + production.replace(old, new) + tests, True)

# Integration contracts complement executed production functions: the main
# owner must keep physical teardown/recovery, close RX before local sleep, and
# never use the legacy historical generation to cancel metadata-only sessions.
sleep = function('static void remember_local_power_sleep_request(', main_source)
assert sleep.index('update_normal_downlink_admission(false)') < sleep.index('nuwax_speaker_playback_abort_metadata_only_session') < sleep.index('const bool speaker_pending')
assert 'clear_active_turn_guard("power_key_sleep_metadata_only")' in sleep
timeout = function('static void check_conversation_turn_timeout(', main_source)
assert 's_turn_guard.speaker_generation == 0U' in timeout
assert 'nuwax_speaker_playback_expire_metadata_only_session' in timeout
assert 'if (!first_pcm_timeout &&' in timeout
assert 'recover_normal_conversation_after_playback_abort' in timeout
assert '#define NUWAX_CONVERSATION_FIRST_PCM_TIMEOUT_MS 15000U' in main_source
print('PASS: main-owner first-PCM retirement and local-sleep admission ordering')

sleep_stubs = r'''
#define CONFIG_NUWAX_SPEAKER_ENABLE 1
#define CONFIG_NUWAX_WAKE_WORD_ENABLE 0
#define NUWAX_SLEEP_TRANSACTION_SPEAKER_FADE_MS 80
#define NUWAX_SPEAKER_CANCEL_REQUEST_COMPLETED 4
#define NUWAX_SPEAKER_CANCEL_REQUEST_FAILED 5
typedef int nuwax_ws_client_t;
typedef struct {int status;uint32_t connection_epoch,playback_generation;} nuwax_speaker_cancel_request_result_t;
static bool s_sleep_request_pending,s_sleep_request_local_power,s_sleep_request_speaker_cancel_armed;
static bool s_during_playback_handoff_pending,s_during_playback_handoff_playback_started;
static bool s_during_playback_handoff_seamless,s_post_playback_resume_pending;
static uint32_t s_sleep_request_speaker_generation,s_sleep_request_speaker_epoch;
static uint32_t s_speaker_fault_recovery_epoch,s_speaker_fault_recovery_generation;
static struct {char turn_id[48],playback_id[48];} s_turn_guard;
static unsigned admission_closes,turn_clears,async_cancels,followup_cancels;
static bool race_start;
static void remember_sleep_request(const char *r){s_sleep_request_pending=true;}
static bool nuwax_ws_is_connected(nuwax_ws_client_t *ws){return true;}
static uint32_t nuwax_ws_connection_epoch(nuwax_ws_client_t *ws){return 2;}
static uint32_t active_ws_epoch_snapshot(void){return 2;}
static uint32_t sleep_transaction_current_speaker_generation(uint32_t e){return s_speaker_ctx.submitted_generation;}
static uint32_t update_normal_downlink_admission(bool allowed){
 assert(!allowed);admission_closes++;
 /* Simulate START winning the mutex immediately before the sleep fence. */
 if(race_start){s_speaker_ctx.start_gate_pending=true;s_speaker_ctx.stream.active=true;}
 s_speaker_ctx.downlink_admission_allowed=false;return admission_closes;
}
static void clear_active_turn_guard(const char *reason){
 assert(!s_speaker_ctx.playback_session_active&&!s_speaker_ctx.downlink_admission_allowed);
 turn_clears++;memset(&s_turn_guard,0,sizeof(s_turn_guard));
}
static bool nuwax_speaker_playback_request_cancel_generation_ex(uint32_t g,const char *s,uint32_t fade_ms,nuwax_speaker_cancel_request_result_t *r){
 assert(g==s_speaker_ctx.submitted_generation);async_cancels++;
 if(!s_speaker_ctx.stream.active){r->status=0;return false;} /* original stale-gen symptom */
 r->status=1;r->connection_epoch=2;r->playback_generation=g;return true;
}
static void cancel_auto_followup_capture(const char *r){followup_cancels++;}
'''
sleep_tests = r'''
static void setup_sleep(void){
 memset(&s_speaker_ctx,0,sizeof(s_speaker_ctx));records=samples=0;
 s_sleep_request_pending=s_sleep_request_local_power=s_sleep_request_speaker_cancel_armed=false;
 s_sleep_request_speaker_generation=s_sleep_request_speaker_epoch=0;
 s_speaker_fault_recovery_epoch=s_speaker_fault_recovery_generation=0;
 admission_closes=turn_clears=async_cancels=followup_cancels=0;race_start=false;
 now_us=1000000;s_speaker_ctx.submitted_generation=25;s_speaker_ctx.drop_until_end=true;
 s_speaker_ctx.downlink_admission_allowed=true;
 strcpy(s_turn_guard.turn_id,"turn");strcpy(s_turn_guard.playback_id,"pb");
 nuwax_speaker_playback_note_playback_begin("turn","pb",0,0);
}
int main(void){
 nuwax_ws_client_t ws=0;
 setup_sleep();remember_local_power_sleep_request(&ws,"power_key");
 assert(turn_clears==1&&async_cancels==0&&!nuwax_speaker_playback_has_pending());
 assert(s_sleep_request_pending&&s_sleep_request_local_power&&followup_cancels==1);
 remember_local_power_sleep_request(&ws,"power_key");assert(turn_clears==1&&async_cancels==0);
 setup_sleep();race_start=true;s_speaker_ctx.drop_until_end=false;
 remember_local_power_sleep_request(&ws,"power_key");
 assert(turn_clears==0&&async_cancels==1&&s_sleep_request_speaker_cancel_armed);
 assert(s_speaker_fault_recovery_generation==25&&s_speaker_fault_recovery_epoch==2);
 remember_local_power_sleep_request(&ws,"power_key");assert(async_cancels==1);
 s_speaker_ctx.playback_session_active=false;s_speaker_ctx.start_gate_pending=false;
 s_speaker_ctx.stream.active=false;race_start=false;
 remember_local_power_sleep_request(&ws,"power_key");
 assert(!s_sleep_request_speaker_cancel_armed&&s_sleep_request_speaker_generation==0);
 puts("local sleep metadata retirement, START race, one cancel, physical completion: PASS");
}
'''
sleep_code = common + production + function('bool nuwax_speaker_playback_has_pending(') + sleep_stubs + sleep + sleep_tests
run('production_local_sleep', sleep_code)
run('sleep_metadata_leak', sleep_code.replace('if (ids_match && metadata_only && expired)',
                                            'if (false && ids_match && metadata_only && expired)'), True)
