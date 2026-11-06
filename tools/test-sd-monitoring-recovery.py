"""Deterministic regression of production pressure, ACK cancellation and tail order.

Uses real functions extracted from the spool, with controllable RTOS doubles.
No hardware, gateway archives or device settings are modified.
"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
out = root / 'tmp/sd-monitoring-recovery-checks'
out.mkdir(parents=True, exist_ok=True)
source = (root/'main/offline_audio_spool.c').read_text(encoding='utf-8-sig')


def extract(name):
    m = re.search(r'^(?:static\s+)?\w+[\s*]+\b' + name + r'\([^;{}]*\)\s*\{', source, re.M)
    assert m, name
    pos = source.index('{', m.start()) + 1
    depth = 1
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[m.start():pos]


common = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "esp_err.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define pdMS_TO_TICKS(x) (x)
#define portTICK_PERIOD_MS 1U
'''
pressure = common + r'''
#include "sd_capture_admission.h"
static sd_capture_admission_t s_capture_admission;
#define CONFIG_NUWAX_SD_SESSION_ENABLE 1
#define CONFIG_NUWAX_OTA_ENABLE 1
#define CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE 1
#define QUEUE_DEPTH 128U
typedef struct { bool connected; uint32_t epoch; } nuwax_ws_client_t;
static struct { atomic_bool media_recovery_requested, writer_error, active, closing,
    capture_running, writer_busy; void *queue; } s;
static atomic_bool s_shutdown_requested, s_media_ready=true, s_card_online=true,
    s_formal_replay_cancel_requested, s_card_finishing, s_card_failed;
static atomic_uint s_mirror_closers;
static bool ota, active=true;
static uint32_t queued, backlog, delays, action;
static int64_t now=1000;
static uint32_t offline_now_ms(void){return (uint32_t)(now/1000);}
static bool enabled(void){return true;}
static bool runtime_initialized(void){return true;}
static bool nuwax_ota_device_is_active(void){return ota;}
static bool nuwax_sd_session_active(void){return active;}
static bool nuwax_ws_is_connected(nuwax_ws_client_t *ws){return ws->connected;}
static uint32_t nuwax_ws_connection_epoch(nuwax_ws_client_t *ws){return ws->epoch;}
static uint32_t uxQueueMessagesWaiting(void *queue){assert(queue);return queued;}
static uint32_t nuwax_wake_word_capture_backlog_ms(void){return backlog;}
static int64_t esp_timer_get_time(void){return now;}
static bool replay_ota_active(void){return ota;}
static bool nuwax_power_ctrl_shutdown_preparing(void){return atomic_load(&s_shutdown_requested);}
static void vTaskDelay(unsigned ticks){
    assert(ticks);++delays;now+=ticks*1000;
    if(action==1)s_card_online=false;
    else if(action==2)ota=true;
    else if(action==3)s_media_ready=false;
    else if(action==4)s_shutdown_requested=true;
    else if(action==5)s_formal_replay_cancel_requested=true;
    else {if(queued)queued--;if(backlog)backlog--;}
}
static void nuwax_wake_word_wait_capture_headroom(unsigned ms){vTaskDelay(ms);}
'''
pressure += '\n'.join(extract(n) for n in [
    'nuwax_offline_audio_storage_busy', 'replay_capture_queue_depth',
    'replay_ack_cancel_status', 'replay_test_connect_cancelled'])
pressure += '\n#include "offline_replay_pressure.inc"\n'
pressure += r'''
int main(void){
    nuwax_ws_client_t ws={true,7};s.queue=&queued;
    for(queued=0;queued<=128;queued++){
        assert(!nuwax_offline_audio_storage_busy());
        assert(replay_ack_cancel_status(&ws,7,false)==ESP_OK);
        assert(!replay_test_connect_cancelled(NULL));
    }
    uint64_t waited=0;queued=63;
    assert(replay_wait_capture_headroom(&ws,7,NULL,&waited)==ESP_OK && !delays);
    queued=64;
    assert(replay_wait_capture_headroom(&ws,7,NULL,&waited)==ESP_OK);
    assert(queued==32 && delays==32 && waited==32000 && ws.connected);
    delays=0;queued=0;backlog=90;
    assert(replay_wait_capture_headroom(&ws,7,NULL,&waited)==ESP_OK && backlog==30 && delays==60);
    for(action=1;action<=5;action++){
        queued=100;backlog=0;
        assert(replay_wait_capture_headroom(&ws,7,NULL,NULL)==ESP_ERR_NOT_FINISHED);
        assert(queued==100 && ws.connected);
        s_card_online=true;ota=false;s_media_ready=true;s_shutdown_requested=false;
        s_formal_replay_cancel_requested=false;
    }
    action=0;queued=100;ws.epoch++;
    assert(replay_wait_capture_headroom(&ws,7,NULL,NULL)==ESP_ERR_INVALID_STATE);
    ws.epoch=7;atomic_bool cancel=true;
    assert(replay_wait_capture_headroom(&ws,7,&cancel,NULL)==ESP_ERR_NOT_FINISHED);
    cancel=false;queued=8;backlog=0;delays=0;
    assert(replay_read_storage_enter(&ws,7,&cancel,&waited)==ESP_OK && queued==2 && delays==6);
    replay_read_storage_leave();
    cancel=true;assert(replay_read_storage_enter(&ws,7,&cancel,NULL)==ESP_ERR_NOT_FINISHED);
    assert(!nuwax_offline_audio_storage_busy()); // Card meeting uses the same pressure gates.
    active=false;assert(nuwax_offline_audio_storage_busy()); // Legacy busy policy unchanged.
    queued=0;assert(!nuwax_offline_audio_storage_busy());
    puts("PASS: writer hysteresis retains ACK/connection; cancellation, wake, meeting and legacy guards preserved");
}
'''

header = (root/'main/offline_audio_spool.h').read_text(encoding='utf-8')
mirror_type = re.search(r'typedef struct \{\s*bool started;.*?\} nuwax_offline_audio_mirror_t;', header, re.S).group(0)
frame_type = re.search(r'typedef struct \{\s*uint32_t sequence;.*?\} nuwax_offline_audio_frame_t;', header, re.S).group(0)
staged_type = re.search(r'typedef struct \{\s*uint32_t sequence;.*?\} mirror_staged_frame_t;', source, re.S).group(0)
tail = common + r'''
#define CONFIG_NUWAX_SD_SESSION_ENABLE 0
#define MIRROR_STAGING_FRAMES 768U
#define MIRROR_CLOSE_TIMEOUT_MS 20000U
#define OFFLINE_MIRROR_ATTACH_RETRY_MS 1U
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define pdPASS 1
typedef unsigned TickType_t;
typedef int nuwax_offline_audio_mode_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
static unsigned critical;
#define portENTER_CRITICAL(p) do { (void)(p); assert(!critical);critical++; } while(0)
#define portEXIT_CRITICAL(p) do { (void)(p); assert(critical);critical--; } while(0)
static atomic_uint s_mirror_closers;
static atomic_uint s_mirror_staging_rings;
'''+mirror_type+frame_type+staged_type+r'''
static uint32_t now=1, order[32], written, lost, ends;
static bool permit_attach, archive_open, reject_append, spawn_fail, invoke_older;
static unsigned workers, allocations;
static void (*tasks[8])(void *);static void *args[8];
static uint32_t offline_now_ms(void){return now;}
static bool offline_deadline_elapsed(uint32_t a,uint32_t b){return (int32_t)(a-b)>=0;}
static void *heap_caps_malloc(size_t n,unsigned caps){(void)caps;allocations++;return calloc(1,n);}
static void heap_caps_free(void *p){if(p){assert(allocations);allocations--;free(p);}}
static void nuwax_offline_audio_supervisor_wake(void){}
static void vTaskDeleteWithCaps(void *p){assert(!p);}
static int xTaskCreatePinnedToCoreWithCaps(void (*fn)(void*),const char *name,unsigned stack,
    void *arg,unsigned priority,void *handle,unsigned core,unsigned caps){
    (void)name;(void)caps;assert(stack==4096&&priority==3&&!handle&&core==0);
    if(spawn_fail)return 0;assert(workers<8);tasks[workers]=fn;args[workers++]=arg;return pdPASS;
}
static void vTaskDelay(unsigned ms){
    assert(!critical);now+=ms;
    if(invoke_older){invoke_older=false;tasks[0](args[0]);}
}
static esp_err_t begin_archive(int mode,const char *session,unsigned hz,unsigned channels,
    unsigned ms,bool online,bool mirror){
    (void)mode;(void)session;(void)hz;(void)channels;(void)ms;(void)online;(void)mirror;
    if(!permit_attach||archive_open)return ESP_ERR_NOT_FINISHED;
    archive_open=true;return ESP_OK;
}
static esp_err_t append_frame(const nuwax_offline_audio_frame_t *f,TickType_t wait,bool loss){
    assert(archive_open&&!wait&&!loss);
    if(reject_append)return ESP_ERR_NOT_FINISHED;
    order[written++]=f->sequence;return ESP_OK;
}
static esp_err_t nuwax_offline_audio_request_end(bool complete){
    (void)complete;assert(archive_open);archive_open=false;ends++;return ESP_OK;
}
'''
tail += '\n'.join(extract(n) for n in ['mirror_note_loss','mirror_drain','mirror_finish'])
tail += '\n#include "offline_mirror_close_queue.inc"\n'
tail += '\n'.join(extract(n) for n in ['mirror_close_task','nuwax_offline_audio_mirror_request_close'])
tail += r'''
static nuwax_offline_audio_mirror_t make(unsigned sequence){
    nuwax_offline_audio_mirror_t m={.started=true,.complete=true,.staging_count=2};
    strcpy(m.session_id,"monitor_tail");
    m.staging=heap_caps_malloc(MIRROR_STAGING_FRAMES*sizeof(mirror_staged_frame_t),1);
    atomic_fetch_add(&s_mirror_staging_rings,1U);
    for(unsigned i=0;i<2;i++)((mirror_staged_frame_t*)m.staging)[i].sequence=sequence+i;
    return m;
}
int main(void){
    nuwax_offline_audio_mirror_t a=make(10),b=make(20);
    assert(nuwax_offline_audio_mirror_request_close(&a,true,now)==ESP_OK);
    assert(nuwax_offline_audio_mirror_request_close(&b,true,now)==ESP_OK);
    assert(!a.started&&!b.started&&s_mirror_closers==2&&written==0);
    permit_attach=true;invoke_older=true;tasks[1](args[1]); // Younger worker wins CPU first.
    assert(!invoke_older&&written==4&&order[0]==10&&order[1]==11&&order[2]==20&&order[3]==21);
    assert(!s_mirror_closers&&!s_mirror_staging_rings&&!allocations&&!archive_open&&ends==2);
    // Launch failure removes the unpublished middle/tail node without a ticket hole.
    workers=0;permit_attach=false;a=make(30);b=make(40);
    nuwax_offline_audio_mirror_request_close(&a,true,now);
    spawn_fail=true;nuwax_offline_audio_mirror_request_close(&b,true,now);
    assert(!b.complete&&b.rejected_frames==2&&s_mirror_closers==1);
    spawn_fail=false;permit_attach=true;tasks[0](args[0]);
    assert(!s_mirror_closers&&!allocations&&order[4]==30&&order[5]==31);
    // A blocked predecessor times out, marks loss and releases FIFO ownership.
    workers=0;permit_attach=false;a=make(50);b=make(60);
    nuwax_offline_audio_mirror_request_close(&a,true,now);
    nuwax_offline_audio_mirror_request_close(&b,true,now);
    uint32_t start=now;permit_attach=true;reject_append=true;tasks[0](args[0]);
    assert(now-start>=MIRROR_CLOSE_TIMEOUT_MS&&s_mirror_closers==1);
    reject_append=false;tasks[1](args[1]);
    assert(!s_mirror_closers&&!s_mirror_staging_rings&&!allocations&&!archive_open&&order[6]==60&&order[7]==61);
    // Removal of a non-head failed spawn cannot detach the remaining head.
    mirror_close_node_t n1={0},n2={0},n3={0};
    mirror_close_enqueue(&n1);mirror_close_enqueue(&n2);mirror_close_enqueue(&n3);
    mirror_close_remove(&n2);assert(mirror_close_is_head(&n1));
    mirror_close_remove(&n1);assert(mirror_close_is_head(&n3));
    mirror_close_remove(&n3);assert(!s_mirror_closers);
    (void)lost;puts("PASS: production tail workers keep PCM FIFO across inverted dispatch, spawn failure and timeout; no leaks");
}
'''

for name, code in [('pressure', pressure), ('tails', tail)]:
    file = out / f'{name}.c'; file.write_text(code, encoding='utf-8')
    exe = out / f'{name}.exe'
    subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
        '-I'+str(root/'main/audio_modes/meeting/host_stubs'), '-I'+str(root/'main'),str(file),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)

exe = out/'catalog-isolation.exe'
subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-DSD_AUDIO_CATALOG_ROOT="catalog"',
    '-DESP_ERR_INVALID_CRC=0x109','-DESP_ERR_NOT_FOUND=0x105',
    '-I'+str(root/'main/audio_modes/meeting/host_stubs'),'-I'+str(root/'main'),
    str(root/'main/sd_audio_catalog.c'),str(root/'main/sd_audio_isolation_host_check.c'),'-o',str(exe)],check=True)
with tempfile.TemporaryDirectory(prefix='sd-scene-isolation-') as directory:
    subprocess.run([str(exe)],cwd=directory,check=True)
