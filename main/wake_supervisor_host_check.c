#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <setjmp.h>
#include <stdio.h>
#define CONFIG_NUWAX_WAKE_WORD_ENABLE 1
#define CONFIG_NUWAX_OFFLINE_AUDIO_ENABLE 0
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
#define portMAX_DELAY 0xffffffffU
#define ESP_OK 0
#define NUWAX_WAKE_SUPERVISOR_POLL_MS 20
#define NUWAX_WAKE_SUPERVISOR_READY_TIMEOUT_MS 5000U
#define NUWAX_WAKE_SUPERVISOR_DATA_STALE_MS 5000U
#define NUWAX_WAKE_SUPERVISOR_RETRY_MS 100U
#define NUWAX_WAKE_WORD_STOP_TIMEOUT_MS 500U
#define NUWAX_APP_STATE_ERROR 1
#define TAG "test"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
typedef int esp_err_t;
static atomic_bool s_wake_detector_desired=true, s_reprovision_quiesce_requested;
static atomic_uint s_wake_detector_request_ms=1;
static int s_wake_word_events;
static unsigned tick, scenario, stops, starts, stop_at;
static bool running=true;
static jmp_buf finished;
static uint32_t monotonic_ms(void) {return tick*1000;}
static bool elapsed_ms(uint32_t now,uint32_t deadline) {return (int32_t)(now-deadline)>=0;}
static unsigned ulTaskNotifyTake(int clear,unsigned wait) {
    (void)clear;(void)wait;if(++tick>20)longjmp(finished,1);return 0;
}
static bool nuwax_wake_word_detector_flash_paused(void) {return scenario==0 && tick>=4 && tick<=14;}
static bool conversation_capture_busy_for_sleep(void) {return false;}
static bool meeting_or_monitoring_mode_blocks_sleep(void) {return false;}
static bool ota_blocks_wake_detector(void) {return false;}
static unsigned uxQueueMessagesWaiting(int queue) {(void)queue;return 0;}
static bool nuwax_wake_word_detector_is_ready(void) {
    return running && (scenario==0 ? !nuwax_wake_word_detector_flash_paused()
        : tick<=3 || (scenario==1 && stops>0));
}
static uint32_t nuwax_wake_word_detector_last_feed_ms(void) {return monotonic_ms();}
static bool nuwax_wake_word_detector_is_running(void) {return running;}
static int nuwax_wake_word_detector_stop_with_timeout(unsigned timeout) {
    (void)timeout;stops++;stop_at=tick;running=false;return ESP_OK;
}
static bool start_sleep_wake_word_detector(const char *reason) {(void)reason;starts++;running=true;return false;}
static void set_state(int s) {(void)s;assert(false);}
static void nuwax_diagnostics_log_heap_label(const char *tag,const char *label) {(void)tag;(void)label;}
static void vTaskDelay(unsigned ticks) {(void)ticks;}
static void esp_restart(void) {assert(false);}
#include "wake_supervisor_host.inc"
int main(void) {
    for(scenario=0;scenario<3;scenario++) {
        tick=stops=starts=stop_at=0;running=true;
        if(!setjmp(finished))wake_word_supervisor_task(NULL);
        if(scenario==0)assert(stops==0 && starts==0); // Eleven-second planned pause.
        else if(scenario==1)assert(stops==1 && stop_at==9); // Five seconds since readiness loss, not old request.
        else assert(stops==2 && stop_at==15); // A restarted worker gets a fresh readiness budget.
    }
    puts("PASS: production wake supervisor ignores planned Flash pause and times real readiness loss from its onset");
}
