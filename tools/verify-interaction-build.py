"""Verify the actual linked interaction fixes, stack frame and BIN/ELF pair."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('build', type=Path)
args = parser.parse_args()
build = args.build.resolve()
toolchain = Path('C:/Espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin')
elf = build / 'NuwaxAIPartner.elf'
image = (build / 'NuwaxAIPartner.bin').read_bytes()
sha = hashlib.sha256(elf.read_bytes()).hexdigest()
assert image[176:208].hex() == sha, 'BIN/ELF pair mismatch'
symbols = subprocess.check_output([str(toolchain / 'xtensa-esp32s3-elf-nm.exe'),
                                  '--defined-only', str(elf)], text=True)
assert 'nuwax_emotion_animation_player_render_scene' in symbols
assert 'nuwax_speaker_playback_service_volume_persistence' in symbols
assert 'nuwax_i2c_bus_sample_transfer_done' in symbols
assert 'nuwax_i2c_bus_transfer_done' in symbols
assert 's_transition_cache_build_mask' in symbols
assert 's_volume_cache_settle_us' in symbols
for marker in [b'interaction optimization revision=20260922-interaction-v3',
               b'speaker volume deferred save value=', b'I2C sample calls=',
               b'I2C window client=', b'component_dirty=1 volume_settle_ms=150',
               b'face RGB565 scene active overlays=lvgl ownership=shared',
               b'capture owner work allocated heap=psram bytes=', b'sprite_frames=']:
    assert marker in image, f'Missing linked marker: {marker!r}'
names = [line.split()[-1] for line in symbols.splitlines()
         if line.split()[-1].startswith('stream_microphone_audio')]
assert names
frames = {}
for name in names:
    asm = subprocess.check_output([str(toolchain / 'xtensa-esp32s3-elf-objdump.exe'),
                                   '-d', '--disassemble=' + name, str(elf)], text=True)
    # Only the first entry after the exact function label is an instruction;
    # decoding embedded literal pools as instructions gives bogus frame sizes.
    body = asm.split('<' + name + '>:', 1)[1]
    match = re.search(r'\bentry\s+a1,\s*(0x[0-9a-f]+|\d+)', body)
    assert match, name
    frames[name] = int(match[1], 0)
    assert frames[name] <= 4096, f'Capture stack frame regressed: {frames}'
result = dict(revision='20260922-interaction-v3', elfSha256=sha,
              binSha256=hashlib.sha256(image).hexdigest(),
              captureStackFrames=frames, previousCaptureStackFrame=5424,
              linkedMarkersVerified=True, hardwareValidated=False)
(build / 'interaction-verification.json').write_text(
    json.dumps(result, indent=2), encoding='utf-8')
print('PASS: linked interaction fixes, capture stack ' + str(frames) + ', ELF ' + sha)
