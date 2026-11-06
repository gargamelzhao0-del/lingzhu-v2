"""Execute the actual LVGL owner rebalance function with controlled heap/DMA."""
from pathlib import Path
import subprocess, re
root = Path(__file__).resolve().parents[1]
out = root/'build-host-checks/lvgl-memory-pressure'
out.mkdir(parents=True, exist_ok=True)
source = (root/'main/nuwax_lvgl_port.c').read_text(encoding='utf-8-sig')
start = source.index('static void lvgl_maybe_rebalance_internal_buffers(')
end = source.index('static void lvgl_flush_cb(', start)
constants = '\n'.join(re.findall(r'^#define (?:DOUBLE_BUFFER_\w+|LVGL_INTERNAL_DRAW_BUFFER_MIN_FREE) .*$',source,re.M))
support = r'''
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include "ui_memory_pressure.h"
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define DRAW_BUF_SIZE 14400U
#define DRAW_BUF_SPLIT_SIZE 7200U
#define DRAW_BUF_DOUBLE_BUFFER_ENABLE 1
#define LV_DISPLAY_RENDER_MODE_PARTIAL 0
#define ESP_ERR_NO_MEM -1
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
static int64_t s_double_buffer_last_health_us,s_double_buffer_restore_candidate_us,
    s_double_buffer_released_us,s_buffer_restore_last_report_us;
static nuwax_ui_memory_pressure_t s_double_buffer_pressure;
static bool s_draw_buf1_internal,s_draw_buf2_internal,s_runtime_internal_preferred,
    s_internal_restore_incident_active,flush_busy,locked,s_draw_buffers_split;
static uint8_t first[14400],second[14400],third[14400];
static uint8_t *s_draw_buf1,*s_draw_buf2,*s_driver_draw_buffer,*last_freed;
static void *s_display=(void*)1;
static size_t free_bytes,largest;
static unsigned releases,set_calls,warnings,loan_bytes,allocations;
static size_t post_alloc_loss;
static bool fail_alloc;
static unsigned nuwax_replay_dma_loan_bytes(uint32_t now){(void)now;return loan_bytes;}
static size_t heap_caps_get_free_size(int caps){(void)caps;return free_bytes;}
static size_t heap_caps_get_largest_free_block(int caps){(void)caps;return largest;}
static void *heap_caps_aligned_alloc(unsigned align,size_t bytes,int caps){
    (void)caps;assert(align==16 && bytes==14400 && free_bytes>=bytes);allocations++;
    if(fail_alloc)return NULL;
    free_bytes-=bytes+post_alloc_loss;return third;
}
static bool lvgl_flush_pipeline_take(unsigned wait){assert(wait==0 && !locked);locked=!flush_busy;return locked;}
static void lvgl_flush_pipeline_give(void){assert(locked);locked=false;}
static void *display_acquire_draw_buffer(size_t bytes){assert(locked && bytes==14400);return NULL;}
static void release_draw_buffer(void *p){if(!p)return;
    assert(p==first || p==third);last_freed=p;releases++;free_bytes+=14400;
}
static void release_draw_buffer_pair(void *a,void *b,bool split){release_draw_buffer(a);if(!split)release_draw_buffer(b);}
static void lv_display_set_buffers(void*d,void*a,void*b,size_t size,int mode){
    assert(d==s_display && a && locked && mode==0);
    if(size==7200)assert(a==second && b==second+7200);
    else assert(size==14400 && b!=second+7200);
    set_calls++;
}
static void report_ui_warning_detail(const char*a,int b,size_t c,size_t d,size_t e,size_t f){
    (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;warnings++;
}
'''
tests=r'''
static void reset(void){
    s_double_buffer_last_health_us=s_double_buffer_restore_candidate_us=s_double_buffer_released_us=0;
    s_buffer_restore_last_report_us=0;s_double_buffer_pressure=(nuwax_ui_memory_pressure_t){0};
    s_draw_buf1=first;s_draw_buf2=second;s_driver_draw_buffer=second;last_freed=NULL;
    s_draw_buf1_internal=s_draw_buf2_internal=s_runtime_internal_preferred=true;
    s_internal_restore_incident_active=flush_busy=locked=s_draw_buffers_split=fail_alloc=false;
    allocations=0;post_alloc_loss=0;
    free_bytes=18435;largest=10000;loan_bytes=8192;releases=set_calls=warnings=0;
}
int main(void){
    reset();lvgl_maybe_rebalance_internal_buffers(1000000);assert(!releases && s_double_buffer_pressure.deadline_us==1050000);
    // Ordinary 500 ms throttle must become a 10 ms retry during the loan.
    free_bytes+=8192;loan_bytes=0;lvgl_maybe_rebalance_internal_buffers(1010000);
    assert(!releases && !s_double_buffer_pressure.deadline_us && s_draw_buf2==second);
    // Repeated DMA loans do not renew sustained pressure indefinitely.
    reset();for(int64_t t=1000000;t<1050000;t+=10000)lvgl_maybe_rebalance_internal_buffers(t);
    assert(!releases);lvgl_maybe_rebalance_internal_buffers(1050000);
    assert(releases==1 && last_freed==first && s_draw_buf1==second && s_draw_buf2==second+7200 && s_draw_buffers_split && warnings==1 && !locked);
    // Critical/unrelated pressure retains the immediate release path.
    reset();free_bytes=16000;lvgl_maybe_rebalance_internal_buffers(1000000);assert(releases==1);
    reset();loan_bytes=0;lvgl_maybe_rebalance_internal_buffers(1000000);assert(releases==1);
    reset();loan_bytes=4096;lvgl_maybe_rebalance_internal_buffers(1000000);assert(releases==1);
    // An in-flight flush never loses its buffer, even after grace expiry.
    reset();lvgl_maybe_rebalance_internal_buffers(1000000);flush_busy=true;
    lvgl_maybe_rebalance_internal_buffers(1050000);assert(!releases);
    flush_busy=false;lvgl_maybe_rebalance_internal_buffers(1060000);assert(releases==1);
    // Previous 30-second cooldown and stable-headroom restoration survive.
    free_bytes=100000;largest=100000;loan_bytes=0;
    lvgl_maybe_rebalance_internal_buffers(2060000);assert(s_draw_buffers_split);
    lvgl_maybe_rebalance_internal_buffers(32060000);assert(s_draw_buffers_split);
    lvgl_maybe_rebalance_internal_buffers(34060000);assert(s_draw_buf2==third && !s_draw_buffers_split && set_calls==2 && !locked);
    // Fragmented 41 KB heap: recover two halves without allocating or lowering reserves.
    reset();s_draw_buf1=second;s_draw_buf2=NULL;free_bytes=41491;largest=12800;loan_bytes=0;
    flush_busy=true;lvgl_maybe_rebalance_internal_buffers(1000000);assert(!s_draw_buf2);
    flush_busy=false;lvgl_maybe_rebalance_internal_buffers(1500000);
    assert(s_draw_buffers_split && s_draw_buf2==second+7200 && !allocations && !releases && free_bytes==41491);
    // Even critical pressure must not free the interior pointer or the driver's lease.
    free_bytes=1000;lvgl_maybe_rebalance_internal_buffers(2000000);
    assert(s_draw_buffers_split && !releases);
    free_bytes=100000;largest=100000;fail_alloc=true;
    lvgl_maybe_rebalance_internal_buffers(3000000);lvgl_maybe_rebalance_internal_buffers(5000000);
    assert(s_draw_buffers_split && !releases);
    // Concurrent allocation after precheck: retain compact pair, reject the new buffer.
    fail_alloc=false;post_alloc_loss=60000;
    lvgl_maybe_rebalance_internal_buffers(6000000);lvgl_maybe_rebalance_internal_buffers(8000000);
    assert(s_draw_buffers_split && releases==1 && last_freed==third);
    free_bytes=100000;post_alloc_loss=0;
    lvgl_maybe_rebalance_internal_buffers(9000000);lvgl_maybe_rebalance_internal_buffers(11000000);
    assert(!s_draw_buffers_split && s_draw_buf2==third && !locked);
    // Sustained pressure on a restored pair returns to compact layout exactly once.
    free_bytes=10000;lvgl_maybe_rebalance_internal_buffers(12000000);
    assert(s_draw_buffers_split && last_freed==third && releases==2);
    lvgl_maybe_rebalance_internal_buffers(13000000);assert(releases==2);
    puts("PASS actual LVGL rebalance: transient DMA keeps both buffers; 10ms recheck, 50ms bound, critical/unrelated release, flush ownership and cooldown/restoration");
}
'''
(out/'owner.c').write_text(support+constants+'\n'+source[start:end]+tests,encoding='utf-8')
exe=out/'owner.exe'
subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(root/'main'),
    str(out/'owner.c'),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
