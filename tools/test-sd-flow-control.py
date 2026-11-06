"""Production TLS progress deadlines, TCP events and real capture admission.

Uses virtual network/RTOS time plus the production encoder/send loop. This
proves state/lifetime/bounds, not hardware Wi-Fi throughput or an eight-hour run.
"""
from pathlib import Path
import ctypes, json, os, subprocess

root=Path(__file__).resolve().parents[1]
out=root/'build-host-checks/sd-flow-control'
out.mkdir(parents=True,exist_ok=True)
if os.name=='nt':ctypes.windll.kernel32.SetErrorMode(0x8003)
source=(root/'main/ws_transport.c').read_text(encoding='utf-8-sig')
spool=(root/'main/offline_audio_spool.c').read_text(encoding='utf-8-sig')
def between(a,b):
    start=source.index(a);return source[start:source.index(b,start+len(a))]
def function(text,marker):
    start=text.index(marker);at=text.index('{',start)+1;depth=1
    while depth:
        depth+=(text[at]=='{')-(text[at]=='}');at+=1
    return text[start:at]+'\n'
code=between('static esp_err_t send_all_until(','static esp_err_t send_all(')
code+=between('static esp_err_t ensure_lifecycle_lock(void) {','static void lifecycle_store(')
code+=between('void nuwax_ws_init(','/* Caller owns tx_frame_lock')
code+=between('esp_err_t nuwax_ws_connect(','esp_err_t nuwax_ws_send_text(')
code+=between('esp_err_t nuwax_ws_send_archive_chunk_if_epoch(','esp_err_t nuwax_ws_send_archive_text_if_epoch(')
(out/'ws_frame_production_host.inc').write_text(code,encoding='utf-8')
fixture=(root/'main/ws_frame_host_check.c').read_text(encoding='utf-8-sig')
fixture=fixture.replace('static unsigned retry_budget;',r'''
static unsigned retry_budget;
static int64_t flow_ready_us,flow_last_bio_us;
static bool flow_bio_progress;
static void flow_request_if_due(void);
''')
fixture=fixture.replace('    if (f->tls_stalls) {',r'''
    flow_request_if_due();
    if (flow_ready_us && esp_timer_get_time()<flow_ready_us) {
        f->pending_tls_input=p;f->pending_tls_bytes=len;
        if(flow_bio_progress && active_tls_metrics && esp_timer_get_time()-flow_last_bio_us>=50000) {
            active_tls_metrics->tls_bio_bytes+=128;flow_last_bio_us=esp_timer_get_time();
        }
        errno=EAGAIN;return f->tls_want;
    }
    if (f->tls_stalls) {''',1)
prefix=r'''
#include "sd_tcp_recovery.h"
#include "sd_capture_admission.h"
#define CONFIG_NUWAX_SD_SESSION_ENABLE 1
#define CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE 1
#define REPLAY_ACK_BIT 1U
#define REPLAY_CHUNK_ACK_BIT 2U
typedef unsigned nuwax_offline_audio_mode_t;
static struct {SemaphoreHandle_t admission_lock;int events;} s;
static sd_capture_admission_t s_capture_admission;
static atomic_bool s_formal_replay_running,s_formal_replay_cancel_requested;
static bool runtime_initialized(void){return true;}
static bool nuwax_offline_audio_replay_running(void){return s_formal_replay_running;}
static unsigned xEventGroupSetBits(int e,unsigned bits){assert(e==1&&bits==3);return bits;}
static bool flow_events,flow_capacity,flow_events_timeout;
static uint32_t flow_event_sequence,flow_queued;
static unsigned flow_waits;
static int64_t flow_release_at;
bool nuwax_sd_tcp_snapshot(uintptr_t owner,uint32_t epoch,nuwax_sd_tcp_snapshot_t *state){
    (void)owner;assert(epoch==1);
    if(!flow_events)return false;
    *state=(nuwax_sd_tcp_snapshot_t){.valid=flow_capacity,.sequence=flow_event_sequence,
        .queued_bytes=flow_queued,.send_free=131072-flow_queued};return true;
}
int nuwax_sd_tcp_wait_progress(uintptr_t owner,uint32_t epoch,uint32_t sequence,uint32_t ms){
    (void)owner;(void)sequence;assert(epoch==1);if(!flow_events)return -1;
    assert(!((mutex_t*)s_lifecycle_lock)->owner);++flow_waits;
    virtual_us+=(int64_t)(flow_events_timeout?ms:ms<2?ms:2)*1000;
    if(flow_release_at && virtual_us>=flow_release_at){flow_queued=0;flow_release_at=0;}
    if(!flow_events_timeout){++flow_event_sequence;return 1;}return 0;
}
bool nuwax_sd_socket_wait_begin(nuwax_ws_send_metrics_t *m){(void)m;return false;}
void nuwax_sd_socket_wait_end(void){assert(0);}
'''
prefix+=function(spool,'esp_err_t nuwax_sd_session_prepare_start(')
prefix+=function(spool,'void nuwax_sd_session_release_start(')
prefix+=function(source,'bool nuwax_ws_is_connected(')
prefix+=r'''
static bool request_capture;
static unsigned request_mode;
static int64_t request_at;
static void flow_request_if_due(void){
    if(request_capture && esp_timer_get_time()>=request_at){
        request_capture=false;
        assert(nuwax_sd_session_prepare_start(request_mode,esp_timer_get_time()+10000000)==ESP_ERR_NOT_FINISHED);
        assert(s_formal_replay_cancel_requested);
    }
}
static bool flow_cancelled(void){return s_formal_replay_cancel_requested;}
#define ESP_PLATFORM 1
'''
prefix+=function(source,'static esp_err_t archive_wait_capacity(')
fixture=fixture.replace('#include "ws_frame_production_host.inc"',prefix+'\n#include "ws_frame_production_host.inc"',1)
fixture=fixture.replace('int main(void) {','int previous_main(void) {',1)
fixture+=r'''
int main(void){
    contended=CreateEvent(NULL,TRUE,FALSE,NULL);assert(contended);
    assert(ensure_lifecycle_lock()==ESP_OK);s.admission_lock=xSemaphoreCreateMutex();s.events=1;
    for(size_t i=0;i<sizeof(audio);i++)audio[i]=(uint8_t)(i*37);
    for(unsigned test=0;test<8;test++){
        virtual_clock=true;virtual_us=1000000;flow_last_bio_us=virtual_us;flow_event_sequence=1;
        fixture_t f;init_fixture(&f);f.ws.use_tls=true;
        f.tls_want=test==6?ESP_TLS_ERR_SSL_WANT_READ:ESP_TLS_ERR_SSL_WANT_WRITE;
        flow_bio_progress=test!=2;
        flow_ready_us=virtual_us+(test==0?2000000LL:test==1||test==2||test==6?3000000LL:20000000LL);
        flow_events=test!=0;flow_capacity=false;flow_events_timeout=test==7;flow_waits=0;
        const bool handoff=test==4||test==5;request_mode=test==5;
        request_capture=handoff;request_at=virtual_us+100000;
        s_formal_replay_running=handoff;s_formal_replay_cancel_requested=false;
        nuwax_ws_send_metrics_t m={.shared_tls_archive=true,.tls_plaintext_limit=8192,
            .cancelled=flow_cancelled};
        uint8_t scratch[8142];int64_t start=virtual_us;
        const int err=nuwax_ws_send_archive_buffered_if_epoch(&f.ws,audio,sizeof(audio),250,1,scratch,sizeof(scratch),&m);
        const int64_t elapsed=virtual_us-start;
        const bool success=test==0||test==1||test==6;
        assert((err==ESP_OK)==success && f.ws.connected==success);
        if(success){
            assert(elapsed>=(test==0?2000000:3000000));
            assert(check_frame(&f,0,2,audio,sizeof(audio))==f.wire_len&&!f.pending_tls_input);
        }else if(test==2)assert(elapsed>=2500000&&elapsed<2530000);
        else if(handoff)assert(elapsed>=2600000&&elapsed<2630000);
        else assert(elapsed>=8250000&&elapsed<8280000);
        if(test==6)assert(!flow_waits&&m.tls_read_waits>0&&!m.tls_write_waits);
        if(flow_events&&test!=6)assert(m.tcp_event_waits>0);
        printf("PASS production TLS case=%u elapsed_ms=%lld success=%u events=%u BIO_bytes=%u\n",
            test,(long long)(elapsed/1000),success,flow_waits,m.tls_bio_bytes);
        flow_ready_us=0;flow_events=false;s_formal_replay_running=false;
        nuwax_sd_session_release_start(request_mode);s_formal_replay_cancel_requested=false;
        free_fixture(&f);
    }
    /* Successful real admission -> cancel -> current frame drain -> new mode
     * admitted on SAME WS epoch, for both meeting and monitoring. */
    for(unsigned mode=0;mode<2;mode++){
        virtual_us=1000000;flow_last_bio_us=virtual_us;flow_events=true;flow_capacity=false;
        flow_events_timeout=false;flow_ready_us=virtual_us+350000;flow_bio_progress=true;
        request_mode=mode;request_at=virtual_us+100000;request_capture=true;
        s_formal_replay_running=true;s_formal_replay_cancel_requested=false;
        fixture_t f;init_fixture(&f);f.ws.use_tls=true;f.tls_want=ESP_TLS_ERR_SSL_WANT_WRITE;
        nuwax_ws_send_metrics_t m={.shared_tls_archive=true,.cancelled=flow_cancelled};uint8_t scratch[8142];
        assert(nuwax_ws_send_archive_buffered_if_epoch(&f.ws,audio,sizeof(audio),250,1,scratch,sizeof(scratch),&m)==ESP_OK);
        assert(s_formal_replay_cancel_requested&&f.ws.connected&&f.ws.connection_epoch==1);
        assert(check_frame(&f,0,2,audio,sizeof(audio))==f.wire_len);
        s_formal_replay_running=false;
        assert(nuwax_sd_session_prepare_start(mode,virtual_us+5000000)==ESP_OK);
        assert(sd_capture_admission_held(&s_capture_admission,(uint32_t)(virtual_us/1000)));
        flow_ready_us=0;size_t before=f.wire_len;const uint8_t reply[]="mode_ack";
        assert(send_frame(&f.ws,1,reply,sizeof(reply)-1)==ESP_OK);
        assert(check_frame(&f,before,1,reply,sizeof(reply)-1)==f.wire_len);
        nuwax_sd_session_release_start(mode);s_formal_replay_cancel_requested=false;
        assert(!sd_capture_admission_held(&s_capture_admission,(uint32_t)(virtual_us/1000)));
        free_fixture(&f);
    }
    puts("PASS actual capture admission + TLS drain + new meeting/monitoring admission + control ACK on retained epoch");
    /* Capacity wait owns NO frame/IO/lifecycle lock and writes nothing until
     * admission. A persistent full queue defers without disconnecting TLS. */
    for(unsigned release=0;release<2;release++){
        virtual_us=1000000;flow_events=flow_capacity=true;flow_event_sequence=1;flow_queued=131072;
        flow_release_at=release?virtual_us+10000:0;flow_ready_us=0;
        fixture_t f;init_fixture(&f);f.ws.use_tls=true;
        nuwax_ws_send_metrics_t m={.shared_tls_archive=true,.tcp_queue_ceiling_bytes=131072};uint8_t scratch[8142];
        int err=nuwax_ws_send_archive_buffered_if_epoch(&f.ws,audio,sizeof(audio),250,1,scratch,sizeof(scratch),&m);
        assert(err==(release?ESP_OK:ESP_ERR_TIMEOUT));assert(f.ws.connected&&m.tcp_admission_waits);
        if(!release)assert(!m.write_calls&&!f.wire_len);
        else assert(check_frame(&f,0,2,audio,sizeof(audio))==f.wire_len);
        free_fixture(&f);
    }
    /* Observer absence cannot bypass the memory budget. A smaller configured
     * socket may still use the SDK path when worst-case growth fits. */
    flow_events=false;flow_capacity=false;
    fixture_t fallback;init_fixture(&fallback);fallback.ws.use_tls=true;
    nuwax_ws_send_metrics_t fallback_m={.shared_tls_archive=true,.tcp_queue_ceiling_bytes=131072};
    uint8_t fallback_scratch[8142];
    assert(nuwax_ws_send_archive_buffered_if_epoch(&fallback.ws,audio,sizeof(audio),250,1,
        fallback_scratch,sizeof(fallback_scratch),&fallback_m)==ESP_ERR_NO_MEM);
    assert(fallback.ws.connected&&!fallback.wire_len&&!fallback_m.write_calls);
    fallback_m.tcp_queue_ceiling_bytes=46080;
    assert(nuwax_ws_send_archive_buffered_if_epoch(&fallback.ws,audio,sizeof(audio),250,1,
        fallback_scratch,sizeof(fallback_scratch),&fallback_m)==ESP_OK);
    assert(check_frame(&fallback,0,2,audio,sizeof(audio))==fallback.wire_len);free_fixture(&fallback);
    virtual_clock=false;destroy_mutex(s.admission_lock);destroy_mutex(s_lifecycle_lock);CloseHandle(contended);
    puts("PASS production capacity admission yields locks, defers safely and resumes same frame when ACK releases queue");
    return 0;
}
'''
(out/'production-flow.c').write_text(fixture,encoding='utf-8')
def run(name,args):
    done=subprocess.run(args,cwd=root,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=120)
    (out/(name+'.log')).write_text(done.stdout,encoding='utf-8')
    print(done.stdout,end='');assert done.returncode==0,(name,done.returncode)
compile_args=['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function',
              '-I'+str(root/'main/audio_modes/meeting/host_stubs'),'-I'+str(root/'main'),'-I'+str(out)]
run('compile-flow',compile_args+[str(out/'production-flow.c'),'-o',str(out/'production-flow.exe')])
run('production-flow',[str(out/'production-flow.exe')])
run('compile-tcp',['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function',
    '-I'+str(root/'main'),str(root/'main/sd_tcp_flow_host_check.c'),'-o',str(out/'tcp-flow.exe')])
run('tcp-flow',[str(out/'tcp-flow.exe')])
run('compile-policy',['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror',
    '-I'+str(root/'main'),str(root/'main/ws_archive_flow_policy_host_check.c'),'-o',str(out/'policy.exe')])
run('policy',[str(out/'policy.exe')])
# R211 changes storage fault classification deliberately, exercised by
# test-sd-integrity.py. Keep protocol, ACK transaction and capture boundaries.
baseline=Path('C:/tmp/nuwax-state-sync-release-20261030-r209')
preserved=['main/sd_audio_protocol.h','main/sd_audio_scene_protocol.h','main/sd_audio_session_storage.inc',
    'main/sd_audio_catalog.c','main/sd_audio_catalog_io.inc',
    'main/audio_frame.c','main/ws_mask.h','main/tls_archive_crypto.c','main/sd_socket_wait.c',
    'main/nuwax_ai_partner_main.c','main/sd_audio_control_sync.h',
    'main/audio_modes/meeting/meeting_mode.c','main/audio_modes/monitoring/monitoring_mode.c']
if baseline.exists():
    for rel in preserved:
        if rel == 'main/nuwax_ai_partner_main.c':
            text=(root/rel).read_text(encoding='utf-8-sig')
            notification='''    if (state == NUWAX_MEETING_STATE_ERROR) {
        /* ERROR may become DISABLED before the single state publisher runs.
         * Latch the terminal result separately; LVGL consumes this before
         * draining subsequent touch actions, with no transient ERROR frame. */
        nuwax_app_shell_notify_action_rejected(NUWAX_APP_SHELL_ACTION_SET_MEETING_MODE);
    }
'''
            assert text.count(notification)==1
            assert text.replace(notification,'')==(baseline/rel).read_text(encoding='utf-8-sig'),rel
        elif rel=='main/sd_audio_catalog.c':
            from sd_source_contracts import catalog_without_directory_guard
            assert catalog_without_directory_guard((root/rel).read_text(encoding='utf-8-sig'))==(baseline/rel).read_text(encoding='utf-8-sig'),rel
        else:
            assert (root/rel).read_bytes()==(baseline/rel).read_bytes(),rel
    previous=(baseline/'main/ws_transport.c').read_text(encoding='utf-8-sig')
    assert function(source,'static esp_err_t send_frame_unlocked_buffered(')==function(previous,'static esp_err_t send_frame_unlocked_buffered(')
    from sd_source_contracts import file_sender_without_r216
    assert file_sender_without_r216(function(spool,'static esp_err_t replay_one_ready_file('))==function((baseline/'main/offline_audio_spool.c').read_text(encoding='utf-8-sig'),'static esp_err_t replay_one_ready_file(')
    print('PASS R209 exact source fences: capture, state synchronization, catalog, wire format, encoder, TLS crypto and durable file ACK transaction preserved')
(out/'results.json').write_text(json.dumps(dict(productionFlow=True,productionCaptureAdmission=True,
    productionTcpObserver=True,realThreadAckRaces=400,hardwareValidated=False,
    hardwareThroughputKiBs=None),indent=2)+'\n',encoding='utf-8')
