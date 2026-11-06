"""Host-check real boot decoder/streamer and compare every RGB565 output pixel."""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--elf', type=Path, help='Verify logo bytes in the linked firmware')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
out = root / 'build-host-checks/boot-logo'
out.mkdir(parents=True, exist_ok=True)
display = (root / 'main/display.c').read_text(encoding='utf-8')
start = display.index('static esp_err_t st77916_draw_boot_logo(void)')
end = display.index('\n}\n', start) + 3
streamer = display[start:end]
fixture = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "boot_logo.h"
#define ST77916_WIDTH 360
#define ST77916_HEIGHT 360
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_SIZE -2
#define TAG "test"
#define ESP_LOGE(...) ((void)0)
#define ESP_RETURN_ON_ERROR(expr, ...) do { int e=(expr); if(e)return e; }while(0)
typedef int esp_err_t;
static uint16_t s_line_buffer[360], panel[360*360];
static size_t calls, position;
static int fail_row=-1;
static bool fail_window;
static esp_err_t st77916_set_addr_window(int x,int y,int w,int h) {
    assert(x==0 && y==0 && w==360 && h==360);
    calls=0;position=0;
    return fail_window?ESP_FAIL:ESP_OK;
}
static esp_err_t spi_write_pixels(const uint16_t *p,size_t n) {
    assert(n==360 && position+n<=360U*360U);
    if((int)calls++==fail_row)return ESP_FAIL;
    memcpy(panel+position,p,n*2);position+=n;return ESP_OK;
}
'''
checks = r'''
int main(int argc,char **argv) {
    assert(argc==2);
    uint16_t guarded[362];
    for(size_t i=0;i<362;i++)guarded[i]=0x5a5a;
    assert(!nuwax_boot_logo_decode_row(360,guarded+1,360));
    assert(!nuwax_boot_logo_decode_row(0,guarded+1,359));
    assert(!nuwax_boot_logo_decode_row(0,NULL,360));
    const uint8_t valid[]={2,0x34,0x12,1,0xcd,0xab};
    assert(nuwax_boot_logo_decode(valid,sizeof(valid),guarded+1,3));
    assert(guarded[0]==0x5a5a && guarded[1]==0x1234 && guarded[2]==0x1234 &&
           guarded[3]==0xabcd && guarded[4]==0x5a5a);
    assert(!nuwax_boot_logo_decode(valid,5,guarded+1,3)); // truncated packet
    assert(!nuwax_boot_logo_decode(valid,6,guarded+1,2)); // output overflow
    assert(!nuwax_boot_logo_decode(valid,6,guarded+1,4)); // short row
    const uint8_t zero[]={0,0,0};
    assert(!nuwax_boot_logo_decode(zero,3,guarded+1,360));
    for(size_t y=0;y<360;y++) {
        assert(nuwax_boot_logo_decode_row(y,guarded+1,360));
        assert(guarded[0]==0x5a5a && guarded[361]==0x5a5a);
    }
    assert(st77916_draw_boot_logo()==ESP_OK);
    assert(calls==360 && position==360U*360U);
    FILE *f=fopen(argv[1],"wb");assert(f);
    assert(fwrite(panel,sizeof(panel),1,f)==1);assert(fclose(f)==0);
    fail_window=true;
    assert(st77916_draw_boot_logo()==ESP_FAIL && calls==0);
    fail_window=false;
    for(int y=0;y<360;y++) {
        fail_row=y;
        assert(st77916_draw_boot_logo()==ESP_FAIL);
        assert(calls==(size_t)y+1 && position==(size_t)y*360);
    }
    puts("PASS: production boot decoder and streamer; bounded rows, malformed packets, all 360 injected transfer failures and address-window failure");
}
'''
c = out / 'boot-logo-host.c'
c.write_text(fixture + streamer + checks, encoding='utf-8')
exe = out / 'boot-logo-host.exe'
subprocess.run(['gcc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                '-I', str(root/'main'), str(c), str(root/'main/boot_logo.c'),
                str(root/'main/assets/ling_zhu_boot_logo.c'), '-o', str(exe)], check=True)
raw = out / 'streamed.rgb565'
print(subprocess.check_output([str(exe), str(raw)], text=True).strip())
expected = root / 'assets/boot/ling-zhu-360.rgb565'
assert raw.read_bytes() == expected.read_bytes(), 'Output differs from approved converted image'
manifest = json.loads((root/'assets/boot/manifest.json').read_text())
assert hashlib.sha256(raw.read_bytes()).hexdigest() == manifest['rgb565Sha256']
# Check boot handoff: the only logo writer runs while backlight is off and
# before readiness; opening LVGL output invalidates the complete screen.
init = display[display.index('esp_err_t nuwax_display_init(void)'):]
assert init.index('st77916_backlight_configure_off()') < init.index('st77916_draw_boot_logo()') < init.index('s_initialized = true;')
port = (root/'main/nuwax_lvgl_port.c').read_text(encoding='utf-8')
gate = port[port.index('void nuwax_lvgl_port_set_panel_output_enabled'):]
assert 'lv_obj_invalidate(screen);' in gate[:1200]
main = (root/'main/nuwax_ai_partner_main.c').read_text(encoding='utf-8')
boot = main[main.index('esp_err_t display_err = nuwax_display_init();'):]
assert boot.index('boot_backlight_err') < boot.index('nuwax_lvgl_port_init()')
report = {'rgb565Sha256': manifest['rgb565Sha256'], 'pixelsVerified': 129600,
          'transferFailurePositionsVerified': 360, 'hardwareValidated': False}
for file, key in [('assets/boot/ling-zhu-source.png', 'sourceSha256'),
                  ('main/assets/ling_zhu_boot_logo.c', 'sourceCsha256')]:
    assert hashlib.sha256((root/file).read_bytes()).hexdigest() == manifest[key]
if args.elf:
    from elftools.elf.elffile import ELFFile
    with args.elf.open('rb') as f:
        elf = ELFFile(f)
        symbols = elf.get_section_by_name('.symtab')
        def symbol_bytes(name):
            matches = symbols.get_symbol_by_name(name)
            assert matches and len(matches) == 1, name
            sym = matches[0]
            section = elf.get_section(sym['st_shndx'])
            assert 'rodata' in section.name, (name, section.name)
            offset = sym['st_value'] - section['sh_addr']
            return section.data()[offset:offset+sym['st_size']]
        data = symbol_bytes('ling_zhu_boot_logo_data')
        row_bytes = symbol_bytes('ling_zhu_boot_logo_rows')
        rows = [int.from_bytes(row_bytes[i:i+4], 'little')
                for i in range(0, len(row_bytes), 4)]
        assert len(data) == manifest['rleBytes'] and len(rows) == 361
        decoded = bytearray()
        for a, b in zip(rows, rows[1:]):
            assert 0 <= a < b <= len(data) and (b-a) % 3 == 0
            row = b''.join(data[i+1:i+3] * data[i] for i in range(a, b, 3))
            assert len(row) == 720
            decoded.extend(row)
        assert decoded == expected.read_bytes(), 'Linked logo differs from approved image'
    report['linkedElfSha256'] = hashlib.sha256(args.elf.read_bytes()).hexdigest()
    print('PASS: final ELF Flash resource decodes to the exact approved 360x360 image')
(out/'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
print('PASS: all 129600 streamed pixels match approved RGB565; boot/backlight and full-repaint handoff ordering')
