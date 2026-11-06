
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
typedef int esp_err_t;typedef void* TaskHandle_t;typedef int StaticTask_t;typedef unsigned StackType_t;
typedef void(*nuwax_shake_detector_event_cb_t)(void*);
typedef struct {int i2c_address,accel_range,gyro_range,accel_odr,gyro_odr;bool use_interrupt;int int_gpio;} qmi8658a_config_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_NO_MEM 3
#define QMI8658A_ACCEL_RANGE_8G 2
#define QMI8658A_GYRO_RANGE_256DPS 4
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_SPIRAM 4
#define NUWAX_SHAKE_TASK_STACK_BYTES 3072
#define NUWAX_SHAKE_TASK_PRIORITY 8
#define ESP_LOGW(...) ((void)0)
static int allocations,deletes;static bool early;
static void* heap_caps_malloc(size_t n,int flags){allocations++;return malloc(n);}
static int qmi8658a_init(const qmi8658a_config_t*c){return 9;}
static int nuwax_device_warning_report_enqueue(const char*a,const char*b,int c,const char*d,unsigned e,void*f){return 0;}
static void vTaskDelete(void*p){deletes++;}
static void* xTaskCreateStatic(void(*task)(void*),const char*n,unsigned z,void*a,unsigned priority,void*stack,void*tcb){if(early)task(a);return tcb;}
typedef struct {
    nuwax_shake_detector_event_cb_t callback;
    void *ctx;
    TaskHandle_t task;
    StaticTask_t *tcb;
    StackType_t *stack;
    bool running;
} nuwax_shake_detector_runtime_t;
static nuwax_shake_detector_runtime_t s_runtime;
static void shake_detector_task(void *arg) {
    (void)arg;
    const qmi8658a_config_t config = {
        .i2c_address = 0,
        .accel_range = QMI8658A_ACCEL_RANGE_8G,
        .gyro_range = QMI8658A_GYRO_RANGE_256DPS,
        .accel_odr = 62,
        .gyro_odr = 62,
        .use_interrupt = false,
        .int_gpio = -1,
    };
    const esp_err_t init_err = qmi8658a_init(&config);
    if (init_err != ESP_OK) {
        ESP_LOGW(TAG,
                 "QMI8658A shake detector unavailable err=%s; conversation continues without shake input",
                 esp_err_to_name(init_err));
        (void)nuwax_device_warning_report_enqueue(
            "motion", "sensor_unavailable", init_err, "shake", 0U, NULL);
        s_runtime.running = false;
        s_runtime.task = NULL;
        /* The static task stack cannot be freed from the task itself. The
         * worker exits once, so this bounded allocation is retained for the
         * lifetime of the firmware and prevents a retry race. */
        vTaskDelete(NULL);
        return;
    }
}
esp_err_t nuwax_shake_detector_start(nuwax_shake_detector_event_cb_t callback,
                                     void *ctx) {
    if (!callback) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_runtime.task) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_runtime, 0, sizeof(s_runtime));
    s_runtime.callback = callback;
    s_runtime.ctx = ctx;
    s_runtime.tcb = heap_caps_malloc(sizeof(StaticTask_t),
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_runtime.stack = heap_caps_malloc(NUWAX_SHAKE_TASK_STACK_BYTES,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_runtime.tcb || !s_runtime.stack) {
        ESP_LOGW(TAG, "shake detector task memory unavailable; feature disabled");
        (void)nuwax_device_warning_report_enqueue(
            "motion", "task_memory_unavailable", ESP_ERR_NO_MEM,
            "shake", 0U, NULL);
        free(s_runtime.tcb);
        free(s_runtime.stack);
        memset(&s_runtime, 0, sizeof(s_runtime));
        return ESP_ERR_NO_MEM;
    }
    s_runtime.task = xTaskCreateStatic(shake_detector_task,
                                       "nuwax_shake",
                                       NUWAX_SHAKE_TASK_STACK_BYTES,
                                       NULL,
                                       NUWAX_SHAKE_TASK_PRIORITY,
                                       s_runtime.stack,
                                       s_runtime.tcb);
    if (!s_runtime.task) {
        free(s_runtime.tcb);
        free(s_runtime.stack);
        memset(&s_runtime, 0, sizeof(s_runtime));
        return ESP_ERR_NO_MEM;
    }
    s_runtime.running = true;
    return ESP_OK;
}bool nuwax_shake_detector_is_running(void) {
    return s_runtime.running;
}
static void cb(void*p){}
int main(void){early=true;int rc=nuwax_shake_detector_start(cb,0);
 assert(rc==ESP_OK && deletes==1 && nuwax_shake_detector_is_running() && s_runtime.task);
 assert(nuwax_shake_detector_start(cb,0)==ESP_ERR_INVALID_STATE);
 puts("REPRODUCED early init failure: deleted worker reported running, restart rejected");
 free(s_runtime.stack);free(s_runtime.tcb);memset(&s_runtime,0,sizeof(s_runtime));allocations=deletes=0;early=false;
 assert(nuwax_shake_detector_start(cb,0)==ESP_OK);void*oldstack=s_runtime.stack;void*oldtcb=s_runtime.tcb;
 shake_detector_task(0);assert(!s_runtime.task&&!s_runtime.running);
 assert(nuwax_shake_detector_start(cb,0)==ESP_OK);assert(allocations==4&&s_runtime.stack!=oldstack&&s_runtime.tcb!=oldtcb);
 puts("REPRODUCED late init failure + explicit retry: retained allocation pointers overwritten (no current auto-retry caller)");
 free(oldstack);free(oldtcb);free(s_runtime.stack);free(s_runtime.tcb);}
