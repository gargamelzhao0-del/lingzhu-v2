#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "cloud_connection_epoch_policy.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 0x103
#define NUWAX_WS_CLOSE_REASON_PREVIEW_BYTES 96U
#define CONFIG_NUWAX_OTA_ENABLE 0
#define NUWAX_CLOUD_AUTHFAIL 2
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
typedef struct {
    uint32_t epoch;
    atomic_bool connected;
    void (*on_peer_close)(uint32_t,uint16_t,const uint8_t *,size_t,void *);
    void *user_ctx;
} nuwax_ws_client_t;
static int s_cloud_auth_lock;
static bool s_cloud_service_ready, s_cloud_authenticated, s_cloud_auth_failed_latched;
static uint32_t s_cloud_connection_epoch, s_cloud_provision_required_epoch, now_ms;
static bool binding_required,s_cloud_http_rebind_pending,admit;
static atomic_bool s_reprovision_quiesce_requested;
static unsigned requests;
static unsigned close_echoes;
static void websocket_close_reason_preview(const uint8_t *p,size_t n,char *s,size_t cap){s[0]=0;}
static void report_ws_warning(const char *r,int e){}
static int send_frame(nuwax_ws_client_t *c,int opcode,const uint8_t *p,size_t n){
    assert(opcode==8 && n==24 && p[0]==3 && p[1]==240);++close_echoes;return 0;
}
static uint32_t nuwax_ws_connection_epoch(nuwax_ws_client_t *w){return w->epoch;}
static void nuwax_provision_main_note_binding_rejected(void){binding_required=true;}
static bool nuwax_provision_main_binding_required(void){return binding_required;}
static void nuwax_app_shell_set_cloud_state(int s){}
static uint32_t cloud_provision_required_epoch(void){return s_cloud_provision_required_epoch;}
static bool consume_cloud_provision_required(uint32_t e){return nuwax_cloud_epoch_consume_provision_required(e,&s_cloud_provision_required_epoch);}
static uint32_t monotonic_ms(void){return now_ms;}
static bool elapsed_ms(uint32_t now,uint32_t then){return (int32_t)(now-then)>=0;}
static bool queue_ble_reprovision_request(const char *s){
    ++requests;if(admit)atomic_store(&s_reprovision_quiesce_requested,true);return admit;
}
/* PRODUCTION_FUNCTIONS */
int main(void){
    const uint8_t *reason=(const uint8_t *)"device binding changed";
    assert(nuwax_cloud_peer_close_requires_provision(1008,reason,22));
    assert(!nuwax_cloud_peer_close_requires_provision(1000,reason,22));
    assert(!nuwax_cloud_peer_close_requires_provision(1008,reason,21));
    assert(!nuwax_cloud_peer_close_requires_provision(1008,reason,23));
    assert(!nuwax_cloud_peer_close_requires_provision(1008,NULL,22));
    uint8_t bad[22];memcpy(bad,reason,22);bad[8]=0;
    assert(!nuwax_cloud_peer_close_requires_provision(1008,bad,22));
    nuwax_ws_client_t ws={.epoch=5};s_cloud_connection_epoch=5;
    on_ws_peer_close(4,1008,reason,22,&ws);assert(!binding_required);
    ws.epoch=4;on_ws_peer_close(4,1008,reason,22,&ws);assert(!binding_required);
    ws.epoch=5;on_ws_peer_close(5,1008,reason,22,&ws);
    assert(binding_required && s_cloud_provision_required_epoch==5 && requests==0);
    assert(!nuwax_cloud_epoch_accepts_positive(5,5,s_cloud_provision_required_epoch));
    assert(cloud_connect_cancelled_for_reprovision(NULL));
    now_ms=10000;admit=false;
    assert(service_cloud_binding_reprovision() && requests==1);
    assert(service_cloud_binding_reprovision() && requests==1);
    now_ms+=1000;admit=true;
    assert(service_cloud_binding_reprovision() && requests==2);
    assert(s_cloud_provision_required_epoch==0 && binding_required);
    /* Persistent rejection must not starve the drain/park branch. */
    assert(!service_cloud_binding_reprovision() && requests==2);
    atomic_store(&s_reprovision_quiesce_requested,false);admit=false;now_ms+=1000;
    assert(service_cloud_binding_reprovision() && requests==3);
    binding_required=false;s_cloud_http_rebind_pending=true;s_cloud_provision_required_epoch=5;
    assert(!service_cloud_binding_reprovision());
    assert(!s_cloud_provision_required_epoch && !s_cloud_http_rebind_pending && requests==3);
    assert(!cloud_connect_cancelled_for_reprovision(NULL));
    uint8_t frame[24]={3,240};memcpy(frame+2,reason,22);
    ws.on_peer_close=on_ws_peer_close;ws.user_ctx=&ws;atomic_store(&ws.connected,true);
    assert(!strcmp(receive_close(&ws,frame,24),"peer_close"));
    assert(binding_required && s_cloud_provision_required_epoch==5 && close_echoes==1 && requests==3);
    binding_required=false;s_cloud_provision_required_epoch=0;ws.on_peer_close=NULL;
    assert(!strcmp(receive_close(&ws,frame,24),"peer_close"));
    assert(!binding_required && close_echoes==2); /* Dedicated gateway has no binding callback. */
    puts("PASS: exact close reason/current epoch only; callback publishes no transaction; failed admission retries; joined owner can drain; replacement releases reconnect");
    return 0;
}
