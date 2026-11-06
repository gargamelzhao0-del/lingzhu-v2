"""Execute production digest cache, startup reconciliation and confirmation.

Flash, SHA primitive and NVS I/O are fault-injected; production decisions and
the chunk/yield loop are extracted verbatim. No timing sleeps or device access.
"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root/'main/nuwax_ota_new/nuwax_ota_main.c').read_text(encoding='utf-8-sig')
device = (root/'main/nuwax_ota_new/nuwax_ota_device.c').read_text(encoding='utf-8-sig')

def function(signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

prefix = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nuwax_ota_new/nuwax_ota_release_policy.h"
#define assert(x) do {if(!(x)){fprintf(stderr,"failed line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL 1
#define ESP_ERR_INVALID_ARG 2
#define ESP_ERR_NOT_FINISHED 3
#define ESP_ERR_NVS_NOT_FOUND 4
#define ESP_ERR_INVALID_CRC 5
#define ESP_ERR_INVALID_VERSION 6
#define ESP_ERR_INVALID_STATE 7
#define ESP_ERR_INVALID_SIZE 8
#define ESP_ERR_TIMEOUT 9
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define NUWAX_OTA_MANIFEST_SHA256_LEN 65
#define NUWAX_OTA_MANIFEST_VERSION_MAX_LEN 32
#define NUWAX_OTA_MANIFEST_MAX_SIZE 4194304U
#define NUWAX_OTA_IDENTITY_HASH_CHUNK_BYTES 1024U
#define NUWAX_OTA_IDENTITY_HASH_YIELD_BYTES 4096U
#define NUWAX_OTA_PENDING_TXN_MAGIC 0x4f544150U
#define NUWAX_OTA_PENDING_TXN_VERSION 3U
static int locked, hashes, reads, yields, inject_busy, fail_read, fail_final, nv_error, marked;
static bool pending, legacy, have_record;
static int nv_read_error, nv_fail_at, nv_reads, state_read_error;
static uint64_t accepted_sequence, effective_sequence;
static char s_status_text[128];
static const char *NVS_KEY_ACCEPTED_SEQ="seq", *NVS_KEY_LAST="last";
#define portENTER_CRITICAL(x) do{assert(!locked);locked=1;}while(0)
#define portEXIT_CRITICAL(x) do{assert(locked);locked=0;}while(0)
typedef struct {uint32_t address,size;char label[16];} esp_partition_t;
typedef struct {char version[32];uint8_t app_elf_sha256[32];} esp_app_desc_t;
typedef struct {char image_sha256[65];uint64_t release_sequence;uint32_t target_partition_address;bool pending_verify;} nuwax_ota_running_identity_t;
static esp_partition_t partition={0x6e0000,0x420000,"ota_1"};
static esp_app_desc_t app;
static const esp_partition_t *esp_ota_get_running_partition(void){return &partition;}
static const esp_app_desc_t *esp_app_get_description(void){return &app;}
static int64_t esp_timer_get_time(void){return (int64_t)yields*10000;}
static size_t strlcpy(char *d,const char*s,size_t n){size_t l=strlen(s);if(n){size_t z=l<n-1?l:n-1;memcpy(d,s,z);d[z]=0;}return l;}
static esp_err_t running_partition_sha256(const esp_partition_t*,uint32_t,char*,bool);
typedef struct {int unused;} nuwax_ota_sha256_t;
static esp_err_t nuwax_ota_sha256_init(nuwax_ota_sha256_t*h){assert(!locked);hashes++;return 0;}
static esp_err_t nuwax_ota_sha256_update(nuwax_ota_sha256_t*h,void*p,size_t n){return 0;}
static void nuwax_ota_sha256_abort(nuwax_ota_sha256_t*h){}
static esp_err_t nuwax_ota_sha256_final(nuwax_ota_sha256_t*h,char*out){if(fail_final)return ESP_FAIL;memset(out,'a',64);out[64]=0;return 0;}
static void vTaskDelay(int n){assert(!locked&&n==1);yields++;}
static esp_err_t esp_partition_read(const esp_partition_t*p,uint32_t off,void*out,size_t n){
 assert(!locked);reads++;if(inject_busy){inject_busy=0;char b[65];
 assert(running_partition_sha256(p,8192,b,false)==ESP_ERR_NOT_FINISHED);
 assert(running_partition_sha256(p,8192,b,true)==ESP_ERR_NOT_FINISHED);}
 if(fail_read && reads==fail_read)return ESP_FAIL;memset(out,0,n);return 0;}
'''
record = re.search(r'typedef struct \{\s*uint32_t magic;.*?\} nuwax_ota_pending_txn_t;', source, re.S).group()
cache = re.search(r'static struct \{\s*bool busy;.*?\} s_running_hash;', source, re.S).group()
stubs = r'''
static nuwax_ota_pending_txn_t stored, accepted;
static esp_err_t nvs_get_pending_txn(nuwax_ota_pending_txn_t*r,bool*l){nv_reads++;if(nv_read_error && (!nv_fail_at || nv_reads==nv_fail_at))return nv_read_error;if(!have_record)return ESP_ERR_NVS_NOT_FOUND;*r=stored;*l=legacy;return 0;}
static esp_err_t nvs_get_accepted_txn(nuwax_ota_pending_txn_t*r){*r=accepted;return 0;}
static esp_err_t nvs_set_accepted_txn(const nuwax_ota_pending_txn_t*r){if(nv_error)return ESP_FAIL;accepted=*r;return 0;}
static esp_err_t nvs_get_accepted_sequence(uint64_t*s){*s=accepted_sequence;return 0;}
static esp_err_t nvs_set_sequence(const char*k,uint64_t s){if(nv_error)return ESP_FAIL;accepted_sequence=s;return 0;}
static esp_err_t nvs_clear_pending_txn(void){have_record=false;return 0;}
static esp_err_t nvs_set_status(const char*k,const char*v){return 0;}
static uint64_t effective_release_sequence(void){return effective_sequence;}
static void set_effective_release_sequence(uint64_t s){effective_sequence=s;}
static esp_err_t nuwax_ota_running_image_pending_verify(bool*p){if(state_read_error)return state_read_error;*p=pending;return 0;}
static esp_err_t nuwax_ota_confirm_running_image(void){marked++;pending=false;return 0;}
'''
signatures = ['static bool sha256_hex_valid(', 'static bool pending_txn_valid(',
 'static esp_err_t partition_sha256_hex(', 'static esp_err_t running_partition_sha256(',
 'static bool pending_txn_metadata_matches_running(', 'static esp_err_t pending_txn_matches_running(',
 'static esp_err_t reconcile_pending_txn(', 'static esp_err_t get_running_identity(',
 'esp_err_t nuwax_ota_main_get_running_identity(', 'esp_err_t nuwax_ota_main_peek_running_identity(',
 'esp_err_t nuwax_ota_main_confirm_running_image(']
production = '\n'.join(function(s) for s in signatures)
tests = r'''
static void reset(void){memset(&s_running_hash,0,sizeof(s_running_hash));memset(&stored,0,sizeof(stored));
 memset(&app,0,sizeof(app));strcpy(app.version,"7.0.103");partition.address=0x6e0000;
 hashes=reads=yields=inject_busy=fail_read=fail_final=nv_error=marked=0;
 nv_read_error=nv_fail_at=nv_reads=state_read_error=0;pending=have_record=true;legacy=false;
 accepted_sequence=7000102;effective_sequence=7000103;stored.magic=NUWAX_OTA_PENDING_TXN_MAGIC;
 stored.format_version=3;stored.record_size=sizeof(stored);stored.release_sequence=7000103;
 stored.target_partition_address=partition.address;stored.image_size=8192;strcpy(stored.version,app.version);
 memset(stored.image_sha256,'a',64);stored.image_sha256[64]=0;}
int main(void){char digest[65];bool matches=false;nuwax_ota_running_identity_t identity;
 reset();assert(reconcile_pending_txn(true,accepted_sequence)==0&&hashes==0&&marked==0);
 assert(nuwax_ota_main_peek_running_identity(&identity)==ESP_ERR_NOT_FINISHED&&hashes==0);
 inject_busy=1;assert(nuwax_ota_main_get_running_identity(&identity)==0&&hashes==1&&reads==8&&yields==1);
 assert(nuwax_ota_main_peek_running_identity(&identity)==0&&hashes==1);
 assert(pending_txn_matches_running(&stored,&partition,&app,&matches)==0&&matches&&hashes==1);
 stored.image_sha256[0]='b';assert(nuwax_ota_main_get_running_identity(&identity)==ESP_ERR_INVALID_CRC);
 assert(nuwax_ota_main_confirm_running_image()==ESP_ERR_INVALID_CRC&&marked==0&&pending);
 stored.image_sha256[0]='a';assert(nuwax_ota_main_confirm_running_image()==0&&marked==1&&!pending&&hashes==1);
 assert(nuwax_ota_main_peek_running_identity(&identity)==0&&!identity.pending_verify&&hashes==1);
 reset();stored.target_partition_address++;assert(reconcile_pending_txn(true,accepted_sequence)==ESP_ERR_INVALID_CRC&&hashes==0);
 reset();stored.release_sequence=accepted_sequence;assert(reconcile_pending_txn(true,accepted_sequence)==ESP_ERR_INVALID_VERSION);
 reset();legacy=true;assert(reconcile_pending_txn(true,accepted_sequence)==ESP_ERR_INVALID_CRC);
 reset();assert(running_partition_sha256(&partition,8192,digest,true)==0);app.app_elf_sha256[0]++;
 assert(running_partition_sha256(&partition,8192,digest,false)==ESP_ERR_NOT_FINISHED);
 assert(running_partition_sha256(&partition,8192,digest,true)==0&&hashes==2);
 strcpy(app.version,"7.0.104");assert(running_partition_sha256(&partition,8192,digest,true)==0&&hashes==3);
 assert(running_partition_sha256(&partition,4096,digest,true)==0&&hashes==4);
 partition.address=0x2c0000;assert(running_partition_sha256(&partition,4096,digest,true)==0&&hashes==5);
 esp_partition_t foreign={0x6e0000,0x420000,"ota_1"};assert(running_partition_sha256(&foreign,4096,digest,true)==ESP_ERR_INVALID_ARG&&hashes==5);
 reset();fail_read=3;assert(nuwax_ota_main_get_running_identity(&identity)==ESP_FAIL&&!s_running_hash.valid&&!s_running_hash.busy);
 fail_read=0;assert(nuwax_ota_main_get_running_identity(&identity)==0&&hashes==2);
 reset();fail_final=1;assert(nuwax_ota_main_get_running_identity(&identity)==ESP_FAIL&&!s_running_hash.valid);
 fail_final=0;assert(nuwax_ota_main_get_running_identity(&identity)==0&&hashes==2);
 reset();s_running_hash.busy=true;assert(nuwax_ota_main_confirm_running_image()==ESP_ERR_NOT_FINISHED&&pending&&!marked);
 reset();nv_error=1;assert(nuwax_ota_main_confirm_running_image()==ESP_FAIL&&!pending&&marked==1&&have_record);
 nv_error=0;assert(reconcile_pending_txn(false,accepted_sequence)==0&&!have_record&&hashes==1&&accepted_sequence==7000103);
 reset();stored.image_size=3835296;assert(nuwax_ota_main_get_running_identity(&identity)==0&&hashes==1&&yields==936);
 for(int i=0;i<100;i++)assert(nuwax_ota_main_peek_running_identity(&identity)==0);
 assert(hashes==1&&yields==936);
 puts("PASS: production startup/hello do not hash; one shared digest; concurrency, binding, I/O failures, digest mismatch and durable confirmation recovery");
}
'''
with tempfile.TemporaryDirectory(prefix='ota-identity-') as tmp:
    for name, code, negative in [
        ('production', production, False),
        ('no_size_binding', production.replace('s_running_hash.size == image_size', 'true'), True),
        ('no_elf_binding', production.replace('memcmp(s_running_hash.elf_sha256, app->app_elf_sha256,\n               sizeof(s_running_hash.elf_sha256)) == 0', 'true'), True),
        ('cache_claimed_identity', production.replace('strcmp(record->image_sha256, actual_sha256) == 0', 'true'), True),
    ]:
        if negative:
            assert code != production, name
        c=Path(tmp)/(name+'.c');exe=Path(tmp)/(name+'.exe')
        c.write_text(prefix+record+cache+stubs+code+tests, encoding='utf-8')
        result=subprocess.run(['gcc','-std=c11','-O0','-I',str(root/'main'),str(c),'-o',str(exe)],capture_output=True,text=True)
        assert result.returncode==0,result.stderr
        result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=10)
        assert (result.returncode!=0)==negative,(name,result.stdout,result.stderr)
        print('PASS:',name,'mutant rejected' if negative else result.stdout.strip())

assert 'esp_err_t ready_err = process_cloud_ready_with_retry(ready_ms);' in device
assert 'xQueueReceive(s_commands, &item, pdMS_TO_TICKS(100))' in device
assert 'if (!validation_retry_pending)' in device
assert 'if (!validation_retry_pending && ready_err != ESP_OK' in device

# Integration fences: cannot confirm or emit staged before full verification;
# a concurrent worker must re-arm the existing controller, not lose the event.
ready=device[device.index('static esp_err_t process_cloud_ready_event(uint32_t ready_ms)\n{'):]
assert ready.index('nuwax_ota_main_pending_identity_matches') < ready.index('emit_result_reliably("staged"') < ready.index('nuwax_ota_main_confirm_running_image()')
assert 'if (err == ESP_ERR_NOT_FINISHED)' in ready and 's_cloud_ready_event_pending = true;' in ready
assert ready.index('deadline_reached(monotonic_ms(), s_boot_validation_deadline_ms)') < ready.index('emit_result_reliably("staged"')
hello=(root/'main/session_protocol.c').read_text(encoding='utf-8-sig')
assert 'nuwax_ota_main_peek_running_identity(&running_identity)' in hello
transport=(root/'main/nuwax_ota_new/nuwax_ota_transport.c').read_text(encoding='utf-8-sig')
assert 'update_partition->address == running_partition->address' in transport
print('PASS: controller retry/deadline, staged ACK-before-confirm, nonblocking hello and inactive-partition-only transport wiring')

# Execute the complete production pending-verify branch as well. The confirmed
# / rollback-report branch is outside this change and is excluded, not mocked
# as having passed. Network result delivery and reboot are injected boundaries.
controller = ready[:ready.index('    if (nuwax_ota_device_is_active())')] + 'return ESP_OK;\n}\n'
controller_stubs = r'''
#define ESP_ERR_TIMEOUT 9
typedef enum {OTA_BOOT_DECISION_NONE,OTA_BOOT_DECISION_COMPLETE,OTA_BOOT_DECISION_TIMEOUT,
 OTA_BOOT_DECISION_CLOUD_READY,OTA_BOOT_DECISION_LOCAL_HEALTH_FAILED} ota_boot_decision_owner_t;
typedef struct {void*ctx;esp_err_t(*check_boot_health)(void*,char*,size_t);} nuwax_ota_device_callbacks_t;
typedef struct {uint32_t target_partition_address;char image_sha256[65];} ota_report_context_t;
static ota_boot_decision_owner_t s_boot_decision_owner;
static bool s_boot_pending_verify,s_cloud_ready_event_pending,s_cloud_authenticated;
static uint32_t s_boot_retry_count,s_boot_retry_log_ms;
#define OTA_HEALTH_PENDING_LOG_MS 5000U
static uint32_t s_boot_validation_deadline_ms,s_cloud_ready_event_ms;
static int rolled_back,staged,committed,refreshes,ack_error,ack_advance,health_error;
static uint32_t monotonic_ms(void){return 100+(uint32_t)yields*10;}
static bool deadline_reached(uint32_t now,uint32_t d){return (int32_t)(now-d)>=0;}
static esp_err_t health(void*x,char*d,size_t n){return health_error;}
static nuwax_ota_device_callbacks_t s_callbacks={NULL,health};
static bool should_log_health_pending(uint32_t n,char*d){return false;}
static void reset_health_pending_log(void){}
static esp_err_t execute_boot_rollback(ota_boot_decision_owner_t o,const char*c,const char*d){rolled_back++;return ESP_ERR_TIMEOUT;}
static esp_err_t execute_timeout_rollback(void){rolled_back++;return ESP_ERR_TIMEOUT;}
static ota_report_context_t report_context_snapshot(void){ota_report_context_t r={.target_partition_address=stored.target_partition_address};strcpy(r.image_sha256,stored.image_sha256);return r;}
static esp_err_t nuwax_ota_main_running_image_pending_verify(bool*p){return nuwax_ota_running_image_pending_verify(p);}
static bool begin_terminal_report(void){return true;}
static void complete_terminal_report(esp_err_t e){}
static void request_reconnect_after_terminal_failure(esp_err_t e){}
static void request_firmware_metadata_refresh(void){refreshes++;}
static esp_err_t clear_report_context(void){return 0;}
static void refresh_interlocks(void){}
static bool nuwax_ota_device_blocks_audio(void){return s_boot_pending_verify;}
static esp_err_t emit_result_reliably(const char*s,const char*m){
 if(!strcmp(s,"staged")){assert(!marked);staged++;yields+=ack_advance;return ack_error;}
 assert(marked&&!pending);committed++;return 0;}
'''
old_source = source
try:
    source=device
    claim=function('static ota_boot_decision_owner_t claim_cloud_boot_decision(')
    retry=function('static esp_err_t process_cloud_ready_with_retry(')
finally:
    source=old_source
controller_tests = r'''
static void fresh(void){reset();s_boot_decision_owner=OTA_BOOT_DECISION_NONE;s_boot_pending_verify=true;
 s_cloud_ready_event_pending=false;s_boot_validation_deadline_ms=100000;s_cloud_ready_event_ms=0;
 rolled_back=staged=committed=refreshes=ack_error=ack_advance=health_error=0;
 s_cloud_authenticated=true;s_boot_retry_count=s_boot_retry_log_ms=0;}
int main(void){fresh();s_running_hash.busy=true;
 assert(process_cloud_ready_event(42)==ESP_ERR_NOT_FINISHED&&s_cloud_ready_event_pending);
 assert(s_cloud_ready_event_ms==42&&s_boot_decision_owner==OTA_BOOT_DECISION_NONE&&!marked);
 s_running_hash.busy=false;s_cloud_ready_event_pending=false;
 assert(process_cloud_ready_event(42)==0&&hashes==1&&marked==1&&staged==1&&committed==1);
 assert(!s_boot_pending_verify&&refreshes==1);
 fresh();s_boot_validation_deadline_ms=105;assert(process_cloud_ready_event(42)==ESP_ERR_TIMEOUT);
 assert(rolled_back==1&&!marked&&!staged); /* expires while hashing */
 fresh();s_boot_validation_deadline_ms=115;ack_advance=1;
 assert(process_cloud_ready_event(42)==ESP_ERR_TIMEOUT&&staged==1&&!marked&&rolled_back==1);
 fresh();ack_error=ESP_ERR_TIMEOUT;assert(process_cloud_ready_event(42)==ESP_ERR_TIMEOUT&&!marked);
 assert(pending&&s_boot_pending_verify&&s_boot_decision_owner==OTA_BOOT_DECISION_NONE);
 fresh();stored.image_sha256[0]='b';assert(process_cloud_ready_event(42)==ESP_ERR_INVALID_CRC&&!marked&&!staged);
 fresh();fail_read=1;assert(process_cloud_ready_event(42)==ESP_FAIL&&!marked&&!staged);
 fresh();health_error=ESP_ERR_NOT_FINISHED;assert(process_cloud_ready_event(42)==ESP_ERR_NOT_FINISHED&&hashes==0&&s_cloud_ready_event_pending);
 /* Metadata guard conflicts before the hash starts must retain the event. */
 for(unsigned kind=0;kind<2;kind++) {
  fresh();nv_read_error=kind?ESP_ERR_TIMEOUT:ESP_ERR_INVALID_STATE;
  assert(process_cloud_ready_with_retry(42)==nv_read_error);
  assert(s_cloud_ready_event_pending&&s_cloud_ready_event_ms==42&&s_boot_retry_count==1);
  assert(!marked&&!staged&&s_boot_validation_deadline_ms==100000);
  nv_read_error=0; nuwax_ota_running_identity_t id;
  assert(nuwax_ota_main_get_running_identity(&id)==ESP_OK&&hashes==1);
  s_cloud_ready_event_pending=false;
  assert(process_cloud_ready_with_retry(42)==ESP_OK&&marked==1&&committed==1&&hashes==1);
 }
 fresh();state_read_error=ESP_ERR_INVALID_STATE;
 assert(process_cloud_ready_with_retry(42)==state_read_error&&s_cloud_ready_event_pending);
 state_read_error=0;s_cloud_ready_event_pending=false;
 assert(process_cloud_ready_with_retry(42)==ESP_OK&&marked==1);
 /* A second metadata read can collide after the staged ACK, before marking valid. */
 fresh();nv_read_error=ESP_ERR_INVALID_STATE;nv_fail_at=2;
 assert(process_cloud_ready_with_retry(42)==ESP_ERR_INVALID_STATE);
 assert(staged==1&&!marked&&s_cloud_ready_event_pending);
 nv_read_error=0;s_cloud_ready_event_pending=false;
 assert(process_cloud_ready_with_retry(42)==ESP_OK&&marked==1&&committed==1);
 /* Persistent conflict cannot renew the deadline; timeout remains claimable. */
 fresh();nv_read_error=ESP_ERR_INVALID_STATE;
 for(unsigned i=0;i<5;i++) {
  s_cloud_ready_event_pending=false;
  assert(process_cloud_ready_with_retry(42)==ESP_ERR_INVALID_STATE&&s_cloud_ready_event_pending);
  assert(s_boot_validation_deadline_ms==100000&&!marked);
 }
 yields=9990;s_cloud_ready_event_pending=false;
 assert(process_cloud_ready_with_retry(42)==ESP_ERR_TIMEOUT&&rolled_back==1&&!marked);
 assert(!s_cloud_ready_event_pending);
 fresh();s_cloud_authenticated=false;nv_read_error=ESP_ERR_INVALID_STATE;
 assert(process_cloud_ready_with_retry(42)==ESP_ERR_INVALID_STATE&&!s_cloud_ready_event_pending);
 fresh();s_boot_decision_owner=OTA_BOOT_DECISION_TIMEOUT;
 assert(process_cloud_ready_with_retry(42)==ESP_ERR_INVALID_STATE&&!s_cloud_ready_event_pending);
 fresh();stored.image_sha256[0]='b';
 assert(process_cloud_ready_with_retry(42)==ESP_ERR_INVALID_CRC&&!s_cloud_ready_event_pending&&!marked);
 fresh();assert(process_cloud_ready_with_retry(42)==ESP_OK&&!s_boot_pending_verify&&!s_cloud_ready_event_pending);
 state_read_error=ESP_ERR_INVALID_STATE;
 assert(process_cloud_ready_with_retry(42)==ESP_ERR_INVALID_STATE&&!s_cloud_ready_event_pending);
 puts("PASS: transient metadata/state conflicts resume, concurrent hash reused, pre-confirm conflict, fixed deadline, auth and completed/rollback fences");
 puts("PASS: production controller busy requeue, resumed commit, hash/ACK deadline expiry, bad digest, read failure, missing ACK and pending local health");
}
'''
with tempfile.TemporaryDirectory(prefix='ota-controller-') as tmp:
    for name, body, negative in [
        ('controller',controller+retry,False),
        ('lost_guard_retry',controller+retry.replace('s_cloud_ready_event_pending = true;', 's_cloud_ready_event_pending = false;'),True),
        ('lost_retry',(controller+retry).replace('s_cloud_ready_event_pending = true;', 's_cloud_ready_event_pending = false;'),True),
        ('deadline_bypass',(controller+retry).replace('deadline_reached(monotonic_ms(), s_boot_validation_deadline_ms)', 'false'),True),
    ]:
        c=Path(tmp)/(name+'.c');exe=Path(tmp)/(name+'.exe')
        c.write_text(prefix+record+cache+stubs+production+
                     function('esp_err_t nuwax_ota_main_pending_identity_matches(')+
                     tests[:tests.index('int main(void)')]+controller_stubs+claim+body+controller_tests,encoding='utf-8')
        result=subprocess.run(['gcc','-std=c11','-O0','-I',str(root/'main'),str(c),'-o',str(exe)],capture_output=True,text=True)
        assert result.returncode==0,result.stderr
        result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=10)
        assert (result.returncode!=0)==negative,(name,result.stdout,result.stderr)
        print('PASS:',name,'mutant rejected' if negative else result.stdout.strip())
