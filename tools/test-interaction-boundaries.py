"""Execute production emotion publication and QMI polling with fault injection."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]


def function(text, signature):
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


app = (root / 'main/nuwax_ai_partner_main.c').read_text(encoding='utf-8-sig')
qmi = (root / 'main/board/qmi8658a_driver.c').read_text(encoding='utf-8-sig')
common = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#define ESP_LOGI(...) ((void)0)
#define portENTER_CRITICAL(...) ((void)0)
#define portEXIT_CRITICAL(...) ((void)0)
'''
emotion = common + r'''
#include "conversation/emotion_ui_pending.h"
#define NUWAX_APP_STATE_SLEEPING 1
static nuwax_emotion_ui_pending_t s_emotion_ui_sync;
static bool ready=true, fail, nested, replace, invalidate;
static unsigned sets, clears;
static uint32_t owner;
static void sync_pending_semantic_emotion(void);
static bool ui_ready_snapshot(void) {return ready;}
static int conversation_state_current(void) {return 0;}
static bool nuwax_state_bridge_refresh_semantic_emotion_name_owned_lease(
 const char *e,const char *s,uint32_t r,uint32_t d) {(void)e;(void)s;(void)r;(void)d;return false;}
static bool nuwax_state_bridge_set_semantic_emotion_name_owned_leased(
 const char *e,const char *s,uint32_t r,uint32_t d,bool f) {
 (void)e;(void)s;(void)d;(void)f; sets++;
 if(nested) {nested=false;sync_pending_semantic_emotion();}
 if(replace) {replace=false;nuwax_emotion_ui_submit(&s_emotion_ui_sync,"tired","cloud");}
 if(invalidate) {invalidate=false;nuwax_emotion_ui_invalidate(&s_emotion_ui_sync);}
 if(fail)return false;
 owner=r;return true;
}
static bool nuwax_state_bridge_clear_semantic_emotion_if_owner(const char *s,uint32_t r) {
 (void)s; if(owner!=r)return false;owner=0;clears++;return true;
}
'''
emotion += function(app, 'static void sync_pending_semantic_emotion(void)')
emotion += r'''
int main(void) {
 uint32_t r=nuwax_emotion_ui_submit(&s_emotion_ui_sync,"excited","text_response_pcm");
 nested=true;sync_pending_semantic_emotion();
 assert(sets==1 && clears==0 && owner==r && !s_emotion_ui_sync.pending);
 sync_pending_semantic_emotion();assert(sets==1 && owner==r);
 assert(nuwax_emotion_ui_revision_current(&s_emotion_ui_sync,r));
 assert(!nuwax_emotion_ui_matches(&s_emotion_ui_sync,r));
 nuwax_emotion_ui_submit(&s_emotion_ui_sync,"happy","cloud");
 replace=true;sync_pending_semantic_emotion();
 assert(owner==0 && clears==1 && s_emotion_ui_sync.pending);
 r=s_emotion_ui_sync.revision;sync_pending_semantic_emotion();
 assert(owner==r && !s_emotion_ui_sync.pending);
 nuwax_emotion_ui_submit(&s_emotion_ui_sync,"happy","cloud");
 invalidate=true;sync_pending_semantic_emotion();assert(!owner && clears==2);
 r=nuwax_emotion_ui_submit(&s_emotion_ui_sync,"playful","cloud");
 fail=true;sync_pending_semantic_emotion();assert(s_emotion_ui_sync.pending);
 fail=false;ready=false;sync_pending_semantic_emotion();assert(s_emotion_ui_sync.pending);
 ready=true;sync_pending_semantic_emotion();assert(owner==r && !s_emotion_ui_sync.pending);
 puts("PASS: nested publishers, completion vs revocation, replacement, cancel, retry");
}
'''
poll = common + r'''
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_TIMEOUT 2
#define ESP_ERR_NOT_FINISHED 3
#define QMI8658A_SAMPLE_TIMEOUT_MS 10
#define QMI8658A_SAMPLE_LOCK_TIMEOUT_MS 10U
static struct {void *dev;} s_qmi={(void *)1};
static uint8_t s_qmi_tx_scratch[2],s_qmi_rx_scratch[12];
static bool lock_fail,transfer_fail,held;
static unsigned releases,transfers;
static int nuwax_i2c_bus_transaction_begin(unsigned ms) {
 assert(ms==10 && !held);if(lock_fail)return ESP_ERR_TIMEOUT;held=true;return 0;
}
static void nuwax_i2c_bus_sample_transfer_done(int err) {assert(held);(void)err;}
static void nuwax_i2c_bus_transaction_end(void) {assert(held);held=false;releases++;}
static int i2c_master_transmit_receive(void *d,const void *tx,size_t n,void *rx,size_t len,int ms) {
 (void)d;(void)tx;assert(n==1 && ms==10 && held);transfers++;memset(rx,0x42,len);
 return transfer_fail ? ESP_ERR_TIMEOUT : ESP_OK;
}
'''
poll += function(qmi, 'static esp_err_t read_regs(')
poll += r'''
int main(void) {
 uint8_t out[12];memset(out,0xa5,sizeof out);
 lock_fail=true;assert(read_regs(0x35,out,12)==ESP_ERR_NOT_FINISHED);assert(!transfers&&!releases);
 lock_fail=false;transfer_fail=true;assert(read_regs(0x35,out,12)==ESP_ERR_TIMEOUT);
 assert(!held && releases==1 && out[0]==0xa5);
 transfer_fail=false;assert(read_regs(0x35,out,12)==ESP_OK);
 assert(!held && releases==2 && out[0]==0x42 && out[11]==0x42);
 assert(read_regs(0x35,out,13)==ESP_ERR_INVALID_ARG);
 puts("PASS: bounded QMI polling, lock miss, transfer error cleanup, valid sample");
}
'''
assert '#define QMI8658A_SAMPLE_TIMEOUT_MS 10' in qmi
with tempfile.TemporaryDirectory(prefix='nuwax-interaction-') as directory:
    for name, code in [('emotion', emotion), ('qmi', poll)]:
        source = Path(directory) / (name + '.c')
        executable = source.with_suffix('.exe')
        source.write_text(code, encoding='utf-8')
        subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra',
                        '-Wno-unused-variable', '-I' + str(root / 'main'),
                        str(source), '-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
