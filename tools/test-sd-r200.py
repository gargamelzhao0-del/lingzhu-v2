"""Production key cache + unmodified ESP GCM, and post-ACK/local repair paths.

Host AES ECB/CTR is an OpenSSL oracle. GHASH, record reset, production dispatch,
key invalidation, catalog and local commit logic are the actual C sources.
This verifies correctness, not target performance or hardware DMA timing.
"""
from pathlib import Path
import ctypes, hashlib, json, os, random, re, shutil, subprocess, tempfile
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

root = Path(__file__).resolve().parents[1]
fast_mode = os.environ.get('NUWAX_SD_TEST_FAST_GHASH')
assert fast_mode in (None, '0', '1')
out = root / ('build-host-checks/sd-r200' + ('-ghash-' + fast_mode if fast_mode else ''))
out.mkdir(parents=True, exist_ok=True)
if os.name == 'nt': ctypes.windll.kernel32.SetErrorMode(0x8003)
sdk = Path(os.environ.get('IDF_PATH', 'C:/esp/v6.0/esp-idf'))
inc = out / 'include'
def header(name, text):
    p = inc / name; p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text, encoding='utf-8')
header('aes/esp_aes.h', '''#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#define bzero(p,n) memset((p),0,(n))
#define AES_BLOCK_BYTES 16U
#define ESP_AES_ENCRYPT 1
#define ESP_AES_DECRYPT 0
typedef struct {uint8_t key_bytes;volatile uint8_t key_in_hardware;uint8_t key[32];} esp_aes_context;
int esp_aes_crypt_ecb(esp_aes_context*,int,const unsigned char*,unsigned char*);
int esp_aes_crypt_ctr(esp_aes_context*,size_t,size_t*,unsigned char*,unsigned char*,const unsigned char*,unsigned char*);
''')
for n in ['esp_aes_internal.h','hal/aes_hal.h','soc/soc_memory_layout.h','esp_aes_dma_priv.h']:
    header(n, '#pragma once\n')
header('esp_log.h', '#pragma once\n#define ESP_LOGE(tag,...) ((void)(tag))\n#define ESP_LOGW(tag,...) ((void)(tag))\n#define ESP_LOGI(tag,...) ((void)(tag))\n')
header('sdkconfig.h', '#pragma once\n#define CONFIG_MBEDTLS_HARDWARE_GCM 0\n#define CONFIG_NUWAX_SD_TLS_SCOPED_AES 1\n#define CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN 8192\n')
header('soc/soc_caps.h', '#pragma once\n#define SOC_AES_SUPPORT_DMA 1\n#define SOC_AES_SUPPORT_AES_192 0\n')
header('esp_heap_caps.h', '''#pragma once
#include <stdlib.h>
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_DMA 4
#define MALLOC_CAP_SPIRAM 8
size_t heap_caps_get_free_size(unsigned);
size_t heap_caps_get_largest_free_block(unsigned);
void *heap_caps_calloc(size_t,size_t,unsigned);
void *heap_caps_malloc(size_t,unsigned);
void *heap_caps_aligned_alloc(size_t,size_t,unsigned);
void heap_caps_free(void*);
''')
header('esp_crypto_shared_gdma.h', '#pragma once\nvoid esp_crypto_shared_gdma_free(void);\n')
header('esp_timer.h', '#pragma once\n#include <stdint.h>\n/* Synthetic test clock; not a host or device performance measurement. */\nstatic inline int64_t esp_timer_get_time(void){static int64_t t;return ++t;}\n')
header('mbedtls/platform_util.h', '#pragma once\n#include <stddef.h>\nvoid mbedtls_platform_zeroize(void*,size_t);\n')
header('psa/crypto.h', '''#pragma once
#include <stdint.h>
#include <stddef.h>
#define PSA_ERROR_INVALID_ARGUMENT (-135)
#define PSA_ERROR_NOT_SUPPORTED (-134)
#define PSA_ERROR_BAD_STATE (-137)
#define PSA_ERROR_INVALID_SIGNATURE (-149)
#define PSA_SUCCESS 0
#define PSA_ALG_GCM 7
#define PSA_KEY_TYPE_AES 9
typedef int psa_status_t;
typedef unsigned psa_algorithm_t;
typedef struct {unsigned type,bits;} psa_key_attributes_t;
static inline unsigned psa_get_key_type(const psa_key_attributes_t *a){return a->type;}
static inline unsigned psa_get_key_bits(const psa_key_attributes_t *a){return a->bits;}
''')
shutil.copy2(sdk/'components/mbedtls/port/aes/esp_aes_gcm.c', out/'esp_aes_gcm.c')
shutil.copy2(sdk/'components/mbedtls/port/include/aes/esp_aes_gcm.h', inc/'aes/esp_aes_gcm.h')
bridge = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "aes/esp_aes_gcm.h"
#include "tls_record_metrics.h"
#include "tls_archive_crypto.h"
#include "psa/crypto.h"
typedef int(*ecb_fn)(const uint8_t*,size_t,const uint8_t*,uint8_t*);
typedef int(*ctr_fn)(const uint8_t*,size_t,uint8_t*,size_t,const uint8_t*,uint8_t*);
typedef int(*aead_fn)(const uint8_t*,size_t,const uint8_t*,const uint8_t*,size_t,const uint8_t*,size_t,uint8_t*);
static ecb_fn oracle_ecb;static ctr_fn oracle_ctr;static aead_fn oracle_aead;
static nuwax_ws_send_metrics_t metrics;
static unsigned ecbs,ctrs,sw,allocs,frees,fail_ctr;static bool scope=true;
static size_t internal=65536,largest=16384;
static struct {void *raw;uint8_t *p;size_t n;bool external;} guarded[8];
static void *guard_alloc(size_t n,unsigned caps){
 unsigned i=0;while(i<8&&guarded[i].p)i++;assert(i<8);
 uint8_t *raw=malloc(n+144);assert(raw);memset(raw,0xa7,n+144);
 uint8_t *p=(uint8_t*)(((uintptr_t)raw+79U)&~(uintptr_t)63U);memset(p,0,n);
 guarded[i].raw=raw;guarded[i].p=p;guarded[i].n=n;guarded[i].external=(caps&8U)!=0;
 if(!guarded[i].external){assert(internal>=n);internal-=n;}allocs++;return p;
}
nuwax_ws_send_metrics_t *nuwax_tls_archive_metrics(void){return scope?&metrics:NULL;}
size_t heap_caps_get_free_size(unsigned c){return (c&8U)?2097152:internal;}
size_t heap_caps_get_largest_free_block(unsigned c){(void)c;return largest;}
void *heap_caps_calloc(size_t n,size_t s,unsigned c){assert(c==3U);return guard_alloc(n*s,c);}
void *heap_caps_aligned_alloc(size_t a,size_t n,unsigned c){assert(a==64&&c==7U);return guard_alloc(n,c);}
void *heap_caps_malloc(size_t n,unsigned c){assert(c==(8U|2U));return guard_alloc(n,c);}
void heap_caps_free(void *p){if(p){unsigned i=0;while(i<8&&guarded[i].p!=p)i++;assert(i<8);
 for(int j=0;j<16;j++)assert(guarded[i].p[-16+j]==0xa7&&guarded[i].p[guarded[i].n+j]==0xa7);
 if(!guarded[i].external)internal+=guarded[i].n;
 free(guarded[i].raw);memset(&guarded[i],0,sizeof guarded[i]);frees++;}}
void esp_crypto_shared_gdma_free(void){}
void mbedtls_platform_zeroize(void *p,size_t n){volatile uint8_t *b=p;while(n--)*b++=0;}
int esp_aes_crypt_ecb(esp_aes_context *c,int mode,const unsigned char *in,unsigned char *o){
 (void)mode;ecbs++;return oracle_ecb(c->key,c->key_bytes,in,o);
}
int esp_aes_crypt_ctr(esp_aes_context *c,size_t n,size_t *off,unsigned char *iv,
 unsigned char *stream,const unsigned char *in,unsigned char *o){
 (void)stream;ctrs++;if(fail_ctr && !--fail_ctr){memset(o,0xde,n);return -1;}assert(!*off);
 int ret=oracle_ctr(c->key,c->key_bytes,iv,n,in,o);*off=n%16;return ret;
}
psa_status_t __real_mbedtls_psa_aead_encrypt(const psa_key_attributes_t *a,const uint8_t *k,size_t kb,
 psa_algorithm_t alg,const uint8_t *iv,size_t ivn,const uint8_t *aad,size_t ad,
 const uint8_t *in,size_t n,uint8_t *o,size_t cap,size_t *written){
 (void)a;(void)alg;assert(ivn==12&&cap>=n+16);sw++;int err=oracle_aead(k,kb,iv,aad,ad,in,n,o);if(!err)*written=n+16;return err;
}
psa_status_t __wrap_mbedtls_psa_aead_encrypt(const psa_key_attributes_t*,const uint8_t*,size_t,psa_algorithm_t,
 const uint8_t*,size_t,const uint8_t*,size_t,const uint8_t*,size_t,uint8_t*,size_t,size_t*);
__declspec(dllexport) void hooks(ecb_fn a,ctr_fn b,aead_fn c){oracle_ecb=a;oracle_ctr=b;oracle_aead=c;}
__declspec(dllexport) void prepare(void){nuwax_tls_archive_crypto_prepare();assert(nuwax_tls_archive_crypto_capacity()==8193);}
__declspec(dllexport) void release(void){nuwax_tls_archive_crypto_release();assert(!nuwax_tls_archive_crypto_capacity()&&allocs==frees);}
__declspec(dllexport) void failure(int f){fail_ctr=(unsigned)f;}
__declspec(dllexport) void small_window(void){internal=32000;largest=8192;}
__declspec(dllexport) unsigned count(unsigned n){switch(n){case 0:return ecbs;case 1:return sw;
 case 2:return metrics.tls_hw_key_setups;case 3:return metrics.tls_hw_key_reuses;case 4:return metrics.tls_hw_verified;
 case 5:return metrics.tls_hw_failures;case 6:return allocs;case 7:return metrics.tls_hw_staged_calls;
 case 8:return metrics.tls_gcm_fast_updates;default:return ctrs;}}
__declspec(dllexport) int record(const uint8_t *key,size_t kb,const uint8_t *iv,const uint8_t *aad,size_t ad,
 const uint8_t *input,size_t n,uint8_t *out,int in_place){
 psa_key_attributes_t a={PSA_KEY_TYPE_AES,(unsigned)kb*8};size_t written=0;
 if(in_place)memcpy(out,input,n);
 int err=__wrap_mbedtls_psa_aead_encrypt(&a,key,kb,PSA_ALG_GCM,iv,12,aad,ad,in_place?out:input,n,out,n+16,&written);
 assert(!err&&written==n+16);return err;
}
'''
(out/'bridge.c').write_text(bridge,encoding='utf-8')
dll=out/'production-gcm.dll'
fast_flags=['-DNUWAX_ARCHIVE_GHASH_HOST_TEST=1','-DNUWAX_ARCHIVE_GHASH_FORCE_TEST='+fast_mode] if fast_mode else []
subprocess.run(['gcc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-sign-compare','-shared',*fast_flags,
    '-I'+str(inc),'-I'+str(root/'main'),str(out/'esp_aes_gcm.c'),str(root/'main/tls_archive_crypto.c'),str(out/'bridge.c'),'-o',str(dll)],check=True)
lib=ctypes.CDLL(str(dll));P=ctypes.POINTER(ctypes.c_uint8);Z=ctypes.c_size_t
E=ctypes.CFUNCTYPE(ctypes.c_int,P,Z,P,P);T=ctypes.CFUNCTYPE(ctypes.c_int,P,Z,P,Z,P,P)
A=ctypes.CFUNCTYPE(ctypes.c_int,P,Z,P,P,Z,P,Z,P)
@E
def ecb(key,kb,src,dst):
    c=Cipher(algorithms.AES(ctypes.string_at(key,kb)),modes.ECB()).encryptor()
    ctypes.memmove(dst,c.update(ctypes.string_at(src,16))+c.finalize(),16);return 0
@T
def ctr(key,kb,iv,n,src,dst):
    raw=ctypes.string_at(iv,16);c=Cipher(algorithms.AES(ctypes.string_at(key,kb)),modes.CTR(raw)).encryptor()
    ctypes.memmove(dst,c.update(ctypes.string_at(src,n))+c.finalize(),n)
    ctypes.memmove(iv,((int.from_bytes(raw,'big')+(n+15)//16)%(1<<128)).to_bytes(16,'big'),16);return 0
@A
def aead(key,kb,iv,aad,ad,src,n,dst):
    data=AESGCM(ctypes.string_at(key,kb)).encrypt(ctypes.string_at(iv,12),ctypes.string_at(src,n),ctypes.string_at(aad,ad))
    ctypes.memmove(dst,data,len(data));return 0
lib.hooks.argtypes=[E,T,A];lib.hooks(ecb,ctr,aead)
lib.record.argtypes=[P,Z,P,P,Z,P,Z,P,ctypes.c_int]
buf=lambda d:(ctypes.c_uint8*max(1,len(d))).from_buffer_copy(d or b'\0')
rng=random.Random(200500);records=0
keys=[bytes(range(16)),bytes(range(32)),bytes([3])*16,bytes(range(16))]
for profile in range(2):
    if profile: lib.small_window()
    lib.prepare();assert lib.count(0)==1+profile*5
    for key in keys:
        for n in [0,1,15,16,17,31,320,1024,3896,4040,4096,4097,4098,8136,8192,8193]:
            for ad in [0,5,13]:
                iv=rng.randbytes(12);aad=rng.randbytes(ad);data=rng.randbytes(n)
                for inplace in [0,1]:
                    output=(ctypes.c_uint8*(n+16))()
                    assert lib.record(buf(key),len(key),buf(iv),buf(aad),ad,buf(data),n,output,inplace)==0
                    assert bytes(output)==AESGCM(key).encrypt(iv,data,aad);records+=1
    verified=4 if profile==0 else 12 # Each traffic key verifies BOTH paths independently.
    assert lib.count(0)==(profile+1)*5 and lib.count(1)==verified and lib.count(2)==(profile+1)*4
    assert lib.count(3)==(profile+1)*380 and lib.count(4)==verified and lib.count(5)==0
    allocs=lib.count(6);lib.prepare();assert lib.count(6)==allocs # file boundary retains context/key
    lib.release()
assert records==768 and lib.count(7)==96
if fast_mode is not None: assert (lib.count(8)>0)==(fast_mode=='1')
lib.prepare();assert lib.count(0)==11
key=keys[0];iv=rng.randbytes(12);data=rng.randbytes(8193);aad=rng.randbytes(13)
lib.failure(2);output=(ctypes.c_uint8*8209)() # fail AFTER first 4 KiB was encrypted
assert lib.record(buf(key),len(key),buf(iv),buf(aad),13,buf(data),8193,output,1)==0
assert bytes(output)==AESGCM(key).encrypt(iv,data,aad) and lib.count(5)==1
lib.failure(0);lib.release()
print('PASS actual SDK + production cache: 768 ciphertext/tag checks, 8K/4K DMA windows, 8 traffic keys, 760 table reuses; second-chunk failure preserves plaintext/fallback')
if fast_mode is not None: print('PASS production GHASH backend',fast_mode,'oracle and failure checks; host selection is forced, no speed claim')

# Use production catalog/files and the pending-commit include with bounded
# platform substitutes. No mocked ACK decision, index IO or recovery logic.
flags=['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function',
 '-DESP_ERR_INVALID_CRC=0x109','-DESP_ERR_NOT_FOUND=0x105','-DSD_AUDIO_CATALOG_ROOT="catalog"',
 '-DSD_AUDIO_ARCHIVE_ROOT="audio"','-DSD_AUDIO_ARCHIVE_NEW_ROOT="audio2"',
 '-I'+str(root/'main/audio_modes/meeting/host_stubs'),'-I'+str(root/'main')]
exe=out/'pending-repair.exe'
subprocess.run(flags+[str(root/'main/sd_audio_pending_commit_host_check.c'),str(root/'main/audio_frame.c'),'-o',str(exe)],check=True)
with tempfile.TemporaryDirectory(prefix='nuwax-r200-') as cwd:
    subprocess.run([str(exe)],cwd=cwd,check=True,timeout=60)
spool=(root/'main/offline_audio_spool.c').read_text(encoding='utf-8-sig')
block=spool[spool.index('if (s_card_replay_segment.payload_crc != footer.crc32'):spool.index('s_replay_route = !header.mode')]
assert block.index('recheck_archive_path(path)')<block.index('sd_audio_catalog_reconcile_file(')
assert 'nuwax_sd_card_get_identity(&after)' in block and 'memcmp(owner, s_card_replay_session.owner' in block
shared=(root/'main/offline_replay_shared.inc').read_text()
assert 'nuwax_tls_archive_crypto_release();' not in shared
end=spool[spool.index('No frame/reader is left in flight here.'):spool.index('static uint32_t s_formal_retry_backoff_ms')]
assert end.index('nuwax_tls_archive_crypto_release();')<end.index('atomic_store_explicit(&s.replay_running, false')
(out/'results.json').write_text(json.dumps(dict(records=records,trafficKeys=8,tableReuses=760,stagedRecords=96,actualSdkGcm=True,
    productionWrapper=True,sdkGcmSha256=hashlib.sha256((out/'esp_aes_gcm.c').read_bytes()).hexdigest(),
    targetPerformanceMeasured=False,forcedGhashBackend=fast_mode,passed=True),indent=2)+'\n')
print('PASS r200: full-CRC-before-repair, physical-card/binding fences, batch crypto release before owner idle')
