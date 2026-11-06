"""Execute current startup/initialization publication with adverse scheduling."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'main/board/shake_detector.c').read_text(encoding='utf-8-sig')

def function(signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

runtime = re.search(r'typedef struct \{[^}]+\} nuwax_shake_detector_runtime_t;', source)[0]
state = source[source.index('typedef enum {\n    SHAKE_STOPPED'):source.index('static portMUX_TYPE s_interaction_gate_mux')]
worker = function('static void shake_detector_task(')
worker = worker[:worker.index('    ESP_LOGI(TAG,')] + '}\n'
body = runtime + '\nstatic nuwax_shake_detector_runtime_t s_runtime;\n' + state + worker
body += function('esp_err_t nuwax_shake_detector_start(')
body += function('bool nuwax_shake_detector_is_running(')
common = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
typedef int esp_err_t;
typedef void *TaskHandle_t;
typedef int StaticTask_t;
typedef unsigned StackType_t;
typedef void (*nuwax_shake_detector_event_cb_t)(void *);
typedef struct {int i2c_address, accel_range, gyro_range, accel_odr, gyro_odr;
    bool use_interrupt; int int_gpio;} qmi8658a_config_t;
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
static unsigned allocations, live, deleted, fail_alloc;
static int sensor_error;
static bool early, create_fail, nested;
static void cb(void *p) {(void)p;}
esp_err_t nuwax_shake_detector_start(nuwax_shake_detector_event_cb_t, void *);
static void *heap_caps_malloc(size_t n, int flags) {
    (void)flags; allocations++;
    if (allocations == fail_alloc) return NULL;
    live++; return malloc(n);
}
static void checked_free(void *p) {if(p) {assert(live); live--; free(p);}}
#define free checked_free
static int qmi8658a_init(const qmi8658a_config_t *c) {(void)c; return sensor_error;}
static int nuwax_device_warning_report_enqueue(const char*a,const char*b,int c,const char*d,unsigned e,void*f) {
    (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;return 0;
}
static void vTaskDelete(void *p) {(void)p; deleted++;}
static void *xTaskCreateStatic(void(*task)(void*), const char*n, unsigned bytes,
    void *arg, unsigned priority, void *stack, void *tcb) {
    (void)n;(void)bytes;(void)priority;assert(stack && tcb);
    if(nested) assert(nuwax_shake_detector_start(cb, NULL)==ESP_ERR_INVALID_STATE);
    if(create_fail) return NULL;
    if(early) task(arg);
    return tcb;
}
'''
tests = r'''
static void reset(void) {
    free(s_runtime.stack); free(s_runtime.tcb);
    memset(&s_runtime, 0, sizeof s_runtime);
    atomic_store(&s_task_state, SHAKE_STOPPED);
    assert(!live); allocations=deleted=fail_alloc=0;
    early=create_fail=nested=false; sensor_error=0;
}
int main(void) {
    assert(nuwax_shake_detector_start(NULL,NULL)==ESP_ERR_INVALID_ARG);
    for(unsigned before=0;before<2;before++) for(unsigned failure=0;failure<2;failure++) {
        reset(); early=before; nested=true; sensor_error=failure?77:0;
        int result=nuwax_shake_detector_start(cb,NULL);
        assert(result==(before && failure ? 77 : ESP_OK));
        if(!before) {assert(!nuwax_shake_detector_is_running());shake_detector_task(NULL);}
        assert(nuwax_shake_detector_is_running()==!failure);
        assert(deleted==failure && live==2);
        for(unsigned i=0;i<10;i++) assert(nuwax_shake_detector_start(cb,NULL)==ESP_ERR_INVALID_STATE);
        assert(live==2 && allocations==2);
    }
    for(unsigned allocation=1;allocation<=2;allocation++) {
        reset(); fail_alloc=allocation;
        assert(nuwax_shake_detector_start(cb,NULL)==ESP_ERR_NO_MEM && !live);
        fail_alloc=0;early=true;
        assert(nuwax_shake_detector_start(cb,NULL)==ESP_OK && nuwax_shake_detector_is_running());
    }
    reset(); create_fail=true;
    assert(nuwax_shake_detector_start(cb,NULL)==ESP_ERR_NO_MEM && !live);
    create_fail=false;early=true;
    assert(nuwax_shake_detector_start(cb,NULL)==ESP_OK && nuwax_shake_detector_is_running());
    reset();
    puts("PASS: early/late init success/failure, concurrent start, allocation/create failure, ready publication and bounded retained storage");
}
'''
with tempfile.TemporaryDirectory(prefix='shake-startup-') as folder:
    c = Path(folder)/'check.c'
    exe = c.with_suffix('.exe')
    variants = {'current': body,
        'failed_becomes_ready': body.replace('return s_runtime.startup_error;',
            'atomic_store(&s_task_state, SHAKE_RUNNING);'),
        'retry_failed_storage': body.replace('SHAKE_FAILED, memory_order_release',
            'SHAKE_STOPPED, memory_order_release')}
    for name, variant in variants.items():
        assert name=='current' or variant!=body
        c.write_text(common+variant+tests, encoding='utf-8')
        subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror',str(c),'-o',str(exe)],check=True)
        result=subprocess.run([str(exe)], capture_output=True, text=True)
        assert (result.returncode==0)==(name=='current'), result.stdout+result.stderr
        print(result.stdout.strip() if name=='current' else 'PASS: rejected '+name)
