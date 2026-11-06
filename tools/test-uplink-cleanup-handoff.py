"""Execute production handoff functions with injected scheduling; no hardware claim."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'main/conversation/audio_uplink.c').read_text(encoding='utf-8-sig')

def extract(s, signature):
    start = s.index(signature)
    end = s.index('{', start) + 1
    depth = 1
    while depth:
        depth += (s[end] == '{') - (s[end] == '}')
        end += 1
    return s[start:end]

queue = extract(source, 'static void audio_uplink_queue_detached_cleanup(')
take = extract(source, 'static nuwax_audio_uplink_t *audio_uplink_take_detached_cleanup(')
destroy = extract(source, 'esp_err_t nuwax_audio_uplink_destroy_owned(')
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_TIMEOUT 1
#define ESP_ERR_NOT_FINISHED 2
#define NUWAX_AUDIO_UPLINK_REAPER_CANCEL_GRACE_MS 250
#define pdTRUE 1
typedef struct nuwax_audio_uplink {
 struct nuwax_audio_uplink *cleanup_next;
 void *task; uint32_t ws_connection_epoch;
 bool finished, capture_done, send_failed;
} nuwax_audio_uplink_t;
static nuwax_audio_uplink_t *s_audio_uplink_cleanup_head, *s_audio_uplink_cleanup_tail;
static int s_audio_uplink_cleanup_lock, s_audio_uplink_reaper_task;
static int notices, frees, destroyed, lock_depth;
static bool delete_ok, immediate;
static unsigned long logged_epoch;
static nuwax_audio_uplink_t **caller_owner;
static void after_unlock(void);
static nuwax_audio_uplink_t *audio_uplink_take_detached_cleanup(void);
#define portENTER_CRITICAL(p) do { (void)(p); assert(lock_depth++==0); } while(0)
#define portEXIT_CRITICAL(p) do { (void)(p); assert(--lock_depth==0); after_unlock(); } while(0)
#define TAG "test"
static void log_event(const char *tag, const char *fmt, ...) {
 (void)tag; if(!strstr(fmt,"epoch=%lu"))return;
 va_list ap;va_start(ap,fmt);logged_epoch=va_arg(ap,unsigned long);va_end(ap);
}
#define ESP_LOGW(...) log_event(__VA_ARGS__)
#define ESP_LOGE(...) ((void)0)
static bool audio_uplink_flag_load(bool *f) {return *f;}
static void audio_uplink_flag_store(bool *f,bool v) {*f=v;}
static void audio_uplink_notify_sender(nuwax_audio_uplink_t *c) {(void)c;}
static bool audio_uplink_delete_task(nuwax_audio_uplink_t *c) {if(delete_ok)c->task=NULL;return delete_ok;}
static void nuwax_audio_uplink_destroy(nuwax_audio_uplink_t *c) {assert(!c->task);destroyed++;}
static void heap_caps_free(void *p) {(void)p;frees++;}
static void xTaskNotifyGive(int t) {(void)t;notices++;}
static void report_audio_uplink_fault(const char *s,int e) {(void)s;(void)e;}
static int s_audio_uplink_reaper_queue=1;
static int xQueueSend(int q,void *p,int wait) {(void)q;(void)p;(void)wait;return 0;}
'''
tests = r'''
static void after_unlock(void) {
 if(immediate) {
  immediate=false;
  assert(caller_owner && *caller_owner==NULL);
  nuwax_audio_uplink_t *p=audio_uplink_take_detached_cleanup(); assert(p);
  /* Simulate reaper reuse immediately after publication. Caller must only
   * use its saved epoch, never read the transferred object for logging. */
  p->ws_connection_epoch=0xdead;
 }
}
int main(void) {
 nuwax_audio_uplink_t a={.task=(void*)1,.ws_connection_epoch=42}, b={.task=(void*)2,.ws_connection_epoch=43};
 nuwax_audio_uplink_t *owner=&a;
 assert(nuwax_audio_uplink_destroy_owned(&owner)==ESP_ERR_NOT_FINISHED);
 assert(!owner && a.capture_done && a.send_failed && !frees);
 assert(nuwax_audio_uplink_destroy_owned(&owner)==ESP_OK);
 owner=&b; assert(nuwax_audio_uplink_destroy_owned(&owner)==ESP_ERR_NOT_FINISHED);
 assert(audio_uplink_take_detached_cleanup()==&a);
 assert(audio_uplink_take_detached_cleanup()==&b);
 assert(!audio_uplink_take_detached_cleanup() && !s_audio_uplink_cleanup_tail);
 a.finished=true;owner=&a;delete_ok=false;
 assert(nuwax_audio_uplink_destroy_owned(&owner)==ESP_ERR_NOT_FINISHED);
 assert(!owner && audio_uplink_take_detached_cleanup()==&a && !frees);
 owner=&a;delete_ok=true;assert(nuwax_audio_uplink_destroy_owned(&owner)==ESP_OK);
 assert(!owner && frees==1 && destroyed==1);
 owner=&a;assert(nuwax_audio_uplink_destroy_owned(&owner)==ESP_OK);assert(frees==2);
 for(int finished=0;finished<2;finished++) {
  a.task=(void*)1;a.finished=finished;a.ws_connection_epoch=42;delete_ok=false;
  owner=&a;caller_owner=&owner;immediate=true;
  assert(nuwax_audio_uplink_destroy_owned(&owner)==ESP_ERR_NOT_FINISHED);
  assert(!owner && logged_epoch==42);
 }
 assert(notices==5 && lock_depth==0);
 puts("PASS: production allocation-free handoff, FIFO, repeated cleared owner, deferred deletion, immediate reaper and saved identity");
}
'''
variants = [('production', queue, destroy, True),
            ('owner_cleared_too_late', queue, destroy.replace('*owner = NULL;\n            audio_uplink_queue_detached_cleanup(ctx);', 'audio_uplink_queue_detached_cleanup(ctx);\n            *owner = NULL;'), False),
            ('read_after_transfer', queue, destroy.replace('(unsigned long)epoch', '(unsigned long)ctx->ws_connection_epoch'), False),
            ('lost_fifo_tail', queue.replace('s_audio_uplink_cleanup_tail->cleanup_next = ctx;', '(void)ctx;'), destroy, False)]
before = root / 'tools/fixtures/uplink-cleanup-before-au04.c'
if before.exists():
    old = before.read_text(encoding='utf-8-sig')
    variants.append(('old_queue_rejection', extract(old, 'static bool audio_uplink_queue_detached_cleanup('), extract(old, 'esp_err_t nuwax_audio_uplink_destroy_owned('), False))
with tempfile.TemporaryDirectory(prefix='uplink-handoff-') as d:
    p=Path(d)/'check.c';exe=p.with_suffix('.exe')
    for name,q,fn,expected in variants:
        p.write_text(prefix+take+q+fn+tests, encoding='utf-8')
        subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Wno-unused-function','-Wno-unused-variable',str(p),'-o',str(exe)],check=True)
        result=subprocess.run([str(exe)],capture_output=True,text=True)
        assert (result.returncode==0)==expected, name+result.stdout+result.stderr
        print(result.stdout.strip() if expected else 'PASS: rejected '+name)

# Actual worker checks list before waiting, and startup provisions worker first.
worker=extract(source,'static void audio_uplink_detached_cleanup_task(')
assert worker.index('audio_uplink_take_detached_cleanup()') < worker.index('ulTaskNotifyTake')
assert 'xQueueSend(s_audio_uplink_reaper_queue' not in source
