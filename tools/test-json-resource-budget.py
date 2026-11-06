"""Real parser allocations, randomized legal corpus and ingress isolation."""
from pathlib import Path
import json
import random
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
cjson = root/'managed_components/espressif__cjson/cJSON'
rng = random.Random(927)

def value(depth=0):
    if depth >= 4 or rng.randrange(3) == 0:
        return rng.choice([None, True, False, rng.randrange(-999999,999999),
                           '灵珠 "你好" \\ []{}', 'line\n\tend', 'x'*rng.randrange(200)])
    if rng.randrange(2):
        return [value(depth+1) for _ in range(rng.randrange(5))]
    return {f'key{i}':value(depth+1) for i in range(rng.randrange(5))}

corpus = [json.dumps(value(),ensure_ascii=True,separators=(',',':')) for _ in range(1000)]
# Existing field-size envelopes, separate from cryptographic/schema validation
# covered by the product suites. Escape expansion must not shrink compatibility.
envelopes = [
 {'type':'device_command','command':'ota_update','update_id':'o'*63,
  'url':'https://example.com/'+'a'*235,'version':'9'*31,'size':4194304,'sha256':'a'*64},
 {'device_id':'d'*39,'cloud_url':'wss://example.com/'+'a'*173,'device_secret':'s'*95,'version':1},
 {'type':'device_command','command':'set_wake_word','request_id':'r'*39,
  'wake_words':['x'*63 for _ in range(10)]},
 {'type':'text_response','text':'灵'*170},  # 510 UTF-8 bytes before JSON escaping
 {'type':'hello_ack','ok':True,'capabilities':{'offline_audio':{'version':2,'window_bytes':524288}}},
 {'type':'device_command','command':'set_emotion','request_id':'r'*39,'emotion':'happy','duration_ms':7000}
]
corpus += [json.dumps(v,ensure_ascii=True,separators=(',',':')) for v in envelopes]
prefix = r'''
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include "json_resource_budget.h"
#include "session_protocol.h"
typedef union {max_align_t align;size_t n;} header;
static size_t live,peak,calls,fail_at;
static void *alloc(size_t n){calls++;if(fail_at && calls==fail_at)return NULL;
 header *p=malloc(sizeof(*p)+n);if(!p)return NULL;p->n=n;live+=n;if(live>peak)peak=live;return p+1;}
static void release(void *v){if(v){header *p=(header*)v-1;live-=p->n;free(p);}}
static void reset(void){assert(!live);calls=peak=fail_at=0;}
static void valid(const char *s){reset();cJSON *p=nuwax_json_parse_with_length(s,strlen(s));
 assert(p && peak<=NUWAX_JSON_MAX_ESTIMATED_BYTES);cJSON_Delete(p);assert(!live);}
static void reject(const char *s,size_t n){reset();const char *end=(void*)1;
 assert(!nuwax_json_parse_with_length_opts(s,n,&end,false));assert(!calls && !live && end==s);}
'''
tests = r'''
int main(void){cJSON_Hooks hooks={alloc,release};cJSON_InitHooks(&hooks);
 char wide[16385]="{\"type\":\"pong\",\"extra\":[";size_t n=strlen(wide);
 for(int i=0;i<8000;i++){if(i)wide[n++]=',';wide[n++]='0';}wide[n++]=']';wide[n++]='}';wide[n]=0;
 reset();cJSON *old=cJSON_ParseWithLength(wide,n);assert(old && peak>500000);cJSON_Delete(old);assert(!live);
 printf("baseline wide JSON peak=%zu bytes\n",peak);
 reject(wide,n);assert(!nuwax_session_validate_text_frame(wide,(int)n));assert(!calls);
 for(size_t i=0;i<sizeof(corpus)/sizeof(corpus[0]);i++)valid(corpus[i]);
 valid("{\"type\":\"pong\"}");valid("{\"extra\":\"\\\"[]{}\\\\\\u4f60\\u597d\"}");
 valid("\xef\xbb\xbf{\"type\":\"pong\"}");
 valid("[true,false,null,-1.25e+100,18446744073709551615]");
 /* Full-size legitimate string payload remains supported. */
 wide[0]='"';memset(wide+1,'x',16382);wide[16383]='"';wide[16384]=0;valid(wide);
 reset();old=nuwax_json_parse_with_length_opts(wide,16385,NULL,true);assert(old);cJSON_Delete(old);assert(!live);
 /* Length guard must run before dereferencing oversized buffer lengths. */
 reject("x",(size_t)-1);reject(NULL,1);reject("",0);
 char a[2048];n=0;a[n++]='[';for(int i=0;i<256;i++){if(i)a[n++]=',';a[n++]='0';}a[n++]=']';a[n]=0;valid(a);
 a[n-1]=',';a[n++]='0';a[n++]=']';a[n]=0;reject(a,n);
 n=0;for(int i=0;i<32;i++)a[n++]='[';a[n++]='0';for(int i=0;i<32;i++)a[n++]=']';a[n]=0;valid(a);
 memmove(a+1,a,n);a[0]='[';a[++n]=']';a[++n]=0;reject(a,n);
 memset(a,'1',129);a[129]=0;reject(a,129);
 /* Many small containers hit total token budget, not per-container size. */
 n=0;a[n++]='[';for(int i=0;i<180;i++){if(i)a[n++]=',';memcpy(a+n,"[0,0]",5);n+=5;}a[n++]=']';a[n]=0;reject(a,n);
 const char *bad[]={"[}","{\"unterminated","[\"bad\n\"]","[\"tail\\", "{\"x\": [1,2,}","[0,garbage]"};
 for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);i++){reset();old=nuwax_json_parse_with_length(bad[i],strlen(bad[i]));assert(!old && !live);}
 const char embedded[]={ '[','0',0,']' };reject(embedded,sizeof(embedded));
 /* Decoded NUL must never truncate a value or a key. Escaped literal
  * backslashes and normal Unicode remain supported. */
 const char *nul[]={"{\"audio_session_id\":\"abc\\u0000def\"}",
                    "{\"audio_session_id\\u0000suffix\":\"abc\"}", "[\"\\u0000\"]"};
 for(size_t i=0;i<sizeof(nul)/sizeof(nul[0]);++i)reject(nul[i],strlen(nul[i]));
 valid("{\"text\":\"literal \\\\u0000\",\"id\":\"Cloud\\u005fnew\"}");
 for(int i=0;i<1000;i++){reject(wide,16385+1);valid("{\"type\":\"pong\"}");}
 /* Actual parser allocation failures release partial trees. */
 const char *allocation_case="{\"a\":[1,2,3],\"b\":\"x\"}";
 for(size_t i=1;i<20;i++){reset();fail_at=i;old=nuwax_json_parse_with_length(allocation_case,strlen(allocation_case));cJSON_Delete(old);assert(!live);}
 puts("PASS: real cJSON width/depth/byte/token guards, 1000 random legal fixtures plus six protocol field-size envelopes, escaped strings, allocation failures and reject-then-pong; rejected budgets allocate zero bytes");
}
'''

with tempfile.TemporaryDirectory(prefix='json-resources-') as d:
    work=Path(d)
    (work/'corpus.h').write_text('static const char *corpus[]={\n'+',\n'.join(json.dumps(s) for s in corpus)+'\n};',encoding='utf-8')
    (work/'test.c').write_text(prefix+'\n#include "corpus.h"\n'+tests,encoding='utf-8')
    (work/'validation.c').write_text((root/'main/session_protocol.c').read_text(encoding='utf-8-sig').split('static void log_text_preview(')[0],encoding='utf-8')
    command=['gcc','-std=c11','-DCJSON_NESTING_LIMIT=32','-ffunction-sections','-fdata-sections',
             '-I'+str(root/'main/session_protocol_host_stubs'),'-I'+str(root/'main'),'-I'+str(cjson),
             str(work/'test.c'),str(work/'validation.c'),str(cjson/'cJSON.c'),'-Wl,--gc-sections','-o',str(work/'test.exe')]
    subprocess.run(command,check=True)
    subprocess.run([str(work/'test.exe')],check=True,timeout=30)
    header=(root/'main/json_resource_budget.h').read_text(encoding='utf-8')
    (work/'json_resource_budget.h').write_text(header.replace('if (!nuwax_json_resource_admit(data, length))', 'if (false)'),encoding='utf-8')
    subprocess.run(command,check=True)
    negative=subprocess.run([str(work/'test.exe')],capture_output=True,timeout=30)
    assert negative.returncode!=0, 'unguarded parser mutant survived'
    (work/'json_resource_budget.h').unlink()
    print('PASS: rejected unguarded parser mutant')
    main_source=(root/'main/nuwax_ai_partner_main.c').read_text(encoding='utf-8-sig')
    start=main_source.index('static void on_ws_text(const char *payload')
    end=main_source.index('/* Keep the ordinary session command envelope',start)
    entry=main_source[start:end]
    dispatch_prefix=r'''
#include <stdatomic.h>
#define CONFIG_NUWAX_OTA_ENABLE 1
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define TAG "test"
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
typedef int nuwax_ws_client_t;
static atomic_uint s_ws_text_callbacks_active;
static int s_post_end_boundary_lock,ota_calls,ordinary_calls,activity;
static uint32_t nuwax_ws_connection_epoch(nuwax_ws_client_t *w){return *w;}
static void ws_mark_application_activity(void){activity++;}
static bool nuwax_ota_device_handle_text(const char *p,int n,uint32_t e){
 (void)e;ota_calls++;cJSON *v=nuwax_json_parse_with_length(p,(size_t)n);assert(v);cJSON_Delete(v);
 return strstr(p,"ota_result_ack")!=NULL;
}
static void on_ws_text_non_ota(const char *p,int n,nuwax_ws_client_t *w,uint32_t e){
 (void)w;(void)e;ordinary_calls++;cJSON *v=nuwax_json_parse_with_length(p,(size_t)n);assert(v);cJSON_Delete(v);activity++;
}
'''
    dispatch_test=r'''
int main(void){cJSON_Hooks hooks={alloc,release};cJSON_InitHooks(&hooks);int ws=1;
 char wide[16385]="[";size_t n=1;for(int i=0;i<8000;i++){if(i)wide[n++]=',';wide[n++]='0';}wide[n++]=']';wide[n]=0;
 for(int i=0;i<1000;i++){
   reset();int before=activity,ob=ota_calls,nb=ordinary_calls;
   on_ws_text(wide,(int)n,&ws);
   assert(!calls && !live && activity==before && ota_calls==ob && ordinary_calls==nb);
   on_ws_text("{\"type\":\"pong\"}",15,&ws);assert(activity==before+1 && ordinary_calls==nb+1 && !live);
   const char *ack="{\"type\":\"ota_result_ack\"}";on_ws_text(ack,(int)strlen(ack),&ws);
   assert(activity==before+2 && ordinary_calls==nb+1 && !live && !atomic_load(&s_ws_text_callbacks_active));
 }
 puts("PASS: real WS dispatch: 1000 oversized-frame -> pong -> OTA cycles; over-budget frames reach no parser/liveness callback; callback counter balanced");
}
'''
    # Session header defines the WS type in this harness, so omit it here.
    ingress_prefix=prefix.replace('#include "session_protocol.h"','')
    (work/'dispatch.c').write_text(ingress_prefix+dispatch_prefix+entry+dispatch_test,encoding='utf-8')
    cmd=['gcc','-std=c11','-DCJSON_NESTING_LIMIT=32','-I'+str(root/'main'),'-I'+str(cjson),
         str(work/'dispatch.c'),str(cjson/'cJSON.c'),'-o',str(work/'dispatch.exe')]
    subprocess.run(cmd,check=True)
    subprocess.run([str(work/'dispatch.exe')],check=True,timeout=30)

# Every project parser call, including direct non-WS entries, must be budgeted.
count=0
for p in (root/'main').rglob('*.c'):
    if 'host' in str(p) or '_archive' in str(p):continue
    s=p.read_text(encoding='utf-8-sig')
    assert not re.search(r'\bcJSON_Parse\w*\s*\(',s),p
    if 'nuwax_json_parse_with_length' in s:
        assert '#include "json_resource_budget.h"' in s,p
        count+=len(re.findall(r'nuwax_json_parse_with_length(?:_opts)?\(',s))
main=(root/'main/nuwax_ai_partner_main.c').read_text(encoding='utf-8-sig')
entry=main[main.index('static void on_ws_text(const char *payload'):]
assert entry.index('nuwax_json_resource_admit') < entry.index('nuwax_ota_device_handle_text')
# Removing the SD hello capability observer removes one JSON entry point.
# Every remaining parser still undergoes the per-file budget checks above.
assert count>=13,count
print(f'PASS: {count} production parse calls budgeted; shared WS gate precedes OTA and ordinary dispatch')
