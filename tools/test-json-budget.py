"""Compile actual parser/session validation with the product nesting budget."""
from pathlib import Path
import re
import subprocess

root = Path(__file__).resolve().parents[1]
out = root / 'build-host-checks/json-budget'
out.mkdir(parents=True, exist_ok=True)
cjson = root / 'managed_components/espressif__cjson/cJSON'
for name in ['sdkconfig.defaults', 'sdkconfig.product-ota.defaults', 'sdkconfig.sd-replay-formal.defaults']:
    assert re.findall(r'^CONFIG_CJSON_NESTING_LIMIT=(\d+)$', (root/name).read_text(encoding='utf-8-sig'), re.M) == ['32'], name

code = r'''
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "session_protocol.h"
static unsigned allocated,peak;
static void *alloc(size_t n){void *p=malloc(n);if(p){allocated++;if(allocated>peak)peak=allocated;}return p;}
static void release(void *p){if(p){assert(allocated>0);allocated--;free(p);}}
static void nested(unsigned levels,bool expected){
 char p[4096]="{\"type\":\"pong\",\"extra\":";size_t n=strlen(p);
 for(unsigned i=0;i<levels;i++)p[n++]='[';
 p[n++]='0';for(unsigned i=0;i<levels;i++)p[n++]=']';p[n++]='}';p[n]=0;
 cJSON *r=cJSON_ParseWithLength(p,n);assert((r!=NULL)==expected);cJSON_Delete(r);assert(!allocated);
 assert(nuwax_session_validate_text_frame(p,(int)n)==expected);assert(!allocated);
}
static void valid(const char *s){assert(nuwax_session_validate_text_frame(s,(int)strlen(s)));assert(!allocated);}
int main(void){
 cJSON_Hooks hooks={alloc,release};cJSON_InitHooks(&hooks);
 nested(0,true);nested(1,true);nested(30,true);nested(31,true);
 nested(32,false);nested(256,false);nested(999,false);
 valid("{\"type\":\"pong\"}");
 valid("{\"type\":\"text_response\",\"assistant\":\"[{\\\"hello\\\"}]\\\\ still a string\"}");
 valid("{\"type\":\"playback_begin\",\"turn_id\":\"turn_1\",\"playback_id\":\"pb_1\",\"frames\":40}");
 const char *bad[]={"{", "{\"type\":\"pong\"}junk", "{\"type\":\"pong\",\"type\":\"pong\"}",
                     "{\"type\":\"pong\",\"extra\":[[}","{\"type\":\"pong\",\"extra\":\"unterminated}"};
 for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++){
  assert(!nuwax_session_validate_text_frame(bad[i],(int)strlen(bad[i])));assert(!allocated);
 }
 for(unsigned i=0;i<1000;i++){nested(256,false);valid("{\"type\":\"pong\"}");}
 printf("PASS: actual JSON/session validator depth boundary, escaped strings, malformed/duplicate/tail rejection, 1000 reject-then-pong cycles; no allocation leak; peak_nodes_and_strings=%u\n",peak);
}
'''
source = out/'check.c'
source.write_text(code, encoding='utf-8')
# Keep the production validation translation unit through its last validator;
# transport/board send functions below it are unrelated to ingress parsing.
validation = out/'validation.c'
validation.write_text((root/'main/session_protocol.c').read_text(encoding='utf-8-sig').split('static void log_text_preview(')[0], encoding='utf-8')
def build_and_run(limit, negative=False):
    exe = out / f'check-{limit}.exe'
    cmd=['gcc','-std=c11','-O0','-ffunction-sections','-fdata-sections',f'-DCJSON_NESTING_LIMIT={limit}',
         '-I',str(root/'main/session_protocol_host_stubs'),'-I',str(root/'main'),'-I',str(cjson),
         str(source),str(validation),str(cjson/'cJSON.c'),'-Wl,--gc-sections','-o',str(exe)]
    result=subprocess.run(cmd,capture_output=True,text=True)
    assert result.returncode==0,result.stderr
    result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=20)
    assert (result.returncode != 0)==negative,(limit,result.stdout,result.stderr)
    print('PASS: rejected unsafe 1000-level configuration' if negative else result.stdout.strip())
build_and_run(32)
build_and_run(1000,True)
