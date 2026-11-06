#!/usr/bin/env python3
"""Generate small native LVGL monochrome icons and subset fonts (Pillow)."""
from pathlib import Path
import re
from PIL import Image, ImageDraw, ImageFont
from ui_asset_codec import crop_glyph

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'main/nuwax_ui_v28/assets'
S = 4

def array(name, data):
    return 'static const uint8_t '+name+'[] = {\n'+ '\n'.join('    '+','.join(f'0x{x:02x}' for x in data[i:i+24])+',' for i in range(0,len(data),24))+'\n};\n'

def icon(kind, size=48):
    im=Image.new('L',(48*S,48*S)); d=ImageDraw.Draw(im)
    def box(b): return tuple(round(v*S) for v in b)
    def ellipse(b, fill=255): d.ellipse(box(b),fill=fill)
    def line(pts,w=3,fill=255):
        p=[(round(x*S),round(y*S)) for x,y in pts];d.line(p,fill=fill,width=round(w*S),joint='curve')
        for x,y in (pts[0],pts[-1]):ellipse((x-w/2,y-w/2,x+w/2,y+w/2),fill)
    def arc(b,a,z,w=3): d.arc(box(b),a,z,fill=255,width=round(w*S))
    def round_arc(cx,cy,r,a,z,w):
        import math
        line([(cx+r*math.cos(math.radians(a+(z-a)*i/40)),cy+r*math.sin(math.radians(a+(z-a)*i/40))) for i in range(41)],w)
    def rr(b,r=4,fill=None,w=3):d.rounded_rectangle(box(b),radius=r*S,fill=fill,outline=255 if fill is None else None,width=w*S)
    if kind.startswith('wifi') or kind=='provisioning':
        strength=int(kind[-1]) if kind.startswith('wifi') else 3
        for n,r in enumerate((29,20,11)):
            if strength>=3-n:round_arc(24,40,r,228,312,4.5)
        ellipse((20,36,28,44))
    elif kind.startswith('sun_'):
        import math
        level={'sun_low':0,'sun_mid':1,'sun_high':2}[kind]
        r=[6,7,8][level]
        ellipse((24-r,21-r,24+r,21+r))
        if level < 2:ellipse((26-r,23-r,22+r,19+r),0)
        for i in range(8):
            a=i*math.pi/4
            inner=[12,13,13][level];outer=[12.5,16,18][level]
            line([(24+inner*math.cos(a),21+inner*math.sin(a)),
                  (24+outer*math.cos(a),21+outer*math.sin(a))],2.5 if level==0 else 3)
    elif kind=='cloud':
        # Erode a union silhouette to obtain one smooth hollow outline.
        from PIL import ImageFilter, ImageChops
        ellipse((3,20,23,41));ellipse((11,9,34,33));ellipse((27,18,45,41));rr((12,27,37,41),4,255)
        im=ImageChops.subtract(im,im.filter(ImageFilter.MinFilter(2*round(2.5*S)+1)))
        d=ImageDraw.Draw(im)
        ellipse((17,25,21,30));ellipse((28,25,32,30))
    elif kind=='battery':
        rr((4,12,40,36),4);rr((41,20,44,28),1,255)
    elif kind in ('conversation','meeting','meeting_state'):
        rr((19,5,29,29),5,255);arc((13,12,35,36),0,180,3);line([(24,35),(24,41)]);line([(18,42),(30,42)])
        if kind=='conversation':
            round_arc(15,25,8,145,215,2.7);round_arc(15,25,13,150,210,2.7)
            round_arc(33,25,8,-35,35,2.7);round_arc(33,25,13,-30,30,2.7)
    elif kind in ('device_info','cat'):
        d.polygon([tuple(round(v*S) for v in p) for p in [(6,18),(8,5),(19,11),(29,11),(40,5),(42,18)]],fill=255)
        ellipse((5,10,43,43));ellipse((16,24,20,29),0);ellipse((28,24,32,29),0)
        d.arc(box((20,28,28,36)),0,180,fill=0,width=2*S)
    elif kind=='sleeping':
        rr((5,9,43,39),13);line([(12,25),(18,25)]);line([(30,25),(36,25)])
    elif kind=='monitoring':
        rr((5,7,43,35),5);line([(24,35),(24,42)]);line([(14,42),(34,42)]);ellipse((17,14,31,28))
    elif kind=='qr':
        for x,y in [(5,5),(27,5),(5,27)]:
            d.rectangle(box((x,y,x+15,y+15)),fill=255);d.rectangle(box((x+3,y+3,x+12,y+12)),fill=0);d.rectangle(box((x+6,y+6,x+9,y+9)),fill=255)
        for x,y in [(27,27),(36,27),(30,33),(39,36),(27,39),(34,40)]:d.rectangle(box((x,y,x+4,y+4)),fill=255)
    elif kind=='volume':
        d.polygon([tuple(round(v*S) for v in p) for p in [(6,19),(15,19),(25,10),(25,38),(15,29),(6,29)]],fill=255)
        arc((21,13,35,35),300,420,3);arc((23,7,43,41),300,420,3)
    elif kind=='check':
        ellipse((4,4,44,44));line([(14,24),(21,31),(34,17)],4,0)
    elif kind=='up':line([(12,29),(24,17),(36,29)],4)
    elif kind=='warning':
        ellipse((4,4,44,44));line([(24,13),(24,26)],4,0);ellipse((22,32,26,36),0)
    return im.resize((size,size),Image.Resampling.LANCZOS)

def charging_mark():
    # Approved 60px battery-space design: white bolt with a black separator.
    # Crop transparent margins only; the drawer places this at (16, 15).
    scale=8
    im=Image.new('RGBA',(60*scale,60*scale),(0,0,0,0))
    d=ImageDraw.Draw(im)
    points=[(x*scale,y*scale) for x,y in
            [(30,20),(21,31),(27,31),(25,40),(35,27),(29,27)]]
    d.line(points+[points[0]],fill=(0,0,0,255),width=3*scale,joint='curve')
    d.polygon(points,fill=(255,255,255,255))
    return im.resize((60,60),Image.Resampling.LANCZOS).crop((16,15,41,45))

def generate_icons():
    names=['wifi_0','wifi_1','wifi_2','wifi_3','cloud','battery','conversation','sleeping','meeting_state','monitoring','meeting','provisioning','qr','device_info','volume','cat','check','up','warning','sun_low','sun_mid','sun_high']
    c=['/* Generated by tools/generate-lingzhu-ui-assets.py. */\n#include "nuwax_control_center_icons.h"\n']
    for n in names:
        size=44 if n.startswith('sun_') else 36 if n in ('cat','up') else 28 if n in ('check','warning') else 30 if n=='volume' else 48 if n in ('meeting','provisioning','qr','device_info') else 60
        im=icon(n,size);data=im.tobytes();c.append(array('s_'+n+'_data',data))
        c.append(f'static const lv_image_dsc_t s_{n} = {{.header.magic=LV_IMAGE_HEADER_MAGIC, .header.cf=LV_COLOR_FORMAT_A8, .header.w={size}, .header.h={size}, .header.stride={size}, .data_size=sizeof(s_{n}_data), .data=s_{n}_data}};\n')
    mark=charging_mark()
    c.append(array('s_battery_charging_mark_data',mark.tobytes('raw','BGRA')))
    c.append('static const lv_image_dsc_t s_battery_charging_mark = {.header.magic=LV_IMAGE_HEADER_MAGIC, .header.cf=LV_COLOR_FORMAT_ARGB8888, .header.w=25, .header.h=30, .header.stride=100, .data_size=sizeof(s_battery_charging_mark_data), .data=s_battery_charging_mark_data};\n')
    c.append('const lv_image_dsc_t *nuwax_control_center_icon_get(nuwax_control_center_icon_id_t id) {\nswitch(id) {\n')
    c.append('case NUWAX_CC_ICON_BATTERY_CHARGING_MARK: return &s_battery_charging_mark;\n')
    for n in names:c.append(f'case NUWAX_CC_ICON_{n.upper()}: return &s_{n};\n')
    c.append('default: return NULL;\n}\n}\n');(OUT/'nuwax_control_center_icons.c').write_text(''.join(c),encoding='utf-8')
    h=OUT/'nuwax_control_center_icons.h';s=h.read_text(encoding='utf-8')
    if 'NUWAX_CC_ICON_CAT' not in s:s=s.replace('    NUWAX_CC_ICON_COUNT','    NUWAX_CC_ICON_CAT,\n    NUWAX_CC_ICON_CHECK,\n    NUWAX_CC_ICON_UP,\n    NUWAX_CC_ICON_WARNING,\n    NUWAX_CC_ICON_COUNT')
    h.write_text(s,encoding='utf-8')

def generate_fonts():
    source='\n'.join((ROOT/p).read_text(encoding='utf-8') for p in (
        'main/nuwax_ui_v28/nuwax_drawer.c',
        'main/nuwax_ui_v28/menu/nuwax_device_qr_view.c',
        'main/nuwax_ui_v28/menu/nuwax_detail_style.h'))
    texts=''.join(re.findall(r'"([^"\n]*)"',source))
    chars=sorted(set(''.join(chr(c) for c in range(32,127))+'灵珠状态快捷应用菜单测试音量'+''.join(c for c in texts if '\u4e00'<=c<='\u9fff')),key=ord)
    path=ROOT/'lib/lvgl/scripts/built_in_font/SourceHanSansSC-Normal.otf'
    out=['/* Generated UI subsets and 2-bit network-name fallback. */\n#include "lvgl.h"\nLV_FONT_DECLARE(nuwax_font_network_14);\n']
    for name,size,network in [('lingzhu_14',14,False),('lingzhu_17',17,False),('lingzhu_22',22,False),('network_14',14,True)]:
        # Network names are runtime data, not a fixed UI vocabulary. Keep the
        # common Simplified Chinese repertoire at 2bpp to bound Flash cost.
        alphabet=set(chars)
        if network:
            for high in range(0xA1,0xF8):
                for low in range(0xA1,0xFF):
                    try: alphabet.add(bytes((high,low)).decode('gb2312'))
                    except UnicodeDecodeError: pass
        glyph_chars=sorted(alphabet,key=ord);bpp=2 if network else 4
        font=ImageFont.truetype(str(path),size*S);bitmap=bytearray();descs=['{0}'];h=size+4
        for char in glyph_chars:
            w=max(1,round(font.getlength(char)/S));im=Image.new('L',(w*S,h*S));d=ImageDraw.Draw(im)
            d.text((0,(size+1)*S),char,font=font,fill=255,anchor='ls',stroke_width=2)
            im=im.resize((w,h),Image.Resampling.LANCZOS);pixels=[round(v*((1<<bpp)-1)/255) for v in im.tobytes()]
            pixels,box_w,box_h,ofs_x,ofs_y=crop_glyph(pixels,w,h)
            per_byte=8//bpp
            while len(pixels)%per_byte:pixels.append(0)
            descs.append(f'{{.bitmap_index={len(bitmap)},.adv_w={w*16},.box_w={box_w},.box_h={box_h},.ofs_x={ofs_x},.ofs_y={ofs_y}}}')
            bitmap.extend(sum(pixels[i+j]<<(8-bpp*(j+1)) for j in range(per_byte)) for i in range(0,len(pixels),per_byte))
        p=f'f_{name}';out.append(array(p+'_bitmap',bitmap));out.append('static const lv_font_fmt_txt_glyph_dsc_t '+p+'_glyphs[]={'+','.join(descs)+'};\n')
        base=ord(glyph_chars[0]);out.append('static const uint16_t '+p+'_unicode[]={'+','.join(str(ord(c)-base) for c in glyph_chars)+'};\n')
        out.append(f'static const lv_font_fmt_txt_cmap_t {p}_map[] = {{{{.range_start={base},.range_length={ord(glyph_chars[-1])-base+1},.glyph_id_start=1,.unicode_list={p}_unicode,.list_length={len(glyph_chars)},.type=LV_FONT_FMT_TXT_CMAP_SPARSE_TINY}}}};\n')
        out.append(f'static const lv_font_fmt_txt_dsc_t {p}_dsc={{.glyph_bitmap={p}_bitmap,.glyph_dsc={p}_glyphs,.cmaps={p}_map,.cmap_num=1,.bpp={bpp}}};\n')
        fallback=',.fallback=&nuwax_font_network_14' if name=='lingzhu_14' else ''
        out.append(f'const lv_font_t nuwax_font_{name}={{.get_glyph_dsc=lv_font_get_glyph_dsc_fmt_txt,.get_glyph_bitmap=lv_font_get_bitmap_fmt_txt,.line_height={h},.base_line=0,.dsc=&{p}_dsc{fallback}}};\n')
    (OUT/'fonts/nuwax_font_lingzhu.c').write_text(''.join(out),encoding='utf-8')

if __name__=='__main__':generate_icons();generate_fonts()
