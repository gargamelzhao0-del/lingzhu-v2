from pathlib import Path
import ctypes as C
import hashlib,json,re,subprocess
from cryptography.hazmat.primitives.ciphers import Cipher,algorithms,modes
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
root=Path(__file__).resolve().parents[1]
out=root/'build-host-checks/sd-sdk-gcm'
out.mkdir(exist_ok=True,parents=True)
sdk=Path('C:/esp/v6.0/esp-idf/components/mbedtls/port')
source=(sdk/'aes/esp_aes_gcm.c').read_text(encoding='utf-8')
header=(sdk/'include/aes/esp_aes_gcm.h').read_text(encoding='utf-8')
types=header[header.index('typedef enum'):header.index('/**\n * \\brief          The GCM context structure.')]
start=header.index('typedef struct {');end=header.index('} esp_gcm_context;')+len('} esp_gcm_context;')
types+=header[start:end]
source=re.sub(r'^#include.*$','',source,flags=re.M)
prefix=r'''
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#define bzero(p,n) memset(p,0,n)
#define ESP_LOGE(...) ((void)0)
#define AES_BLOCK_BYTES 16
#define ESP_AES_ENCRYPT 1
#define ESP_AES_DECRYPT 0
#define SOC_AES_SUPPORT_DMA 1
#define SOC_AES_SUPPORT_AES_192 0
#define CONFIG_MBEDTLS_AES_USE_INTERRUPT 0
#define CONFIG_MBEDTLS_HARDWARE_GCM 0
#define PSA_ERROR_NOT_SUPPORTED -134
#define PSA_ERROR_INVALID_ARGUMENT -135
#define PSA_ERROR_BAD_STATE -137
#define PSA_ERROR_INVALID_SIGNATURE -149
typedef struct {uint8_t key[32];size_t key_bytes;} esp_aes_context;
typedef int (*block_fn)(const uint8_t *,size_t,const uint8_t *,uint8_t *);
static block_fn block;
void set_block(block_fn callback){block=callback;}
static int esp_aes_crypt_ecb(esp_aes_context *c,int mode,const uint8_t *i,uint8_t *o){
 (void)mode;return block(c->key,c->key_bytes,i,o);
}
static int esp_aes_crypt_ctr(esp_aes_context *c,size_t n,size_t *off,uint8_t *counter,uint8_t *stream,const uint8_t *i,uint8_t *o){
 size_t b=*off;
 for(size_t at=0;at<n;at++){
  if(!b){int e=block(c->key,c->key_bytes,counter,stream);if(e)return e;
    for(int j=15;j>=0;j--)if(++counter[j])break;}
  o[at]=i[at]^stream[b];b=(b+1)&15;
 }
 *off=b;return 0;
}
'''
suffix=r'''
int audit_record(const uint8_t *key,unsigned key_bytes,const uint8_t *iv,size_t iv_bytes,
 const uint8_t *aad,size_t aad_bytes,const uint8_t *input,size_t bytes,uint8_t *output,int inplace){
 esp_gcm_context ctx;esp_aes_gcm_init(&ctx);
 int e=esp_aes_gcm_setkey(&ctx,0,key,key_bytes*8);
 if(inplace)memcpy(output,input,bytes);
 if(!e)e=esp_aes_gcm_crypt_and_tag(&ctx,ESP_AES_ENCRYPT,bytes,iv,iv_bytes,aad,aad_bytes,
  inplace?output:input,output,16,output+bytes);
 esp_aes_gcm_free(&ctx);return e;
}
'''
src=out/'actual-sdk-gcm.c';src.write_text(prefix+types+'\n'+source+suffix,encoding='utf-8')
dll=out/'actual-sdk-gcm.dll'
subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-variable','-Wno-unused-parameter','-Wno-sign-compare',
 '-shared',str(src),'-o',str(dll)],check=True)
lib=C.CDLL(str(dll));P=C.POINTER(C.c_uint8)
callback_type=C.CFUNCTYPE(C.c_int,P,C.c_size_t,P,P)
contexts={};errors=[]
@callback_type
def ecb(k,kb,i,o):
 try:
    key=C.string_at(k,kb)
    if key not in contexts:contexts[key]=Cipher(algorithms.AES(key),modes.ECB()).encryptor()
    cipher=contexts[key].update(C.string_at(i,16));C.memmove(o,cipher,16);return 0
 except Exception as e:errors.append(str(e));return -1
lib.set_block.argtypes=[callback_type];lib.set_block(ecb)
lib.audit_record.argtypes=[P,C.c_uint,P,C.c_size_t,P,C.c_size_t,P,C.c_size_t,P,C.c_int]
lib.audit_record.restype=C.c_int
def buf(data):return (C.c_uint8*max(len(data),1)).from_buffer_copy(data or b'\0')
count=0
for kb in (16,32):
 for n in (0,1,15,16,17,3896,3968,4096,4097,4098,8064,8136,8192,8193):
  for ad in (0,13,31):
   key=bytes((j*19+kb)%256 for j in range(kb));iv=bytes((j*7+n)%256 for j in range(12))
   aad=bytes((j*3+ad)%256 for j in range(ad));plain=bytes((j*17+n)%256 for j in range(n))
   expected=AESGCM(key).encrypt(iv,plain,aad)
   for inplace in (0,1):
    original=buf(plain);output=(C.c_uint8*(n+16))()
    assert lib.audit_record(buf(key),kb,buf(iv),12,buf(aad),ad,original,n,output,inplace)==0
    assert bytes(output)==expected,(kb,n,ad,inplace)
    assert bytes(original)[:n]==plain
    count+=1
assert not errors,errors
report={'source':str(sdk/'aes/esp_aes_gcm.c'),'sdkSourceSha256':hashlib.sha256((sdk/'aes/esp_aes_gcm.c').read_bytes()).hexdigest(),
 'recordsCompared':count,'inPlaceAndSeparate':True,'variedNonceAndAAD':True,
 'actualSDKGHASH':True,'aesBlockHardwareReplacedByTrustedECB':True,'realDMAHardwareValidated':False}
(out/'actual-sdk-gcm-result.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
print('PASS: actual ESP-IDF GCM source,',count,'AES128/256 records, varied nonce/AAD, exact in-place and separate buffers match AESGCM')
print('AES block primitive uses trusted ECB callback; real DMA/peripheral behavior is not emulated')
