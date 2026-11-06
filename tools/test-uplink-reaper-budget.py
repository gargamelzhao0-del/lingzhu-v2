"""Execute production cancellation loop and epoch-close helper with virtual time."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'main/conversation/audio_uplink.c').read_text(encoding='utf-8-sig')

def extract(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

prefix = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
typedef struct {
 void *task,*ws; bool finished,capture_done,send_failed;
 uint32_t ws_connection_epoch;
 atomic_bool integrity_close_requested;
 atomic_uint_fast32_t sender_stage_since_ms;
} nuwax_audio_uplink_t;
static nuwax_audio_uplink_t c;
static jmp_buf stop;
static uint32_t elapsed,origin,finish_at,progress_until,current_epoch,first_close;
static unsigned closes,restarts,attempts;
static bool healthy,refuse_close,finish_on_close,finish_at_heartbeat;
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define TAG "test"
#define pdMS_TO_TICKS(x) (x)
#define portMAX_DELAY 0xffffffffU
#define NUWAX_WS_CLOSE_CAUSE_TRANSPORT_INTEGRITY 1
static uint32_t audio_uplink_now_ms(void){return origin+elapsed;}
static bool audio_uplink_flag_load(bool *p){return *p;}
static void audio_uplink_flag_store(bool *p,bool v){*p=v;}
static void audio_uplink_notify_sender(nuwax_audio_uplink_t *p){(void)p;}
static bool nuwax_ws_heartbeat_healthy_if_epoch(void *p,uint32_t e){
 (void)p;if(finish_at_heartbeat)c.finished=true;return healthy && e==current_epoch;
}
static uint32_t nuwax_ws_connection_epoch(void *p){(void)p;return current_epoch;}
static bool nuwax_ws_close_if_epoch(void *p,uint32_t e,int cause){
 (void)p;(void)cause;attempts++;
 if(e!=current_epoch || refuse_close)return false;
 if(!closes)first_close=elapsed;
 closes++;if(finish_on_close)c.finished=true;return true;
}
static void audio_uplink_log_stalled_sender(nuwax_audio_uplink_t *p,const char *r,uint32_t t){(void)p;(void)r;(void)t;}
static void nuwax_diagnostics_log_heap_label(const char *a,const char *b){(void)a;(void)b;}
static void esp_restart(void){restarts++;longjmp(stop,1);}
static void vTaskDelay(uint32_t t){
 elapsed+=t;
 if(finish_at && elapsed>=finish_at)c.finished=true;
 if(progress_until && elapsed<=progress_until)atomic_store(&c.sender_stage_since_ms,audio_uplink_now_ms());
 if(elapsed>100000)longjmp(stop,2);
}
'''
tests = r'''
static void reset(void){
 c=(nuwax_audio_uplink_t){.task=(void*)1,.ws=(void*)1,.ws_connection_epoch=1};
 atomic_init(&c.integrity_close_requested,false);atomic_init(&c.sender_stage_since_ms,0);
 elapsed=origin=finish_at=progress_until=first_close=closes=restarts=attempts=0;
 current_epoch=1;healthy=true;refuse_close=finish_on_close=finish_at_heartbeat=false;
}
static void run(void){if(!setjmp(stop))worker(&c);}
#define CHECK(x) do{if(!(x)){fprintf(stderr,"failed line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(void){
 reset();run();CHECK(closes==1 && first_close==2000 && restarts==1 && elapsed==90100);
 reset();healthy=false;run();CHECK(closes==1 && first_close==300 && restarts==1);
 reset();finish_at=200;run();CHECK(!closes && !restarts && c.finished);
 reset();finish_at=1800;run();CHECK(!closes && !restarts && c.finished);
 reset();finish_at=4000;progress_until=4000;run();CHECK(!closes && !restarts && c.finished);
 reset();progress_until=100000;run();CHECK(first_close==5000 && closes==1 && restarts==1);
 reset();progress_until=1000;finish_on_close=true;run();CHECK(first_close==3000 && !restarts && c.finished);
 reset();finish_on_close=true;run();CHECK(first_close==2000 && closes==1 && !restarts);
 reset();current_epoch=2;run();CHECK(!closes && restarts==1 && elapsed==90100);
 reset();refuse_close=true;run();CHECK(!closes && attempts>1 && restarts==1);
 reset();c.ws=NULL;run();CHECK(!closes && !attempts && restarts==1);
 reset();c.ws_connection_epoch=0;run();CHECK(!closes && !attempts && restarts==1);
 reset();origin=UINT32_MAX-999;run();CHECK(first_close==2000 && restarts==1);
 reset();c.finished=true;run();CHECK(!closes && !restarts);
 reset();healthy=false;finish_at_heartbeat=true;run();CHECK(!closes && !restarts && c.finished);
 reset();finish_at=90000;run();CHECK(closes==1 && !restarts && c.finished);
 reset();finish_at=90100;run();CHECK(closes==1 && !restarts && c.finished);
 puts("PASS: 17 production reaper cases: healthy stall, progress grace/limit, stale epoch, rejected close, missing transport, completion races and clock wrap");
 return 0;
}
'''

def program(text):
    definitions = '\n'.join(re.findall(r'^#define NUWAX_AUDIO_UPLINK_REAPER_\w+_MS .*$', text, re.M))
    helper = extract(text, 'static bool audio_uplink_request_integrity_close(')
    begin = text.index('        const uint32_t cleanup_started_ms = audio_uplink_now_ms();')
    end = text.index('        /* A finished sender', begin)
    return definitions+'\n'+prefix+helper+'\nstatic void worker(nuwax_audio_uplink_t *ctx){\n'+text[begin:end]+'}\n'+tests

variants = [('production', source, True),
 ('heartbeat_veto', source.replace('(!heartbeat_healthy || cancel_stalled)', '!heartbeat_healthy'), False),
 ('no_absolute_cancel_limit', source.replace('cleanup_elapsed_ms >= NUWAX_AUDIO_UPLINK_REAPER_CANCEL_LIMIT_MS', 'false'), False),
 ('restart_requires_close', source.replace('if (cleanup_elapsed_ms >=\n                    NUWAX_AUDIO_UPLINK_REAPER_FATAL_RESTART_MS', 'if (integrity_close_requested && cleanup_elapsed_ms >=\n                    NUWAX_AUDIO_UPLINK_REAPER_FATAL_RESTART_MS'), False),
 ('early_healthy_close', source.replace('(!heartbeat_healthy || cancel_stalled)', 'true'), False)]
before = root/'docs/audits/2026-09-27-au05/audio_uplink.before.c'
if before.exists():
    variants.append(('before_au05', before.read_text(encoding='utf-8-sig'), False))
with tempfile.TemporaryDirectory(prefix='uplink-reaper-') as directory:
    cfile = Path(directory)/'check.c'
    exe = cfile.with_suffix('.exe')
    for name, text, expected in variants:
        if name not in ('production','before_au05'):
            assert text != source, f'mutation not applied: {name}'
        cfile.write_text(program(text), encoding='utf-8')
        subprocess.run(['gcc','-std=c11',str(cfile),'-o',str(exe)],check=True)
        result = subprocess.run([str(exe)],capture_output=True,text=True,timeout=10)
        assert (result.returncode == 0) == expected, name+result.stdout+result.stderr
        print(result.stdout.strip() if expected else 'PASS: rejected '+name+' ('+result.stderr.strip()+')')
