"""Fault tests of the exact production plain RX loop; no device timing claims."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
out = root / 'build-host-checks'
out.mkdir(exist_ok=True)
test = out / 'ws_plain_event_rx_check.c'
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
typedef long suseconds_t;
struct timeval { long tv_sec; suseconds_t tv_usec; };
typedef struct { int sock; } fd_set;
#define FD_ZERO(p) ((p)->sock=-1)
#define FD_SET(s,p) ((p)->sock=(s))
typedef struct { int sock; bool use_tls, close; atomic_bool connected;
    uint32_t epoch, plain_rx_reads, plain_rx_waits, plain_rx_timeouts, tx_waiters;
} nuwax_ws_client_t;
static nuwax_ws_client_t *active;
static int64_t now;
static unsigned mode, reads, waits;
static int64_t esp_timer_get_time(void){return now;}
static bool io_deadline_expired(int64_t d){return now>=d;}
static uint32_t nuwax_ws_connection_epoch(nuwax_ws_client_t *c){return c->epoch;}
static bool close_requested_for_epoch(nuwax_ws_client_t *c,uint32_t e){return c->close && c->epoch==e;}
static ssize_t fake_recv(int sock,void *data,size_t len,int flags){
    assert(sock==4 && len>0 && flags==0); reads++;
    if(mode==3)return 0;
    if(mode==4){errno=ECONNRESET;return -1;}
    if(mode==5 && reads==1){errno=EINTR;return -1;}
    if(mode==6)now+=20000;
    if(mode==1 || mode==2 || mode==7 || mode==8 || mode==9 || mode==10 || mode==11 || mode==12 || (mode==0 && reads==2)){
        errno=EAGAIN;return -1;
    }
    *(uint8_t*)data=(uint8_t)reads;return 1;
}
static int fake_select(int n,fd_set *r,void *w,void *e,struct timeval *t){
    assert(n==5 && r->sock==4 && !w && !e && t->tv_sec==0);
    assert(t->tv_usec>0 && t->tv_usec<=100000);waits++;
    if(mode==0){now+=123;return 1;}
    if(mode==8){errno=EBADF;return -1;}
    if(mode==9){mode=0;errno=EINTR;return -1;}
    if(mode==10){mode=0;errno=EAGAIN;return -1;}
    now+=t->tv_usec;
    if(mode==2)active->close=true;
    if(mode==7)active->epoch++;
    if(mode==11)active->connected=false;
    if(mode==12){mode=0;return 1;} /* spurious readiness, recv checks again */
    return 0;
}
#define recv fake_recv
#define select fake_select
#include "ws_plain_rx.inc"
static void reset(nuwax_ws_client_t *c,unsigned m){
    *c=(nuwax_ws_client_t){.sock=4,.connected=true,.epoch=3,.tx_waiters=1};
    active=c;mode=m;now=0;reads=waits=0;
}
int main(void){
    nuwax_ws_client_t c;uint8_t bytes[3];
    reset(&c,0);assert(recv_plain_event_until(&c,bytes,3,500000)==ESP_OK);
    assert(bytes[0]==1 && bytes[1]==3 && bytes[2]==4 && waits==1 && now==123 && c.tx_waiters==1);
    reset(&c,1);assert(recv_plain_event_until(&c,bytes,1,250001)==ESP_ERR_TIMEOUT);
    assert(waits==3 && now==250001 && c.plain_rx_timeouts==3);
    for(unsigned m=2;m<=12;m++){
        reset(&c,m);
        int result=recv_plain_event_until(&c,bytes,1,m==6?10000:500000);
        int expected=(m==2 || m==7 || m==11)?ESP_ERR_INVALID_STATE:
                     (m==3 || m==4 || m==8)?ESP_FAIL:ESP_OK;
        assert(result==expected);
        if(m==2 || m==7 || m==11)assert(now==100000 && reads==1);
        if(m==6)assert(now==20000 && bytes[0]==1);
    }
    reset(&c,0);c.use_tls=true;assert(recv_plain_event_until(&c,bytes,1,500000)==ESP_ERR_INVALID_STATE && !reads);
    reset(&c,0);c.sock=-1;assert(recv_plain_event_until(&c,bytes,1,500000)==ESP_ERR_INVALID_STATE && !reads);
    reset(&c,0);c.close=true;assert(recv_plain_event_until(&c,bytes,1,500000)==ESP_ERR_INVALID_STATE && !reads);
    puts("PASS: production plain event RX fragmented reads, immediate wake despite TX, bounded idle/cancel/epoch/disconnect, EOF/errors, EINTR/EAGAIN and consumed-byte deadline");
}
''', encoding='utf-8')
exe = test.with_suffix('.exe')
subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
    '-I' + str(root / 'main'), '-I' + str(root / 'main/audio_modes/meeting/host_stubs'),
    str(test), '-o', str(exe)], check=True)
subprocess.run([str(exe)], check=True)
