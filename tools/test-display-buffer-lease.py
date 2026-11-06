"""Exercise production DMA lease and copy fallback without hardware."""
from pathlib import Path
import subprocess,json,hashlib
root=Path(__file__).resolve().parents[1]
src=(root/'main/display.c').read_text(encoding='utf-8')
port=(root/'main/nuwax_lvgl_port.c').read_text(encoding='utf-8')
def extract(s, signature):
 a=s.index(signature); b=s.index('{',a); n=1; e=b+1
 while n:
  n+=(s[e]=='{')-(s[e]=='}');e+=1
 return s[a:e]
out=root/'build-host-checks/display-buffer-lease';out.mkdir(parents=True,exist_ok=True)
prefix=r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define CONFIG_NUWAX_DISPLAY_QSPI_ENABLE 1
#define CONFIG_NUWAX_UI_PERF_LOG_ENABLE 0
#define CONFIG_NUWAX_DISPLAY_PIN_CS 0
#define ST77916_LINE_BUFFER_PIXELS 360U
#define ST77916_TX_BUFFER_PIXELS 7200U
#define ST77916_SPI_SAFE_TRANSFER_PIXELS 15360U
#define ST77916_INTERRUPT_TRANSFER_MIN_BYTES 2048U
#define ST77916_FLUSH_YIELD_CHUNKS 8U
#define ST77916_QSPI_WRITE_PIXELS_CMD 0x32
#define ST77916_QSPI_WRITE_PIXELS_ADDR 0x003c00
#define SPI_TRANS_MODE_QIO 1
#define SPI_TRANS_VARIABLE_CMD 2
#define SPI_TRANS_VARIABLE_ADDR 4
#define SPI_TRANS_VARIABLE_DUMMY 8
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG -1
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
typedef int esp_err_t;
static _Alignas(16) uint16_t tx[7202],line[362];
static uint16_t *s_tx_buffer=tx,*s_line_buffer=line;
static bool s_tx_buffer_leased,s_initialized=true;
static int s_spi;
static unsigned chunks,max_bytes,wire_count;
static uint16_t wire[65536];
typedef struct {size_t length;const void *tx_buffer;int flags,cmd,addr;} spi_transaction_t;
typedef struct {spi_transaction_t base;int command_bits,address_bits,dummy_bits;} spi_transaction_ext_t;
static int spi_device_transmit(int spi,spi_transaction_t *t) {
 (void)spi;assert(t->tx_buffer==(s_tx_buffer_leased?(void *)line:(void *)tx));
 unsigned bytes=t->length/8;assert(wire_count+bytes/2<65536);
 memcpy(wire+wire_count,t->tx_buffer,bytes);wire_count+=bytes/2;chunks++;
 if(bytes>max_bytes)max_bytes=bytes;
 return ESP_OK;
}
static int spi_device_polling_transmit(int spi,spi_transaction_t *t) {return spi_device_transmit(spi,t);}
static void gpio_set_level(int pin,int value) {(void)pin;(void)value;}
static void taskYIELD(void) {}
static uint8_t *s_driver_draw_buffer;
static unsigned frees;
static void heap_caps_free(void *p) {assert(p!=tx);frees++;}
"""
functions='\n'.join(extract(src,s) for s in ['void *display_acquire_draw_buffer(', 'bool display_release_draw_buffer(', 'static void copy_pixels_byte_swapped(', 'static esp_err_t spi_write_pixels_strided('])
functions+='\n'+extract(port,'static void release_draw_buffer(')
tests=r"""
static uint16_t source[400*100],original[7200];
static void check(const uint16_t *src,size_t w,size_t h,size_t stride) {
 wire_count=chunks=max_bytes=0;
 assert(spi_write_pixels_strided(src,w,h,stride)==ESP_OK);
 assert(wire_count==w*h);
 for(size_t y=0;y<h;y++)for(size_t x=0;x<w;x++) {
   uint16_t v=src[y*stride+x];assert(wire[y*w+x]==(uint16_t)((v<<8)|(v>>8)));
 }
 assert(max_bytes<=(s_tx_buffer_leased?720:14400));
 assert(tx[7200]==0xcafe && tx[7201]==0xbeef);
 assert(line[360]==0xcafe && line[361]==0xbeef);
}
int main(void) {
 tx[7200]=line[360]=0xcafe;tx[7201]=line[361]=0xbeef;
 for(unsigned i=0;i<40000;i++)source[i]=(uint16_t)(i*43+19);
 assert(!display_acquire_draw_buffer(14401));assert(!display_acquire_draw_buffer(0));
 s_initialized=false;assert(!display_acquire_draw_buffer(14400));s_initialized=true;
 s_driver_draw_buffer=display_acquire_draw_buffer(14400);assert(s_driver_draw_buffer==(uint8_t *)tx);
 assert(!display_acquire_draw_buffer(14400));assert(!display_release_draw_buffer(source));
 for(unsigned i=0;i<7200;i++)tx[i]=original[i]=(uint16_t)(i*73+2);
 check(source+3,337,43,400);assert(!memcmp(tx,original,sizeof original));
 check(tx+3,137,20,360);assert(!memcmp(tx,original,sizeof original));
 release_draw_buffer(s_driver_draw_buffer);assert(!s_driver_draw_buffer && !s_tx_buffer_leased && !frees);
 assert(!display_release_draw_buffer(tx));check(source+3,337,43,400);
 assert(max_bytes==14400);release_draw_buffer(source);assert(frees==1);
 for(unsigned i=0;i<500;i++) {s_driver_draw_buffer=display_acquire_draw_buffer(14400);assert(s_driver_draw_buffer);release_draw_buffer(s_driver_draw_buffer);}
 puts("PASS: production DMA lease, duplicate/invalid acquisition, 500 release cycles, borrowed buffer never heap-freed, clipped/strided fallback preserves concurrent drawing surface and guards");
}
"""
c=out/'lease.c';c.write_text(prefix+functions+tests,encoding='utf-8')
exe=out/'lease.exe';subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra',str(c),'-o',str(exe)],check=True)
print(subprocess.check_output([str(exe)],text=True).strip())

# Run the production rebalance function against a controllable owner/transfer
# fence. Heap fallback and failed pipeline acquisition must preserve live DMA.
lifecycle=r"""
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#define CONFIG_NUWAX_DISPLAY_QSPI_ENABLE 1
#define ST77916_TX_BUFFER_PIXELS 7200U
#define DRAW_BUF_SIZE 14400U
#define DRAW_BUF_SPLIT_SIZE 7200U
#include "ui_memory_pressure.h"
static nuwax_ui_memory_pressure_t s_double_buffer_pressure;
static unsigned nuwax_replay_dma_loan_bytes(uint32_t now){(void)now;return 0;}
#define DRAW_BUF_DOUBLE_BUFFER_ENABLE 1
#define DOUBLE_BUFFER_HEALTH_INTERVAL_MS 500U
#define LVGL_INTERNAL_DRAW_BUFFER_MIN_FREE 24000U
#define DOUBLE_BUFFER_RESTORE_COOLDOWN_MS 1000U
#define DOUBLE_BUFFER_RESTORE_WARNING_COOLDOWN_MS 60000U
#define DOUBLE_BUFFER_RESTORE_STABLE_MS 1000U
#define DOUBLE_BUFFER_RESTORE_HEADROOM 8000U
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define LV_DISPLAY_RENDER_MODE_PARTIAL 0
#define ESP_ERR_NO_MEM -1
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
static void report_ui_warning_detail(const char*a,int b,size_t c,size_t d,size_t e,size_t f){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;}
static uint16_t tx[7200],*s_tx_buffer=tx;
static bool s_initialized=true,s_tx_buffer_leased;
static uint8_t owned[14400],extra[14400];
static uint8_t *s_draw_buf1,*s_draw_buf2,*s_driver_draw_buffer;
static bool s_draw_buf1_internal,s_draw_buf2_internal,s_runtime_internal_preferred=true,s_draw_buffers_split;
static int64_t s_double_buffer_last_health_us,s_double_buffer_restore_candidate_us,
 s_double_buffer_released_us,s_buffer_restore_last_report_us;
static bool s_internal_restore_incident_active;
static void *s_display;
static size_t free_bytes=20000,largest=20000;
static bool pipeline_busy,pipeline_held,fail_alloc;
static unsigned frees,changes;
static size_t heap_caps_get_free_size(int c) {(void)c;return free_bytes;}
static size_t heap_caps_get_largest_free_block(int c) {(void)c;return largest;}
static void *heap_caps_aligned_alloc(size_t a,size_t n,int c) {(void)a;(void)c;assert(n==14400);return fail_alloc?NULL:extra;}
static void heap_caps_free(void *p) {assert(p==owned || p==extra);frees++;free_bytes+=14400;}
static bool lvgl_flush_pipeline_take(unsigned ms) {(void)ms;if(pipeline_busy)return false;assert(!pipeline_held);pipeline_held=true;return true;}
static void lvgl_flush_pipeline_give(void) {assert(pipeline_held);pipeline_held=false;}
static void lv_display_set_buffers(void *d,void *a,void *b,unsigned n,int m) {
 (void)d;(void)m;assert(pipeline_held);assert((n==14400 || n==7200) && a && a!=b);
 if(n==7200)assert(a==(void*)tx && b==(uint8_t*)tx+7200);
 changes++;
}
"""
production='\n'.join(extract(src,x) for x in ['void *display_acquire_draw_buffer(', 'bool display_release_draw_buffer('])
production+='\n'+extract(port,'static void release_draw_buffer(')+'\n'+extract(port,'static void release_draw_buffer_pair(')+'\n'+extract(port,'static void lvgl_maybe_rebalance_internal_buffers(')
lifecycle_tests=r"""
int main(void) {
 s_draw_buf1=owned;s_draw_buf2=s_driver_draw_buffer=display_acquire_draw_buffer(14400);
 s_draw_buf1_internal=s_draw_buf2_internal=true;
 pipeline_busy=true;lvgl_maybe_rebalance_internal_buffers(1000000);
 assert(s_draw_buf1==owned && s_draw_buf2==(uint8_t *)tx && !frees && !changes);
 pipeline_busy=false;lvgl_maybe_rebalance_internal_buffers(2000000);
 assert(s_draw_buf1==(uint8_t *)tx && s_draw_buf2==(uint8_t*)tx+7200 && s_draw_buffers_split && s_tx_buffer_leased && frees==1);
 free_bytes=80000;largest=30000;fail_alloc=true;
 lvgl_maybe_rebalance_internal_buffers(4000000);lvgl_maybe_rebalance_internal_buffers(6000000);
 assert(s_draw_buffers_split && s_draw_buf1==(uint8_t *)tx);
 fail_alloc=false;lvgl_maybe_rebalance_internal_buffers(8000000);lvgl_maybe_rebalance_internal_buffers(10000000);
 assert(s_draw_buf1==(uint8_t *)tx && s_draw_buf2==extra && s_tx_buffer_leased);
 free_bytes=20000;lvgl_maybe_rebalance_internal_buffers(12000000);
 assert(s_draw_buf1==(uint8_t *)tx && s_draw_buf2==(uint8_t*)tx+7200 && s_draw_buffers_split && frees==2);
 pipeline_held=true;release_draw_buffer_pair(s_draw_buf1,s_draw_buf2,s_draw_buffers_split);pipeline_held=false;
 s_draw_buf2=NULL;s_draw_buffers_split=false;
 assert(!s_tx_buffer_leased && frees==2);
 s_draw_buf1=owned;s_driver_draw_buffer=NULL;free_bytes=30000;s_double_buffer_released_us=0;
 lvgl_maybe_rebalance_internal_buffers(14000000);
 assert(s_draw_buf2==(uint8_t *)tx && s_tx_buffer_leased && frees==2);
 assert(!pipeline_held);
 puts("PASS: production buffer rebalance respects busy DMA fence, frees owned buffer while retaining loan, handles failed restore and restores double buffering without freeing leased memory");
}
"""
lc=out/'lifecycle.c';lc.write_text(lifecycle+production+lifecycle_tests,encoding='utf-8')
le=out/'lifecycle.exe';subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-I'+str(root/'main'),str(lc),'-o',str(le)],check=True)
print(subprocess.check_output([str(le)],text=True).strip())
(out/'report.json').write_text(json.dumps(dict(displaySha256=hashlib.sha256((root/'main/display.c').read_bytes()).hexdigest(),portSha256=hashlib.sha256((root/'main/nuwax_lvgl_port.c').read_bytes()).hexdigest(),hardwareValidated=False),indent=2))
