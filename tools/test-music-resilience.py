"""Run production status outbox and SDK read loop under transport failures.

Network/RTOS boundaries are simulated; no device commands are sent.
"""
import argparse
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build-host-checks/music-resilience'
OUT.mkdir(parents=True, exist_ok=True)
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--idf', type=Path, default=Path('C:/esp/v6.0/esp-idf'))
args = p.parse_args()
music = (ROOT/'main/music_playback/music_playback.c').read_text(encoding='utf-8-sig')
control = (ROOT/'main/music_playback/music_test_control.c').read_text(encoding='utf-8-sig')
status = (ROOT/'main/music_playback/music_status.c').read_text(encoding='utf-8-sig')

def function(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

def check(name, source, negative=False):
    c, exe = OUT/f'{name}.c', OUT/f'{name}.exe'
    c.write_text(source, encoding='utf-8')
    build = subprocess.run(['gcc', '-std=c11', '-O1', '-Wall', '-Wextra',
        '-Wno-unused-function', '-Wno-unused-variable', '-Wno-misleading-indentation',
        str(c), '-o', str(exe)], capture_output=True, text=True, timeout=60)
    assert build.returncode == 0, build.stdout + build.stderr
    run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=15)
    if negative:
        assert run.returncode != 0, f'{name}: regression escaped'
        print(f'PASS: {name} detected at runtime')
    else:
        assert run.returncode == 0, run.stdout + run.stderr
        print(run.stdout.strip())

COMMON = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM -2
#define ESP_ERR_TIMEOUT -3
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
'''
STATUS_SHIM = r'''
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
#define pdPASS 1
#define pdMS_TO_TICKS(v) (v)
typedef int portMUX_TYPE;
typedef void *TaskHandle_t;
typedef struct { bool connected; uint32_t epoch; } nuwax_ws_client_t;
static int64_t now_us;
static int sends, send_result, during_send;
static bool alloc_fail, task_fail;
static char sent_state[16], sent_id[80];
void nuwax_music_status_service(nuwax_ws_client_t *ws);
void nuwax_music_status_post(nuwax_ws_client_t*,uint32_t,const char*,const char*,const char*);
static size_t strlcpy(char*d,const char*s,size_t n){size_t l=strlen(s);if(n){size_t k=l<n-1?l:n-1;memcpy(d,s,k);d[k]=0;}return l;}
static int64_t esp_timer_get_time(void){return now_us;}
static void *heap_caps_calloc(size_t n,size_t s,int caps){(void)caps;return alloc_fail?NULL:calloc(n,s);}
static void heap_caps_free(void*p){free(p);}
static int xTaskCreateWithCaps(void(*fn)(void*),const char*n,unsigned sz,void*a,unsigned pri,TaskHandle_t*t,int caps){
 (void)fn;(void)n;(void)sz;(void)a;(void)pri;(void)caps;*t=task_fail?NULL:(void*)1;return !task_fail;}
static void vTaskDelay(unsigned ms){(void)ms;}
static uint32_t nuwax_ws_connection_epoch(nuwax_ws_client_t*w){return w->epoch;}
static bool nuwax_ws_is_connected(nuwax_ws_client_t*w){return w->connected;}
static int nuwax_session_send_music_playback_status_until_if_epoch(nuwax_ws_client_t*w,const char*id,const char*state,const char*reason,int64_t deadline,uint32_t epoch){
 (void)reason;assert(deadline-now_us==200000 && w->epoch==epoch);sends++;
 strlcpy(sent_id,id,sizeof(sent_id));strlcpy(sent_state,state,sizeof(sent_state));
 /* Simulate the producer publishing a terminal state while playing is in flight. */
 if(during_send){during_send=0;nuwax_music_status_post(w,epoch,id,"cancelled","replaced");}
 return send_result;
}
'''
STATUS_TEST = r'''
int main(void){
 nuwax_ws_client_t ws={true,1};
 alloc_fail=true;assert(nuwax_music_status_init()==ESP_ERR_NO_MEM && !s_entries);
 alloc_fail=false;task_fail=true;assert(nuwax_music_status_init()==ESP_ERR_NO_MEM && !s_entries);
 task_fail=false;assert(nuwax_music_status_init()==ESP_OK);
 assert(nuwax_music_status_reserve(&ws,1,"song"));
 nuwax_music_status_post(&ws,1,"song","playing",NULL);
 assert(sends==0);send_result=ESP_FAIL;nuwax_music_status_service(&ws);assert(sends==1);
 now_us=499999;nuwax_music_status_service(&ws);assert(sends==1);
 now_us=500000;send_result=ESP_OK;nuwax_music_status_service(&ws);assert(sends==2);
 assert(find_entry(&ws,1,"song") && !find_entry(&ws,1,"song")->ready);
 nuwax_music_status_post(&ws,1,"song","completed",NULL);
 nuwax_music_status_service(&ws);assert(!find_entry(&ws,1,"song") && !strcmp(sent_state,"completed"));
 /* New terminal status must survive an older in-flight send's completion. */
 assert(nuwax_music_status_reserve(&ws,1,"race"));
 nuwax_music_status_post(&ws,1,"race","playing",NULL);during_send=1;
 nuwax_music_status_service(&ws);assert(find_entry(&ws,1,"race")->ready);
 nuwax_music_status_service(&ws);assert(!strcmp(sent_state,"cancelled") && !find_entry(&ws,1,"race"));
 /* Rapid ending coalesces unsent playing into the correct terminal state. */
 assert(nuwax_music_status_reserve(&ws,1,"short"));
 nuwax_music_status_post(&ws,1,"short","playing",NULL);
 nuwax_music_status_post(&ws,1,"short","failed","decode");
 nuwax_music_status_service(&ws);assert(!strcmp(sent_state,"failed"));
 assert(nuwax_music_status_reserve(&ws,1,"stale"));
 nuwax_music_status_post(&ws,1,"stale","playing",NULL);
 int before=sends;ws.epoch=2;nuwax_music_status_service(&ws);
 assert(sends==before && !find_entry(&ws,1,"stale"));
 /* Fixed capacity rejects admission rather than evicting someone else's final status. */
 for(unsigned i=0;i<MUSIC_STATUS_SLOTS;i++){char id[10];snprintf(id,sizeof(id),"q%u",i);assert(nuwax_music_status_reserve(&ws,2,id));}
 assert(!nuwax_music_status_reserve(&ws,2,"overflow"));
 ws.connected=false;nuwax_music_status_service(&ws);ws.connected=true;
 assert(nuwax_music_status_reserve(&ws,2,"timeout"));
 nuwax_music_status_post(&ws,2,"timeout","failed","download");
 send_result=ESP_FAIL;nuwax_music_status_service(&ws);before=sends;
 now_us+=15000000;nuwax_music_status_service(&ws);
 assert(sends==before && !find_entry(&ws,2,"timeout"));
 free(s_entries);
 puts("PASS: async status retries, revision race, coalescing, capacity, stale epoch and expiry");
}
'''
status_body = re.sub(r'^#include[^\n]*\n', '', status, flags=re.M)
check('status_outbox', COMMON+STATUS_SHIM+status_body+STATUS_TEST)
check('status_loses_inflight_terminal', COMMON+STATUS_SHIM+status_body.replace(
    'entry->used && entry->revision == send.revision', 'entry->used')+STATUS_TEST, True)

SDK_SHIM = r'''
#define ESP_LOG_WARN 2
#define ESP_LOG_DEBUG 1
#define ESP_LOG_LEVEL(...) ((void)0)
#define ERR_TCP_TRANSPORT_CONNECTION_CLOSED_BY_FIN -2
#define ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT -3
#define ESP_ERR_HTTP_EAGAIN 77
#define HTTP_EVENT_ERROR 1
#define HTTP_EVENT_ON_CONNECTED 2
#define HTTP_EVENT_ON_HEADER 3
#define HTTP_EVENT_ON_DATA 4
#define SHUT_RDWR 2
typedef int esp_log_level_t;
typedef struct {int raw_len;char *raw_data,*data,*output_ptr;} esp_http_buffer_t;
typedef struct {esp_http_buffer_t *buffer;bool is_chunked;int64_t data_process,content_length;} response_t;
typedef struct client {response_t*response;bool is_chunk_complete;int buffer_size_rx,timeout_ms;void*transport,*parser,*parser_settings;} *esp_http_client_handle_t;
typedef struct {int event_id;esp_http_client_handle_t client;} esp_http_client_event_t;
static uint64_t elapsed_ms;static unsigned calls;static bool cancellation_requested,socket_shutdown;
static bool cancelled(void){return cancellation_requested;}
static int esp_http_client_get_socket(esp_http_client_handle_t c){(void)c;return 42;}
static int shutdown(int fd,int how){assert(fd==42 && how==SHUT_RDWR);socket_shutdown=true;return 0;}
static void esp_http_client_cached_buf_cleanup(esp_http_buffer_t*b){(void)b;}
static int esp_transport_read(void*t,char*b,int n,int timeout){
 (void)t;assert(n>0 && timeout==200);calls++;if(socket_shutdown){errno=ECONNRESET;return -1;}
 elapsed_ms+=100;b[0]='x';if(elapsed_ms>=1000)cancellation_requested=true;return 1;
}
static esp_err_t download_event(esp_http_client_event_t *event);
static void http_parser_execute(void*p,void*s,char*b,int n){
 (void)s;esp_http_client_handle_t c=p;
 if(n>0){memcpy(c->response->buffer->output_ptr,b,n);c->response->buffer->raw_len=n;c->response->data_process+=n;
 esp_http_client_event_t event={HTTP_EVENT_ON_DATA,c};download_event(&event);}
}
static int esp_transport_translate_error(int e){return e;}
static void*esp_transport_get_error_handle(void*t){return t;}
static bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c){return c->response->data_process==c->response->content_length;}
static void http_dispatch_event(esp_http_client_handle_t c,int e,void*p,int n){(void)c;(void)e;(void)p;(void)n;}
static void http_dispatch_event_to_event_loop(int e,void*p,int n){(void)e;(void)p;(void)n;}
'''
SDK_TEST = r'''
int main(void){char scratch[4096],output[4096];esp_http_buffer_t b={.data=scratch};
 response_t r={.buffer=&b,.content_length=8192};struct client c={.response=&r,.buffer_size_rx=4096,.timeout_ms=200};c.parser=&c;
 esp_http_client_event_t e={HTTP_EVENT_ON_DATA,&c};download_event(&e);assert(!socket_shutdown);
 int n=esp_http_client_read(&c,output,sizeof(output));
 assert(n<4096 && elapsed_ms==1000 && calls<=11 && socket_shutdown);
 printf("PASS: actual SDK slow read exits on cancel after %llu ms (%u reads), not 409600 ms\n",(unsigned long long)elapsed_ms,calls);
}
'''
sdk=(args.idf/'components/esp_http_client/esp_http_client.c').read_text(encoding='utf-8-sig')
read_body=function(sdk,'int esp_http_client_read(')
event=function(music,'static esp_err_t download_event(')
check('sdk_cancel_trickle',COMMON+SDK_SHIM+event+read_body+SDK_TEST)
check('sdk_cancel_without_shutdown',COMMON+SDK_SHIM+event.replace(
    '(void)shutdown(fd, SHUT_RDWR)', '(void)fd')+read_body+SDK_TEST,True)

backoff=function(control,'static uint32_t control_retry_delay(')
check('poll_backoff',COMMON+backoff+r'''
int main(void){unsigned failures=0;unsigned expect[]={1000,2000,4000,5000,5000};
 for(unsigned i=0;i<5;i++)assert(control_retry_delay(false,&failures)==expect[i]);
 assert(control_retry_delay(true,&failures)==500 && failures==0);
 assert(control_retry_delay(false,&failures)==1000);
 puts("PASS: gateway failures back off; recovery restores responsive polling");}
''')
emotion=(ROOT/'main/nuwax_ui_v28/face/emotion_animation_player.c').read_text(encoding='utf-8-sig')
check('emotion_transition_gate',COMMON+r'''
#include <stdatomic.h>
static atomic_uint s_queued_asset_id;
static bool s_ready;
static struct {bool active;} s_component_bridge;
static unsigned s_asset_id=4;
'''+function(emotion,'bool nuwax_emotion_animation_player_transition_active(')+r'''
int main(void){
 assert(!nuwax_emotion_animation_player_transition_active());s_ready=true;
 assert(!nuwax_emotion_animation_player_transition_active());
 s_component_bridge.active=true;assert(nuwax_emotion_animation_player_transition_active());
 s_component_bridge.active=false;s_queued_asset_id=5;assert(nuwax_emotion_animation_player_transition_active());
 s_queued_asset_id=4;assert(!nuwax_emotion_animation_player_transition_active());
 s_queued_asset_id=0;assert(!nuwax_emotion_animation_player_transition_active());
 puts("PASS: background UI cache pauses only for active or queued emotion transitions");}
''')
