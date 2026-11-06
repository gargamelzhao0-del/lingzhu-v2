"""Compile real wrappers/recovery; check result/ownership and retry bounds.

Optional final ELF check verifies callers bind to wrappers, wrappers bind to
original SDK functions, and the enforced configuration is in the built image.
No physical throughput claim.
"""
from pathlib import Path
import argparse, subprocess, re, json, hashlib

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path)
parser.add_argument('--expected-version', default='7.0.133', help='Expected app descriptor version for the linked build')
parser.add_argument('--wifi-cache', type=int, choices=[16,32], default=32)
parser.add_argument('--tcpip-core', type=int, choices=[0,2147483647], default=0)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
out = root / 'build-host-checks/network-tx-probe'
out.mkdir(parents=True, exist_ok=True)
source = r'''
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
static unsigned critical_depth;
static void (*before_lock_hook)(void), (*after_unlock_hook)(void);
static void (*before_cas_hook)(void *);
static void (*before_load_hook)(const void *);
static inline void test_enter_lock(portMUX_TYPE *lock){
 (void)lock;assert(!critical_depth);
 if(before_lock_hook)before_lock_hook();
 assert(!critical_depth);critical_depth=1;
}
static inline void test_exit_lock(portMUX_TYPE *lock){
 (void)lock;assert(critical_depth==1);critical_depth=0;
 if(after_unlock_hook)after_unlock_hook();
}
#define portENTER_CRITICAL(lock) test_enter_lock(lock)
#define portEXIT_CRITICAL(lock) test_exit_lock(lock)
#undef atomic_compare_exchange_strong_explicit
#define atomic_compare_exchange_strong_explicit(object, expected, desired, success, failure) \
 ((before_cas_hook ? before_cas_hook((void *)(object)) : (void)0), \
  __atomic_compare_exchange_n((object),(expected),(desired),0,(success),(failure)))
#undef atomic_load_explicit
#define atomic_load_explicit(object, order) \
 ((before_load_hook ? before_load_hook((const void *)(object)) : (void)0), \
  __atomic_load_n((object),(order)))
typedef int esp_err_t,err_t;
typedef struct {int unused;} esp_netif_t;
struct tcp_seg {struct tcp_seg *next;unsigned len;};
static struct tcp_seg test_unsent_segment={.len=1440};
struct tcp_pcb {unsigned remote_port,local_port,snd_wnd,flags,cwnd,snd_buf,snd_queuelen,nrtx,state,dupacks;
 uint32_t lastack,snd_nxt,snd_lbb;int16_t rto,rtime;
 struct tcp_seg *unsent,*unacked;struct tcp_pcb *next;};
static struct tcp_pcb *tcp_active_pcbs;
static unsigned now_ms,posts;
static int post_result;
static bool immediate_callback;
static void (*posted)(void *);static void *posted_arg;
static int64_t esp_timer_get_time(void){return (int64_t)now_ms*1000;}
static int tcpip_try_callback(void (*f)(void *),void *arg){
 assert(!critical_depth);++posts;assert(!posted);if(post_result)return post_result;
 if(immediate_callback){f(arg);return 0;}posted=f;posted_arg=arg;return 0;
}
#define CONFIG_NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE 1
#define NUWAX_NETWORK_PROBE_HOST_TEST 1
#define ESP_OK 0
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_STATE 0x103
#define ERR_OK 0
#define ERR_MEM -1
#define ERR_RTE -4
#define TF_NAGLEMEMERR 0x80
#define ESTABLISHED 4
#define SYN_SENT 2
#define SYN_RCVD 3
#define TCP_SLOW_INTERVAL 500U
#define TF_RTO 0x800
#define ESP_LOGI(tag,...) do {(void)(tag);if(0)printf(__VA_ARGS__);}while(0)
#define ESP_LOGW ESP_LOGI
#define ESP_TIMER_TASK 0
typedef void *esp_timer_handle_t;
typedef struct { void (*callback)(void *); int dispatch_method; const char *name; } esp_timer_create_args_t;
static esp_timer_create_args_t timer_args;
static bool timer_active;
static unsigned timer_deadline, timer_creates;
static int timer_create_error, timer_start_error;
static int esp_timer_create(const esp_timer_create_args_t *args,esp_timer_handle_t *h){
 assert(!critical_depth);++timer_creates;timer_args=*args;if(timer_create_error)return timer_create_error;*h=&timer_args;return 0;
}
static int esp_timer_start_once(esp_timer_handle_t h,uint64_t us){
 assert(!critical_depth);assert(h==&timer_args && us>=1000 && us<=100000);if(timer_start_error)return timer_start_error;
 if(timer_active)return ESP_ERR_INVALID_STATE;
 timer_active=true;timer_deadline=now_ms+(unsigned)(us/1000);return 0;
}
static int xPortGetCoreID(void){return 0;}

struct pbuf { void *payload;unsigned len; };
struct netif { unsigned id; };
typedef struct { int8_t rssi;uint8_t primary; } wifi_ap_record_t;
typedef unsigned wifi_ps_type_t;
#define WIFI_PS_NONE 0U
static unsigned radio_reads;
static int radio_error;
static int esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap){assert(!critical_depth);radio_reads++;if(radio_error)return radio_error;ap->rssi=-62;ap->primary=6;return 0;}
static int esp_wifi_get_ps(wifi_ps_type_t *ps){assert(!critical_depth);radio_reads++;if(radio_error)return radio_error;*ps=0;return 0;}
#include "network_tx_probe.c"
static struct pbuf *expected_packet;
static struct netif *expected_input;
static unsigned input_calls,input_mode;
static uint32_t input_ack;
void __real_tcp_input(struct pbuf *p,struct netif *inp){
 assert(!critical_depth && p==expected_packet && inp==expected_input);input_calls++;
 now_ms+=2;
 if(input_mode==1 && tcp_active_pcbs)tcp_active_pcbs->lastack=input_ack;
 if(input_mode==2)tcp_active_pcbs=NULL; // SDK closed/freed endpoint
 if(p){p->payload=(void*)1;p->len=0;} // SDK consumed pbuf: touching it afterwards would fault
}

static unsigned net_calls,tcp_calls;
static esp_err_t next_net;
static err_t next_tcp;
static int simulate_output;
static void *want_data,*want_buf;static size_t want_len;
static esp_netif_t *want_net;
static struct tcp_pcb *want_pcb;
esp_err_t __real_esp_netif_transmit_wrap(esp_netif_t *net,void *data,size_t len,void *buf){
 assert(net==want_net && data==want_data && len==want_len && buf==want_buf);
 ++net_calls;return next_net;
}
err_t __real_tcp_output(struct tcp_pcb *p){
 assert(!critical_depth);assert(p==want_pcb);++tcp_calls;
 if(simulate_output && p){if(next_tcp==ERR_MEM)p->flags|=TF_NAGLEMEMERR;else if(next_tcp==ERR_OK)p->flags&=~TF_NAGLEMEMERR;}
 return next_tcp;
}
static void deliver(void){assert(posted);void (*fn)(void*)=posted;void *arg=posted_arg;posted=NULL;posted_arg=NULL;fn(arg);}
static void reset_retry(void){
 assert(!posted);atomic_store(&s_retry_budget,0);atomic_store(&s_retry_queued,0);atomic_store(&s_retry_next_ms,0);
 now_ms=1000;posts=0;post_result=0;simulate_output=1;
}
static void check_recovery(void){
 struct tcp_pcb p={.remote_port=8810,.state=ESTABLISHED,.unsent=&test_unsent_segment};
 struct tcp_pcb cloud={.remote_port=443,.state=ESTABLISHED,.unsent=&test_unsent_segment,.flags=TF_NAGLEMEMERR,.next=&p};
 reset_retry();tcp_active_pcbs=&cloud;want_pcb=&p;next_tcp=ERR_MEM;
 unsigned before=tcp_calls;assert(__wrap_tcp_output(&p)==ERR_MEM);assert(tcp_calls==before+1);
 for(unsigned i=0;i<5;i++){nuwax_network_tx_retry_poll();assert(posts==0);now_ms++;}
 nuwax_network_tx_retry_poll();assert(posts==1 && posted && tcp_calls==before+1);
 for(unsigned i=0;i<100;i++){nuwax_network_tx_retry_poll();}assert(posts==1);
 next_tcp=ERR_OK;deliver();assert(tcp_calls==before+2 && !atomic_load(&s_retry_budget));
 assert(atomic_load(&s_retry_recovered)==1 && cloud.flags==TF_NAGLEMEMERR);
 // Persistent driver pressure has only four output attempts, no busy loop.
 reset_retry();next_tcp=ERR_MEM;__wrap_tcp_output(&p);before=tcp_calls;
 unsigned gaps[]={5,10,20,40};
 for(unsigned i=0;i<4;i++){
  now_ms+=gaps[i]-1;nuwax_network_tx_retry_poll();assert(!posted);
  now_ms++;nuwax_network_tx_retry_poll();assert(posted);deliver();
 }
 assert(tcp_calls==before+4 && !atomic_load(&s_retry_budget) && atomic_load(&s_retry_exhausted)==1);
 now_ms+=10000;nuwax_network_tx_retry_poll();assert(!posted && posts==4);
 // A PCB closed after enqueue cannot be dereferenced later.
 reset_retry();__wrap_tcp_output(&p);now_ms+=20;nuwax_network_tx_retry_poll();
 before=tcp_calls;tcp_active_pcbs=NULL;deliver();assert(tcp_calls==before && !atomic_load(&s_retry_budget));
 assert(atomic_load(&s_retry_stale)==1);
 // Successful natural recovery, a different port or a closing PCB are skipped.
 for(unsigned mode=0;mode<3;mode++){
  reset_retry();p.remote_port=8810;p.state=ESTABLISHED;p.flags=TF_NAGLEMEMERR;next_tcp=ERR_MEM;__wrap_tcp_output(&p);
  tcp_active_pcbs=&p;now_ms+=20;nuwax_network_tx_retry_poll();before=tcp_calls;
  if(mode==0)p.flags=0;else if(mode==1)p.remote_port=443;else p.state=0;
  deliver();assert(tcp_calls==before && !atomic_load(&s_retry_budget));
 }
 // Posting failures never block or retain pointers; retry after backoff.
 reset_retry();p.remote_port=8810;p.state=ESTABLISHED;tcp_active_pcbs=&p;next_tcp=ERR_MEM;__wrap_tcp_output(&p);
 now_ms+=20;post_result=ERR_MEM;before=tcp_calls;nuwax_network_tx_retry_poll();
 assert(!posted && !atomic_load(&s_retry_queued) && tcp_calls==before);
 post_result=0;now_ms+=99;nuwax_network_tx_retry_poll();assert(!posted);
 now_ms++;nuwax_network_tx_retry_poll();assert(posted);next_tcp=ERR_RTE;deliver();
 assert(!atomic_load(&s_retry_budget) && atomic_load(&s_tcp_route)==1);
 // Millisecond wrap is safe; unrelated ports/errors never arm recovery.
 reset_retry();now_ms=0xfffffff0U;next_tcp=ERR_MEM;__wrap_tcp_output(&p);
 now_ms=0xfffffff4U;nuwax_network_tx_retry_poll();assert(!posted);
 now_ms=0xfffffff5U;nuwax_network_tx_retry_poll();assert(posted);next_tcp=0;deliver();
 reset_retry();next_tcp=ERR_RTE;__wrap_tcp_output(&p);now_ms+=100;nuwax_network_tx_retry_poll();assert(!posted);
 p.remote_port=443;next_tcp=ERR_MEM;__wrap_tcp_output(&p);now_ms+=100;nuwax_network_tx_retry_poll();assert(!posted);
 puts("PASS: bounded delayed ERR_MEM recovery, one queued callback, four retries, unchanged TCP state/other ports, closed PCB, natural recovery, posting failure, route errors and timer wrap");
}
static void fire_timer(void){
 assert(timer_active);now_ms=timer_deadline;timer_active=false;
 timer_args.callback(NULL);
}
static void check_timer(void){
 reset_retry();timer_create_error=ESP_ERR_NO_MEM;nuwax_network_tx_probe_init();
 assert(atomic_load(&s_timer_state)==3 && !timer_active);
 unsigned failed_creates=timer_creates;
 timer_create_error=0;now_ms+=999;nuwax_network_tx_retry_poll();
 assert(timer_creates==failed_creates && atomic_load(&s_timer_state)==3);
 now_ms++;nuwax_network_tx_retry_poll();
 assert(timer_creates==failed_creates+1 && atomic_load(&s_timer_state)==2);
 reset_retry();
 unsigned creates=timer_creates;nuwax_network_tx_probe_init();assert(timer_creates==creates && !timer_active);
 struct tcp_pcb p={.remote_port=8810,.state=ESTABLISHED,.unsent=&test_unsent_segment};
 tcp_active_pcbs=&p;want_pcb=&p;next_tcp=ERR_MEM;__wrap_tcp_output(&p);
 assert(timer_active && timer_deadline==1005);
 unsigned before=tcp_calls;
 // Uploader never polls: timer alone posts; it never calls TCP directly.
 for(unsigned i=0;i<4;i++){
  fire_timer();assert(posted && tcp_calls==before+i && !timer_active);
  // Another producer cannot queue a duplicate callback.
  nuwax_network_tx_retry_poll();deliver();
 }
 assert(tcp_calls==before+4 && now_ms==1075 && !timer_active && !atomic_load(&s_retry_budget));
 // Natural recovery or close before timer firing is safe and becomes idle.
 for(unsigned mode=0;mode<2;mode++){
  reset_retry();tcp_active_pcbs=&p;next_tcp=ERR_MEM;__wrap_tcp_output(&p);
  if(mode==0)p.flags=0;else tcp_active_pcbs=NULL;
  before=tcp_calls;fire_timer();deliver();assert(tcp_calls==before && !timer_active);
 }
 // Timer and caller may both reach the same deadline; only one core callback.
 reset_retry();tcp_active_pcbs=&p;next_tcp=ERR_MEM;__wrap_tcp_output(&p);
 now_ms+=5;nuwax_network_tx_retry_poll();assert(posted);fire_timer();
 next_tcp=ERR_OK;deliver();assert(!timer_active && !atomic_load(&s_retry_budget));
 // Mailbox pressure backs off without a busy timer; other ports remain untouched.
 reset_retry();next_tcp=ERR_MEM;__wrap_tcp_output(&p);post_result=ERR_MEM;
 fire_timer();assert(!posted && timer_active && timer_deadline==now_ms+100);
 post_result=0;fire_timer();next_tcp=ERR_OK;deliver();assert(!timer_active);
 // Persistent mailbox failure consumes the budget and leaves no idle timer.
 reset_retry();next_tcp=ERR_MEM;__wrap_tcp_output(&p);post_result=ERR_MEM;
 for(unsigned i=0;i<4;i++){fire_timer();assert(!posted);}
 assert(!timer_active && !atomic_load(&s_retry_budget));post_result=0;
 // Timer failure keeps the original poll path functional.
 reset_retry();timer_start_error=ESP_ERR_NO_MEM;next_tcp=ERR_MEM;__wrap_tcp_output(&p);
 assert(!timer_active);now_ms+=5;nuwax_network_tx_retry_poll();next_tcp=0;deliver();
 timer_start_error=0;
 // A queued old timer fires early after a caller schedules a later retry.
 reset_retry();next_tcp=ERR_MEM;__wrap_tcp_output(&p);now_ms+=5;nuwax_network_tx_retry_poll();deliver();
 assert(timer_active);fire_timer();assert(!posted && timer_active && timer_deadline==1015);
 fire_timer();next_tcp=ERR_OK;deliver();assert(!timer_active);
 // Wrapped clock deadline also works without any caller polling.
 reset_retry();now_ms=0xfffffffdU;next_tcp=ERR_MEM;__wrap_tcp_output(&p);
 assert(timer_deadline==2);fire_timer();next_tcp=ERR_OK;deliver();assert(!timer_active);
 assert(atomic_load(&s_tcp_core0)>0 && atomic_load(&s_tcp_core1)==0);
 puts("PASS: timer-only recovery during uploader pause, no idle timer, unchanged four-attempt bound, one initialization, alloc/start/mailbox failure fallback, caller/timer races, close/natural recovery and clock wrap");
}
static int preemption_mode;
static void preempt_admission(void){
 before_lock_hook=NULL;
 fire_timer();assert(posted);
 next_tcp=preemption_mode==1?ERR_OK:ERR_MEM;
 deliver();
 if(preemption_mode==1)post_result=ERR_MEM;
}
static void arm_after_failed_round(void){
 if(atomic_load(&s_retry_budget) || atomic_load(&s_retry_queued))return;
 after_unlock_hook=NULL;
 assert(__wrap_tcp_output(want_pcb)==ERR_MEM);
}
static void preempt_initializer(void *object){
 if(object!=&s_timer_state)return;
 before_cas_hook=NULL;
 nuwax_network_tx_probe_init(); // completes 3 -> 1 -> 3 with a new cooldown
}
static void check_preemption(void){
 struct tcp_pcb p={.remote_port=8810,.state=ESTABLISHED,.unsent=&test_unsent_segment};
 for(int mode=1;mode<=2;mode++){
  reset_retry();timer_active=false;tcp_active_pcbs=&p;want_pcb=&p;next_tcp=ERR_MEM;
  __wrap_tcp_output(&p);now_ms+=5;unsigned before=tcp_calls;
  preemption_mode=mode;before_lock_hook=preempt_admission;
  nuwax_network_tx_retry_poll();
  assert(!before_lock_hook && !posted && !atomic_load(&s_retry_queued));
  assert(posts==1 && tcp_calls==before+1);
  if(mode==1){
   assert(!atomic_load(&s_retry_budget) && !timer_active);
   now_ms+=1000;nuwax_network_tx_retry_poll();assert(posts==1);
  }else{
   assert(atomic_load(&s_retry_budget)==3 && atomic_load(&s_retry_next_ms)==1015);
   for(unsigned i=0;i<3;i++){fire_timer();assert(posted);deliver();}
   assert(posts==4 && tcp_calls==before+4 && !atomic_load(&s_retry_budget) && !timer_active);
  }
 }
 // A new natural failure after the final posting failure starts a fresh
 // 5ms round. Old completion must not overwrite it with a 100ms backoff.
 reset_retry();next_tcp=ERR_MEM;__wrap_tcp_output(&p);post_result=ERR_MEM;
 for(unsigned i=0;i<3;i++)fire_timer();
 assert(atomic_load(&s_retry_budget)==1);
 after_unlock_hook=arm_after_failed_round;fire_timer();
 assert(!after_unlock_hook && atomic_load(&s_retry_budget)==4 && !atomic_load(&s_retry_queued));
 assert(atomic_load(&s_retry_next_ms)==now_ms+5 && timer_deadline==now_ms+5);
 post_result=0;fire_timer();next_tcp=ERR_OK;deliver();assert(!timer_active);
 // TCP/IP may finish before the posting caller returns. Caller must leave
 // the completed callback's state alone (also applies to unbound A/B builds).
 reset_retry();next_tcp=ERR_MEM;__wrap_tcp_output(&p);immediate_callback=true;
 for(unsigned i=0;i<4;i++)fire_timer();
 immediate_callback=false;
 assert(posts==4 && !posted && !timer_active && !atomic_load(&s_retry_budget));
 // Independent initialization-failure fixture: a 3->1->3 ABA must not
 // bypass the newly published cooldown or allocate a second timer.
 reset_retry();atomic_store(&s_timer_state,0);timer_create_error=ESP_ERR_NO_MEM;
 nuwax_network_tx_probe_init();now_ms+=1000;
 unsigned creates=timer_creates;before_cas_hook=preempt_initializer;
 nuwax_network_tx_probe_init();
 assert(!before_cas_hook && timer_creates==creates+1 && atomic_load(&s_timer_state)==3);
 for(unsigned i=0;i<100;i++)nuwax_network_tx_retry_poll();
 assert(timer_creates==creates+1);
 // Poll recovery remains usable during the creation cooldown.
 __wrap_tcp_output(&p);now_ms+=5;nuwax_network_tx_retry_poll();assert(posted);
 next_tcp=ERR_OK;deliver();assert(!atomic_load(&s_retry_budget));
 timer_create_error=0;now_ms=atomic_load(&s_timer_init_next_ms);
 nuwax_network_tx_retry_poll();assert(atomic_load(&s_timer_state)==2 && timer_creates==creates+2);
 assert(!timer_active && !critical_depth);
 puts("PASS: preempted admission cannot underflow or retry early; final post failure/new round isolation; callback-before-post-return; init cooldown ABA and automatic allocation recovery; all SDK operations outside critical sections");
}
static struct tcp_pcb snapshot_preempt_pcb;
static void preempt_snapshot_read(const void *object){
 if(object!=&s_sample.lastack)return;
 before_load_hook=NULL;
 replay_sample_tcp(&snapshot_preempt_pcb);
}
static void check_snapshot(void){
 reset_retry();simulate_output=0;next_tcp=ERR_OK;
 s_tcp_previous=(replay_tcp_sample_t){0};atomic_store(&s_sample.version,0);
 replay_tcp_sample_t sample;
 assert(!replay_sample_read(&sample));
 struct tcp_seg a2={.len=400},a1={.next=&a2,.len=1440},u1={.len=80};
 struct tcp_pcb p={.remote_port=8810,.local_port=49152,.state=ESTABLISHED,
  .lastack=0xfffffff0U,.snd_nxt=128,.snd_lbb=200,.unacked=&a1,.unsent=&u1,
  .rto=3,.rtime=2,.nrtx=1,.cwnd=1440,.snd_wnd=65535,.snd_buf=0,.snd_queuelen=4};
 struct tcp_pcb saved=p;struct tcp_seg saved_a1=a1,saved_a2=a2,saved_u1=u1;
 want_pcb=&p;now_ms=0xfffffff0U;__wrap_tcp_output(&p);
 assert(replay_sample_read(&sample) && sample.connection==1 && sample.local_port==49152);
 assert(sample.rto_ms==1500 && sample.rtime_ms==1000 && sample.timer_active && sample.nrtx==1);
 assert(sample.unacked_segments==2 && sample.unacked_bytes==1840 && sample.unsent_segments==1 && sample.unsent_bytes==80);
 assert(!memcmp(&saved,&p,sizeof(p)) && !memcmp(&saved_a1,&a1,sizeof(a1)) && !memcmp(&saved_a2,&a2,sizeof(a2)) && !memcmp(&saved_u1,&u1,sizeof(u1)));
 // Normal sequence/time wrap remains one connection; queue walks are throttled.
 now_ms=0x20;p.lastack=0x10;p.unacked=NULL;p.rtime=-1;__wrap_tcp_output(&p);
 assert(replay_sample_read(&sample) && sample.connection==1 && sample.ack_observed_bytes==32 && sample.ack_advances==1);
 assert(sample.ack_progress_ms==0x20 && !sample.timer_active && sample.rtime_ms==0);
 assert(sample.queue_sample_ms==0xfffffff0U && sample.unacked_segments==2);
 now_ms=0x53;__wrap_tcp_output(&p);assert(replay_sample_read(&sample) && sample.unacked_segments==2);
 now_ms=0x54;__wrap_tcp_output(&p);assert(replay_sample_read(&sample) && sample.unacked_segments==0 && sample.queue_sample_ms==0x54);
 // RTO entry refreshes queues before 100 ms; repeated RTO samples do not.
 p.flags=TF_RTO;p.unacked=&a2;now_ms++;__wrap_tcp_output(&p);
 assert(replay_sample_read(&sample) && sample.in_rto && sample.unacked_bytes==400 && sample.queue_sample_ms==0x55);
 p.unacked=NULL;now_ms++;__wrap_tcp_output(&p);assert(replay_sample_read(&sample) && sample.unacked_bytes==400);
 // Port/SYN changes reset the observed ACK baseline; another port cannot publish.
 p.local_port++;now_ms++;__wrap_tcp_output(&p);
 assert(replay_sample_read(&sample) && sample.connection==2 && !sample.ack_observed_bytes && !sample.ack_advances);
 p.remote_port=443;p.lastack=10000;now_ms+=1000;__wrap_tcp_output(&p);
 replay_tcp_sample_t other;assert(replay_sample_read(&other) && !memcmp(&sample,&other,sizeof(sample)));
 p.remote_port=8810;p.state=SYN_SENT;__wrap_tcp_output(&p);
 assert(replay_sample_read(&sample) && sample.connection==3);
 p.state=ESTABLISHED;p.lastack++;__wrap_tcp_output(&p);
 assert(replay_sample_read(&sample) && sample.connection==3 && sample.ack_observed_bytes==1);
 p.lastack-=100;__wrap_tcp_output(&p);assert(replay_sample_read(&sample) && sample.connection==4 && !sample.ack_observed_bytes);
 // Corrupt/oversized list cannot create unbounded callback work; count is partial.
 struct tcp_seg large[REPLAY_QUEUE_SCAN_LIMIT+1];
 for(unsigned i=0;i<REPLAY_QUEUE_SCAN_LIMIT+1;i++)large[i]=(struct tcp_seg){.next=i<REPLAY_QUEUE_SCAN_LIMIT?&large[i+1]:NULL,.len=20};
 p.unacked=large;now_ms+=100;__wrap_tcp_output(&p);
 assert(replay_sample_read(&sample) && sample.queue_truncated && sample.unacked_segments==REPLAY_QUEUE_SCAN_LIMIT && sample.unacked_bytes==REPLAY_QUEUE_SCAN_LIMIT*20);
 // A write between fields forces a retry, never a mixed-generation snapshot.
 snapshot_preempt_pcb=p;snapshot_preempt_pcb.local_port=60000;snapshot_preempt_pcb.lastack=42;snapshot_preempt_pcb.cwnd=9999;
 before_load_hook=preempt_snapshot_read;assert(replay_sample_read(&sample));
 assert(!before_load_hook && sample.local_port==60000 && sample.lastack==42 && sample.cwnd==9999 && sample.connection==5);
 // A preempted publisher cannot make the logging task spin indefinitely.
 atomic_fetch_add(&s_sample.version,1);assert(!replay_sample_read(&sample));atomic_fetch_add(&s_sample.version,1);
 unsigned before=tcp_calls;now_ms+=1000;nuwax_network_tx_probe_log();assert(tcp_calls==before);
 assert(replay_sample_read(&sample) && now_ms-sample.sample_ms==1000);
 puts("PASS: read-only TCP sample, observed ACK/connection baseline, sequence/time wrap, 100ms queue sampling/RTO entry, bounded queue traversal, stale age, other-port isolation, coherent preempted snapshot and bounded reader");
}

static void input_packet(unsigned source,unsigned dest,unsigned length){
 uint8_t bytes[20]={source>>8,source&255,dest>>8,dest&255};
 struct pbuf packet={bytes,length};struct netif iface={42};
 expected_packet=&packet;expected_input=&iface;
 unsigned before=input_calls;__wrap_tcp_input(&packet,&iface);assert(input_calls==before+1);
}
static void check_receive_observer(void){
 reset_retry();simulate_output=0;next_tcp=0;
 memset(&s_tcp_previous,0,sizeof(s_tcp_previous));atomic_store(&s_sample.version,0);
 struct tcp_pcb p={.remote_port=8810,.local_port=62000,.state=ESTABLISHED,.lastack=1000,
   .snd_lbb=2000,.snd_nxt=2000,.snd_buf=45080,.snd_wnd=65535,.cwnd=23040};
 tcp_active_pcbs=&p;want_pcb=&p;input_mode=0;now_ms=100;
 replay_sample_tcp(&p);now_ms=400;input_mode=1;input_ack=1500;input_packet(8810,62000,20);
 replay_tcp_sample_t sample;assert(replay_sample_read(&sample));
 assert(sample.lastack==1500 && sample.ack_observed_bytes==500 && sample.ack_gap_peak_ms==302 && sample.ack_gap_at_ms==402);
 assert(sample.ack_gap_cwnd==23040 && atomic_load(&s_tcp_input_max_us)==2000);
 // A bad-checksum or duplicate raw ACK does not advance the SDK PCB or our sample.
 unsigned advances=sample.ack_advances;input_mode=0;input_ack=1999;input_packet(8810,62000,20);
 assert(replay_sample_read(&sample) && sample.lastack==1500 && sample.ack_advances==advances);
 // Quiet gaps between files must not inflate the measured outstanding-data gap.
 p.lastack=p.snd_lbb;replay_sample_tcp(&p);now_ms+=100000;
 p.snd_lbb+=1000;p.snd_nxt=p.snd_lbb;replay_sample_tcp(&p);now_ms+=50;
 input_mode=1;input_ack=p.snd_lbb;input_packet(8810,62000,20);
 assert(replay_sample_read(&sample) && sample.ack_gap_peak_ms==302);
 p.snd_lbb+=1000;p.snd_nxt=p.snd_lbb;replay_sample_tcp(&p);
 p.flags=TF_RTO;p.nrtx=1;p.dupacks=3;replay_sample_tcp(&p);replay_sample_tcp(&p);
 now_ms+=1600;input_ack=p.snd_lbb;input_packet(8810,62000,20);
 assert(replay_sample_read(&sample) && sample.rto_entries==1 && sample.dupacks_peak==3 && sample.ack_gap_peak_ms==1602 && sample.ack_gap_in_rto==1 && sample.ack_gap_nrtx==1);
 // Non-replay traffic and short headers keep the exact original SDK call but no probe work.
 unsigned version=atomic_load(&s_sample.version),reads=radio_reads,packets=atomic_load(&s_tcp_inputs);
 input_mode=0;input_packet(443,62000,20);input_packet(8810,62000,19);input_packet(8810,62001,20);
 assert(atomic_load(&s_sample.version)==version && radio_reads==reads && atomic_load(&s_tcp_inputs)==packets+1);
 // No dangling PCB access after SDK processing closes a connection.
 input_mode=2;input_packet(8810,62000,20);assert(!tcp_active_pcbs && atomic_load(&s_sample.version)==version);
 // A matching local port on multiple endpoints is ambiguous, never mislabeled.
 struct tcp_pcb duplicate=p;p.next=&duplicate;tcp_active_pcbs=&p;input_mode=0;
 unsigned ambiguous=atomic_load(&s_tcp_input_ambiguous);input_packet(8810,62000,20);
 assert(atomic_load(&s_tcp_input_ambiguous)==ambiguous+1 && atomic_load(&s_sample.version)==version);
 p.next=&p;input_packet(8810,62001,20); // bounded traversal despite corrupt/cyclic list
 assert(atomic_load(&s_tcp_input_ambiguous)==ambiguous+2);
 p.next=NULL;tcp_active_pcbs=&p;
 // Millisecond wrap and sequence wrap preserve gap and byte arithmetic.
 memset(&s_tcp_previous,0,sizeof(s_tcp_previous));p.lastack=UINT32_MAX-100;p.snd_lbb=200;p.snd_nxt=200;
 now_ms=UINT32_MAX-200;replay_sample_tcp(&p);now_ms=100;input_mode=1;input_ack=200;input_packet(8810,62000,20);
 assert(replay_sample_read(&sample) && sample.ack_gap_peak_ms==303 && sample.ack_observed_bytes==301);
 unsigned before=radio_reads;nuwax_network_tx_probe_log();assert(radio_reads==before+2);
 radio_error=ESP_ERR_INVALID_STATE;nuwax_network_tx_probe_log();radio_error=0;
 tcp_active_pcbs=NULL;
 puts("PASS: actual TCP input wrapper: exactly one original call, consumed pbuf/closed PCB safety, validated ACK-only progress, quiet-gap exclusion, RTO transitions, bounded/ambiguous lookup, other-port isolation, clock/sequence wrap and read-only radio failure");
}
int main(void){
 esp_netif_t net={0};unsigned char data[1536];memset(data,0x93,sizeof(data));
 want_net=&net;want_data=data;want_buf=&net;want_len=sizeof(data);
 int errors[]={ESP_OK,ESP_ERR_NO_MEM,-1,0x1234};
 for(unsigned i=0;i<4;i++){next_net=errors[i];assert(__wrap_esp_netif_transmit_wrap(&net,data,sizeof(data),&net)==next_net);}
 for(unsigned i=0;i<sizeof(data);i++)assert(data[i]==0x93);
 assert(net_calls==4 && atomic_load(&s_netif_calls)==4 && atomic_load(&s_netif_nomem)==1 && atomic_load(&s_netif_other)==2);
 struct tcp_pcb p={.remote_port=8810,.snd_wnd=46080,.cwnd=5760,.snd_buf=1440,.snd_queuelen=29,.nrtx=1};
 struct tcp_pcb saved=p;want_pcb=&p;next_tcp=0;
 assert(__wrap_tcp_output(&p)==0 && !memcmp(&p,&saved,sizeof(p)));
 p.snd_wnd=0;p.flags=TF_RTO;p.nrtx=2;next_tcp=-1;
 assert(__wrap_tcp_output(&p)==-1);
 assert(atomic_load(&s_tcp_calls)==2 && atomic_load(&s_tcp_errors)==1 && atomic_load(&s_zero_window)==1 && atomic_load(&s_rto_samples)==1 && atomic_load(&s_nrtx_peak)==2);
 p.remote_port=443;assert(__wrap_tcp_output(&p)==-1);assert(atomic_load(&s_tcp_calls)==2);
 want_pcb=NULL;assert(__wrap_tcp_output(NULL)==-1);assert(tcp_calls==4);
 nuwax_network_tx_probe_log();assert(net_calls==4 && tcp_calls==4);
 check_recovery();
 check_timer();
 check_preemption();
 check_snapshot();
 check_receive_observer();
 puts("PASS: exact arguments/results, one original call, source/PCB preservation, error classification, replay-port filtering, null PCB and read-only diagnostics");
}
'''
(out/'check.c').write_text(source, encoding='utf-8')
subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-I'+str(root/'main'),str(out/'check.c'),'-o',str(out/'check.exe')],check=True)
subprocess.run([str(out/'check.exe')],check=True)
if args.build:
    elf=args.build/'NuwaxAIPartner.elf'
    config=(args.build/'config/sdkconfig.h').read_text(encoding='utf-8')
    settings={'ESP_WIFI_CACHE_TX_BUFFER_NUM':args.wifi_cache,'ESP_WIFI_STATIC_TX_BUFFER_NUM':6,
              'ESP_WIFI_STATIC_RX_BUFFER_NUM':6,'ESP_WIFI_TX_BA_WIN':6,
              'LWIP_TCP_SND_BUF_DEFAULT':46080,'SPIRAM_TRY_ALLOCATE_WIFI_LWIP':1}
    for key,value in settings.items(): assert f'#define CONFIG_{key} {value}\n' in config,key
    core=int(re.search(r'^#define CONFIG_LWIP_TCPIP_TASK_AFFINITY (\S+)',config,re.M).group(1),0)
    assert core==args.tcpip_core
    settings['LWIP_TCPIP_TASK_AFFINITY']=core
    tool=Path('C:/Espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin/xtensa-esp32s3-elf-objdump.exe')
    def reachable_assembly(names):
        # Xtensa padding after an unconditional jump can desynchronize a
        # linear sweep. Decode from real branch entries, never from padding.
        nm=subprocess.check_output([str(tool.with_name('xtensa-esp32s3-elf-nm.exe')),'-S',str(elf)],text=True)
        bounds={m[3]:(int(m[0],16),int(m[0],16)+int(m[1],16)) for line in nm.splitlines()
                if len(m:=line.split())==4 and m[2].lower()=='t'}
        result=[]
        for name in names:
            start,end=bounds[name];pending=[start];seen=set()
            while pending:
                pc=pending.pop()
                if pc in seen or not start<=pc<end: continue
                block=subprocess.check_output([str(tool),'-d',f'--start-address={pc}',f'--stop-address={end}',str(elf)],text=True)
                for line in block.splitlines():
                    match=re.match(r'\s*([0-9a-f]+):\s+([0-9a-f]+)\s+([\w.]+)\s*(.*)',line)
                    if not match:continue
                    addr=int(match[1],16)
                    if addr!=pc or addr in seen:break
                    seen.add(addr);result.append(line);pc+=len(match[2])//2
                    op=match[3]
                    if op.startswith('b') or op=='j':
                        target=re.search(r'\b([0-9a-f]+)\s+<',match[4])
                        if target:pending.append(int(target[1],16))
                    if op in ('j','jx','retw','retw.n','ret','ret.n'):break
        return '\n'.join(result)
    # Relocations in SDK objects prove network callers use wrapped symbols;
    # final ELF literal targets prove wrappers call the original functions.
    checks={}
    for caller,expected in [('__wrap_tcp_input','tcp_input'),('__wrap_tcp_output','tcp_output'),('__wrap_esp_netif_transmit_wrap','esp_netif_transmit_wrap')]:
        asm=subprocess.check_output([str(tool),'-d','--disassemble='+caller,str(elf)],text=True)
        assert '<'+caller+'>:' in asm and '<'+expected+'>' in asm,caller
        (out/(caller+'.txt')).write_text(asm,encoding='utf-8')
        checks[caller]=expected
    for caller,expected in [('ip4_input','__wrap_tcp_input'),('lwip_netconn_do_writemore','__wrap_tcp_output'),('low_level_output','__wrap_esp_netif_transmit_wrap')]:
        symbols=subprocess.check_output([str(tool.with_name('xtensa-esp32s3-elf-nm.exe')),str(elf)],text=True)
        names=[line.split()[-1] for line in symbols.splitlines() if line.split() and
               (line.split()[-1]==caller or line.split()[-1].startswith(caller+'$'))]
        assert names,caller
        asm='\n'.join(subprocess.check_output([str(tool),'-d','--disassemble='+name,str(elf)],text=True) for name in names)
        assert '<'+expected+'>' in asm,caller
        (out/(caller+'.txt')).write_text(asm,encoding='utf-8')
        checks[caller]=expected
    for caller,expected in [('replay_output_retry','__wrap_tcp_output'),
                            ('nuwax_network_tx_retry_poll','tcpip_try_callback'),
                            ('replay_retry_timer','nuwax_network_tx_retry_poll'),
                            ('replay_retry_schedule','esp_timer_start_once'),
                            ('nuwax_network_tx_probe_init','esp_timer_create'),
                            ('connect_transport_tx_locked','nuwax_network_tx_probe_init'),
                            ('send_all_until','nuwax_network_tx_retry_poll'),
                            ('replay_wait_ack','nuwax_network_tx_retry_poll')]:
        names=[line.split()[-1] for line in symbols.splitlines() if line.split() and
               (line.split()[-1]==caller or line.split()[-1].startswith(caller+'$'))]
        if not names and caller=='connect_transport_tx_locked':
            # Single static caller can be inlined; still verify initialization
            # is reached by the real connect entry point in the final image.
            names=['nuwax_ws_connect']
        assert names,caller
        asm='\n'.join(subprocess.check_output([str(tool),'-d','--disassemble='+name,str(elf)],text=True) for name in names)
        if '<'+expected+'>' not in asm:
            asm=reachable_assembly(names)
        assert '<'+expected+'>' in asm,(caller,expected)
        (out/(caller+'.txt')).write_text(asm,encoding='utf-8')
        checks[caller]=expected
    binary=(args.build/'NuwaxAIPartner.bin').read_bytes()
    assert binary[48:80].split(b'\0')[0]==args.expected_version.encode('ascii')
    assert b'SD network profile revision=133' in binary
    assert b'recovery=locked_timer_retry' in binary
    assert b'state=locked' in binary
    assert b'layout=batch_first' in binary
    assert b'ack trace revision=148' in binary and b'offline writable trace revision=148' in binary
    assert b'ack_observed_bytes=' in binary and b'queue_age_ms=' in binary
    assert b'snapshot unavailable=no_sample_or_publish_busy' in binary
    for symbol in ['replay_output_retry','nuwax_network_tx_retry_poll','replay_retry_timer','nuwax_network_tx_probe_init']:
        assert symbol in symbols,symbol
    report=dict(version=args.expected_version,settings=settings,linkedCalls=checks,
                elfSha256=hashlib.sha256(elf.read_bytes()).hexdigest(),hardwareThroughputValidated=False,
                hostTimerRecovery=dict(noUploaderPolling=True,retryTimesMs=[5,15,35,75],
                                      maxAttempts=4,permanentMailboxFailureStops=True,
                                      preemptedAdmission=True,lateRoundIsolation=True,
                                      callbackBeforePostReturn=True,initFailureRecovers=True,
                                      initCooldownABA=True,sdkCallsOutsideCriticalSection=True),
                hostTcpDiagnostics=dict(observationalOnly=True,processedInputObserved=True,pbufAndClosedPcbSafe=True,quietGapExcluded=True,radioReadOnly=True,queueSampleIntervalMs=100,
                                        queueScanLimit=128,sequenceAndTimeWrap=True,
                                        boundedCoherentSnapshot=True,pcbAndPayloadUnchanged=True,
                                        otherPortsUntouched=True))
    (out/'report.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print('PASS: final firmware identity, effective network settings, real network callers -> probes -> original SDK functions')
