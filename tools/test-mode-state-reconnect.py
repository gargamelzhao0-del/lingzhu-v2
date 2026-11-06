"""Actual controllers/protocols: reconnect announces state without taking audio.

Uses deterministic SDK/storage doubles. No serial port or gateway is touched.
Run run-sd-session-host-checks.py first to generate shared platform fixtures.
"""
from pathlib import Path
import ctypes
import json
import subprocess
import sys
from c_source_extract import function

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build-host-checks/mode-state-reconnect'
OUT.mkdir(parents=True, exist_ok=True)
if hasattr(ctypes, 'windll'):
    ctypes.windll.kernel32.SetErrorMode(0x8003)

for name in ['meeting', 'monitoring']:
    fixture = ROOT / f'build-host-checks/sd_{name}_mode_host_check.c'
    if not fixture.is_file():
        raise RuntimeError('Run tools/run-sd-session-host-checks.py first')
    base = fixture.read_text(encoding='utf-8')
    base = base.replace('#include "sd_audio_mode_host_check.inc"',
        '#define main previous_sd_suite_main\n#include "sd_audio_mode_host_check.inc"\n#undef main')
    base = base.replace('static bool sd_fail_command_ack;',
        'static bool sd_fail_command_ack;\nstatic unsigned fail_reports, report_calls;')
    needle = ' if (sd_fail_command_ack && strstr(payload, "device_command_ack"))'
    assert needle in base
    base = base.replace(needle, '''
 if (strstr(payload,"audio_mode_request") && strstr(payload,"_sync_")) {
     ++report_calls;
     if(fail_reports){--fail_reports;return ESP_FAIL;}
 }
''' + needle)
    base += f'''
#include "cJSON.h"
#define MODE_SYNC nuwax_{name}_mode_sync_current_state
#define MODE_PENDING nuwax_{name}_mode_has_pending
#define MODE_PREEMPT nuwax_{name}_mode_should_preempt_normal
#define MODE_POLL nuwax_{name}_mode_poll_control_events
'''
    base += r'''
static void advance_to_retry(MODE_TYPE *mode){
    uint32_t now=(uint32_t)(s_now_us/1000);
    uint32_t delta=mode->sd_sync.next_send_ms-now;
    assert(delta<=10000U);s_now_us+=(int64_t)delta*1000;
}
static void check_report(MODE_TYPE *mode,bool enabled,const char *session){
    bool found=false;
    for(unsigned i=0;i<s_sent_count;i++){
        cJSON *j=cJSON_Parse(s_sent_messages[i]);assert(j);
        const cJSON *r=cJSON_GetObjectItem(j,"request_id");
        if(cJSON_IsString(r)&&!strcmp(r->valuestring,mode->sd_sync.request_id)){
            assert(!strcmp(cJSON_GetObjectItem(j,"type")->valuestring,"audio_mode_request"));
            assert(!strcmp(cJSON_GetObjectItem(j,"mode")->valuestring,MODE_NAME));
            assert(!strcmp(cJSON_GetObjectItem(j,"state")->valuestring,enabled?"enabled":"disabled"));
            assert(!strcmp(cJSON_GetObjectItem(j,"reason")->valuestring,
                enabled?"offline_audio_recovered":"device_state_sync"));
            assert(!cJSON_GetObjectItem(j,"interrupt_current_turn"));
            assert(cJSON_GetArraySize(j)==(enabled?6:5));
            const cJSON *id=cJSON_GetObjectItem(j,"audio_session_id");
            if(enabled)assert(cJSON_IsString(id)&&!strcmp(id->valuestring,session));
            else assert(!id);
            found=true;
        }
        cJSON_Delete(j);
    }
    assert(found);
}
static void reply(MODE_TYPE *mode,nuwax_ws_client_t *ws,const char *request,bool enabled,bool ok){
    char json[384];
    snprintf(json,sizeof(json),"{\"type\":\"audio_mode_request_ack\",\"request_id\":\"%s\","
        "\"mode\":\"" MODE_NAME "\",\"state\":\"%s\",\"ok\":%s}",
        request,enabled?"enabled":"disabled",ok?"true":"false");
    send_protocol(mode,ws,json);
}
static void fresh(MODE_TYPE *mode,nuwax_ws_client_t *ws,const CALLBACK_TYPE *callbacks){
    card_active=capture_live=false;card_available=card_detected=true;card_used_count=0;
    card_health=NUWAX_OFFLINE_CAPTURE_RECORDING;s_now_us=1000000;
    reset_sent_messages();fail_reports=report_calls=0;
    assert(MODE_INIT(mode,callbacks)==ESP_OK);(void)ws;
}
static void check_inactive(const CALLBACK_TYPE *callbacks){
    MODE_TYPE m;nuwax_ws_client_t ws={.connected=true,.connection_epoch=101};
    fresh(&m,&ws,callbacks);card_available=card_detected=false;
    MODE_SYNC(&m,&ws);assert(m.state==STATE_DISABLED&&m.sd_sync.announced);
    check_report(&m,false,NULL);assert(!capture_live&&!MODE_PREEMPT(&m));
    char request[48];strcpy(request,m.sd_sync.request_id);
    /* Passive confirmations must not enter the normal audio preemption path,
     * including duplicate bursts which fill the existing bounded queues. */
    for(unsigned i=0;i<10;i++)reply(&m,&ws,request,false,true);
    assert(ws.connected&&!MODE_PREEMPT(&m)&&!MODE_PENDING(&m));
    for(unsigned i=0;i<4;i++)MODE_SYNC(&m,&ws);
    assert(m.sd_sync.confirmed&&atomic_load(&m.queued_sync_acks)==0);
    assert(!MODE_PREEMPT(&m)&&!capture_live);
    for(unsigned i=0;i<20;i++){s_now_us+=1000000;MODE_SYNC(&m,&ws);}
    assert(report_calls==1);
    ws.connected=false;MODE_DISCONNECT(&m);ws.connected=true;++ws.connection_epoch;
    reset_sent_messages();MODE_SYNC(&m,&ws);check_report(&m,false,NULL);
    assert(strcmp(request,m.sd_sync.request_id));
    char current[48];strcpy(current,m.sd_sync.request_id);
    reply(&m,&ws,request,false,true);MODE_SYNC(&m,&ws);assert(!m.sd_sync.confirmed);
    reply(&m,&ws,current,true,true);MODE_SYNC(&m,&ws);assert(!m.sd_sync.confirmed);
    reply(&m,&ws,current,false,true);MODE_SYNC(&m,&ws);assert(m.sd_sync.confirmed);
    MODE_DEINIT(&m);
    fresh(&m,&ws,callbacks);MODE_SYNC(&m,&ws);
    strcpy(current,m.sd_sync.request_id);
    unified_open(&m,&ws,"ordinary_app_open",NULL);
    reply(&m,&ws,current,false,true);
    unsigned queued=uxQueueMessagesWaiting(m.events);
    MODE_SYNC(&m,&ws);
    assert(m.state==STATE_DISABLED&&!capture_live&&MODE_PENDING(&m));
    assert(uxQueueMessagesWaiting(m.events)==queued);
    /* Only the existing control-dispatch owner may consume the real open. */
    MODE_POLL(&m,&ws);assert(m.state==STATE_STARTING&&!capture_live);
    MODE_DEINIT(&m);
    puts("PASS " MODE_NAME ": dialogue/no-card disabled state each epoch; exact wire; passive ACKs do not preempt; saturation, stale/wrong replies");
}
static void check_active(const CALLBACK_TYPE *callbacks){
    MODE_TYPE m;nuwax_ws_client_t ws={.connected=false,.connection_epoch=201};
    fresh(&m,&ws,callbacks);
    assert(MODE_UI(&m,NULL,true)==ESP_OK);MODE_SERVICE(&m,NULL);
    assert(m.state==STATE_ACTIVE&&capture_live);
    char session[48];strcpy(session,m.audio_session_id);
    unsigned starts=card_starts,stops=card_stops;
    ws.connected=true;MODE_SYNC(&m,&ws);check_report(&m,true,session);
    char previous[48];strcpy(previous,m.sd_sync.request_id);
    reply(&m,&ws,previous,false,true);MODE_SYNC(&m,&ws);
    assert(!m.sd_sync.confirmed&&m.state==STATE_ACTIVE&&capture_live);
    reply(&m,&ws,previous,true,true);MODE_SYNC(&m,&ws);assert(m.sd_sync.confirmed);
    for(unsigned epoch=202;epoch<207;epoch++){
        ws.connected=false;MODE_DISCONNECT(&m);MODE_SERVICE(&m,NULL);
        assert(m.state==STATE_ACTIVE&&capture_live&&!strcmp(session,m.audio_session_id));
        ws.connected=true;ws.connection_epoch=epoch;reset_sent_messages();
        if(epoch==202){
            /* Incoming enabled state must not suppress our mandatory report. */
            unified_open(&m,&ws,"cloud_already_enabled",NULL);MODE_SERVICE(&m,&ws);
        }else MODE_SYNC(&m,&ws);
        assert(m.sd_sync.announced);check_report(&m,true,session);
        assert(strcmp(previous,m.sd_sync.request_id));strcpy(previous,m.sd_sync.request_id);
        if(epoch==203)reply(&m,&ws,previous,true,false);
        for(unsigned i=0;i<8;i++){s_now_us+=1000000;MODE_SYNC(&m,&ws);}
        assert(m.sd_sync.attempts<=3&&m.state==STATE_ACTIVE&&capture_live);
        assert(card_starts==starts&&card_stops==stops&&!strcmp(session,m.audio_session_id));
    }
    /* More than five seconds and three failures before the first successful
     * TX must not permanently suppress this connection's announcement. */
    ws.connected=false;MODE_DISCONNECT(&m);ws.connected=true;++ws.connection_epoch;
    reset_sent_messages();fail_reports=8;unsigned before=report_calls;
    for(unsigned i=0;i<8;i++){
        MODE_SYNC(&m,&ws);assert(!m.sd_sync.announced&&m.sd_sync.attempts==0);
        unsigned calls=report_calls;MODE_SYNC(&m,&ws);assert(report_calls==calls);
        assert(m.state==STATE_ACTIVE&&capture_live);advance_to_retry(&m);
    }
    MODE_SYNC(&m,&ws);assert(report_calls==before+9&&m.sd_sync.announced&&m.sd_sync.attempts==1);
    check_report(&m,true,session);assert(card_starts==starts&&card_stops==stops);
    for(unsigned i=0;i<2;i++){advance_to_retry(&m);MODE_SYNC(&m,&ws);}
    assert(m.sd_sync.attempts==3);
    /* Explicit App close remains an immediate state command, even with no ID
     * and without a successful report acknowledgement. No restored filter. */
    cloud_stop(&m,&ws,"real_app_close","");MODE_SYNC(&m,&ws);
    assert(m.state==STATE_ACTIVE&&capture_live&&card_stops==stops);
    MODE_SERVICE(&m,&ws);
    assert(m.state==STATE_STOPPING&&!capture_live&&card_stops==stops+1);
    assert(!sent_message_contains("real_app_close","\"ok\":true",NULL));
    card_active=false;MODE_SERVICE(&m,&ws);assert(m.state==STATE_DISABLED);
    assert(sent_message_contains("real_app_close","\"ok\":true",NULL));
    reset_sent_messages();MODE_SYNC(&m,&ws);check_report(&m,false,NULL);
    assert(!capture_live&&card_starts==starts);MODE_DEINIT(&m);
    puts("PASS " MODE_NAME ": offline/repeated reconnect keeps scene and MIC; own report despite cloud enable; lost/rejected ACK; bounded failed-TX retry; explicit stop/seal");
}
int main(void){
    CALLBACK_TYPE callbacks={.can_start=can_start};
    check_inactive(&callbacks);check_active(&callbacks);
    nuwax_sd_control_sync_t p={0};
    nuwax_sd_control_sync_connect(&p,1,UINT32_MAX-500U,0);
    assert(nuwax_sd_control_sync_due(&p,UINT32_MAX-500U));
    nuwax_sd_control_sync_note_send(&p,UINT32_MAX-500U,false);
    assert(!nuwax_sd_control_sync_due(&p,498));assert(nuwax_sd_control_sync_due(&p,499));
    nuwax_sd_control_sync_note_send(&p,499,true);assert(p.announced&&p.attempts==1);
    assert(!nuwax_sd_control_disable_rejection(&p,1,500,"stop","","scene"));
    puts("PASS shared retry wrap and rollback of reconnect-disable filtering");return 0;
}
'''
    generated = OUT / (name + '.c')
    generated.write_text(base, encoding='utf-8')
    exe = OUT / (name + '.exe')
    command = ['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
        '-DCONFIG_NUWAX_SD_SESSION_ENABLE=1', '-DCONFIG_NUWAX_OFFLINE_AUDIO_ENABLE=1',
        '-DCONFIG_NUWAX_SD_LEGACY_CONTROL_COMPAT=1',
        '-I' + str(ROOT / 'main/audio_modes/meeting/host_stubs'), '-I' + str(ROOT / 'main'),
        '-I' + str(ROOT / 'main/conversation'), '-I' + str(ROOT / 'managed_components/espressif__cjson/cJSON'),
        str(generated), str(ROOT / f'main/audio_modes/{name}/{name}_mode.c'),
        str(ROOT / f'main/audio_modes/{name}/{name}_protocol.c'),
        str(ROOT / 'main/audio_modes/meeting/host_stubs/device_error_report.c'),
        str(ROOT / 'managed_components/espressif__cjson/cJSON/cJSON.c')]
    if name == 'meeting':
        command.append(str(ROOT / 'main/audio_modes/meeting/meeting_processor.c'))
    command += ['-o', str(exe)]
    build = subprocess.run(command, text=True, capture_output=True)
    (OUT / (name + '-build.log')).write_text(build.stdout + build.stderr, encoding='utf-8')
    if build.returncode:
        raise RuntimeError(build.stdout + build.stderr)
    result = subprocess.run([str(exe)], text=True, capture_output=True, timeout=30)
    (OUT / (name + '.log')).write_text(result.stdout + result.stderr, encoding='utf-8')
    print(result.stdout + result.stderr, end='')
    if result.returncode:
        raise RuntimeError(f'{name}: {result.returncode}')

# Execute the real root service gate: state reporting must not add a blocking
# control send inside main-owned microphone, speaker or no-card audio loops.
app = (ROOT / 'main/nuwax_ai_partner_main.c').read_text(encoding='utf-8')
body = function(app, 'service_audio_mode_state_sync')
source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#define CONFIG_NUWAX_SD_SESSION_ENABLE 1
#define CONFIG_NUWAX_SPEAKER_ENABLE 1
#define NUWAX_MEETING_STATE_ACTIVE 2
#define NUWAX_MONITORING_STATE_ACTIVE 2
typedef struct {bool connected;} nuwax_ws_client_t;
typedef struct {int state;atomic_bool sd_local;} mode_t;
static mode_t s_meeting_mode,s_monitoring_mode;
static bool ready,capture_busy,sender_busy,speaker_busy;
static unsigned meeting_reports,monitor_reports;
static bool nuwax_ws_is_connected(nuwax_ws_client_t *ws){return ws->connected;}
static bool cloud_service_is_ready(void){return ready;}
static bool conversation_capture_busy_for_sleep(void){return capture_busy;}
static bool nuwax_audio_uplink_sender_busy(void){return sender_busy;}
static bool nuwax_speaker_playback_has_pending(void){return speaker_busy;}
static int nuwax_meeting_mode_state(mode_t *m){return m->state;}
static int nuwax_monitoring_mode_state(mode_t *m){return m->state;}
static void nuwax_meeting_mode_sync_current_state(mode_t *m,nuwax_ws_client_t *ws){(void)m;(void)ws;meeting_reports++;}
static void nuwax_monitoring_mode_sync_current_state(mode_t *m,nuwax_ws_client_t *ws){(void)m;(void)ws;monitor_reports++;}
''' + body + r'''
int main(void){
 nuwax_ws_client_t ws={.connected=true};ready=true;
 service_audio_mode_state_sync(&ws);assert(meeting_reports==1&&monitor_reports==1);
 for(unsigned gate=0;gate<7;gate++){
  meeting_reports=monitor_reports=0;
  ws.connected=ready=true;capture_busy=sender_busy=speaker_busy=false;
  s_meeting_mode.state=s_monitoring_mode.state=0;
  atomic_store(&s_meeting_mode.sd_local,false);atomic_store(&s_monitoring_mode.sd_local,false);
  switch(gate){case 0:ws.connected=false;break;case 1:ready=false;break;
   case 2:capture_busy=true;break;case 3:sender_busy=true;break;case 4:speaker_busy=true;break;
   case 5:s_meeting_mode.state=2;break;case 6:s_monitoring_mode.state=2;break;}
  service_audio_mode_state_sync(&ws);assert(meeting_reports==0&&monitor_reports==0);
 }
 s_monitoring_mode.state=2;atomic_store(&s_monitoring_mode.sd_local,true);
 service_audio_mode_state_sync(&ws);assert(meeting_reports==1&&monitor_reports==1);
 puts("PASS root owner service: complete idle/SD snapshot; readiness and all realtime audio fences");return 0;
}
'''
generated = OUT / 'root-gate.c'
generated.write_text(source, encoding='utf-8')
exe = OUT / 'root-gate.exe'
subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(generated), '-o', str(exe)], check=True)
subprocess.run([str(exe)], check=True)
assert '        service_audio_mode_state_sync(&ws);' in app
assert '    service_audio_mode_state_sync(ws);' in function(app, 'drain_session_commands_limited')
(OUT / 'evidence.json').write_text(json.dumps({'hardwareValidated': False,
    'controllers': ['meeting', 'monitoring'], 'rootServiceExecuted': True,
    'initialFilterRestored': False}, indent=2), encoding='utf-8')
