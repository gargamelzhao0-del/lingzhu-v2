"""Verify the linked audio ownership revision and BIN/ELF identity; no device I/O."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('build', type=Path)
args = parser.parse_args()
build = args.build.resolve()
elf = build / 'NuwaxAIPartner.elf'
binary = (build / 'NuwaxAIPartner.bin').read_bytes()
sha = hashlib.sha256(elf.read_bytes()).hexdigest()
assert binary[176:208].hex() == sha, 'BIN/ELF pair mismatch'
nm = 'C:/Espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin/xtensa-esp32s3-elf-nm.exe'
symbols = subprocess.check_output([nm, '--defined-only', str(elf)], text=True)
names = {line.split()[-1] for line in symbols.splitlines() if line.split()}
for name in ['nuwax_audio_board_init', 'nuwax_audio_board_deinit', 's_refs',
             's_next_ref_token', 'nuwax_speaker_output_deinit', 'nuwax_mic_input_deinit']:
    assert name in names, f'Missing ownership symbol: {name}'
for marker in [b'audio board ownership revision=20260922-audio-owner-v1',
               b'stale audio board ref rejected', b'board owns cleanup']:
    assert marker in binary, f'Missing ownership marker: {marker!r}'
assert b'lease output still owns a channel' in binary
root = Path(__file__).resolve().parents[1]
sources = ['main/mic_input.c', 'main/mic_input.h', 'main/board/audio_board.c', 'main/board/audio_board.h']
for relative, headers in [('mic_input.c', ['main/mic_input.h']), ('board/audio_board.c', ['main/board/audio_board.h'])]:
    obj = build/'esp-idf/main/CMakeFiles/__idf_main.dir'/(relative+'.obj')
    assert max((root/p).stat().st_mtime for p in ['main/'+relative]+headers) <= obj.stat().st_mtime <= elf.stat().st_mtime
result = dict(revision='20260922-audio-owner-v1', admissionRevision='20260924-audio-admission-v1',
              sourceHashes={p:hashlib.sha256((root/p).read_bytes()).hexdigest() for p in sources}, elfSha256=sha,
              binSha256=hashlib.sha256(binary).hexdigest(), bytes=len(binary),
              referenceRegistryBytes=64, referenceHandleBytes=8,
              linkedMarkersVerified=True, hardwareValidated=False)
(build / 'audio-owner-verification.json').write_text(
    json.dumps(result, indent=2), encoding='utf-8')
print('PASS: audio owner fixes linked, BIN/ELF matched; ELF SHA256=' + sha)
