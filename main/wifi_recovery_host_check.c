#include <windows.h>
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "esp_err.h"
#define ESP_ERR_WIFI_CONN 0x3007
#define WIFI_CONNECTED_BIT 1U
#define WIFI_FAIL_BIT 2U
#define WIFI_RECONNECT_BIT 16U
#define WIFI_CONNECT_TIMEOUT_MS 30000U
#define pdMS_TO_TICKS(ms) (ms)
#define pdFALSE 0
#define TAG "test"
#define ESP_RETURN_ON_ERROR(expr,...) do{int e=(expr);if(e!=ESP_OK)return e;}while(0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
typedef uint32_t TickType_t;
typedef unsigned EventBits_t;
static atomic_bool s_wifi_connect_requested,s_wifi_simulated_offline,s_wifi_driver_connecting,s_synchronous_connect_active;
static atomic_bool connected;
static atomic_uint driver_calls;
static atomic_uint s_wifi_intent_revision;
static bool provisioning;
static unsigned event_bits,wait_calls,full_connects,disconnects,lock_depth,mode;
static int driver_error,s_wifi_event_group=1;
static char s_current_ip[16];
#define WIFI_IF_STA 0
#define WIFI_NVS_FLASH_GUARD_TIMEOUT_MS 500U
#define WIFI_AUTH_OPEN 0
#define WIFI_AUTH_WPA2_PSK 3
typedef struct {
    unsigned marker;
    struct { char password[64]; struct { int authmode; } threshold; } sta;
} wifi_config_t;
static wifi_config_t driver_config;
static bool normalize_driver;
static int get_config_error = ESP_FAIL;
static int esp_wifi_get_config(unsigned mode,wifi_config_t *config) {
    assert(mode==WIFI_IF_STA);
    *config=driver_config; return get_config_error;
}
static int guard_error, config_error;
static unsigned guard_depth, guard_ends, config_calls;
static int nuwax_wake_word_detector_begin_flash_operation(uint32_t timeout) {
    assert(timeout==WIFI_NVS_FLASH_GUARD_TIMEOUT_MS);
    if(guard_error!=ESP_OK)return guard_error;
    guard_depth++;return ESP_OK;
}
static void nuwax_wake_word_detector_end_flash_operation(void) {
    assert(guard_depth);guard_depth--;guard_ends++;
}
static int esp_wifi_set_config(unsigned mode,wifi_config_t *config) {
    assert(mode==WIFI_IF_STA && config->marker==42U && guard_depth);
    config_calls++;
    if(config_error==ESP_OK) {
        driver_config=*config;
        if(normalize_driver)driver_config.marker=99U;
    }
    return config_error;
}
static TickType_t ticks;
static bool nuwax_wifi_is_connected(void){return connected;}
static bool nuwax_wifi_provisioning_active(void){return provisioning;}
static int wifi_init_once(void){return ESP_OK;}
static int wifi_operation_lock(void){lock_depth++;return ESP_OK;}
static void wifi_operation_unlock(void){assert(lock_depth);lock_depth--;}
static int esp_wifi_connect(void){driver_calls++;Sleep(1);return driver_error;}
static int esp_wifi_disconnect(void){disconnects++;connected=false;return ESP_OK;}
static int nuwax_wifi_connect(void){assert(lock_depth);full_connects++;connected=true;return ESP_OK;}
static TickType_t xTaskGetTickCount(void){return ticks;}
static unsigned xEventGroupGetBits(int group){assert(group==1);return event_bits;}
static void xEventGroupClearBits(int group,unsigned bits){assert(group==1);event_bits&=~bits;}
static unsigned xEventGroupWaitBits(int group,unsigned bits,int clear,int all,unsigned timeout){
    assert(group==1 && bits==3 && !clear && !all && timeout<=100);wait_calls++;ticks+=timeout;
    if(mode==0){connected=true;s_wifi_driver_connecting=false;event_bits|=WIFI_CONNECTED_BIT;}
    if(mode==2)event_bits|=WIFI_FAIL_BIT;
    if(mode==3)provisioning=true;
    if(mode==4)s_wifi_simulated_offline=true;
    if(mode==5){s_wifi_intent_revision++;event_bits|=WIFI_FAIL_BIT;} // Handoff and cancellation raced with wait.
    return event_bits&bits;
}
#include "wifi_recovery_host.inc"
static void reset(void){
    assert(!lock_depth);s_wifi_connect_requested=true;s_wifi_simulated_offline=false;
    s_wifi_driver_connecting=false;s_synchronous_connect_active=false;connected=false;
    driver_calls=wait_calls=full_connects=disconnects=0;provisioning=false;
    event_bits=WIFI_RECONNECT_BIT;driver_error=ESP_OK;ticks=0;mode=0;s_wifi_intent_revision=0;
}
static DWORD WINAPI competing_start(void *unused){(void)unused;assert(wifi_driver_connect_once()==ESP_OK);return 0;}
int main(void){
    wifi_config_t config={.marker=42U};
    guard_error=ESP_ERR_TIMEOUT;
    assert(wifi_set_config_guarded(&config)==ESP_ERR_TIMEOUT && !config_calls && !guard_ends && !guard_depth);
    guard_error=ESP_ERR_INVALID_STATE;
    assert(wifi_set_config_guarded(&config)==ESP_ERR_INVALID_STATE && !config_calls && !guard_ends);
    guard_error=ESP_OK;
    const int config_results[]={ESP_OK,ESP_FAIL,ESP_ERR_INVALID_STATE};
    for(unsigned i=0;i<3;i++){
        config_error=config_results[i];
        assert(wifi_set_config_guarded(&config)==config_error && !guard_depth);
        assert(config_calls==i+1 && guard_ends==i+1);
    }
    guard_depth=1;config_error=ESP_OK;
    assert(wifi_set_config_guarded(&config)==ESP_OK && guard_depth==1);
    guard_depth=0;
    puts("PASS: production Wi-Fi driver config Flash fence; no write after failed stop; success/error release; nested ownership preserved");
    get_config_error=ESP_OK;driver_config=config;
    unsigned before=config_calls;
    assert(wifi_set_config_guarded(&config)==ESP_OK && config_calls==before);
    strcpy(config.sta.password,"host-test-only"); driver_config=config;
    driver_config.sta.threshold.authmode=WIFI_AUTH_WPA2_PSK;
    assert(wifi_set_config_guarded(&config)==ESP_OK && config_calls==before);
    driver_config.marker=1U;
    assert(wifi_set_config_guarded(&config)==ESP_OK && config_calls==before+1);
    assert(wifi_set_config_guarded(&config)==ESP_OK && config_calls==before+1);
    driver_config.sta.password[0]='x'; // External provisioning changed driver state.
    assert(wifi_set_config_guarded(&config)==ESP_OK && config_calls==before+2);
    normalize_driver=true; driver_config.marker=7U;
    assert(wifi_set_config_guarded(&config)==ESP_OK && config_calls==before+3);
    assert(driver_config.marker==99U);
    assert(wifi_set_config_guarded(&config)==ESP_OK && config_calls==before+3);
    get_config_error=ESP_FAIL; guard_error=ESP_ERR_TIMEOUT;
    assert(wifi_set_config_guarded(&config)==ESP_ERR_TIMEOUT && config_calls==before+3);
    puts("PASS: unchanged config and automatic WPA2 threshold skip Flash; changed/external config retains fence; readback reuse");
    reset();assert(wifi_driver_connect_once()==ESP_OK && driver_calls==1);
    assert(nuwax_wifi_recover()==ESP_OK && driver_calls==1 && !full_connects && !disconnects);
    assert(nuwax_wifi_recover()==ESP_OK && driver_calls==1);
    reset();assert(nuwax_wifi_recover()==ESP_OK && driver_calls==1 && !full_connects && !(event_bits&WIFI_RECONNECT_BIT));
    reset();mode=1;ticks=UINT32_MAX-1000U;
    assert(nuwax_wifi_recover()==ESP_ERR_TIMEOUT && wait_calls==300 && !s_wifi_driver_connecting && !s_wifi_connect_requested);
    assert(nuwax_wifi_recover()==ESP_OK && full_connects==1);
    reset();mode=2;assert(nuwax_wifi_recover()==ESP_FAIL && !s_wifi_connect_requested);
    reset();event_bits|=WIFI_FAIL_BIT;assert(nuwax_wifi_recover()==ESP_OK && full_connects==1 && !driver_calls);
    reset();driver_error=ESP_FAIL;assert(nuwax_wifi_recover()==ESP_FAIL && !s_wifi_driver_connecting && !wait_calls);
    reset();driver_error=ESP_ERR_WIFI_CONN;assert(wifi_driver_connect_once()==ESP_OK && s_wifi_driver_connecting);
    assert(wifi_driver_disconnect()==ESP_OK && !s_wifi_driver_connecting && disconnects==1);
    for(unsigned m=3;m<=4;m++){reset();mode=m;assert(nuwax_wifi_recover()==ESP_ERR_INVALID_STATE && wait_calls==1 && !full_connects && !lock_depth);}
    reset();mode=5;assert(nuwax_wifi_recover()==ESP_ERR_INVALID_STATE && s_wifi_connect_requested);
    reset();provisioning=true;assert(wifi_driver_connect_once()==ESP_ERR_INVALID_STATE && !driver_calls);
    reset();s_wifi_simulated_offline=true;assert(nuwax_wifi_recover()==ESP_ERR_INVALID_STATE && !driver_calls);
    reset();HANDLE tasks[20];
    for(unsigned i=0;i<20;i++)tasks[i]=CreateThread(NULL,0,competing_start,NULL,0,NULL);
    for(unsigned i=0;i<20;i++){assert(WaitForSingleObject(tasks[i],3000)==WAIT_OBJECT_0);CloseHandle(tasks[i]);}
    assert(driver_calls==1);
    puts("PASS: production Wi-Fi attempt coalescing with 20 callers; watcher/recovery join; queued start; IP/failure/timeout/tick wrap; provisioning/offline cancellation; fresh retry after timeout");
}
