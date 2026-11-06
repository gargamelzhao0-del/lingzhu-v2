#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <setjmp.h>
#include "nuwax_provision_policy.h"
typedef int esp_err_t;
typedef int nvs_handle_t;
typedef struct { struct { uint8_t ssid[32], password[64]; } sta; } wifi_config_t;
typedef struct { uint8_t ssid[32]; } wifi_ap_record_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_NOT_ENOUGH_SPACE 0x1105
#define ESP_ERR_WIFI_NOT_CONNECT 0x300f
#define ESP_ERR_WIFI_STATE 0x3006
#define ESP_ERR_WIFI_CONN 0x3007
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define WIFI_IF_STA 0
#define WIFI_STORAGE_FLASH 1
#define PROVISION_NVS_FLASH_GUARD_TIMEOUT_MS 100
#define CONFIG_NUWAX_PROVISION_RUNTIME_IDLE_TIMEOUT_MS 600000
#define ROLLBACK_CONNECT_TIMEOUT_MS 10000
#define NUWAX_PROVISION_OUTCOME_SUCCESS 1
#define NUWAX_PROVISION_OUTCOME_OLD_WIFI_RECOVERED 9
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define TAG "test"
#define ESP_RETURN_ON_ERROR(expr, ...) do {int e_=(expr);if(e_!=ESP_OK)return e_;}while(0)
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
#define pdMS_TO_TICKS(t) (t)
/* PRODUCTION_TYPES */
static bool s_provisioned_pair_ready, s_has_staged_vox_config;
static char s_staged_vox_config[16];
static int s_state_lock;
typedef struct {
    bool wifi_candidate_hash_valid, new_wifi_attempt_active, failed_attempt_release_pending;
    uint32_t wifi_candidate_hash, wifi_attempt_owner_generation, owner_generation;
    int64_t failed_attempt_release_deadline_us, credentials_received_us, runtime_activity_us;
    int outcome;
} provision_state_t;
static provision_state_t s_state;
static wifi_config_t wifi, saved_wifi, verified_wifi, commit_wifi;
static bool connected, verified_exists, commit_exists, request_exists, pending;
static unsigned char request_bytes[sizeof(reprovision_record_t)];
static int active_key, replacement_key, nvs_error, commit_error, marker_error, writes, cut_at;
static int64_t clock_us;
static jmp_buf power_cut;
static void durable_step(void){if(++writes==cut_at)longjmp(power_cut,1);}
static int nvs_open(const char *ns,int mode,nvs_handle_t *h){*h=1;return nvs_error;}
static void nvs_close(nvs_handle_t h){}
static int nvs_get_blob(nvs_handle_t h,const char *key,void *p,size_t *n){
    assert(!strcmp(key,REPROVISION_NVS_KEY));
    if(!request_exists)return ESP_ERR_NVS_NOT_FOUND;
    assert(*n>=sizeof(request_bytes));memcpy(p,request_bytes,sizeof(request_bytes));*n=sizeof(request_bytes);return 0;
}
static int nvs_set_blob(nvs_handle_t h,const char *key,const void *p,size_t n){
    assert(!strcmp(key,REPROVISION_NVS_KEY)&&n==sizeof(request_bytes));
    memcpy(request_bytes,p,n);request_exists=true;durable_step();return 0;
}
static int nvs_erase_key(nvs_handle_t h,const char *key){
    if(!strcmp(key,REPROVISION_NVS_KEY))request_exists=false;
    else {assert(!strcmp(key,WIFI_COMMIT_NVS_KEY));commit_exists=false;}
    durable_step();return 0;
}
static int nvs_commit(nvs_handle_t h){durable_step();return 0;}
static bool nuwax_cloud_config_has_config(void){return active_key!=0;}
static bool nuwax_cloud_config_has_pending(void){return pending;}
static int nuwax_cloud_config_commit_pending(void){
    if(commit_error)return commit_error;
    if(pending){active_key=replacement_key;durable_step();pending=false;durable_step();}
    return 0;
}
static int nuwax_cloud_config_discard_pending(void){pending=false;durable_step();return 0;}
static int nuwax_wifi_import_driver_credentials(void){saved_wifi=wifi;durable_step();return 0;}
static bool nuwax_wifi_saved_credentials_match(const char *s,const char *p){
    return !strcmp(s,(const char *)saved_wifi.sta.ssid)&&!strcmp(p,(const char *)saved_wifi.sta.password);
}
static bool nuwax_wifi_is_connected(void){return connected;}
static int esp_wifi_get_config(int i,wifi_config_t *p){*p=wifi;return 0;}
static int esp_wifi_sta_get_ap_info(wifi_ap_record_t *p){memcpy(p->ssid,wifi.sta.ssid,sizeof(p->ssid));return connected?0:ESP_FAIL;}
static int esp_wifi_set_storage(int s){return 0;}
static int esp_wifi_set_config(int i,const wifi_config_t *p){wifi=*p;durable_step();return 0;}
static int esp_wifi_disconnect(void){connected=false;return 0;}
static int esp_wifi_connect(void){connected=true;return 0;}
static int64_t esp_timer_get_time(void){return clock_us;}
static void vTaskDelay(unsigned ms){clock_us+=(int64_t)ms*1000;}
static bool wait_for_wifi_connection_to(const wifi_config_t *c,uint32_t ms){return connected;}
static uint32_t wifi_credentials_hash(const wifi_config_t *c){
    return nuwax_provision_credentials_hash(c->sta.ssid,sizeof(c->sta.ssid),c->sta.password,sizeof(c->sta.password));
}
static bool wifi_verified_marker_matches(const wifi_config_t *c){return verified_exists&&!memcmp(c,&verified_wifi,sizeof(*c));}
static bool wifi_commit_marker_matches(const wifi_config_t *c){return commit_exists&&!memcmp(c,&commit_wifi,sizeof(*c));}
static int write_wifi_verified_marker(const wifi_config_t *c){
    if(marker_error)return marker_error;
    verified_wifi=*c;verified_exists=true;durable_step();return 0;
}
static int retry_write_wifi_verified_marker(const wifi_config_t *c){return write_wifi_verified_marker(c);}
static int clear_wifi_verified_marker(void){verified_exists=false;durable_step();return 0;}
static int write_wifi_commit_marker(const wifi_config_t *c){commit_wifi=*c;commit_exists=true;durable_step();return 0;}
static int retry_persist_operation(int (*op)(void),const char *label){return op();}
static int nuwax_wake_word_detector_begin_flash_operation(unsigned ms){return 0;}
static void nuwax_wake_word_detector_end_flash_operation(void){}
static bool nuwax_provision_main_is_active(void){return true;}
static provision_state_t get_state_snapshot(void){return s_state;}
static void set_reason_locked(const char *reason){}
/* PRODUCTION_FUNCTIONS */
static void reset_ram(void){
    atomic_store(&s_force_reprovision,false);
    atomic_store(&s_binding_required,false);atomic_store(&s_binding_rejection_durable,false);
    atomic_store(&s_cancel_state,PROVISION_CANCEL_DISABLED);
    memset(&s_reprovision_record,0,sizeof(s_reprovision_record));
    s_reprovision_record_durable=false;s_provisioned_pair_ready=false;
    memset(&s_state,0,sizeof(s_state));
}
static void fresh(void){
    reset_ram();request_exists=commit_exists=pending=false;
    nvs_error=commit_error=marker_error=writes=cut_at=0;clock_us=0;
    memset(&wifi,0,sizeof(wifi));strcpy((char *)wifi.sta.ssid,"old-ap");
    strcpy((char *)wifi.sta.password,"test-password");saved_wifi=verified_wifi=wifi;
    verified_exists=connected=true;active_key=1;replacement_key=2;
}
static void prepare_new_pair(void){
    strcpy((char *)wifi.sta.ssid,"new-ap");pending=true;
    s_state.wifi_candidate_hash_valid=true;s_state.wifi_candidate_hash=wifi_credentials_hash(&wifi);
    s_state.owner_generation=s_state.wifi_attempt_owner_generation=7;
    s_state.new_wifi_attempt_active=true;
}
static void rejected_record(void){
    nuwax_provision_main_note_binding_rejected();
    assert(nuwax_provision_main_reset_wifi_credentials()==0);
    assert(s_reprovision_record_durable && atomic_load(&s_binding_rejection_durable));
}
int main(void){
    fresh();assert(nuwax_provision_main_reset_wifi_credentials()==0);
    assert(old_wifi_allowed(true));assert(nuwax_provision_main_request_runtime_cancel());
    assert(provisioning_cancel_requested());
    assert(nuwax_provision_main_rollback_wifi_credentials()==0);
    assert(!request_exists && active_key==1);
    fresh();rejected_record();reset_ram();assert(load_reprovision_record()==0);
    assert(nuwax_provision_main_binding_required());
    assert(!old_wifi_allowed(true) && !old_wifi_allowed(false));
    assert(!runtime_timeout(true) && !runtime_timeout(false));
    assert(!nuwax_provision_main_request_runtime_cancel() && !provisioning_cancel_requested());
    int before=writes;
    assert(finalize_old_wifi_recovery()==ESP_ERR_INVALID_STATE);
    assert(nuwax_provision_main_rollback_wifi_credentials()==ESP_ERR_INVALID_STATE);
    assert(nuwax_provision_main_commit_wifi_credentials()==ESP_ERR_INVALID_STATE);
    assert(writes==before && request_exists && active_key==1);
    assert(clear_reprovision_record()==0);
    assert(nuwax_provision_main_binding_required());
    puts("PASS: manual rollback preserved; rejected binding survives restart and blocks old-Wi-Fi/cancel/idle/false commit");

    fresh();nuwax_provision_main_note_binding_rejected();nvs_error=ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    assert(nuwax_provision_main_reset_wifi_credentials()!=0);
    assert(nuwax_provision_main_binding_required() && !atomic_load(&s_binding_rejection_durable));
    nvs_error=0;assert(nuwax_provision_main_reset_wifi_credentials()==0);
    prepare_new_pair();commit_error=ESP_FAIL;
    assert(finalize_new_provisioning()!=0 && nuwax_provision_main_binding_required());
    commit_error=0;assert(finalize_new_provisioning()==0);
    assert(active_key==2 && !pending && !request_exists && !nuwax_provision_main_binding_required());
    puts("PASS: NVS exhaustion retains rejection; failed APP commit cannot release it; successful replacement commits normally");

    /* A new rejection must not reuse an older transaction's commit proof. */
    for(int cut=1;cut<=3;cut++){
        fresh();rejected_record();commit_wifi=wifi;commit_exists=true;
        nuwax_provision_main_note_binding_rejected();writes=0;cut_at=cut;
        if(!setjmp(power_cut)) (void)nuwax_provision_main_reset_wifi_credentials();
        cut_at=0;reset_ram();assert(load_reprovision_record()==0);
        assert(nuwax_provision_main_binding_required() && !commit_exists);
        assert(recover_commit()!=0 && !old_wifi_allowed(true));
    }
    puts("PASS: new rejection invalidates older commit proof before durable request; reset at every NVS write cannot resurrect old binding");

    /* Cut at each individually durable step of the real APP finalizer. */
    fresh();rejected_record();prepare_new_pair();writes=0;
    assert(finalize_new_provisioning()==0);int steps=writes;
    for(int cut=1;cut<=steps;cut++){
        fresh();rejected_record();prepare_new_pair();writes=0;cut_at=cut;
        if(!setjmp(power_cut)) (void)finalize_new_provisioning();
        cut_at=0;reset_ram();assert(load_reprovision_record()==0);
        (void)recover_commit();
        if(nuwax_provision_main_binding_required()){
            assert(!old_wifi_allowed(true));
        }else{
            assert(active_key==2 && !pending);
            assert(!strcmp((const char *)saved_wifi.sta.ssid,"new-ap"));
        }
    }
    puts("PASS: APP commit power cuts never release rejected credentials or combine old Wi-Fi with a new key");

    fresh();rejected_record();prepare_new_pair();marker_error=ESP_FAIL;
    assert(finalize_new_provisioning()==0);
    assert(active_key==2 && !pending && request_exists && commit_exists);
    marker_error=0;reset_ram();assert(load_reprovision_record()==0);
    assert(recover_commit()==0 && !nuwax_provision_main_binding_required());
    assert(wifi_verified_marker_matches(&wifi) && !request_exists);
    puts("PASS: deferred Wi-Fi verification retains commit proof and completes on reboot without re-entering BLE");

    fresh();rejected_record();prepare_new_pair();marker_error=ESP_FAIL;
    assert(finalize_new_provisioning()==0 && !force_reprovision_requested());
    marker_error=0;assert(nuwax_provision_main_reset_wifi_credentials()==0);
    assert(!strcmp((char *)s_reprovision_record.wifi_config.sta.ssid,"new-ap"));
    assert(!commit_exists && !nuwax_provision_main_binding_required());
    assert(nuwax_provision_main_rollback_wifi_credentials()==0);
    assert(!strcmp((char *)wifi.sta.ssid,"new-ap") && active_key==2);
    puts("PASS: later manual rebind cannot reuse a completed transaction's old Wi-Fi snapshot");
    return 0;
}
