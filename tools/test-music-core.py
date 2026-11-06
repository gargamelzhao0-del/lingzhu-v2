"""Compile real production decoder/output functions with boundary stubs."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build-host-checks' / 'music'
OUT.mkdir(parents=True, exist_ok=True)
source = (ROOT / 'main/music_playback/music_playback.c').read_text(encoding='utf-8')
begin = source.index('static bool input_done(')
end = source.index('static void music_task(', begin)
(OUT / 'music_core_under_test.inc').write_text(source[begin:end], encoding='utf-8')
helix = ROOT / 'managed_components/chmorgan__esp-libhelix-mp3/libhelix-mp3'
sources = list(helix.glob('*.c')) + list((helix / 'real').glob('*.c'))
exe = OUT / 'music_core_check.exe'
# The vendor decoder only supplies target-specific assembly. Replace those
# arithmetic primitives for the host, leaving the actual decoder unchanged.
arithmetic = OUT / 'helix_host_arithmetic.h'
arithmetic.write_text('''#ifndef _ASSEMBLY_H
#define _ASSEMBLY_H
#include <stdint.h>
typedef int64_t Word64;
static inline int MULSHIFT32(int x, int y) { return (int)(((int64_t)x * y) >> 32); }
static inline int FASTABS(int x) { return x < 0 ? (int)(0U - (uint32_t)x) : x; }
static inline int CLZ(int x) { return x ? __builtin_clz((unsigned)x) : 32; }
static inline Word64 MADD64(Word64 s, int x, int y) { return s + (int64_t)x * y; }
static inline Word64 SHL64(Word64 x, int n) { return (Word64)((uint64_t)x << n); }
static inline Word64 SAR64(Word64 x, int n) { return x >> n; }
#endif
''', encoding='utf-8')
subprocess.run(['gcc', '-std=c11', '-O1', '-fwrapv', '-include', str(arithmetic),
                '-I' + str(helix / 'pub'), '-I' + str(helix / 'real'), '-I' + str(helix),
                '-I' + str(OUT), str(ROOT / 'main/music_playback/music_core_host_check.c'),
                *map(str, sources), '-lm', '-o', str(exe)], check=True)
for rate in (44100, 48000):
    mp3 = OUT / f'tone-{rate}.mp3'
    subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y', '-f', 'lavfi',
                    '-i', f'sine=frequency=997:sample_rate={rate}:duration=1',
                    '-ac', '2', '-codec:a', 'libmp3lame', '-b:a', '192k', str(mp3)], check=True)
    subprocess.run([str(exe), str(mp3), str(rate)], check=True, timeout=15)
owner_exe = OUT / 'music_owner_check.exe'
subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Wno-sign-compare',
                str(ROOT / 'main/music_playback/music_owner_host_check.c'),
                '-o', str(owner_exe)], check=True)
subprocess.run([str(owner_exe)], check=True, timeout=15)
