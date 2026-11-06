#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>

#define CONFIG_NUWAX_OFFLINE_AUDIO_ENABLE 1
#ifndef CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE
#define CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE 1
#endif
#define CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE (!CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE)
#define CONFIG_NUWAX_OTA_ENABLE 1
#define ESP_OK 0
#define ESP_ERR_NOT_FINISHED 1
#define ESP_ERR_NOT_SUPPORTED 2
#define ESP_ERR_TIMEOUT 3
#define ESP_ERR_INVALID_STATE 4
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
typedef int esp_err_t;
typedef struct { bool connected; uint32_t epoch; } nuwax_ws_client_t;
static uint32_t clock_ms, s_offline_replay_attempt_epoch;
static uint32_t s_offline_replay_next_attempt_ms, s_offline_replay_backoff_ms;
static bool lease, meeting, monitoring, provision, notification, sender, ota;
static bool ready, busy, running, retry;
static bool simulated_offline, sd_monitoring, sd_meeting, sd_online;
static bool nuwax_wifi_simulated_offline(void) { return simulated_offline; }
static bool nuwax_sd_session_monitoring(void) { return sd_monitoring; }
static bool nuwax_sd_session_active(void) { return sd_monitoring || sd_meeting; }
static void nuwax_sd_session_network(bool online) { sd_online=online; }
static unsigned starts;
static unsigned supervisor_wakes;
static int start_result;
static int s_meeting_mode, s_monitoring_mode, s_notification_transaction;
static struct { bool capture_active; } s_prelisten;
static bool offline_mode_lease_pending(void) { return lease; }
static bool nuwax_meeting_mode_has_pending(void *p) { (void)p; return meeting; }
static bool nuwax_monitoring_mode_has_pending(void *p) { (void)p; return monitoring; }
static bool reprovision_request_pending(void) { return provision; }
static bool nuwax_notification_blocks_conversation(void *p) { (void)p; return notification; }
static bool nuwax_audio_uplink_sender_busy(void) { return sender; }
static bool nuwax_ota_device_is_active(void) { return ota; }
static bool nuwax_ws_is_connected(nuwax_ws_client_t *p) { return p->connected; }
static uint32_t nuwax_ws_connection_epoch(nuwax_ws_client_t *p) { return p->epoch; }
static bool nuwax_sd_card_is_mounted(void) { return true; }
static esp_err_t nuwax_offline_audio_recover_if_idle(unsigned ms) { (void)ms; return ESP_OK; }
static void nuwax_offline_audio_supervisor_wake(void) { supervisor_wakes++; }
static uint32_t monotonic_ms(void) { return clock_ms; }
static bool elapsed_ms(uint32_t now, uint32_t deadline) { return (int32_t)(now-deadline)>=0; }
static bool nuwax_offline_audio_replay_retry_pending(void) { return retry; }
static bool nuwax_offline_audio_replay_retry_required(void) { bool r=retry; retry=false; return r; }
static bool nuwax_offline_audio_storage_busy(void) { return busy; }
static bool nuwax_offline_audio_replay_running(void) { return running; }
static bool nuwax_offline_audio_has_ready(void) { return ready; }
#if CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE
static esp_err_t nuwax_offline_audio_replay_test_start(void) {
#else
static esp_err_t nuwax_offline_audio_replay_formal_start(nuwax_ws_client_t *ws, uint32_t epoch) {
    assert(ws && ws->connected && ws->epoch==epoch);
#endif
    starts++;
    running=start_result==ESP_OK;
    if (start_result==ESP_ERR_NOT_FINISHED) retry=true;
    return start_result;
}
static esp_err_t service_application_keepalive(nuwax_ws_client_t *p,const char *c) { (void)p; (void)c; return ESP_OK; }
static bool nuwax_ws_heartbeat_healthy_if_epoch(nuwax_ws_client_t *p,uint32_t e) { return p->epoch==e; }
static bool nuwax_ws_keepalive_timeout_is_transient(bool a,bool b,bool c,bool d) { return a&&b&&c&&d; }
#include "offline_dispatch_host.inc"

/* Compile the actual MIC transfer API and publication function. The delay
 * stub exercises reader publication, cancellation and stalled teardown. */
typedef struct { void *rx_handle; void *raw_samples; } nuwax_mic_input_t;
static bool capture_running;
static unsigned release_count, transfer_scenario;
static int64_t clock_us;
static nuwax_mic_input_t input={ (void*)1, (void*)2 };
static bool nuwax_offline_audio_capture_running(void) { return capture_running; }
static int64_t esp_timer_get_time(void) { return clock_us; }
static esp_err_t nuwax_mic_input_deinit(nuwax_mic_input_t *p) { release_count++; memset(p,0,sizeof(*p)); return ESP_OK; }
#define pdMS_TO_TICKS(x) (x)
static void vTaskDelay(unsigned ticks);
#include "offline_mic_transfer_host.inc"
static void vTaskDelay(unsigned ticks) {
    clock_us+=(int64_t)ticks*1000;
    if (transfer_scenario==1 || transfer_scenario==2) {
        unsigned ticket=atomic_load(&s_mic_transfer);
        if (ticket&1U) assert(publish_capture_input(&input,ticket));
        if (transfer_scenario==1) capture_running=false;
    }
}

int main(void) {
    nuwax_ws_client_t ws={true,3};
    ready=true; busy=true; retry=true;
    /* Writer closes seven seconds after idle wait begins. */
    for (clock_ms=0;clock_ms<7000;clock_ms+=100) {
        service_idle_wait_application_keepalive(&ws);
        assert(starts==0);
    }
    busy=false;
    service_idle_wait_application_keepalive(&ws);
    assert(starts==1 && running && !retry);
    /* A fresh file / exhausted network retry needs no edge flag. */
    running=false; retry=false; clock_ms=11999;
    service_idle_wait_application_keepalive(&ws); assert(starts==1);
    clock_ms=12000; service_idle_wait_application_keepalive(&ws); assert(starts==2);
    running=false; clock_ms=20000;
    meeting=true; service_idle_wait_application_keepalive(&ws); assert(starts==2);
    sd_meeting=true;assert(!offline_replay_control_busy());sd_meeting=false;
    meeting=false; monitoring=true; service_idle_wait_application_keepalive(&ws); assert(starts==2);
    monitoring=false; lease=true; service_idle_wait_application_keepalive(&ws); assert(starts==2);
    lease=false; ota=true; service_idle_wait_application_keepalive(&ws); assert(starts==2);
    ota=false; start_result=ESP_ERR_NOT_SUPPORTED;
    service_idle_wait_application_keepalive(&ws); assert(starts==3);
    clock_ms=20999; service_idle_wait_application_keepalive(&ws); assert(starts==3);
    clock_ms=21000; start_result=ESP_OK;
    service_idle_wait_application_keepalive(&ws); assert(starts==4);

    /* A recovery race can raise a retry edge after the busy snapshot. It
     * must not defeat the deadline on every 10 ms application poll. */
    running=false; start_result=ESP_ERR_NOT_FINISHED;
    clock_ms=26000; service_idle_wait_application_keepalive(&ws);
    assert(starts==5 && retry);
    const uint32_t due=s_offline_replay_next_attempt_ms;
    for (clock_ms=26010;clock_ms<due;clock_ms+=10) {
        service_idle_wait_application_keepalive(&ws);
        assert(starts==5 && s_offline_replay_next_attempt_ms==due);
    }
    busy=true; retry=true;
    clock_ms=due; service_idle_wait_application_keepalive(&ws);
    assert(starts==5 && retry);
    busy=false; start_result=ESP_OK;
    service_idle_wait_application_keepalive(&ws);
    assert(starts==6 && running && !retry);
    assert(supervisor_wakes==1);
    ws.epoch++;
    service_idle_wait_application_keepalive(&ws);
    assert(supervisor_wakes==2);
    running=false; clock_ms=100000;
    simulated_offline=true; service_idle_wait_application_keepalive(&ws);
    assert(starts==6 && !sd_online);
    simulated_offline=false; sd_monitoring=monitoring=true;
    service_idle_wait_application_keepalive(&ws);
    assert(starts==7 && sd_online && s_offline_replay_backoff_ms==100);
    sd_monitoring=monitoring=false;

    nuwax_mic_input_t taken={0};
    capture_running=true; transfer_scenario=1;
    assert(nuwax_offline_audio_take_capture_input(&taken,1000)==ESP_OK);
    assert(taken.rx_handle==input.rx_handle && taken.raw_samples==input.raw_samples && release_count==0);
    capture_running=true; transfer_scenario=0;
    assert(nuwax_offline_audio_take_capture_input(&taken,10)==ESP_ERR_TIMEOUT);
    assert(release_count==0 && atomic_load(&s_mic_transfer)==0);
    /* A cancelled publication cannot satisfy a subsequent request (ABA). */
    unsigned old_ticket=s_mic_transfer_ticket-2;
    atomic_store(&s_mic_transfer,s_mic_transfer_ticket);
    assert(!publish_capture_input(&input,old_ticket));
    atomic_store(&s_mic_transfer,0);
    capture_running=true; transfer_scenario=2;
    assert(nuwax_offline_audio_take_capture_input(&taken,10)==ESP_ERR_TIMEOUT);
    assert(release_count==1 && atomic_load(&s_mic_transfer)==0);
    puts("offline dispatch and MIC transfer: PASS");
    return 0;
}
