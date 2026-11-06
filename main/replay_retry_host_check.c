#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "esp_err.h"
#include "sd_audio_protocol.h"
static unsigned s_replay_route = SD_UPLOAD_LEGACY;
typedef struct { bool connected; uint32_t epoch; } nuwax_ws_client_t;
#define TAG "host"
static unsigned sends, waits, arms, disarms, failures;
static bool busy, disconnect_send, change_epoch, yield_after_ack, fail_send;
static const uint8_t bytes[]={1,2,3,4};
static void log_ignore(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
#define ESP_LOGW(...) log_ignore(__VA_ARGS__)
static bool nuwax_offline_audio_storage_busy(void) { return busy; }
static bool nuwax_ws_is_connected(nuwax_ws_client_t *ws) { return ws->connected; }
static uint32_t nuwax_ws_connection_epoch(nuwax_ws_client_t *ws) { return ws->epoch; }
static esp_err_t replay_arm_ack(uint32_t epoch, const char *request, const char *type, const char *segment) {
    assert(epoch==7 && !strcmp(request,"unchanged-request") && !strcmp(segment,"segment"));
    assert(!strcmp(type,sd_audio_type(s_replay_route,SD_CHUNK_ACK))); arms++; return ESP_OK;
}
static void replay_disarm_ack(void) { disarms++; }
static esp_err_t replay_send_frame(nuwax_ws_client_t *ws, uint32_t epoch,
    const uint8_t *data, size_t size, bool chunk, bool independent) {
    assert(epoch==7 && chunk && independent && size==sizeof(bytes));
    assert(!memcmp(data,bytes,size)); sends++;
    if(disconnect_send) { ws->connected=false; return ESP_ERR_TIMEOUT; }
    if(fail_send && sends<=failures) return ESP_ERR_TIMEOUT;
    return ESP_OK;
}
static esp_err_t replay_wait_ack(nuwax_ws_client_t *ws, uint32_t epoch,
    const char *request, bool chunk, bool bulk_wait, uint32_t *offset) {
    assert(epoch==7 && !strcmp(request,"unchanged-request") && chunk && !bulk_wait); waits++;
    if(change_epoch) ws->epoch++;
    if(yield_after_ack) busy=true;
    if(waits<=failures) return ESP_ERR_TIMEOUT;
    *offset=1234; return ESP_OK;
}
#include "replay_retry_host.inc"
static void reset(void) {
    sends=waits=arms=disarms=failures=0;
    busy=disconnect_send=change_epoch=yield_after_ack=fail_send=false;
}
static esp_err_t exchange(nuwax_ws_client_t *ws) {
    uint32_t offset=0;
    esp_err_t err=replay_exchange_chunk(ws,7,bytes,sizeof(bytes),"unchanged-request","segment",true,&offset);
    if(err==ESP_OK) assert(offset==1234);
    return err;
}
int main(void) {
    nuwax_ws_client_t ws={true,7};
    reset(); failures=1; assert(exchange(&ws)==ESP_OK && sends==2 && arms==2);
    reset(); failures=10; assert(exchange(&ws)==ESP_ERR_TIMEOUT && sends==REPLAY_CHUNK_ACK_ATTEMPTS);
    reset(); disconnect_send=true; assert(exchange(&ws)==ESP_ERR_TIMEOUT && sends==1 && waits==0 && disarms==1);
    ws.connected=true; reset(); failures=1; change_epoch=true;
    assert(exchange(&ws)==ESP_ERR_TIMEOUT && sends==1); ws.epoch=7;
    reset(); failures=1; yield_after_ack=true;
    assert(exchange(&ws)==ESP_ERR_NOT_FINISHED && sends==1);
    reset(); busy=true; assert(exchange(&ws)==ESP_ERR_NOT_FINISHED && sends==0);
    reset(); fail_send=true; failures=1; assert(exchange(&ws)==ESP_OK && sends==3 && disarms==1);
    for (unsigned route=SD_UPLOAD_MEETING; route<=SD_UPLOAD_HISTORY; ++route) {
        s_replay_route=route; reset(); failures=1;
        assert(exchange(&ws)==ESP_OK && sends==2 && arms==2);
    }
    puts("PASS: production chunk ACK loss retry, identical bytes/request, bounded exhaustion, epoch fencing, partial-send disconnect and recording preemption");
}
