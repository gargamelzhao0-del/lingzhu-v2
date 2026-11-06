/* Compile the production encoder, send wrappers and reconnect admission.
 * Only RTOS/socket boundaries are replaced. Windows events deliberately
 * schedule a competing sender at the first real I/O chunk unlock. */
#include <windows.h>
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "ws_transport.h"
#include "ws_send_failure_policy.h"
#include "ws_archive_retry_policy.h"
#include "ws_mask.h"

typedef struct { HANDLE handle; atomic_uint owner; bool io_lock; } mutex_t;
typedef mutex_t *SemaphoreHandle_t;
typedef unsigned TickType_t;
#include "ws_archive_flow_policy.h"
typedef struct { int stage, operation_errno; unsigned io_wait_ms, tx_waiters; nuwax_ws_send_metrics_t *metrics; const uint8_t *payload_tail; size_t payload_prefix_bytes; nuwax_ws_archive_deadline_t archive_deadline; } ws_send_trace_t;
typedef struct { int unused; } parsed_ws_url_t;
static SemaphoreHandle_t s_lifecycle_lock;
static HANDLE contended;
static unsigned allocations, connect_calls;
static bool close_fails;
static const char *TAG = "host";
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
#define NUWAX_WS_SEND_CHUNK_BYTES 3968
#define CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN 8192
#define NUWAX_DEVICE_ERROR_RESULT_BYTES 512
#define NUWAX_WS_LIFECYCLE_LOCK_TIMEOUT_MS 30
#define NUWAX_WS_IO_TOTAL_TIMEOUT_MS 2000
#define NUWAX_WS_REALTIME_INFLIGHT_DRAIN_TIMEOUT_MS 1000
#define NUWAX_WS_ARCHIVE_LIFECYCLE_LOCK_TIMEOUT_MS 25
#define NUWAX_WS_ARCHIVE_IO_TIMEOUT_MS 250
#define NUWAX_WS_SHARED_ARCHIVE_DRAIN_MS 750
#define ESP_TLS_ERR_SSL_WANT_READ -0x6900
#define ESP_TLS_ERR_SSL_WANT_WRITE -0x6880
#define NUWAX_WS_ARCHIVE_CHUNK_HEADER_BYTES 64
#define NUWAX_WS_ARCHIVE_CHUNK_MAX_PAYLOAD_BYTES 8064
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_SPIRAM 4
static void log_ignore(const char *tag, const char *format, ...) { (void)tag; (void)format; }
#define ESP_LOGE(...) log_ignore(__VA_ARGS__)
#define ESP_LOGW(...) log_ignore(__VA_ARGS__)
#define ESP_LOGI(...) log_ignore(__VA_ARGS__)
#define ESP_LOGD(...) log_ignore(__VA_ARGS__)
#define ESP_RETURN_ON_ERROR(expr, ...) do { esp_err_t r_ = (expr); if (r_ != ESP_OK) return r_; } while (0)
#define nuwax_device_error_report_enqueue_detail(...) 0
static unsigned heap_caps_get_free_size(unsigned mask) { (void)mask; return 100000; }
static unsigned heap_caps_get_largest_free_block(unsigned mask) { return heap_caps_get_free_size(mask); }
static bool virtual_clock;
static int64_t virtual_us;
static int64_t esp_timer_get_time(void) { return virtual_clock ? virtual_us : (int64_t)GetTickCount64()*1000; }
static int64_t io_deadline_after_ms(uint32_t ms) { return esp_timer_get_time() + (int64_t)ms*1000; }
static bool io_deadline_expired(int64_t deadline) { return esp_timer_get_time() >= deadline; }
static SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    mutex_t *m = calloc(1, sizeof(*m)); assert(m);
    m->handle = CreateMutex(NULL, FALSE, NULL); assert(m->handle); allocations++; return m;
}
static int xSemaphoreTake(SemaphoreHandle_t m, TickType_t timeout) {
    /* A task waiting for a client's frame must never hold global lifecycle. */
    assert(m == s_lifecycle_lock || m->io_lock || !s_lifecycle_lock ||
           s_lifecycle_lock->owner != GetCurrentThreadId());
    DWORD r = WaitForSingleObject(m->handle, 0);
    if (r == WAIT_TIMEOUT) {
        SetEvent(contended);
        r = WaitForSingleObject(m->handle, timeout);
    }
    if (r != WAIT_OBJECT_0) { assert(r == WAIT_TIMEOUT); return 0; }
    assert(m->owner == 0); m->owner = GetCurrentThreadId(); return pdTRUE;
}
static void xSemaphoreGive(SemaphoreHandle_t m) {
    assert(m->owner == GetCurrentThreadId()); m->owner = 0; assert(ReleaseMutex(m->handle));
}
static void destroy_mutex(void *ptr) {
    mutex_t *m = ptr; if (!m) return;
    assert(m->owner == 0); CloseHandle(m->handle); free(m);
}
typedef struct {
    nuwax_ws_client_t ws;
    uint8_t wire[65536]; size_t wire_len;
    unsigned chunks; bool pause, fail_after_first;
    size_t max_write, fail_after_bytes;
    bool stall_before_first, retry_once;
    int64_t stall_until;
    unsigned write_delay_ms;
    int tls_want;
    bool tls_retry_each_write, tls_retry_injected;
    unsigned tls_stalls, bio_per_stall;
    const void *pending_tls_input;
    size_t pending_tls_bytes;
    HANDLE first_chunk, resume;
} fixture_t;
static bool transport_is_open(const nuwax_ws_client_t *c) { return atomic_load(&c->connected); }
uint32_t nuwax_ws_connection_epoch(const nuwax_ws_client_t *c) { return atomic_load(&c->connection_epoch); }
bool nuwax_ws_heartbeat_healthy_if_epoch(const nuwax_ws_client_t *c, uint32_t epoch) { (void)c; (void)epoch; return false; }
uint32_t nuwax_ws_heartbeat_remaining_ms_if_epoch(const nuwax_ws_client_t *c, uint32_t epoch) { (void)c; (void)epoch; return 0; }
static bool close_requested_for_epoch(nuwax_ws_client_t *c, uint32_t epoch) { return atomic_load(&c->close_requested_epoch) == epoch; }
static void tx_stage_store(nuwax_ws_client_t *c, int stage, uint32_t epoch) { (void)c; (void)stage; (void)epoch; }
static const char *tx_stage_name(int stage) { (void)stage; return "host"; }
static unsigned deferred_reports;
static void report_ws_warning(const char *message, int err) { if(!strcmp(message,"reliable_frame_deferred")) deferred_reports++; (void)err; }
static atomic_uint random_calls;
static atomic_uint tls_profiles_started, tls_profiles_finished;
static _Thread_local nuwax_ws_send_metrics_t *active_tls_metrics;
bool nuwax_tls_metrics_begin(nuwax_ws_send_metrics_t *metrics) {
    assert(metrics && metrics->shared_tls_archive);
    assert(!active_tls_metrics);active_tls_metrics=metrics;
    atomic_fetch_add(&tls_profiles_started,1U);metrics->tls_profiled_calls++;return true;
}
void nuwax_tls_metrics_end(void) { assert(active_tls_metrics);active_tls_metrics=NULL;atomic_fetch_add(&tls_profiles_finished,1U); }
static void esp_fill_random(void *p, size_t len) {
    unsigned seed=atomic_fetch_add(&random_calls,1U);
    for (size_t i=0;i<len;i++) ((uint8_t *)p)[i]=(uint8_t)(i+1+seed);
}
static bool transport_io_lock_until(nuwax_ws_client_t *c, int64_t deadline) {
    int64_t remaining = deadline-esp_timer_get_time();
    return remaining>0 && xSemaphoreTake(c->io_lock, (unsigned)(remaining/1000));
}
static void transport_io_unlock(nuwax_ws_client_t *c) {
    fixture_t *f=(fixture_t *)c; xSemaphoreGive(c->io_lock);
    if (++f->chunks == 1 && f->pause) {
        SetEvent(f->first_chunk);
        assert(WaitForSingleObject(f->resume, 3000)==WAIT_OBJECT_0);
    }
}
static unsigned retry_budget;
static nuwax_ws_send_metrics_t *limit_mutation_target;
static ssize_t transport_write_unlocked(nuwax_ws_client_t *c, const void *p, size_t len) {
    fixture_t *f=(fixture_t *)c;
    assert(((mutex_t *)c->io_lock)->owner == GetCurrentThreadId());
    assert(((mutex_t *)c->tx_frame_lock)->owner == GetCurrentThreadId());
    if (f->pending_tls_input) { assert(p == f->pending_tls_input && len == f->pending_tls_bytes); }
    if (f->tls_stalls) {
        --f->tls_stalls;f->pending_tls_input=p;f->pending_tls_bytes=len;
        if(active_tls_metrics)active_tls_metrics->tls_bio_bytes+=f->bio_per_stall;
        errno=EAGAIN;return f->tls_want;
    }
    if (f->tls_retry_each_write && !f->tls_retry_injected) {
        f->tls_retry_injected=true; f->pending_tls_input=p; f->pending_tls_bytes=len;
        if(limit_mutation_target){limit_mutation_target->tls_plaintext_limit=2048;limit_mutation_target=NULL;}
        errno=EAGAIN; return f->tls_want;
    }
    if (!transport_is_open(c) || (f->fail_after_first && f->wire_len) ||
        (f->fail_after_bytes && f->wire_len>=f->fail_after_bytes)) { errno=ECONNRESET; return -1; }
    if(f->wire_len && retry_budget){--retry_budget;errno=EAGAIN;return -1;}
    if(f->wire_len && f->retry_once){f->retry_once=false;errno=EAGAIN;return -1;}
    if ((f->wire_len || f->stall_before_first) && esp_timer_get_time()<f->stall_until) {
        errno=EAGAIN;
        if (f->tls_want) { f->pending_tls_input=p; f->pending_tls_bytes=len; return f->tls_want; }
        return -1;
    }
    f->pending_tls_input=NULL;
    f->tls_retry_injected=false;
    if(f->write_delay_ms)Sleep(f->write_delay_ms);
    if(f->max_write && len>f->max_write)len=f->max_write;
    assert(f->wire_len+len <= sizeof(f->wire));
    memcpy(f->wire+f->wire_len,p,len); f->wire_len+=len; return (ssize_t)len;
}
static bool io_retryable(nuwax_ws_client_t *c, ssize_t ret, int err) { (void)c; (void)ret; return err==EAGAIN; }
static unsigned writable_probes, tick_retries;
static bool probe_failure, probe_forced_timeout, probe_fail_after_timeout;
static bool probe_exact_budget;
static unsigned tls_probe_budgets[16], tls_probe_count;

static void vTaskDelay(unsigned ticks) {tick_retries++;if(virtual_clock)virtual_us+=(int64_t)ticks*1000;else Sleep(ticks);}
static esp_err_t transport_wait_write_ready_until(nuwax_ws_client_t *c, int64_t deadline) {
    (void)c;writable_probes++;if(probe_failure)return ESP_FAIL;
    if(virtual_clock)virtual_us+=1000;else Sleep(1);
    if(probe_fail_after_timeout)((fixture_t*)c)->fail_after_bytes=((fixture_t*)c)->wire_len;
    return probe_forced_timeout || io_deadline_expired(deadline) ? ESP_ERR_TIMEOUT : ESP_OK;
}
static esp_err_t transport_wait_tls_retry_until(nuwax_ws_client_t *c, bool want_read, int64_t deadline) {
    fixture_t *f=(fixture_t*)c;
    assert(want_read == (f->tls_want == ESP_TLS_ERR_SSL_WANT_READ));
    assert(f->pending_tls_input);
    if(probe_exact_budget){
        assert(virtual_clock&&tls_probe_count<16U&&deadline>virtual_us);
        tls_probe_budgets[tls_probe_count++]=(unsigned)(deadline-virtual_us)/1000U;
        virtual_us=deadline;return ESP_ERR_TIMEOUT;
    }
    return transport_wait_write_ready_until(c,deadline);
}
static void transport_interrupt_if_epoch(nuwax_ws_client_t *c, uint32_t epoch) {
    if (nuwax_ws_connection_epoch(c)==epoch) atomic_store(&c->connected,false);
}
static void transport_interrupt(nuwax_ws_client_t *c) { atomic_store(&c->connected,false); }
static void lifecycle_store(nuwax_ws_client_t *c, nuwax_ws_lifecycle_state_t state) { atomic_store(&c->lifecycle_state,state); }
static void owner_close_status_store(nuwax_ws_client_t *c, nuwax_ws_close_failure_stage_t stage, esp_err_t err) { (void)c; (void)stage; (void)err; }
static esp_err_t parse_ws_url(const char *url, parsed_ws_url_t *parsed) { (void)url; (void)parsed; return ESP_OK; }
esp_err_t nuwax_ws_owner_close(nuwax_ws_client_t *c) {
    if (close_fails) return ESP_ERR_TIMEOUT;
    atomic_store(&c->connected,false); return ESP_OK;
}
bool nuwax_ws_owner_resources_released(const nuwax_ws_client_t *c) { (void)c; return !close_fails; }
static esp_err_t connect_transport_tx_locked(nuwax_ws_client_t *c, const nuwax_ws_config_t *cfg, parsed_ws_url_t url) {
    (void)cfg; (void)url;
    assert(((mutex_t *)c->tx_frame_lock)->owner==GetCurrentThreadId());
    connect_calls++; atomic_fetch_add(&c->connection_epoch,1);
    atomic_store(&c->connected,true); return ESP_OK;
}
#include "ws_frame_production_host.inc"

static void init_fixture(fixture_t *f) {
    memset(f,0,sizeof(*f)); nuwax_ws_init(&f->ws);
    f->ws.io_lock=xSemaphoreCreateMutex(); f->ws.tx_frame_lock=xSemaphoreCreateMutex();
    ((mutex_t *)f->ws.io_lock)->io_lock=true;
    atomic_store(&f->ws.connected,true); atomic_store(&f->ws.rx_task_running,true);
    atomic_store(&f->ws.connection_epoch,1);
    f->first_chunk=CreateEvent(NULL,TRUE,FALSE,NULL); f->resume=CreateEvent(NULL,TRUE,FALSE,NULL);
}
static void free_fixture(fixture_t *f) {
    destroy_mutex(f->ws.io_lock); destroy_mutex(f->ws.tx_frame_lock);
    CloseHandle(f->first_chunk); CloseHandle(f->resume);
}
typedef struct { fixture_t *f; uint8_t opcode; const uint8_t *payload; size_t len; bool until; esp_err_t result; } sender_t;
static bool test_buffered, test_parts, test_batch;
static DWORD WINAPI sender(void *arg) {
    sender_t *s=arg;
    if(test_batch && s->opcode==2) {
        uint8_t scratch[4U*8142U];nuwax_ws_send_metrics_t m={0};
        nuwax_ws_archive_frame_t frames[4];
        for(unsigned i=0;i<4;i++)frames[i]=(nuwax_ws_archive_frame_t){s->payload,s->payload+64,s->len-64};
        s->result=nuwax_ws_send_archive_batch_if_epoch(&s->f->ws,frames,4,1500,1,scratch,sizeof(scratch),&m);
        return 0;
    }
    if(test_buffered && s->opcode==2) {
        uint8_t scratch[8142];nuwax_ws_send_metrics_t m={0};
        s->result=test_parts ?
            nuwax_ws_send_archive_parts_if_epoch(&s->f->ws,s->payload,s->payload+64,s->len-64,1500,1,scratch,sizeof(scratch),&m) :
            nuwax_ws_send_archive_buffered_if_epoch(&s->f->ws,s->payload,s->len,1500,1,scratch,sizeof(scratch),&m);
        return 0;
    }
    s->result=s->until ? send_frame_until(&s->f->ws,s->opcode,s->payload,s->len,io_deadline_after_ms(2000),1) :
        send_frame_with_timeout(&s->f->ws,s->opcode,s->payload,s->len,2000,2000,true,false,1);
    return 0;
}
static void join(HANDLE thread) { assert(WaitForSingleObject(thread,3000)==WAIT_OBJECT_0); CloseHandle(thread); }
static size_t check_frame(fixture_t *f, size_t offset, uint8_t opcode, const uint8_t *payload, size_t len) {
    const uint8_t *p=f->wire+offset; assert(p[0]==(uint8_t)(0x80|opcode)); assert(p[1]&0x80);
    size_t n=p[1]&127, header=2;
    if(n==126) { n=((size_t)p[2]<<8)|p[3]; header=4; }
    assert(n==len && offset+header+4+len<=f->wire_len);
    for(size_t i=0;i<len;i++) assert((uint8_t)(p[header+4+i]^p[header+i%4])==payload[i]);
    return offset+header+4+len;
}
static uint8_t audio[8128];
static const uint8_t control[]="nuwax";
static void check_interleave(uint8_t opcode, bool until) {
    fixture_t f, cloud; init_fixture(&f); init_fixture(&cloud); f.pause=true;
    if(test_batch){f.max_write=8136;f.retry_once=true;}
    ResetEvent(contended);
    sender_t a={&f,2,audio,sizeof(audio),false,ESP_FAIL}, b={&f,opcode,control,5,until,ESP_FAIL};
    HANDLE ta=CreateThread(NULL,0,sender,&a,0,NULL);
    assert(WaitForSingleObject(f.first_chunk,3000)==WAIT_OBJECT_0);
    HANDLE tb=CreateThread(NULL,0,sender,&b,0,NULL);
    assert(WaitForSingleObject(contended,3000)==WAIT_OBJECT_0);
    /* RX and independent cloud traffic both progress while replay TX waits. */
    assert(xSemaphoreTake(f.ws.io_lock,0)); xSemaphoreGive(f.ws.io_lock);
    assert(send_frame(&cloud.ws,1,control,5)==ESP_OK);
    assert(f.wire_len==(test_batch?32544U:test_buffered?8136U:3968U));
    SetEvent(f.resume); join(ta); join(tb);
    assert(a.result==ESP_OK && b.result==ESP_OK);
    size_t end=check_frame(&f,0,2,audio,sizeof(audio));
    if(test_batch)for(unsigned i=1;i<4;i++)end=check_frame(&f,end,2,audio,sizeof(audio));
    assert(check_frame(&f,end,opcode,control,5)==f.wire_len);
    free_fixture(&cloud); free_fixture(&f);
}
static void check_timeout_failure_reconnect(void) {
    fixture_t f; init_fixture(&f); f.pause=true;
    if(test_batch)f.ws.use_tls=true; // Pause inside the first batch frame.
    sender_t a={&f,2,audio,sizeof(audio),false,ESP_FAIL};
    HANDLE ta=CreateThread(NULL,0,sender,&a,0,NULL);
    assert(WaitForSingleObject(f.first_chunk,3000)==WAIT_OBJECT_0);
    assert(send_frame_until(&f.ws,9,control,5,io_deadline_after_ms(10),1)==ESP_ERR_TIMEOUT);
    assert(atomic_load(&f.ws.connected) && f.wire_len==3968U);
    /* owner_close can interrupt; reconnect cannot publish epoch 2 until TX exits. */
    const nuwax_ws_config_t cfg={.url="ws://test",.token="test"};
    unsigned calls=connect_calls;
    assert(nuwax_ws_connect(&f.ws,&cfg)==ESP_ERR_TIMEOUT);
    assert(connect_calls==calls && nuwax_ws_connection_epoch(&f.ws)==1);
    SetEvent(f.resume); join(ta); assert(a.result!=ESP_OK);
    assert(!atomic_load(&f.ws.connected));
    void *mutex=f.ws.tx_frame_lock; unsigned allocated=allocations;
    assert(nuwax_ws_connect(&f.ws,&cfg)==ESP_OK);
    assert(f.ws.tx_frame_lock==mutex && allocations==allocated && nuwax_ws_connection_epoch(&f.ws)==2);
    size_t bytes=f.wire_len;
    assert(send_frame_with_timeout(&f.ws,1,control,5,30,30,true,false,1)==ESP_ERR_INVALID_STATE);
    assert(send_frame_until(&f.ws,1,control,5,io_deadline_after_ms(30),1)==ESP_ERR_INVALID_STATE);
    assert(f.wire_len==bytes && atomic_load(&f.ws.connected));
    assert(send_frame_with_timeout(&f.ws,1,control,5,30,30,true,false,2)==ESP_OK);
    close_fails=true; calls=connect_calls;
    assert(nuwax_ws_connect(&f.ws,&cfg)==ESP_ERR_TIMEOUT && connect_calls==calls);
    close_fails=false; free_fixture(&f);
}
static void check_partial_failure(void) {
    fixture_t f; init_fixture(&f); f.fail_after_first=true;
    assert(send_frame_with_timeout(&f.ws,2,audio,sizeof(audio),30,30,true,false,1)==ESP_FAIL);
    assert(f.wire_len==3968 && !atomic_load(&f.ws.connected));
    assert(send_frame_until(&f.ws,10,control,5,io_deadline_after_ms(30),1)!=ESP_OK);
    assert(f.wire_len==3968); free_fixture(&f);
}
static DWORD WINAPI hold_lifecycle(void *arg) {
    fixture_t *f=arg;
    assert(xSemaphoreTake(s_lifecycle_lock,100));
    SetEvent(f->first_chunk);
    assert(WaitForSingleObject(f->resume,3000)==WAIT_OBJECT_0);
    xSemaphoreGive(s_lifecycle_lock); return 0;
}
static void check_lifecycle_timeout_and_first_connect(void) {
    fixture_t f; init_fixture(&f);
    HANDLE holder=CreateThread(NULL,0,hold_lifecycle,&f,0,NULL);
    assert(WaitForSingleObject(f.first_chunk,3000)==WAIT_OBJECT_0);
    assert(send_frame_with_timeout(&f.ws,9,control,5,10,30,true,false,1)==ESP_ERR_TIMEOUT);
    assert(((mutex_t *)f.ws.tx_frame_lock)->owner==0);
    assert(atomic_load(&f.ws.connected) && f.wire_len==0);
    assert(send_frame_until(&f.ws,10,control,5,io_deadline_after_ms(10),1)==ESP_ERR_TIMEOUT);
    assert(((mutex_t *)f.ws.tx_frame_lock)->owner==0);
    SetEvent(f.resume); join(holder);
    assert(send_frame(&f.ws,9,control,5)==ESP_OK); free_fixture(&f);

    nuwax_ws_client_t fresh; nuwax_ws_init(&fresh);
    const nuwax_ws_config_t cfg={.url="ws://test",.token="test"};
    unsigned allocated=allocations;
    for(unsigned i=1;i<=100;i++) {
        assert(nuwax_ws_connect(&fresh,&cfg)==ESP_OK);
        assert(nuwax_ws_connection_epoch(&fresh)==i);
        assert(allocations==allocated+1);
    }
    destroy_mutex(fresh.tx_frame_lock);
}
static void check_buffered_transport(void) {
    uint8_t scratch[8142];
    for(unsigned mode=0;mode<5;mode++) {
        fixture_t f;init_fixture(&f);nuwax_ws_send_metrics_t m={0};
        if(mode)f.max_write=1440;
        if(mode==2)f.stall_until=io_deadline_after_ms(35);
        if(mode==3)f.fail_after_first=true;
        if(mode==4)f.ws.use_tls=true;
        int err=nuwax_ws_send_archive_buffered_if_epoch(&f.ws,audio,sizeof(audio),1500,1,scratch,sizeof(scratch),&m);
        assert(err==(mode==3?ESP_FAIL:ESP_OK));
        if(mode!=3)assert(check_frame(&f,0,2,audio,sizeof(audio))==f.wire_len);
        else assert(f.wire_len==1440 && !atomic_load(&f.ws.connected));
        if(mode==0)assert(m.write_calls==1 && f.chunks==1);
        if(mode==1)assert(m.write_calls==6 && m.short_writes==5 && f.chunks==1);
        if(mode==2) {
            assert(m.retries>0 && m.writable_wait_us>0 && m.short_writes==5);
            assert(m.writable_wait_calls==m.retries && m.writable_wait_max_us>0);
            assert(m.writable_wait_max_us<=m.writable_wait_us);
        }
        if(mode==4)assert(f.chunks==3); // TLS chunk geometry unchanged.
        free_fixture(&f);
    }
    fixture_t f;init_fixture(&f);
    assert(nuwax_ws_send_archive_buffered_if_epoch(&f.ws,audio,sizeof(audio),1500,1,audio,sizeof(audio),NULL)==ESP_ERR_INVALID_ARG);
    assert(nuwax_ws_send_archive_buffered_if_epoch(&f.ws,audio,sizeof(audio),1500,2,scratch,sizeof(scratch),NULL)==ESP_ERR_INVALID_STATE);
    assert(f.wire_len==0);free_fixture(&f);
}
static void check_split_transport(void) {
    uint8_t scratch[8142],header[64],tail[8064],joined[8128];
    memcpy(header,audio,64);memcpy(tail,audio+64,8064);
    const size_t lengths[]={1,61,62,63,64,65,3904,3905,8064};
    for(unsigned mode=0;mode<6;mode++)for(unsigned k=0;k<sizeof(lengths)/sizeof(lengths[0]);k++) {
        const size_t n=lengths[k];
        fixture_t f;init_fixture(&f);nuwax_ws_send_metrics_t m={0};
        if(mode==1 || mode==2 || mode==3)f.max_write=32;
        if(mode==2)f.stall_until=io_deadline_after_ms(35);
        if(mode==3)f.fail_after_first=true;
        if(mode>=4)f.ws.use_tls=true;
        if(mode==5)f.max_write=32;
        memcpy(joined,header,64);memcpy(joined+64,tail,n);
        int err=nuwax_ws_send_archive_parts_if_epoch(&f.ws,header,tail,n,1500,1,scratch,sizeof(scratch),&m);
        assert(err==(mode==3?ESP_FAIL:ESP_OK));
        if(mode!=3)assert(check_frame(&f,0,2,joined,n+64)==f.wire_len);
        else assert(f.wire_len==32 && !atomic_load(&f.ws.connected));
        assert(!memcmp(header,audio,64) && !memcmp(tail,audio+64,8064));
        free_fixture(&f);
    }
    fixture_t f;init_fixture(&f);
    assert(nuwax_ws_send_archive_parts_if_epoch(&f.ws,header,tail,8064,1500,2,scratch,sizeof(scratch),NULL)==ESP_ERR_INVALID_STATE);
    assert(nuwax_ws_send_archive_parts_if_epoch(&f.ws,header,tail,8065,1500,1,scratch,sizeof(scratch),NULL)==ESP_ERR_INVALID_ARG);
    assert(nuwax_ws_send_archive_parts_if_epoch(&f.ws,header,tail,1,1500,1,scratch,78,NULL)==ESP_ERR_INVALID_ARG);
    assert(nuwax_ws_send_archive_parts_if_epoch(&f.ws,scratch,tail,1,1500,1,scratch,sizeof(scratch),NULL)==ESP_ERR_INVALID_ARG);
    assert(nuwax_ws_send_archive_parts_if_epoch(&f.ws,header,scratch+1,1,1500,1,scratch,sizeof(scratch),NULL)==ESP_ERR_INVALID_ARG);
    assert(nuwax_ws_send_archive_parts_if_epoch(&f.ws,header,tail,1,1500,1,NULL,0,NULL)==ESP_ERR_INVALID_ARG);
    assert(f.wire_len==0);free_fixture(&f);
    puts("PASS: split header/audio protocol equivalence, length boundaries, short writes, TLS, epoch and overlap rejection; source audio preserved");
}
static void check_mask_alignment(void) {
    _Alignas(4) uint8_t src[8200], dst[8200];
    const uint8_t mask[4]={0x01,0x79,0xbd,0xff};
    for(size_t i=0;i<sizeof(src);i++)src[i]=(uint8_t)(i*37U+5U);
    const size_t sizes[]={0,1,2,3,4,5,7,8,9,15,16,17,31,32,33,63,64,65,
                          125,126,127,3960,3968,8064,8128,8191};
    for(unsigned a=0;a<4;a++)for(unsigned b=0;b<4;b++)for(unsigned phase=0;phase<4;phase++)
        for(unsigned k=0;k<sizeof(sizes)/sizeof(sizes[0]);k++) {
            size_t n=sizes[k];memset(dst,0xcc,sizeof(dst));
            nuwax_ws_mask_copy(dst+a,src+b,n,mask,phase);
            for(size_t i=0;i<n;i++)assert(dst[a+i]==(uint8_t)(src[b+i]^mask[(phase+i)&3U]));
            for(size_t i=0;i<a;i++)assert(dst[i]==0xcc);
            for(size_t i=a+n;i<sizeof(dst);i++)assert(dst[i]==0xcc);
        }
    for(size_t i=0;i<sizeof(src);i++)assert(src[i]==(uint8_t)(i*37U+5U));
}
static void check_batch_transport(void) {
    uint8_t scratch[4U*8142U];
    nuwax_ws_archive_frame_t frames[4];
    const size_t lengths[]={1,61,62,63,3904,3905,8063,8064};
    for(unsigned mode=0;mode<6;mode++)for(size_t count=1;count<=4;count++)
    for(size_t k=0;k<sizeof(lengths)/sizeof(lengths[0]);k++) {
        size_t required=0;
        for(size_t i=0;i<count;i++) {
            frames[i]=(nuwax_ws_archive_frame_t){audio,audio+64,lengths[(k+i)%8]};
            required+=frames[i].bytes+78U;
        }
        fixture_t f;init_fixture(&f);nuwax_ws_send_metrics_t m={0};
        if(mode==1 || mode==2 || mode==3)f.max_write=127;
        if(mode==2)f.stall_until=io_deadline_after_ms(20);
        if(mode==3){f.fail_after_first=true;f.max_write=32;}
        if(mode>=4)f.ws.use_tls=true;
        if(mode==5)f.max_write=127;
        unsigned allocated=allocations, masks=atomic_load(&random_calls);
        int err=nuwax_ws_send_archive_batch_if_epoch(&f.ws,frames,count,1500,1,scratch,required,&m);
        assert(err==(mode==3?ESP_FAIL:ESP_OK));
        if(mode!=3) {
            size_t at=0;
            for(size_t i=0;i<count;i++)at=check_frame(&f,at,2,audio,64+frames[i].bytes);
            assert(at==f.wire_len && atomic_load(&f.ws.connected));
            assert(atomic_load(&random_calls)==masks+count);
            size_t masked=0;
            for(size_t i=0;i<count;i++)masked+=64+frames[i].bytes;
            assert(m.mask_word_bytes+m.mask_scalar_bytes==masked);
            assert(m.encode_us>=m.mask_us+m.mask_random_us);
        } else assert(f.wire_len>0 && !atomic_load(&f.ws.connected));
        assert(m.batch_calls==1 && m.batch_frames==count && m.batch_max_frames==count);
        assert(m.write_bytes==f.wire_len && allocations==allocated);
        if(mode==0)assert(m.write_calls==1);
        for(size_t i=0;i<sizeof(audio);i++)assert(audio[i]==(uint8_t)(i*37));
        free_fixture(&f);
    }
    for(size_t i=0;i<4;i++)frames[i]=(nuwax_ws_archive_frame_t){audio,audio+64,8064};
    fixture_t f;init_fixture(&f);
#define BATCH_CALL(c,t,e,s,n) nuwax_ws_send_archive_batch_if_epoch(&f.ws,frames,c,t,e,s,n,NULL)
    assert(BATCH_CALL(4,1500,2,scratch,sizeof(scratch))==ESP_ERR_INVALID_STATE);
    assert(BATCH_CALL(0,1500,1,scratch,sizeof(scratch))==ESP_ERR_INVALID_ARG);
    assert(BATCH_CALL(5,1500,1,scratch,sizeof(scratch))==ESP_ERR_INVALID_ARG);
    assert(BATCH_CALL(4,1501,1,scratch,sizeof(scratch))==ESP_ERR_INVALID_ARG);
    assert(BATCH_CALL(4,0,1,scratch,sizeof(scratch))==ESP_ERR_INVALID_ARG);
    assert(BATCH_CALL(4,1500,0,scratch,sizeof(scratch))==ESP_ERR_INVALID_ARG);
    assert(BATCH_CALL(4,1500,1,scratch,sizeof(scratch)-1)==ESP_ERR_INVALID_ARG);
    assert(BATCH_CALL(4,1500,1,NULL,sizeof(scratch))==ESP_ERR_INVALID_ARG);
    assert(BATCH_CALL(4,1500,1,(uint8_t *)frames,sizeof(scratch))==ESP_ERR_INVALID_ARG);
    frames[3].header=scratch;
    assert(BATCH_CALL(4,1500,1,scratch,sizeof(scratch))==ESP_ERR_INVALID_ARG);
    frames[3].header=audio;frames[3].data=scratch+1;
    assert(BATCH_CALL(4,1500,1,scratch,sizeof(scratch))==ESP_ERR_INVALID_ARG);
    frames[3].data=audio+64;frames[3].bytes=8065;
    assert(BATCH_CALL(4,1500,1,scratch,sizeof(scratch))==ESP_ERR_INVALID_ARG);
    frames[3].bytes=0;
    assert(BATCH_CALL(4,1500,1,scratch,sizeof(scratch))==ESP_ERR_INVALID_ARG);
    frames[3].bytes=8064;
    assert(f.wire_len==0 && atomic_load(&f.ws.connected));free_fixture(&f);
    // A failure after a whole first frame is still unsafe batch progress.
    for(unsigned tls=0;tls<2;tls++) {
        init_fixture(&f);f.ws.use_tls=tls!=0;f.max_write=8136;f.fail_after_bytes=8136;
        assert(BATCH_CALL(4,1500,1,scratch,sizeof(scratch))==ESP_FAIL);
        assert(f.wire_len==8136 && !atomic_load(&f.ws.connected));
        assert(BATCH_CALL(4,1500,1,scratch,sizeof(scratch))!=ESP_OK && f.wire_len==8136);
        free_fixture(&f);
    }
    for(unsigned progress=0;progress<2;progress++) {
        init_fixture(&f);f.max_write=8136;f.stall_before_first=progress==0;
        f.stall_until=io_deadline_after_ms(200);
        assert(BATCH_CALL(4,10,1,scratch,sizeof(scratch))==ESP_ERR_TIMEOUT);
        assert(f.wire_len==(progress?8136U:0U));
        assert(atomic_load(&f.ws.connected)==(progress==0));
        free_fixture(&f);
    }
#undef BATCH_CALL
    puts("PASS: 192 batch encodings; distinct per-frame masks; source preservation; one TCP write; TLS geometry; zero allocation; argument/epoch/overlap checks; partial-batch disconnect and zero-progress retry");
}
static int64_t archive_cancel_at;
static nuwax_ws_client_t *archive_change_epoch;
static bool archive_cancelled(void) {
    if (esp_timer_get_time() < archive_cancel_at) return false;
    if (archive_change_epoch) {
        atomic_fetch_add(&archive_change_epoch->connection_epoch, 1U);
        return false; // The production epoch check must catch this independently.
    }
    return true;
}
static void check_shared_tls_pause_points(void) {
    virtual_clock=true;
    for(unsigned raw=0;raw<2;raw++)for(unsigned read=0;read<2;read++)for(unsigned point=0;point<3;point++) {
        virtual_us=1000000;
        fixture_t f;init_fixture(&f);f.ws.use_tls=true;
        f.tls_want=read?ESP_TLS_ERR_SSL_WANT_READ:ESP_TLS_ERR_SSL_WANT_WRITE;
        f.stall_before_first=point!=2;f.max_write=point==2?3968:0;
        f.stall_until=virtual_us+350000;
        archive_cancel_at=point?virtual_us+100000:virtual_us;archive_change_epoch=NULL;
        nuwax_ws_send_metrics_t m={.shared_tls_archive=true,.cancelled=archive_cancelled};
        uint8_t scratch[sizeof(audio)+14];
        int err=nuwax_ws_send_archive_buffered_if_epoch(&f.ws,audio,sizeof(audio),250,1,
            raw?NULL:scratch,raw?0:sizeof(scratch),&m);
        assert(f.ws.connected);
        if(!point)assert(err==ESP_ERR_NOT_FINISHED&&!f.wire_len&&!m.write_calls);
        else assert(err==ESP_OK&&!f.pending_tls_input&&check_frame(&f,0,2,audio,sizeof(audio))==f.wire_len);
        const size_t before=f.wire_len;const unsigned calls=m.write_calls;
        assert(nuwax_ws_send_archive_buffered_if_epoch(&f.ws,audio,sizeof(audio),250,1,
            raw?NULL:scratch,raw?0:sizeof(scratch),&m)==ESP_ERR_NOT_FINISHED);
        assert(f.ws.connected&&f.wire_len==before&&m.write_calls==calls);
        free_fixture(&f);
    }
    virtual_clock=false;
    puts("PASS: 12 shared TLS pause points: unsent retains connection; WANT_READ/WRITE and partial frame finish exact bytes; next frame remains paused");
}
static void check_shared_tls_archive(void) {
    {
        fixture_t f; init_fixture(&f); f.ws.use_tls=true;
        uint8_t scratch[sizeof(audio)+14];
        nuwax_ws_send_metrics_t m={.shared_tls_archive=true};
        const unsigned before=writable_probes;
        assert(nuwax_ws_send_archive_buffered_if_epoch(&f.ws,audio,sizeof(audio),250,1,scratch,sizeof(scratch),&m)==ESP_OK);
        assert(writable_probes==before); // Ready writes do not wait for TCP SNDLOWAT.
        assert(m.write_calls==1 && m.write_bytes==8136U); // One complete TLS record.
        assert(check_frame(&f,0,2,audio,sizeof(audio))==f.wire_len);
        free_fixture(&f);
    }
    for (unsigned unbuffered=0; unbuffered<2; ++unbuffered)
    for (unsigned mode=0; mode<7; ++mode) {
        fixture_t f; init_fixture(&f); f.ws.use_tls=true;
        f.tls_want=mode==1 ? ESP_TLS_ERR_SSL_WANT_READ : ESP_TLS_ERR_SSL_WANT_WRITE;
        f.stall_before_first=mode==2 || mode==4;
        f.max_write=3968;
        const int64_t start=esp_timer_get_time();
        f.stall_until=start+(mode==3 ? 4000000LL : 350000LL);
        uint8_t scratch[sizeof(audio)+14];
        nuwax_ws_send_metrics_t m={.shared_tls_archive=mode!=4};
        if(mode>=5) {
            m.cancelled=archive_cancelled; archive_cancel_at=start+100000;
            archive_change_epoch=mode==6?&f.ws:NULL;
        }
        const int err=nuwax_ws_send_archive_buffered_if_epoch(&f.ws,audio,sizeof(audio),250,1,
            unbuffered ? NULL : scratch, unbuffered ? 0U : sizeof(scratch),&m);
        const int64_t elapsed=esp_timer_get_time()-start;
        if(mode<3 || mode==5) {
            assert(err==ESP_OK && f.ws.connected && m.progress_recoveries==1);
            assert(elapsed>=300000 && elapsed<1050000);
            assert(check_frame(&f,0,2,audio,sizeof(audio))==f.wire_len);
            assert(mode==1 ? m.tls_read_waits>0 && !m.tls_write_waits : m.tls_write_waits>0 && !m.tls_read_waits);
        } else {
            assert(err==(mode==6?ESP_ERR_INVALID_STATE:ESP_ERR_TIMEOUT) && !f.ws.connected);
            assert(elapsed<(mode==3?2700000:500000));
            if(mode==3)assert(elapsed>=2500000);
            if(mode==4)assert(!m.tls_read_waits && !m.tls_write_waits);
        }
        archive_change_epoch=NULL;
        free_fixture(&f);
    }
    puts("PASS: buffered AND low-memory formal TLS WANT_READ/WRITE; pending-frame pause drains identical bytes; hard stop and epoch fencing; legacy deadlines unchanged");
    int64_t elapsed[2];
    virtual_clock=true;
    for(unsigned optimized=0;optimized<2;optimized++) {
        virtual_us=1000000;
        fixture_t f;init_fixture(&f);f.ws.use_tls=true;f.tls_retry_each_write=true;
        f.tls_want=ESP_TLS_ERR_SSL_WANT_WRITE;
        uint8_t scratch[sizeof(audio)+14];
        nuwax_ws_send_metrics_t m={.shared_tls_archive=optimized!=0};
        const unsigned ticks_before=tick_retries;
        const int64_t started=esp_timer_get_time();
        for(unsigned i=0;i<6;i++) {
            size_t at=f.wire_len;
            assert(nuwax_ws_send_archive_buffered_if_epoch(&f.ws,audio,sizeof(audio),250,1,scratch,sizeof(scratch),&m)==ESP_OK);
            assert(check_frame(&f,at,2,audio,sizeof(audio))==f.wire_len);
        }
        elapsed[optimized]=esp_timer_get_time()-started;
        if(optimized)assert(tick_retries==ticks_before && m.tls_write_waits>0);
        else assert(tick_retries>ticks_before && !m.tls_write_waits);
        free_fixture(&f);
    }
    virtual_clock=false;
    assert(elapsed[1]*2<elapsed[0]);
    printf("PASS: controlled 1ms-ready TLS workload old_wait_us=%lld readiness_wait_us=%lld; exact bytes, faster socket-ready retry (virtual time, not a Wi-Fi benchmark)\n",(long long)elapsed[0],(long long)elapsed[1]);
}

static void check_archive_progress(void) {
    uint8_t scratch[4U*8142U];
    nuwax_ws_archive_frame_t frames[4];
    for(unsigned i=0;i<4;i++)frames[i]=(nuwax_ws_archive_frame_t){audio,audio+64,8064};
    assert(nuwax_ws_archive_extra_ms(1500)==10500);
    assert(nuwax_ws_archive_progress_deadline(1000,2000)==2000);
    assert(nuwax_ws_archive_progress_deadline(1000,20000000)==6001000);
    for(unsigned mode=0;mode<9;mode++) {
        fixture_t f;init_fixture(&f);f.max_write=4032;
        nuwax_ws_send_metrics_t m={.bounded_progress=true};
        const int64_t started=esp_timer_get_time();
        f.stall_until=started+((mode==1 || mode==7)?8000000LL:5000000LL);
        f.stall_before_first=mode==2 || mode==7;
        archive_change_epoch=NULL;
        if(mode==3 || mode==8){
            m.cancelled=archive_cancelled;archive_cancel_at=started+2100000;
            if(mode==8)archive_change_epoch=&f.ws;
        }
        if(mode==4)f.ws.use_tls=true;
        if(mode==5)m.bounded_progress=false;
        if(mode==6){f.stall_until=0;f.max_write=512;f.write_delay_ms=200;}
        const unsigned timeout=mode==4 || mode==5?20:1500;
        int err=nuwax_ws_send_archive_batch_if_epoch(&f.ws,frames,4,timeout,1,scratch,sizeof(scratch),&m);
        int64_t elapsed=esp_timer_get_time()-started;
        if(mode==0 || mode==2) {
            assert(err==ESP_OK && m.progress_recoveries==1 && elapsed>=4900000 && elapsed<6500000);
            size_t at=0;for(unsigned i=0;i<4;i++)at=check_frame(&f,at,2,audio,8128);
            assert(at==f.wire_len && atomic_load(&f.ws.connected));
        } else if(mode==3 || mode==8) {
            assert(err==ESP_ERR_INVALID_STATE && elapsed>=2000000 && elapsed<2800000);
            assert(f.wire_len>0 && !atomic_load(&f.ws.connected));
        } else {
            assert(err==ESP_ERR_TIMEOUT);
            assert(atomic_load(&f.ws.connected)==(mode==7));
            if(mode==6)assert(elapsed>=11900000 && elapsed<13000000);
            else if(mode==1 || mode==7)assert(elapsed>=5900000 && elapsed<7000000);
            else assert(elapsed<250000); // TLS and non-bulk retain short deadlines.
            if(mode==7)assert(f.wire_len==0);
        }
        archive_change_epoch=NULL;
        free_fixture(&f);
    }
    puts("PASS: bulk recovers 5s TCP stalls before/after first write; 6s no-progress and 12s total limits; cancellation/epoch interrupt extended wait; exact masked bytes; TLS/non-bulk deadlines unchanged");
}

static void check_timeout_outcomes(void) {
    uint8_t scratch[8142];nuwax_ws_archive_frame_t frame={audio,audio+64,8064};
    for(unsigned mode=0;mode<4;mode++) {
        fixture_t f;init_fixture(&f);f.max_write=64;retry_budget=mode==2?1:3;
        nuwax_ws_send_metrics_t m={.bounded_progress=mode!=3};
        probe_forced_timeout=true;probe_fail_after_timeout=mode==2;
        const int err=nuwax_ws_send_archive_batch_if_epoch(&f.ws,&frame,1,1500,1,scratch,sizeof(scratch),&m);
        if(mode==2) {
            assert(err==ESP_FAIL && m.timeout_next_error==1 && !m.timeout_next_progress && !m.timeout_next_blocked);
            assert(!f.ws.connected && f.wire_len==64);
        } else {
            assert(err==ESP_OK && check_frame(&f,0,2,audio,8128)==f.wire_len);
            if(mode==3)assert(!m.timeout_next_progress && !m.timeout_next_blocked && !m.timeout_next_bytes);
            else assert(m.timeout_next_progress==1 && m.timeout_next_blocked==2 && m.timeout_next_bytes==64 && !m.timeout_next_error);
        }
        free_fixture(&f);
    }
    probe_forced_timeout=probe_fail_after_timeout=false;retry_budget=0;
    nuwax_ws_send_metrics_t m={0};
    nuwax_ws_metrics_after_timeout(&m,17,false,99);
    nuwax_ws_metrics_after_timeout(&m,-1,true,10);
    nuwax_ws_metrics_after_timeout(&m,0,false,11);
    nuwax_ws_metrics_after_timeout(NULL,-1,true,1);
    assert(m.timeout_next_progress==1 && m.timeout_next_bytes==17 && m.timeout_next_blocked==1 && m.timeout_next_error==1 && m.timeout_resume_max_us==99);
    puts("PASS: production send timeout outcome attribution: positive/blocked/error separated; unchanged bytes, retries, partial failure close, non-bulk isolation and null metrics");
}
static void check_advisory_keepalive(void) {
    const char *ping="{\"type\":\"ping\",\"now_ms\":123}";
    fixture_t f; init_fixture(&f);
    f.stall_before_first=true; f.stall_until=io_deadline_after_ms(35);
    assert(send_frame_until(&f.ws,1,(const uint8_t*)ping,strlen(ping),io_deadline_after_ms(15),1)==ESP_ERR_TIMEOUT);
    assert(f.wire_len==0 && f.ws.connected);
    assert(nuwax_ws_send_keepalive_until_if_epoch(&f.ws,ping,io_deadline_after_ms(20),1)==ESP_OK);
    assert(check_frame(&f,0,1,(const uint8_t*)ping,strlen(ping))==f.wire_len);
    free_fixture(&f);
    init_fixture(&f); f.ws.use_tls=true; f.stall_before_first=true;f.stall_until=io_deadline_after_ms(150);
    assert(nuwax_ws_send_keepalive_until_if_epoch(&f.ws,ping,io_deadline_after_ms(20),1)==ESP_ERR_TIMEOUT);
    assert(!f.ws.connected); /* TLS attempted-write uncertainty still retires connection. */
    free_fixture(&f);
    init_fixture(&f); unsigned reports=deferred_reports;
    HANDLE holder=CreateThread(NULL,0,hold_lifecycle,&f,0,NULL);
    assert(WaitForSingleObject(f.first_chunk,3000)==WAIT_OBJECT_0);
    assert(nuwax_ws_send_keepalive_until_if_epoch(&f.ws,ping,io_deadline_after_ms(10),1)==ESP_ERR_TIMEOUT);
    assert(f.ws.connected && f.wire_len==0 && deferred_reports==reports);
    SetEvent(f.resume);join(holder);
    assert(nuwax_ws_send_keepalive_until_if_epoch(&f.ws,ping,io_deadline_after_ms(10),2)==ESP_ERR_INVALID_STATE);
    free_fixture(&f);
    puts("PASS: advisory keepalive admission/write separation, exact wire, bounded TLS failure, busy/epoch safety and no persistent busy diagnostic");
}
static fixture_t *record_cancel_target;
static bool record_cancelled(void){return record_cancel_target && record_cancel_target->wire_len>0;}
static void check_archive_tls_record_limits(void) {
    const size_t limits[]={1024,2048,4096,8192};uint8_t scratch[8142];
    virtual_clock=true;virtual_us=1;
    for(unsigned k=0;k<4;k++)for(unsigned want=0;want<2;want++)for(unsigned mode=0;mode<3;mode++) {
        fixture_t f;init_fixture(&f);f.ws.use_tls=true;
        f.tls_want=want?ESP_TLS_ERR_SSL_WANT_READ:ESP_TLS_ERR_SSL_WANT_WRITE;
        f.tls_retry_each_write=true;
        nuwax_ws_send_metrics_t m={.shared_tls_archive=true,.tls_plaintext_limit=limits[k]};
        if(mode==1)f.max_write=127;
        if(mode==2 && limits[k]==4096)limit_mutation_target=&m;
        int e=nuwax_ws_send_archive_parts_if_epoch(&f.ws,audio,audio+64,8064,1500,1,scratch,sizeof(scratch),&m);
        assert(e==ESP_OK && check_frame(&f,0,2,audio,sizeof(audio))==f.wire_len);
        assert(atomic_load(&f.ws.connected) && !f.pending_tls_input && !limit_mutation_target);
        if(!mode)assert(m.short_writes==0);
        if(limits[k]<=4096)assert(m.tls_record_splits>0);
        assert(m.retries>0);free_fixture(&f);
    }
    fixture_t f;init_fixture(&f);f.ws.use_tls=true;record_cancel_target=&f;
    nuwax_ws_send_metrics_t m={.shared_tls_archive=true,.tls_plaintext_limit=4096,.cancelled=record_cancelled};
    assert(nuwax_ws_send_archive_parts_if_epoch(&f.ws,audio,audio+64,8064,1500,1,scratch,sizeof(scratch),&m)==ESP_OK);
    assert(check_frame(&f,0,2,audio,sizeof(audio))==f.wire_len&&atomic_load(&f.ws.connected));
    record_cancel_target=NULL;free_fixture(&f);
    init_fixture(&f);f.ws.use_tls=true;f.fail_after_bytes=4096;
    m=(nuwax_ws_send_metrics_t){.shared_tls_archive=true,.tls_plaintext_limit=4096};
    assert(nuwax_ws_send_archive_parts_if_epoch(&f.ws,audio,audio+64,8064,1500,1,scratch,sizeof(scratch),&m)==ESP_FAIL);
    assert(f.wire_len>0&&f.wire_len<sizeof(audio)&&!atomic_load(&f.ws.connected));free_fixture(&f);
    virtual_clock=false;
    puts("PASS: 24 production TLS record-limit cases (1/2/4/8K), WANT pointer/length stability even when hint changes, partial writes, complete WS bytes, cancellation drain and mid-record failure close");
}

static void check_archive_tls_probe_backoff(void) {
    const unsigned expected[]={2,4,8,16,16,16};uint8_t scratch[8142];
    virtual_clock=true;virtual_us=1;probe_exact_budget=true;
    for(unsigned want=0;want<2U;want++)for(unsigned bio=0;bio<2U;bio++){
        fixture_t f;init_fixture(&f);f.ws.use_tls=true;f.tls_stalls=6;
        f.tls_want=want?ESP_TLS_ERR_SSL_WANT_READ:ESP_TLS_ERR_SSL_WANT_WRITE;
        f.bio_per_stall=bio?1440U:0U;tls_probe_count=0U;
        nuwax_ws_send_metrics_t m={.shared_tls_archive=true,.tls_plaintext_limit=4096};
        assert(nuwax_ws_send_archive_parts_if_epoch(&f.ws,audio,audio+64,8064,1500,1,scratch,sizeof(scratch),&m)==ESP_OK);
        assert(tls_probe_count==6U&&m.retries==6U&&m.timeout_next_blocked==5U&&m.timeout_next_progress==1U);
        assert(m.tls_timeout_bio_progress==(bio?5U:0U));
        for(unsigned i=0;i<tls_probe_count;i++)assert(tls_probe_budgets[i]==(bio?2U:expected[i]));
        assert(check_frame(&f,0,2,audio,sizeof(audio))==f.wire_len&&!f.pending_tls_input&&f.ws.connected);
        free_fixture(&f);
    }
    probe_exact_budget=false;virtual_clock=false;
    puts("PASS: production TLS wait backs off 2/4/8/16ms only without plaintext/BIO progress; WANT_READ/WRITE and identical span/masked bytes preserved");
}
int main(void) {
    check_mask_alignment();
    contended=CreateEvent(NULL,TRUE,FALSE,NULL); assert(contended);
    assert(ensure_lifecycle_lock()==ESP_OK);
    for(size_t i=0;i<sizeof(audio);i++) audio[i]=(uint8_t)(i*37);
    for(unsigned i=0;i<20;i++) {
        check_interleave(9,false); check_interleave(10,true);
        check_interleave(1,true); check_interleave(8,false);
    }
    test_buffered=true;
    for(unsigned i=0;i<20;i++) {
        check_interleave(9,false);check_interleave(10,true);
        check_interleave(1,true);check_interleave(8,false);
    }
    test_parts=true;
    for(unsigned i=0;i<20;i++) {
        check_interleave(9,false);check_interleave(10,true);
        check_interleave(1,true);check_interleave(8,false);
    }
    test_batch=true;
    for(unsigned i=0;i<20;i++) {
        check_interleave(9,false);check_interleave(10,true);
        check_interleave(1,true);check_interleave(8,false);
    }
    test_buffered=test_parts=test_batch=false;check_buffered_transport();
    check_shared_tls_pause_points();
    check_timeout_failure_reconnect(); check_partial_failure();
    test_batch=true;check_timeout_failure_reconnect();test_batch=false;
    check_lifecycle_timeout_and_first_connect();
    check_split_transport();
    check_batch_transport();
    check_advisory_keepalive();
    check_timeout_outcomes();
    check_shared_tls_archive();
    check_archive_tls_record_limits();
    check_archive_tls_probe_backoff();
    check_archive_progress();
    fixture_t stalled; init_fixture(&stalled);
    writable_probes=tick_retries=0;
    stalled.stall_until=io_deadline_after_ms(350);
    assert(nuwax_ws_send_archive_chunk_if_epoch(&stalled.ws,audio,sizeof(audio),1)==ESP_ERR_TIMEOUT);
    assert(!atomic_load(&stalled.ws.connected));
    assert(writable_probes>0 && tick_retries==0);free_fixture(&stalled);
    init_fixture(&stalled); stalled.stall_until=io_deadline_after_ms(350);
    assert(nuwax_ws_send_archive_chunk_with_timeout_if_epoch(&stalled.ws,audio,sizeof(audio),1500,1)==ESP_OK);
    assert(check_frame(&stalled,0,2,audio,sizeof(audio))==stalled.wire_len);
    assert(atomic_load(&stalled.ws.connected)); free_fixture(&stalled);
    init_fixture(&stalled);stalled.stall_until=io_deadline_after_ms(350);probe_failure=true;
    assert(nuwax_ws_send_archive_chunk_with_timeout_if_epoch(&stalled.ws,audio,sizeof(audio),1500,1)==ESP_FAIL);
    assert(!atomic_load(&stalled.ws.connected));free_fixture(&stalled);probe_failure=false;
    destroy_mutex(s_lifecycle_lock); CloseHandle(contended);
    puts("PASS: 320 production legacy/buffered/split/batch interleavings; full mask/decode; short writes/backpressure metrics; partial failure closes; TLS geometry preserved; RX/other-client progress; epoch fencing; 100 reconnects");
    assert(atomic_load(&tls_profiles_started)==atomic_load(&tls_profiles_finished));
    return 0;
}
