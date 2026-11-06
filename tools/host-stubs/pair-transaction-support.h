static bool s_provisioned_pair_ready, s_has_staged_vox_config;
static nuwax_cloud_config_t s_staged_vox_config;
static int s_state_lock;
typedef struct {
    bool wifi_candidate_hash_valid, new_wifi_attempt_active, failed_attempt_release_pending;
    uint32_t wifi_candidate_hash, wifi_attempt_owner_generation, owner_generation;
    int64_t failed_attempt_release_deadline_us, credentials_received_us, runtime_activity_us;
    int outcome;
} provision_state_t;
static provision_state_t s_state;

typedef struct { int ns; char key[24]; size_t size; uint8_t value[2048]; } slot_t;
static slot_t disk[16];
static wifi_config_t wifi, old_wifi, target_wifi;
static nuwax_cloud_config_t old_cloud, target_cloud;
static bool connected;
static int operations, cut_at, fail_at, after_mutation, open_error;
static const char *read_error_key;
static int64_t clock_us;
static jmp_buf power_cut;
static slot_t *slot(int ns,const char *key,bool create) {
    slot_t *free_slot=NULL;
    for(size_t i=0;i<16;i++) {
        if(disk[i].ns==ns&&!strcmp(disk[i].key,key))return &disk[i];
        if(disk[i].ns==0)free_slot=&disk[i];
    }
    if(!create)return NULL;
    assert(free_slot);free_slot->ns=ns;strcpy(free_slot->key,key);return free_slot;
}
static int before_write(void) {
    operations++;
    if(!after_mutation&&operations==cut_at)longjmp(power_cut,1);
    return !after_mutation&&operations==fail_at ? ESP_ERR_NVS_NOT_ENOUGH_SPACE:ESP_OK;
}
static int after_write(void) {
    if(after_mutation&&operations==cut_at)longjmp(power_cut,1);
    return after_mutation&&operations==fail_at ? ESP_FAIL:ESP_OK;
}
int nvs_open(const char *name,int mode,nvs_handle_t *h) {
    if(open_error)return open_error;
    *h=!strcmp(name,"nuwax_reprov")?1:!strcmp(name,"nuwax_cloud")?2:3;
    return ESP_OK;
}
void nvs_close(nvs_handle_t h) {}
int nvs_get_blob(int ns,const char *key,void *p,size_t *size) {
    if(read_error_key&&!strcmp(key,read_error_key))return ESP_FAIL;
    slot_t *s=slot(ns,key,false);
    if(!s)return ESP_ERR_NVS_NOT_FOUND;
    if(p&&*size<s->size){*size=s->size;return ESP_ERR_NVS_INVALID_LENGTH;}
    if(p)memcpy(p,s->value,s->size);
    *size=s->size;return ESP_OK;
}
int nvs_set_blob(int ns,const char *key,const void *p,size_t size) {
    int err=before_write();if(err)return err;
    slot_t *s=slot(ns,key,true);assert(size<=sizeof(s->value));
    s->size=size;memcpy(s->value,p,size);return after_write();
}
int nvs_erase_key(int ns,const char *key) {
    int err=before_write();if(err)return err;
    slot_t *s=slot(ns,key,false);bool found=s!=NULL;
    if(s)memset(s,0,sizeof(*s));
    err=after_write();return err?err:found?ESP_OK:ESP_ERR_NVS_NOT_FOUND;
}
int nvs_commit(int ns) {int err=before_write();return err?err:after_write();}
int esp_wifi_set_storage(int mode) {return ESP_OK;}
int esp_wifi_set_config(int iface,const wifi_config_t *c) {
    int err=nvs_set_blob(3,"driver",c,sizeof(*c));
    slot_t *s=slot(3,"driver",false);if(s)memcpy(&wifi,s->value,sizeof(wifi));
    return err;
}
int esp_wifi_get_config(int iface,wifi_config_t *c) {*c=wifi;return ESP_OK;}
int nuwax_wifi_import_driver_credentials(void) {
    int err=nvs_set_blob(3,"ssid",wifi.sta.ssid,sizeof(wifi.sta.ssid));
    if(!err)err=nvs_set_blob(3,"password",wifi.sta.password,sizeof(wifi.sta.password));
    if(!err)err=nvs_commit(3);
    return err;
}
bool nuwax_wifi_saved_credentials_match(const char *ssid,const char *password) {
    slot_t *s=slot(3,"ssid",false),*p=slot(3,"password",false);
    return s&&p&&!strncmp((const char *)s->value,ssid,32)&&
           !strncmp((const char *)p->value,password,64);
}
int nuwax_device_uid_init(void) {return ESP_OK;}
const char *nuwax_device_uid_string(void) {return "NX-TEST-DEVICE";}
uint32_t esp_crc32_le(uint32_t crc,const uint8_t *p,uint32_t n) {
    crc=~crc;
    while(n--){crc^=*p++;for(int i=0;i<8;i++)crc=(crc>>1)^((crc&1)?0xedb88320U:0);}
    return ~crc;
}
uint32_t esp_random(void) {return 0x12345678U;}
static int esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap) {
    memset(ap,0,sizeof(*ap));memcpy(ap->ssid,wifi.sta.ssid,sizeof(wifi.sta.ssid));
    return connected?ESP_OK:ESP_FAIL;
}
static bool nuwax_wifi_is_connected(void) {return connected;}
static int esp_wifi_disconnect(void) {connected=false;return ESP_OK;}
static int esp_wifi_connect(void) {connected=true;return ESP_OK;}
static bool wait_for_wifi_connection_to(const wifi_config_t *c,uint32_t t) {return connected;}
static int64_t esp_timer_get_time(void) {return clock_us;}
static void vTaskDelay(unsigned ms) {clock_us+=(int64_t)ms*1000;}
static int nuwax_wake_word_detector_begin_flash_operation(unsigned ms) {return ESP_OK;}
static void nuwax_wake_word_detector_end_flash_operation(void) {}
static bool nuwax_provision_main_is_active(void) {return true;}
static provision_state_t get_state_snapshot(void) {return s_state;}
static void set_reason_locked(const char *reason) {}
/* PRODUCTION_FUNCTIONS */

static void reset_ram(void) {
    atomic_store(&s_force_reprovision,false);
    atomic_store(&s_binding_required,false);atomic_store(&s_binding_rejection_durable,false);
    atomic_store(&s_cancel_state,PROVISION_CANCEL_DISABLED);
    memset(&s_reprovision_record,0,sizeof(s_reprovision_record));s_reprovision_record_durable=false;
    memset(&s_state,0,sizeof(s_state));s_provisioned_pair_ready=false;
    memset(&s_target,0,sizeof(s_target));atomic_store(&s_commit_pending,false);
    atomic_store(&s_repair_required,false);
    cloud_initialized=s_has_config=s_has_pending=false;
    memset(&s_config,0,sizeof(s_config));memset(&s_pending,0,sizeof(s_pending));
    slot_t *d=slot(3,"driver",false);memset(&wifi,0,sizeof(wifi));
    if(d)memcpy(&wifi,d->value,sizeof(wifi));
    connected=true;clock_us=0;
}
static void clear_faults(void) {
    cut_at=fail_at=open_error=operations=0;read_error_key=NULL;
}
static void app_candidate(void) {
    assert(nuwax_cloud_config_stage(&target_cloud)==ESP_OK);
    assert(esp_wifi_set_config(0,&target_wifi)==ESP_OK);
    s_state.wifi_candidate_hash_valid=true;
    s_state.wifi_candidate_hash=wifi_credentials_hash(&wifi);
    s_state.owner_generation=s_state.wifi_attempt_owner_generation=7;
    s_state.new_wifi_attempt_active=true;connected=true;
}
static void fresh(int scenario) {
    clear_faults();memset(disk,0,sizeof(disk));reset_ram();
    memset(&old_wifi,0,sizeof(old_wifi));strcpy((char *)old_wifi.sta.ssid,"old-ap");
    strcpy((char *)old_wifi.sta.password,"old-password");target_wifi=old_wifi;
    if(scenario!=1){strcpy((char *)target_wifi.sta.ssid,"new-ap");strcpy((char *)target_wifi.sta.password,"new-password");}
    memset(&old_cloud,0,sizeof(old_cloud));old_cloud.version=1;
    strcpy(old_cloud.device_id,nuwax_device_uid_string());
    strcpy(old_cloud.cloud_url,"wss://cloud.example/ws");strcpy(old_cloud.device_secret,"old-key");
    target_cloud=old_cloud;if(scenario!=4)strcpy(target_cloud.device_secret,"new-key");
    if(scenario!=3) {
        assert(esp_wifi_set_config(0,&old_wifi)==ESP_OK);
        assert(nuwax_wifi_import_driver_credentials()==ESP_OK);
        assert(nuwax_cloud_config_set(&old_cloud)==ESP_OK);
        assert(write_wifi_verified_marker(&old_wifi)==ESP_OK);
        if(scenario==1||scenario==2)nuwax_provision_main_note_binding_rejected();
        assert(nuwax_provision_main_reset_wifi_credentials()==ESP_OK);
    }
    app_candidate();clear_faults();
}
static bool cloud_equals(const nuwax_cloud_config_t *c) {
    slot_t *s=slot(2,"active",false);return s&&s->size==sizeof(*c)&&!memcmp(s->value,c,sizeof(*c));
}
static void assert_target(void) {
    assert(cloud_equals(&target_cloud));
    assert(!memcmp(&wifi,&target_wifi,sizeof(wifi)));
    assert(nuwax_wifi_saved_credentials_match((char *)target_wifi.sta.ssid,(char *)target_wifi.sta.password));
    assert(wifi_verified_marker_matches(&target_wifi));
    assert(!slot(1,"request",false)&&!slot(1,"commit_pending",false)&&!slot(2,"pending",false));
    assert(!nuwax_provision_main_binding_required()&&!nuwax_pair_commit_blocks_rollback());
}
static int boot_and_sanitize(void) {
    reset_ram();
    int err=nuwax_cloud_config_init();if(err)return err;
    err=recover_provisioned_pair();if(err)return err;
    if(force_reprovision_requested()||!nuwax_cloud_config_has_config()) {
        err=discard_uncommitted_pair_candidate();if(err)return err;
        if(!previous_pair_unusable()&&reprovision_record_can_restore(&s_reprovision_record)) {
            return finalize_old_wifi_recovery();
        }
    }
    return ESP_OK;
}
static void assert_old_or_empty(int scenario) {
    assert(!slot(1,"commit_pending",false));
    if(scenario==3) {
        assert(!slot(2,"active",false)&&wifi.sta.ssid[0]==0);
    } else {
        assert(cloud_equals(&old_cloud));assert(!memcmp(&wifi,&old_wifi,sizeof(wifi)));
        if(scenario==1||scenario==2)assert(nuwax_provision_main_binding_required());
    }
}
static bool cut_finalizer(int step,int after) {
    operations=0;cut_at=step;after_mutation=after;
    bool cut=false;
    if(setjmp(power_cut))cut=true;
    else assert(finalize_new_provisioning()==ESP_OK);
    clear_faults();return cut;
}
static bool cut_boot(int step,int after) {
    operations=0;cut_at=step;after_mutation=after;
    bool cut=false;
    if(setjmp(power_cut))cut=true;
    else assert(boot_and_sanitize()==ESP_OK);
    clear_faults();return cut;
}
int main(void) {
    unsigned cases=0;
    fresh(2);assert(finalize_new_provisioning()==ESP_OK);int steps=operations;
    assert_target();assert(steps>=12);
    for(int scenario=0;scenario<5;scenario++)for(int after=0;after<2;after++) {
        for(int first=1;first<=steps;first++) {
            fresh(scenario);assert(cut_finalizer(first,after));
            bool has_intent=slot(1,"commit_pending",false)!=NULL;
            bool finished=!has_intent&&cloud_equals(&target_cloud)&&
                          !slot(1,"request",false)&&!slot(2,"pending",false);
            slot_t snapshot[16];memcpy(snapshot,disk,sizeof(disk));
            for(int second_after=0;second_after<2;second_after++) {
                for(int second=1;second<=steps+2;second++) {
                    memcpy(disk,snapshot,sizeof(disk));
                    (void)cut_boot(second,second_after);
                    /* Exercise a third interrupted boot at a varying boundary. */
                    (void)cut_boot((first+second)%(steps+2)+1,after);
                    assert(boot_and_sanitize()==ESP_OK);
                    if(has_intent||finished)assert_target();else assert_old_or_empty(scenario);
                    assert(boot_and_sanitize()==ESP_OK);
                    if(has_intent||finished)assert_target();else assert_old_or_empty(scenario);
                    cases++;
                }
            }
        }
    }
    printf("PASS: %u repeated-cut cases (same/different Wi-Fi, rejected/manual/initial pair, unchanged secret); each followed by two clean boots\n",cases);

    for(int after=0;after<2;after++)for(int step=1;step<=steps;step++) {
        fresh(2);fail_at=step;after_mutation=after;
        assert(finalize_new_provisioning()!=ESP_OK);
        assert(nuwax_pair_commit_pending()&&nuwax_provision_main_binding_required());
        clear_faults();int before=operations;
        assert(!nuwax_provision_main_request_runtime_cancel());
        assert(discard_uncommitted_pair_candidate()==ESP_ERR_INVALID_STATE);
        assert(finalize_old_wifi_recovery()==ESP_ERR_INVALID_STATE);
        assert(nuwax_provision_main_rollback_wifi_credentials()==ESP_ERR_INVALID_STATE);
        assert(nuwax_provision_main_commit_wifi_credentials()==ESP_ERR_INVALID_STATE);
        assert(nuwax_provision_main_reset_wifi_credentials()==ESP_ERR_INVALID_STATE);
        assert(operations==before);
        connected=false;memset(&s_state,0,sizeof(s_state));
        assert(finalize_new_provisioning()==ESP_OK);assert_target();
    }
    puts("PASS: every persistence error before/after mutation retains rejection; cancellation, cleanup, rollback and new requests cannot replace the target; retry works after IP/APP loss");

    fresh(2);read_error_key="active";assert(finalize_new_provisioning()!=ESP_OK);
    assert(nuwax_provision_main_binding_required()&&slot(1,"commit_pending",false));
    read_error_key=NULL;assert(finalize_new_provisioning()==ESP_OK);assert_target();
    puts("PASS: failed flash readback cannot publish success or erase journal");

    for(int legacy=0;legacy<4;legacy++) {
        fresh(legacy%2);uint32_t old_marker[2]={0x4e585643U,wifi_credentials_hash(&wifi)};
        if(legacy<2)assert(nvs_set_blob(1,"commit_pending",old_marker,sizeof(old_marker))==ESP_OK);
        else {
            assert(nuwax_pair_commit_begin(&wifi,&target_cloud)==ESP_OK);
            assert(persist_target()==ESP_OK);
            slot(1,"commit_pending",false)->value[0]^=1;
        }
        for(int boot=0;boot<3;boot++) {
            assert(boot_and_sanitize()==ESP_OK);
            assert(nuwax_pair_commit_requires_app()&&slot(1,"commit_pending",false));
            assert(!nuwax_provision_main_request_runtime_cancel());
            assert(finalize_old_wifi_recovery()==ESP_ERR_INVALID_STATE);
            assert(cloud_equals(&old_cloud));
        }
        app_candidate();assert(finalize_new_provisioning()==ESP_OK);assert_target();
    }
    puts("PASS: legacy/corrupt Wi-Fi-only proof survives repeated cleanup/restarts and requires a complete fresh APP pair");

    for(int bad=0;bad<2;bad++) {
        fresh(0);slot_t *r=slot(1,"request",false);assert(r);
        if(bad)r->size=sizeof(r->value);else r->value[0]^=1;
        for(int boot=0;boot<3;boot++) {
            assert(boot_and_sanitize()==ESP_OK);
            assert(nuwax_pair_commit_requires_app()&&slot(1,"request",false));
            assert(!nuwax_provision_main_request_runtime_cancel());
        }
        app_candidate();assert(finalize_new_provisioning()==ESP_OK);assert_target();
    }
    puts("PASS: incompatible/corrupt rollback records enter APP repair, retain evidence, and recover without a reset loop");

    for(int after=0;after<2;after++)for(int step=1;step<=2;step++) {
        fresh(0);slot(1,"request",false)->value[0]^=1;
        reset_ram();operations=0;fail_at=step;after_mutation=after;
        assert(recover_provisioned_pair()!=ESP_OK);
        assert(slot(1,"request",false));clear_faults();
        assert(boot_and_sanitize()==ESP_OK);assert(nuwax_pair_commit_requires_app());
        app_candidate();assert(finalize_new_provisioning()==ESP_OK);assert_target();
    }
    puts("PASS: repair-fence persistence errors retain invalid rollback evidence and recover on next boot");

    fresh(0);fail_at=3;operations=0;after_mutation=0;
    assert(nuwax_cloud_config_commit_pending()!=ESP_OK);
    assert(nuwax_cloud_config_has_pending()&&!slot(2,"pending",false));
    clear_faults();assert(nuwax_cloud_config_commit_pending()==ESP_OK);
    assert(nuwax_cloud_config_verify_persisted(&target_cloud)==ESP_OK);
    fresh(0);slot(2,"pending",false)->value[0]^=1;reset_ram();read_error_key="active";
    assert(nuwax_cloud_config_init()!=ESP_OK);read_error_key=NULL;
    puts("PASS: cloud pending erase is idempotent; unrelated cleanup cannot mask an active-slot read failure");

    fresh(0);assert(nuwax_provision_main_request_runtime_cancel());
    assert(provisioning_cancel_requested());assert(finalize_new_provisioning()==ESP_ERR_INVALID_STATE);
    assert(nuwax_provision_main_rollback_wifi_credentials()==ESP_OK);
    assert_old_or_empty(0);assert(!slot(1,"request",false));
    assert(boot_and_sanitize()==ESP_OK);assert(!force_reprovision_requested());
    fresh(2);assert(boot_and_sanitize()==ESP_OK);
    assert(nuwax_provision_main_binding_required());
    assert(!nuwax_provision_main_request_runtime_cancel());
    puts("PASS: healthy manual cancel still restores old pair; rejected binding remains rejected without commit intent");

    fresh(0);assert(finalize_new_provisioning()==ESP_OK);assert_target();
    assert(boot_and_sanitize()==ESP_OK);assert_target();
    nuwax_provision_main_note_binding_rejected();assert(nuwax_provision_main_reset_wifi_credentials()==ESP_OK);
    assert(boot_and_sanitize()==ESP_OK);assert(nuwax_provision_main_binding_required());
    assert(!slot(1,"commit_pending",false));
    puts("PASS: completed pair boots normally; a later rejection cannot reuse old proof");

    fresh(2);assert(nuwax_pair_commit_begin(&wifi,&target_cloud)==ESP_OK);assert(persist_target()==ESP_OK);
    clear_faults();reset_ram();read_error_key="commit_pending";
    assert(recover_provisioned_pair()!=ESP_OK);assert(slot(1,"commit_pending",false));
    read_error_key=NULL;assert(boot_and_sanitize()==ESP_OK);assert_target();
    puts("PASS: journal read failure preserves recovery evidence");
    return 0;
}
