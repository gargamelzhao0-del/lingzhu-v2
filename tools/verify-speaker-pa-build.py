"""Verify the PA sequence changes are present in the built OTA application."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('build', type=Path)
p.add_argument('--nm', default='C:/Espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin/xtensa-esp32s3-elf-nm.exe')
a = p.parse_args()
b = a.build.resolve()
root = Path(__file__).resolve().parents[1]
elf = b / 'NuwaxAIPartner.elf'
binary = (b / 'NuwaxAIPartner.bin').read_bytes()
elf_sha = hashlib.sha256(elf.read_bytes()).hexdigest()
assert binary[176:208].hex() == elf_sha, 'BIN/ELF identity mismatch'
symbols = subprocess.check_output([a.nm, '--defined-only', str(elf)], text=True)
names = {line.split()[-1] for line in symbols.splitlines() if line.split()}
required = ['nuwax_vocat_audio_board_force_speaker_mute',
            'nuwax_vocat_audio_board_prepare_speaker_playback',
            'nuwax_speaker_output_stop', 'nuwax_speaker_output_write_pcm16']
for name in required:
    assert name in names, f'Missing symbol: {name}'
markers = [b'ES8311 init ok addr8=0x%02x volume=55 pa=GPIO%d held_off=1',
           b'pa_muted_dma_silence', b'quieting=pa_off_dma_silence',
           b'PA prepare rollback failed:', b'audio init PA mute failed']
for marker in markers:
    assert marker in binary, f'Missing PA sequence marker: {marker!r}'
assert b'ES8311 init PA settle' not in binary, 'Old init-time PA enable remains'
sources = ['main/board/vocat_audio_board.c', 'main/speaker_output.c',
           'main/conversation/speaker_playback.c']
source_hashes = {}
for name in sources:
    src = root / name
    obj = b / 'esp-idf/main/CMakeFiles/__idf_main.dir' / (name.removeprefix('main/') + '.obj')
    assert obj.is_file() and obj.stat().st_mtime_ns >= src.stat().st_mtime_ns, f'Stale object: {name}'
    source_hashes[name] = hashlib.sha256(src.read_bytes()).hexdigest()
result = dict(revision='pa-sequence-v1', elfSha256=elf_sha,
              binSha256=hashlib.sha256(binary).hexdigest(),
              sourceHashes=source_hashes, requiredSymbols=required,
              integrationMarkersPresent=True, hardwareValidated=False)
(b / 'speaker-pa-verification.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
print('PASS: PA sequence markers, fresh objects, BIN/ELF identity ' + elf_sha)
