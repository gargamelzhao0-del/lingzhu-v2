"""Execute production provisioning buffer migration and reject broken variants."""
from pathlib import Path
import hashlib
import json
import subprocess

root = Path(__file__).resolve().parents[1]
port = (root/'main/nuwax_lvgl_port.c').read_text(encoding='utf-8-sig')
display = (root/'main/display.c').read_text(encoding='utf-8-sig')
out = root/'build-host-checks/w01/provisioning-buffers'
out.mkdir(parents=True, exist_ok=True)


def function(text, signature):
    start = text.index(signature); end = text.index('{', start)+1; depth = 1
    while depth:
        depth += (text[end]=='{')-(text[end]=='}');end += 1
    return text[start:end]


prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
typedef int lv_obj_t;
#define CONFIG_NUWAX_DISPLAY_QSPI_ENABLE 1
#define ST77916_TX_BUFFER_PIXELS 7200U
#define DRAW_BUF_SIZE 14400U
#define DRAW_BUF_DOUBLE_BUFFER_ENABLE 1
#define LVGL_INTERNAL_SINGLE_BUFFER_MIN_FREE 12000U
#define LVGL_INTERNAL_DRAW_BUFFER_MIN_FREE 24000U
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_SPIRAM 4
#define LV_DISPLAY_RENDER_MODE_PARTIAL 0
#define FLUSH_PIPELINE_CONTROL_TIMEOUT_MS 100
#define ESP_OK 0
#define ESP_ERR_NO_MEM -1
#define ESP_ERR_INVALID_STATE -2
#define ESP_ERR_TIMEOUT -3
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define report_ui_warning(...) ((void)0)
static uint16_t tx[7200],*s_tx_buffer=tx;
static bool s_initialized=true,s_tx_buffer_leased;
static uint8_t heap[8][DRAW_BUF_SIZE];
static bool allocated[8];
static unsigned allocations,fail_allocation,frees,changes;
static uint8_t *s_draw_buf1,*s_draw_buf2,*s_driver_draw_buffer,*bound1,*bound2;
static bool s_draw_buf1_internal,s_draw_buf2_internal,s_runtime_internal_preferred,s_draw_buffers_split;
static int64_t s_double_buffer_restore_candidate_us,s_double_buffer_released_us;
static void *s_display=(void*)1,*s_lvgl_mutex=(void*)1;
static bool fail_lock,fail_pipeline,locked,pipeline;
static size_t free_bytes=100000,largest=100000;
static size_t heap_caps_get_free_size(int caps) {(void)caps;return free_bytes;}
static size_t heap_caps_get_largest_free_block(int caps) {(void)caps;return largest;}
static void *heap_caps_aligned_alloc(size_t alignment,size_t size,int caps) {
 (void)alignment;(void)caps;assert(size==DRAW_BUF_SIZE);
 allocations++;if(allocations==fail_allocation)return NULL;
 for(unsigned i=0;i<8;i++)if(!allocated[i]){allocated[i]=true;return heap[i];}
 assert(false);return NULL;
}
static void heap_caps_free(void *p) {
 assert(p!=(void*)tx && p!=bound1 && p!=bound2);
 for(unsigned i=0;i<8;i++)if(p==heap[i]){assert(allocated[i]);allocated[i]=false;frees++;return;}
 assert(false);
}
static bool nuwax_lvgl_port_lock(unsigned ms) {(void)ms;assert(!locked);locked=!fail_lock;return locked;}
static void nuwax_lvgl_port_unlock(void) {assert(locked && !pipeline);locked=false;}
static bool lvgl_flush_pipeline_take(unsigned ms) {(void)ms;assert(locked&&!pipeline);pipeline=!fail_pipeline;return pipeline;}
static void lvgl_flush_pipeline_give(void) {assert(locked&&pipeline);pipeline=false;}
static void lv_display_set_buffers(void *d,void *a,void *b,unsigned n,int mode) {
 (void)d;(void)mode;assert(locked&&pipeline&&a&&a!=b&&n==DRAW_BUF_SIZE);
 bound1=a;bound2=b;changes++;
}
static lv_obj_t *lv_screen_active(void) {return NULL;}
static void lv_obj_invalidate(lv_obj_t *obj) {(void)obj;}
'''
production = '\n'.join(function(display, name) for name in
                       ['void *display_acquire_draw_buffer(', 'bool display_release_draw_buffer('])
production += '\n'+function(port,'static void release_draw_buffer(')
production += '\n'+function(port,'static void release_draw_buffer_pair(')
production += '\n'+function(port,'esp_err_t nuwax_lvgl_port_set_runtime_provisioning_memory(')
tests = r'''
static unsigned live(void){unsigned n=0;for(unsigned i=0;i<8;i++)n+=allocated[i];return n;}
static void setup(void) {
 memset(allocated,0,sizeof allocated);allocations=frees=changes=fail_allocation=0;
 fail_lock=fail_pipeline=locked=pipeline=false;free_bytes=largest=100000;
 s_draw_buffers_split=false;s_tx_buffer_leased=false;s_initialized=true;s_display=s_lvgl_mutex=(void*)1;
 allocated[0]=true;s_draw_buf1=bound1=heap[0];
 s_draw_buf2=s_driver_draw_buffer=bound2=display_acquire_draw_buffer(DRAW_BUF_SIZE);
 s_draw_buf1_internal=s_draw_buf2_internal=s_runtime_internal_preferred=true;
}
static void unchanged(void) {
 assert(s_draw_buf1==heap[0] && s_draw_buf2==(uint8_t*)tx && live()==1);
 assert(s_tx_buffer_leased && !changes && !locked && !pipeline);
}
int main(void) {
 setup();assert(nuwax_lvgl_port_set_runtime_provisioning_memory(false)==ESP_OK);
 unchanged();assert(!allocations);
 setup();fail_allocation=1;
 assert(nuwax_lvgl_port_set_runtime_provisioning_memory(true)==ESP_ERR_NO_MEM);unchanged();
 setup();fail_lock=true;
 assert(nuwax_lvgl_port_set_runtime_provisioning_memory(true)==ESP_ERR_TIMEOUT);unchanged();assert(frees==2);
 setup();fail_pipeline=true;
 assert(nuwax_lvgl_port_set_runtime_provisioning_memory(true)==ESP_ERR_TIMEOUT);unchanged();assert(frees==2);
 setup();assert(nuwax_lvgl_port_set_runtime_provisioning_memory(true)==ESP_OK);
 assert(live()==2 && !s_tx_buffer_leased && !s_driver_draw_buffer && frees==1);
 assert(s_draw_buf1==bound1 && s_draw_buf2==bound2 && !s_draw_buf1_internal);
 unsigned count=allocations;
 assert(nuwax_lvgl_port_set_runtime_provisioning_memory(true)==ESP_OK && allocations==count);
 free_bytes=1000;
 assert(nuwax_lvgl_port_set_runtime_provisioning_memory(false)==ESP_ERR_NO_MEM);
 assert(s_runtime_internal_preferred && live()==2 && !locked && !pipeline);
 free_bytes=100000;
 assert(nuwax_lvgl_port_set_runtime_provisioning_memory(false)==ESP_OK);
 assert(live()==1 && frees==3 && s_tx_buffer_leased && s_draw_buf2==(uint8_t*)tx);
 assert(s_draw_buf1_internal && s_draw_buf2_internal && !locked && !pipeline);
 setup();fail_allocation=2;
 assert(nuwax_lvgl_port_set_runtime_provisioning_memory(true)==ESP_OK);
 assert(live()==1 && !s_draw_buf2 && !s_tx_buffer_leased && frees==1);
 setup();allocated[0]=false;s_draw_buf1=bound1=(uint8_t*)tx;
 s_draw_buf2=bound2=(uint8_t*)tx+DRAW_BUF_SIZE/2;s_draw_buffers_split=true;
 assert(nuwax_lvgl_port_set_runtime_provisioning_memory(true)==ESP_OK);
 assert(live()==2&&!s_tx_buffer_leased&&!s_draw_buffers_split&&frees==0);
 setup();s_draw_buf2=bound2=s_draw_buf1+DRAW_BUF_SIZE/2;s_draw_buffers_split=true;
 assert(display_release_draw_buffer(s_driver_draw_buffer));s_driver_draw_buffer=NULL;
 assert(nuwax_lvgl_port_set_runtime_provisioning_memory(true)==ESP_OK);
 assert(live()==2&&!s_draw_buffers_split&&frees==1);
 puts("PASS: production provisioning migration; allocation/lock/DMA failures preserve old buffers, borrowed return, idempotence, degraded single buffer and restore");
}
'''
variants = {
    'production': production,
    'missing-owned-release': production.replace('release_draw_buffer(first);','(void)first;'),
    'double-owned-release': production.replace('release_draw_buffer(first);','release_draw_buffer(first);release_draw_buffer(first);'),
    'free-borrowed-buffer': production.replace('if (!split) release_draw_buffer(second);','if (!split) heap_caps_free(second);'),
    'skip-dma-fence': production.replace('if (!lvgl_flush_pipeline_take(FLUSH_PIPELINE_CONTROL_TIMEOUT_MS))','if (false)'),
}
results = {}
for name, code in variants.items():
    if name != 'production': assert code != production, 'mutation did not match current source: '+name
    source=out/(name+'.c');exe=out/(name+'.exe')
    source.write_text(prefix+code+tests,encoding='utf-8')
    subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Wno-unused-function',str(source),'-o',str(exe)],check=True)
    result=subprocess.run([str(exe)],capture_output=True,text=True)
    (out/(name+'.txt')).write_text(result.stdout+result.stderr,encoding='utf-8')
    assert (result.returncode==0)==(name=='production'),(name,result.stdout,result.stderr)
    results[name]=dict(exit_code=result.returncode,expected_failure=name!='production')
    print(f"PASS: {name} {'passed' if name=='production' else 'rejected'}")
(out/'report.json').write_text(json.dumps(dict(results=results,
    port_sha256=hashlib.sha256(port.encode()).hexdigest(),hardware_validated=False),indent=2),encoding='utf-8')
