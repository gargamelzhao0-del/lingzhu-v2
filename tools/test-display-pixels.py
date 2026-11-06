"""Test the production RGB565 copy, including IDF no-builtin-memcpy codegen."""
import argparse, hashlib, json, re, subprocess
from pathlib import Path
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--elf',type=Path)
args=parser.parse_args()
root=Path(__file__).resolve().parents[1]
source=root/'main/display.c';text=source.read_text(encoding='utf-8')
start=text.index('static void copy_pixels_byte_swapped(');brace=text.index('{',start)
end=brace+1;depth=1
while depth:
    depth += (text[end]=='{')-(text[end]=='}');end+=1
function=text[start:end]
out=root/'build-host-checks/display-pixels';out.mkdir(parents=True,exist_ok=True)
prefix='#include <stdint.h>\n#include <stddef.h>\n#include <string.h>\n'
harness=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static _Alignas(16) uint16_t src[129640],dst[129640];
static void check(size_t n,size_t so,size_t d) {
    for(size_t i=0;i<129640;i++)src[i]=(uint16_t)(i*73U+(i>>3));
    memset(dst,0xa5,sizeof(dst));
    copy_pixels_byte_swapped(dst+d,src+so,n);
    for(size_t i=0;i<129640;i++) {
        uint16_t expected=0xa5a5;
        if(i>=d && i-d<n) {
            uint16_t v=src[so+i-d];expected=(uint16_t)((v<<8)|(v>>8));
        }
        assert(dst[i]==expected);
        assert(src[i]==(uint16_t)(i*73U+(i>>3)));
    }
}
int main(void) {
    for(size_t so=0;so<8;so++)for(size_t d=0;d<8;d++) {
        for(size_t n=0;n<34;n++)check(n,so,d);
        const size_t sizes[]={359,360,361,7199,7200,7201,15359,15360,129599,129600};
        for(size_t j=0;j<sizeof(sizes)/sizeof(sizes[0]);j++)check(sizes[j],so,d);
        // Exhaustive RGB565 colours, including source/destination alignment variants.
        for(size_t i=0;i<65536;i++)src[so+i]=(uint16_t)i;
        copy_pixels_byte_swapped(dst+d,src+so,65536);
        for(size_t i=0;i<65536;i++)assert(dst[d+i]==(uint16_t)((i<<8)|(i>>8)));
    }
    // Odd-width strided rows exercise changing halfword alignment independently.
    for(size_t y=0;y<31;y++) {
        for(size_t x=0;x<367;x++)src[y*367+x]=(uint16_t)(y*367+x);
        copy_pixels_byte_swapped(dst+y*359,src+y*367+3,359);
        for(size_t x=0;x<359;x++) {
            uint16_t v=src[y*367+x+3];assert(dst[y*359+x]==(uint16_t)((v<<8)|(v>>8)));
        }
    }
    puts("PASS: production RGB565 copy; all 65536 colours, all halfword alignments within 16 bytes, zero/odd/even/chunk/full-frame lengths, guards, immutable source and odd strided rows");
}
'''
c=out/'pixels.c';c.write_text(prefix+function+harness,encoding='utf-8')
exe=out/'pixels.exe'
subprocess.run(['gcc','-O2','-fstrict-aliasing','-fno-builtin-memcpy','-std=c11','-Wall','-Wextra','-Werror',str(c),'-o',str(exe)],check=True)
print(subprocess.check_output([str(exe)],text=True).strip())
toolchain=Path('C:/Espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin')
report={'sourceSha256':hashlib.sha256(source.read_bytes()).hexdigest(),'hardwareValidated':False}
for label,fn in [('ordinary',function.replace('__builtin_memcpy(', 'memcpy(')),('builtin',function)]:
    c=out/f'{label}.c';obj=out/f'{label}.o'
    c.write_text(prefix+fn+'\nvoid probe(uint16_t *d,const uint16_t *s,size_t n){copy_pixels_byte_swapped(d,s,n);}\n',encoding='utf-8')
    subprocess.run([str(toolchain/'xtensa-esp32s3-elf-gcc.exe'),'-O2','-mlongcalls','-fstrict-aliasing','-fno-builtin-memcpy','-Wall','-Wextra','-Werror','-c',str(c),'-o',str(obj)],check=True)
    undefined=subprocess.check_output([str(toolchain/'xtensa-esp32s3-elf-nm.exe'),'-u',str(obj)],text=True)
    asm=subprocess.check_output([str(toolchain/'xtensa-esp32s3-elf-objdump.exe'),'-dr',str(obj)],text=True)
    (out/f'{label}.asm').write_text(asm,encoding='utf-8')
    calls_memcpy=bool(re.search(r'\bmemcpy\b',undefined))
    assert calls_memcpy == (label=='ordinary'),(label,undefined)
    if label=='builtin':
        assert not undefined.strip(),undefined
        assert 'l32i' in asm and 'l16ui' in asm, 'Missing aligned/halfword load paths'
        assert not re.search(r'\b(?:l8ui|s8i)\b',asm), 'Byte-copy/stack staging returned'
    report[label]={'undefinedSymbols':undefined.strip()}
if args.elf:
    symbols=subprocess.check_output([str(toolchain/'xtensa-esp32s3-elf-nm.exe'),'-S',str(args.elf)],text=True)
    names=re.findall(r'^\S+\s+\S+\s+\w\s+(spi_write_pixels_strided\S*)$',symbols,re.M)
    assert names,'Missing linked production copy path'
    for name in names:
        asm=subprocess.check_output([str(toolchain/'xtensa-esp32s3-elf-objdump.exe'),'-d',f'--disassemble={name}',str(args.elf)],text=True)
        assert '<memcpy>' not in asm,'Linked production copy still calls ROM memcpy'
        assert 'l16ui' in asm,'Missing halfword-safe source load'
        (out/f'linked-{name}.asm').write_text(asm,encoding='utf-8')
    report['linked']={'elfSha256':hashlib.sha256(args.elf.read_bytes()).hexdigest(),'symbols':names,'romMemcpyReferences':0}
(out/'report.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
print('PASS: Xtensa negative control calls memcpy; optimized conversion has no undefined helper calls'+('; linked copy path has no ROM memcpy reference' if args.elf else ''))
