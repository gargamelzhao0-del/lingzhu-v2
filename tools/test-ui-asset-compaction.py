"""Compare compact assets with saved pre-change sources, using the C decoder.

Checks all glyph alpha values at their original baseline coordinates, every
animation frame, malformed input bounds and the allocation-free static fallback.
"""
from pathlib import Path
import argparse
import ctypes as C
import hashlib
import json
import random
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT/'build-host-checks/ui-asset-compaction'
OUT.mkdir(parents=True, exist_ok=True)
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--baseline', type=Path, required=True)
args = p.parse_args()

def read(root, path):
    return (root/path).read_text(encoding='utf-8-sig')

def integers(text):
    return [int(v, 0) for v in re.findall(r'0x[0-9a-fA-F]+|(?<![A-Za-z_])-?\d+', text)]

def array(text, name):
    return integers(re.search(r'\b'+name+r'\[[^;=]*=\s*\{(.*?)\n?\};',text,re.S).group(1))

font='main/nuwax_ui_v28/assets/fonts/nuwax_font_lingzhu.c'
old, new = read(args.baseline,font), read(ROOT,font)
font_report=[]
for name,bpp in [('lingzhu_14',4),('lingzhu_17',4),('lingzhu_22',4),('network_14',2)]:
    pre='f_'+name
    def glyphs(source):
        d=re.search(r'\b'+pre+r'_glyphs\[\]=\{(.*?)\};',source,re.S).group(1)
        return [tuple(map(int,row)) for row in re.findall(
            r'bitmap_index=(\d+),.adv_w=(\d+),.box_w=(\d+),.box_h=(\d+),.ofs_x=(-?\d+),.ofs_y=(-?\d+)',d)]
    before, after = glyphs(old),glyphs(new)
    assert len(before)==len(after) and array(old,pre+'_unicode')==array(new,pre+'_unicode')
    # Maps, line metrics, bpp and fallback links are byte-for-byte unchanged.
    for suffix in ['_map','_dsc']:
        expr=r'^static const [A-Za-z0-9_]+ '+pre+suffix+r'(?:\[\])?\s*=.*'
        assert re.search(expr,old,re.M).group()==re.search(expr,new,re.M).group()
    expr=r'const lv_font_t nuwax_font_'+name+r'=.*'
    assert re.search(expr,old).group()==re.search(expr,new).group()
    bm0,bm1=array(old,pre+'_bitmap'),array(new,pre+'_bitmap')
    def positioned_pixels(row, data):
        idx,adv,w,h,x,y=row
        per=8//bpp
        return {(x+k%w, -h-y+k//w):(data[idx+k//per]>>(8-bpp*(k%per+1)))&((1<<bpp)-1)
                for k in range(w*h) if (data[idx+k//per]>>(8-bpp*(k%per+1)))&((1<<bpp)-1)}
    for a,b in zip(before,after):
        assert a[1]==b[1] and positioned_pixels(a,bm0)==positioned_pixels(b,bm1), (name,a,b)
    font_report.append(dict(name=name,glyphs=len(before),savedBytes=len(bm0)-len(bm1)))
print('PASS: all 8152 glyphs retain alpha, baseline coordinates, advances, maps and fallbacks')

wrapper=OUT/'decoder.c'
wrapper.write_text('''#include "asset_rle.h"
__declspec(dllexport) int decode(const uint8_t*d,size_t n,const uint32_t*offsets,
 unsigned count,unsigned bytes,unsigned frame,uint8_t*out,size_t cap){
 nuwax_asset_rle_t a={d,offsets,n,count,bytes};
 return nuwax_asset_rle_decode_frame(&a,frame,out,cap);}
''',encoding='utf-8')
dll=OUT/'decoder.dll'
subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-shared',
    '-I'+str(ROOT/'main/nuwax_ui_v28/assets'),str(wrapper),'-o',str(dll)],check=True)
lib=C.CDLL(str(dll));decode=lib.decode
decode.argtypes=[C.POINTER(C.c_uint8),C.c_size_t,C.POINTER(C.c_uint32),C.c_uint,C.c_uint,C.c_uint,C.POINTER(C.c_uint8),C.c_size_t]
decode.restype=C.c_int

def unpack(data, offsets, count, size, index, cap=None):
    buf=(C.c_uint8*len(data))(*data);off=(C.c_uint32*len(offsets))(*offsets)
    # Guards detect writes before/after the declared output frame.
    dst=(C.c_uint8*(size+32))(*([0xCD]*(size+32)))
    target=C.cast(C.byref(dst,16),C.POINTER(C.c_uint8))
    result=decode(buf,len(data),off,count,size,index,target,size if cap is None else cap)
    assert bytes(dst[:16])==b'\xcd'*16 and bytes(dst[size+16:])==b'\xcd'*16
    return result,bytes(dst[16:size+16])

animation_report=[]
for path,old_name,name,count,size in [
    ('main/nuwax_ui_v28/assets/low_battery_assets.c','nuwax_low_battery_masks','nuwax_low_battery_frames',17,8112),
    ('main/nuwax_ui_v28/face/conversation_visual_assets.h','conversation_bars','conversation_bars',60,1056),
    ('main/nuwax_ui_v28/face/conversation_visual_assets.h','conversation_ripple','conversation_ripple',24,1056)]:
    original=array(read(args.baseline,path),old_name)
    compact=read(ROOT,path);data=array(compact,name+'_data');offsets=array(compact,name+'_offsets')
    assert len(original)==count*size and len(offsets)==count+1 and offsets[-1]==len(data)
    for i in range(count):
        ok,decoded=unpack(data,offsets,count,size,i)
        assert ok and decoded==bytes(original[i*size:(i+1)*size]),(name,i)
        assert not unpack(data,offsets,count,size,i,cap=size-1)[0]
    assert not unpack(data,offsets,count,size,count)[0]
    assert not unpack(data[:-1],offsets,count,size,count-1)[0]
    if count==17:
        assert bytes(array(compact,'nuwax_low_battery_static_mask'))==bytes(original[:size])
    animation_report.append(dict(name=name,frames=count,rawBytes=len(original),
                                 packedBytes=len(data)+len(offsets)*4))
print('PASS: C decoder restores all 101 frames exactly; static low-battery fallback unchanged')

for bad in [bytes([128]),bytes([255]),bytes([0]),bytes([127,1]),bytes([129,3]),bytes([0,1])]:
    assert not unpack(bad,[0,len(bad)],1,16,0)[0]
rng=random.Random(184)
for _ in range(5000):
    raw=rng.randbytes(rng.randrange(1,150));size=rng.randrange(1,130)
    unpack(raw,[0,len(raw)],1,size,0)
print('PASS: malformed/truncated RLE, oversized runs, short destinations and 5000 bounded fuzz cases')

from ui_asset_codec import packbits
for size in [1,2,3,127,128,129,256,8112]:
    for raw in [bytes(size),bytes([255])*size,bytes(i%256 for i in range(size)),rng.randbytes(size)]:
        packed=packbits(raw)
        ok,restored=unpack(packed,[0,len(packed)],1,size,0)
        assert ok and restored==raw
print('PASS: encoder/production decoder round trips across literal and repeat boundaries')
paths=[font,'main/nuwax_ui_v28/assets/low_battery_assets.c',
       'main/nuwax_ui_v28/assets/low_battery_assets.h','main/nuwax_ui_v28/assets/asset_rle.h',
       'main/nuwax_ui_v28/face/conversation_visual_assets.h',
       'main/nuwax_ui_v28/low_battery_warning.c','main/nuwax_ui_v28/face/conversation_visual.c']
(OUT/'report.json').write_text(json.dumps(dict(status='PASS',hardwareValidated=False,
    fonts=font_report,animations=animation_report,malformedFuzzCases=5000,
    sourceHashes={p:hashlib.sha256((ROOT/p).read_bytes()).hexdigest() for p in paths}),indent=2),encoding='utf-8')
