"""Link actual root handoff/owner selection and both production controllers.

Run run-sd-session-host-checks.py first to generate its shared platform wrapper.
The SD identity API is stubbed here; the real catalog is tested by that suite.
"""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
out = root / 'build-host-checks/sd-control-boundaries'
out.mkdir(parents=True, exist_ok=True)
app = (root/'main/nuwax_ai_partner_main.c').read_text(encoding='utf-8')
start = app.index('static bool prepare_exclusive_audio_cloud_handoff(')
end = app.index('static __attribute__((noinline)) void on_ws_text_non_ota(', start)
(out/'root_handoff.inc').write_text(app[start:end], encoding='utf-8')
start = app.index('    /* Finish the current exclusive owner before consuming')
end = app.index('#if CONFIG_NUWAX_AUDIO_SOURCE_I2S_MIC', start)
(out/'root_selection.inc').write_text(app[start:end], encoding='utf-8')
base = (root/'build-host-checks/sd_meeting_mode_host_check.c').read_text(encoding='utf-8')
base = base.replace('#include "sd_audio_mode_host_check.inc"',
    '#define main sd_baseline_main\n#include "sd_audio_mode_host_check.inc"\n#undef main')
code = r'''
#include <strings.h>
#include "audio_modes/monitoring/monitoring_mode.h"
#include "audio_modes/monitoring/monitoring_protocol.h"
#include "cJSON.h"
static nuwax_meeting_mode_t s_meeting_mode;
static nuwax_monitoring_mode_t s_monitoring_mode;
static int s_notification_transaction;
static bool reprovision, notification, on_rx, fail_identity, change_epoch, local_stop_during_check;
static unsigned identity_checks, lease_cancels, selected_owner;
static nuwax_ws_client_t *checking_ws;
static bool reprovision_request_pending(void) { return reprovision; }
static bool nuwax_notification_blocks_conversation(const int *p) { (void)p; return notification; }
static bool offline_mode_lease_pending(void) { return false; }
static nuwax_offline_audio_mode_t offline_mode_kind(void) { return NUWAX_OFFLINE_AUDIO_MODE_MEETING; }
static bool cancel_offline_mode_lease(nuwax_offline_audio_mode_t m, bool sleep, const char *why)
{ (void)m; (void)sleep; (void)why; ++lease_cancels; return false; }
esp_err_t nuwax_sd_session_check_new_identity(const char *id)
{
    assert(!on_rx); ++identity_checks;
    if (change_epoch) ++checking_ws->connection_epoch;
    if (local_stop_during_check) atomic_fetch_add(&s_meeting_mode.local_stop_generation, 1U);
    if (fail_identity || !card_available) return ESP_ERR_NOT_FINISHED;
    for (unsigned i=0; i<card_used_count; ++i)
        if (!strcasecmp(card_used_ids[i], id)) return ESP_ERR_INVALID_STATE;
    return ESP_OK;
}
#include "root_handoff.inc"
static esp_err_t run_monitoring_audio_source(nuwax_ws_client_t *ws)
{ selected_owner=1; return nuwax_monitoring_mode_run(&s_monitoring_mode, ws); }
static esp_err_t run_meeting_audio_source(nuwax_ws_client_t *ws)
{ selected_owner=2; return nuwax_meeting_mode_run(&s_meeting_mode, ws); }
bool nuwax_audio_uplink_sender_busy(void) { return false; }
static esp_err_t select_owner(nuwax_ws_client_t *ws)
{
#include "root_selection.inc"
    return ESP_OK;
}
static void owner_poll(void *ctx, nuwax_ws_client_t *ws)
{ (void)ctx; service_exclusive_audio_handoff(ws); }

static void setup(bool target_meeting, nuwax_ws_client_t *ws)
{
    card_active=capture_live=false; card_available=card_detected=true;
    card_health=NUWAX_OFFLINE_CAPTURE_RECORDING; card_used_count=0;
    reprovision=notification=fail_identity=change_epoch=local_stop_during_check=false;
    identity_checks=lease_cancels=0; checking_ws=ws; s_can_start=true;
    card_identity_hook=nuwax_sd_session_check_new_identity;
    reset_sent_messages();
    const nuwax_meeting_mode_callbacks_t mc={.can_start=can_start,
        .prepare_cloud_start=meeting_prepare_cloud_start,.poll_controls=owner_poll};
    const nuwax_monitoring_mode_callbacks_t oc={.can_start=can_start,
        .prepare_cloud_start=monitoring_prepare_cloud_start,.poll_controls=owner_poll};
    assert(nuwax_meeting_mode_init(&s_meeting_mode,&mc)==ESP_OK);
    assert(nuwax_monitoring_mode_init(&s_monitoring_mode,&oc)==ESP_OK);
    if (target_meeting) {
        assert(nuwax_monitoring_mode_request_ui(&s_monitoring_mode,ws,true)==ESP_OK);
        nuwax_monitoring_mode_service_sd(&s_monitoring_mode,ws);
        assert(s_monitoring_mode.state==NUWAX_MONITORING_STATE_ACTIVE);
    } else {
        assert(nuwax_meeting_mode_request_ui(&s_meeting_mode,ws,true)==ESP_OK);
        nuwax_meeting_mode_service_sd(&s_meeting_mode,ws);
        assert(s_meeting_mode.state==NUWAX_MEETING_STATE_ACTIVE);
    }
}
static void teardown(void)
{
    nuwax_meeting_mode_deinit(&s_meeting_mode);
    nuwax_monitoring_mode_deinit(&s_monitoring_mode);
    card_active=capture_live=false;
}
static bool dispatch(nuwax_ws_client_t *ws,const char *json)
{
    const unsigned checked=identity_checks;
    on_rx=true;
    assert(prepare_exclusive_audio_cloud_handoff(ws,json,(int)strlen(json)));
    bool consumed=nuwax_monitoring_mode_handle_protocol_text(&s_monitoring_mode,ws,json,(int)strlen(json));
    if(!consumed) consumed=nuwax_meeting_mode_handle_protocol_text(&s_meeting_mode,ws,json,(int)strlen(json));
    on_rx=false; assert(identity_checks==checked);
    return consumed;
}
static void command(char *out,bool meeting,const char *id)
{
    if (!id) {
        snprintf(out,400,"{\"type\":\"device_command\",\"command\":\"set_audio_mode\","
            "\"request_id\":\"switch_open\",\"mode\":\"%s\",\"state\":\"enabled\"}",
            meeting?"meeting":"monitoring");
        return;
    }
    snprintf(out,400,"{\"type\":\"device_command\",\"command\":\"set_audio_mode\","
        "\"request_id\":\"switch_open\",\"mode\":\"%s\",\"state\":\"enabled\",\"audio_session_id\":\"%s\"}",
        meeting?"meeting":"monitoring",id);
}
static void cross_mode(bool target_meeting, unsigned scenario)
{
    nuwax_ws_client_t ws={.connected=true,.connection_epoch=900}; setup(target_meeting,&ws);
    const unsigned stops=card_stops,starts=card_starts;
    char json[400]; const char *id="Cloud_new";
    if(scenario==0) id=NULL;
    if(scenario==1) id="bad.id";
    if(scenario==2) strcpy(card_used_ids[card_used_count++],id);
    if(scenario==3) id=target_meeting?s_monitoring_mode.audio_session_id:s_meeting_mode.audio_session_id;
    if(scenario==4) fail_identity=true;
    if(scenario==5) change_epoch=true;
    if(scenario==6) local_stop_during_check=true;
    if(scenario==7) s_can_start=false;
    if(scenario==8) notification=true;
    if(scenario==9) reprovision=true;
    if(scenario==10) card_available=false;
    command(json,target_meeting,id); assert(dispatch(&ws,json));
    assert(card_stops==stops && capture_live && lease_cancels==0);
    service_exclusive_audio_handoff(&ws);
    if (scenario == 2 || scenario == 3) service_exclusive_audio_handoff(&ws);
    /* Consume only the old owner's stop, if a valid switch was admitted. */
    if(target_meeting) nuwax_monitoring_mode_service_sd(&s_monitoring_mode,&ws);
    else nuwax_meeting_mode_service_sd(&s_meeting_mode,&ws);
    if (scenario == 4 || scenario == 10) {
        /* Busy storage is now an asynchronous preflight. The old owner stays
         * ACTIVE throughout, then the same 20s command deadline rejects it. */
        assert(target_meeting ? s_meeting_mode.state==NUWAX_MEETING_STATE_STARTING :
                                s_monitoring_mode.state==NUWAX_MONITORING_STATE_STARTING);
        s_now_us += 20000000LL;
        service_exclusive_audio_handoff(&ws);
    }
    const bool renewed_id = scenario < 4;
    if(scenario>=4 && scenario<11) {
        assert(card_stops==stops && card_starts==starts && capture_live && lease_cancels==0);
        assert(target_meeting ? s_monitoring_mode.state==NUWAX_MONITORING_STATE_ACTIVE &&
            s_meeting_mode.state==NUWAX_MEETING_STATE_DISABLED :
            s_meeting_mode.state==NUWAX_MEETING_STATE_ACTIVE && s_monitoring_mode.state==NUWAX_MONITORING_STATE_DISABLED);
        if (scenario == 5) assert(!sent_message_contains("switch_open", "device_command_ack", NULL));
        else assert(sent_message_contains("switch_open","\"ok\":false",NULL));
    } else {
        assert(card_stops>stops && !capture_live && card_starts==starts);
        assert(target_meeting?s_meeting_mode.state==NUWAX_MEETING_STATE_STARTING:
                              s_monitoring_mode.state==NUWAX_MONITORING_STATE_STARTING);
        /* Keep writer ownership: selection must continue servicing old STOPPING. */
        assert(select_owner(&ws)==ESP_OK);
        assert(selected_owner==(target_meeting?1U:2U) && card_starts==starts);
        card_active=false;
        assert(select_owner(&ws)==ESP_OK);
        assert(select_owner(&ws)==ESP_OK);
        assert(card_starts==starts+1 && capture_live);
        const char *actual_id = target_meeting ? s_meeting_mode.audio_session_id : s_monitoring_mode.audio_session_id;
        assert(sd_audio_identity_valid(actual_id));
        assert(renewed_id ? (!id || strcmp(id, actual_id) != 0) : strcmp(id, actual_id) == 0);
        assert(sent_message_contains("switch_open","\"ok\":true",actual_id));
    }
    teardown();
}

static unsigned allocation_count, fail_at, live_allocations;
static void *fail_alloc(size_t n)
{ ++allocation_count; if(allocation_count==fail_at)return NULL;
  void *p=malloc(n);if(p)++live_allocations;return p; }
static void fail_free(void *p) { if(p){assert(live_allocations);--live_allocations;free(p);} }
static void sync_oom(void)
{
    nuwax_ws_client_t ws={.connected=true,.connection_epoch=901};
    cJSON_Hooks hooks={.malloc_fn=fail_alloc,.free_fn=fail_free};
    for(unsigned mode=0;mode<2;++mode) for(unsigned i=1;i<40;++i) {
        reset_sent_messages();allocation_count=0;fail_at=i;cJSON_InitHooks(&hooks);
        esp_err_t err= mode?nuwax_monitoring_protocol_send_resume_request(&ws,"oom_sync","CloudScene"):
            nuwax_meeting_protocol_send_resume_request(&ws,"oom_sync","CloudScene");
        assert(live_allocations==0);cJSON_InitHooks(NULL);
        if(err==ESP_OK) {
            assert(s_sent_count==1);
            cJSON *json=cJSON_Parse(s_sent_messages[0]);assert(json);
            const char *fields[]={"type","request_id","mode","state","reason","audio_session_id"};
            for(unsigned f=0;f<6;++f) assert(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(json,fields[f])));
            assert(!strcmp(cJSON_GetObjectItemCaseSensitive(json,"audio_session_id")->valuestring,"CloudScene"));
            cJSON_Delete(json);
        } else assert(s_sent_count==0);
    }
    puts("PASS: all JSON allocation failure positions send nothing, preserve full sync identity, and release partial trees");
}
static void escaped_ids(void)
{
    for(unsigned mode=0;mode<2;++mode) {
        nuwax_ws_client_t ws={.connected=true,.connection_epoch=902}; setup(mode!=0,&ws);
        unsigned stops=card_stops,starts=card_starts;char json[400];
        command(json,mode!=0,"Cloud\\u0000suffix");
        assert(!dispatch(&ws,json)); service_exclusive_audio_handoff(&ws);
        assert(card_stops==stops && card_starts==starts && capture_live);
        command(json,mode!=0,"Cloud\\u005fnew");assert(dispatch(&ws,json));
        service_exclusive_audio_handoff(&ws);
        assert(mode?s_meeting_mode.state==NUWAX_MEETING_STATE_STARTING:
                    s_monitoring_mode.state==NUWAX_MONITORING_STATE_STARTING);
        teardown();
    }
    puts("PASS: escaped NUL rejected without preemption; ordinary escaped valid IDs still accepted");
}
static void legacy_and_queue_boundaries(void)
{
    for(unsigned direction=0;direction<2;++direction) {
        bool meeting=direction!=0;
        nuwax_ws_client_t ws={.connected=true,.connection_epoch=903};setup(meeting,&ws);
        /* Existing no-card controls do not acquire the new SD ID constraint. */
        card_available=card_detected=card_active=capture_live=false;
        s_meeting_mode.sd_local=s_monitoring_mode.sd_local=false;
        char json[400];command(json,meeting,"");assert(dispatch(&ws,json));
        service_exclusive_audio_handoff(&ws);
        if(meeting) nuwax_monitoring_mode_poll_control_events(&s_monitoring_mode,&ws);
        else nuwax_meeting_mode_poll_control_events(&s_meeting_mode,&ws);
        assert(identity_checks==0);
        assert(meeting?s_meeting_mode.state==NUWAX_MEETING_STATE_STARTING &&
            s_monitoring_mode.state==NUWAX_MONITORING_STATE_STOPPING:
            s_monitoring_mode.state==NUWAX_MONITORING_STATE_STARTING &&
            s_meeting_mode.state==NUWAX_MEETING_STATE_STOPPING);
        teardown();
        for(unsigned fill_old=0;fill_old<2;++fill_old) {
            setup(meeting,&ws);
            /* Stale controls fill either target admission or old-owner stop
             * queue. They must not turn a rejected switch into a stop. */
            bool fill_meeting=fill_old?!meeting:meeting;
            if(fill_meeting) {
                nuwax_meeting_event_t event={.type=NUWAX_MEETING_EVENT_CLOUD_DISABLE,
                    .ws_connection_epoch=ws.connection_epoch-1};
                while(xQueueSend(s_meeting_mode.events,&event,0)==pdTRUE){}
            } else {
                nuwax_monitoring_event_t event={.type=NUWAX_MONITORING_EVENT_CLOUD_DISABLE,
                    .ws_connection_epoch=ws.connection_epoch-1};
                while(xQueueSend(s_monitoring_mode.events,&event,0)==pdTRUE){}
            }
            unsigned stops=card_stops;
            command(json,meeting,"Queue_scene");assert(dispatch(&ws,json));
            service_exclusive_audio_handoff(&ws);
            if(meeting) nuwax_monitoring_mode_service_sd(&s_monitoring_mode,&ws);
            else nuwax_meeting_mode_service_sd(&s_meeting_mode,&ws);
            assert(card_stops==stops && capture_live && lease_cancels==0);
            assert(sent_message_contains("switch_open","\"ok\":false",NULL));
            teardown();
        }
    }
    puts("PASS: no-card cross-mode controls unchanged; full target/stop queues reject without stopping current audio");
}
#include "sd_audio_command_deadline_host_check.inc"
int main(void)
{
    assert(sd_baseline_main()==0);
    for(unsigned direction=0;direction<2;++direction)
        for(unsigned scenario=0;scenario<12;++scenario)cross_mode(direction!=0,scenario);
    puts("PASS: 24 cross-mode cases: invalid/used/current IDs, busy media, allocation-independent owner admission, epoch/local stop races, valid switches and tail drain ordering");
    sync_oom();escaped_ids();legacy_and_queue_boundaries();deadline_cases();
    return 0;
}
'''
(out/'test.c').write_text(base+code, encoding='utf-8')
sources = [root/f'main/audio_modes/{mode}/{mode}_{part}.c'
           for mode in ('meeting', 'monitoring') for part in ('mode', 'protocol')]
sources += [root/'main/audio_modes/meeting/meeting_processor.c', out/'test.c',
            root/'main/audio_modes/meeting/host_stubs/device_error_report.c',
            root/'managed_components/espressif__cjson/cJSON/cJSON.c']
includes = [out, root/'main/audio_modes/meeting/host_stubs', root/'main/audio_modes/meeting',
            root/'main/audio_modes/monitoring', root/'main', root/'main/conversation',
            root/'managed_components/espressif__cjson/cJSON']
for compat in (0, 1):
    exe=out/f'test-compat-{compat}.exe'
    subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function',
        '-DCONFIG_NUWAX_SD_SESSION_ENABLE=1','-DCONFIG_NUWAX_OFFLINE_AUDIO_ENABLE=1',
        f'-DCONFIG_NUWAX_SD_LEGACY_CONTROL_COMPAT={compat}',
        *['-I'+str(p) for p in includes], *map(str,sources), '-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=30)
