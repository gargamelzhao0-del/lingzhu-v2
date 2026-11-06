#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <windows.h>
#include "conversation/wake_word_input_policy.h"
#define NUWAX_WAKE_CAPTURE_CHUNK_SAMPLES 240U
#define CONFIG_NUWAX_WAKE_CAPTURE_QUEUE_DEPTH 12U
#define NUWAX_WAKE_CAPTURE_QUEUE_RECOVERY_WAIT_MS 20U
#define NUWAX_WAKE_CAPTURE_QUEUE_FAULT_STREAK 3U
#define NUWAX_WAKE_CAPTURE_READ_TIMEOUT_MS 52U
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG -2
#define ESP_ERR_INVALID_STATE -3
#define ESP_ERR_INVALID_RESPONSE -4
#define ESP_ERR_TIMEOUT -5
#define ESP_ERR_NOT_SUPPORTED -6
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) (ms)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define NUWAX_MIC_OWNER_WAKE_DETECTOR 1
typedef int esp_err_t;
typedef unsigned TickType_t;
typedef unsigned UBaseType_t;
typedef HANDLE TaskHandle_t;
typedef struct {CRITICAL_SECTION lock;uint8_t items[12];unsigned count;} queue_t;
typedef queue_t *QueueHandle_t;
typedef struct {bool live;} nuwax_mic_input_t;
static struct {atomic_bool stop_requested;} s_detector;
static atomic_uint s_wake_capture_backlog_ms;
static atomic_uint drained_signals;
static void wake_capture_signal_drained(void){drained_signals++;}
static SRWLOCK s_wake_capture_pressure_lock = SRWLOCK_INIT;
#define portENTER_CRITICAL(lock) AcquireSRWLockExclusive(lock)
#define portEXIT_CRITICAL(lock) ReleaseSRWLockExclusive(lock)
static atomic_uint generated, mono_reads, dual_reads, releases;
static bool init_fail;
static uint32_t detector_monotonic_ms(void) {return GetTickCount();}
static void vTaskDelay(unsigned ms) {Sleep(ms ? ms : 1);}
static void vTaskSuspend(void *task) {(void)task;ExitThread(0);}
static void xTaskNotifyGive(HANDLE task) {(void)task;}
static unsigned uxQueueMessagesWaiting(QueueHandle_t q) {
    EnterCriticalSection(&q->lock);unsigned n=q->count;LeaveCriticalSection(&q->lock);return n;
}
static int xQueueSend(QueueHandle_t q,const uint8_t *v,unsigned wait) {
    (void)wait;EnterCriticalSection(&q->lock);int ok=q->count<12;
    if(ok)q->items[q->count++]=*v;LeaveCriticalSection(&q->lock);return ok;
}
static int xQueueReceive(QueueHandle_t q,uint8_t *v,unsigned wait) {
    DWORD start=GetTickCount();
    do {EnterCriticalSection(&q->lock);bool ok=q->count>0;
        if(ok){*v=q->items[0];memmove(q->items,q->items+1,--q->count);}
        LeaveCriticalSection(&q->lock);if(ok)return pdTRUE;
        if(!wait)break;Sleep(1);
    }while(GetTickCount()-start<wait);
    return 0;
}
static int nuwax_mic_input_init_with_owner(nuwax_mic_input_t *mic,int owner) {
    assert(owner==1);mic->live=!init_fail;return init_fail?ESP_FAIL:ESP_OK;
}
static int nuwax_mic_input_deinit(nuwax_mic_input_t *mic) {
    assert(mic->live);mic->live=false;atomic_fetch_add(&releases,1);return ESP_OK;
}
static int read_pcm(nuwax_mic_input_t *mic,int16_t *a,int16_t *b,size_t n,unsigned ms,size_t *got) {
    assert(mic->live && n==240 && ms==52);Sleep(15);
    unsigned first=atomic_fetch_add(&generated,(unsigned)n);
    for(size_t i=0;i<n;i++){a[i]=(int16_t)(first+i);if(b)b[i]=(int16_t)-(first+i);}
    *got=n;return ESP_OK;
}
static int nuwax_mic_input_read_pcm16(nuwax_mic_input_t *mic,int16_t *a,size_t n,unsigned ms,size_t *got) {
    atomic_fetch_add(&mono_reads,1);return read_pcm(mic,a,NULL,n,ms,got);
}
static int nuwax_mic_input_read_vocat_mic12_pcm16(nuwax_mic_input_t *mic,int16_t *a,int16_t *b,size_t n,unsigned ms,size_t *got) {
    atomic_fetch_add(&dual_reads,1);return read_pcm(mic,a,b,n,ms,got);
}
#include "wake_capture_types_host.inc"
#include "wake_capture_producer_host.inc"
#include "wake_capture_reader_host.inc"
static DWORD WINAPI run_capture(void *arg) {wake_capture_task(arg);return 0;}
static void check(bool dual,bool overrun,bool fail)
{
    atomic_store(&generated,0);atomic_store(&mono_reads,0);atomic_store(&dual_reads,0);atomic_store(&releases,0);
    atomic_store(&s_detector.stop_requested,false);init_fail=fail;
    nuwax_wake_capture_context_t ctx={0};queue_t free_q={0},ready_q={0};
    InitializeCriticalSection(&free_q.lock);InitializeCriticalSection(&ready_q.lock);
    ctx.free_chunks=&free_q;ctx.ready_chunks=&ready_q;ctx.producer_generation=1;ctx.dual_input=dual;
    for(uint8_t i=0;i<12;i++)assert(xQueueSend(&free_q,&i,0));
    HANDLE thread=CreateThread(NULL,0,run_capture,&ctx,0,NULL);assert(thread);
    while(!atomic_load(&ctx.ready) && !atomic_load(&ctx.exited))Sleep(1);
    if(!fail) {
        int16_t a[512],b[512];size_t got;
        assert(wake_capture_read(&ctx,a,dual?b:NULL,512,200,&got)==ESP_OK && got==512);
        for(unsigned i=0;i<512;i++){assert(a[i]==(int16_t)i);if(dual)assert(b[i]==(int16_t)-i);}
        Sleep(overrun?350:100); // Simulate inference stall while producer continues draining DMA.
        if(overrun) {
            bool gap=false;
            for(unsigned k=0;k<6 && !gap;k++) {
                int err=wake_capture_read(&ctx,a,dual?b:NULL,512,200,&got);
                assert(err==ESP_OK || err==ESP_ERR_INVALID_RESPONSE);gap=err==ESP_ERR_INVALID_RESPONSE;
            }
            assert(gap && atomic_load(&ctx.queue_drops)>0);
        } else {
            for(unsigned k=1;k<6;k++) {
                assert(wake_capture_read(&ctx,a,dual?b:NULL,512,200,&got)==ESP_OK && got==512);
                for(unsigned i=0;i<512;i++){assert(a[i]==(int16_t)(k*512+i));if(dual)assert(b[i]==(int16_t)-(k*512+i));}
            }
            assert(atomic_load(&ctx.queue_drops)==0 && atomic_load(&ctx.queue_high_water)>=4);
        }
        atomic_store(&ctx.stop_requested,true);
    }
    assert(WaitForSingleObject(thread,2000)==WAIT_OBJECT_0);CloseHandle(thread);
    assert(atomic_load(&ctx.exited) && !atomic_load(&ctx.ready));
    assert(atomic_load(&s_wake_capture_backlog_ms)==0);
    assert(atomic_load(&releases)==(fail?0U:1U));
    assert(ctx.producer_generation==1U); // Queue eviction must not mark new tail PCM as discontinuous.
    assert(!atomic_load(&ctx.queue_invariant_faults));
    assert(fail || (dual ? atomic_load(&mono_reads)==0 : atomic_load(&dual_reads)==0));
    if(!fail)assert(free_q.count+ready_q.count+(ctx.consumer_holds_chunk?1U:0U)==12);
    DeleteCriticalSection(&free_q.lock);DeleteCriticalSection(&ready_q.lock);
}
static void check_consumer_pressure(void) {
    nuwax_wake_capture_context_t ctx={0};queue_t free_q={0},ready_q={0};
    InitializeCriticalSection(&free_q.lock);InitializeCriticalSection(&ready_q.lock);
    ctx.free_chunks=&free_q;ctx.ready_chunks=&ready_q;atomic_store(&ctx.ready,true);
    for(uint8_t i=0;i<4;i++) {
        ctx.chunks[i].sample_count=240;ctx.chunks[i].generation=1;ctx.chunks[i].sequence=i;
        assert(xQueueSend(&ready_q,&i,0));
    }
    assert(wake_capture_publish_pressure(&ctx)==4 && s_wake_capture_backlog_ms==60);
    int16_t pcm[480];size_t got;
    // No producer runs: draining must publish immediately, not await next I2S read.
    assert(wake_capture_read(&ctx,pcm,NULL,480,100,&got)==ESP_OK && got==480);
    assert(s_wake_capture_backlog_ms==30);
    assert(wake_capture_read(&ctx,pcm,NULL,480,100,&got)==ESP_OK && got==480);
    assert(s_wake_capture_backlog_ms==0);
    uint8_t index=4;ctx.chunks[index].sample_count=240;ctx.chunks[index].generation=1;ctx.chunks[index].sequence=4;
    assert(xQueueSend(&ready_q,&index,0));atomic_store(&ctx.ready,false);
    assert(wake_capture_publish_pressure(&ctx)==0);
    assert(wake_capture_read(&ctx,pcm,NULL,240,100,&got)==ESP_OK);
    assert(s_wake_capture_backlog_ms==0); // Stopped producer's residual data cannot resurrect pressure.
    DeleteCriticalSection(&free_q.lock);DeleteCriticalSection(&ready_q.lock);
}
static void check_gap_boundary(void) {
    nuwax_wake_capture_context_t ctx={0};queue_t free_q={0},ready_q={0};
    InitializeCriticalSection(&free_q.lock);InitializeCriticalSection(&ready_q.lock);
    ctx.free_chunks=&free_q;ctx.ready_chunks=&ready_q;atomic_store(&ctx.ready,true);
    for(uint8_t i=0;i<10;i++) {
        ctx.chunks[i].sample_count=240;ctx.chunks[i].generation=1;ctx.chunks[i].sequence=i;
        for(unsigned j=0;j<240;j++)ctx.chunks[i].mic1[j]=(int16_t)(i*240+j);
        assert(xQueueSend(&ready_q,&i,0));
    }
    int16_t pcm[480];size_t got=0;
    assert(wake_capture_read(&ctx,pcm,NULL,192,100,&got)==ESP_OK && got==192);
    // Consumer owns the tail of chunk 0. Producer evicts queued chunks 1 and 2.
    uint8_t evicted;
    assert(xQueueReceive(&ready_q,&evicted,0) && evicted==1);
    assert(xQueueSend(&free_q,&evicted,0));
    assert(xQueueReceive(&ready_q,&evicted,0) && evicted==2);
    assert(xQueueSend(&free_q,&evicted,0));
    assert(wake_capture_read(&ctx,pcm,NULL,480,100,&got)==ESP_ERR_INVALID_RESPONSE);
    assert(got==48 && ctx.consumer_chunk==3 && ctx.consumer_offset==0);
    for(unsigned i=0;i<48;i++)assert(pcm[i]==(int16_t)(192+i));
    // Detect once at the real gap; subsequent continuous data does not reset.
    for(unsigned k=0;k<3;k++) {
        assert(wake_capture_read(&ctx,pcm,NULL,480,100,&got)==ESP_OK && got==480);
        for(unsigned i=0;i<480;i++)assert(pcm[i]==(int16_t)(720+k*480+i));
    }
    assert(wake_capture_read(&ctx,pcm,NULL,240,100,&got)==ESP_OK && got==240);
    assert(free_q.count==10 && !ctx.consumer_holds_chunk && ready_q.count==0);
    // Sequence wrap is contiguous. A real DMA generation change still resets.
    ctx.consumer_sequence=UINT32_MAX;
    uint8_t index=10;ctx.chunks[index].sequence=0;ctx.chunks[index].generation=1;ctx.chunks[index].sample_count=240;
    assert(xQueueSend(&ready_q,&index,0));
    assert(wake_capture_read(&ctx,pcm,NULL,240,100,&got)==ESP_OK);
    index=11;ctx.chunks[index].sequence=1;ctx.chunks[index].generation=2;ctx.chunks[index].sample_count=240;
    assert(xQueueSend(&ready_q,&index,0));
    assert(wake_capture_read(&ctx,pcm,NULL,240,100,&got)==ESP_ERR_INVALID_RESPONSE && got==0);
    assert(wake_capture_read(&ctx,pcm,NULL,240,100,&got)==ESP_OK);
    assert(free_q.count==12 && ready_q.count==0 && !ctx.consumer_holds_chunk);
    DeleteCriticalSection(&free_q.lock);DeleteCriticalSection(&ready_q.lock);
}
int main(void) {
    check_gap_boundary();
    check_consumer_pressure();
    check(false,false,false);check(true,false,false);check(false,true,false);check(false,false,true);
    puts("PASS: eviction detected at head before mixed PCM, no false tail gaps, sequence wrap, DMA gap, ownership; consumer pressure, mono/dual capture, inference stalls and stop");
}
