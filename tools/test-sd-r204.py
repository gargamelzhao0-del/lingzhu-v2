"""Scoped SDK socket timing and bounded formal TCP recovery.

Compiles the production modules and the installed IDF semaphore wait/SELECT
cleanup against deterministic RTOS/network boundaries. Includes real Windows
thread interleavings for callbacks that outlive the original semaphore wait.
Does not operate hardware or claim end-to-end hardware throughput.
"""
from pathlib import Path
import subprocess, re, json, hashlib, os, ctypes

root = Path(__file__).resolve().parents[1]
out = root / 'build-host-checks/sd-r204'
out.mkdir(parents=True, exist_ok=True)
if os.name == 'nt': ctypes.windll.kernel32.SetErrorMode(0x8003)
sdk = Path(os.environ.get('IDF_PATH', 'C:/esp/v6.0/esp-idf'))

def block(text, marker):
    at = text.index(marker); start = text.index('{', at); end = start + 1; depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}'); end += 1
    return text[at:end]

sem_file = sdk / 'components/lwip/port/freertos/sys_arch.c'
socket_file = sdk / 'components/lwip/lwip/src/api/sockets.c'
sem = block(sem_file.read_text(), 'sys_arch_sem_wait(sys_sem_t *sem, u32_t timeout)')
sem = 'u32_t\n' + sem.replace('sys_arch_sem_wait(', '__real_sys_arch_sem_wait(', 1)
cleanup = block(socket_file.read_text(), 'if (API_SELECT_CB_VAR_REF(select_cb).sem_signalled && (!waited || (waitres == SYS_ARCH_TIMEOUT)))')
(out / 'sdk_wait.inc').write_text(sem, encoding='utf-8')
(out / 'sdk_select_cleanup.inc').write_text(cleanup, encoding='utf-8')

def run(name, code):
    source = out / (name + '.c'); exe = out / (name + '.exe')
    source.write_text(code, encoding='utf-8')
    subprocess.run(['gcc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
        '-Wno-unused-function', '-I'+str(root/'main'), '-I'+str(out), str(source), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=90)
    return exe

run('precise-sdk-wait', r'''
#include <windows.h>
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <limits.h>
#include <stdatomic.h>
#define NUWAX_SOCKET_WAIT_HOST_TEST 1
#define CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE 1
typedef uint32_t u32_t;
typedef int BaseType_t;
typedef unsigned TickType_t;
typedef struct { volatile LONG count; bool alive; } StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
typedef SemaphoreHandle_t sys_sem_t;
typedef CRITICAL_SECTION portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {0}
#define portENTER_CRITICAL(p) EnterCriticalSection(p)
#define portEXIT_CRITICAL(p) LeaveCriticalSection(p)
#define pdTRUE 1
#define errQUEUE_EMPTY 0
#define portMAX_DELAY UINT_MAX
#define portTICK_PERIOD_MS 10U
#define SYS_ARCH_TIMEOUT UINT32_MAX
#define LWIP_ASSERT(text,cond) assert(cond)
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_TIMER_TASK 0
typedef struct { void (*callback)(void *); int dispatch_method; const char *name; } esp_timer_create_args_t;
typedef struct { void (*callback)(void *); bool armed; int64_t at; } timer_t;
typedef timer_t *esp_timer_handle_t;
static timer_t timer;
static int64_t now_us, ack_at=-1;
static uintptr_t task=1;
static bool create_failure, start_failure, real_signalled;
static unsigned real_calls, timer_callbacks, fallback_ticks, mode;
static SemaphoreHandle_t socket_sem;
static HANDLE entered, release_callback, worker;
static bool hold_callback;
static int64_t esp_timer_get_time(void){ return now_us; }
static uintptr_t xTaskGetCurrentTaskHandle(void){return task;}
static SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *s){s->count=0;s->alive=true;return s;}
static int esp_timer_create(const esp_timer_create_args_t *args,esp_timer_handle_t *t){
 if(create_failure){return -1;}timer.callback=args->callback;*t=&timer;return 0;
}
static int esp_timer_start_once(esp_timer_handle_t t,uint64_t us){
 if(start_failure){return -1;}assert(!t->armed);t->armed=true;t->at=now_us+(int64_t)us;return 0;
}
static int esp_timer_stop(esp_timer_handle_t t){t->armed=false;return 0;}
static int xSemaphoreGive(SemaphoreHandle_t sem){
 assert(sem&&sem->alive);
 if(sem==socket_sem&&hold_callback){
   SetEvent(entered);assert(WaitForSingleObject(release_callback,5000)==WAIT_OBJECT_0);
 }
 return InterlockedCompareExchange(&sem->count,1,0)==0;
}
static DWORD WINAPI fire_thread(void *unused){(void)unused;timer.callback(NULL);return 0;}
static int take_now(SemaphoreHandle_t s){return InterlockedCompareExchange(&s->count,0,1)==1;}
static int xSemaphoreTake(SemaphoreHandle_t sem,unsigned ticks){
 assert(sem&&sem->alive);if(take_now(sem))return pdTRUE;if(!ticks)return errQUEUE_EMPTY;
 if(ticks==portMAX_DELAY){
   /* The original wait has returned while the real callback is paused BEFORE
    * Give. The wrapper must join it before the caller can free the sem. */
   assert(hold_callback);SetEvent(release_callback);
   assert(WaitForSingleObject(worker,5000)==WAIT_OBJECT_0);assert(take_now(sem));return pdTRUE;
 }
 if(mode==1||mode==2){
   unsigned current=mode;mode=0;now_us=timer.at;
   hold_callback=true;ResetEvent(entered);ResetEvent(release_callback);
   worker=CreateThread(NULL,0,fire_thread,NULL,0,NULL);assert(worker);
   assert(WaitForSingleObject(entered,5000)==WAIT_OBJECT_0);
   /* Mode 1: real ACK was consumed just before the late Give. Mode 2:
    * the original RTOS timeout won before the late timer Give. */
   real_signalled=current==1;return current==1?pdTRUE:errQUEUE_EMPTY;
 }
 const int64_t end=now_us+(int64_t)ticks*10000;
 fallback_ticks=ticks;
 for(unsigned events=0;events<6;events++){
   if(ack_at>=now_us&&ack_at<=end&&(!timer.armed||ack_at<timer.at)){
     now_us=ack_at;ack_at=-1;real_signalled=true;InterlockedExchange(&sem->count,1);
   }else if(timer.armed&&timer.at<=end){
     now_us=timer.at;timer.armed=false;timer_callbacks++;
     timer.callback(NULL);
   }else{now_us=end;return errQUEUE_EMPTY;}
   if(take_now(sem))return pdTRUE;
 }
 assert(!"event-loop bound");return 0;
}
#include "sd_socket_wait.c"
/* Execute the SDK function itself, rather than imitating its tick math. */
#include "sdk_wait.inc"
static u32_t wait_sdk(sys_sem_t *sem,u32_t timeout){real_calls++;return __wrap_sys_arch_sem_wait(sem,timeout);}
static void select_cleanup(sys_sem_t *sem,u32_t result){
 struct { bool sem_signalled; sys_sem_t *sem; } select_cb={real_signalled,sem};
 int waited=1;u32_t waitres=result;
#define API_SELECT_CB_VAR_REF(value) value
#define sys_arch_sem_wait(sem,timeout) wait_sdk(sem,timeout)
#include "sdk_select_cleanup.inc"
#undef sys_arch_sem_wait
#undef API_SELECT_CB_VAR_REF
}
static nuwax_ws_send_metrics_t metrics;
static sys_sem_t fresh(StaticSemaphore_t *store){
 hold_callback=false;now_us=100000;ack_at=-1;real_signalled=false;mode=0;
 socket_sem=xSemaphoreCreateBinaryStatic(store);return socket_sem;
}
static void release_sem(sys_sem_t s){assert(!s_wait.armed&&!s_wait.callback_active&&!s_wait.sem&&!timer.armed);assert(!s->count);s->alive=false;}
int main(void){
 setvbuf(stdout,NULL,_IONBF,0);
 InitializeCriticalSection(&s_lock);entered=CreateEvent(NULL,TRUE,FALSE,NULL);release_callback=CreateEvent(NULL,TRUE,FALSE,NULL);
 StaticSemaphore_t store;sys_sem_t sem=fresh(&store);
 metrics.shared_tls_archive=true;
 assert(!nuwax_sd_socket_wait_begin(&metrics));create_failure=true;nuwax_sd_socket_wait_prepare();assert(!nuwax_sd_socket_wait_begin(&metrics));
 create_failure=false;nuwax_sd_socket_wait_prepare();assert(s_timer);
 int64_t begin=now_us;assert(wait_sdk(&sem,2)==SYS_ARCH_TIMEOUT);assert(now_us-begin==20000&&fallback_ticks==2);
 assert(nuwax_sd_socket_wait_begin(&metrics));begin=now_us;
 assert(wait_sdk(&sem,2)==SYS_ARCH_TIMEOUT);assert(now_us-begin==2000);select_cleanup(&sem,SYS_ARCH_TIMEOUT);release_sem(sem);
 nuwax_sd_socket_wait_end();puts("PASS installed SDK wait: 2ms scope removes the 20ms tick timeout; outside scope unchanged");
 sem=fresh(&store);assert(nuwax_sd_socket_wait_begin(&metrics));
 assert(wait_sdk(&sem,2)==SYS_ARCH_TIMEOUT);real_signalled=true;assert(xSemaphoreGive(sem));
 select_cleanup(&sem,SYS_ARCH_TIMEOUT);release_sem(sem);nuwax_sd_socket_wait_end();
 puts("PASS real ACK arriving after timer completion is drained by installed SDK SELECT cleanup");
 sem=fresh(&store);assert(nuwax_sd_socket_wait_begin(&metrics));ack_at=now_us+500;begin=now_us;
 assert(wait_sdk(&sem,16)==0&&now_us-begin==500);select_cleanup(&sem,0);release_sem(sem);nuwax_sd_socket_wait_end();
 puts("PASS genuine socket event wakes before timeout, without leaving semaphore tokens");
 sem=fresh(&store);assert(nuwax_sd_socket_wait_begin(&metrics));task=2;begin=now_us;
 assert(wait_sdk(&sem,2)==SYS_ARCH_TIMEOUT&&now_us-begin==20000);nuwax_sd_socket_wait_end();assert(atomic_load(&s_owner)==1);
 task=1;begin=now_us;assert(wait_sdk(&sem,17)==SYS_ARCH_TIMEOUT&&now_us-begin==30000);
 start_failure=true;begin=now_us;assert(wait_sdk(&sem,2)==SYS_ARCH_TIMEOUT&&now_us-begin==20000);start_failure=false;
 release_sem(sem);nuwax_sd_socket_wait_end();puts("PASS task/scope/timeout isolation and optional-resource failure retain SDK behavior");
 for(unsigned which=1;which<=2;which++)for(unsigned i=0;i<200;i++){
   sem=fresh(&store);assert(nuwax_sd_socket_wait_begin(&metrics));mode=which;
   assert(wait_sdk(&sem,2)==SYS_ARCH_TIMEOUT);hold_callback=false;
   assert(WaitForSingleObject(worker,5000)==WAIT_OBJECT_0);CloseHandle(worker);
   select_cleanup(&sem,SYS_ARCH_TIMEOUT);release_sem(sem);nuwax_sd_socket_wait_end();
 }
 puts("PASS 400 real thread races: ACK/RTOS timeout before callback Give; join and SDK cleanup leave no stale token or freed-sem access");
 sem=fresh(&store);assert(nuwax_sd_socket_wait_begin(&metrics));void (*selected)(void*)=timer.callback;
 assert(wait_sdk(&sem,2)==SYS_ARCH_TIMEOUT);now_us+=100;
 /* A callback selected before stop cannot expire a later scope early. */
 portENTER_CRITICAL(&s_lock);s_wait.armed=true;s_wait.deadline_us=now_us+8000;s_wait.sem=sem;s_wait.fired=false;portEXIT_CRITICAL(&s_lock);
 selected(NULL);assert(!s_wait.fired&&!sem->count);s_wait.armed=false;s_wait.sem=NULL;
 release_sem(sem);nuwax_sd_socket_wait_end();
 printf("PASS precise-wait statistics calls=%u timeouts=%u fallbacks=%u; no timer allocation in hot path\n",metrics.precise_wait_calls,metrics.precise_wait_timeouts,metrics.precise_wait_fallbacks);
 return 0;
}
''')

run('formal-tcp-recovery', r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define NUWAX_SD_TCP_RECOVERY_HOST_TEST 1
#define CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE 1
#define CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE 0
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
static unsigned locked;
#define portENTER_CRITICAL(p) do{(void)(p);assert(!locked);locked=1;}while(0)
#define portEXIT_CRITICAL(p) do{(void)(p);assert(locked);locked=0;}while(0)
typedef int err_t;
#define ERR_OK 0
#define ERR_MEM -1
#define ERR_RTE -4
#define ESP_OK 0
#define ESP_TIMER_TASK 0
#define ESTABLISHED 4
#define TF_NAGLEMEMERR 0x80U
#define TF_RTO 0x400U
#define CONFIG_LWIP_TCP_SND_BUF_DEFAULT 46080U
#define portTICK_PERIOD_MS 10U
typedef unsigned TickType_t;
typedef struct {unsigned count;} StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
static SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *s){s->count=0;return s;}
static int xSemaphoreGive(SemaphoreHandle_t s){s->count=1;return 1;}
static int xSemaphoreTake(SemaphoreHandle_t s,TickType_t ticks);
typedef unsigned socklen_t;
#define AF_INET 2
struct in_addr {uint32_t s_addr;};
struct sockaddr {unsigned short sa_family;};
struct sockaddr_storage {unsigned short ss_family;uint16_t port;struct in_addr addr;};
struct sockaddr_in {unsigned short sin_family;uint16_t sin_port;struct in_addr sin_addr;};
static uint16_t ntohs(uint16_t p){return p;}
static unsigned family=AF_INET;
static int getsockname(int sock,struct sockaddr *a,socklen_t *len){(void)len;struct sockaddr_in *s=(void*)a;s->sin_family=family;s->sin_port=1234;s->sin_addr.s_addr=1;return sock<0?-1:0;}
static int getpeername(int sock,struct sockaddr *a,socklen_t *len){(void)len;struct sockaddr_in *s=(void*)a;s->sin_family=family;s->sin_port=443;s->sin_addr.s_addr=2;return sock<0?-1:0;}
typedef struct {uint32_t addr;bool v4;} ip_addr_t;
#define IP_IS_V4_VAL(ip) ((ip).v4)
#define ip_2_ip4(ip) (ip)
#define ip4_addr_get_u32(ip) ((ip)->addr)
struct tcp_seg {struct tcp_seg *next;unsigned len;};
struct tcp_pcb {struct tcp_pcb *next;ip_addr_t local_ip,remote_ip;uint16_t local_port,remote_port;int state;
 unsigned flags,snd_wnd,snd_buf,snd_queuelen,cwnd,nrtx,lastack,snd_nxt;struct tcp_seg *unsent;};
static struct tcp_pcb *tcp_active_pcbs;
typedef int esp_err_t;
typedef struct {void (*callback)(void*);int dispatch_method;const char *name;} esp_timer_create_args_t;
typedef struct {void (*callback)(void*);bool armed;int64_t due;} timer_t;
typedef timer_t *esp_timer_handle_t;
static timer_t timer;
static int64_t now_us;
static int xSemaphoreTake(SemaphoreHandle_t s,TickType_t ticks){if(s->count){s->count=0;return 1;}now_us+=(int64_t)ticks*10000;return 0;}
static bool create_failure,post_failure,execute_inline;
static unsigned output_calls,delivered,failures_left;
static err_t output_result=ERR_OK;
static void (*posted)(void*);
static int64_t esp_timer_get_time(void){return now_us;}
static int esp_timer_create(const esp_timer_create_args_t *args,esp_timer_handle_t *handle){assert(!locked);if(create_failure)return -1;timer.callback=args->callback;*handle=&timer;return 0;}
static int esp_timer_start_once(esp_timer_handle_t t,uint64_t delay){assert(!locked);if(t->armed)return -1;t->armed=true;t->due=now_us+(int64_t)delay;return 0;}
static err_t tcpip_try_callback(void (*cb)(void*),void *arg){assert(!locked&&!posted);if(post_failure)return ERR_MEM;if(execute_inline)cb(arg);else posted=cb;return ERR_OK;}
static void (*during_output)(void);
err_t __real_tcp_output(struct tcp_pcb *pcb){
 assert(!locked);output_calls++;if(during_output){void (*cb)(void)=during_output;during_output=NULL;cb();}
 errno=123;err_t result=failures_left?(failures_left--,ERR_MEM):output_result;
 if(result==ERR_MEM)pcb->flags|=TF_NAGLEMEMERR;
 else if(result==ERR_OK&&pcb->snd_wnd&&pcb->unsent){
   while(pcb->unsent){delivered+=pcb->unsent->len;pcb->unsent=pcb->unsent->next;}
   pcb->flags&=~TF_NAGLEMEMERR;
 }
 return result;
}
#include "sd_tcp_recovery.c"
static struct tcp_seg payload={NULL,8064};
static struct tcp_pcb pcb;
static void fresh(void){
 pcb=(struct tcp_pcb){.local_ip={1,true},.remote_ip={2,true},.local_port=1234,.remote_port=443,.state=ESTABLISHED,
 .snd_wnd=50000,.snd_buf=46080,.cwnd=10000,.unsent=&payload};tcp_active_pcbs=&pcb;
 output_result=ERR_OK;failures_left=0;delivered=0;now_us+=1000000;timer.armed=false;post_failure=false;execute_inline=false;
}
static void run_posted(void){assert(posted);void (*cb)(void*)=posted;posted=NULL;cb(NULL);}
static void fire(void){assert(timer.armed);now_us=timer.due;timer.armed=false;timer.callback(NULL);}
static uint32_t begin_file(unsigned epoch){uint32_t token=nuwax_sd_tcp_recovery_begin(0x1234,epoch,7);assert(token);return token;}
static nuwax_sd_tcp_recovery_metrics_t end_file(uint32_t token){nuwax_sd_tcp_recovery_metrics_t m;nuwax_sd_tcp_recovery_end(token,&m);return m;}
int main(void){
 fresh();assert(!nuwax_sd_tcp_recovery_begin(1,1,7));create_failure=true;nuwax_sd_tcp_recovery_prepare();assert(!nuwax_sd_tcp_recovery_begin(1,1,7));
 create_failure=false;nuwax_sd_tcp_recovery_prepare();assert(s_timer);
 failures_left=1;assert(__wrap_tcp_output(&pcb)==ERR_MEM&&errno==123&&!timer.armed);
 family=10;assert(!nuwax_sd_tcp_recovery_begin(1,1,7));family=AF_INET;
 fresh();uint32_t token=begin_file(1);assert(!nuwax_sd_tcp_recovery_begin(99,1,7));
 pcb.remote_ip.addr=3;failures_left=1;assert(__wrap_tcp_output(&pcb)==ERR_MEM&&!timer.armed);pcb.remote_ip.addr=2;
 failures_left=1;unsigned calls=output_calls;int64_t start=now_us;
 assert(__wrap_tcp_output(&pcb)==ERR_MEM&&errno==123&&timer.armed&&delivered==0);
 unsigned window=pcb.cwnd,seq_state=pcb.nrtx;fire();assert(now_us-start==5000&&posted);run_posted();
 assert(output_calls==calls+2&&delivered==8064&&!pcb.unsent&&pcb.cwnd==window&&pcb.nrtx==seq_state);
 nuwax_sd_tcp_recovery_metrics_t m=end_file(token);assert(m.output_nomem==1&&m.retries==1&&m.recovered==1&&!m.exhausted);
 puts("PASS formal endpoint only: rejected TCP-owned output recovered at 5ms, same packet delivered once, SDK congestion/retransmit state unchanged");
 fresh();token=begin_file(2);failures_left=1000;assert(__wrap_tcp_output(&pcb)==ERR_MEM);start=now_us;
 for(unsigned i=0;i<4;i++){fire();assert(posted);run_posted();}
 assert(now_us-start==75000&&!timer.armed&&pcb.unsent==&payload&&!delivered);
 assert(__wrap_tcp_output(&pcb)==ERR_MEM&&!timer.armed);m=end_file(token);assert(m.retries==4&&m.exhausted==1);
 puts("PASS persistent backpressure: four 5/10/20/40ms retries then SDK fallback/cooldown, unsent data retained");
 fresh();token=begin_file(3);post_failure=true;failures_left=1;assert(__wrap_tcp_output(&pcb)==ERR_MEM);
 for(unsigned i=0;i<4;i++){fire();}assert(!timer.armed&&!posted);m=end_file(token);assert(m.post_failures==4&&!m.retries&&m.exhausted==1);
 puts("PASS full TCP/IP mailbox consumes bounded budget; no blocking timer task or unbounded retry loop");
 fresh();token=begin_file(4);failures_left=1;assert(__wrap_tcp_output(&pcb)==ERR_MEM);fire();assert(posted);calls=output_calls;
 nuwax_sd_tcp_recovery_close(0x1234,3);assert(s_scope.open);nuwax_sd_tcp_recovery_close(0x1234,4);assert(!s_scope.open);
 uint32_t newer=begin_file(5);failures_left=1;assert(__wrap_tcp_output(&pcb)==ERR_MEM);calls=output_calls;
 assert(s_ticket.token==token&&s_round.token==newer);run_posted();assert(output_calls==calls);
 fire();run_posted();assert(delivered==8064);end_file(token);assert(s_scope.open);m=end_file(newer);assert(m.retries==1&&m.recovered==1);
 puts("PASS closed epoch and reused endpoint: old queued ticket cannot retry or clear new file state");
 fresh();token=begin_file(6);failures_left=1;assert(__wrap_tcp_output(&pcb)==ERR_MEM);fire();tcp_active_pcbs=NULL;calls=output_calls;run_posted();assert(output_calls==calls);m=end_file(token);assert(m.stale==1);
 fresh();token=begin_file(7);output_result=ERR_RTE;assert(__wrap_tcp_output(&pcb)==ERR_RTE&&!timer.armed);end_file(token);
 fresh();token=begin_file(8);execute_inline=true;failures_left=1;assert(__wrap_tcp_output(&pcb)==ERR_MEM);fire();m=end_file(token);assert(m.recovered==1&&!s_queued&&delivered==8064);
 puts("PASS freed/missing PCB, route failure and callback completing before post returns");
 for(unsigned i=0;i<2000;i++){
   fresh();token=begin_file(i+9);failures_left=1;assert(__wrap_tcp_output(&pcb)==ERR_MEM);fire();calls=output_calls;
   end_file(token);run_posted();assert(output_calls==calls&&!s_queued&&!s_scope.open);
 }
 puts("PASS 2000 file-end/queued-callback interleavings; no retained PCB/client/socket pointers");
 return 0;
}
''')

for formal, test in [(0,0),(0,1),(1,1)]:
    code = f'''#include <assert.h>
#define NUWAX_SD_TCP_RECOVERY_HOST_TEST 1
#define CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE {formal}
#define CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE {test}
#include "sd_tcp_recovery.c"
int main(void){{
 nuwax_sd_tcp_recovery_prepare();assert(!nuwax_sd_tcp_recovery_begin(1,1,7));
 nuwax_sd_tcp_recovery_metrics_t metrics={{.outputs=10}};
 nuwax_sd_tcp_recovery_close(1,1);nuwax_sd_tcp_recovery_end(0,&metrics);assert(!metrics.outputs);
}}
'''
    run(f'isolated-config-{formal}-{test}', code)
print('PASS disabled and test-gateway variants compile without formal TCP wrapper dependencies')

transport_source = (root/'main/ws_transport.c').read_text(encoding='utf-8-sig')
transport_boundaries = '\n'.join(block(transport_source, marker) for marker in [
    'static esp_err_t transport_close(nuwax_ws_client_t *client)',
    'static void transport_interrupt_if_epoch(nuwax_ws_client_t *client,',
    'uint32_t nuwax_ws_archive_recovery_begin(nuwax_ws_client_t *client,'])
run('transport-lifecycle', r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#define ESP_PLATFORM 1
#define CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE 1
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_TIMEOUT 2
#define ESP_ERR_NOT_FINISHED 3
#define NUWAX_WS_HANDLE_LOCK_TIMEOUT_MS 20U
#define NUWAX_WS_INTERRUPT_LOCK_TIMEOUT_MS 20U
#define pdMS_TO_TICKS(ms) (ms)
#define pdTRUE 1
#define SHUT_RDWR 2
#define ESP_LOGE(...) ((void)0)
typedef int esp_err_t;
typedef void *SemaphoreHandle_t;
typedef struct {void *io_lock,*tls;int sock;bool use_tls,open,closing;uint32_t epoch;} nuwax_ws_client_t;
static nuwax_ws_client_t client;
static bool locked,lock_failed,scope_open;
static unsigned prepares,registrations,shutdowns,destroys;
static uint32_t scope_epoch,last_close_epoch;
static uint32_t nuwax_ws_connection_epoch(const nuwax_ws_client_t *c){return c->epoch;}
static bool transport_owns_any_handle(nuwax_ws_client_t *c){return c->tls||c->sock>=0;}
static bool transport_is_open(nuwax_ws_client_t *c){return c->open;}
static bool close_requested_for_epoch(nuwax_ws_client_t *c,uint32_t e){return c->closing&&c->epoch==e;}
static int64_t esp_timer_get_time(void){return 1000;}
static int xSemaphoreTake(void *lock,unsigned ms){(void)ms;assert(lock&&!locked);if(lock_failed)return 0;locked=true;return 1;}
static int xSemaphoreGive(void *lock){assert(lock&&locked);locked=false;return 1;}
static int take_io_lock_with_timeout(nuwax_ws_client_t *c,unsigned ms){return xSemaphoreTake(c->io_lock,ms)?ESP_OK:ESP_ERR_TIMEOUT;}
static bool transport_io_lock_until(nuwax_ws_client_t *c,int64_t at){assert(at==51000);return c->io_lock&&xSemaphoreTake(c->io_lock,50);}
static void nuwax_sd_tcp_recovery_prepare(void){assert(!locked);prepares++;}
static uint32_t nuwax_sd_tcp_recovery_begin(uintptr_t owner,uint32_t epoch,int sock){
 assert(locked&&owner==(uintptr_t)&client&&epoch==client.epoch&&sock==client.sock);
 scope_open=true;scope_epoch=epoch;registrations++;return 5;
}
static void nuwax_sd_tcp_recovery_close(uintptr_t owner,uint32_t epoch){
 assert(locked&&owner==(uintptr_t)&client);last_close_epoch=epoch;
 if(!epoch||epoch==scope_epoch)scope_open=false;
}
static void esp_tls_conn_destroy(void *tls){assert(tls&&locked&&!scope_open);destroys++;}
static int shutdown(int sock,int how){assert(sock==7&&how==SHUT_RDWR&&locked&&!scope_open);shutdowns++;return 0;}
static int close(int sock){assert(sock==7&&locked&&!scope_open);return 0;}
''' + transport_boundaries + r'''
int main(void){
 client=(nuwax_ws_client_t){.io_lock=(void*)1,.tls=(void*)2,.sock=7,.use_tls=true,.open=true,.epoch=8};
 assert(!nuwax_ws_archive_recovery_begin(NULL,8));
 assert(!nuwax_ws_archive_recovery_begin(&client,0));
 assert(!nuwax_ws_archive_recovery_begin(&client,7)&&!locked&&!registrations);
 client.closing=true;assert(!nuwax_ws_archive_recovery_begin(&client,8));client.closing=false;
 client.use_tls=false;assert(!nuwax_ws_archive_recovery_begin(&client,8));client.use_tls=true;
 client.open=false;assert(!nuwax_ws_archive_recovery_begin(&client,8));client.open=true;
 lock_failed=true;assert(!nuwax_ws_archive_recovery_begin(&client,8)&&!locked);lock_failed=false;
 assert(nuwax_ws_archive_recovery_begin(&client,8)==5&&scope_open&&!locked&&registrations==1);
 transport_interrupt_if_epoch(&client,7);assert(scope_open&&!shutdowns&&!locked);
 lock_failed=true;transport_interrupt_if_epoch(&client,8);assert(scope_open&&!shutdowns&&!locked);lock_failed=false;
 transport_interrupt_if_epoch(&client,8);assert(!scope_open&&shutdowns==1&&last_close_epoch==8&&!locked);
 assert(nuwax_ws_archive_recovery_begin(&client,8)==5&&scope_open);
 /* Connection manager already bumped epoch. Owner close must still cancel
  * the scope associated with the actual old handles before destroying TLS. */
 client.epoch=9;assert(transport_close(&client)==ESP_OK&&!scope_open&&last_close_epoch==0);
 assert(!locked&&!client.tls&&client.sock==-1&&!client.use_tls&&destroys==1);
 puts("PASS production WS lifecycle: scope admitted under I/O lock only on checked TLS epoch; stale interrupt cannot cancel; owner close invalidates old epoch before handle destruction");
 return 0;
}
''')

transport = (root/'main/ws_transport.c').read_text(encoding='utf-8-sig')
spool = (root/'main/offline_audio_spool.c').read_text(encoding='utf-8-sig')
cmake = (root/'tools/cmake/sd-tcp-writable.cmake').read_text()
shared = (root/'main/offline_replay_shared.inc').read_text()
assert 'tls_probe_ms *= 2U' in transport and 'tls_probe_ms < 16U' in transport
assert 'if (bio_progress || ret > 0) tls_probe_ms = 2U;' in transport
assert 'nuwax_sd_socket_wait_begin(trace->metrics)' in transport
assert 'if (precise) nuwax_sd_socket_wait_end();' in transport
assert 'const bool want_read = ret == ESP_TLS_ERR_SSL_WANT_READ;' in transport
assert 'const size_t tls_limit = tls_archive ? trace->metrics->tls_plaintext_limit : 0U;' in transport
assert transport.count('nuwax_sd_tcp_recovery_close((uintptr_t)client,') == 2
assert 'nuwax_sd_socket_wait_prepare();' in shared
assert '--wrap=sys_arch_sem_wait' in cmake and '--wrap=tcp_output' in cmake
wrapped = block(spool, 'static esp_err_t replay_one_ready(nuwax_ws_client_t *ws, uint32_t epoch,')
assert wrapped.index('nuwax_ws_archive_recovery_begin') < wrapped.index('replay_one_ready_file(') < wrapped.index('nuwax_sd_tcp_recovery_end')
tcp_out = (sdk/'components/lwip/lwip/src/core/tcp_out.c').read_text()
assert 'tcp_set_flags(pcb, TF_NAGLEMEMERR);' in tcp_out
assert 'if (err != ERR_OK)' in tcp_out and 'return err;' in tcp_out
report = dict(installedSdkSemWaitSha256=hashlib.sha256(sem_file.read_bytes()).hexdigest(),
    installedSdkSelectSha256=hashlib.sha256(socket_file.read_bytes()).hexdigest(),
    scopedWaitMilliseconds=2, sdkTickRoundedMilliseconds=20, callbackThreadRaces=400,
    tcpRetireInterleavings=2000, actualTransportLifecycleTested=True, isolatedBuildVariants=3,
    hardwareValidated=False)
(out/'results.json').write_text(json.dumps(report,indent=2)+'\n')
print('PASS R204 production modules, installed SDK wait/cleanup, epoch fences and TLS span/direction contracts')
