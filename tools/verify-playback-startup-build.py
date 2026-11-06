"""Bind startup recovery fixes to fresh production objects and the flash BIN."""
from pathlib import Path
import hashlib
import json
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
build = Path(sys.argv[1]).resolve()
elf = build / 'NuwaxAIPartner.elf'
binary = (build / 'NuwaxAIPartner.bin').read_bytes()
sha = hashlib.sha256(elf.read_bytes()).hexdigest()
assert binary[176:208].hex() == sha, 'BIN/ELF mismatch'
for marker in [b'revision=20260923-startup-v1', b'startup_buffer_timeout',
               b'tts_first_pcm_timeout', b'power_key_sleep_metadata_only']:
    assert marker in binary, marker
sources = ['main/conversation/speaker_playback.c', 'main/conversation/speaker_playback.h',
           'main/conversation/speaker_jitter_policy.h', 'main/nuwax_ai_partner_main.c',
           'tools/test-playback-startup.py']
commands = json.loads((build / 'compile_commands.json').read_text())
for name in [sources[0], sources[3]]:
    path = root / name
    assert len([c for c in commands if Path(c['file']).resolve() == path.resolve()]) == 1
    obj = build / 'esp-idf/main/CMakeFiles/__idf_main.dir' / (name.removeprefix('main/') + '.obj')
    assert path.stat().st_mtime <= obj.stat().st_mtime <= elf.stat().st_mtime
tool = Path('C:/Espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin/xtensa-esp32s3-elf-objdump.exe')
frames = {}
for fn in ['nuwax_speaker_playback_task', 'speaker_wait_for_jitter_watermark']:
    asm = subprocess.check_output([str(tool), '-d', '--disassemble=' + fn, str(elf)], text=True)
    if '<' + fn + '>:' in asm:
        frame = int(re.search(r'\bentry\s+a1,\s*(0x[0-9a-f]+|\d+)', asm.split('<' + fn + '>:', 1)[1])[1], 0)
        assert frame <= 1024, (fn, frame)
        frames[fn] = frame
assert frames, 'speaker functions not found'
result = dict(revision='20260923-startup-v1', elfSha256=sha,
              binSha256=hashlib.sha256(binary).hexdigest(), bytes=len(binary),
              stackFrames=frames, hardwareValidated=False,
              sourceHashes={p: hashlib.sha256((root / p).read_bytes()).hexdigest() for p in sources})
(build / 'playback-startup-verification.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
print(f'PASS: startup recovery markers, fresh objects, stack frames {frames}, BIN/ELF {sha}')
