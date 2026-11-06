/* Included by the production-controller/root-selector host harness. */
static nuwax_audio_command_deadline_t *target_command(bool meeting)
{ return meeting ? &s_meeting_mode.command_deadline : &s_monitoring_mode.command_deadline; }
static bool target_disabled(bool meeting)
{ return meeting ? s_meeting_mode.state == NUWAX_MEETING_STATE_DISABLED :
                   s_monitoring_mode.state == NUWAX_MONITORING_STATE_DISABLED; }
static void finish_old(bool meeting, nuwax_ws_client_t *ws)
{
    card_active = capture_live = false;
    if (meeting) nuwax_monitoring_mode_service_sd(&s_monitoring_mode, ws);
    else nuwax_meeting_mode_service_sd(&s_meeting_mode, ws);
}
static void deadline_cases(void)
{
    for (unsigned direction = 0; direction < 2; ++direction) {
        const bool meeting = direction != 0;
        for (unsigned scenario = 0; scenario < 10; ++scenario) {
            nuwax_ws_client_t ws = {.connected=true, .connection_epoch=990};
            /* Exercise the received_ms low-32 wrap without changing the deadline. */
            s_now_us = scenario == 9 ? ((int64_t)UINT32_MAX - 10000) * 1000 : 1000000;
            setup(meeting, &ws);
            const unsigned starts = card_starts, stops = card_stops;
            char json[400]; command(json, meeting, "Deadline_scene");
            const int64_t received = s_now_us;
            assert(dispatch(&ws, json));
            if (scenario == 1) s_now_us = received + 21000000; /* Queue already expired. */
            if (scenario == 2 || scenario == 3) fail_identity = true;
            service_exclusive_audio_handoff(&ws);
            if (scenario == 1) {
                assert(target_disabled(meeting) && card_starts == starts && card_stops == stops);
                assert(capture_live && sent_message_contains("switch_open", "\"ok\":false", NULL));
                teardown(); continue;
            }
            if (scenario == 2 || scenario == 3) {
                assert(capture_live && card_stops == stops && !target_command(meeting)->prepared);
                s_now_us = received + (scenario == 2 ? 21000000 : 15000000);
                fail_identity = false;
                service_exclusive_audio_handoff(&ws);
                if (scenario == 2) {
                    assert(target_disabled(meeting) && capture_live && card_stops == stops);
                    assert(sent_message_contains("switch_open", "mode_handoff_failed", "\"ok\":false"));
                    teardown(); continue;
                }
            }
            assert(select_owner(&ws) == ESP_OK); /* Old owner remains in STOPPING. */
            assert(!capture_live && card_starts == starts);
            if (scenario == 3) {
                finish_old(meeting, &ws); assert(select_owner(&ws) == ESP_OK);
                assert(capture_live && card_starts == starts + 1);
                assert(sent_message_contains("switch_open", "\"ok\":true", NULL));
                teardown(); continue;
            }
            if (scenario == 4 || scenario == 7) {
                card_health = scenario == 4 ? NUWAX_OFFLINE_CAPTURE_STARTING : NUWAX_OFFLINE_CAPTURE_RECORDING;
                if (scenario == 7) sd_fail_command_ack = true;
                finish_old(meeting, &ws); assert(select_owner(&ws) == ESP_OK);
                assert(card_starts == starts + 1);
            }
            if (scenario == 5 || scenario == 6) sd_fail_command_ack = true;
            if (scenario == 8) { ++ws.connection_epoch; service_exclusive_audio_handoff(&ws); }
            else { s_now_us = received + 20000000; service_exclusive_audio_handoff(&ws); }
            nuwax_audio_command_deadline_t *c = target_command(meeting);
            assert(c->decided && (c->ok == (scenario == 7)));
            assert(c->received_us == received);
            if (scenario == 7) {
                assert(capture_live); /* A real successful start is not rolled back for lost ACK. */
            } else {
                assert(!capture_live);
                assert(!sent_message_contains("switch_open", "\"ok\":true", NULL));
            }
            if (scenario == 5) {
                sd_fail_command_ack = false; s_now_us = received + 20500000;
                service_exclusive_audio_handoff(&ws);
                assert(c->attempts == 2 && !c->ack_pending);
                assert(sent_message_contains("switch_open", "mode_handoff_failed", NULL));
            }
            const unsigned attempts = c->attempts;
            sd_fail_command_ack = false;
            s_now_us = received + 25000000;
            service_exclusive_audio_handoff(&ws);
            assert(!c->ack_pending && c->attempts == attempts);
            if (scenario == 7) {
                const unsigned sent = s_sent_count;
                assert(dispatch(&ws, json));
                if (meeting) nuwax_meeting_mode_poll_control_events(&s_meeting_mode, &ws);
                else nuwax_monitoring_mode_poll_control_events(&s_monitoring_mode, &ws);
                assert(s_sent_count == sent && capture_live && c->attempts == attempts);
                teardown(); continue;
            }
            finish_old(meeting, &ws);
            if (scenario == 4) { /* Finish the cancelled new capture's own tail. */
                if (meeting) nuwax_meeting_mode_service_sd(&s_meeting_mode, &ws);
                else nuwax_monitoring_mode_service_sd(&s_monitoring_mode, &ws);
            }
            card_health = NUWAX_OFFLINE_CAPTURE_RECORDING;
            const unsigned final_starts = card_starts;
            assert(dispatch(&ws, json)); /* Late retry must not reset its clock. */
            if (meeting) nuwax_meeting_mode_poll_control_events(&s_meeting_mode, &ws);
            else nuwax_monitoring_mode_poll_control_events(&s_monitoring_mode, &ws);
            assert(select_owner(&ws) == ESP_OK);
            assert(target_disabled(meeting) && card_starts == final_starts && !capture_live);
            assert(c->received_us == received && c->attempts == attempts);
            teardown();
        }
    }
    puts("PASS: 20 deadline cases: both handoff directions, 20s cancellation/25s ACK cutoff, queue age, slow preflight, late MIC readiness, ACK retry/loss, reconnect, duplicate requests and clock wrap");
}
