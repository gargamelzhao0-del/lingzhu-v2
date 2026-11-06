"""Fault-inject production music transport, codec guard and control task bodies.

Negative mutants must fail: these checks cover failures, not only success paths.
No hardware timing or analog audio quality is inferred from these host tests.
"""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build-host-checks/music-failures'
OUT.mkdir(parents=True, exist_ok=True)
board = (ROOT / 'main/board/vocat_audio_board.c').read_text(encoding='utf-8-sig')
music = (ROOT / 'main/music_playback/music_playback.c').read_text(encoding='utf-8-sig')
control = (ROOT / 'main/music_playback/music_test_control.c').read_text(encoding='utf-8-sig')

def function(source, signature):
    start = source.index(signature)
    while ';' in source[start:source.index('{', start)]:
        start = source.index(signature, start + len(signature))
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

COMMON = r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM -2
#define ESP_ERR_INVALID_STATE -3
#define ESP_ERR_INVALID_RESPONSE -4
#define ESP_ERR_INVALID_SIZE -5
#define ESP_ERR_TIMEOUT -6
#define ESP_ERR_HTTP_EAGAIN 77
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
#define pdMS_TO_TICKS(ms) (ms)
#define pdTRUE 1
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
typedef int esp_err_t;
typedef int portMUX_TYPE;
static size_t strlcpy(char *d,const char *s,size_t n){size_t z=strlen(s);if(n){size_t c=z<n-1?z:n-1;memcpy(d,s,c);d[c]=0;}return z;}
'''

CODEC = r'''
#define ESP_CODEC_DEV_OK 0
#define ESP_CODEC_DEV_INVALID_ARG -10
#define ESP_CODEC_DEV_DRV_ERR -11
#define VOCAT_CODEC_I2C_GUARD_TIMEOUT_MS 100
typedef struct ctrl audio_codec_ctrl_if_t;
struct ctrl {int (*read_reg)(const audio_codec_ctrl_if_t*,int,int,void*,int);int (*write_reg)(const audio_codec_ctrl_if_t*,int,int,void*,int);};
typedef struct {audio_codec_ctrl_if_t base;const audio_codec_ctrl_if_t *delegate;atomic_uint io_failures;} vocat_codec_ctrl_guard_t;
static vocat_codec_ctrl_guard_t s_es8311_ctrl_guard;
typedef struct {unsigned sample_rate;} esp_codec_dev_sample_info_t;
typedef struct codec audio_codec_if_t;
struct codec {int (*set_fs)(const audio_codec_if_t*,esp_codec_dev_sample_info_t*);};
static const audio_codec_if_t *s_es8311_if;
static int fail_operation, operations, bus_locks, bus_ends;
static int nuwax_i2c_bus_transaction_begin(unsigned ms){(void)ms;bus_locks++;return fail_operation==3?ESP_FAIL:ESP_OK;}
static void nuwax_i2c_bus_transaction_end(void){bus_ends++;}
static int read_reg(const audio_codec_ctrl_if_t *c,int r,int n,void *p,int s){(void)c;(void)r;(void)n;memset(p,0,s);operations++;return fail_operation==1?ESP_FAIL:ESP_OK;}
static int write_reg(const audio_codec_ctrl_if_t *c,int r,int n,void *p,int s){(void)c;(void)r;(void)n;(void)p;(void)s;operations++;return fail_operation==2?ESP_FAIL:ESP_OK;}
'''
codec_bodies = '\n'.join(function(board, sig) for sig in [
    'static int codec_ctrl_guard_result(', 'static int codec_ctrl_guard_read(',
    'static int codec_ctrl_guard_write(', 'static esp_err_t codec_dev_ret_to_err(',
    'static esp_err_t set_es8311_sample_info('])
CODEC_TEST = r'''
/* Mirrors the vendor set_fs returning OK after a failed read or write. */
static int swallowing_set_fs(const audio_codec_if_t *c,esp_codec_dev_sample_info_t *fs){
 (void)c;(void)fs;int value=0;const audio_codec_ctrl_if_t *ctrl=&s_es8311_ctrl_guard.base;
 (void)codec_ctrl_guard_read(ctrl,2,1,&value,1);
 (void)codec_ctrl_guard_write(ctrl,2,1,&value,1);
 return ESP_CODEC_DEV_OK;
}
int main(void){
 const audio_codec_ctrl_if_t delegate={read_reg,write_reg};
 const audio_codec_if_t codec={swallowing_set_fs};
 s_es8311_if=&codec;s_es8311_ctrl_guard.delegate=&delegate;
 esp_codec_dev_sample_info_t fs={16000};
 assert(set_es8311_sample_info(&fs,"voice restore")==ESP_OK);
 for(int fault=1;fault<=3;fault++){
   fail_operation=fault;
   assert(set_es8311_sample_info(&fs,"voice restore")!=ESP_OK);
   fail_operation=0;
   assert(set_es8311_sample_info(&fs,"voice restore")==ESP_OK);
 }
 assert(bus_locks-bus_ends==2);
 puts("PASS: swallowed codec read/write and bus-lock failures remain visible; recovery succeeds");
}
'''

HTTP = r'''
#define HTTP_METHOD_GET 0
#define HTTP_METHOD_POST 1
#define CONFIG_NUWAX_MUSIC_MAX_BYTES 32000000
#define CONFIG_NUWAX_MUSIC_HTTP_TIMEOUT_MS 10000
typedef void *esp_http_client_handle_t;
typedef struct {const char *url;int method,timeout_ms;bool disable_auto_redirect;int buffer_size,buffer_size_tx;void *crt_bundle_attach;int (*event_handler)(void*);} esp_http_client_config_t;
static int download_event(void *e){(void)e;return ESP_OK;}
static void *esp_crt_bundle_attach;
static int64_t header_result, now_us;
static int http_status=200, reads, cleanups, mode;
static bool chunked, complete, stop_flag;
static bool cancelled(void){return stop_flag;}
static int64_t esp_timer_get_time(void){return now_us;}
static void *heap_caps_malloc(size_t n,int c){(void)c;return malloc(n);}
static void heap_caps_free(void *p){free(p);}
static void vTaskDeleteWithCaps(void *p){(void)p;}
static void *esp_http_client_init(const esp_http_client_config_t *c){(void)c;return (void*)1;}
static int esp_http_client_open(void *c,int n){(void)c;(void)n;return ESP_OK;}
static int64_t esp_http_client_fetch_headers(void *c){(void)c;return header_result;}
static bool esp_http_client_is_chunked_response(void *c){(void)c;return chunked;}
static int esp_http_client_get_status_code(void *c){(void)c;return http_status;}
static void esp_http_client_set_timeout_ms(void *c,int n){(void)c;(void)n;}
static bool esp_http_client_is_complete_data_received(void *c){(void)c;return complete;}
static void esp_http_client_cleanup(void *c){(void)c;cleanups++;}
static int esp_http_client_read(void *c,char *p,int n){
 (void)c;(void)p;(void)n;reads++;
 if(mode==1){now_us+=200000;return -ESP_ERR_HTTP_EAGAIN;}
 if(mode==2){stop_flag=true;return -ESP_ERR_HTTP_EAGAIN;}
 return reads==1?4:0;
}
typedef struct {char url[64];void *compressed;atomic_bool producer_done;esp_err_t producer_result;atomic_uint received;} music_job_t;
static size_t xStreamBufferSend(void *s,const void *p,size_t n,unsigned ticks){(void)s;(void)p;(void)ticks;return n;}
'''
HTTP_TEST = r'''
static void run(int64_t length,bool is_chunked,bool is_complete,int expected){
 music_job_t job={0};header_result=length;chunked=is_chunked;complete=is_complete;
 reads=cleanups=0;now_us=0;stop_flag=false;download_task(&job);
 assert(atomic_load(&job.producer_done)&&cleanups==1);
 assert(job.producer_result==expected);
 if(length<0)assert(reads==0);
}
int main(void){
 run(-ESP_ERR_HTTP_EAGAIN,false,false,ESP_ERR_INVALID_RESPONSE);
 run(ESP_FAIL,false,false,ESP_ERR_INVALID_RESPONSE);
 run(4,false,true,ESP_OK);run(0,true,true,ESP_OK);
 run(8,false,false,ESP_ERR_INVALID_SIZE);
 mode=1;run(4,false,false,ESP_ERR_TIMEOUT);
 mode=2;run(4,false,false,ESP_ERR_INVALID_STATE);
 puts("PASS: incomplete headers rejected before body, known/chunked length, truncation, timeout, cancel");
}
'''

CONTROL = r'''
#include "cJSON.h"
#define CONFIG_NUWAX_MUSIC_CONTROL_TEST_URL "http://127.0.0.1:8798"
#define HTTP_METHOD_POST 1
#include "music_test_control.h"
typedef struct {char request_id[80],state[16],reason[48];unsigned sample_rate;} nuwax_music_snapshot_t;
typedef int QueueHandle_t;
static nuwax_music_test_command_t queued;
static bool has_queued;
static int cancellations;
static jmp_buf poll_end;
static int xQueueReceive(int q,void *p,unsigned t){(void)q;(void)t;if(!has_queued)return 0;memcpy(p,&queued,sizeof(queued));has_queued=false;return pdTRUE;}
static void xQueueOverwrite(int q,const void *p){(void)q;memcpy(&queued,p,sizeof(queued));has_queued=true;}
static void nuwax_music_playback_cancel(const char *r){(void)r;cancellations++;}
static void nuwax_music_playback_snapshot(nuwax_music_snapshot_t *p){memset(p,0,sizeof(*p));strcpy(p->request_id,"new_music");strcpy(p->state,"playing");}
static bool nuwax_wifi_is_connected(void){return true;}
static void esp_efuse_mac_get_default(uint8_t *p){memset(p,0,6);}
static unsigned long esp_random(void){return 1;}
static void vTaskDelay(unsigned ms){(void)ms;longjmp(poll_end,1);}
typedef void *esp_http_client_handle_t;
typedef struct {const char *url;int method,timeout_ms,buffer_size,buffer_size_tx;bool disable_auto_redirect;} esp_http_client_config_t;
static void *esp_http_client_init(const esp_http_client_config_t *c){(void)c;return (void*)1;}
static void esp_http_client_set_header(void *c,const char *k,const char *v){(void)c;(void)k;(void)v;}
static int esp_http_client_open(void *c,int n){(void)c;(void)n;return ESP_OK;}
static int esp_http_client_write(void *c,const char *p,int n){(void)c;(void)p;return n;}
static const char *response="{\"request_id\":\"old_stop\",\"command\":\"stop_music\"}";
static bool advance_ack;
static int64_t esp_http_client_fetch_headers(void *c){
 (void)c;
 /* Owner consumes the prior poll's STOP while this request is in flight. */
 if(advance_ack)nuwax_music_test_control_result("old_stop",true,"accepted");
 return strlen(response);
}
static int esp_http_client_get_status_code(void *c){(void)c;return 200;}
static int esp_http_client_read_response(void *c,char *p,int n){(void)c;memcpy(p,response,n);return n;}
static void esp_http_client_cleanup(void *c){(void)c;}
'''
control_bodies = control[control.index('static QueueHandle_t s_commands;'):control.index('esp_err_t nuwax_music_test_control_init(')]
CONTROL_TEST = r'''
static void poll(void){if(!setjmp(poll_end))control_task(NULL);}
int main(void){
 s_commands=1;advance_ack=true;poll();
 assert(cancellations==0);
 nuwax_music_test_command_t cmd;assert(!nuwax_music_test_control_take(&cmd));
 response="{\"request_id\":\"new_stop\",\"command\":\"stop_music\"}";
 advance_ack=false;poll();assert(nuwax_music_test_control_take(&cmd)&&cmd.stop);
 nuwax_music_test_control_result(cmd.request_id,true,"accepted");
 assert(!nuwax_music_test_control_take(&cmd));
 puts("PASS: delayed duplicate STOP cannot cancel new music; fresh STOP reaches owner once");
}
'''

START = r'''
#define NUWAX_SESSION_MUSIC_URL_MAX_LEN 1024
#define MUSIC_BUFFER_BYTES 98304
#define MUSIC_FIRST_AUDIO_TIMEOUT_US 10000000LL
static int64_t esp_timer_get_time(void){return 1000;}
#define pdPASS 1
typedef void *TaskHandle_t;
typedef struct {unsigned epoch;} nuwax_ws_client_t;
typedef struct {char request_id[80],state[16],reason[48];unsigned sample_rate;} nuwax_music_snapshot_t;
static nuwax_music_snapshot_t s_snapshot;
static atomic_bool s_active,s_audio_active,s_stop,s_output_quiet;
static int s_lock;
typedef struct {void *ws;unsigned epoch;bool pcm,local;char id[80],url[1024];void *storage,*compressed;int stream_control;atomic_bool producer_done;atomic_uint received;int64_t first_audio_deadline_us;} music_job_t;
static int allocation_step,fail_step,allocations;
static void *allocate(size_t n){if(++allocation_step==fail_step)return NULL;void *p=calloc(1,n);if(p)allocations++;return p;}
static void *heap_caps_malloc(size_t n,int caps){(void)caps;return allocate(n);}
static void *heap_caps_calloc(size_t n,size_t size,int caps){(void)caps;return allocate(n*size);}
static void heap_caps_free(void *p){if(p){allocations--;free(p);}}
static void *xStreamBufferCreateStatic(size_t n,unsigned t,void *s,void *c){(void)n;(void)t;(void)s;(void)c;return ++allocation_step==fail_step?NULL:(void*)1;}
static void vStreamBufferDelete(void *s){assert(s==(void*)1);}
static unsigned nuwax_ws_connection_epoch(nuwax_ws_client_t *ws){return ws->epoch;}
static void music_task(void *p){(void)p;}
static int xTaskCreateWithCaps(void (*f)(void*),const char *n,unsigned s,void *j,unsigned pr,TaskHandle_t *t,int c){(void)f;(void)n;(void)s;(void)j;(void)pr;(void)t;(void)c;assert(++allocation_step==fail_step);return 0;}
'''
START_TEST = r'''
int main(void){
 nuwax_ws_client_t ws={1};
 for(fail_step=1;fail_step<=4;fail_step++){
   allocation_step=0;s_output_quiet=false;
   strcpy(s_snapshot.state,"buffering");strcpy(s_snapshot.request_id,"old");
   assert(!start(&ws,"new","https://example.test/song.mp3","mp3",false));
   assert(!s_active&&!s_audio_active&&s_output_quiet&&allocations==0);
   assert(!strcmp(s_snapshot.request_id,"new")&&!strcmp(s_snapshot.state,"failed"));
 }
 puts("PASS: allocation/task failures release ownership and report the failed request");
}
'''

SHELL = r'''
#define NUWAX_APP_SHELL_ACTION_HEAD_TOUCH_AGENT 1
#define NUWAX_APP_SHELL_ACTION_SHAKE_AGENT 2
#define NUWAX_APP_SHELL_ACTION_CANCEL 3
#define NUWAX_APP_SHELL_ACTION_START_LISTEN 4
typedef int nuwax_ws_client_t;
typedef struct {int action;} nuwax_app_shell_result_t;
static bool owned=true,head_claimed=true,shake_claimed=true;
static int cancels,dispatches,s_shell_action_dispatch_callbacks;
static bool product_music_blocks_audio(void){return owned;}
static void product_music_cancel(const char *r){(void)r;cancels++;}
static void nuwax_shake_detector_release_shake(void){shake_claimed=false;}
static void nuwax_shake_detector_release_head_touch(void){head_claimed=false;}
static void nuwax_shell_action_dispatch(nuwax_ws_client_t *w,const nuwax_app_shell_result_t *a,void *c){(void)w;(void)a;(void)c;dispatches++;}
static void service_pending_volume_report(nuwax_ws_client_t *w){(void)w;}
'''
SHELL_TEST = r'''
int main(void){
 nuwax_app_shell_result_t action={NUWAX_APP_SHELL_ACTION_HEAD_TOUCH_AGENT};
 handle_shell_action(NULL,&action);assert(!head_claimed&&cancels==1&&dispatches==0);
 action.action=NUWAX_APP_SHELL_ACTION_SHAKE_AGENT;
 handle_shell_action(NULL,&action);assert(!shake_claimed&&cancels==2&&dispatches==0);
 owned=false;handle_shell_action(NULL,&action);assert(dispatches==1);
 puts("PASS: queued touch/shake during music releases interaction gates without dialogue");
}
'''

# The production control header needs only esp_err_t from this SDK header.
(OUT / 'esp_err.h').write_text('typedef int esp_err_t;\n', encoding='utf-8')
cjson = ROOT / 'managed_components/espressif__cjson/cJSON'

def check(name, body, negative=False, json=False):
    src, exe = OUT / f'{name}.c', OUT / f'{name}.exe'
    src.write_text(COMMON + body, encoding='utf-8')
    command = ['gcc', '-std=c11', '-O1', '-Wall', '-Wno-unused-function', '-Wno-unused-variable',
               '-I'+str(OUT), '-I'+str(ROOT / 'main/music_playback'), '-I'+str(cjson), str(src)]
    if json: command += [str(cjson / 'cJSON.c')]
    subprocess.run(command + ['-o', str(exe)], check=True, capture_output=True)
    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
    if negative:
        assert result.returncode != 0, f'{name}: negative mutant unexpectedly passed'
        print(f'PASS: {name} caught at runtime')
    else:
        assert result.returncode == 0, result.stdout + result.stderr
        print(result.stdout.strip())

check('codec', CODEC + codec_bodies + CODEC_TEST)
check('codec_without_failure_latch', CODEC + codec_bodies.replace(
    'rc = ESP_CODEC_DEV_DRV_ERR;', 'rc = ESP_CODEC_DEV_OK;') + CODEC_TEST, negative=True)
download = function(music, 'static void download_task(')
check('download', HTTP + download + HTTP_TEST)
check('download_accepts_header_error', HTTP + download.replace('length < 0 || ', '') + HTTP_TEST, negative=True)
check('control', CONTROL + control_bodies + CONTROL_TEST, json=True)
check('control_early_duplicate_stop', CONTROL + control_bodies.replace(
    'xQueueOverwrite(s_commands, &next);', 'if (stop) nuwax_music_playback_cancel("gateway_stop"); xQueueOverwrite(s_commands, &next);') + CONTROL_TEST,
    negative=True, json=True)
check('start_failure', START + function(music, 'static bool start(') + START_TEST)
main = (ROOT / 'main/nuwax_ai_partner_main.c').read_text(encoding='utf-8-sig')
shell = function(main, 'static void handle_shell_action(')
check('shell_handoff', SHELL + shell + SHELL_TEST)
check('shell_leaks_head_claim', SHELL + shell.replace(
    'nuwax_shake_detector_release_head_touch();', '(void)0;') + SHELL_TEST, negative=True)
