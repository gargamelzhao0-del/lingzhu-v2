/* Executes the production observer/waiter. Only SDK/RTOS boundaries are
 * replaced; real Windows threads exercise permanent semaphore lifetime. */
#define WIN32_LEAN_AND_MEAN 1
#include <windows.h>
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>
#define NUWAX_SD_TCP_RECOVERY_HOST_TEST 1
#define CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE 1
#define CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE 0
#define CONFIG_LWIP_TCP_SND_BUF_DEFAULT 131072U
#define portTICK_PERIOD_MS 10U
#define ESP_OK 0
#define ESP_TIMER_TASK 0
#define ERR_OK 0
#define ERR_MEM -1
#define ESTABLISHED 4
#define TF_NAGLEMEMERR 0x80U
#define TF_RTO 0x400U
typedef unsigned TickType_t;
typedef int err_t;
typedef CRITICAL_SECTION portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {0}
static _Thread_local unsigned critical_depth;
#define portENTER_CRITICAL(p) do { EnterCriticalSection(p); ++critical_depth; } while(0)
#define portEXIT_CRITICAL(p) do { --critical_depth; LeaveCriticalSection(p); } while(0)
typedef struct {HANDLE handle;} StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
static bool real_clock;
static int64_t virtual_us=1000000;
static int64_t esp_timer_get_time(void){return real_clock?(int64_t)GetTickCount64()*1000:virtual_us;}
static SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *s){
    assert(!critical_depth);s->handle=CreateSemaphore(NULL,0,1,NULL);assert(s->handle);return s;
}
static int xSemaphoreGive(SemaphoreHandle_t s){
    assert(!critical_depth);int ok=ReleaseSemaphore(s->handle,1,NULL);
    assert(ok||GetLastError()==ERROR_TOO_MANY_POSTS);return ok;
}
static void (*before_take)(void);
static int xSemaphoreTake(SemaphoreHandle_t s,TickType_t ticks){
    assert(!critical_depth);
    if(ticks&&before_take){void (*cb)(void)=before_take;before_take=NULL;cb();}
    DWORD result=WaitForSingleObject(s->handle,real_clock?ticks*10:0);
    assert(result==WAIT_OBJECT_0||result==WAIT_TIMEOUT);
    if(!real_clock&&result==WAIT_TIMEOUT)virtual_us+=(int64_t)ticks*10000;
    return result==WAIT_OBJECT_0;
}
typedef struct {uint32_t addr;bool v4;} ip_addr_t;
#define IP_IS_V4_VAL(ip) ((ip).v4)
#define ip_2_ip4(ip) (ip)
#define ip4_addr_get_u32(ip) ((ip)->addr)
struct tcp_seg {struct tcp_seg *next;unsigned len;};
struct tcp_pcb {struct tcp_pcb *next;ip_addr_t local_ip,remote_ip;
    uint16_t local_port,remote_port;int state;
    uint32_t flags,snd_wnd,snd_buf,snd_queuelen,cwnd,nrtx,lastack,snd_nxt;
    struct tcp_seg *unsent;};
static struct tcp_pcb *tcp_active_pcbs;
typedef unsigned socklen_t;
#define AF_INET 2
struct in_addr {uint32_t s_addr;};
struct sockaddr {unsigned short sa_family;};
struct sockaddr_storage {unsigned short ss_family;uint16_t port;struct in_addr addr;};
struct sockaddr_in {unsigned short sin_family;uint16_t sin_port;struct in_addr sin_addr;};
static uint16_t ntohs(uint16_t p){return p;}
static int getsockname(int fd,struct sockaddr *a,socklen_t *n){
    (void)n;assert(fd==7);struct sockaddr_in *s=(void*)a;*s=(struct sockaddr_in){AF_INET,1234,{1}};return 0;
}
static int getpeername(int fd,struct sockaddr *a,socklen_t *n){
    (void)n;assert(fd==7);struct sockaddr_in *s=(void*)a;*s=(struct sockaddr_in){AF_INET,443,{2}};return 0;
}
typedef void *esp_timer_handle_t;
typedef struct {void (*callback)(void*);int dispatch_method;const char *name;} esp_timer_create_args_t;
static int esp_timer_create(const esp_timer_create_args_t *a,esp_timer_handle_t *t){(void)a;*t=(void*)1;return 0;}
static int esp_timer_start_once(esp_timer_handle_t t,uint64_t delay){(void)t;(void)delay;return 0;}
static err_t tcpip_try_callback(void (*cb)(void*),void *arg){cb(arg);return 0;}
static void (*during_output)(void);
err_t __real_tcp_output(struct tcp_pcb *pcb){
    (void)pcb;assert(!critical_depth);
    if(during_output){void (*cb)(void)=during_output;during_output=NULL;cb();}
    errno=1234;return ERR_OK;
}
#include "sd_tcp_recovery.c"
static struct tcp_pcb pcb;
static uint32_t token;
static uint32_t open_file(uint32_t epoch){return nuwax_sd_tcp_recovery_begin(0x1234,epoch,7);}
static nuwax_sd_tcp_snapshot_t snapshot(uint32_t epoch){
    nuwax_sd_tcp_snapshot_t state={0};assert(nuwax_sd_tcp_snapshot(0x1234,epoch,&state));return state;
}
static void observe(void){assert(__wrap_tcp_output(&pcb)==ERR_OK&&errno==1234);}
static void acknowledge(void){
    pcb.lastack=pcb.snd_nxt;pcb.snd_buf=CONFIG_LWIP_TCP_SND_BUF_DEFAULT;observe();
}
static void enqueue(void){pcb.snd_nxt+=8192;pcb.snd_buf-=8192;observe();}
static void acknowledge_race(void){virtual_us+=1000;acknowledge();}
static uint32_t replacement;
static void replace_scope(void){
    nuwax_sd_tcp_recovery_end(token,NULL);replacement=open_file(2);assert(replacement);
}
static DWORD WINAPI ack_thread(void *arg){Sleep((unsigned)(uintptr_t)arg%3);acknowledge();return 0;}
static DWORD WINAPI close_thread(void *arg){(void)arg;Sleep(2);nuwax_sd_tcp_recovery_close(0x1234,3);return 0;}
int main(void){
    InitializeCriticalSection(&s_lock);nuwax_sd_tcp_recovery_prepare();
    pcb=(struct tcp_pcb){.local_ip={1,true},.remote_ip={2,true},.local_port=1234,.remote_port=443,
        .state=ESTABLISHED,.snd_wnd=262144,.snd_buf=131072,.cwnd=64000,.lastack=100,.snd_nxt=100};
    tcp_active_pcbs=&pcb;token=open_file(1);assert(token);observe();
    enqueue();nuwax_sd_tcp_snapshot_t state=snapshot(1);assert(state.queued_bytes==8192);
    virtual_us+=50000;acknowledge();
    assert(nuwax_sd_tcp_wait_progress(0x1234,1,state.sequence,20)==1);
    state=snapshot(1);assert(state.send_free==131072&&state.peer_window==262144);
    assert(nuwax_sd_tcp_wait_progress(0x1234,999,state.sequence,20)==-1);
    assert(nuwax_sd_tcp_wait_progress(99,1,state.sequence,20)==-1);
    assert(!nuwax_sd_tcp_snapshot(0x1234,1,NULL));
    /* ACK races exactly between predicate check and semaphore Take. */
    enqueue();state=snapshot(1);(void)xSemaphoreTake(s_event,0);
    before_take=acknowledge_race;
    assert(nuwax_sd_tcp_wait_progress(0x1234,1,state.sequence,20)==1&&!before_take);
    state=snapshot(1);int64_t start=virtual_us;
    xSemaphoreGive(s_event); /* A stale event must not count as TCP progress. */
    assert(nuwax_sd_tcp_wait_progress(0x1234,1,state.sequence,20)==0&&virtual_us-start==20000);
    /* Sequence numbers cross 2^32 without negative/giant accounting. */
    pcb.lastack=pcb.snd_nxt=UINT32_MAX-4095;observe();enqueue();virtual_us+=40000;acknowledge();
    nuwax_sd_tcp_recovery_metrics_t metrics;nuwax_sd_tcp_recovery_end(token,&metrics);
    assert(metrics.rtt_samples==3&&metrics.rtt_min_us==1000&&metrics.rtt_max_us==50000);
    assert(metrics.acked_bytes>=3U*8192&&metrics.queued_bytes_peak==8192);
    assert(metrics.cwnd_max==64000&&metrics.inflight_peak==8192);
    assert(metrics.peer_window_min==262144&&metrics.peer_window_max==262144);
    assert(!s_scope.open&&pcb.cwnd==64000&&pcb.snd_wnd==262144);
    puts("PASS production TCP flow: >64KiB capacity, ACK-before-wait, check/Take race, stale wake, sequence wrap, high-resolution RTT and unchanged SDK state");
    token=open_file(1);assert(token);during_output=replace_scope;observe();
    assert(s_scope.token==replacement&&!s_scope.snapshot.valid&&!s_scope.metrics.outputs);
    nuwax_sd_tcp_recovery_end(replacement,NULL);
    puts("PASS old output completing after same-endpoint end/begin cannot publish into replacement scope");
    token=open_file(3);assert(token);observe();real_clock=true;
    for(unsigned i=0;i<400;i++){
        enqueue();state=snapshot(3);
        HANDLE thread=CreateThread(NULL,0,ack_thread,(void*)(uintptr_t)i,0,NULL);assert(thread);
        assert(nuwax_sd_tcp_wait_progress(0x1234,3,state.sequence,1000)==1);
        assert(WaitForSingleObject(thread,2000)==WAIT_OBJECT_0);CloseHandle(thread);
    }
    state=snapshot(3);HANDLE thread=CreateThread(NULL,0,close_thread,NULL,0,NULL);assert(thread);
    assert(nuwax_sd_tcp_wait_progress(0x1234,3,state.sequence,1000)==-1);
    assert(WaitForSingleObject(thread,2000)==WAIT_OBJECT_0);CloseHandle(thread);
    nuwax_sd_tcp_recovery_end(token,&metrics);assert(metrics.ack_events==400);
    assert(pcb.cwnd==64000&&pcb.nrtx==0&&pcb.flags==0);
    CloseHandle(s_event->handle);DeleteCriticalSection(&s_lock);
    puts("PASS 400 real ACK/wait interleavings and close while waiting; permanent semaphore, epoch fences and original output return preserved");
    return 0;
}
