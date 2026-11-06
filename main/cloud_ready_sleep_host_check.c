#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#define CONFIG_NUWAX_OTA_ENABLE 1
#define NUWAX_APP_STATE_WS_CONNECTING 1
#define NUWAX_APP_STATE_SLEEPING 2
#define ESP_LOGW(...) ((void)0)
static int state, transitions;
static bool ready, s_awake_resume_after_ws_reconnect, awake, capture, notification, exclusive, ota;
static atomic_bool s_reprovision_quiesce_requested;
static int s_awake_session, s_notification_transaction;
static struct { bool active; } s_turn_guard;
static struct { bool pending, capture_active; } s_prelisten;
static int conversation_state_current(void){return state;}
static bool cloud_service_is_ready(void){return ready;}
static bool nuwax_awake_session_state_is_active(void *p){(void)p;return awake;}
static bool conversation_capture_busy_for_sleep(void){return capture;}
static bool nuwax_notification_blocks_conversation(void *p){(void)p;return notification;}
static bool meeting_or_monitoring_mode_blocks_sleep(void){return exclusive;}
static bool nuwax_ota_device_blocks_audio(void){return ota;}
static void request_sleeping_conversation_state(const char *reason){assert(reason);state=2;++transitions;}
#include "cloud_ready_sleep_host.inc"
int main(void){
    state=1;assert(!reconcile_cloud_ready_sleep_state("ready") && !transitions);
    ready=true;
    bool *guards[]={&s_awake_resume_after_ws_reconnect,&awake,&capture,&notification,&exclusive,&ota,
        &s_turn_guard.active,&s_prelisten.pending,&s_prelisten.capture_active};
    for(unsigned i=0;i<sizeof guards/sizeof guards[0];i++) {
        *guards[i]=true;
        assert(!reconcile_cloud_ready_sleep_state("ready") && !transitions && state==1);
        *guards[i]=false;
    }
    s_reprovision_quiesce_requested=true;assert(!reconcile_cloud_ready_sleep_state("ready"));
    s_reprovision_quiesce_requested=false;
    assert(reconcile_cloud_ready_sleep_state("ready") && transitions==1);
    assert(!reconcile_cloud_ready_sleep_state("again") && transitions==1);
    puts("PASS: ready converges once; awake/turn/prelisten/capture/notification/exclusive/OTA/provisioning guards retained");
}
