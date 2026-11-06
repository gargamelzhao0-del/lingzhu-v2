"""Actual LVGL buffer geometry and display swap/direct-transmit on both halves.

The fake SPI transfer renders into the other half while consuming this half.
This checks bounds/ownership/pixel equivalence, not hardware FPS or DMA timing.
"""
from pathlib import Path
import subprocess
r=Path(__file__).resolve().parents[1]
out=r/'build-host-checks/display-compact-pair';out.mkdir(parents=True,exist_ok=True)
def extract(path,signature):
    s=(r/path).read_text(encoding='utf-8-sig');a=s.index(signature);b=s.index('{',a)+1;n=1
    while n:n+=(s[b]=='{')-(s[b]=='}');b+=1
    return s[a:b]
support=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#define LV_ASSERT_MSG(x,...) assert(x)
#define LV_ASSERT_FORMAT_MSG(x,...) assert(x)
#define LV_DISPLAY_RENDER_MODE_PARTIAL 0
#define LV_DISPLAY_RENDER_MODE_FULL 1
typedef int lv_color_format_t;
typedef int lv_display_render_mode_t;
typedef struct {unsigned w,h,stride,size;uint8_t *data;} lv_draw_buf_t;
typedef struct {lv_draw_buf_t _static_buf1,_static_buf2;int stride_is_auto;} lv_display_t;
static int lv_display_get_color_format(lv_display_t*d){(void)d;return 16;}
static unsigned lv_display_get_original_horizontal_resolution(lv_display_t*d){(void)d;return 360;}
static unsigned lv_display_get_original_vertical_resolution(lv_display_t*d){(void)d;return 360;}
static void *lv_draw_buf_align(void*p,int cf){assert(cf==16);return(void*)(((uintptr_t)p+15)&~(uintptr_t)15);}
static unsigned lv_draw_buf_width_to_stride(unsigned w,int cf){assert(cf==16);return w*2;}
static void lv_draw_buf_init(lv_draw_buf_t*b,unsigned w,unsigned h,int cf,unsigned stride,void*p,unsigned size){
 assert(cf==16 && stride*h<=size);*b=(lv_draw_buf_t){w,h,stride,size,p};
}
static void lv_display_set_draw_buffers(lv_display_t*d,lv_draw_buf_t*a,lv_draw_buf_t*b){assert(a==&d->_static_buf1 && b==&d->_static_buf2);}
static void lv_display_set_render_mode(lv_display_t*d,int m){(void)d;assert(m==0);}
#define CONFIG_NUWAX_UI_PERF_LOG_ENABLE 0
#define CONFIG_NUWAX_DISPLAY_PIN_CS 0
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
typedef int esp_err_t;
typedef struct {size_t length;const void*tx_buffer;int flags,cmd,addr;} spi_transaction_t;
typedef struct {spi_transaction_t base;int command_bits,address_bits,dummy_bits;} spi_transaction_ext_t;
static int s_spi;
static _Alignas(16) uint16_t memory[7232];
static uint16_t *base=memory+16,*other;
static uint16_t expected[3600];
static size_t count;
static unsigned transfers;
static int spi_device_transmit(int spi,spi_transaction_t*t){
 (void)spi;const uint16_t*p=t->tx_buffer;assert(t->length==count*16);
 assert(p==base || p==base+3600);
 assert(((uintptr_t)p&15)==0);
 // Simulate LVGL filling the other strip while this DMA job owns p.
 for(unsigned i=0;i<3600;i++)other[i]=(uint16_t)(0x1234+i);
 for(size_t i=0;i<count;i++)assert(p[i]==expected[i]);
 for(unsigned i=0;i<16;i++)assert(memory[i]==0xface && memory[7216+i]==0xbeef);
 transfers++;return ESP_OK;
}
static int spi_device_polling_transmit(int spi,spi_transaction_t*t){return spi_device_transmit(spi,t);}
static void gpio_set_level(int p,int v){(void)p;(void)v;}
static void taskYIELD(void){}
'''
production=extract('lib/lvgl/src/display/lv_display.c','void lv_display_set_buffers(')
production+=extract('main/display.c','static void swap_pixels_inplace(')
production+=extract('main/display.c','static esp_err_t st77916_transmit_pixels_direct(')
tests=r'''
int main(void){
 lv_display_t d={0};
 lv_display_set_buffers(&d,base,base+3600,7200,LV_DISPLAY_RENDER_MODE_PARTIAL);
 assert(d._static_buf1.w==360 && d._static_buf1.h==10 && d._static_buf2.h==10);
 assert(d._static_buf1.data+d._static_buf1.size==d._static_buf2.data);
 assert(d._static_buf2.data+d._static_buf2.size==(uint8_t*)(base+7200));
 for(unsigned i=0;i<16;i++){memory[i]=0xface;memory[7216+i]=0xbeef;}
 for(unsigned half=0;half<2;half++)for(count=1;count<=3600;count++){
  uint16_t*p=base+half*3600;other=base+(1-half)*3600;
  for(size_t i=0;i<count;i++){p[i]=(uint16_t)(i*73+count);expected[i]=(uint16_t)((p[i]<<8)|(p[i]>>8));}
  swap_pixels_inplace(p,count);
  assert(st77916_transmit_pixels_direct(p,count)==ESP_OK);
  for(unsigned i=0;i<3600;i++)assert(other[i]==(uint16_t)(0x1234+i));
 }
 assert(transfers==7200);
 puts("PASS: actual LVGL 360x10 geometry; 7200 direct transfers across both aligned halves; simultaneous other-strip writes, exact wire pixels, guard bytes and zero overlap");
}
'''
c=out/'pixels.c';c.write_text(support+production+tests,encoding='utf-8')
exe=out/'pixels.exe'
subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror',str(c),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
