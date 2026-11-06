"""Execute production TLS profiling/policy and the SD recovery admission race.

Host doubles prove isolation, framing geometry and lifecycle behavior, not
hardware throughput. --baseline-spool demonstrates the old admission failure.
"""
from pathlib import Path
import argparse
import re
import subprocess

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--baseline-spool', type=Path)
args = parser.parse_args()
out = root/'build-host-checks/sd-formal-r188'
out.mkdir(parents=True, exist_ok=True)

def run(name, code, expected=0):
    src = out/(name+'.c'); exe = out/(name+'.exe')
    src.write_text(code, encoding='utf-8')
    subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror',
        '-I'+str(root/'main'), '-I'+str(root/'main/audio_modes/meeting/host_stubs'),
        str(src),'-o',str(exe)],check=True)
    result = subprocess.run([str(exe)],capture_output=True,text=True,timeout=30)
    print(result.stdout, end='')
    if result.returncode != expected:
        print(result.stderr)
        raise AssertionError((name,result.returncode,expected))

module = (root/'main/tls_record_metrics.c').read_text(encoding='utf-8')
module = re.sub(r'^#include.*$', '', module, flags=re.M)
run('tls', r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdatomic.h>
#include "ws_send_metrics.h"
#include "tls_cipher_policy.h"
#include "offline_replay_policy.h"
#define CONFIG_MBEDTLS_CHACHAPOLY_C 1
typedef int psa_status_t;
typedef unsigned mbedtls_svc_key_id_t;
typedef unsigned psa_algorithm_t;
static uintptr_t task=1;
static int64_t now;
static void *xTaskGetCurrentTaskHandle(void) { return (void*)task; }
static int64_t esp_timer_get_time(void) { errno=EDOM; return now; }
static int suites[]={0xc030,0xcca8,0xc02f,0xcca9,0x1303,0};
static const int *esp_tls_get_ciphersuites_list(void) { return suites; }
static uint8_t nonce[12],ad[5],plain[64],cipher[100];
static int real_result;
psa_status_t __real_psa_aead_encrypt(mbedtls_svc_key_id_t key,psa_algorithm_t alg,
 const uint8_t *n,size_t nl,const uint8_t *a,size_t al,const uint8_t *p,size_t pl,
 uint8_t *c,size_t cl,size_t *written) {
 assert(key==7 && alg==9 && n==nonce && nl==sizeof nonce && a==ad && al==sizeof ad);
 assert(p==plain && pl==sizeof plain && c==cipher && cl==sizeof cipher);
 now+=700;errno=EAGAIN;*written=80;return real_result;
}
int __real_mbedtls_net_send(void *ctx,const unsigned char *buf,size_t len) {
 assert(ctx==cipher && buf==cipher && len==80);now+=90;errno=EAGAIN;
 return real_result ? real_result : 80;
}
''' + module + r'''
static void call_aead(void) {
 size_t written=0;
 assert(__wrap_psa_aead_encrypt(7,9,nonce,sizeof nonce,ad,sizeof ad,plain,sizeof plain,
     cipher,sizeof cipher,&written)==real_result);
 assert(errno==EAGAIN && written==80);
}
int main(void) {
 int ordered[6]={0};
 assert(nuwax_tls_order_suites(suites,ordered,6));
 const int expected[]={0xcca8,0xcca9,0x1303,0xc02f,0xc030,0};
 assert(!memcmp(ordered,expected,sizeof expected));
 assert(!nuwax_tls_order_suites(suites,ordered,5));
 assert(!nuwax_tls_order_suites(NULL,ordered,6));
 int empty[]={0};assert(!nuwax_tls_order_suites(empty,ordered,6));
 int legacy[]={0xc030,0xc02f,0};assert(nuwax_tls_order_suites(legacy,ordered,6));
 const int legacy_expected[]={0xc02f,0xc030,0};
 assert(!memcmp(ordered,legacy_expected,sizeof legacy_expected));
 int mixed[]={0x009d,0xc030,0xc02b,0x1302,0x1301,0xcca8,0xc02f,0};
 int ranked[8];
 const int mixed_expected[]={0xcca8,0xc02b,0x1301,0xc02f,0x009d,0xc030,0x1302,0};
 assert(nuwax_tls_order_suites(mixed,ranked,8));
 assert(!memcmp(ranked,mixed_expected,sizeof mixed_expected));
 const int *persistent=nuwax_tls_software_suites();
 assert(persistent && !memcmp(persistent,expected,sizeof expected));
 assert(nuwax_tls_software_suites()==persistent);
 nuwax_ws_send_metrics_t a={0},b={0};
 assert(!nuwax_tls_metrics_begin(NULL));call_aead();
 assert(nuwax_tls_metrics_begin(&a));
 call_aead();assert(__wrap_mbedtls_net_send(cipher,cipher,80)==80 && errno==EAGAIN);
 assert(a.tls_aead_us==700 && a.tls_aead_bytes==64 && a.tls_aead_calls==1);
 assert(a.tls_bio_us==90 && a.tls_bio_bytes==80 && a.tls_bio_calls==1);
 task=2;call_aead();assert(__wrap_mbedtls_net_send(cipher,cipher,80)==80);
 assert(!nuwax_tls_metrics_begin(&b) && b.tls_profile_skipped==1);
 nuwax_tls_metrics_end(); // A different task cannot release the owner.
 task=1;call_aead();assert(a.tls_aead_us==1400 && a.tls_aead_calls==2);
 real_result=-99;call_aead();assert(__wrap_mbedtls_net_send(cipher,cipher,80)==-99 && errno==EAGAIN);
 assert(a.tls_bio_bytes==80 && a.tls_bio_calls==2 && a.tls_aead_us==2100);
 nuwax_tls_metrics_end();task=2;assert(nuwax_tls_metrics_begin(&b));
 real_result=0;call_aead();nuwax_tls_metrics_end();
 assert(b.tls_aead_us==700 && a.tls_aead_us==2100 && !s_metrics);
 assert(a.tls_profiled_calls==1 && b.tls_profiled_calls==1);
 assert(a.tls_aead_max_us==700 && a.tls_bio_max_us==90);
 puts("PASS TLS: all cipher fallbacks retained; task isolation, contention, error/errno/output preservation and scope reuse");
}
''')

source = (args.baseline_spool or root/'main/offline_audio_spool.c').read_text(encoding='utf-8-sig')
start = source.index('esp_err_t nuwax_offline_audio_recover_if_idle(')
end = source.index('\n}',start)+2
function = source[start:end]
run('recovery',r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "esp_err.h"
#include "offline_audio_policy.h"
#define CONFIG_NUWAX_SD_SESSION_ENABLE 1
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGI(...) ((void)0)
typedef uint32_t TickType_t;
static struct {
 atomic_bool active,closing,writer_busy,degraded,writer_error,media_recovery_requested;
 atomic_uint ready_count;
 void *file;
 unsigned admission_lock,writer_gate,lock,queue;
 uint32_t media_recovery_deadline_ms,next_index,writer_retry_deadline_ms,writer_retry_backoff_ms;
 uint64_t used_bytes;
 bool writer_fault_latched,media_was_ready;
 int media_recovery_last_error;
} s;
static atomic_bool s_media_ready,s_directory_dirty,s_card_pending_upload,s_quarantine_recheck_pending;
static const char *s_writer_fault_stage __attribute__((unused));
static int s_writer_fault_errno __attribute__((unused));
static uint32_t s_card_boot_id;
static unsigned locks,probes,recoveries,queued,injection,catalog_scans,prefix_scans,invalidations;
static bool scene,capture,online,shutdown;
static int64_t now;
static bool enabled(void){return true;}
static bool runtime_initialized(void){return true;}
static esp_err_t nuwax_offline_audio_init(void){return ESP_OK;}
static bool nuwax_sd_session_active(void){return scene;}
static bool healthy_capture_defers_media_probe(void){return capture;}
static bool nuwax_offline_audio_replay_running(void){return false;}
static bool nuwax_power_ctrl_shutdown_preparing(void){return shutdown;}
static int64_t esp_timer_get_time(void){return now;}
static TickType_t recovery_remaining_ticks(bool unlimited,int64_t deadline){(void)unlimited;(void)deadline;return 10;}
static uint32_t recovery_remaining_ms(bool unlimited,int64_t deadline){(void)unlimited;(void)deadline;return 10;}
static int xSemaphoreTake(unsigned lock,unsigned ticks){
 (void)ticks;assert(!(locks&lock));locks|=lock;
 // The scene wins s.lock after recovery's optimistic check and before it can
 // acquire all storage locks. No FILE/worker is required for this reservation.
 if(lock==s.admission_lock && injection==1)scene=true;
 if(lock==s.admission_lock && injection==2)capture=true;
 return pdTRUE;
}
static void xSemaphoreGive(unsigned lock){assert(locks&lock);locks&=~lock;}
static unsigned uxQueueMessagesWaiting(unsigned q){assert(q==8);return queued;}
static void notify_writer_task(void){}
static void vTaskDelay(TickType_t ticks){now+=ticks*1000;queued=0;}
static bool nuwax_sd_card_is_mounted(void){return online;}
static int nuwax_sd_card_probe(void){probes++;return ESP_OK;}
static int nuwax_sd_card_recover(uint32_t timeout){(void)timeout;recoveries++;return ESP_OK;}
static void nuwax_sd_card_mark_io_fault(esp_err_t err){(void)err;}
static int ensure_directory(void){return ESP_OK;}
static int prepare_media_space(void){return ESP_OK;}
static int recover_open_segments(void){prefix_scans++;return ESP_OK;}
static int sd_audio_catalog_recover(uint32_t boot){(void)boot;catalog_scans++;return ESP_OK;}
static int sd_audio_catalog_has_pending(bool *pending){*pending=false;return ESP_OK;}
int sd_audio_catalog_prepare_remount(void){return ESP_OK;}
static int refresh_capture_capacity_locked(void){return ESP_OK;}
static void reset_media_recovery_locked(bool changed){if(changed)invalidations++;s.media_was_ready=true;}
static void schedule_media_recovery_locked(int err){(void)err;}
static uint32_t offline_now_ms(void){return (uint32_t)(now/1000);}
static bool offline_deadline_elapsed(uint32_t a,uint32_t b){return (int32_t)(a-b)>=0;}
static uint32_t __attribute__((unused)) next_segment_index(void){return 1;}
static esp_err_t __attribute__((unused)) refresh_next_segment_index(void){return ESP_OK;}
static uint64_t scan_used_bytes(void){return 0;}
static uint32_t scan_ready_count(void){return 0;}
''' + function + r'''
static void reset(void){
 memset(&s,0,sizeof s);s.admission_lock=1;s.writer_gate=2;s.lock=4;s.queue=8;
 s.media_was_ready=true;catalog_scans=prefix_scans=invalidations=0;
 locks=probes=recoveries=queued=injection=0;scene=capture=shutdown=false;online=true;now=0;
 s_media_ready=true;s_directory_dirty=false;
}
int main(void){
 for(unsigned phase=0;phase<2;phase++){
  reset();injection=phase==0?0:1;scene=phase==0;
  const int result=nuwax_offline_audio_recover_if_idle(100);
  if(result!=ESP_ERR_NOT_FINISHED || probes || recoveries){
   puts("REPRO: scene acquired SD during recovery admission; the recovery still touched the card");return 42;
  }
  assert(!locks && !s.media_recovery_requested && s_media_ready);
 }
 reset();injection=2;assert(nuwax_offline_audio_recover_if_idle(100)==ESP_ERR_NOT_FINISHED);
 assert(!locks && !probes && !s.media_recovery_requested);
 reset();assert(nuwax_offline_audio_recover_if_idle(100)==ESP_OK && probes==1 && !locks);
 reset();online=false;assert(nuwax_offline_audio_recover_if_idle(100)==ESP_OK && recoveries==1 && !locks);
 reset();queued=2;assert(nuwax_offline_audio_recover_if_idle(100)==ESP_OK && probes==1 && !locks);
 reset();s_directory_dirty=true;
 assert(nuwax_offline_audio_recover_if_idle(100)==ESP_OK && !locks);
 assert(!catalog_scans && !prefix_scans && !invalidations && !s_directory_dirty);
 for(unsigned fault=0;fault<4;fault++){
  reset();s_directory_dirty=true;
  if(fault==0)s.degraded=true;
  if(fault==1)s.writer_error=true;
  if(fault==2)s.media_was_ready=false;
  if(fault==3)s.media_recovery_last_error=ESP_FAIL;
  assert(nuwax_offline_audio_recover_if_idle(100)==ESP_OK && !locks);
  assert(catalog_scans==1 && prefix_scans==1 && invalidations==1);
 }
 puts("PASS SD recovery: existing/newly reserved scene cannot be scanned; capture race, idle probe, remount and queue drain retain lock/gate balance");
}
''',expected=42 if args.baseline_spool else 0)
