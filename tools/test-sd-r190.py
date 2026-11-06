"""Exercise production scoped-crypto/fallback and diagnostic admission paths.

The SDK crypto itself is checked on device against a known answer and the
software driver. These host doubles check isolation, bounds and recovery.
"""
from pathlib import Path
import re
import subprocess

root = Path(__file__).resolve().parents[1]
out = root / 'build-host-checks/sd-r190'
out.mkdir(parents=True, exist_ok=True)

def run(name, source):
    src, exe = out / (name + '.c'), out / (name + '.exe')
    src.write_text(source, encoding='utf-8')
    subprocess.run(['gcc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(root / 'main'), str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=30)

module = (root / 'main/tls_archive_crypto.c').read_text(encoding='utf-8')
module = re.sub(r'^#include.*$', '', module, flags=re.M)
run('crypto', r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "ws_send_metrics.h"
#define CONFIG_NUWAX_SD_TLS_SCOPED_AES 1
#define CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN 8192
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_SPIRAM 4
#define MALLOC_CAP_DMA 8
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_AES_ENCRYPT 1
#define PSA_ALG_GCM 7
#define PSA_KEY_TYPE_AES 9
#define PSA_SUCCESS 0
typedef int psa_status_t;
typedef unsigned psa_algorithm_t;
typedef struct { unsigned type,bits; } psa_key_attributes_t;
typedef struct { uint8_t key[32]; size_t bytes; } esp_gcm_context;
static unsigned psa_get_key_type(const psa_key_attributes_t *a){return a->type;}
static unsigned psa_get_key_bits(const psa_key_attributes_t *a){return a->bits;}
static size_t internal=65536,largest=16384;
static unsigned allocations, frees, hw_calls, sw_calls, dma_frees;
static bool fail_alloc, fail_hw, mismatch;
static nuwax_ws_send_metrics_t metrics;
static bool archive_scope=true;
static nuwax_ws_send_metrics_t *nuwax_tls_archive_metrics(void){return archive_scope?&metrics:NULL;}
static size_t heap_caps_get_free_size(unsigned caps){(void)caps;return internal;}
static size_t heap_caps_get_largest_free_block(unsigned caps){(void)caps;return largest;}
static void *heap_caps_calloc(size_t n,size_t s,unsigned caps){(void)caps;if(fail_alloc)return NULL;allocations++;return calloc(n,s);}
/* Legacy no-workspace profile; the real SDK/staged path is covered by r200. */
static void *heap_caps_malloc(size_t n,unsigned caps){(void)n;(void)caps;return NULL;}
static void *heap_caps_aligned_alloc(size_t a,size_t s,unsigned caps){(void)a;(void)caps;if(fail_alloc)return NULL;allocations++;return malloc(s);}
static void heap_caps_free(void *p){if(p){frees++;free(p);}}
static void mbedtls_platform_zeroize(void *p,size_t n){memset(p,0,n);}
static void esp_crypto_shared_gdma_free(void){dma_frees++;}
static void esp_aes_gcm_init(esp_gcm_context *c){memset(c,0,sizeof *c);}
static void esp_aes_gcm_free(esp_gcm_context *c){memset(c,0,sizeof *c);}
static int esp_aes_gcm_setkey(esp_gcm_context *c,int cipher,const uint8_t *k,unsigned bits){
 (void)cipher;c->bytes=bits/8;memcpy(c->key,k,c->bytes);return 0;
}
static int esp_aes_gcm_starts(esp_gcm_context *c,int m,const uint8_t *iv,size_t n){(void)c;(void)m;(void)iv;(void)n;assert(!"staged path requires real SDK test");return -1;}
static int esp_aes_gcm_update_ad(esp_gcm_context *c,const uint8_t *a,size_t n){(void)c;(void)a;(void)n;return -1;}
static int esp_aes_gcm_update(esp_gcm_context *c,const uint8_t *a,size_t n,uint8_t *o,size_t cap,size_t *w){(void)c;(void)a;(void)n;(void)o;(void)cap;(void)w;return -1;}
static int esp_aes_gcm_finish(esp_gcm_context *c,uint8_t *o,size_t cap,size_t *w,uint8_t *t,size_t tn){(void)c;(void)o;(void)cap;(void)w;(void)t;(void)tn;return -1;}
static const uint8_t kat[32]={
 0x03,0x88,0xda,0xce,0x60,0xb6,0xa3,0x92,0xf3,0x28,0xc2,0xb9,0x71,0xb2,0xfe,0x78,
 0xab,0x6e,0x47,0xd4,0x2c,0xec,0x13,0xbd,0xf5,0x3a,0x67,0xb2,0x12,0x57,0xbd,0xdf};
static void fake_encrypt(const uint8_t *key,size_t kb,const uint8_t *input,size_t n,uint8_t *output,uint8_t *tag){
 for(size_t i=0;i<n;i++)output[i]=input[i]^key[i%kb]^(uint8_t)i;
 memset(tag,0xa5,16);
}
static int esp_aes_gcm_crypt_and_tag(esp_gcm_context *c,int mode,size_t n,const uint8_t *iv,
 size_t ivn,const uint8_t *aad,size_t ad,const uint8_t *input,uint8_t *output,size_t tn,uint8_t *tag){
 (void)mode;(void)iv;(void)ivn;(void)aad;assert(tn==16);hw_calls++;
 if(fail_hw){memset(output,0xde,n);return -1;}
 if(n==16 && !ad && !c->key[0]){memcpy(output,kat,16);memcpy(tag,kat+16,16);}
 else fake_encrypt(c->key,c->bytes,input,n,output,tag);
 if(mismatch && ad)output[0]^=1;
 return 0;
}
psa_status_t __real_mbedtls_psa_aead_encrypt(const psa_key_attributes_t *a,const uint8_t *key,
 size_t kb,psa_algorithm_t alg,const uint8_t *iv,size_t ivn,const uint8_t *aad,size_t ad,
 const uint8_t *input,size_t n,uint8_t *output,size_t cap,size_t *written){
 (void)a;(void)alg;(void)iv;(void)ivn;(void)aad;(void)ad;sw_calls++;
 if(cap<n+16)return -7;
 fake_encrypt(key,kb,input,n,output,output+n);*written=n+16;return 0;
}
''' + module + r'''
static void send_record(unsigned bits,unsigned alg,bool in_place){
 uint8_t key[32],iv[12]={0},aad[13]={0},plain[8193],expected[8209],actual[8209];
 memset(key,3,sizeof key);memset(plain,0x4c,sizeof plain);memcpy(actual,plain,sizeof plain);
 fake_encrypt(key,bits/8,plain,sizeof plain,expected,expected+sizeof plain);
 psa_key_attributes_t a={PSA_KEY_TYPE_AES,bits};size_t written=0;
 assert(__wrap_mbedtls_psa_aead_encrypt(&a,key,bits/8,alg,iv,sizeof iv,aad,sizeof aad,
  in_place?actual:plain,sizeof plain,actual,sizeof actual,&written)==0);
 assert(written==sizeof actual && !memcmp(actual,expected,written));
}
int main(void){
 internal=24000;nuwax_tls_archive_crypto_prepare();assert(!s_context && !allocations);
 internal=65536;fail_alloc=true;nuwax_tls_archive_crypto_prepare();assert(!s_context && !s_output);
 fail_alloc=false;nuwax_tls_archive_crypto_prepare();assert(s_context && s_output && hw_calls==1);
 send_record(128,PSA_ALG_GCM,true);assert(metrics.tls_hw_verified==1 && sw_calls==1);
 send_record(128,PSA_ALG_GCM,true);send_record(256,PSA_ALG_GCM,false);
 assert(metrics.tls_hw_calls==1 && sw_calls==2 && metrics.tls_hw_verified==2);
 assert(metrics.tls_hw_key_setups==2 && metrics.tls_hw_key_reuses==1);
 unsigned h=hw_calls;archive_scope=false;send_record(128,PSA_ALG_GCM,true);assert(hw_calls==h);
 archive_scope=true;send_record(128,99,true);assert(hw_calls==h);
 internal=11000;send_record(128,PSA_ALG_GCM,true);assert(hw_calls==h && metrics.tls_hw_fallbacks==1);
 internal=65536;fail_hw=true;send_record(128,PSA_ALG_GCM,true);
 assert(s_disabled && metrics.tls_hw_failures==1);h=hw_calls;
 fail_hw=false;send_record(128,PSA_ALG_GCM,true);assert(hw_calls==h);
 nuwax_tls_archive_crypto_release();assert(!s_context && !s_output && allocations==frees);
 s_disabled=false;nuwax_tls_archive_crypto_prepare();mismatch=true;
 send_record(128,PSA_ALG_GCM,true);assert(s_disabled && !s_verified);
 nuwax_tls_archive_crypto_release();assert(allocations==frees);
 puts("PASS scoped AES: admission, 128/256-bit records, in-place equivalence, foreign-task/algorithm isolation, memory/driver failure fallback, mismatch shutdown and resource release");
}
''')

report = (root / 'main/device_error_report.c').read_text(encoding='utf-8-sig')
start = report.index('void nuwax_device_error_report_service(')
service = report[start:report.index('\nbool nuwax_device_error_report_handle_ack', start)]
run('diagnostic', r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define ESP_OK 0
#define ESP_ERR_TIMEOUT 1
#define ESP_ERR_NOT_FINISHED 2
#define ESP_FAIL 3
#define ERROR_REPORT_EVENT_ID_BYTES 48
#define ERROR_REPORT_MAX_ATTEMPTS 3
#define ERROR_REPORT_ACK_TIMEOUT_MS 1000
#define ERROR_REPORT_RETRY_BACKOFF_MS 100
#define portENTER_CRITICAL(...) ((void)0)
#define portEXIT_CRITICAL(...) ((void)0)
#define TAG "diagnostic"
#define ESP_LOGI(tag,...) do{(void)(tag);if(0)printf(__VA_ARGS__);}while(0)
#define ESP_LOGW(tag,...) do{(void)(tag);if(0)printf(__VA_ARGS__);}while(0)
typedef int esp_err_t;
typedef int nuwax_ws_client_t;
typedef struct {char event_id[48];bool in_flight,warning;uint32_t attempts,last_attempt_ms,attempt_epoch;} error_report_event_t;
static error_report_event_t s_events[16];
static unsigned s_head,s_count,popped,send_calls;
static uint32_t tick=1,epoch=1;
static bool connected=true,priority_error,change_epoch,drop_link;
static int send_result;
static const char *esp_err_to_name(int err){(void)err;return "test";}
static bool nuwax_ws_is_connected(nuwax_ws_client_t *w){(void)w;return connected;}
static uint32_t nuwax_ws_connection_epoch(nuwax_ws_client_t *w){(void)w;return epoch;}
static uint32_t now_ms(void){return tick;}
static bool event_is_warning(const error_report_event_t *e){return e->warning;}
static bool queue_has_error_after_head_locked(void){return priority_error;}
static void copy_text(char *out,size_t n,const char *s){snprintf(out,n,"%s",s);}
static void pop_head_locked(void){s_count--;s_head++;popped++;}
static bool head_matches_locked(const char *id){return s_count && !strcmp(s_events[s_head].event_id,id);}
static int send_event(nuwax_ws_client_t *w,unsigned e,const error_report_event_t *event){
 (void)w;(void)e;(void)event;send_calls++;if(change_epoch)epoch++;if(drop_link)connected=false;return send_result;
}
''' + service + r'''
static void reset(void){memset(s_events,0,sizeof s_events);strcpy(s_events[0].event_id,"event");
 s_count=1;s_head=popped=send_calls=0;connected=true;priority_error=change_epoch=drop_link=false;
 tick=1;epoch=1;send_result=ESP_ERR_TIMEOUT;}
int main(void){nuwax_ws_client_t ws=0;reset();
 for(unsigned i=0;i<20;i++){nuwax_device_error_report_service(&ws);tick+=101;}
 assert(!popped && !s_events[0].attempts && send_calls==20 && !s_events[0].in_flight);
 send_result=ESP_OK;nuwax_device_error_report_service(&ws);assert(s_events[0].attempts==1 && s_events[0].in_flight);
 for(unsigned i=0;i<3;i++){tick+=1001;nuwax_device_error_report_service(&ws);}
 assert(popped==1); // Real missing ACKs still exhaust the bounded retries.
 reset();change_epoch=true;nuwax_device_error_report_service(&ws);assert(s_events[0].attempts==1);
 reset();drop_link=true;nuwax_device_error_report_service(&ws);assert(s_events[0].attempts==1);
 reset();s_events[0].warning=true;nuwax_device_error_report_service(&ws);
 priority_error=true;tick+=101;nuwax_device_error_report_service(&ws);assert(popped==1);
 puts("PASS diagnostics: 20 unsent admission timeouts retain event; real ACK/epoch failures count; warnings cannot block higher-priority errors");
}
''')

transport = (root / 'main/ws_transport.c').read_text(encoding='utf-8')
at = transport.index('static esp_err_t send_frame_with_timeout_buffered(')
at = transport.index('    const int64_t operation_started_us', at)
admission = transport[at:transport.index('    if (nuwax_ws_connection_epoch(client)', at)]
run('priority', r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "ws_send_metrics.h"
#define ESP_OK 0
#define ESP_ERR_TIMEOUT 1
typedef int esp_err_t;
typedef struct { atomic_uint control_waiters; } nuwax_ws_client_t;
static nuwax_ws_client_t ws;
static int64_t clock_us;
static unsigned sleeps,controls_complete;
static bool release_control,entered;
static int64_t esp_timer_get_time(void){return clock_us;}
static int64_t io_deadline_after_ms(unsigned ms){return clock_us+(int64_t)ms*1000;}
static bool io_deadline_expired(int64_t deadline){return clock_us>=deadline;}
static void vTaskDelay(unsigned ticks){assert(!entered);sleeps++;clock_us+=1000*ticks;
 if(release_control && sleeps==2){atomic_store(&ws.control_waiters,0);controls_complete++;}}
static int take_frame_lifecycle_locks_until(nuwax_ws_client_t *c,int64_t deadline){
 (void)c;assert(!entered);if(clock_us>=deadline)return ESP_ERR_TIMEOUT;entered=true;return ESP_OK;}
static esp_err_t admit(nuwax_ws_client_t *client,unsigned opcode,nuwax_ws_send_metrics_t *metrics,unsigned lock_timeout_ms){
 const bool interrupt_on_lock_timeout=false;
''' + admission + r'''
 return ESP_OK;
}
static void reset(void){clock_us=0;sleeps=controls_complete=0;entered=false;release_control=true;atomic_store(&ws.control_waiters,1);}
int main(void){nuwax_ws_send_metrics_t m={.shared_tls_archive=true};
 reset();assert(admit(&ws,2,&m,25)==ESP_OK && controls_complete==1 && sleeps==2 && entered);
 reset();release_control=false;assert(admit(&ws,2,&m,25)==ESP_ERR_TIMEOUT && !entered && clock_us==25000);
 reset();assert(admit(&ws,1,&m,25)==ESP_OK && !sleeps && entered);
 reset();m.shared_tls_archive=false;assert(admit(&ws,2,&m,25)==ESP_OK && !sleeps && entered);
 puts("PASS transport: archive yields at frame boundary to pending controls; bounded timeout holds no locks; ordinary audio/control unchanged");
}
''')
