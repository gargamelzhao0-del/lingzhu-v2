"""Exercise the production RX contention path, without a socket or scheduler."""
from pathlib import Path
import re
import subprocess

root = Path(__file__).resolve().parents[1]
source = (root / 'main/ws_transport.c').read_text(encoding='utf-8')
functions = []
for name in ('transport_read_until', 'recv_exact_until'):
    match = re.search(r'(?ms)^static esp_err_t ' + name + r'\(.*?^}', source)
    assert match, name
    functions.append(match.group())
out = root / 'build-host-checks'
out.mkdir(exist_ok=True)
test = out / 'ws_rx_yield_host_check.c'
test.write_text(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include "esp_err.h"
typedef struct { bool plain_event_rx, use_tls; atomic_uint tx_waiters; atomic_bool rx_task_running, connected; } nuwax_ws_client_t;
#define NUWAX_WS_RX_IO_LOCK_SLICE_MS 10U
#define pdMS_TO_TICKS(ms) ((ms)/10U)
static int64_t now;
static unsigned delays, locks, reads, failure, advance;
static nuwax_ws_client_t *active;
static int64_t esp_timer_get_time(void){return now;}
static bool io_deadline_expired(int64_t deadline){return now>=deadline;}
static void vTaskDelay(unsigned ticks){assert(ticks==1);delays++;now+=10000;active->tx_waiters=0;}
static bool transport_io_lock_until(nuwax_ws_client_t *c,int64_t deadline){
    (void)c;(void)deadline;locks++;if(failure==1){failure=0;return false;}return true;
}
static void transport_io_unlock(nuwax_ws_client_t *c){(void)c;}
static ssize_t transport_read_unlocked(nuwax_ws_client_t *c,uint8_t *p,size_t n){
    (void)c;(void)n;reads++;now+=advance;
    if(failure==2){failure=0;errno=EAGAIN;return -1;}
    *p=0x82;return 1;
}
static bool io_retryable(nuwax_ws_client_t *c,ssize_t r,int e){(void)c;return r<0 && e==EAGAIN;}
static esp_err_t recv_plain_event_until(nuwax_ws_client_t *c,uint8_t *p,size_t n,int64_t d){
    (void)c;(void)p;(void)n;(void)d;assert(!"legacy path must not use plain event RX");return ESP_FAIL;
}
''' + '\n'.join(functions) + r'''
int main(void){
    nuwax_ws_client_t ws={0};ws.rx_task_running=true;ws.connected=true;active=&ws;
    for(unsigned scenario=0;scenario<4;scenario++){
        now=0;delays=locks=reads=0;failure=scenario==2?1:scenario==3?2:0;
        ws.tx_waiters=scenario==1;uint8_t bytes[2]={0};
        assert(recv_exact_until(&ws,bytes,2,100000)==ESP_OK);
        assert(bytes[0]==0x82 && bytes[1]==0x82);
        assert(delays==(scenario?1U:0U));
    }
    now=0;advance=20000;uint8_t byte=0;
    assert(recv_exact_until(&ws,&byte,1,10000)==ESP_OK && byte==0x82);
    /* Successfully consumed bytes survive crossing the deadline. */
    advance=0;ws.connected=false;
    assert(recv_exact_until(&ws,&byte,1,100000)==ESP_ERR_INVALID_STATE);
    puts("PASS: production RX has one blocking yield per TX/lock/EAGAIN retry; successful read deadline and disconnect preserved");
}
''', encoding='utf-8')
exe = out / 'ws_rx_yield_host_check.exe'
subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
    '-I' + str(root / 'main/audio_modes/meeting/host_stubs'), str(test), '-o', str(exe)], check=True)
subprocess.run([str(exe)], check=True)
