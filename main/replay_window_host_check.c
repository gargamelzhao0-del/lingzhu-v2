#include <windows.h>
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include "audio_frame.h"
#include "sd_audio_read_recovery_host.h"
#ifndef CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN
#define CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN 4096
#endif
#ifndef CONFIG_LWIP_TCP_SND_BUF_DEFAULT
/* This fixture retains the independent PC-gateway memory trace. Formal
 * 128KiB admission/allocator limits run in test-sd-flow-control/formal-speed. */
#define CONFIG_LWIP_TCP_SND_BUF_DEFAULT 46080U
#endif
#include "sd_audio_protocol.h"
#include "sd_audio_scene_protocol.h"
#include "offline_replay_policy.h"
static unsigned s_replay_route = SD_UPLOAD_LEGACY;
#include "cJSON.h"
#include "offline_replay_identity.h"
#include "ws_send_metrics.h"
#include "ws_archive_batch.h"
#include "ws_close_policy.h"
#include "offline_io_schedule.h"
#ifndef CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_LANE_PROBE
#define CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_LANE_PROBE 1
#endif
#ifdef NUWAX_CRC_ROM_HOST_TEST
#include "esp_rom_crc.h"
#endif
#define ESP_ERR_INVALID_CRC 0x109
#define ESP_ERR_NOT_FOUND 0x105
#define TAG "test"
static unsigned long logged_ack_yields, logged_ack_yields_saved;
static unsigned long logged_reader_yields, logged_read_credits;
static unsigned long logged_sender_yields, logged_send_credits;
static void test_log(const char *tag,const char *fmt,...) {
    (void)tag;
    if(strncmp(fmt,"offline replay batching ",23)==0) {
        va_list args;va_start(args,fmt);
        for(unsigned i=0;i<4;i++)(void)va_arg(args,unsigned long);
        logged_ack_yields=va_arg(args,unsigned long);
        logged_ack_yields_saved=va_arg(args,unsigned long);
        va_end(args);
    }
    if(strncmp(fmt,"offline replay reader work ",27)==0) {
        va_list args;va_start(args,fmt);
        logged_reader_yields=va_arg(args,unsigned long);
        (void)va_arg(args,unsigned long);(void)va_arg(args,unsigned long);
        logged_read_credits=va_arg(args,unsigned long);
        va_end(args);
    }
    if(strncmp(fmt,"offline replay sender work ",27)==0) {
        va_list args;va_start(args,fmt);
        logged_sender_yields=va_arg(args,unsigned long);
        (void)va_arg(args,unsigned long);(void)va_arg(args,unsigned long);
        logged_send_credits=va_arg(args,unsigned long);va_end(args);
    }
}
#define ESP_LOGI(...) test_log(__VA_ARGS__)
#define ESP_LOGW(...) test_log(__VA_ARGS__)
#define HEADER_BYTES 96U
#define FOOTER_BYTES 32U
#define MAX_FRAME_BYTES 2048U
#define ARCHIVE_STDIO_BUFFER_BYTES 16384U
#define NUWAX_OFFLINE_AUDIO_MODE_MONITORING 1U
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
#define MALLOC_CAP_DMA 8
#define REPLAY_CHUNK_MAX_PAYLOAD_BYTES 8064U
#define REPLAY_WINDOW_INITIAL_BYTES 65536U
#define REPLAY_WINDOW_MAX_BYTES 524288U
#define REPLAY_CHUNK_HEADER_BYTES 64U
#define REPLAY_FORMAL_CHUNK_PAYLOAD_BYTES 3896U
#define REPLAY_CHUNK_YIELD_MS 1U
#define pdMS_TO_TICKS(ms) (ms)
#define portTICK_PERIOD_MS 10U
typedef unsigned UBaseType_t;
static _Thread_local UBaseType_t current_priority = 5U;
static atomic_uint priority_changes;
static UBaseType_t uxTaskPriorityGet(void *task) {(void)task;return current_priority;}
static void vTaskPrioritySet(void *task,UBaseType_t priority) {
    assert(!task);
    priority_changes++;
    current_priority=priority;
}
#define pdTRUE 1
#define pdPASS 1
#define portENTER_CRITICAL(lock) ((void)0)
#define portEXIT_CRITICAL(lock) ((void)0)
#define REPLAY_CHUNK_ACK_ATTEMPTS 3U
#define CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE 1
static atomic_bool s_formal_replay_cancel_requested;
static atomic_bool s_replay_lane_probe_taken;
static bool lane_probe_expected;
static void check_lane_before_send(void);
typedef struct { atomic_bool connected; atomic_uint epoch; } nuwax_ws_client_t;
#include "offline_archive_types_host.inc"
#include "replay_crc_host.inc"
#include "replay_frame_validator_host.inc"
#include "offline_replay_stream.inc"
static atomic_ullong clock_us;
static atomic_uint test_backlog;
static atomic_bool pressure_frozen;
static atomic_ullong pressure_until_us;
static unsigned pressure_action;
static nuwax_ws_client_t *pressure_ws;
static atomic_bool prefix_gate, prefix_gate_reached;
static uint32_t nuwax_wake_word_capture_backlog_ms(void) {
    if(prefix_gate && current_priority==5U && !prefix_gate_reached)return 90U;
    return atomic_load(&test_backlog);
}
static atomic_uint packets,heap_blocks;
typedef struct { HANDLE event; } StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
static atomic_uint signal_handles, signal_takes, signal_gives;
static SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *storage) {
    storage->event=CreateEvent(NULL,FALSE,FALSE,NULL);assert(storage->event);
    signal_handles++;return storage;
}
static int xSemaphoreTake(SemaphoreHandle_t signal,unsigned timeout) {
    if(!signal)return pdTRUE; /* Uncontended spool lock in this harness. */
    signal_takes++;
    const ULONGLONG started=GetTickCount64();
    int result=WaitForSingleObject(signal->event,timeout)==WAIT_OBJECT_0 ? pdTRUE : 0;
    clock_us+=(GetTickCount64()-started)*1000ULL;
    return result;
}
static void xSemaphoreGive(SemaphoreHandle_t signal) {
    if(signal){signal_gives++;assert(SetEvent(signal->event));}
}
static void vSemaphoreDelete(SemaphoreHandle_t signal) {
    assert(signal && CloseHandle(signal->event));signal_handles--;
}
static unsigned acks,armed,inject;
static unsigned ack_delay_us;
static _Thread_local bool after_ack;
static _Thread_local unsigned delays_after_ack;
static uint32_t persisted,window_start,window_end;
static atomic_bool busy;
static bool commit_timeout;
static atomic_bool monitoring_now;
static atomic_bool s_card_online = ATOMIC_VAR_INIT(true);
static bool monitor_after_ack, s_card_preempted;
static bool nuwax_sd_session_monitoring(void) { return atomic_load(&monitoring_now); }
static bool nuwax_sd_session_active(void) { return atomic_load(&monitoring_now); }
static bool replay_bulk_cancelled(void);
static unsigned commits;
static char sent_segment[48];
static uint8_t expected[4400000];
static uint32_t expected_len;
static size_t free_psram=3U*1024U*1024U;
static size_t free_internal=32768U;
static void *internal_allocation;
static size_t internal_allocation_bytes;
static void *hot_allocation;
static size_t hot_allocation_bytes;
static bool fail_hot, pressure_after_hot;
static size_t internal_overhead, internal_largest=32768U;
static unsigned internal_attempts, fail_internal_attempts;
static unsigned largest_queries;
static bool fail_internal, pressure_after_internal;
static unsigned internal_reads, fallback_reads;
static bool fail_read_once, short_read_once;
static bool probe_read_preempt;
static size_t heap_caps_get_free_size(int caps) {return caps & MALLOC_CAP_INTERNAL ? free_internal : free_psram;}
static size_t heap_caps_get_largest_free_block(int caps) {
    if(caps==MALLOC_CAP_SPIRAM)return free_psram;
    assert(caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA));
    largest_queries++;
    return internal_largest<free_internal?internal_largest:free_internal;
}
static bool heap_fail_all,heap_fail_context;
static bool heap_fail_batch;
static size_t psram_pressure_bytes;
static unsigned psram_pressure_attempts, bulk_allocation_attempts;
static void *bulk_allocation;
static size_t bulk_allocation_bytes;
static size_t replay_buffer_size;
static void *heap_caps_malloc(size_t n,int caps) {
    if(heap_fail_all || (heap_fail_context && n<8192))return NULL;
    if(caps== (MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT) && n>=2U*8192U+8206U) {
        assert(!bulk_allocation);
        bulk_allocation_attempts++;
        if(heap_fail_batch && n>280000U)return NULL;
        replay_buffer_size=n;
        if(n+8U>free_psram)return NULL;
        bulk_allocation=malloc(n);if(!bulk_allocation)return NULL;
        bulk_allocation_bytes=n+8U;free_psram-=bulk_allocation_bytes;
        if(psram_pressure_attempts){
            assert(free_psram>=psram_pressure_bytes);
            free_psram-=psram_pressure_bytes;psram_pressure_attempts--;
        }
        heap_blocks++;return bulk_allocation;
    }
    if(caps & MALLOC_CAP_INTERNAL) {
        if(caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)) {
            assert(!hot_allocation && n<=128 && free_internal>=n+16384);
            if(fail_hot)return NULL;
            hot_allocation=malloc(n);if(!hot_allocation)return NULL;
            hot_allocation_bytes=n+4;free_internal-=hot_allocation_bytes;
            if(pressure_after_hot){free_internal-=4096;pressure_after_hot=false;}
            heap_blocks++;return hot_allocation;
        }
        assert(caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA));
        assert(!internal_allocation && n<=8192 && free_internal>=n+16384);
        internal_attempts++;
        if(fail_internal)return NULL;
        if(fail_internal_attempts){fail_internal_attempts--;return NULL;}
        if(n>internal_largest)return NULL;
        assert(free_internal>=n+internal_overhead);
        internal_allocation=malloc(n);if(!internal_allocation)return NULL;
        internal_allocation_bytes=n+internal_overhead;free_internal-=internal_allocation_bytes;
        if(pressure_after_internal){free_internal-=4096;pressure_after_internal=false;}
        heap_blocks++;return internal_allocation;
    }
    void *p=malloc(n);if(p)heap_blocks++;return p;
}
static void heap_caps_free(void *p) {
    assert(priority_changes==0U); // Read, error and cleanup paths never change priority.
    if(p && p==bulk_allocation){free_psram+=bulk_allocation_bytes;bulk_allocation=NULL;bulk_allocation_bytes=0;}
    if(p && p==hot_allocation){free_internal+=hot_allocation_bytes;hot_allocation=NULL;hot_allocation_bytes=0;}
    if(p){heap_blocks--;if(p==internal_allocation){free_internal+=internal_allocation_bytes;internal_allocation=NULL;internal_allocation_bytes=0;}}
    free(p);
}
static int64_t esp_timer_get_time(void) {clock_us+=100;return clock_us;}
static void vTaskDelay(unsigned ticks) {clock_us+=ticks*10000;
    if(after_ack){delays_after_ack++;after_ack=false;}
    unsigned long long until=atomic_load(&pressure_until_us);
    if(until && atomic_load(&clock_us)>=until &&
       atomic_compare_exchange_strong(&pressure_until_us,&until,0)) {
        if(pressure_action==1)busy=true;
        if(pressure_action==2)pressure_ws->connected=false;
        if(pressure_action==3)pressure_ws->epoch++;
        if(pressure_action==4)s_formal_replay_cancel_requested=true;
        atomic_store(&test_backlog,0);pressure_frozen=false;
    }
    unsigned backlog=atomic_load(&test_backlog);
    if(!pressure_frozen && backlog)atomic_store(&test_backlog,backlog>15?backlog-15:0);
    Sleep(1);}
static void nuwax_wake_word_wait_capture_headroom(uint32_t ms) {
    (void)ms;vTaskDelay(1U); /* RTOS boundary; event implementation has its own fault tests. */
}
static uint32_t esp_random(void) {static uint32_t n;return ++n;}
static bool nuwax_offline_audio_storage_busy(void) {return busy || (inject==3 && packets>3);}
static bool nuwax_ws_is_connected(nuwax_ws_client_t *ws) {return ws->connected;}
static uint32_t nuwax_ws_connection_epoch(nuwax_ws_client_t *ws) {return ws->epoch;}
static bool nuwax_ws_close_if_epoch(nuwax_ws_client_t *ws,uint32_t epoch,nuwax_ws_close_cause_t cause) {
    assert(cause==NUWAX_WS_CLOSE_CAUSE_OWNERSHIP_TRANSFER);
    if(ws->epoch!=epoch)return false;
    ws->connected=false;return true;
}
static char armed_request[48];
static bool armed_commit;
static FILE *packet_export;
static bool file_ack_test;
static bool bounded_probe;
static size_t bounded_bytes_read, bounded_corrupt_at = SIZE_MAX;
static unsigned file_finals;
static FILE *control_export;
static int replay_arm_ack(uint32_t epoch,const char *r,const char *t,const char *s) {
    after_ack=false;
    (void)epoch;(void)s;armed_commit=strstr(t,"_commit")!=NULL;strcpy(armed_request,r);armed++;window_start=window_end=UINT32_MAX;return ESP_OK;
}
static void replay_disarm_ack(void) {}
static int replay_send_frame(nuwax_ws_client_t *ws,uint32_t epoch,const uint8_t *p,size_t n,bool binary,bool independent) {
    if(lane_probe_expected && acks==0)check_lane_before_send();
    (void)independent;
    assert(binary&&ws->epoch==epoch);
    uint32_t version,off,len,flags; memcpy(&version,p+8,4);memcpy(&off,p+12,4);memcpy(&len,p+16,4);memcpy(&flags,p+60,4);
    assert(version==(file_ack_test?3U:2U) && n==len+64 && len<=8064 && off+len<=expected_len);
    assert(len>0);
    if(!file_ack_test)assert(!strcmp((const char *)p+20,armed_request));
    else {assert(!strcmp((const char *)p+20,(const char *)expected+36));uint32_t idx;memcpy(&idx,p+56,4);assert(idx==((const archive_header_t *)expected)->segment_index);}
    /* Existing v2 accepts any nonempty <=8064B payload, including a RAM-slot
     * tail inside a larger durable window. Only flags=1 persists the window. */
    if(bounded_probe && bounded_corrupt_at>=off && bounded_corrupt_at<off+len) {
        const size_t bad=bounded_corrupt_at-off;
        assert(!memcmp(p+64,expected+off,bad));
        assert(p[64+bad]==(uint8_t)(expected[off+bad]^0x40));
        assert(!memcmp(p+65+bad,expected+off+bad+1,len-bad-1));
    } else assert(memcmp(p+64,expected+off,len)==0);
    if(packet_export) {
        uint32_t count=(uint32_t)n;
        assert(fwrite(&count,1,4,packet_export)==4 && fwrite(p,1,n,packet_export)==n);
    }
    if(window_start==UINT32_MAX) window_start=off; else assert(window_end==off);
    window_end=off+len; packets++;
    if(inject==7 && packets==2) {
        atomic_store(&test_backlog,90);pressure_frozen=true;
        pressure_until_us=atomic_load(&clock_us)+2500000ULL;
    }
    if(inject==4 && packets==2) {ws->connected=false;return ESP_FAIL;}
    if(inject==5 && packets==2) {ws->epoch++;return ESP_ERR_INVALID_STATE;}
    if(inject==8 && packets==2) return ESP_ERR_NO_MEM;
    if(inject==9 && packets==2) monitoring_now=true;
    if(inject==10 && packets==2) s_card_online=false;
    if(inject==11 && packets==2) s_formal_replay_cancel_requested=true;
    if(inject==12 && packets==2) busy=true;
    if(file_ack_test) { assert(flags==(off+len==expected_len?1U:0U)); if(flags)file_finals++; }
    if(flags) persisted=window_end;
    return ESP_OK;
}
static void check_prefetch_during_ack(void);
static int replay_send_frame_buffered(nuwax_ws_client_t *ws, uint32_t epoch,
    const uint8_t *p, size_t n, bool chunk, bool independent, uint8_t *scratch,
    size_t capacity, nuwax_ws_send_metrics_t *metrics) {
    assert(chunk && !independent && !scratch && !capacity && metrics && metrics->shared_tls_archive);
    assert(n + 8U <= 3968U && n + 8U <= CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN);
    ++metrics->write_calls;
    return replay_send_frame(ws,epoch,p,n,true,false);
}
static bool prefetch_probe;
static int replay_wait_ack(nuwax_ws_client_t *ws,uint32_t epoch,const char *r,bool chunk,bool bulk_wait,uint32_t *off) {
    (void)ws;(void)epoch;(void)r;
    if(!chunk) {if(off)*off=0;return !off && armed_commit && commit_timeout ? ESP_ERR_TIMEOUT : ESP_OK;}
    assert(bulk_wait);
    acks++;
    if(monitor_after_ack && acks==1) monitoring_now=true;
    if(bulk_wait && replay_bulk_cancelled())return ESP_ERR_NOT_FINISHED;
    if(prefetch_probe && acks==1)check_prefetch_during_ack();
    *off=persisted+(inject==2 ? 1U:0U);
    if(inject==1 && acks==1) return ESP_ERR_TIMEOUT;
    if(inject==6)return ESP_ERR_TIMEOUT;
    clock_us+=ack_delay_us;after_ack=true;
    return ESP_OK;
}
static unsigned transport_deferrals, defer_action;
static unsigned simulated_tx_wait_us;
static unsigned first_group;
static int nuwax_ws_send_archive_parts_if_epoch(nuwax_ws_client_t *ws,
    const uint8_t *header,const uint8_t *data,size_t n,unsigned timeout,uint32_t epoch,
    uint8_t *scratch,size_t capacity,nuwax_ws_send_metrics_t *metrics) {
    assert(scratch && capacity>=n+64U+14U && metrics && (timeout==1500U || timeout==250U));
    if (timeout==250U) assert(!metrics->bounded_progress && metrics->shared_tls_archive && metrics->cancelled);
    else assert(metrics->cancelled);
    if(metrics->cancelled && metrics->cancelled())
        return metrics->shared_tls_archive ? ESP_ERR_NOT_FINISHED : ESP_ERR_INVALID_STATE;
    metrics->operation_wait_max_us=simulated_tx_wait_us;
    if(!packets && !first_group)first_group=1U;
    if(transport_deferrals) {
        transport_deferrals--;
        if(defer_action==1)busy=true;
        if(defer_action==2)ws->connected=false;
        if(defer_action==3)ws->epoch++;
        return ESP_ERR_TIMEOUT;
    }
    uint8_t packet[8128];assert(n<=8064U);
    memcpy(packet,header,64U);memcpy(packet+64U,data,n);
    return replay_send_frame(ws,epoch,packet,n+64U,true,true);
}
static unsigned batch_calls;
static int nuwax_ws_send_archive_batch_if_epoch(nuwax_ws_client_t *ws,
    const nuwax_ws_archive_frame_t *frames,size_t count,unsigned timeout,uint32_t epoch,
    uint8_t *scratch,size_t capacity,nuwax_ws_send_metrics_t *metrics) {
    assert(count>1U && count<=NUWAX_WS_ARCHIVE_BATCH_MAX_FRAMES);
    batch_calls++;
    if(!packets && !first_group)first_group=(unsigned)count;
    for(size_t i=0;i<count;i++) {
        uint32_t flags;memcpy(&flags,frames[i].header+60,4);
        if(i+1U<count)assert(flags==0U);
        int err=nuwax_ws_send_archive_parts_if_epoch(ws,frames[i].header,
            frames[i].data,frames[i].bytes,timeout,epoch,scratch,capacity,metrics);
        if(err!=ESP_OK)return err;
    }
    return ESP_OK;
}
#define REPLAY_SEND_RETRY_ATTEMPTS 8U
#define REPLAY_SEND_RETRY_DELAY_MS 5U
#define REPLAY_ISOLATED_IO_TIMEOUT_MS 1500U
typedef unsigned TickType_t;
#include "replay_parts_host.inc"
typedef struct {HANDLE thread;void (*fn)(void *);void *arg; unsigned priority;} *TaskHandle_t;
enum {eRunning,eSuspended};
static bool task_create_fail;
static unsigned workers_created,workers_joined;
static void *current_reader;
static DWORD WINAPI read_worker(void *arg) {TaskHandle_t t=arg;current_priority=t->priority;t->fn(t->arg);return 0;}
static int xTaskCreateWithCaps(void (*fn)(void *),const char *name,unsigned stack,void *arg,
    unsigned priority,TaskHandle_t *out,unsigned caps) {
    (void)name;(void)caps;assert(stack==8192 && priority==OFFLINE_REPLAY_READER_PRIORITY);
    if(task_create_fail)return 0;
    TaskHandle_t t=calloc(1,sizeof(*t));assert(t);t->fn=fn;t->arg=arg;t->priority=priority;
    current_reader=arg;
    t->thread=CreateThread(NULL,0,read_worker,t,0,NULL);assert(t->thread);*out=t;workers_created++;return pdPASS;
}
static void vTaskSuspend(TaskHandle_t task) {assert(!task);}
static int xTaskCreatePinnedToCoreWithCaps(void (*fn)(void *),const char *name,unsigned stack,void *arg,
    unsigned priority,TaskHandle_t *out,int core,unsigned caps) {
    assert(core==0);return xTaskCreateWithCaps(fn,name,stack,arg,priority,out,caps);
}
static int eTaskGetState(TaskHandle_t task) {return WaitForSingleObject(task->thread,0)==WAIT_OBJECT_0?eSuspended:eRunning;}
static void vTaskDeleteWithCaps(TaskHandle_t task) {
    assert(WaitForSingleObject(task->thread,0)==WAIT_OBJECT_0);
    CloseHandle(task->thread);free(task);workers_joined++;
}
#define QUEUE_DEPTH 128U
static uint32_t replay_capture_queue_depth(void) { return 0U; }
#include "offline_replay_pressure.inc"
static atomic_ullong replay_bytes_read;
static bool streaming_probe;
static uint32_t streaming_read_limit;
static unsigned simulated_read_wait_us;
static unsigned record_reads, record_cancel_after;
static nuwax_ws_client_t *record_epoch_change;
static ssize_t replay_test_read(int fd,void *p,size_t count) {
    clock_us+=simulated_read_wait_us;
    after_ack=false; // Subsequent serial read work has its own fairness delays.
    if(streaming_probe && replay_bytes_read>=streaming_read_limit && packets==0) {
        prefix_gate_reached=true;
        // Block the second upload read until the first network packet arrives.
        // Whole-window publication deadlocks here; incremental publication does not.
        const DWORD started=GetTickCount();
        while(packets==0 && GetTickCount()-started<3000)Sleep(1);
        assert(packets>0);
    }
    if(probe_read_preempt && replay_bytes_read>=16384U)busy=true;
    if(p==internal_allocation){internal_reads++;assert(free_internal>=16384);assert(lseek(fd,0,SEEK_CUR)%512==0);}else fallback_reads++;
    if(fail_read_once){fail_read_once=false;errno=EIO;return -1;}
    if(short_read_once){short_read_once=false;count/=2;}
    const ssize_t got=read(fd,p,(unsigned)count);
    if(got>0) {
        replay_bytes_read+=(size_t)got;
        if(record_cancel_after && ++record_reads==record_cancel_after) {
            if(record_epoch_change)record_epoch_change->epoch++;
            else atomic_store(&s_formal_replay_cancel_requested,true);
        }
        if(bounded_probe) {
            if(bounded_corrupt_at>=bounded_bytes_read&&bounded_corrupt_at<bounded_bytes_read+(size_t)got)
                ((uint8_t*)p)[bounded_corrupt_at-bounded_bytes_read]^=0x40;
            bounded_bytes_read+=(size_t)got;
        }
    }
    return got;
}
/* Windows host shim for POSIX pread. Production uses IDF FatFS pread, which
 * locks the file and restores its position. No reader worker runs at metadata. */
static unsigned metadata_fault, metadata_calls;
static size_t metadata_bytes;
static ssize_t replay_test_pread(int fd, void *out, size_t count, off_t offset) {
    ++metadata_calls;
    if (metadata_fault == 1) { metadata_fault=0; errno=EINTR; return -1; }
    if (metadata_fault == 2) { metadata_fault=0; count=count>7?7:count; }
    if (metadata_fault == 3) { errno=EIO; return -1; }
    const off_t before=lseek(fd,0,SEEK_CUR); assert(before>=0);
    assert(lseek(fd,offset,SEEK_SET)==offset);
    const ssize_t got=read(fd,out,(unsigned)count);
    const int saved=errno;
    assert(lseek(fd,before,SEEK_SET)==before);
    errno=saved;
    if(got>0)metadata_bytes+=(size_t)got;
    return got;
}
#define pread replay_test_pread
#define read replay_test_read
static void nuwax_sd_card_mark_io_fault(int err);
#include "offline_replay_read.inc"
/* Scheduler races/priority restoration are tested with production code in
 * test-replay-ui-schedule.py; this harness has no live UI owner. */
#include "offline_replay_prefetch.inc"
#define CONFIG_NUWAX_SD_SESSION_ENABLE 1
#include "replay_window_host.inc"
#undef CONFIG_NUWAX_SD_SESSION_ENABLE
#undef read
#undef pread
static void check_prefetch_during_ack(void) {
    replay_prefetch_t *p=current_reader;
    if(p->lane_probe) {
        assert(atomic_load(&p->probe_read_done));
        assert(atomic_load(&p->slots[0].published)==131072U);
        Sleep(20);
        assert(!atomic_load(&p->slots[1].ready) && replay_bytes_read==131072U);
        return;
    }
    for(unsigned i=0;i<3000 && !atomic_load(&p->slots[1].ready);i++)Sleep(1);
    assert(atomic_load(&p->slots[0].ready) && atomic_load(&p->slots[1].ready));
    assert(atomic_load(&p->slots[0].published)==p->slots[0].count);
    assert(p->slots[1].position==p->slots[0].position+p->slots[0].count);
    Sleep(20); // Reader has time to try wrapping; unacknowledged slot must survive.
    assert(memcmp(p->slots[0].data,expected+p->slots[0].position,p->slots[0].count)==0);
}
static void check_lane_before_send(void) {
    replay_prefetch_t *p=current_reader;
    assert(p->lane_probe && atomic_load(&p->probe_read_done));
    assert(replay_bytes_read==131072U && !atomic_load(&p->slots[1].ready));
}
#define ROOT_DIR "test-directory"
typedef struct { unsigned next; int error; } DIR;
struct dirent { char d_name[256]; };
static struct { atomic_bool degraded; atomic_uint ready_count; SemaphoreHandle_t lock; uint64_t used_bytes;
    struct {uint32_t window_bytes; sd_scene_ack_t scene;} replay_ack; } s;
static atomic_bool s_quarantine_recheck_pending = ATOMIC_VAR_INIT(true);
static atomic_bool s_directory_dirty;
static size_t records_test_fread(void *out, size_t size, size_t count, FILE *file) {
    size_t got=fread(out,size,count,file);
    if(bounded_probe) {
        if(bounded_corrupt_at >= bounded_bytes_read && bounded_corrupt_at < bounded_bytes_read+got*size)
            ((uint8_t*)out)[bounded_corrupt_at-bounded_bytes_read] ^= 0x40;
        bounded_bytes_read += got*size;
    }
    if(record_cancel_after && ++record_reads==record_cancel_after) {
        if(record_epoch_change)record_epoch_change->epoch++;
        else atomic_store(&s_formal_replay_cancel_requested,true);
    }
    return got;
}
#define fread records_test_fread
#include "replay_records_host.inc"
#undef fread
typedef struct {uint32_t payload_crc,total_bytes,index;char path[64];uint8_t sealed,realtime;bool missing,acknowledged;uint64_t start_ms,end_ms;} host_segment_t;
typedef host_segment_t sd_audio_segment_t;
#include "replay_recovery_host.inc"
static unsigned scans;
static bool include_bad;
static DIR directory;
static DIR *opendir(const char *p) {assert(strcmp(p,ROOT_DIR)==0);scans++;directory.next=0;return &directory;}
static struct dirent *readdir(DIR *d) {static struct dirent entry;
    if(d->next>=200) {
        if(include_bad && d->next++==200) {strcpy(entry.d_name,"aaa.ready.bad");return &entry;}
        return NULL;
    }
    snprintf(entry.d_name,sizeof(entry.d_name),"offline_%08u.ready",199-d->next++);return &entry;}
static int closedir(DIR *d) {(void)d;return 0;}
/* Pagination tests mock enumeration; real two-root traversal has its own I/O test. */
typedef DIR archive_dir_t;
static archive_dir_t *archive_dir_open(archive_dir_t *d) {(void)d;return opendir(ROOT_DIR);}
static struct dirent *archive_dir_read(archive_dir_t *d) {return readdir(d);}
static void archive_dir_close(archive_dir_t *d) {(void)closedir(d);}

static atomic_uint media_faults;
static void nuwax_sd_card_mark_io_fault(int err) {assert(err!=ESP_OK);atomic_fetch_add(&media_faults,1U);}
static void log_sd_step(const char *step,int64_t started) {(void)step;(void)started;}
static bool nuwax_audio_uplink_background_ready(void) {return true;}
static esp_err_t replay_send_json(nuwax_ws_client_t *ws,uint32_t epoch,const cJSON *json,bool independent) {
    (void)ws;(void)epoch;(void)independent;
    if (s_replay_route == SD_UPLOAD_LEGACY) strcpy(sent_segment,cJSON_GetObjectItemCaseSensitive(json,"segment_id")->valuestring);
    else {
        assert(!cJSON_HasObjectItem(json,"segment_id"));
        assert(!cJSON_HasObjectItem(json,"capture_start_ms") && !cJSON_HasObjectItem(json,"capture_end_ms") && !cJSON_HasObjectItem(json,"capture_age_ms"));
        if(cJSON_HasObjectItem(json,"segment_index"))assert(sd_audio_file_key(sent_segment,cJSON_GetObjectItemCaseSensitive(json,"audio_session_id")->valuestring,
            (uint32_t)cJSON_GetObjectItemCaseSensitive(json,"segment_index")->valuedouble));
    }
    const char *type=cJSON_GetObjectItemCaseSensitive(json,"type")->valuestring;
    if (control_export) {
        char *text=cJSON_PrintUnformatted(json);assert(text);
        fprintf(control_export,"%s\n",text);cJSON_free(text);
    }
    if(strstr(type,"_commit"))commits++;
    if(s_replay_route!=SD_UPLOAD_LEGACY && strstr(type,"_begin")) {
        assert(!cJSON_HasObjectItem(json,"window_bytes"));
        strcpy(armed_request,cJSON_GetObjectItemCaseSensitive(json,"request_id")->valuestring);
        window_start=window_end=UINT32_MAX;
    }
    return ESP_OK;
}
static esp_err_t replay_exchange_chunk(nuwax_ws_client_t *ws,uint32_t epoch,const uint8_t *p,size_t n,
    const char *request,const char *segment,bool independent,uint32_t *ack) {
    (void)ws;(void)epoch;(void)request;(void)segment;(void)independent;
    uint32_t version,off,len;memcpy(&version,p+8,4);memcpy(&off,p+12,4);memcpy(&len,p+16,4);
    assert(version==1 && n==len+64 && off+len<=expected_len);
    assert(memcmp(p+64,expected+off,len)==0);*ack=off+len;return ESP_OK;
}
static bool card_live_pending(nuwax_ws_client_t *ws,uint32_t epoch) {(void)ws;(void)epoch;return false;}
typedef struct {uint8_t owner[32],mode;bool ended,done;char session_id[48];uint32_t boot_id,segment_count;uint64_t start_ms;} host_session_t;
typedef host_session_t sd_audio_session_t;
#define SD_AUDIO_SEGMENT_ATTEMPTED 2U
static host_session_t s_card_replay_session;
static host_segment_t s_card_replay_segment;
static uint32_t s_card_boot_id;
static uint64_t card_online_since_ms(void) { return 0; }
static bool typed_test_live;
static bool card_owner(uint8_t owner[32]) {memset(owner,0,32);return true;}
typedef struct {uint8_t bytes[40];} nuwax_sd_card_identity_t;
static bool nuwax_sd_card_get_identity(nuwax_sd_card_identity_t *id) {memset(id,0,sizeof(*id));return true;}
static esp_err_t sd_audio_catalog_load(const char *id,host_session_t *out) {
    assert(id[0]);memset(out,0,sizeof(*out));out->mode=expected[12];out->ended=true;strcpy(out->session_id,id);out->segment_count=3;return ESP_OK;
}
static esp_err_t sd_audio_catalog_read(const char *id,uint32_t index,host_segment_t *out) {
    assert(id[0] && index==2U);memset(out,0,sizeof(*out));memcpy(&out->payload_crc,expected+expected_len-8,4);
    out->index=index;out->total_bytes=expected_len;strcpy(out->path,"typed-file.ready");out->sealed=true;out->missing=false;return ESP_OK;
}
static int catalog_bind_result;
static esp_err_t sd_audio_catalog_bind(const char *id,uint32_t index,
    host_session_t *session,host_segment_t *segment,bool *reused) {
    *reused=false;if(catalog_bind_result)return catalog_bind_result;
    int err=sd_audio_catalog_load(id,session);
    return err==ESP_OK?sd_audio_catalog_read(id,index,segment):err;
}
static esp_err_t sd_audio_catalog_drop_backup(const char *path) { (void)path;return ESP_OK; }
static void sd_audio_catalog_note_deleted(const char *id,uint32_t index) {
    assert(id[0] && index==2U);
    assert(access("typed-file.ready",F_OK)!=0);
}
static bool sd_audio_segment_realtime(const host_session_t *session,const host_segment_t *segment,
    uint32_t boot,uint64_t now,uint64_t online) {
    (void)session;(void)segment;(void)boot;(void)now;(void)online;return typed_test_live;
}
static unsigned catalog_acks;
static int catalog_result;
static esp_err_t sd_audio_catalog_ack(const char *session,uint32_t index) {
    assert(session[0] && index==2U);catalog_acks++;return catalog_result;
}
static esp_err_t sd_audio_catalog_mark_attempted(const char *id,uint32_t index) {(void)id;(void)index;return ESP_OK;}
static esp_err_t sd_audio_catalog_reconcile_file(const host_session_t *scene,const host_segment_t *part,bool *changed) {
    (void)scene;(void)part;(void)changed;assert(!"unexpected index repair; real catalog cases run in r200");return ESP_FAIL;
}
#include "cloud_time.h"
static esp_err_t sd_audio_catalog_session_time(const host_session_t *session,
    uint32_t boot, const nuwax_cloud_time_anchor_t *anchor, bool stopped, uint64_t stop,
    sd_audio_session_time_t *out) {
    (void)session;(void)boot;(void)anchor;(void)stopped;(void)stop;
    memset(out,0,sizeof(*out));return ESP_OK;
}
static void *scene_host_calloc(size_t count,size_t bytes,unsigned caps) {(void)caps;return calloc(count,bytes);}
#define heap_caps_calloc scene_host_calloc
#define CONFIG_NUWAX_SD_SESSION_ENABLE 1
#include "sd_audio_scene_replay.inc"
#undef heap_caps_calloc
static atomic_uint s_mirror_staging_rings;
#define MIRROR_STAGING_FRAMES 768U
typedef struct { uint32_t sequence,timestamp_ms; uint8_t flags; int16_t samples[320]; } mirror_staged_frame_t;
#define fread records_test_fread
#include "offline_replay_shared.inc"
#undef fread
#define heap_caps_calloc scene_host_calloc
/* Scene/pending mocks use their own small calloc objects, outside the DMA/read
 * allocator accounting. r200 checks their real lifetime and ACK fault paths. */
static void pending_host_free(void *p) {free(p);}
#define heap_caps_free pending_host_free
#include "sd_audio_pending_commit.inc"
#undef heap_caps_free
#undef heap_caps_calloc
#include "replay_commit_host.inc"
#undef CONFIG_NUWAX_SD_SESSION_ENABLE
#include "replay_directory_host.inc"
static bool keep_checkpoint;
static void reset(unsigned mode) {card_commit_saved();card_commit_release_idle();card_scene_reset();if(!keep_checkpoint)replay_checkpoint_invalidate();packets=acks=armed=persisted=0;busy=false;s_card_online=true;s_formal_replay_cancel_requested=false;inject=mode;clock_us=0;s_replay_lane_probe_taken=true;lane_probe_expected=false;}
static void fixture(const char *path) {
    FILE *f=fopen(path,"wb");assert(f);
    assert(fwrite(expected,1,expected_len,f)==expected_len);assert(fclose(f)==0);
}
static void check_metadata(void) {
    const char *path="metadata.ready";fixture(path);
    FILE *f=fopen(path,"rb");assert(f);
    char stdio_buffer[ARCHIVE_STDIO_BUFFER_BYTES];
    assert(setvbuf(f,stdio_buffer,_IOFBF,sizeof(stdio_buffer))==0);
    for(unsigned fault=0;fault<4;fault++) {
        uint8_t header[HEADER_BYTES],footer[FOOTER_BYTES];
        metadata_fault=fault;metadata_bytes=metadata_calls=0;
        const int err=replay_metadata_read_at(f,header,sizeof(header),0);
        if(fault==3)assert(err==ESP_FAIL);
        else {
            assert(err==ESP_OK && memcmp(header,expected,sizeof(header))==0);
            assert(replay_metadata_read_at(f,footer,sizeof(footer),expected_len-sizeof(footer))==ESP_OK);
            assert(memcmp(footer,expected+expected_len-sizeof(footer),sizeof(footer))==0);
            assert(metadata_bytes==HEADER_BYTES+FOOTER_BYTES);
            assert(metadata_calls==(fault?3U:2U));
        }
        assert(lseek(fileno(f),0,SEEK_CUR)==0 && ftell(f)==0);
    }
    metadata_fault=0;
    uint8_t bytes[HEADER_BYTES];
    assert(replay_metadata_read_at(f,bytes,sizeof(bytes),expected_len-1)==ESP_ERR_INVALID_SIZE);
    // Positional metadata never consumes the subsequent buffered v1 read.
    assert(fread(bytes,1,sizeof(bytes),f)==sizeof(bytes));
    assert(memcmp(bytes,expected,sizeof(bytes))==0);
    fclose(f);assert(remove(path)==0);
}
static void check_commit(void) {
    char ready_id[48]={0};
    for(unsigned mode=0;mode<6;mode++) {
        reset(0);commits=0;commit_timeout=mode==3;
        const char *path=mode==0 ? "session_00000001.ready" : "session_00000001.ready.bad";
        s.replay_ack.window_bytes=mode==5 ? 0:524288;
        s.used_bytes=expected_len;atomic_store(&s.ready_count,5);
        if(mode==2)expected[140]^=1; // Valid frame geometry, invalid footer CRC.
        fixture(path);FILE *f=fopen(path,"rb");assert(f);bool closed=false;
        char stdio_buffer[ARCHIVE_STDIO_BUFFER_BYTES];
        assert(setvbuf(f,stdio_buffer,_IOFBF,sizeof(stdio_buffer))==0);
        if(mode==4)inject=3; // Recording starts in the middle of transmission.
        nuwax_ws_client_t ws={true,7};
        const int err=replay_one_ready_file(&ws,7,path,true,f,&closed);
        if(!closed)fclose(f);
        struct stat st;
        if(mode==0 || mode==1 || mode==5) {
            assert(err==ESP_OK && commits==1 && stat(path,&st)!=0 && errno==ENOENT);
            assert(atomic_load(&s.ready_count)==(mode==0?4U:5U));assert(s.used_bytes==0);
        } else {
            assert(err==(mode==2?ESP_ERR_INVALID_CRC:mode==3?ESP_ERR_TIMEOUT:ESP_ERR_NOT_FINISHED));
            assert(commits==(mode==3?1U:0U) && stat(path,&st)==0);
            if(mode==2)assert(packets>0 && persisted<expected_len && commits==0); // Late CRC failure cannot commit/delete.
            assert(atomic_load(&s.ready_count)==5 && s.used_bytes==expected_len);remove(path);
        }
        if(mode==0)strcpy(ready_id,sent_segment);else assert(strcmp(ready_id,sent_segment)==0);
        if(mode==2)expected[140]^=1;
        assert(heap_blocks==0);
    }
    reset(0);commit_timeout=false;
    // Metadata I/O and truncation fail before any network transaction or delete.
    for(unsigned failure=0;failure<2;failure++) {
        const char *path="metadata-failure.ready";fixture(path);
        if(failure) {
            FILE *out=fopen(path,"wb");assert(out);
            assert(fwrite(expected,1,80,out)==80);fclose(out);
        }
        FILE *f=fopen(path,"rb");assert(f);bool closed=false;
        nuwax_ws_client_t ws={true,7};reset(0);commits=0;
        metadata_fault=failure?0:3;
        assert(replay_one_ready_file(&ws,7,path,true,f,&closed)==
               (failure?ESP_ERR_INVALID_SIZE:ESP_FAIL));
        assert(!closed && !commits && !packets && !armed);
        struct stat st;assert(stat(path,&st)==0);
        metadata_fault=0;fclose(f);assert(remove(path)==0);
    }
}
static void check_formal_pressure_cancel(void) {
    reset(0);commits=0;s.replay_ack.window_bytes=0;
    const char *path="formal-pressure.ready";fixture(path);
    FILE *file=fopen(path,"rb");assert(file);bool closed=false;
    nuwax_ws_client_t ws={true,7};
    atomic_store(&test_backlog,90);pressure_frozen=true;pressure_action=4;
    pressure_until_us=atomic_load(&clock_us)+2500000ULL;
    assert(replay_one_ready_file(&ws,7,path,false,file,&closed)==ESP_ERR_NOT_FINISHED);
    assert(!closed && !commits && ws.connected && ws.epoch==7);
    assert(s_formal_replay_cancel_requested && heap_blocks==0);
    fclose(file);assert(remove(path)==0);
    s_formal_replay_cancel_requested=false;pressure_action=0;
}
static void check_recovery(void) {
    atomic_store(&s.ready_count,0);
    const unsigned baseline=heap_blocks;
    char path[160]="recovery.ready.bad";
    remove("recovery.ready");remove(path);fixture(path);
    busy=true;assert(restore_quarantined_archive(path)==ESP_ERR_NOT_FINISHED);busy=false;
    assert(restore_quarantined_archive(path)==ESP_OK);
    assert(strcmp(path,"recovery.ready")==0 && atomic_load(&s.ready_count)==1);
    assert(recheck_archive_path(path)==ESP_OK && atomic_load(&s_directory_dirty));
    archive_header_t indexed_header;archive_footer_t indexed_footer;
    memcpy(&indexed_header,expected,sizeof(indexed_header));
    memcpy(&indexed_footer,expected+expected_len-sizeof(indexed_footer),sizeof(indexed_footer));
    sd_audio_segment_t indexed={.index=indexed_header.segment_index,.total_bytes=expected_len,.payload_crc=indexed_footer.crc32};
    strcpy(indexed.path,path);assert(recheck_archive_expected(&indexed)==ESP_OK);
    indexed.payload_crc^=1U;assert(recheck_archive_expected(&indexed)==ESP_ERR_INVALID_CRC);
    indexed.payload_crc^=1U;indexed.total_bytes++;assert(recheck_archive_expected(&indexed)==ESP_ERR_INVALID_SIZE);
    strcpy(path,"recovery.ready.bad");fixture(path);
    assert(restore_quarantined_archive(path)==ESP_ERR_INVALID_STATE); // Never overwrite.
    assert(recheck_archive_path(path)==ESP_OK);remove("recovery.ready");
    // Header geometry, PCM content CRC and truncation are independently rejected.
    uint8_t saved=expected[0];expected[0]^=1;fixture(path);expected[0]=saved;
    assert(restore_quarantined_archive(path)==ESP_ERR_INVALID_SIZE);
    saved=expected[140];expected[140]^=1;fixture(path);expected[140]=saved;
    assert(restore_quarantined_archive(path)==ESP_ERR_INVALID_CRC);
    FILE *f=fopen(path,"wb");assert(f);assert(fwrite(expected,1,100,f)==100);fclose(f);
    assert(restore_quarantined_archive(path)==ESP_ERR_INVALID_SIZE);remove(path);
    assert(recheck_archive_path(path)==ESP_FAIL && heap_blocks==baseline);
    assert(atomic_load(&s.ready_count)==1);
}
static uint32_t reference_crc32(uint32_t crc,const uint8_t *p,size_t n) {
    crc=~crc;
    for(size_t i=0;i<n;i++) {
        crc^=p[i];
        for(unsigned bit=0;bit<8;bit++)crc=(crc>>1)^((crc&1U)?0xedb88320U:0U);
    }
    return ~crc;
}
static void check_crc_compatibility(void) {
    assert(crc32_update(0,"123456789",9)==0xcbf43926U);
    uint8_t bytes[8197];
    for(unsigned i=0;i<sizeof(bytes);i++)bytes[i]=(uint8_t)(i*73U+(i>>3));
    const uint32_t seeds[]={0,0xffffffffU,0x1256abefU};
    for(unsigned s=0;s<3;s++)for(unsigned offset=0;offset<4;offset++) {
        const size_t n=sizeof(bytes)-offset;
        uint32_t want=reference_crc32(seeds[s],bytes+offset,n);
        assert(crc32_update(seeds[s],bytes+offset,n)==want);
        uint32_t got=seeds[s];
        for(size_t at=0;at<n;) {
            size_t count=(at%37)+1;if(count>n-at)count=n-at;
            got=crc32_update(got,bytes+offset+at,count);at+=count;
        }
        assert(got==want && crc32_update(got,NULL,0)==got);
    }
}
static void check_capture_pressure(void) {
    nuwax_ws_client_t ws={true,7};uint64_t waited=0;atomic_bool cancel=false;
    atomic_store(&test_backlog,59);
    assert(replay_wait_capture_headroom(&ws,7,&cancel,&waited)==ESP_OK && waited==0);
    atomic_store(&test_backlog,90);
    assert(replay_wait_capture_headroom(&ws,7,&cancel,&waited)==ESP_OK && waited>0 && test_backlog<=30);
    atomic_store(&test_backlog,90);atomic_store(&cancel,true);
    assert(replay_wait_capture_headroom(&ws,7,&cancel,&waited)==ESP_ERR_NOT_FINISHED);
    atomic_store(&cancel,false);ws.epoch=8;
    assert(replay_wait_capture_headroom(&ws,7,&cancel,&waited)==ESP_ERR_INVALID_STATE);
    ws.epoch=7;busy=true;
    assert(replay_wait_capture_headroom(&ws,7,&cancel,&waited)==ESP_ERR_NOT_FINISHED);busy=false;
    for(unsigned action=0;action<4;action++) {
        ws.connected=true;ws.epoch=7;busy=false;pressure_ws=&ws;pressure_action=action;
        atomic_store(&test_backlog,90);pressure_frozen=true;
        pressure_until_us=atomic_load(&clock_us)+2500000ULL;
        const uint64_t before=waited;
        assert(replay_wait_capture_headroom(&ws,7,&cancel,&waited)==
               (action==0?ESP_OK:action==1?ESP_ERR_NOT_FINISHED:ESP_ERR_INVALID_STATE));
        assert(waited-before>=2500000ULL);
    }
    pressure_action=0;busy=false;ws.connected=true;ws.epoch=7;
    // Cancellation/epoch fences also apply below the pressure threshold.
    atomic_store(&cancel,true);
    assert(replay_wait_capture_headroom(&ws,7,&cancel,&waited)==ESP_ERR_NOT_FINISHED);
    atomic_store(&cancel,false);ws.epoch=8;
    assert(replay_wait_capture_headroom(&ws,7,&cancel,&waited)==ESP_ERR_INVALID_STATE);
    pressure_frozen=false;atomic_store(&test_backlog,0);
}
static void check_header_equivalence(const archive_header_t *archive) {
    _Alignas(int16_t) uint8_t frame[662];
    for(unsigned seen_case=0;seen_case<3;seen_case++) {
        for(unsigned at=0;at<NUWAX_AUDIO_FRAME_HEADER_BYTES;at++) {
            for(unsigned byte=0;byte<256;byte++) {
                memcpy(frame,expected+HEADER_BYTES+4,sizeof(frame));frame[at]=(uint8_t)byte;
                uint32_t previous=seen_case==2?UINT32_MAX:0;
                bool seen=seen_case!=0;
                replay_stream_t v={.length=sizeof(frame),.last_sequence=previous,.sequence_seen=seen};
                const int original=validate_archive_frame(frame,sizeof(frame),archive,&previous,&seen);
                const int streaming=replay_stream_frame_header(&v,frame,archive,640);
                assert((original==ESP_OK)==(streaming==ESP_OK));
                if(original==ESP_OK)assert(previous==v.last_sequence && seen==v.sequence_seen);
            }
        }
    }
}
static void check_formal_shared(FILE *file, const archive_header_t *h, const archive_footer_t *f) {
    const unsigned baseline=heap_blocks; /* Sorted directory cache may be retained. */
    /* Actual 4 MiB sender, reader, validator and framing. Zero data ACKs;
     * exactly one read pass even for an unaligned resume or a complete peer. */
    assert(replay_shared_payload_limit(false)==(CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN==8192 ? 8064U : 4024U));
    assert(replay_shared_payload_limit(true)==3896U);
    assert(replay_shared_read_capacity(65536U,true)==65536U);
    assert(replay_shared_read_capacity(8192U,false)==8192U);
    if(CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN==8192) {
        assert(replay_shared_read_capacity(65536U,false)==64512U);
        assert(replay_shared_read_capacity(32768U,false)==32256U);
    }
    for(unsigned serial=0;serial<2;serial++)for(unsigned scenario=0;scenario<8;scenario++) {
        reset(0);task_create_fail=serial!=0;file_ack_test=true;file_finals=0;
        s_replay_route=SD_UPLOAD_MEETING;window_start=window_end=UINT32_MAX;
        replay_bytes_read=0;monitoring_now=scenario==3;
        nuwax_ws_client_t ws={true,7};bool deferred=true;
        uint32_t resume=scenario==1?12345U:scenario==2?expected_len:0U;
        inject=scenario==4?11U:scenario==5?5U:scenario==6?12U:scenario==7?4U:0U;
        int err=replay_send_shared_file(&ws,7,file,"shared",expected_len,resume,h,f,&deferred);
        if(deferred || acks || armed || heap_blocks!=baseline || workers_created!=workers_joined)
            fprintf(stderr,"shared case serial=%u scenario=%u err=%d deferred=%u acks=%u armed=%u heap=%u workers=%u/%u\n",
                serial,scenario,err,deferred,acks,armed,(unsigned)heap_blocks,workers_created,workers_joined);
        assert(!deferred && !acks && !armed && heap_blocks==baseline && workers_created==workers_joined);
        if(scenario<4) {
            assert(err==ESP_OK && replay_bytes_read==expected_len && ws.connected);
            assert(file_finals==(scenario==2?0U:1U));
            if(scenario!=2)assert(persisted==expected_len);
            if(!serial && scenario<3 && CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN==8192)
                assert(packets==(expected_len-resume+8063U)/8064U);
        } else {
            assert(err==((scenario==4 || scenario==6)?ESP_ERR_NOT_FINISHED:scenario==7?ESP_FAIL:ESP_ERR_INVALID_STATE));
            assert(file_finals==0 && packets>=2);
            if(scenario==4 || scenario==6)assert(ws.connected && ws.epoch==7);
        }
    }
    reset(0);task_create_fail=false;monitoring_now=false;file_ack_test=true;
    s_replay_route=SD_UPLOAD_MEETING;window_start=window_end=UINT32_MAX;
    replay_bytes_read=0;streaming_probe=true;streaming_read_limit=8192U;
    nuwax_ws_client_t ws={true,7};bool deferred=false;
    assert(replay_send_shared_file(&ws,7,file,"shared",expected_len,0,h,f,&deferred)==ESP_OK);
    assert(replay_bytes_read==expected_len && !acks && heap_blocks==baseline);
    streaming_probe=false;
    reset(0);window_start=window_end=UINT32_MAX;heap_fail_all=true;
    assert(replay_send_shared_file(&ws,7,file,"shared",expected_len,0,h,f,&deferred)==ESP_ERR_NO_MEM);
    assert(deferred && !packets && heap_blocks==baseline);heap_fail_all=false;
    /* The actual low-memory sender validates one pass including every resumed
     * prefix, retains the file transaction and fits one existing TLS scratch. */
    for(unsigned scenario=0;scenario<10;scenario++) {
        reset(0);file_ack_test=true;s_replay_route=SD_UPLOAD_MEETING;file_finals=0;
        window_start=window_end=UINT32_MAX;bounded_probe=true;bounded_bytes_read=0;
        bounded_corrupt_at=scenario==8?200U:SIZE_MAX;
        uint32_t resume=scenario==1?12345U:scenario==2?expected_len:scenario==3?1U:0U;
        inject=scenario==4?11U:scenario==5?5U:scenario==6?12U:scenario==7?4U:0U;
        ws=(nuwax_ws_client_t){true,7};heap_fail_all=scenario==9;
        int e=replay_send_shared_bounded_file(&ws,7,file,"bounded",expected_len,resume,h,f);
        if(scenario<4) {
            assert(e==ESP_OK && bounded_bytes_read==expected_len && ws.connected);
            assert(file_finals==(scenario==2?0U:1U));
            assert(packets==(expected_len-resume+3895U)/3896U);
        } else if(scenario==8) assert(e==ESP_ERR_INVALID_CRC && !file_finals);
        else if(scenario==9) assert(e==ESP_ERR_NO_MEM && !packets && !bounded_bytes_read);
        else {
            assert(e==((scenario==4||scenario==6)?ESP_ERR_NOT_FINISHED:scenario==7?ESP_FAIL:ESP_ERR_INVALID_STATE));
            assert(!file_finals && packets>=2);
            if(scenario==4||scenario==6)assert(ws.connected&&ws.epoch==7);
        }
        assert(!acks && !armed && heap_blocks==baseline);
        bounded_probe=false;bounded_corrupt_at=SIZE_MAX;heap_fail_all=false;
    }
    puts("PASS actual bounded formal sender: single read, arbitrary/full resume, no data ACK, 3968-byte TLS fit, CRC rejection, cancel/epoch/storage/transport failures and allocation cleanup");
    reset(0);file_ack_test=true;s_replay_route=SD_UPLOAD_MEETING;file_finals=0;
    window_start=window_end=UINT32_MAX;replay_bytes_read=0;
    s_mirror_staging_rings=1;
    free_psram=replay_shared_memory_reserve()+2U*4096U+8142U;
    ws=(nuwax_ws_client_t){true,7};deferred=false;
    assert(replay_send_shared_file(&ws,7,file,"small-prefetch",expected_len,12345,h,f,&deferred)==ESP_OK);
    assert(!deferred&&replay_bytes_read==expected_len&&persisted==expected_len&&file_finals==1&&!acks&&heap_blocks==baseline);
    if(CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN==8192)assert(packets==(expected_len-12345U+4095U)/4096U);
    s_mirror_staging_rings=0;free_psram=3U*1024U*1024U;
    puts("PASS actual 4KiB double-buffer formal sender: unchanged reserve, one-pass full validation, unaligned resume, exact bytes, no data ACK and complete reader cleanup");
    /* Revalidation must stop during its first records, with no bulk scan or TX. */
    for(unsigned epoch=0;epoch<2;epoch++) {
        reset(0);ws.epoch=7;record_reads=0;record_cancel_after=3;
        record_epoch_change=epoch?&ws:NULL;
        assert(validate_archive_records(file,h,f,expected_len-32,&ws,7,
            &s_formal_replay_cancel_requested)==ESP_ERR_INVALID_STATE);
        assert(record_reads<=4 && !packets && ftell(file)<2048);
    }
    record_cancel_after=0;record_epoch_change=NULL;reset(0);file_ack_test=false;
    s_replay_route=SD_UPLOAD_LEGACY;
    puts("PASS: shared formal 4MiB single read, first-block streaming, unaligned/full resume, active monitor, cancel/epoch/storage/partial send, low-memory and bounded revalidation");
}
static void check_sd_backed_windows(void) {
    const unsigned heap_baseline=heap_blocks;
    archive_header_t h={0};memcpy(h.magic,"NWXOFF1",7);h.version=1;h.header_bytes=96;
    h.channels=1;h.codec=NUWAX_AUDIO_CODEC_PCM16;h.sample_rate=16000;h.frame_duration_ms=20;
    strcpy(h.session_id,"meeting_stream_host");h.reserved0=2;
    archive_footer_t f={0};memcpy(f.magic,"NWXEND1",7);f.version=1;f.footer_bytes=32;
    f.complete=1;
    memcpy(expected,&h,96);uint32_t pos=96;
    for(uint32_t i=0;i<6297;i++) {
        int16_t pcm[320];for(unsigned j=0;j<320;j++)pcm[j]=(int16_t)(i+j);
        size_t bytes=0;
        assert(nuwax_audio_frame_build_pcm16(expected+pos+4,sizeof(expected)-pos-4,pcm,320,i,i*20,0,16000,1,20,&bytes)==ESP_OK);
        uint32_t len=(uint32_t)bytes;memcpy(expected+pos,&len,4);
        f.crc32=crc32_update(f.crc32,expected+pos+4,len);pos+=4+len;f.frame_count++;f.payload_bytes+=len;
    }
    memcpy(expected+pos,&f,32);expected_len=pos+32;
    fixture("sd-backed-window.bin");
    FILE *file=fopen("sd-backed-window.bin","rb");assert(file);
    free_psram=1108227U+REPLAY_NETWORK_HEADROOM_BYTES;free_internal=32768U;prefetch_probe=false;task_create_fail=false;
    check_formal_shared(file,&h,&f);
    const uint32_t limits[]={524288,262144,200000,65536,524288,200000};
    for(unsigned k=0;k<6;k++) {
        reset(0);nuwax_ws_client_t ws={true,7};replay_bytes_read=0;
        free_psram=(k>=4?1087084U:1108227U)+REPLAY_NETWORK_HEADROOM_BYTES;
        uint32_t resume=(k==2 || k==5)?12345U:0U;
        if(k==4){packet_export=fopen("sd-backed-window.packets","wb");assert(packet_export);}
        assert(replay_send_windowed(&ws,7,file,"s",expected_len,resume,limits[k],&h,&f,NULL,NULL)==ESP_OK);
        if(packet_export){assert(fclose(packet_export)==0);packet_export=NULL;}
        uint32_t cap=limits[k]<131072U?limits[k]:131072U;
        if(k>=4 && cap>65536U)cap/=2U;
        uint32_t span=cap*(limits[k]/cap);
        assert(acks==(expected_len-resume+span-1U)/span && armed==acks);
        assert(persisted==expected_len && replay_bytes_read==expected_len && ws.connected);
        assert(heap_blocks==heap_baseline && signal_handles==0 && workers_created==workers_joined);
    }
    free_psram=1108227U+REPLAY_NETWORK_HEADROOM_BYTES;
    for(unsigned mode=1;mode<=12;mode++) {
        if(mode==7)continue;
        reset(mode);nuwax_ws_client_t ws={true,7};s_card_preempted=false;
        int err=replay_send_windowed(&ws,7,file,"s",expected_len,0,524288,&h,&f,NULL,NULL);
        assert(err==(mode==2?ESP_ERR_INVALID_RESPONSE:mode==3||mode>=9?ESP_ERR_NOT_FINISHED:
            mode==4?ESP_FAIL:mode==5?ESP_ERR_INVALID_STATE:mode==8?ESP_ERR_NO_MEM:ESP_ERR_TIMEOUT));
        assert(ws.epoch!=7 || !ws.connected); // No uncertain window can leak into another begin.
        assert(heap_blocks==heap_baseline && signal_handles==0 && workers_created==workers_joined);
        uint32_t durable=persisted;
        if(mode==1||mode==2||mode==6)assert(durable==524288U && acks==1);
        else assert(durable==0U);
        monitoring_now=false;keep_checkpoint=true;reset(0);keep_checkpoint=false;ws.connected=true;ws.epoch=8;
        replay_bytes_read=0;
        assert(replay_send_windowed(&ws,8,file,"s",expected_len,durable,524288,&h,&f,NULL,NULL)==ESP_OK);
        assert(persisted==expected_len && heap_blocks==heap_baseline);
        if(durable)assert(replay_bytes_read<=expected_len-durable+131072U);
    }
    // Seed an actual fragmented validator, then exercise production seek/read/TX.
    const uint32_t checkpoints[]={99U,111U,12345U,1048576U};
    for(unsigned k=0;k<4;k++) {
        reset(0);replay_checkpoint_t cache;
        replay_checkpoint_prepare(&cache,"checkpoint",expected_len,&h,&f);
        replay_stream_t v={0};const uint32_t at=checkpoints[k];
        assert(replay_stream_feed(&v,expected,at,expected_len,&h,&f)==ESP_OK);
        cache.states[0]=v;s_replay_checkpoint=cache;replay_bytes_read=0;
        nuwax_ws_client_t ws={true,9};
        assert(replay_send_windowed(&ws,9,file,"checkpoint",expected_len,at+3,524288,&h,&f,NULL,NULL)==ESP_OK);
        assert(replay_bytes_read==expected_len-at && persisted==expected_len);
        // CRC includes the cached prefix, even when a PCM record straddles it.
        assert(replay_stream_feed(&v,expected+at,100,expected_len,&h,&f)==ESP_OK);
        uint8_t changed=expected[at+100]^1;
        assert(replay_stream_feed(&v,&changed,1,expected_len,&h,&f)==ESP_OK);
        assert(replay_stream_feed(&v,expected+at+101,expected_len-at-101,expected_len,&h,&f)!=ESP_OK);
        s_replay_checkpoint=cache;
        replay_checkpoint_t other;
        replay_checkpoint_prepare(&other,"different",expected_len,&h,&f);assert(!other.states[0].position);
        archive_header_t changed_h=h;changed_h.session_id[0]^=1;
        replay_checkpoint_prepare(&other,"checkpoint",expected_len,&changed_h,&f);assert(!other.states[0].position);
        archive_footer_t changed_f=f;changed_f.crc32^=1;
        replay_checkpoint_prepare(&other,"checkpoint",expected_len,&h,&changed_f);assert(!other.states[0].position);
        replay_checkpoint_prepare(&other,"checkpoint",expected_len-1,&h,&f);assert(!other.states[0].position);
        replay_checkpoint_invalidate();
        replay_checkpoint_prepare(&other,"checkpoint",expected_len,&h,&f);assert(!other.states[0].position);
    }
    puts("PASS: resume checkpoints skip validated prefixes; fragmented record CRC preserved; identity/footer/size/media-generation invalidation; lost ACK retry avoids prefix reread");
    fclose(file);
    puts("PASS: SD-backed 512KiB windows, RAM reuse, exact exports, arbitrary resume, bounded negotiation, lost/malformed ACK, cancel/monitor/epoch/partial-send reconnect and resource reclamation");
}

int main(void) {
    check_capture_pressure();
    offline_replay_budget_t replay_budget={0};
    // 25 ms blocked SD + 0.5 ms validation per 8 KiB must not cause 128
    // extra sleeps per MiB. Byte fairness still gives exactly 16 yields.
    for(unsigned i=0;i<128;i++) {
        clock_us+=25000;
        assert(offline_replay_budget_due(&replay_budget,8192,500)==((i%8)==7));
    }
    assert(replay_budget.yields==16);
    assert(!offline_replay_budget_due(&replay_budget,1,19999));
    assert(offline_replay_budget_due(&replay_budget,1,1));
    assert(replay_budget.yields==17);
    assert(!offline_replay_budget_due(&replay_budget,32768,10000));
    offline_replay_budget_relax(&replay_budget); // A real pressure/slot wait already yields.
    assert(!offline_replay_budget_due(&replay_budget,32768,10000));
    offline_io_budget_t budget={0};
    assert(!offline_io_budget_due(&budget,8192,1000));
    assert(!offline_io_budget_due(&budget,8192,2000));
    assert(offline_io_budget_due(&budget,16384,3000));
    offline_io_budget_reset(&budget,3000);
    assert(!offline_io_budget_due(&budget,0,10999));
    assert(offline_io_budget_due(&budget,1,11000));
    check_crc_compatibility();
    archive_header_t h={0};memcpy(h.magic,"NWXOFF1",7);h.version=1;h.header_bytes=96;
    h.channels=1;h.codec=NUWAX_AUDIO_CODEC_PCM16;h.sample_rate=16000;h.frame_duration_ms=20;
    archive_footer_t f={0};memcpy(f.magic,"NWXEND1",7);f.version=1;f.footer_bytes=32;
    memcpy(expected,&h,96);uint32_t pos=96;
    for(uint32_t i=0;i<500;i++) {
        int16_t pcm[320];for(unsigned j=0;j<320;j++)pcm[j]=(int16_t)(i+j);
        size_t bytes=0;
        assert(nuwax_audio_frame_build_pcm16(expected+pos+4,sizeof(expected)-pos-4,pcm,320,i,i*20,0,16000,1,20,&bytes)==ESP_OK);
        uint32_t len=(uint32_t)bytes;memcpy(expected+pos,&len,4);
        f.crc32=crc32_update(f.crc32,expected+pos+4,len);pos+=4+len;f.frame_count++;f.payload_bytes+=len;
    }
    memcpy(expected+pos,&f,32);expected_len=pos+32;
    // Current typed sender: no data ACK is armed/waited, including resume,
    // bounded allocation fallback, shared transport and transport failure.
    for(unsigned case_id=0;case_id<4;case_id++) {
        reset(0);file_ack_test=true;file_finals=0;
        strcpy(armed_request,"file_begin");window_start=window_end=UINT32_MAX;
        FILE *input=fopen("file-ack-fixture.bin","w+b");assert(input);assert(fwrite(expected,1,expected_len,input)==expected_len);fflush(input);
        nuwax_ws_client_t file_ws={true,7};
        if(case_id==3)inject=4;
        int result=replay_send_windowed(&file_ws,7,input,"file",expected_len,
            case_id==1?12345U:case_id==2?expected_len:0U,524288,&h,&f,NULL,"file_begin");
        assert(result==(case_id==3?ESP_FAIL:ESP_OK));
        assert(!acks && !armed && heap_blocks==0);
        assert(file_finals==((case_id==2||case_id==3)?0U:1U));
        fclose(input);assert(remove("file-ack-fixture.bin")==0);file_ack_test=false;
    }
    reset(0);
    // Execute the whole typed production function, including real control JSON,
    // both data senders and the catalogue-before-unlink transaction.
    for (unsigned route=SD_UPLOAD_MEETING;route<=SD_UPLOAD_HISTORY;route++) {
        typed_test_live=route==SD_UPLOAD_LIVE;
        h.mode=route==SD_UPLOAD_MEETING?0:1;h.segment_index=2;h.reserved0=2;
        strcpy(h.session_id,route==SD_UPLOAD_MEETING?"meeting_file_host":"monitor_file_host");
        memcpy(expected,&h,96);
        for (unsigned failure=0;failure<4;failure++) {
            reset(0);s_replay_route=route;file_ack_test=true;file_finals=0;
            commits=catalog_acks=0;commit_timeout=failure==1;catalog_result=failure==2?ESP_FAIL:ESP_OK;
            const char *source="typed-file.ready";fixture(source);
            FILE *input=fopen(source,"rb");assert(input);bool closed=false;
            nuwax_ws_client_t client={true,7};
            if (route==SD_UPLOAD_MEETING && failure==3) {
                fixture("sd-file.bin");packet_export=fopen("sd-file.packets","wb");
                control_export=fopen("sd-file.controls.jsonl","wb");assert(packet_export && control_export);
            }
            int result=replay_one_ready_file(&client,7,source,failure!=3,input,&closed);
            if (!closed)fclose(input);
            if(packet_export){fclose(packet_export);fclose(control_export);packet_export=control_export=NULL;}
            struct stat st;
            assert(!acks && commits==1 && file_finals==1 && heap_blocks==0);
            assert(catalog_acks==(failure==1?0U:1U));
            if(failure==1 || failure==2) {
                assert(result==(failure==1?ESP_ERR_TIMEOUT:ESP_FAIL));
                assert(stat(source,&st)==0);assert(remove(source)==0);
            } else assert(result==ESP_OK && stat(source,&st)!=0);
        }
    }
    s_replay_route=SD_UPLOAD_LEGACY;file_ack_test=false;commit_timeout=false;catalog_result=ESP_OK;
    /* Formal full transaction: streaming CRC rejects damaged content before
     * commit; cancellation never deletes; allocation fallback remains usable. */
    for(unsigned failure=0;failure<6;failure++) {
        h.mode=0;h.segment_index=2;h.reserved0=2;strcpy(h.session_id,"formal_transaction");
        memcpy(expected,&h,96);
        reset(0);s_replay_route=SD_UPLOAD_MEETING;file_ack_test=true;
        commits=catalog_acks=0;file_finals=0;
        if(failure==0)expected[140]^=1;
        if(failure==5)expected[expected_len-32U+28U]=2U;
        fixture("typed-file.ready");FILE *input=fopen("typed-file.ready","rb");assert(input);
        const size_t saved_psram=free_psram;
        if(failure==2 || failure==3)free_psram=REPLAY_PSRAM_RESERVE_BYTES;
        catalog_bind_result=failure==4?ESP_ERR_INVALID_ARG:ESP_OK;
        if(failure==1)inject=11;
        if(failure==3){record_reads=0;record_cancel_after=3;}
        nuwax_ws_client_t client={true,7};bool closed=false;
        int result=replay_one_ready_file(&client,7,"typed-file.ready",false,input,&closed);
        if(!closed)fclose(input);
        assert(client.connected && client.epoch==7 && !heap_blocks && !acks);
        if(failure==2)assert(result==ESP_OK && commits==1 && catalog_acks==1 && access("typed-file.ready",F_OK)!=0);
        else {
            assert(result==(failure==0?ESP_ERR_INVALID_CRC:(failure==1||failure==3)?ESP_ERR_NOT_FINISHED:
                            failure==4?ESP_ERR_INVALID_ARG:failure==5?ESP_ERR_INVALID_SIZE:ESP_ERR_INVALID_STATE));
            assert(!commits && !catalog_acks && access("typed-file.ready",F_OK)==0);
            if(failure==5)assert(recheck_archive_path("typed-file.ready")==ESP_ERR_INVALID_SIZE&&!packets);
            assert(remove("typed-file.ready")==0);
        }
        /* With a single pass, a prefix can precede cancellation. It must not
         * be committed/deleted, and cancellation on the third read sends no
         * third packet. */
        if(failure==3)assert(packets<=2 && record_reads<=3);
        if(failure==0)expected[140]^=1;
        if(failure==5)expected[expected_len-32U+28U]=f.complete;
        free_psram=saved_psram;record_cancel_after=0;catalog_bind_result=ESP_OK;
    }
    puts("PASS invalid footer complete=2: production metadata and independent recheck both reject locally before TX/commit; no permanent cloud scene block");
    s_replay_route=SD_UPLOAD_LEGACY;file_ack_test=false;
    h.mode=0;h.segment_index=0;h.reserved0=0;memset(h.session_id,0,sizeof(h.session_id));memcpy(expected,&h,96);
    reset(0);
    check_header_equivalence(&h);
    const unsigned splits[]={1,3,7,95,96,97,8064,8192,65536};
    for(unsigned k=0;k<sizeof(splits)/sizeof(splits[0]);k++) {
        replay_stream_t v={0};
        for(uint32_t at=0;at<expected_len;) {size_t n=splits[k];if(n>expected_len-at)n=expected_len-at;
            assert(replay_stream_feed(&v,expected+at,n,expected_len,&h,&f)==ESP_OK);at+=n;}
        assert(v.frames==500);
        if(splits[k]==1)assert(v.direct_frames==0 && v.copied_bytes==500*NUWAX_AUDIO_FRAME_HEADER_BYTES);
        if(splits[k]==65536)assert(v.direct_frames>490 && v.copied_bytes<10*NUWAX_AUDIO_FRAME_HEADER_BYTES);
        if(splits[k]==8192)assert(v.direct_frames>450 && v.copied_bytes<50*NUWAX_AUDIO_FRAME_HEADER_BYTES);
    }
    // Complete unaligned frames need no scratch: parsing only reads header bytes.
    uint8_t *unaligned=malloc(expected_len+1U);assert(unaligned);
    memcpy(unaligned+1,expected,expected_len);replay_stream_t odd={0};
    assert(replay_stream_feed(&odd,unaligned+1,expected_len,expected_len,&h,&f)==ESP_OK);
    assert(odd.frames==500 && odd.direct_frames==500 && odd.copied_bytes==0);
    free(unaligned);
    uint8_t save=expected[108];expected[108]^=1;replay_stream_t bad={0};
    assert(replay_stream_feed(&bad,expected,expected_len,expected_len,&h,&f)!=ESP_OK);expected[108]=save;
    FILE *file=fopen("replay-window-fixture.bin","w+b");assert(file);assert(fwrite(expected,1,expected_len,file)==expected_len);fflush(file);
    for(unsigned fallback=0;fallback<2;fallback++) {
    task_create_fail=fallback!=0;
    for(unsigned mode=0;mode<8;mode++) {
        reset(mode);nuwax_ws_client_t ws={true,7};
        replay_bytes_read=0;
        if(mode==0)atomic_store(&test_backlog,90); // Both serial and threaded replay recover after pressure drains.
        prefetch_probe=!fallback && mode<2;
        int err=replay_send_windowed(&ws,7,file,"s",expected_len,12345,131072,&h,&f,NULL,NULL);
        assert(err==(mode==2 ? ESP_ERR_INVALID_RESPONSE : mode==3 ? ESP_ERR_NOT_FINISHED : mode==4 ? ESP_FAIL :
                     mode==5 ? ESP_ERR_INVALID_STATE : mode==6 ? ESP_ERR_TIMEOUT : ESP_OK));
        if(mode==6)assert(acks==3);
        if(mode<2 || mode==7) {
            assert(persisted==expected_len && acks<10);
            assert(replay_bytes_read==expected_len); // Even a >1s mid-window pause reads/validates only once.
            assert(ws.connected && ws.epoch==7);
        }
        assert(heap_blocks==0);
    }
    }
    task_create_fail=prefetch_probe=false;
#if CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_LANE_PROBE
    // The diagnostic window retains real ACK retry bytes and joins on abort.
    for(unsigned mode=0;mode<7;mode++) {
        reset(mode);s_replay_lane_probe_taken=false;lane_probe_expected=true;
        prefetch_probe=true;replay_bytes_read=0;
        nuwax_ws_client_t probe_ws={true,7};
        int err=replay_send_windowed(&probe_ws,7,file,"s",expected_len,0,131072,&h,&f,NULL,NULL);
        assert(err==(mode==2 ? ESP_ERR_INVALID_RESPONSE : mode==3 ? ESP_ERR_NOT_FINISHED :
                     mode==4 ? ESP_FAIL : mode==5 ? ESP_ERR_INVALID_STATE :
                     mode==6 ? ESP_ERR_TIMEOUT : ESP_OK));
        if(mode<2)assert(persisted==expected_len && replay_bytes_read==expected_len);
        assert(s_replay_lane_probe_taken && workers_created==workers_joined && heap_blocks==0 && signal_handles==0);
    }
    reset(0);prefetch_probe=false;
    // A read failure or capture preemption before full publication must wake
    // the diagnostic waiter and join, without any packet/commit being sent.
    for(unsigned fault=0;fault<3;fault++) {
        reset(0);s_replay_lane_probe_taken=false;replay_bytes_read=0;
        fail_read_once=fault==0;short_read_once=fault==1;probe_read_preempt=fault==2;
        nuwax_ws_client_t probe_ws={true,7};
        int err=replay_send_windowed(&probe_ws,7,file,"s",expected_len,0,131072,&h,&f,NULL,NULL);
        assert(err==(fault==2?ESP_ERR_NOT_FINISHED:ESP_FAIL));
        assert(packets==0 && heap_blocks==0 && signal_handles==0 && workers_created==workers_joined);
    }
    probe_read_preempt=false;reset(0);
#endif
    for(unsigned mode=0;mode<5;mode++) {
        reset(0);transport_deferrals=mode==1?20U:2U;defer_action=mode>=2?mode-1:0;
        nuwax_ws_client_t deferred_ws={true,7};
        int err=replay_send_windowed(&deferred_ws,7,file,"s",expected_len,0,131072,&h,&f,NULL,NULL);
        assert(err==(mode==0?ESP_OK:mode==2?ESP_ERR_NOT_FINISHED:ESP_ERR_TIMEOUT));
        if(mode==0)assert(persisted==expected_len && transport_deferrals==0);
        else assert(packets==0);
        if(mode==1)assert(transport_deferrals==12);
        assert(workers_created==workers_joined && heap_blocks==0 && signal_handles==0);
    }
    transport_deferrals=defer_action=0;reset(0);
    // Serial fallback preserves a higher-priority caller as well.
    {
        uint8_t destination[512];replay_stream_t v={0};
        replay_read_stats_t stats={0};uint64_t rd=0,crc=0,work=0;
        size_t n=sizeof(destination);assert(replay_read_rewind(file)==ESP_OK);
        current_priority=8U;
        assert(replay_read_validate(file,destination,&n,&v,expected_len,&h,&f,&rd,&crc,&work,&stats)==ESP_OK);
        assert(current_priority==8U && priority_changes==0U && heap_blocks==0);
        current_priority=5U;
    }
    // Whole replay under each internal-memory condition, including a changed
    // reserve after allocation. Fallback is functional, not a new failure.
    for(unsigned serial=0;serial<2;serial++) {
    task_create_fail=serial!=0;
    for(unsigned memory_case=0;memory_case<6;memory_case++) {
        reset(0);internal_reads=fallback_reads=0;replay_bytes_read=0;
        largest_queries=0;
        free_internal=memory_case==1?16384U:memory_case==3?24576U:32768U;
        fail_internal=memory_case==2;pressure_after_internal=memory_case==3;
        if(memory_case==4){free_internal=24700U;internal_overhead=600U;}
        internal_largest=memory_case==5?4096U:32768U;
        const size_t initial_free=free_internal;
        nuwax_ws_client_t memory_ws={true,7};
        assert(replay_send_windowed(&memory_ws,7,file,"s",expected_len,12345,524288,&h,&f,NULL,NULL)==ESP_OK);
        assert(persisted==expected_len && replay_bytes_read==expected_len);
        assert(heap_blocks==0 && internal_allocation==NULL && workers_created==workers_joined);
        assert(free_internal==initial_free-(memory_case==3?4096U:0U));
        if(memory_case==0)assert(internal_reads>fallback_reads && fallback_reads>0); // Odd window boundaries use the adapter.
        if(memory_case==1 || memory_case==2)assert(internal_reads==0 && fallback_reads>0);
        if(memory_case==3)assert(fallback_reads>0);
        if(memory_case>=3)assert(internal_reads>0); // Smaller slices still cover every byte.
        if(memory_case==0 || memory_case==1 || memory_case==3 || memory_case==4)
            assert(largest_queries==0); // Healthy or total-reserve-limited reads never walk heaps.
        if(memory_case==5)assert(largest_queries>0 && largest_queries<8);
        internal_overhead=0;internal_largest=32768U;
    }
    }
    task_create_fail=false;
    free_internal=32768;fail_internal=pressure_after_internal=false;
    // Attribute each fallback without weakening the post-allocation reserve.
    for(unsigned cause=0;cause<4;cause++) {
        uint8_t destination[512];
        replay_stream_t v={0};replay_read_stats_t stats={0};uint64_t rd=0,crc=0,work=0;
        assert(replay_read_rewind(file)==ESP_OK);
        free_internal=cause==1?16384U:cause==3?19000U:32768U;
        fail_internal=cause==2;pressure_after_internal=cause==3;
        if(cause==0){v.position=1;assert(lseek(fileno(file),1,SEEK_SET)==1);}
        size_t n=sizeof(destination);
        assert(replay_read_validate(file,destination,&n,&v,expected_len,&h,&f,
                                   &rd,&crc,&work,&stats)==ESP_OK);
        assert(n==sizeof(destination));
        assert(stats.fallback_bytes==512 && stats.internal_bytes==0);
        assert(stats.fallback_unaligned==(cause==0));
        assert(stats.fallback_reserve==(cause==1));
        assert(stats.fallback_alloc==(cause==2));
        assert(stats.fallback_post_reserve==(cause==3));
        assert(!internal_allocation && heap_blocks==0);
    }
    free_internal=32768;fail_internal=pressure_after_internal=false;
    // Fixed 8 KiB would fall back in these cases. Adaptive reads must retain
    // reserve, return the real prefix and leave the unpublished suffix intact.
    for(unsigned cause=0;cause<5;cause++) {
        uint8_t destination[8192];memset(destination,0xa5,sizeof(destination));
        replay_stream_t v={0};replay_read_stats_t stats={0};uint64_t rd=0,crc=0,work=0;
        assert(replay_read_rewind(file)==ESP_OK);
        free_internal=cause==0?24576U:cause==1?24700U:32768U;
        internal_overhead=cause==1?700U:0U;
        internal_largest=cause==2?4096U:32768U;
        fail_internal_attempts=cause==3?1U:0U;fail_internal=cause==4;
        const size_t before=free_internal;
        internal_attempts=0;
        size_t n=sizeof(destination);
        assert(replay_read_validate(file,destination,&n,&v,expected_len,&h,&f,
                                   &rd,&crc,&work,&stats)==ESP_OK);
        assert(internal_attempts<=3 && n>0 && n<=sizeof(destination));
        assert(v.position==n && lseek(fileno(file),0,SEEK_CUR)==(off_t)n);
        assert(memcmp(destination,expected,n)==0);
        for(size_t i=n;i<sizeof(destination);i++)assert(destination[i]==0xa5);
        if(cause<4) {
            assert(n<sizeof(destination) && n%512U==0 && stats.downsized_reads==1);
            assert(stats.internal_bytes==n && !stats.fallback_bytes);
            assert(stats.min_capacity==n && stats.max_capacity==n);
        } else {
            assert(internal_attempts==3 && n==sizeof(destination));
            assert(stats.fallback_alloc==1 && stats.allocation_rejects==3);
        }
        if(cause==1) {
            assert(stats.post_reserve_rejects==1 && !stats.fallback_post_reserve);
            assert(stats.rejected_free_before==before && stats.rejected_free_after<16384U);
        }
        if(cause==3)assert(stats.allocation_rejects==1 && !stats.fallback_alloc);
        assert(stats.largest_scans==((cause==2 || cause==3 || cause==4)?1U:0U));
        assert(!internal_allocation && heap_blocks==0 && free_internal==before);
    }
    free_internal=32768;internal_overhead=0;internal_largest=32768;
    fail_internal=false;fail_internal_attempts=0;
    // Fragmentation hints are bounded. They avoid repeated heap walks, expire
    // after 32 reads, and cannot hide fresh free-space pressure or recovery.
    for(unsigned no_sector=0;no_sector<2;no_sector++) {
        replay_read_stats_t stats={0};uint64_t rd=0,crc=0,work=0;
        uint8_t destination[8192];largest_queries=0;
        for(unsigned step=0;step<34;step++) {
            replay_stream_t v={0};size_t n=sizeof(destination);
            assert(replay_read_rewind(file)==ESP_OK);
            internal_largest=step==0?(no_sector?100U:4096U):32768U;
            free_internal=step==2?16384U:32768U;
            const size_t before=free_internal;
            assert(replay_read_validate(file,destination,&n,&v,expected_len,&h,&f,
                                       &rd,&crc,&work,&stats)==ESP_OK);
            assert(n>0 && memcmp(destination,expected,n)==0 && v.position==n);
            if(!no_sector && step<33 && step!=2)assert(n==3584U);
            if(step==33)assert(n==8192U); // Expiration probes the restored large block.
            assert(free_internal==before && !internal_allocation && heap_blocks==0);
        }
        assert(largest_queries==1 && stats.largest_scans==1 && stats.hinted_reads==32);
        if(no_sector)assert(stats.fallback_fragment_hint==32);
        else assert(stats.fallback_reserve==1);
    }
    free_internal=32768;internal_largest=32768;
    // Split state keeps the full 8 KiB read at the hardware-observed free
    // memory level. Missing state or post-allocation pressure must remain a
    // functional fallback, and every archive/error path must release it.
    for(unsigned hot_case=0;hot_case<4;hot_case++) {
        replay_read_stats_t stats={0};replay_stream_t v={0};
        uint64_t rd=0,crc=0,work=0;uint8_t destination[8192];
        free_internal=hot_case==2?16600U:25079U;
        const size_t before=free_internal;
        fail_hot=hot_case==1;pressure_after_hot=hot_case==2;
        replay_read_prepare(&stats);
        assert(stats.hot_validator_allocated==(hot_case==0 || hot_case==3));
        if(hot_case==3)fail_read_once=true;
        assert(replay_read_rewind(file)==ESP_OK);
        size_t n=sizeof(destination);
        assert(replay_read_validate(file,destination,&n,&v,expected_len,&h,&f,
                                   &rd,&crc,&work,&stats)==(hot_case==3?ESP_FAIL:ESP_OK));
        if(hot_case==3)assert(n==0);
        else assert(n==8192 && memcmp(destination,expected,n)==0 && v.position==n);
        if(hot_case<2)assert(stats.internal_bytes==8192 && !stats.post_reserve_rejects);
        replay_read_release(&stats);replay_read_release(&stats);
        assert(!stats.hot_validator && !hot_allocation && !internal_allocation && heap_blocks==0);
        assert(free_internal==before-(hot_case==2?4096U:0U));
    }
    fail_hot=pressure_after_hot=false;
    // Remember measured allocation overhead for 32 reads, still honor fresh
    // pressure, and retry full size after expiration when the overhead clears.
    {
        replay_read_stats_t stats={0};uint64_t rd=0,crc=0,work=0;
        uint8_t destination[8192];
        for(unsigned step=0;step<34;step++) {
            replay_stream_t v={0};size_t n=sizeof(destination);
            assert(replay_read_rewind(file)==ESP_OK);
            free_internal=step==2?16384U:25000U;
            internal_overhead=step<33?600U:0U;
            const size_t before=free_internal;
            assert(replay_read_validate(file,destination,&n,&v,expected_len,&h,&f,
                                       &rd,&crc,&work,&stats)==ESP_OK);
            assert(memcmp(destination,expected,n)==0 && v.position==n);
            assert(n==((step<33 && step!=2)?7680U:8192U));
            assert(free_internal==before && heap_blocks==0);
        }
        assert(stats.post_reserve_rejects==1 && stats.reserve_hinted_reads==32);
        assert(stats.fallback_reserve==1 && !stats.largest_scans);
    }
    free_internal=32768;internal_overhead=0;
    // No partial or failed internal read may be published to the PSRAM slot.
    for(unsigned error_case=0;error_case<2;error_case++) {
        uint8_t destination[512];memset(destination,0xa5,sizeof(destination));
        replay_stream_t v={0};replay_read_stats_t stats={0};uint64_t rd=0,crc=0,work=0;
        assert(replay_read_rewind(file)==ESP_OK);
        fail_read_once=error_case==0;short_read_once=error_case==1;
        const unsigned faults_before=atomic_load(&media_faults);
        size_t n=sizeof(destination);
        assert(replay_read_validate(file,destination,&n,&v,expected_len,&h,&f,
                                   &rd,&crc,&work,&stats)==ESP_FAIL);
        assert(n==0);
        assert(atomic_load(&media_faults)==faults_before+1);
        for(unsigned i=0;i<sizeof(destination);i++)assert(destination[i]==0xa5);
        assert(heap_blocks==0 && !internal_allocation && free_internal==32768);
    }
    // First batch must precede reading a whole window, including odd-byte resume.
    for(unsigned resume_case=0;resume_case<2;resume_case++) {
        const uint32_t resume=resume_case?12345U:0U;
        reset(0);replay_bytes_read=0;streaming_probe=true;streaming_read_limit=resume+8192U;
#if !CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_LANE_PROBE
        // Even the first eligible window must publish/send incrementally in
        // the normal build; the read stub blocks until its first packet.
        s_replay_lane_probe_taken=false;
#endif
        nuwax_ws_client_t streaming_ws={true,7};
        assert(replay_send_windowed(&streaming_ws,7,file,"s",expected_len,resume,524288,&h,&f,NULL,NULL)==ESP_OK);
        assert(persisted==expected_len && replay_bytes_read==expected_len && acks==1);
        assert(heap_blocks==0 && workers_created==workers_joined);
        streaming_probe=false;
    }
    // Extracted production sender must retain fairness for immediate ACKs,
    // but not add a tick after an ACK that already waited a whole tick.
    // Serial mode makes the fake clock independent of the reader thread.
    task_create_fail=true;
    for(unsigned delayed=0;delayed<2;delayed++) {
        reset(0);after_ack=false;delays_after_ack=0;ack_delay_us=delayed?10000U:0U;
        nuwax_ws_client_t ack_ws={true,7};
        assert(replay_send_windowed(&ack_ws,7,file,"s",expected_len,0,524288,&h,&f,NULL,NULL)==ESP_OK);
        assert(persisted==expected_len && acks>0);
        assert(logged_ack_yields==(delayed?0U:acks));
        assert(logged_ack_yields_saved==(delayed?acks:0U));
        assert(delays_after_ack==(delayed?0U:acks));
        after_ack=false;
    }
    ack_delay_us=0;
    unsigned no_wait_yields=0;
    for(unsigned blocked=0;blocked<2;blocked++) {
        reset(0);simulated_tx_wait_us=blocked?10000U:0U;
        nuwax_ws_client_t credit_ws={true,7};
        assert(replay_send_windowed(&credit_ws,7,file,"s",expected_len,0,524288,&h,&f,NULL,NULL)==ESP_OK);
        if(blocked)assert(logged_send_credits>0 && logged_sender_yields<no_wait_yields);
        else {assert(logged_send_credits==0 && logged_sender_yields>0);no_wait_yields=logged_sender_yields;}
    }
    simulated_tx_wait_us=0;task_create_fail=false;
    // Exercise the actual reader without concurrent TX touching the fake clock.
    // One large retained slot fits the fixture; nobody releases it before join.
    // Below-tick reads retain fairness; >=tick reads credit their blocking time.
    for(unsigned slow=0;slow<2;slow++) {
        reset(0);simulated_read_wait_us=slow?10000U:1000U;
        assert(replay_read_rewind(file)==ESP_OK);
        uint8_t *retained=malloc(2U*expected_len);assert(retained);
        nuwax_ws_client_t read_ws={true,7};
        replay_prefetch_t *p=replay_prefetch_start(file,&read_ws,7,expected_len,0,expected_len,retained,&h,&f,false,"reader-test");
        assert(p);
        for(unsigned wait=0;wait<3000 && eTaskGetState(p->task)!=eSuspended;wait++)Sleep(1);
        assert(eTaskGetState(p->task)==eSuspended);
        assert(atomic_load(&p->slots[0].published)==expected_len);
        assert(memcmp(retained,expected,expected_len)==0);
        assert(slow ? (logged_read_credits>0 && logged_reader_yields==0) :
                      (logged_read_credits==0 && logged_reader_yields>0));
        uint64_t rd=0,validation=0;
        assert(replay_prefetch_join(p,false,&rd,&validation)==ESP_OK);
        free(retained);assert(heap_blocks==0 && workers_created==workers_joined);
    }
    simulated_read_wait_us=0;
    // Hold the sender with capture pressure until exactly 1..4 complete frames
    // are published. Then hold the reader until TX: batching must use that
    // prefix without waiting for an unpublished frame or sending a short one.
    for(unsigned ready_frames=1;ready_frames<=4;ready_frames++) {
        reset(0);first_group=0;replay_bytes_read=0;
        prefix_gate=true;prefix_gate_reached=false;
        streaming_probe=true;streaming_read_limit=ready_frames*8192U;
        nuwax_ws_client_t prefix_ws={true,7};
        assert(replay_send_windowed(&prefix_ws,7,file,"s",expected_len,0,524288,&h,&f,NULL,NULL)==ESP_OK);
        assert(prefix_gate_reached && first_group==ready_frames);
        assert(persisted==expected_len && replay_bytes_read==expected_len);
        assert(heap_blocks==0 && workers_created==workers_joined);
        streaming_probe=false;prefix_gate=false;
    }
    reset(0);nuwax_ws_client_t ws={true,7};
    atomic_store(&test_backlog,90);pressure_frozen=true;
    pressure_action=1;pressure_until_us=atomic_load(&clock_us)+2500000ULL;
    assert(replay_send_windowed(&ws,7,file,"s",expected_len,0,524288,&h,&f,NULL,NULL)==ESP_ERR_NOT_FINISHED);
    assert(packets==0 && heap_blocks==0 && workers_created==workers_joined);
    pressure_frozen=false;atomic_store(&test_backlog,0);busy=false;pressure_action=0;
    assert(replay_send_windowed(&ws,7,file,"s",expected_len,expected_len,524288,&h,&f,NULL,NULL)==ESP_OK);
    assert(packets==0 && heap_blocks==0);
    heap_fail_all=true;
    assert(replay_send_windowed(&ws,7,file,"s",expected_len,0,524288,&h,&f,NULL,NULL)==ESP_ERR_NO_MEM);
    heap_fail_all=false;heap_fail_context=true;
    unsigned before=workers_created;
    assert(replay_send_windowed(&ws,7,file,"s",expected_len,0,524288,&h,&f,NULL,NULL)==ESP_OK);
    assert(workers_created==before && heap_blocks==0);heap_fail_context=false;
    // Batch allocation failure and scarce PSRAM shrink read slots first,
    // preserving four-frame writes AND the full durable ACK window.
    for(unsigned memory_case=0;memory_case<2;memory_case++) {
        reset(0);batch_calls=0;replay_buffer_size=0;
        heap_fail_batch=memory_case==0;
        free_psram=memory_case==0?3U*1024U*1024U:REPLAY_PSRAM_RESERVE_BYTES+REPLAY_NETWORK_HEADROOM_BYTES+262144U+32768U;
        assert(replay_send_windowed(&ws,7,file,"s",expected_len,0,524288,&h,&f,NULL,NULL)==ESP_OK);
        assert(persisted==expected_len && acks==1);
        assert(batch_calls>0);
        assert(replay_buffer_size==131072U+4U*(8064U+142U));
        assert(heap_blocks==0 && workers_created==workers_joined);
    }
    heap_fail_batch=false;
    // Latest hardware trace has ~1.1 MiB free: preserve a full 128 KiB
    // double buffer and four-frame wire batching without the old 1 MiB veto.
    reset(0);free_psram=1108227U+REPLAY_NETWORK_HEADROOM_BYTES;replay_buffer_size=0;
    assert(replay_send_windowed(&ws,7,file,"s",expected_len,0,524288,&h,&f,NULL,NULL)==ESP_OK);
    assert(replay_buffer_size==262144U+NUWAX_WS_ARCHIVE_BATCH_MAX_FRAMES*(8064U+142U));
    assert(acks==1 && persisted==expected_len && heap_blocks==0);
    // 130 device trace: the old layout chose 128 KiB x2 + one frame.
    // Exercise every measured free-PSRAM point and all 4 MiB bytes below.
    const size_t trace_psram[]={1087084U,1087072U,1080940U,1080348U,1083056U,1086624U,1086892U};
    for(unsigned i=0;i<sizeof(trace_psram)/sizeof(trace_psram[0]);i++) {
        reset(0);free_psram=trace_psram[i];batch_calls=0;bulk_allocation_attempts=0;
        assert(replay_send_windowed(&ws,7,file,"s",expected_len,0,524288,&h,&f,NULL,NULL)==ESP_OK);
        assert(replay_buffer_size==131072U+4U*8206U && batch_calls>0);
        assert(persisted==expected_len && acks==1 && heap_blocks==0);
        assert(free_psram==trace_psram[i] && workers_created==workers_joined);
    }
    // 131 hardware snapshot, without artificially adding the cache allowance:
    // cache-32 uses smaller read slots, retains four frames and 512 KiB ACK.
    reset(0);free_psram=1114688U;batch_calls=0;
    assert(replay_send_windowed(&ws,7,file,"s",expected_len,0,524288,&h,&f,NULL,NULL)==ESP_OK);
    const size_t expected_slots=REPLAY_NETWORK_HEADROOM_BYTES ? 131072U : 262144U;
    assert(replay_buffer_size==expected_slots+4U*8206U && batch_calls>0);
    assert(persisted==expected_len && acks==1 && free_psram==1114688U && heap_blocks==0);
    free_psram=1108227U+REPLAY_NETWORK_HEADROOM_BYTES;
    // A monitor starting with the durable ACK wins cancellation. The peer
    // may have persisted the window, but the old transaction must not consume
    // that ACK; a fresh begin supplies its durable offset for exact resume.
    reset(0);monitor_after_ack=true;s_card_preempted=false;
    assert(replay_send_windowed(&ws,7,file,"s",expected_len,0,262144,&h,&f,NULL,NULL)==ESP_ERR_NOT_FINISHED);
    assert(!s_card_preempted && !ws.connected && acks==1 && persisted==262144 && heap_blocks==0);
    monitor_after_ack=false;monitoring_now=false;reset(0);ws.connected=true;ws.epoch=8;
    assert(replay_send_windowed(&ws,8,file,"s",expected_len,262144,524288,&h,&f,NULL,NULL)==ESP_OK);
    assert(persisted==expected_len && heap_blocks==0);
    ws.epoch=7;
    bool deferred=true;reset(8);
    assert(replay_send_windowed(&ws,7,file,"s",expected_len,0,524288,&h,&f,&deferred,NULL)==ESP_ERR_NO_MEM);
    assert(!deferred && packets==2 && heap_blocks==0); // Never fall back after partial TX.
    ws.connected=true; // Independent connection is retired for an uncertain streaming window.
    // Real heap accounting plus concurrent allocations after malloc: retain
    // v2 with smaller read slots; the 512 KiB durable window survives.
    for(unsigned race=0;race<3;race++) {
        reset(0);free_psram=1106952U+REPLAY_NETWORK_HEADROOM_BYTES;deferred=true;bulk_allocation_attempts=0;
        psram_pressure_bytes=race==0?16384U:(race==1?65536U:250000U);
        psram_pressure_attempts=1;
        assert(replay_send_windowed(&ws,7,file,"s",expected_len,0,524288,&h,&f,&deferred,NULL)==ESP_OK);
        assert(!deferred && persisted==expected_len && heap_blocks==0);
        assert(workers_created==workers_joined && !bulk_allocation);
        assert(bulk_allocation_attempts>1 && bulk_allocation_attempts<=20);
        assert(free_psram==1106952U+REPLAY_NETWORK_HEADROOM_BYTES-psram_pressure_bytes);
        assert(replay_buffer_size<262144U && acks==1);
    }
    // A reserve-boundary miss caused solely by allocator overhead must also
    // shrink the read slots; no bytes may use an unsafe allocation.
    reset(0);free_psram=REPLAY_PSRAM_RESERVE_BYTES+REPLAY_WORKER_HEADROOM_BYTES+262144U+4U*8206U;
    assert(replay_send_windowed(&ws,7,file,"s",expected_len,0,524288,&h,&f,&deferred,NULL)==ESP_OK);
    assert(!deferred && replay_buffer_size==131072U+4U*8206U && heap_blocks==0);
    // Persistent external pressure leaves no legal v2 layout. Fallback is
    // bounded, releases all allocations and has sent no bytes.
    reset(0);free_psram=1106952U+REPLAY_NETWORK_HEADROOM_BYTES;psram_pressure_bytes=310000U;psram_pressure_attempts=1;
    assert(replay_send_windowed(&ws,7,file,"s",expected_len,0,524288,&h,&f,&deferred,NULL)==ESP_ERR_NO_MEM);
    assert(deferred && packets==0 && heap_blocks==0 && !bulk_allocation);
    psram_pressure_attempts=0;
    reset(0);
    free_psram=REPLAY_PSRAM_RESERVE_BYTES;
    assert(replay_send_windowed(&ws,7,file,"s",expected_len,0,524288,&h,&f,&deferred,NULL)==ESP_ERR_NO_MEM);
    assert(deferred && packets==0);
    assert(heap_blocks==0);free_psram=3U*1024U*1024U;
    // A short final read must fail even when previous batches were ACKed.
    fclose(file);file=fopen("replay-window-fixture.bin","w+b");assert(file);
    assert(fwrite(expected,1,expected_len-1,file)==expected_len-1);fflush(file);reset(0);
    assert(replay_send_windowed(&ws,7,file,"s",expected_len,0,524288,&h,&f,NULL,NULL)==ESP_FAIL);
    assert(heap_blocks==0 && workers_created==workers_joined);
    check_metadata();
    check_commit();
    check_formal_pressure_cancel();
    check_recovery();
    char after[128]={0},name[128],wanted[128];bool found;
    for(unsigned i=0;i<200;i++) {
        assert(find_next_ready_name(after,name,sizeof(name),&found)==ESP_OK && found);
        snprintf(wanted,sizeof(wanted),"offline_%08u.ready",i);assert(strcmp(name,wanted)==0);
        strcpy(after,name);
    }
    assert(find_next_ready_name(after,name,sizeof(name),&found)==ESP_OK && !found);
    assert(scans==5); // Four sorted pages plus one EOF scan, not 201 scans.
    assert(find_next_ready_name("",name,sizeof(name),&found)==ESP_OK && found);
    assert(strcmp(name,"offline_00000000.ready")==0);
    include_bad=true;
    assert(find_next_ready_name("",name,sizeof(name),&found)==ESP_OK && found);
    assert(strcmp(name,"aaa.ready.bad")==0);
    atomic_store(&s_quarantine_recheck_pending,false);
    assert(find_next_ready_name("",name,sizeof(name),&found)==ESP_OK && found);
    assert(strcmp(name,"offline_00000000.ready")==0);
    busy=true;assert(find_next_ready_name("",name,sizeof(name),&found)==ESP_ERR_NOT_FINISHED);
    busy=false;
    fclose(file);
    remove("replay-window-fixture.bin");
    assert(workers_created>0 && workers_created==workers_joined);
    assert(priority_changes==0U && current_priority==5U);
    assert(signal_handles==0 && signal_gives>0 && signal_takes>0);
    StaticSemaphore_t signal_storage;
    SemaphoreHandle_t signal=xSemaphoreCreateBinaryStatic(&signal_storage);
    xSemaphoreGive(signal); xSemaphoreGive(signal);
    assert(!replay_prefetch_wait_signal(signal)); // Publication before wait is retained.
    assert(xSemaphoreTake(signal,0)==0); // Binary publications coalesce.
    assert(replay_prefetch_wait_signal(signal)); // A real timeout grants fairness.
    assert(!offline_replay_wait_relaxes(0,10000));
    assert(!offline_replay_wait_relaxes(9999,10000));
    assert(offline_replay_wait_relaxes(10000,10000));
    assert(!offline_replay_wait_relaxes(10000,0));
    vSemaphoreDelete(signal); assert(signal_handles==0);
    check_sd_backed_windows();
    printf("PASS: Wi-Fi cache=%u additional network headroom=%u; recording reserve preserved\n",
           (unsigned)CONFIG_ESP_WIFI_CACHE_TX_BUFFER_NUM, (unsigned)REPLAY_NETWORK_HEADROOM_BYTES);
    printf("PASS: lane_probe=%u; 16896 header mutations; adaptive DMA/allocator/fragmentation; exact prefix publication, 16KiB reserve, short read; serial/prefetch resume, ACK, cancellation, epoch, commit, split-send retries; reader priority 6, sender 5, zero priority transitions; event fairness and directory regressions\n", CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_LANE_PROBE);
}
