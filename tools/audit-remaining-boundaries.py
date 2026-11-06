"""Historical pre-AU05 reaper reproduction plus current JSON boundary audit.

For current reaper behavior run test-uplink-reaper-budget.py. Historical audit
evidence is immutable; reruns go to tmp rather than overwriting the dated record.
"""
from pathlib import Path
import subprocess
import re
import json
import hashlib

root=Path(__file__).resolve().parents[1]
out=root/'tmp/boundary-audit-rerun';out.mkdir(parents=True,exist_ok=True)
work=root/'build-host-checks/review-20260926';work.mkdir(parents=True,exist_ok=True)
uplink_path='docs/audits/2026-09-27-au05/audio_uplink.before.c'
uplink=(root/uplink_path).read_text(encoding='utf-8-sig')
start=uplink.index('        while (ctx->task && !audio_uplink_flag_load(&ctx->finished))')
end=uplink.index('        /* A finished sender',start)
loop=uplink[start:end]
defines='\n'.join(re.findall(r'^#define NUWAX_AUDIO_UPLINK_REAPER_(?:CANCEL_GRACE|DIAGNOSTIC_INTERVAL|FATAL_RESTART)_MS .*$',uplink,re.M))
code=r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <setjmp.h>
static jmp_buf done;
static uint32_t ms;static unsigned closes,restarts,notices;static bool healthy;
typedef struct {void *task,*ws;bool finished,capture_done,send_failed;uint32_t ws_connection_epoch;} context;
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define TAG "audit"
#define pdMS_TO_TICKS(x) (x)
#define portMAX_DELAY 0xffffffffU
static uint32_t audio_uplink_now_ms(void){return ms;}
static bool audio_uplink_flag_load(bool *p){return *p;}
static void audio_uplink_flag_store(bool *p,bool v){*p=v;}
static void audio_uplink_notify_sender(context *p){(void)p;notices++;}
static bool nuwax_ws_heartbeat_healthy_if_epoch(void *p,uint32_t e){(void)p;(void)e;return healthy;}
static bool audio_uplink_request_integrity_close(context *p,const char *r){(void)p;(void)r;closes++;return true;}
static void audio_uplink_log_stalled_sender(context *p,const char *r,uint32_t t){(void)p;(void)r;(void)t;}
static void nuwax_diagnostics_log_heap_label(const char *a,const char *b){(void)a;(void)b;}
static void esp_restart(void){restarts++;longjmp(done,1);}
static void vTaskDelay(uint32_t t){ms+=t;if(ms>=100000)longjmp(done,1);}
static void run(void){context c={.task=(void*)1,.ws=(void*)1,.ws_connection_epoch=1};context *ctx=&c;
const uint32_t cleanup_started_ms=0;uint32_t next_diagnostic_ms=2000;bool integrity_close_requested=false;
'''+loop+r'''
}
int main(void){for(int h=0;h<2;h++){healthy=h;ms=closes=restarts=notices=0;if(!setjmp(done))run();
printf("heartbeat=%d elapsed_ms=%u closes=%u restarts=%u cancellation_notifications=%u\n",h,ms,closes,restarts,notices);}}
'''
(work/'reaper.c').write_text(defines+'\n'+code,encoding='utf-8')
subprocess.run(['gcc','-std=c11',str(work/'reaper.c'),'-o',str(work/'reaper.exe')],check=True)
reaper=subprocess.check_output([str(work/'reaper.exe')],text=True,timeout=10)
assert 'heartbeat=1 elapsed_ms=100000 closes=0 restarts=0' in reaper
assert 'heartbeat=0 elapsed_ms=90100 closes=1 restarts=1' in reaper

cjson=root/'managed_components/espressif__cjson/cJSON'
session=(root/'main/session_protocol.c').read_text(encoding='utf-8-sig')
(work/'validation.c').write_text(session.split('static void log_text_preview(')[0],encoding='utf-8')
parser=r'''
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "session_protocol.h"
typedef union {max_align_t align;size_t n;} header;
static size_t live,peak,calls;
static void *alloc(size_t n){header *p=malloc(sizeof(*p)+n);if(!p)return NULL;p->n=n;live+=n;if(live>peak)peak=live;calls++;return p+1;}
static void release(void *v){if(v){header *p=(header*)v-1;live-=p->n;free(p);}}
int main(void){char p[16385]="{\"type\":\"pong\",\"extra\":[";size_t n=strlen(p);
for(int i=0;i<8000;i++){if(i)p[n++]=',';p[n++]='0';}p[n++]=']';p[n++]='}';p[n]=0;
cJSON_Hooks hooks={alloc,release};cJSON_InitHooks(&hooks);
int accepted=nuwax_session_validate_text_frame(p,(int)n);assert(n<=16384 && !accepted && !live && !calls);
printf("payload_bytes=%zu array_elements=8000 accepted=%d host_cjson_size=%zu peak_allocated_bytes=%zu allocations=%zu live_after_delete=%zu\n",n,accepted,sizeof(cJSON),peak,calls,live);
}
'''
(work/'wide.c').write_text(parser,encoding='utf-8')
subprocess.run(['gcc','-std=c11','-DCJSON_NESTING_LIMIT=32','-ffunction-sections','-fdata-sections',
 '-I'+str(root/'main/session_protocol_host_stubs'),'-I'+str(root/'main'),'-I'+str(cjson),
 str(work/'wide.c'),str(work/'validation.c'),str(cjson/'cJSON.c'),'-Wl,--gc-sections','-o',str(work/'wide.exe')],check=True)
wide=subprocess.check_output([str(work/'wide.exe')],text=True,timeout=10)
report=dict(reaper=reaper.strip(),wideJson=wide.strip(),hardwareValidated=False,
 sources={p:hashlib.sha256((root/p).read_bytes()).hexdigest() for p in [uplink_path,'main/session_protocol.c','main/ws_transport.c']})
(out/'boundary-experiments.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
print(reaper+wide)
