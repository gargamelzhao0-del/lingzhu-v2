"""Compile the real audio_frame.c with a host-only ESP error-code stub.

Run from any directory with Python 3 and a C11 GCC on PATH. Nothing is flashed.
The executable checks the production codec, not a second implementation of it.
"""
from pathlib import Path
import json
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'build-host-checks' / 'handbook-frame'
STUB = '''#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NO_MEM 0x101
'''
C = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "audio_frame.h"

int main(void) {
    /* The production PCM view is native int16_t. This exercise targets the
       same little-endian PCM interpretation as ESP32-S3 and this host. */
    const uint16_t endian = 1;
    assert(*(const uint8_t *)&endian == 1);
    int16_t pcm[320] = {1000, -1000, 0};
    _Alignas(int16_t) uint8_t frame[662];
    size_t bytes = 0;
    assert(nuwax_audio_frame_build_pcm16(frame, sizeof frame, pcm, 320,
        7, 140, NUWAX_AUDIO_FLAG_START, 16000, 1, 20, &bytes) == ESP_OK);
    assert(bytes == 662);
    /* Independent expected bytes catch a shared encoder/parser mistake. */
    const uint8_t expected[22] = {
        'N','A',1,1,1,1,0x80,0x3e,20,0,7,0,0,0,140,0,0,0,0x80,2,0,0
    };
    assert(memcmp(frame, expected, 22) == 0);
    assert(frame[22] == 0xe8 && frame[23] == 0x03);
    assert(frame[24] == 0x18 && frame[25] == 0xfc);
    const int16_t *view = NULL;
    size_t samples = 0;
    uint32_t sequence = 0;
    uint16_t rate = 0;
    uint8_t channels = 0;
    assert(nuwax_audio_frame_parse_pcm16(frame, bytes, &view, &samples,
        &sequence, &rate, &channels) == ESP_OK);
    assert(samples == 320 && sequence == 7 && rate == 16000 && channels == 1);
    assert(view[0] == 1000 && view[1] == -1000);
    /* A borrowed view observes later writes: the parser did not copy PCM. */
    frame[22] = 0;
    frame[23] = 0;
    assert(view[0] == 0);
    size_t peek = 0;
    assert(nuwax_audio_frame_peek(frame, bytes - 1, NULL, NULL, &peek)
        == ESP_ERR_INVALID_SIZE);
    assert(nuwax_audio_frame_parse_pcm16(frame, bytes - 1, &view, &samples,
        NULL, NULL, NULL) == ESP_ERR_INVALID_SIZE);
    assert(nuwax_audio_frame_build_pcm16(frame, 661, pcm, 320, 7, 140, 0,
        16000, 1, 20, &bytes) == ESP_ERR_NO_MEM);
    frame[0] = 'X';
    assert(nuwax_audio_frame_peek(frame, sizeof frame, NULL, NULL, &peek)
        == ESP_ERR_INVALID_ARG);
    frame[0] = 'N';
    frame[18] = 0x7f; /* 639-byte odd payload is not PCM16. */
    assert(nuwax_audio_frame_parse_pcm16(frame, sizeof frame, &view, &samples,
        NULL, NULL, NULL) == ESP_ERR_INVALID_SIZE);
    frame[18] = 0x80;
    _Alignas(int16_t) uint8_t shifted[663];
    memcpy(shifted + 1, frame, 662);
    assert(nuwax_audio_frame_parse_pcm16(shifted + 1, 662, &view, &samples,
        NULL, NULL, NULL) == ESP_ERR_INVALID_SIZE);
    puts("PASS: header, PCM bytes, roundtrip, borrowed view, short buffer, truncation, magic, odd length, alignment");
}
'''

def main():
    gcc = shutil.which('gcc')
    if not gcc:
        raise SystemExit('GCC is required. Install/select a C11 host compiler, then retry.')
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / 'esp_err.h').write_text(STUB, encoding='utf-8')
    (OUT / 'check.c').write_text(C, encoding='utf-8')
    exe = OUT / 'frame-check.exe'
    subprocess.run([gcc, '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I', str(OUT), '-I', str(ROOT / 'main'),
                    str(OUT / 'check.c'), str(ROOT / 'main/audio_frame.c'),
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    print(json.dumps({'production_source': 'main/audio_frame.c',
                      'hardware_tested': False, 'passed': True}))

if __name__ == '__main__':
    main()
