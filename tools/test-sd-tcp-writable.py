"""Exercise the installed IDF's actual ACK callback and semaphore rounding.

This verifies notification behavior, NOT physical Wi-Fi throughput. No SDK
files, device sockets or saved audio are modified.
"""
from pathlib import Path
import re
import subprocess

root = Path(__file__).resolve().parents[1]
idf = Path('C:/esp/v6.0/esp-idf')
out = root / 'build-host-checks/tcp-writable'
out.mkdir(parents=True, exist_ok=True)
api = (idf / 'components/lwip/lwip/src/api/api_msg.c').read_text()
callback = re.search(r'(?ms)^static err_t\nsent_tcp\(.*?^}', api).group()
sys = (idf / 'components/lwip/port/freertos/sys_arch.c').read_text()
wait = re.search(r'(?ms)^u32_t\nsys_arch_sem_wait\(.*?^}', sys).group()
opt = (idf / 'components/lwip/lwip/src/include/lwip/opt.h').read_text()
default_lowat = re.search(r'^#define TCP_SNDLOWAT\s+(.+)$', opt, re.M).group(1)
cmake = (root / 'tools/cmake/sd-tcp-writable.cmake').read_text()
assert 'PUBLIC "TCP_SNDLOWAT=${nuwax_tcp_lowat}"' in cmake
assert '"2 * ${CONFIG_LWIP_TCP_MSS} + 1"' in cmake
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "ws_send_metrics.h"
#define TCP_MSS 1440U
#define TCP_SND_BUF 46080U
#define LWIP_MIN(a,b) ((a)<(b)?(a):(b))
#define LWIP_MAX(a,b) ((a)>(b)?(a):(b))
#ifdef EARLY_WRITABLE
#define TCP_SNDLOWAT (2U*TCP_MSS+1U)
#else
#define TCP_SNDLOWAT DEFAULT_LOWAT
#endif
#define TCP_SNDQUEUELOWAT 64U
#define LWIP_UNUSED_ARG(x) ((void)(x))
#define LWIP_ASSERT(s,x) assert(x)
#define WRITE_DELAYED
#define NETCONN_WRITE 1
#define NETCONN_CLOSE 2
#define NETCONN_FLAG_CHECK_WRITESPACE 1U
#define NETCONN_EVT_SENDPLUS 2
#define ERR_OK 0
typedef int err_t;
typedef unsigned u16_t;
struct tcp_pcb { unsigned available, queued; };
struct netconn { int state; struct { struct tcp_pcb *tcp; } pcb; unsigned flags; };
static unsigned signals, resumed, closed;
static void lwip_netconn_do_writemore(struct netconn *c){(void)c;resumed++;}
static void lwip_netconn_do_close_internal(struct netconn *c){(void)c;closed++;}
#define tcp_sndbuf(p) ((p)->available)
#define tcp_sndqueuelen(p) ((p)->queued)
#define netconn_clear_flags(c,f) ((c)->flags&=~(f))
#define API_EVENT(c,e,l) do{assert((e)==NETCONN_EVT_SENDPLUS);(void)(c);(void)(l);signals++;}while(0)
CALLBACK
typedef unsigned u32_t,TickType_t,sys_sem_t;
typedef int BaseType_t;
#define portTICK_PERIOD_MS 10U
#define portMAX_DELAY 0xffffffffU
#define pdTRUE 1
#define errQUEUE_EMPTY 0
#define SYS_ARCH_TIMEOUT 0xffffffffU
static unsigned ticks;
static int xSemaphoreTake(unsigned s,unsigned n){(void)s;ticks=n;return n==portMAX_DELAY?pdTRUE:0;}
WAIT
int main(void){
  struct tcp_pcb pcb={0};struct netconn c={.pcb={&pcb}};
  /* Verify every send-space and queue-water boundary, not just one case. */
  for(unsigned q=0;q<=128;q++)for(unsigned free=0;free<=TCP_SND_BUF;free++){
    pcb=(struct tcp_pcb){free,q};signals=0;c.flags=1;
    assert(sent_tcp(&c,&pcb,1440)==0);
    bool ready=free>TCP_SNDLOWAT && q<TCP_SNDQUEUELOWAT;
    assert(signals==(unsigned)ready && c.flags==(ready?0U:1U));
  }
  unsigned first=0;pcb.queued=32;
  for(unsigned ack=1;ack<=32;ack++){
    pcb.available=ack*TCP_MSS;signals=0;sent_tcp(&c,&pcb,1440);
    if(signals){first=ack;break;}
  }
#ifdef EARLY_WRITABLE
  assert(first==3);
#else
  assert(first==17);
#endif
  c.pcb.tcp=NULL;signals=0;sent_tcp(&c,&pcb,1);assert(!signals);
  c.state=NETCONN_WRITE;sent_tcp(&c,&pcb,1);assert(resumed==1);
  c.state=NETCONN_CLOSE;sent_tcp(&c,&pcb,1);assert(closed==1);
  sys_sem_t sem=1;assert(sys_arch_sem_wait(&sem,10)==SYS_ARCH_TIMEOUT && ticks==2);
  assert(sys_arch_sem_wait(&sem,1)==SYS_ARCH_TIMEOUT && ticks==2);
  assert(sys_arch_sem_wait(&sem,0)==0 && ticks==portMAX_DELAY);
  nuwax_ws_send_metrics_t m={0};
  nuwax_ws_metrics_timeout_overrun(&m,100,100);assert(!m.writable_timeout_overrun_us);
  nuwax_ws_metrics_timeout_overrun(&m,90,100);assert(!m.writable_timeout_overrun_us);
  nuwax_ws_metrics_timeout_overrun(&m,20000,10000);
  nuwax_ws_metrics_timeout_overrun(&m,61519,10000);
  nuwax_ws_metrics_timeout_overrun(NULL,20000,10000);
  assert(m.writable_timeout_overrun_us==61519 && m.writable_timeout_overrun_max_us==51519);
  printf("PASS: actual IDF sent_tcp lowat=%u first_writable_after_MSS=%u; queue guard, close/write callbacks, 100 Hz timeout rounding and overrun metrics\n",TCP_SNDLOWAT,first);
}
'''.replace('DEFAULT_LOWAT', default_lowat).replace('CALLBACK', callback).replace('WAIT\n', wait+'\n')
source = out/'check.c'
source.write_text(code, encoding='utf-8')
for early in [False, True]:
    exe = out/('early.exe' if early else 'default.exe')
    subprocess.run(['gcc', '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I'+str(root/'main'), *(['-DEARLY_WRITABLE'] if early else []),
                    str(source), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
