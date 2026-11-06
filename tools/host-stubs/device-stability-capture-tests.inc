static void reset_capture(void) {
    if(launched_config)free(launched_config);
    launched_config=NULL;allocations=launches=0;
    memset(&s,0,sizeof(s));s.admission_lock=s.lock=1;
    atomic_store(&s_media_ready,true);atomic_store(&s_space_blocked,false);
    atomic_store(&s_mirror_closers,0);atomic_store(&s_shutdown_requested,false);
    atomic_store(&s_capture_cancel_epoch,0);atomic_store(&s_offline_limit_reached,false);
    powered_down=cancel_during_alloc=false; initialized=true;
    s_limit_session[0]=0;s_limit_used_us=0;
}
int main(void) {
    reset_capture(); fake_free_bytes=NUWAX_OFFLINE_AUDIO_FREE_RESERVE_BYTES+CONFIG_NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES;
    assert(refresh_capture_capacity_locked()==ESP_OK && !atomic_load(&s_space_blocked));
    fake_free_bytes--;
    assert(refresh_capture_capacity_locked()==ESP_OK && atomic_load(&s_space_blocked));
    fake_free_bytes+=2; s.used_bytes=ARCHIVE_QUOTA_BYTES-CONFIG_NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES+1;
    assert(refresh_capture_capacity_locked()==ESP_OK && atomic_load(&s_space_blocked));
    s.used_bytes=0; capacity_error=ESP_ERR_INVALID_STATE;
    assert(refresh_capture_capacity_locked()==ESP_ERR_INVALID_STATE && atomic_load(&s_space_blocked));
    capacity_error=ESP_OK;
    assert(refresh_capture_capacity_locked()==ESP_OK && !atomic_load(&s_space_blocked));
    puts("PASS production capacity: FAT reserve, archive quota, query failure and space recovery");
    reset_capture();
    assert(!healthy_capture_defers_media_probe());
    atomic_store(&s.capture_running,true);
    assert(healthy_capture_defers_media_probe());
    atomic_store(&s_media_ready,false);
    assert(!healthy_capture_defers_media_probe());
    atomic_store(&s_media_ready,true);atomic_store(&s_space_blocked,true);
    assert(!healthy_capture_defers_media_probe());
    puts("PASS healthy quiet monitoring defers routine SD probe; actual media fault/full remains recoverable");
    for(int gate=0;gate<6;gate++) {
        reset_capture();
        switch(gate) {
            case 0:atomic_store(&s_media_ready,false);break;
            case 1:atomic_store(&s_space_blocked,true);break;
            case 2:atomic_store(&s_mirror_closers,1);break;
            case 3:atomic_store(&s_shutdown_requested,true);break;
            case 4:atomic_store(&s.media_recovery_requested,true);break;
            case 5:powered_down=true;break;
        }
        assert(nuwax_offline_audio_start_capture_continuation(0,"original",16000,1,20,42)==ESP_ERR_NOT_FINISHED);
        assert(!allocations && !launches && !atomic_load(&s.capture_running));
    }
    puts("PASS production capture admission: no card, full, tail owner, shutdown, recovery and board power fence allocate nothing");
    for(int mode=0;mode<2;mode++) {
        reset_capture();strcpy(s_limit_session,"original");s_limit_used_us=987654;
        assert(nuwax_offline_audio_start_capture_continuation(mode,"original",16000,1,20,42)==ESP_OK);
        offline_capture_config_t *cfg=launched_config;
        assert(cfg && cfg->first_sequence==42 && cfg->limit_used_us==987654 && !strcmp(cfg->session_id,"original"));
        assert(!offline_capture_cancelled(cfg->cancel_epoch));
        assert(atomic_load(&s_capture_health)==NUWAX_OFFLINE_CAPTURE_STARTING);
        nuwax_offline_audio_request_stop(false);
        assert(offline_capture_cancelled(cfg->cancel_epoch) && !atomic_load(&s.capture_complete));
    }
    reset_capture();cancel_during_alloc=true;
    assert(nuwax_offline_audio_start_capture_continuation(0,"original",16000,1,20,42)==ESP_OK);
    offline_capture_config_t *cfg=launched_config;
    assert(offline_capture_cancelled(cfg->cancel_epoch));
    puts("PASS production stop epoch survives launch flag reset; session, cursor and consumed 8h budget retained");
    reset_capture();
    return 0;
}
