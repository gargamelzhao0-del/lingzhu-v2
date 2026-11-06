static void reset_mode(int kind) {
    mode_kind=kind; lease=true; capture=expired=false;
    meeting_resume=monitoring_resume=false;
    init_error=ESP_FAIL; clock_ms=1; init_calls=start_calls=warnings=0;
    health=NUWAX_OFFLINE_CAPTURE_UNAVAILABLE; cancel_during_start=false;
    s_meeting_mode=(mode_t){0}; s_monitoring_mode=(mode_t){0};
    offline_mode_identity()->resuming=true;
    s_offline_mode_capture_started=false;
    atomic_store(&s_reprovision_quiesce_requested,false);
    atomic_store(&s_offline_wait_started_ms,clock_ms);
    strcpy(s_offline_mode_resume_session,"original_session");
    reset_offline_capture_retry_state();
    atomic_fetch_add(&s_ui_lifecycle_generation,1);
}

int main(void) {
    for(int kind=0;kind<2;kind++) {
        reset_mode(kind);
        for(unsigned tick=0;tick<6000;tick++,clock_ms+=100) {
            service_offline_capture_retry_disconnected();
            sync_exclusive_audio_mode_ui_state();
            assert(lease && !capture && !start_calls);
            assert(kind ? last_monitoring==NUWAX_MONITORING_STATE_ERROR : last_meeting==NUWAX_MEETING_STATE_ERROR);
        }
        assert(init_calls>0 && init_calls<130 && warnings<=2);
        printf("PASS mode=%d 10min no SD: starts=0, error visible, retries=%u diagnostics=%u\n",kind,init_calls,warnings);
        init_error=ESP_OK; clock_ms+=6000;
        service_offline_capture_retry_disconnected(); sync_exclusive_audio_mode_ui_state();
        assert(capture && start_calls==1 && !atomic_load(&s_offline_mode_capture_failed));
        assert(kind ? last_monitoring==NUWAX_MONITORING_STATE_ACTIVE : last_meeting==NUWAX_MEETING_STATE_ACTIVE);
        assert(!strcmp(s_offline_mode_resume_session,"original_session"));
        assert(cancel_offline_mode_lease(kind,true,"test_stop"));
        service_offline_capture_retry_disconnected(); sync_exclusive_audio_mode_ui_state();
        assert(!lease && !capture && start_calls==1 && !last_meeting && !last_monitoring);
        puts("PASS media recovery preserves session; explicit stop prevents later automatic start");

        reset_mode(kind); clock_ms+=8U*3600000U;
        service_offline_capture_retry_disconnected();
        assert(!lease && !start_calls && !offline_mode_identity()->resuming);
        init_error=ESP_OK; service_offline_capture_retry_disconnected(); assert(!start_calls);
        puts("PASS 8h missing-card wait expires despite zero MIC runtime");

        reset_mode(kind); init_error=ESP_OK; cancel_during_start=true;
        service_offline_capture_retry_disconnected();
        assert(!lease && !capture && !s_offline_mode_capture_started);
        puts("PASS stop during task launch cannot publish successful recording");

        reset_mode(kind); expired=true;
        service_offline_capture_retry_disconnected(); assert(!lease && !start_calls);
    }
    nuwax_offline_capture_watch_t watch={0};
    assert(nuwax_offline_capture_observe(&watch,0,true,false,false)==NUWAX_OFFLINE_CAPTURE_RECORDING);
    assert(nuwax_offline_capture_observe(&watch,20,false,true,false)==NUWAX_OFFLINE_CAPTURE_BUFFERING);
    assert(nuwax_offline_capture_observe(&watch,5000,true,true,false)==NUWAX_OFFLINE_CAPTURE_RECORDING);
    assert(nuwax_offline_capture_observe(&watch,5010,false,false,false)==NUWAX_OFFLINE_CAPTURE_UNAVAILABLE);
    watch=(nuwax_offline_capture_watch_t){0};
    assert(nuwax_offline_capture_observe(&watch,UINT32_MAX-100,false,true,false)==NUWAX_OFFLINE_CAPTURE_BUFFERING);
    assert(nuwax_offline_capture_observe(&watch,100,false,true,false)==NUWAX_OFFLINE_CAPTURE_BUFFERING);
    assert(nuwax_offline_capture_observe(&watch,12000,false,true,false)==NUWAX_OFFLINE_CAPTURE_BUFFERING);
    assert(nuwax_offline_capture_observe(&watch,16000,false,true,false)==NUWAX_OFFLINE_CAPTURE_UNAVAILABLE);
    assert(nuwax_offline_capture_observe(&watch,16001,true,true,true)==NUWAX_OFFLINE_CAPTURE_UNAVAILABLE);
    assert(!nuwax_offline_wait_expired(100,UINT32_MAX-100,8));
    puts("PASS quiet monitoring, transient slow card, sustained failure, loss and clock wrap boundaries");
    return 0;
}
