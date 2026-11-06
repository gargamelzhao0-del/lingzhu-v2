#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include "esp_err.h"
#include "offline_replay_policy.h"
#define CONFIG_NUWAX_SD_SESSION_ENABLE 1
#define CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE 1
#define REPLAY_ACK_BIT 4U
#define REPLAY_CHUNK_ACK_BIT 8U
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(tag,fmt,...) ((void)printf(fmt, __VA_ARGS__))
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(x) (x)
typedef struct {bool connected;uint32_t epoch;} nuwax_ws_client_t;
typedef unsigned EventBits_t;
static uint32_t now, arrive, stop;
static uint32_t ota_at, monitor_at, cancel_at, disconnect_at, epoch_at;
static nuwax_ws_client_t *active_ws;
static atomic_bool s_card_online, s_formal_replay_cancel_requested;
static struct { int events; struct {
    bool waiting, ok, chunk; uint32_t epoch, next_offset, received_ms;
    char expected_type[40], request_id[48];
} replay_ack; } s;
static void replay_disarm_ack(void){s.replay_ack.waiting=false;}
static uint32_t offline_now_ms(void){return now;}
static bool offline_deadline_elapsed(uint32_t t,uint32_t d){return (int32_t)(t-d)>=0;}
static bool nuwax_offline_audio_storage_busy(void){return ota_at && now>=ota_at;}
static bool nuwax_sd_session_active(void){return monitor_at && now>=monitor_at;}
static bool nuwax_ws_is_connected(nuwax_ws_client_t *ws){return ws->connected;}
static uint32_t nuwax_ws_connection_epoch(nuwax_ws_client_t *ws){return ws->epoch;}
static EventBits_t xEventGroupWaitBits(int group,EventBits_t bits,int clear,int all,unsigned delay){
    (void)group;(void)clear;(void)all;assert(delay==100);now+=delay;
    if(stop && now>=stop)atomic_store(&s_card_online,false);
    if(cancel_at && now>=cancel_at)atomic_store(&s_formal_replay_cancel_requested,true);
    if(disconnect_at && now>=disconnect_at)active_ws->connected=false;
    if(epoch_at && now>=epoch_at)active_ws->epoch=8;
    return arrive && now>=arrive?bits:0;
}
#include "ack_wait.inc"
static void arm(const char *type,bool chunk){
    now=arrive=stop=ota_at=monitor_at=cancel_at=disconnect_at=epoch_at=0;
    atomic_store(&s_card_online,true);atomic_store(&s_formal_replay_cancel_requested,false);
    *active_ws=(nuwax_ws_client_t){true,7};
    memset(&s,0,sizeof(s));s.replay_ack.waiting=s.replay_ack.ok=true;s.replay_ack.epoch=7;s.replay_ack.chunk=chunk;
    strcpy(s.replay_ack.expected_type,type);strcpy(s.replay_ack.request_id,"req");
}
int main(void){
    nuwax_ws_client_t ws={true,7};active_ws=&ws;
    arm("monitoring_audio_session_end_ack",false);arrive=100;s.replay_ack.ok=false;
    assert(replay_wait_ack(&ws,7,"req",false,false,NULL)==ESP_FAIL && !s.replay_ack.waiting);
    arm("monitoring_audio_session_end_ack",false);arrive=40000;
    assert(replay_wait_ack(&ws,7,"req",false,false,NULL)==ESP_OK && now==40000);
    arm("meeting_audio_session_end_ack",false);
    assert(replay_wait_ack(&ws,7,"req",false,false,NULL)==ESP_ERR_TIMEOUT && now==60000);
    arm("meeting_audio_chunk_ack",true);arrive=40000;
    assert(replay_wait_ack(&ws,7,"req",true,false,NULL)==ESP_ERR_TIMEOUT && now==2500);
    arm("meeting_audio_session_end_ack",false);stop=300;
    assert(replay_wait_ack(&ws,7,"req",false,false,NULL)==ESP_ERR_NOT_FINISHED && now==300 && !s.replay_ack.waiting);
    arm("meeting_audio_session_end_ack",false);ota_at=200;
    assert(replay_wait_ack(&ws,7,"req",false,false,NULL)==ESP_ERR_NOT_FINISHED && now==200 && !s.replay_ack.waiting);
    arm("meeting_audio_commit_ack",false);ota_at=arrive=100;
    assert(replay_wait_ack(&ws,7,"req",false,false,NULL)==ESP_ERR_NOT_FINISHED && now==100 && !s.replay_ack.waiting);
    arm("meeting_audio_chunk_ack",true);ota_at=arrive=100;
    assert(replay_wait_ack(&ws,7,"req",true,false,NULL)==ESP_ERR_NOT_FINISHED && now==100 && !s.replay_ack.waiting);
    const char *types[]={"meeting_audio_begin_ack","meeting_audio_chunk_ack","meeting_audio_commit_ack"};
    for(unsigned kind=0;kind<3;kind++) {
        arm(types[kind],kind==1);arrive=3500;s.replay_ack.next_offset=524288;
        uint32_t offset=123;
        assert(replay_wait_ack(&ws,7,"req",kind==1,true,&offset)==ESP_OK && now==3500 && offset==524288);
        arm(types[kind],kind==1);
        assert(replay_wait_ack(&ws,7,"req",kind==1,true,NULL)==ESP_ERR_TIMEOUT && now==6000);
        arm(types[kind],kind==1);arrive=3500;
        assert(replay_wait_ack(&ws,7,"req",kind==1,false,NULL)==(kind==1?ESP_ERR_TIMEOUT:ESP_OK) && now==(kind==1?2500U:3500U));
        if(kind!=1) {
            arm(types[kind],false);
            assert(replay_wait_ack(&ws,7,"req",false,false,NULL)==ESP_ERR_TIMEOUT && now==6000);
        }
    }
    // Cancellation wins when ACK and transition occur in the same 100ms wake.
    // No offset is exposed, regardless of chunk/control or serial/bulk mode.
    for(unsigned chunk=0;chunk<2;chunk++)for(unsigned bulk=0;bulk<2;bulk++)for(unsigned ack=0;ack<2;ack++)for(unsigned action=0;action<6;action++) {
        arm(chunk?"meeting_audio_chunk_ack":"meeting_audio_commit_ack",chunk);arrive=ack?100:0;
        uint32_t *at[]={&ota_at,&stop,&monitor_at,&cancel_at,&disconnect_at,&epoch_at};
        *at[action]=100;uint32_t offset=123;s.replay_ack.next_offset=524288;
        if(!bulk && action==2) {
            arrive=100;
            assert(replay_wait_ack(&ws,7,"req",chunk,false,&offset)==ESP_OK && offset==524288);
        } else {
            assert(replay_wait_ack(&ws,7,"req",chunk,bulk,&offset)==
                   (action<4?ESP_ERR_NOT_FINISHED:ESP_ERR_INVALID_STATE));
            assert(now==100 && offset==123 && !s.replay_ack.waiting);
        }
    }
    /* A simultaneous local pause cannot hide a dead link or changed epoch. */
    for(unsigned change=0;change<2;change++)for(unsigned ack=0;ack<2;ack++) {
        arm("meeting_audio_commit_ack",false);arrive=ack?100:0;cancel_at=100;
        if(change)epoch_at=100;else disconnect_at=100;
        uint32_t offset=123;s.replay_ack.next_offset=524288;
        assert(replay_wait_ack(&ws,7,"req",false,false,&offset)==ESP_ERR_INVALID_STATE);
        assert(now==100 && offset==123 && !s.replay_ack.waiting);
    }
    arm("meeting_audio_begin_ack",false);atomic_store(&s_formal_replay_cancel_requested,true);
    assert(replay_wait_ack(&ws,7,"req",false,true,NULL)==ESP_ERR_NOT_FINISHED && now==0);
    assert(replay_ack_timeout_ms("meeting_audio_session_end_ack",true)==60000);
    puts("PASS: production ACK wait: historical bulk 3.5s ACK succeeds/6s expires; typed begin/commit always 6s, legacy chunk 2.5s, final 60s; monitoring/OTA/offline/cancel/connection/epoch wins same-wake ACK without offset advance, within 100ms");
}
