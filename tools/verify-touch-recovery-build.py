"""Verify linked touch arbitration fixes, product switches and OTA identity."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('build', type=Path)
p.add_argument('--version', default='7.0.190')
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
build = a.build.resolve()
toolchain = Path('C:/Espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin')
binary, elf = build/'NuwaxAIPartner.bin', build/'NuwaxAIPartner.elf'
digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
image = binary.read_bytes()
assert image[48:80].split(b'\0')[0].decode() == a.version
assert image[176:208].hex() == digest(elf), 'BIN/ELF pair mismatch'
assert len(image) <= 0x420000, 'OTA partition exceeded'
for marker in [b'head touch agent deferred for ASR token=',
               b'deferred_asr_timeout', b'deferred_asr_session_changed',
               b'deferred_no_speech_not_recoverable',
               b'qualified head touch blocked by lifecycle owner']:
    assert marker in image, f'Missing implementation marker {marker!r}'
symbols = subprocess.check_output([str(toolchain/'xtensa-esp32s3-elf-nm.exe'),
                                  '--defined-only', str(elf)], text=True)
for symbol in ['nuwax_head_touch_agent_intent_defer_asr',
               'nuwax_head_touch_agent_intent_accept_no_speech',
               'nuwax_head_touch_agent_intent_asr_expired']:
    assert re.search(r'\b'+symbol+r'$', symbols, re.M), symbol
frames = {}
for line in symbols.splitlines():
    name = line.split()[-1]
    if not name.startswith('stream_microphone_audio'):
        continue
    asm = subprocess.check_output([str(toolchain/'xtensa-esp32s3-elf-objdump.exe'),
                                   '-d', '--disassemble='+name, str(elf)], text=True)
    body = asm.split('<'+name+'>:', 1)[1]
    match = re.search(r'\bentry\s+a1,\s*(0x[0-9a-f]+|\d+)', body)
    assert match, name
    frames[name] = int(match[1], 0)
    assert frames[name] <= 4096, frames
assert frames
config = json.loads((build/'config/sdkconfig.json').read_text())
assert config.get('NUWAX_HEAD_TOUCH_ENABLE')
assert config.get('NUWAX_TOUCH_INTERRUPT_ACTION_ENABLE')
assert not config.get('NUWAX_TOUCH_WAKE_ACTION_ENABLE', False)
assert config.get('NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE')
assert not config.get('NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE', False)
assert config.get('NUWAX_MUSIC_CONTROL_TEST_ENABLE')
assert config['MBEDTLS_SSL_OUT_CONTENT_LEN'] == 8192
sources = ['main/nuwax_ai_partner_main.c',
           'main/conversation/head_touch_agent_intent.c',
           'main/conversation/head_touch_agent_intent.h',
           'main/conversation/head_touch_control.c',
           'main/nuwax_ui_v28/emotion_display_policy.c',
           'main/nuwax_ui_v28/nuwax_face.c']
result = dict(version=a.version, sizeBytes=len(image), otaSha256=digest(binary),
              elfSha256=digest(elf), captureStackFrames=frames,
              linkedMarkersVerified=True, hardwareValidated=False,
              sources={name:digest(root/name) for name in sources})
(build/'touch-recovery-verification.json').write_text(
    json.dumps(result, indent=2), encoding='utf-8')
print(json.dumps(result, indent=2))
