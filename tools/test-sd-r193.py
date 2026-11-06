"""Real catalog/cache, data-first GC and bounded TLS adapter checks; no device I/O."""
from pathlib import Path
import json
import re
import subprocess
import sys
import tempfile
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

root=Path(__file__).resolve().parents[1]
out=root/'build-host-checks/sd-r193'
out.mkdir(parents=True,exist_ok=True)

def run(name,code,flags=()):
    src=out/(name+'.c');exe=out/(name+'.exe');src.write_text(code,encoding='utf-8')
    subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function',
        '-I'+str(root/'main'),'-I'+str(root/'main/audio_modes/meeting/host_stubs'),'-I'+str(out),
        *flags,str(src),'-o',str(exe)],check=True)
    with tempfile.TemporaryDirectory(prefix='sd-r193-'+name+'-') as directory:
        done=subprocess.run([str(exe)],cwd=directory,capture_output=True,text=True,timeout=60)
    (out/(name+'.log')).write_text(done.stdout+done.stderr,encoding='utf-8')
    print(done.stdout+done.stderr,end='')
    assert done.returncode==0,(name,done.returncode)

def function(source,signature):
    a=source.index(signature);b=source.index('{',a)+1;depth=1
    while depth:
        depth+=(source[b]=='{')-(source[b]=='}');b+=1
    return source[a:b]

source=(root/'main/sd_audio_session_replay.inc').read_text(encoding='utf-8-sig')
arbitration=function(source,'static bool card_live_pending(')
assert 'sd_audio_catalog_next' not in arbitration and 'card_owner' not in arbitration
run('fifo-no-preemption',r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef struct {bool connected;uint32_t epoch;} nuwax_ws_client_t;
"""+arbitration+r"""
int main(void){nuwax_ws_client_t ws={true,1};for(unsigned i=0;i<1000;i++)assert(!card_live_pending(&ws,i));
 puts("PASS FIFO arbitration: actual transport hook never scans SD or preempts an older monitor for newer online audio");}
""")
flags=['-DSD_AUDIO_CATALOG_ROOT="catalog"','-DESP_ERR_INVALID_CRC=0x109','-DESP_ERR_NOT_FOUND=0x105']

run('governor',r'''
#include <assert.h>
#include <stdio.h>
#define CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN 8192
#include "offline_replay_policy.h"
int main(void){
 replay_shared_governor_t g={0};
 assert(replay_shared_payload_admit(&g,true,0,32768,0,8193)==8064);
 assert(replay_shared_payload_admit(&g,true,48,32768,0,8193)==3896);
 assert(replay_shared_payload_admit(&g,true,40,32768,0,8193)==3896);
 assert(replay_shared_payload_admit(&g,true,32,32768,0,8193)==8064);
 assert(replay_shared_payload_admit(&g,true,0,16000,0,8193)==3896);
 assert(replay_shared_payload_admit(&g,true,0,20000,0,8193)==3896);
 assert(replay_shared_payload_admit(&g,true,0,20480,0,8193)==8064);
 assert(replay_shared_payload_admit(&g,true,0,32768,60,8193)==3896);
 for(unsigned monitoring=0;monitoring<2;monitoring++)for(unsigned q=0;q<129;q++){
  size_t n=replay_shared_payload_admit(&g,monitoring,q,32768,0,4097);
  assert(n && n<=8064);
 }
 assert(replay_shared_payload_admit(&g,true,0,32768,0,4097)==8064);
 assert(replay_shared_payload_admit(&g,true,0,32768,0,512)==0);
 assert(replay_shared_payload_admit(&g,false,0,32768,0,8193)==8064);
 puts("PASS governor: complete WS frames, 48/32 queue hysteresis aligned to transport recovery, RAM/wake protection, 4K TLS workspace independent from unchanged 8064-byte wire maximum");
}
''')

room=(root/'main/sd_monitor_storage.inc').read_text(encoding='utf-8-sig')
gc=r'''
#define main legacy_retention_main
#include "sd_monitor_retention_host_check.c"
#undef main
#include "offline_audio_policy.h"
#define CONFIG_NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES 4194304U
#define ARCHIVE_QUOTA_BYTES (1536ULL*1024*1024)
static struct {uint64_t used_bytes;} s;
static sd_audio_session_t s_card_session;
static atomic_bool s_space_blocked,s_monitor_space_blocked,s_directory_dirty;
static uint64_t fake_free;static bool free_recovers;static unsigned free_queries;
static int64_t esp_timer_get_time(void){return 1000;}
static int esp_vfs_fat_info(const char *p,uint64_t *total,uint64_t *free_bytes){(void)p;*total=32ULL<<30;*free_bytes=free_recovers && ++free_queries>=2 ? 32ULL<<30 : fake_free;return ESP_OK;}
static void archive_remove_empty_bucket(const char *p){(void)p;}
#define ESP_LOGW(...) ((void)0)
'''+function(room,'static esp_err_t card_monitor_make_room_locked(')+r'''
static int confirmed(const sd_audio_session_t *session,uint64_t *bytes,char path[128]){
 int e;unsigned calls=0;do{e=sd_audio_catalog_reclaim_confirmed(session->owner,bytes,path);assert(++calls<1000);}while(e==ESP_ERR_NOT_FINISHED);return e;
}
int main(void){
 assert(mkdir("audio",0775)==0);assert(sd_audio_catalog_init()==ESP_OK);
 sd_audio_session_t current=scene("safe_monitor",1,1,true),meeting=scene("safe_meeting",0,1,false),foreign=scene("safe_foreign",1,2,false);
 sd_audio_segment_t unacked=audio(&current,true),ack=audio(&current,true),pin=audio(&current,true),m=audio(&meeting,true),f=audio(&foreign,true);
 assert(sd_audio_catalog_ack(current.session_id,ack.index)==ESP_OK);
 assert(sd_audio_catalog_ack(current.session_id,pin.index)==ESP_OK);
 assert(sd_audio_catalog_ack(meeting.session_id,m.index)==ESP_OK);
 assert(sd_audio_catalog_ack(foreign.session_id,f.index)==ESP_OK);
 assert(sd_audio_catalog_pin(current.session_id,pin.index)==ESP_OK);
 acked_id[0]=0;fail_sync_at=sync_count+1;uint64_t bytes;char path[128];
 assert(confirmed(&current,&bytes,path)==ESP_FAIL && exists(ack.path));fail_sync_at=0;
 assert(confirmed(&current,&bytes,path)==ESP_OK && bytes==794 && !exists(ack.path));
 assert(exists(unacked.path)&&exists(pin.path)&&exists(m.path)&&exists(f.path));
 assert(confirmed(&current,&bytes,path)==ESP_ERR_NOT_FOUND);
 sd_audio_catalog_unpin();assert(confirmed(&current,&bytes,path)==ESP_OK && !exists(pin.path));
 sd_audio_session_t saved;assert(sd_audio_catalog_load(current.session_id,&saved)==ESP_OK&&saved.complete);
 s_card_session=current;fake_free=0;
 assert(card_monitor_make_room_locked(false,50)==ESP_ERR_NO_MEM&&s_monitor_space_blocked&&exists(unacked.path));
 free_recovers=true;free_queries=0;fake_free=0;
 assert(card_monitor_make_room_locked(false,50)==ESP_OK&&!s_monitor_space_blocked&&!s_space_blocked&&free_queries==2);
 puts("PASS space recovery: final capacity sample refreshes both general and monitoring flags");
 free_recovers=false;
 fake_free=32ULL<<30;assert(card_monitor_make_room_locked(false,50)==ESP_OK&&!s_monitor_space_blocked);
 assert(mkdir("bounded",0775)==0&&chdir("bounded")==0&&mkdir("audio",0775)==0&&sd_audio_catalog_init()==ESP_OK);
 current=scene("safe_many",1,3,true);sd_audio_segment_t last={0};
 for(unsigned i=0;i<40;i++)last=audio(&current,true);
 assert(sd_audio_catalog_ack(current.session_id,last.index)==ESP_OK);
 assert(confirmed(&current,&bytes,path)==ESP_OK&&bytes==794&&!exists(last.path));
 for(unsigned i=0;i<39;i++){sd_audio_segment_t part;assert(sd_audio_catalog_read(current.session_id,i,&part)==ESP_OK&&!part.acknowledged&&!part.missing&&exists(part.path));}
 puts("PASS r193 data-first GC: ACK fsync failure keeps source, pinned/meeting/foreign/unconfirmed protected, >16-record cursor progresses, full card stops admission and recovers without deleting unconfirmed audio");
}
'''
run('confirmed-gc',gc,flags+['-DSD_AUDIO_ARCHIVE_ROOT="audio"','-DSD_AUDIO_ARCHIVE_NEW_ROOT="audio2"'])

# Reuse the established adapter harness, replacing its fake transform with
# trusted AES-GCM vectors. The SDK callback is still doubled on the host;
# real SDK equivalence is additionally checked on the device's first record.
subprocess.run([sys.executable,str(root/'tools/test-sd-r190.py')],cwd=root,check=True,stdout=subprocess.DEVNULL)
base=(root/'build-host-checks/sd-r190/crypto.c').read_text(encoding='utf-8')
base=base[:base.rindex('int main(void)')]
def array(data):return '{'+','.join('0x%02x'%v for v in data)+'}' if data else '{0}'
vectors=[];header=['#include <stdint.h>\n#include <stddef.h>\n#include <string.h>']
for kb in (16,32):
    for n in (0,1,15,16,17,3896,3968,4096,4097,4098,8064,8136,8192,8193):
        i=len(vectors);key=bytes([3])*kb;plain=bytes((j*17+9)%256 for j in range(n));cipher=AESGCM(key).encrypt(bytes(12),plain,bytes(13))
        for label,data in [('key',key),('plain',plain),('cipher',cipher)]:header.append(f'static const uint8_t v{i}_{label}[]={array(data)};')
        vectors.append(f'{{{kb},{n},v{i}_key,v{i}_plain,v{i}_cipher}}')
header+=['typedef struct {size_t kb,n;const uint8_t *key,*plain,*cipher;} audit_vector;',
         'static const audit_vector vectors[]={'+','.join(vectors)+'};',
         'static const audit_vector *oracle(const uint8_t *k,size_t kb,const uint8_t *p,size_t n){for(size_t i=0;i<sizeof(vectors)/sizeof(vectors[0]);i++){const audit_vector *v=&vectors[i];if(v->kb==kb&&v->n==n&&!memcmp(v->key,k,kb)&&!memcmp(v->plain,p,n))return v;}assert(!"plaintext/key changed before oracle");return NULL;}']
# assert must be visible before the oracle definition.
header.insert(0,'#include <assert.h>')
(out/'audit_vectors.h').write_text('\n'.join(header),encoding='utf-8')
old=function(base,'static void fake_encrypt(')
base=base.replace(old,'''static void fake_encrypt(const uint8_t *key,size_t kb,const uint8_t *input,size_t n,uint8_t *output,uint8_t *tag){
 const audit_vector *v=oracle(key,kb,input,n);memcpy(output,v->cipher,n);memcpy(tag,v->cipher+n,16);
}''')
base=base.replace('(void)mode;(void)iv;(void)ivn;(void)aad;assert(tn==16);hw_calls++;',
                  '(void)mode;(void)iv;(void)ivn;(void)aad;assert(tn==16 && input==output);hw_calls++;')
# Model actual free-heap consumption and put canaries around every allocation.
# Allocation failure must preserve the recording reserve, not merely choose a
# smaller advertised record size using a constant fake free-heap value.
base=base.replace('static size_t internal=65536,largest=16384;',r'''
static size_t internal=65536,largest=16384;
static struct {void *raw;uint8_t *p;size_t n;} guarded[8];
static void *guard_alloc(size_t n){
 assert(n<=internal);unsigned i=0;while(i<8&&guarded[i].p)i++;assert(i<8);
 uint8_t *raw=malloc(n+144);assert(raw);memset(raw,0xa7,n+144);
 uint8_t *p=(uint8_t *)(((uintptr_t)raw+79U)&~(uintptr_t)63U);
 memset(p,0,n);guarded[i].raw=raw;guarded[i].p=p;guarded[i].n=n;internal-=n;return p;
}
static void guard_free(void *p){
 unsigned i=0;while(i<8&&guarded[i].p!=p)i++;assert(i<8);
 for(unsigned j=0;j<16;j++)assert(guarded[i].p[-16+(int)j]==0xa7&&guarded[i].p[guarded[i].n+j]==0xa7);
 internal+=guarded[i].n;free(guarded[i].raw);memset(&guarded[i],0,sizeof guarded[i]);
}
''')
base=base.replace(function(base,'static void *heap_caps_calloc('),
 'static void *heap_caps_calloc(size_t n,size_t s,unsigned caps){assert(caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));if(fail_alloc)return NULL;allocations++;return guard_alloc(n*s);}')
base=base.replace(function(base,'static void *heap_caps_aligned_alloc('),
 'static void *heap_caps_aligned_alloc(size_t a,size_t n,unsigned caps){assert(a==64&&caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA|MALLOC_CAP_8BIT));if(fail_alloc)return NULL;allocations++;return guard_alloc(n);}')
base=base.replace(function(base,'static void heap_caps_free('),
 'static void heap_caps_free(void *p){if(p){frees++;guard_free(p);}}')
code='#include "audit_vectors.h"\n'+base+r'''
static void record(const audit_vector *v,bool in_place){
 uint8_t iv[12]={0},aad[13]={0},plain[8193],actual[8209];size_t written=0;
 memcpy(plain,v->plain,v->n);memcpy(actual,v->plain,v->n);
 psa_key_attributes_t a={PSA_KEY_TYPE_AES,(unsigned)v->kb*8U};
 assert(__wrap_mbedtls_psa_aead_encrypt(&a,v->key,v->kb,PSA_ALG_GCM,iv,12,aad,13,
  in_place?actual:plain,v->n,actual,sizeof(actual),&written)==0);
 assert(written==v->n+16&&!memcmp(actual,v->cipher,written));
 assert(!memcmp(plain,v->plain,v->n));
}
int main(void){
 internal=65536;largest=16384;nuwax_tls_archive_crypto_prepare();assert(nuwax_tls_archive_crypto_capacity()==8193&&internal>=24576);
 for(unsigned i=0;i<sizeof(vectors)/sizeof(vectors[0]);i++){record(&vectors[i],false);record(&vectors[i],true);}
 assert(!metrics.tls_hw_failures&&metrics.tls_hw_verified==2);
 assert(metrics.tls_hw_key_setups==2&&metrics.tls_hw_key_reuses==54);
 fail_hw=true;record(&vectors[5],true);assert(s_disabled&&metrics.tls_hw_failures==1);fail_hw=false;
 nuwax_tls_archive_crypto_release();assert(allocations==frees&&internal==65536);
 s_disabled=false;internal=32000;largest=8192;nuwax_tls_archive_crypto_prepare();assert(nuwax_tls_archive_crypto_capacity()==4097&&internal>=24576);
 for(unsigned i=0;i<sizeof(vectors)/sizeof(vectors[0]);i++)record(&vectors[i],true);
 assert(metrics.tls_hw_fallbacks>0);nuwax_tls_archive_crypto_release();assert(allocations==frees&&internal==32000);
 s_disabled=false;internal=65536;largest=16384;nuwax_tls_archive_crypto_prepare();mismatch=true;
 record(&vectors[5],true);assert(s_disabled&&!s_verified);nuwax_tls_archive_crypto_release();assert(allocations==frees);
 s_disabled=false;internal=28000;largest=16384;nuwax_tls_archive_crypto_prepare();
 assert(!nuwax_tls_archive_crypto_capacity()&&internal==28000&&allocations==frees);
 internal=65536;fail_alloc=true;nuwax_tls_archive_crypto_prepare();assert(!nuwax_tls_archive_crypto_capacity()&&internal==65536);fail_alloc=false;
 puts("PASS r193 crypto adapter: 28 trusted AES-GCM vectors, 0..8193 bytes, AES128/256, in-place/out-of-place, internal in-place SDK calls, 8K/4K admission, partial hardware failure, software equivalence and mismatch fallback preserve caller input");
 puts("PASS r193 allocator: internal/DMA capabilities, 64-byte alignment, real reserve accounting, allocation failure and guard canaries");
}
'''
run('crypto-vectors',code)
print('PASS: r193 targeted production checks')
