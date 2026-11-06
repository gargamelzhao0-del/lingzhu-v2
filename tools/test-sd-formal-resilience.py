"""Execute production socket readiness and checkpoint/probe policy; no device I/O."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]


def function(source, name):
    start = source.index(name)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


common = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <errno.h>
#include <time.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL 1
#define ESP_ERR_TIMEOUT 2
#define ESP_ERR_INVALID_STATE 3
#define ESP_LOGW(...) ((void)0)
'''
ws = (root/'main/ws_transport.c').read_text(encoding='utf-8')
readiness = common + r'''
#include "ws_send_failure_policy.h"
typedef struct { unsigned bits; } fd_set;
#define FD_ZERO(p) ((p)->bits=0)
#define FD_SET(s,p) ((p)->bits|=1U<<(s))
#define FD_ISSET(s,p) ((p)->bits&(1U<<(s)))
typedef long suseconds_t;
typedef struct { int sock; } nuwax_ws_client_t;
static int64_t now;
static unsigned calls, scenario;
static bool expect_read;
static int64_t esp_timer_get_time(void) { return now; }
static int select(int nfds,fd_set *r,fd_set *w,fd_set *e,struct timeval *timeout) {
    assert(nfds==4 && !e && ((r!=NULL)==expect_read) && ((w!=NULL)!=expect_read));
    assert(timeout->tv_sec==0 && timeout->tv_usec>0 && timeout->tv_usec<=2000);
    calls++; now+=100;
    if(scenario==1) { now+=timeout->tv_usec; return 0; }
    if(scenario==2 && calls==1) { errno=EINTR; return -1; }
    if(scenario==3) { errno=ECONNRESET; return -1; }
    if(scenario==4 && calls==1) { (r?r:w)->bits=0; return 1; }
    return 1;
}
'''
for name in ['static esp_err_t transport_wait_socket_ready_until(',
             'static esp_err_t transport_wait_write_ready_until(',
             'static esp_err_t transport_wait_tls_retry_until(']:
    readiness += function(ws, name) + '\n'
readiness += r'''
int main(void) {
    nuwax_ws_client_t c={3};
    for(unsigned direction=0;direction<2;direction++)for(scenario=0;scenario<5;scenario++) {
        expect_read=direction!=0; now=0; calls=0;
        int result=transport_wait_tls_retry_until(&c,expect_read,2000);
        assert(result==(scenario==1?ESP_ERR_TIMEOUT:scenario==3?ESP_FAIL:ESP_OK));
        assert(calls==((scenario==2 || scenario==4)?2U:1U));
    }
    now=2000; calls=0;assert(transport_wait_tls_retry_until(&c,false,2000)==ESP_ERR_TIMEOUT && !calls);
    c.sock=-1;assert(transport_wait_tls_retry_until(&c,true,4000)==ESP_ERR_INVALID_STATE);
    c.sock=3;expect_read=false;now=0;scenario=0;assert(transport_wait_write_ready_until(&c,2000)==ESP_OK);
}
'''

spool = (root/'main/offline_audio_spool.c').read_text(encoding='utf-8')
prefetch = (root/'main/offline_replay_prefetch.inc').read_text(encoding='utf-8')
start = prefetch.index('#define REPLAY_CHECKPOINT_COUNT')
end = prefetch.index('\ntypedef struct {\n    uint8_t *data;', start)
checkpoint = common + r'''
typedef struct { uint32_t position; } replay_stream_t;
typedef struct { uint32_t identity; } archive_header_t;
typedef struct { uint32_t crc; } archive_footer_t;
static struct { bool media_was_ready; unsigned media_recovery_last_error, media_recovery_deadline_ms, media_recovery_backoff_ms; } s;
static atomic_bool s_replay_media_fault;
static atomic_uint s_media_generation;
''' + prefetch[start:end] + '\n'
checkpoint += function(spool, 'static void reset_media_recovery_locked(bool contents_may_have_changed)')
checkpoint += r'''
int main(void) {
    archive_header_t h={123};archive_footer_t f={456};replay_checkpoint_t c;
    replay_checkpoint_prepare(&c,"sealed_scene",4194304,&h,&f);
    c.states[0].position=2490360;replay_checkpoint_store(&c);
    for(unsigned retry=0;retry<10;retry++) {
        reset_media_recovery_locked(false);
        replay_checkpoint_prepare(&c,"sealed_scene",4194304,&h,&f);
        assert(c.states[0].position==2490360);
    }
    h.identity++;replay_checkpoint_prepare(&c,"sealed_scene",4194304,&h,&f);assert(!c.states[0].position);h.identity--;
    f.crc++;replay_checkpoint_prepare(&c,"sealed_scene",4194304,&h,&f);assert(!c.states[0].position);f.crc--;
    replay_checkpoint_prepare(&c,"other_scene",4194304,&h,&f);assert(!c.states[0].position);
    replay_checkpoint_prepare(&c,"sealed_scene",4194303,&h,&f);assert(!c.states[0].position);
    reset_media_recovery_locked(true);
    replay_checkpoint_prepare(&c,"sealed_scene",4194304,&h,&f);assert(!c.states[0].position);
}
'''
clean = spool[spool.index('if (!recovery_required)'):spool.index('result = ensure_directory();', spool.index('if (!recovery_required)'))]
assert 'reset_media_recovery_locked(false)' in clean
assert spool.count('reset_media_recovery_locked(true)') == 3

fallback = common + r'''
#include "ws_send_metrics.h"
#define CONFIG_NUWAX_SD_SESSION_ENABLE 1
#define CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE 1
#define CONFIG_LWIP_TCP_SND_BUF_DEFAULT 131072
#define CONFIG_ESP_WIFI_CACHE_TX_BUFFER_NUM 32
#include "offline_replay_policy.h"
#define ESP_ERR_NOT_FINISHED 4
#define ESP_ERR_INVALID_ARG 5
#define REPLAY_SEND_RETRY_ATTEMPTS 3U
#define REPLAY_SEND_RETRY_DELAY_MS 10U
#define REPLAY_ISOLATED_IO_TIMEOUT_MS 1500U
#define ESP_LOGD(...) ((void)0)
#define pdMS_TO_TICKS(x) (x)
typedef unsigned TickType_t;
typedef struct { bool connected; uint32_t epoch; } nuwax_ws_client_t;
static atomic_bool s_card_online=true;
static bool cancel, busy, expect_formal=true, invalidate;
static unsigned calls, delays, timeouts;
static int64_t fallback_now_us;
static int64_t esp_timer_get_time(void){return fallback_now_us;}
static size_t replay_shared_memory_reserve(void){return replay_shared_reserve_bytes(0U,0U);}
static bool replay_shared_cancelled(void){return cancel;}
static bool nuwax_offline_audio_storage_busy(void){return busy;}
static bool nuwax_ws_is_connected(nuwax_ws_client_t *ws){return ws->connected;}
static uint32_t nuwax_ws_connection_epoch(nuwax_ws_client_t *ws){return ws->epoch;}
static void vTaskDelay(TickType_t t){assert(t);delays++;fallback_now_us+=(int64_t)t*1000;}
static int nuwax_ws_send_archive_buffered_if_epoch(nuwax_ws_client_t *ws,
 const uint8_t *p,size_t n,uint32_t timeout,uint32_t epoch,
 uint8_t *scratch,size_t capacity,nuwax_ws_send_metrics_t *m){
    assert(p && n==12 && epoch==ws->epoch && !scratch && !capacity);
    if(expect_formal)assert(timeout==250U && m && m->shared_tls_archive && m->cancelled && !m->cancelled() &&
        m->tcp_queue_ceiling_bytes==131072U && m->tcp_psram_reserve_bytes==replay_shared_memory_reserve()-REPLAY_TCP_HEADROOM_BYTES);
    else assert(timeout==1500U && !m);
    calls++;
    if(invalidate){ws->epoch++;return ESP_ERR_TIMEOUT;}
    return calls<=timeouts?ESP_ERR_TIMEOUT:ESP_OK;
}
static int nuwax_ws_send_archive_text_if_epoch(nuwax_ws_client_t *ws,const char *p,uint32_t epoch){
    assert(ws->epoch==epoch && p);calls++;return ESP_OK;
}
''' + function(spool,'static esp_err_t replay_send_frame_buffered(') + r'''
int main(void){
    nuwax_ws_client_t ws={true,7};uint8_t p[12]={0};
    assert(replay_send_frame_buffered(&ws,7,p,12,true,false,NULL,0,NULL)==ESP_OK && calls==1);
    calls=delays=0;timeouts=2;
    assert(replay_send_frame_buffered(&ws,7,p,12,true,false,NULL,0,NULL)==ESP_OK && calls==3 && delays==2);
    calls=0;cancel=true;
    assert(replay_send_frame_buffered(&ws,7,p,12,true,false,NULL,0,NULL)==ESP_ERR_NOT_FINISHED && !calls);
    cancel=false;busy=true;
    assert(replay_send_frame_buffered(&ws,7,p,12,true,false,NULL,0,NULL)==ESP_ERR_NOT_FINISHED && !calls);
    busy=false;s_card_online=false;
    assert(replay_send_frame_buffered(&ws,7,p,12,true,false,NULL,0,NULL)==ESP_ERR_NOT_FINISHED && !calls);
    s_card_online=true;timeouts=0;expect_formal=false;
    assert(replay_send_frame_buffered(&ws,7,p,12,true,true,NULL,0,NULL)==ESP_OK && calls==1);
    assert(replay_send_frame_buffered(&ws,7,p,12,false,false,NULL,0,NULL)==ESP_OK && calls==2);
    calls=delays=0;expect_formal=true;timeouts=UINT32_MAX;
    assert(replay_send_frame_buffered(&ws,7,p,12,true,false,NULL,0,NULL)==ESP_ERR_TIMEOUT);
    assert(calls>REPLAY_SEND_RETRY_ATTEMPTS && calls<=REPLAY_SHARED_ADMISSION_RETRY_MS/REPLAY_SEND_RETRY_DELAY_MS+1U && delays==calls);
    timeouts=0;
    calls=delays=0;expect_formal=true;invalidate=true;
    assert(replay_send_frame_buffered(&ws,7,p,12,true,false,NULL,0,NULL)==ESP_ERR_TIMEOUT && calls==1 && !delays);
}
'''

with tempfile.TemporaryDirectory(prefix='nuwax-formal-resilience-') as directory:
    work = Path(directory)
    variants = [('readiness', readiness, True), ('checkpoint', checkpoint, True),
                ('low_memory_sender',fallback,True),
                ('missing_fallback_policy',fallback.replace('.shared_tls_archive = true',
                 '.shared_tls_archive = false'),False),
                ('old_probe_invalidation', checkpoint.replace('if (contents_may_have_changed)',
                 'if (contents_may_have_changed || true)'), False)]
    for name, code, expected in variants:
        source=work/(name+'.c');exe=work/(name+'.exe')
        source.write_text(code, encoding='utf-8')
        subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(root/'main'),
                        str(source),'-o',str(exe)],check=True)
        result=subprocess.run([str(exe)],capture_output=True,text=True)
        assert (result.returncode==0)==expected, name+result.stdout+result.stderr
        print('PASS:',name,'production behavior' if expected else 'regression mutant rejected')
