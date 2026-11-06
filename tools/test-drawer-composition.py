"""Compile production mask/filter functions and verify RGB565 pixel coverage.

LVGL supplies a defined page/backdrop; the fixture supplies that input and a
frozen gradient. This tests the production flush stage, not physical QSPI.
"""
import hashlib
import json
import re
import subprocess
from pathlib import Path

root = Path(__file__).resolve().parents[1]
source = root / 'main/nuwax_ui_v28/nuwax_drawer.c'
text = source.read_text(encoding='utf-8')
out = root / 'build-host-checks/drawer-composition'
out.mkdir(parents=True, exist_ok=True)

def function(signature):
    start = text.index(signature)
    brace = text.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        if text[end] == '{': depth += 1
        elif text[end] == '}': depth -= 1
        end += 1
    return text[start:end]

# The menu must not claim the startup output gate or create another writer.
for forbidden in ['s_reveal_direct', 'reveal_direct_frame(',
                  'nuwax_lvgl_port_present_rgb565_regions(',
                  'nuwax_lvgl_port_set_panel_output_enabled(']:
    assert forbidden not in text, f'Menu bypasses the shared compositor: {forbidden}'
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define CC_W 360
#define CC_H 360
typedef struct { int16_t left_q4, right_q4; } cc_reveal_mask_row_t;
static cc_reveal_mask_row_t s_reveal_mask_rows[CC_H];
static bool s_backdrop_frozen_face=true;
static bool s_ota_blocked;
static bool s_navigation_cached=false;
static bool s_qr_reveal_active=false;
static void *s_sheet;
static int32_t lv_obj_get_x(void *obj) {(void)obj;return 0;}
static int32_t s_backdrop_progress;
static uint16_t frozen[CC_W*CC_H], actual[CC_W*CC_H];
static const uint16_t *nuwax_face_frozen_rgb565(size_t *stride) {*stride=CC_W;return frozen;}
/* Blank cached page: its base has already been drawn by LVGL. */
static void navigation_cache_compose(int32_t x,int32_t y,int32_t w,int32_t h,uint16_t *p,size_t stride) {(void)x;(void)y;(void)w;(void)h;(void)p;(void)stride;}
static void transition_cache_compose(int32_t x,int32_t y,int32_t w,int32_t h,
 uint16_t *pixels,size_t stride) {(void)x;(void)y;(void)w;(void)h;(void)pixels;(void)stride;}
'''
radius = re.search(r'^#define CC_REVEAL_RADIUS_Q4 (\d+)', text, re.M).group(1)
parts = [function(s) for s in [
    'static uint32_t integer_sqrt_u32(uint32_t value)',
    'static void update_reveal_mask_rows(int32_t progress) {',
    'static uint16_t blend_rgb565_q4(',
    'static void copy_rgb565_span(',
    'static void copy_frozen_span(',
    'static void control_center_flush_filter(']]
test = r'''
static uint16_t page(int x,int y) {return (uint16_t)(0x39e7U ^ (x*19U+y*73U));}
static uint16_t input(int x,int y,int progress) {
 if(progress==0 && x>=0 && y>=0 && x<CC_W && y<CC_H)return frozen[y*CC_W+x];
 return page(x,y);
}
static uint16_t expected(int x,int y,int progress) {
 uint16_t fg=input(x,y,progress);
 if(x<0 || y<0 || x>=CC_W || y>=CC_H || progress==0 || progress==1000)return fg;
 int start=s_reveal_mask_rows[y].left_q4, end=s_reveal_mask_rows[y].right_q4;
 int lo=start>x*16?start:x*16, hi=end<(x+1)*16?end:(x+1)*16;
 int coverage=hi>lo?hi-lo:0;if(coverage>16)coverage=16;
 uint16_t bg=frozen[y*CC_W+x];
 // Independent per-channel reference, including empty and subpixel spans.
 unsigned r=(((bg>>11)&31)*(16-coverage)+((fg>>11)&31)*coverage+8)/16;
 unsigned g=(((bg>>5)&63)*(16-coverage)+((fg>>5)&63)*coverage+8)/16;
 unsigned b=((bg&31)*(16-coverage)+(fg&31)*coverage+8)/16;
 return (uint16_t)((r<<11)|(g<<5)|b);
}
static void full(int progress) {
 s_backdrop_progress=progress;update_reveal_mask_rows(progress);
 for(int y=0;y<CC_H;y++)for(int x=0;x<CC_W;x++)actual[y*CC_W+x]=input(x,y,progress);
 control_center_flush_filter(0,0,CC_W,CC_H,actual,CC_W,NULL);
 for(int y=0;y<CC_H;y++)for(int x=0;x<CC_W;x++)assert(actual[y*CC_W+x]==expected(x,y,progress));
}
static void region(int x,int y,int w,int h,int progress) {
 uint16_t guarded[8192];size_t stride=(size_t)w+7;assert(stride*h+2<8192);
 for(unsigned i=0;i<8192;i++)guarded[i]=0xa55a;
 uint16_t *p=guarded+1;
 s_backdrop_progress=progress;update_reveal_mask_rows(progress);
 for(int yy=0;yy<h;yy++)for(int xx=0;xx<w;xx++)p[yy*stride+xx]=input(x+xx,y+yy,progress);
 control_center_flush_filter(x,y,w,h,p,stride,NULL);
 assert(guarded[0]==0xa55a);
 for(int yy=0;yy<h;yy++) {
  for(int xx=0;xx<w;xx++)assert(p[yy*stride+xx]==expected(x+xx,y+yy,progress));
  for(size_t xx=w;xx<stride;xx++)assert(p[yy*stride+xx]==0xa55a);
 }
 for(size_t i=stride*h+1;i<8192;i++)assert(guarded[i]==0xa55a);
}
int main(void) {
 for(int y=0;y<CC_H;y++)for(int x=0;x<CC_W;x++)frozen[y*CC_W+x]=(uint16_t)(x*173+y*397);
 // Both directions include all empty rows and the final close endpoint.
 for(int p=0;p<=1000;p++)full(p);
 for(int p=1000;p>=0;p-=7)full(p);
 for(int p=0;p<=1000;p+=13) {
  region(-3,-2,23,11,p);region(349,352,17,12,p);
  region(165,0,31,20,p);region(165,173,31,20,p);
  region(0,170,360,20,p);
 }
 full(0);assert(memcmp(actual,frozen,sizeof(actual))==0);
 // Missing frozen frame leaves LVGL's valid backdrop untouched.
 s_backdrop_frozen_face=false;s_backdrop_progress=500;
 memset(actual,0x5a,sizeof(actual));control_center_flush_filter(0,0,360,360,actual,360,NULL);
 for(size_t i=0;i<360U*360U;i++)assert(actual[i]==0x5a5a);
 s_backdrop_frozen_face=true;s_ota_blocked=true;
 for(int progress=0;progress<=1000;progress++) {
  s_backdrop_progress=progress;update_reveal_mask_rows(progress);
  for(size_t i=0;i<360U*360U;i++)actual[i]=(uint16_t)(i*31U);
  control_center_flush_filter(0,0,360,360,actual,360,NULL);
  for(size_t i=0;i<360U*360U;i++)assert(actual[i]==(uint16_t)(i*31U));
 }
 puts("PASS: production RGB565 mask/filter, 1001 progress values, OTA pixels preserved, reverse, close endpoint, subpixel edges, clipped/strided dirty rectangles and guards; no direct writer/output gate in menu");
}
'''
c = out / 'production-composition.c'
c.write_text(prefix + f'\n#define CC_REVEAL_RADIUS_Q4 {radius}\n' + '\n'.join(parts) + test,
             encoding='utf-8')
exe = out / 'production-composition.exe'
subprocess.run(['gcc', '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(exe)], check=True)
result = subprocess.check_output([str(exe)], text=True)
(out / 'result.txt').write_text(result, encoding='utf-8')
(out / 'source-identity.json').write_text(json.dumps({
    'source': str(source), 'sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
    'scope': 'Production pixel filter and mask; LVGL backdrop/frozen-frame fixtures; not a hardware test'
}, indent=2), encoding='utf-8')
print(result)
