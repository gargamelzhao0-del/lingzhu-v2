#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "touch_input.h"
#include "touch_read_policy.h"
#ifndef ESP_ERR_NOT_FINISHED
#define ESP_ERR_NOT_FINISHED 0x10c
#endif

#define CONFIG_NUWAX_TOUCH_POLL_INTERVAL_MS 20U
#define NUWAX_TOUCH_IDLE_FALLBACK_POLL_MS 100U
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define taskENTER_CRITICAL(x) ((void)0)
#define taskEXIT_CRITICAL(x) ((void)0)
static void *s_touch_queue=(void *)1;
static nuwax_touch_event_cb_t s_event_callback;
static void *s_event_user_ctx;
static jmp_buf finished;
typedef struct {uint32_t time; esp_err_t error; bool down; uint16_t x;} sample_t;
static const sample_t *input;
static unsigned input_count, at, event_count, presses;
static uint32_t now;
static nuwax_touch_point_t events[16];
static uint32_t event_times[16];
static unsigned ulTaskNotifyTake(int clear,unsigned wait) {(void)clear;assert(wait>0);return 0;}
static int64_t esp_timer_get_time(void) {return (int64_t)now*1000;}
esp_err_t nuwax_touch_read(nuwax_touch_point_t *point) {
    if(at==input_count)longjmp(finished,1);
    const sample_t sample=input[at++];now=sample.time;
    memset(point,0,sizeof(*point)); // Production read clears output even on failure.
    if(sample.error==ESP_OK) {point->touched=sample.down;point->x=sample.x;point->y=71;}
    return sample.error;
}
static bool touch_queue_send_latest(const nuwax_touch_point_t *point,unsigned wait) {
    (void)wait;assert(event_count<16);event_times[event_count]=now;events[event_count++]=*point;return true;
}
static void pressed(const nuwax_touch_point_t *point,void *ctx) {(void)ctx;assert(point->touched);presses++;}
#include "touch_task_host.inc"
static void run(const sample_t *samples,unsigned count) {
    input=samples;input_count=count;at=event_count=presses=0;
    s_event_callback=pressed;
    if(setjmp(finished)==0)touch_log_task(NULL);
}
int main(void) {
    const sample_t recover[]={{10,ESP_OK,true,12},{30,ESP_ERR_NOT_FINISHED,false,0},
        {80,ESP_OK,true,18},{90,ESP_OK,false,18}};
    run(recover,4);
    assert(presses==1 && event_count==3 && events[1].x==18 && events[1].touched);
    assert(!events[2].cancelled && !events[2].touched && events[2].event==3);
    const sample_t expire[]={{10,ESP_OK,true,45},{50,ESP_ERR_NOT_FINISHED,false,0},
        {109,ESP_ERR_NOT_FINISHED,false,0},{110,ESP_ERR_NOT_FINISHED,false,0},
        {130,ESP_ERR_NOT_FINISHED,false,0},{150,ESP_OK,true,66}};
    run(expire,6);
    assert(event_count==3 && presses==2);
    assert(events[1].cancelled && !events[1].touched && events[1].x==45 && events[1].y==71);
    assert(event_times[1]==110 && events[2].touched && events[2].x==66);
    const sample_t hard[]={{10,ESP_OK,true,22},{11,ESP_FAIL,false,0}};
    run(hard,2);
    assert(event_count==2 && events[1].cancelled && event_times[1]==11 && events[1].x==22);
    puts("PASS: production touch task preserves drag across transient errors, bounded cancel keeps coordinates, release/new press and hard failure");
}
