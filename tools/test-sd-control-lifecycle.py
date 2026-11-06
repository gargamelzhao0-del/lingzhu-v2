"""Actual root/controllers: stop after lost opening ACK and stalled rejection TX."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
out = root/'build-host-checks/sd-control-lifecycle'
out.mkdir(parents=True,exist_ok=True)
baseline = subprocess.run(['python', 'tools/test-sd-control-boundaries.py'], cwd=root,
    capture_output=True, text=True, encoding='utf-8', errors='replace')
(out/'control-baseline.log').write_text(baseline.stdout+baseline.stderr, encoding='utf-8')
assert baseline.returncode == 0
generated = root/'build-host-checks/sd-control-boundaries'
source = (generated/'test.c').read_text(encoding='utf-8')
source = source[:source.rindex('\nint main(void)')]
source = '#define nuwax_ws_send_text_until_if_epoch audit_base_bounded\n' + source
source = source.replace('static bool sd_fail_command_ack;', '''static bool sd_fail_command_ack;
static bool audit_inside_bounded;
static bool audit_fail_close;
static int64_t audit_legacy_delay_us;
static unsigned audit_legacy_sends, audit_bounded_sends;''')
source = source.replace(' if (sd_fail_command_ack && strstr(payload, "device_command_ack"))', '''
 if (strstr(payload, "device_command_ack") && !audit_inside_bounded) {
     ++audit_legacy_sends;
     if (strstr(payload, "local_stop_superseded")) s_now_us += audit_legacy_delay_us;
 }
 if (audit_fail_close && strstr(payload, "audio_mode_request") && strstr(payload, "disabled")) return ESP_FAIL;
 if (sd_fail_command_ack && strstr(payload, "device_command_ack"))''')
source += r'''
#undef nuwax_ws_send_text_until_if_epoch
esp_err_t nuwax_ws_send_text_until_if_epoch(nuwax_ws_client_t *ws,const char *text,int64_t until,uint32_t epoch)
{
    ++audit_bounded_sends; audit_inside_bounded=true;
    assert(until-s_now_us<=250000 && until>s_now_us);
    if (audit_legacy_delay_us && strstr(text,"local_stop_superseded")) {
        s_now_us=until; audit_inside_bounded=false; return ESP_ERR_TIMEOUT;
    }
    esp_err_t result=audit_base_bounded(ws,text,until,epoch);
    audit_inside_bounded=false; return result;
}
static void audit_active_ack_after_stop(bool meeting, unsigned scenario)
{
    nuwax_ws_client_t ws={.connected=true,.connection_epoch=2200};
    s_now_us=1000000;
    setup(meeting,&ws);
    char json[400]; command(json,meeting,"Audit_scene");
    assert(dispatch(&ws,json)); service_exclusive_audio_handoff(&ws);
    assert(select_owner(&ws)==ESP_OK);
    finish_old(meeting,&ws);
    sd_fail_command_ack=true;
    assert(select_owner(&ws)==ESP_OK);
    nuwax_audio_command_deadline_t *c=target_command(meeting);
    assert(c->decided && c->ok && c->ack_pending && capture_live);
    /* User stops while the opening ACK is still waiting for retry. */
    reset_sent_messages();
    audit_fail_close=scenario>=1 && scenario<=3;
    if (scenario==4) {
        /* Natural capture completion before finalization/owner polling. */
        capture_live=false; sd_fail_command_ack=false; s_now_us+=600000;
    } else if(meeting) assert(nuwax_meeting_mode_request_ui(&s_meeting_mode,&ws,false)==ESP_OK);
    else assert(nuwax_monitoring_mode_request_ui(&s_monitoring_mode,&ws,false)==ESP_OK);
    if(meeting) nuwax_meeting_mode_service_sd(&s_meeting_mode,&ws);
    else nuwax_monitoring_mode_service_sd(&s_monitoring_mode,&ws);
    card_active=false;
    if(meeting) nuwax_meeting_mode_service_sd(&s_meeting_mode,&ws);
    else nuwax_monitoring_mode_service_sd(&s_monitoring_mode,&ws);
    assert(target_disabled(meeting) && !capture_live);
    if (scenario==2) {
        unsigned starts=card_starts;
        if (meeting) {
            assert(nuwax_meeting_mode_request_ui(&s_meeting_mode,&ws,true)==ESP_OK);
            nuwax_meeting_mode_service_sd(&s_meeting_mode,&ws);
        } else {
            assert(nuwax_monitoring_mode_request_ui(&s_monitoring_mode,&ws,true)==ESP_OK);
            nuwax_monitoring_mode_service_sd(&s_monitoring_mode,&ws);
        }
        audit_fail_close=sd_fail_command_ack=false; s_now_us+=600000;
        service_exclusive_audio_handoff(&ws);
        assert(capture_live && card_starts==starts+1);
        assert(!sent_message_contains("audio_mode_request","\"state\":\"disabled\"",NULL));
        assert(!sent_message_contains("switch_open","\"ok\":true",NULL));
        puts("PASS: delayed close/old opening cannot affect a new local scene");
        teardown(); return;
    }
    if (scenario==3) {
        ws.connected=false;
        if (meeting) nuwax_meeting_mode_notify_ws_disconnected(&s_meeting_mode);
        else nuwax_monitoring_mode_notify_ws_disconnected(&s_monitoring_mode);
        ws.connected=true; ++ws.connection_epoch;
        audit_fail_close=sd_fail_command_ack=false; s_now_us+=600000;
        service_exclusive_audio_handoff(&ws);
        assert(sent_message_contains("audio_mode_request","\"state\":\"disabled\"",NULL));
        assert(target_disabled(meeting) && !c->ack_pending);
        reset_sent_messages(); service_exclusive_audio_handoff(&ws);
        assert(!sent_message_contains("audio_mode_request","\"state\":\"disabled\"",NULL));
        puts("PASS: stopped SD scene delivers its close once after reconnect; newer local scenes cancel old closes");
        teardown(); return;
    }
    audit_fail_close=false;
    sd_fail_command_ack=false;
    s_now_us += 600000;
    service_exclusive_audio_handoff(&ws);
    bool stale_active=sent_message_contains("switch_open","\"state\":\"enabled\"","\"ok\":true");
    bool close=sent_message_contains("audio_mode_request","\"state\":\"disabled\"",NULL);
    printf("PASS active_ack_after_stop mode=%s scenario=%u disabled=%d stale_active_ack=%d disabled_sync=%d\n",
        meeting?"meeting":"monitoring",scenario,target_disabled(meeting),stale_active,close);
    assert(!stale_active && close && c->retired && !c->ack_pending);
    /* A duplicate old opening command must not restart after the local stop. */
    unsigned starts=card_starts;
    command(json,meeting,"Audit_scene"); assert(dispatch(&ws,json));
    service_exclusive_audio_handoff(&ws);
    assert(target_disabled(meeting) && card_starts==starts);
    assert(!sent_message_contains("switch_open","\"state\":\"enabled\"","\"ok\":true"));
    teardown();
}
static void audit_unbounded_rejection(bool meeting)
{
    nuwax_ws_client_t ws={.connected=true,.connection_epoch=2201};
    s_now_us=1000000; setup(meeting,&ws); fail_identity=true;
    char json[400]; command(json,meeting,"Delayed_scene");
    const int64_t received=s_now_us;
    assert(dispatch(&ws,json)); service_exclusive_audio_handoff(&ws);
    assert(!target_command(meeting)->prepared && !target_disabled(meeting) && capture_live);
    s_now_us=received+19000000;
    snprintf(json,sizeof(json),"{\"type\":\"device_command\",\"command\":\"set_audio_mode\","
        "\"request_id\":\"reject_old_stop\",\"mode\":\"%s\",\"state\":\"disabled\","
        "\"audio_session_id\":\"wrong_old_scene\"}",meeting?"monitoring":"meeting");
    assert(dispatch(&ws,json));
    /* Invalidate this queued stop by a newer local action. An ID mismatch is
     * intentionally no longer a rejection for desired-state commands. */
    if (meeting) atomic_fetch_add(&s_monitoring_mode.local_stop_generation, 1U);
    else atomic_fetch_add(&s_meeting_mode.local_stop_generation, 1U);
    audit_legacy_sends=audit_bounded_sends=0;
    /* Within the existing ordinary send's independent 5s lock + 4s I/O budgets. */
    audit_legacy_delay_us=8000000;
    if(meeting) nuwax_monitoring_mode_service_sd(&s_monitoring_mode,&ws);
    else nuwax_meeting_mode_service_sd(&s_meeting_mode,&ws);
    audit_legacy_delay_us=0;
    assert(s_now_us < received+20000000 && !target_disabled(meeting));
    s_now_us=received+20000000;
    service_exclusive_audio_handoff(&ws);
    const bool ack=sent_message_contains("switch_open","device_command_ack",NULL);
    printf("PASS unbounded_rejection mode=%s elapsed_ms=%lld legacy_sends=%u deadline_sends=%u switch_failure_ack=%d\n",
        meeting?"meeting":"monitoring",(long long)((s_now_us-received)/1000),audit_legacy_sends,audit_bounded_sends,ack);
    assert(target_disabled(meeting) && target_command(meeting)->decided && ack);
    assert(audit_legacy_sends==0 && audit_bounded_sends>0 && s_now_us<received+25000000);
    teardown();
}
static void audit_rx_and_expired_rejections(bool meeting)
{
    nuwax_ws_client_t ws={.connected=true,.connection_epoch=2202};
    s_now_us=100000000; setup(meeting,&ws);
    nuwax_audio_command_deadline_t before=*target_command(meeting);
    audit_bounded_sends=audit_legacy_sends=0;
    char json[400]; command(json,meeting,"Busy_scene"); s_can_start=false;
    assert(dispatch(&ws,json));
    assert(audit_bounded_sends==1 && audit_legacy_sends==0);
    assert(!memcmp(&before,target_command(meeting),sizeof(before)));
    s_can_start=true;
    /* A full RX queue also replies without touching the owner's transaction. */
    if (meeting) {
        nuwax_meeting_event_t event={.type=NUWAX_MEETING_EVENT_CLOUD_DISABLE,
            .ws_connection_epoch=ws.connection_epoch, .received_ms=70000};
        strcpy(event.request_id,"expired_queued_command");
        while (xQueueSend(s_meeting_mode.events,&event,0)==pdTRUE) {}
    } else {
        nuwax_monitoring_event_t event={.type=NUWAX_MONITORING_EVENT_CLOUD_DISABLE,
            .ws_connection_epoch=ws.connection_epoch, .received_ms=70000};
        strcpy(event.request_id,"expired_queued_command");
        while (xQueueSend(s_monitoring_mode.events,&event,0)==pdTRUE) {}
    }
    assert(dispatch(&ws,json));
    assert(audit_bounded_sends==2 && audit_legacy_sends==0);
    assert(!memcmp(&before,target_command(meeting),sizeof(before)));
    if (meeting) (void)nuwax_meeting_mode_abort_pending_controls(&s_meeting_mode,&ws,ws.connection_epoch,"recovery");
    else (void)nuwax_monitoring_mode_abort_pending_controls(&s_monitoring_mode,&ws,ws.connection_epoch,"recovery");
    assert(audit_bounded_sends==2 && audit_legacy_sends==0);
    assert(!sent_message_contains("expired_queued_command","device_command_ack",NULL));
    puts("PASS: busy/queue-full RX replies bounded and owner-isolated; expired queued ACKs suppressed");
    teardown();
}
int main(void)
{
    for (unsigned scenario=0; scenario<5; ++scenario) {
        audit_active_ack_after_stop(false,scenario);
        audit_active_ack_after_stop(true,scenario);
    }
    audit_unbounded_rejection(false);
    audit_unbounded_rejection(true);
    audit_rx_and_expired_rejections(false);
    audit_rx_and_expired_rejections(true);
    return 0;
}
'''
(out/'sd-control-lifecycle.c').write_text(source, encoding='utf-8')
sources=[root/f'main/audio_modes/{mode}/{mode}_{part}.c'
    for mode in ('meeting','monitoring') for part in ('mode','protocol')]
sources += [root/'main/audio_modes/meeting/meeting_processor.c',out/'sd-control-lifecycle.c',
    root/'main/audio_modes/meeting/host_stubs/device_error_report.c',
    root/'managed_components/espressif__cjson/cJSON/cJSON.c']
includes=[generated,root/'main/audio_modes/meeting/host_stubs',root/'main/audio_modes/meeting',
    root/'main/audio_modes/monitoring',root/'main',root/'main/conversation',
    root/'managed_components/espressif__cjson/cJSON']
logs=[]
for compat in (0,1):
    exe=out/f'sd-control-lifecycle-{compat}.exe'
    subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function',
        '-DCONFIG_NUWAX_SD_SESSION_ENABLE=1','-DCONFIG_NUWAX_OFFLINE_AUDIO_ENABLE=1',
        f'-DCONFIG_NUWAX_SD_LEGACY_CONTROL_COMPAT={compat}',
        *['-I'+str(p) for p in includes],*map(str,sources),'-o',str(exe)],check=True)
    result=subprocess.run([str(exe)],cwd=root,capture_output=True,text=True,timeout=30)
    logs.append(f'compat={compat}\n'+result.stdout+result.stderr)
    (out/'sd-control-lifecycle.log').write_text('\n'.join(logs),encoding='utf-8')
    print(logs[-1])
    if result.returncode: raise SystemExit(result.returncode)
