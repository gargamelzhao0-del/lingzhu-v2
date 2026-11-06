static void reset(int kind) {
    memset(&s_awake_session,0,sizeof(s_awake_session));
    nuwax_prelisten_state_clear(&s_prelisten);
    nuwax_turn_guard_clear(&s_turn_guard);
    nuwax_playback_terminal_guard_clear(&s_playback_terminal_guard);
    nuwax_conversation_visual_round_clear(&s_visual_round);
    clear_last_listen_resume(); clear_ws_reconnect_audio_recovery();
    scenario=kind; now=100; harness_end_ms=600100; state=NUWAX_APP_STATE_SLEEPING;
    send_result=kind==2 ? ESP_ERR_INVALID_STATE : ESP_OK;
    queued_wake=true; wake_desired=tts_injected=false; s_wake_word_events=(void*)1;
    real_loop_iterations=sends=0;
    capture_starts=wake_rearms=cleared=status_updates=close_requests=0;
    s_sleep_transaction.active=false;
    s_sleep_transaction.state=NUWAX_SLEEP_TRANSACTION_SLEEPING;
    s_sleep_request_pending=s_deferred_sleep_intent.active=false;
    transport_closed=menu_open=mode_owner=mode_pending=ota_active=false;
    notification_busy=power_pending=false; dynamic_epoch=1;
    atomic_store(&s_reprovision_quiesce_requested,false);
}

static void arm_expired_greeting(void) {
    reset(0); queued_wake=false;
    s_sleep_transaction.state=NUWAX_SLEEP_TRANSACTION_IDLE;
    begin_awake_session("test");
    assert(begin_active_turn_guard("wake_test",NULL,15000,"test"));
    assert(begin_conversation_visual_round(NUWAX_VISUAL_ROUND_WAKE_GREETING,"wake_test","test"));
    now+=15001;
}

int main(void) {
    int ws=0;
    for(int kind=0;kind<2;kind++) {
        reset(kind);
        assert(wait_for_mic_start_action(&ws)==ESP_ERR_INVALID_STATE);
        assert(close_requests==1 && transport_closed && capture_starts==0);
        assert(now-100 < 32000U); /* Existing 15s/30s deadlines, not added delay. */
        assert(preserve_awake_session_for_ws_reconnect("test"));
        dynamic_epoch=2; transport_closed=false; now+=1000;
        assert(arm_ws_reconnect_audio_recovery(&ws,"test"));
        assert(recover_awake_conversation_after_ws_reconnect(&ws,"test")==NUWAX_WS_RECOVERY_CONVERGED);
        assert(s_prelisten.pending && state==NUWAX_APP_STATE_LISTENING);
        assert(!nuwax_ws_close_if_epoch(&ws,1,NUWAX_WS_CLOSE_CAUSE_WAKE_GREETING_TIMEOUT));
        printf("PASS real wake wait scenario=%d: one scoped close; new epoch restores listening; stale close rejected\n",kind);
    }
    reset(2);
    assert(wait_for_mic_start_action(&ws)==ESP_OK);
    assert(s_prelisten.capture_active && !close_requests);
    puts("PASS immediate greeting send failure keeps original local recovery");

    reset(0); assert(wait_for_mic_start_action(&ws)==ESP_ERR_INVALID_STATE);
    assert(preserve_awake_session_for_ws_reconnect("menu"));
    dynamic_epoch=2; transport_closed=false; now+=1000; menu_open=true;
    assert(arm_ws_reconnect_audio_recovery(&ws,"menu"));
    assert(recover_awake_conversation_after_ws_reconnect(&ws,"menu")==NUWAX_WS_RECOVERY_MENU_BLOCKED);
    assert(state==NUWAX_APP_STATE_IDLE && !s_prelisten.pending);
    menu_open=false; maybe_enter_idle_sleep(); assert(s_prelisten.pending);
    puts("PASS reconnect respects menu, then resumes through original idle owner");

    arm_expired_greeting(); ota_active=true;
    check_conversation_turn_timeout(&ws);
    assert(s_turn_guard.active && !close_requests); /* Keep bound expiry, no unbound retry. */
    ota_active=false; check_conversation_turn_timeout(&ws); assert(close_requests==1);
    puts("PASS OTA defers greeting retirement without losing recovery identity");

    for(int fence=0;fence<8;fence++) {
        arm_expired_greeting();
        switch(fence) {
            case 0: s_visual_round.ws_epoch=2; break;
            case 1: s_visual_round.awake_generation++; break;
            case 2: strcpy(s_visual_round.request_id,"new_request"); break;
            case 3: s_visual_round.first_pcm_seen=true; break;
            case 4: power_pending=true; break;
            case 5: s_sleep_request_pending=true; break;
            case 6: mode_owner=true; break;
            case 7: notification_busy=true; break;
        }
        check_conversation_turn_timeout(&ws); assert(!close_requests);
    }
    arm_expired_greeting(); s_turn_guard.speaker_generation=7;
    assert(!nuwax_wake_greeting_needs_epoch_recovery(&s_visual_round,&s_turn_guard,1,s_awake_session.generation));
    arm_expired_greeting(); s_visual_round.kind=NUWAX_VISUAL_ROUND_NORMAL;
    check_conversation_turn_timeout(&ws); assert(!close_requests);
    arm_expired_greeting(); now-=15000;
    check_conversation_turn_timeout(&ws); assert(s_turn_guard.active && !close_requests);
    assert(!nuwax_ws_close_policy_admits(NUWAX_WS_CLOSE_CAUSE_BUSINESS_RECOVERY,true));
    puts("PASS identity, deadline, physical playback, sleep, power, mode and notification fences; generic heartbeat policy unchanged");
    return 0;
}
