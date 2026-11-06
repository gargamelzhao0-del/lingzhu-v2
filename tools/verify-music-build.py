"""Check the linked music firmware and OTA identity without touching a device."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('build', type=Path)
p.add_argument('--gateway', default='http://192.168.31.10:8798')
p.add_argument('--version', default='7.0.184')
args = p.parse_args()
build = args.build.resolve()
root = Path(__file__).resolve().parents[1]
config = (build / 'config/sdkconfig.h').read_text(encoding='utf-8')
binary = (build / 'NuwaxAIPartner.bin').read_bytes()
elf = build / 'NuwaxAIPartner.elf'
elf_sha = hashlib.sha256(elf.read_bytes()).hexdigest()
assert binary[176:208].hex() == elf_sha, 'BIN and ELF do not match'
version = binary[48:80].split(b'\0')[0].decode()
assert version == args.version and re.fullmatch(r'7\.0\.\d+', version), version
for feature in ('MUSIC_PLAYBACK_ENABLE', 'MUSIC_MP3_ENABLE', 'MUSIC_CONTROL_TEST_ENABLE'):
    assert re.search(r'^#define CONFIG_NUWAX_' + feature + r' 1$', config, re.M), feature
assert '#define CONFIG_NUWAX_MUSIC_URL_TEST_ENABLE 1' not in config
assert f'#define CONFIG_NUWAX_MUSIC_CONTROL_TEST_URL "{args.gateway}"' in config
nm = 'C:/Espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin/xtensa-esp32s3-elf-nm.exe'
symbols = subprocess.check_output([nm, '--defined-only', str(elf)], text=True)
names = {line.split()[-1] for line in symbols.splitlines() if line.split()}
required = ('nuwax_music_playback_start', 'nuwax_music_playback_cancel',
            'nuwax_music_playback_audio_active', 'nuwax_music_playback_output_quiesced',
            'nuwax_audio_board_acquire_music', 'nuwax_audio_board_music_rate',
            'nuwax_vocat_audio_board_set_music_rate', 'MP3Decode',
            'nuwax_speaker_playback_deinit_output',
            'nuwax_music_test_control_init', 'nuwax_music_test_control_take',
            'nuwax_music_status_init', 'nuwax_music_status_reserve', 'nuwax_music_status_post',
            'nuwax_emotion_animation_player_transition_active')
for name in required:
    assert name in names, f'Not linked: {name}'
assert b'music audio handoff ready' in binary, 'Music handoff fix not linked'
assert b'first_audio_timeout' in binary, 'First-audio deadline not linked'
assert b'music status delivery expired' in binary, 'Status outbox not linked'
source_hashes = {}
sources = ['main/music_playback/music_playback.c', 'main/music_playback/music_test_control.c',
           'main/board/audio_board.c', 'main/board/vocat_audio_board.c',
           'main/conversation/speaker_playback.c', 'main/speaker_output.c',
           'main/nuwax_ai_partner_main.c', 'main/music_playback/product_music.inc',
           'main/music_playback/music_status.c', 'main/music_playback/music_status.h',
           'main/music_playback/music_playback.h',
           'main/nuwax_ui_v28/nuwax_drawer.c',
           'main/nuwax_ui_v28/face/emotion_animation_player.c',
           'main/nuwax_ui_v28/face/emotion_animation_player.h']
for name in sources:
    obj_source = 'main/nuwax_ai_partner_main.c' if name.endswith('.inc') else name
    if obj_source.endswith('.h'):
        obj_source = obj_source[:-2] + '.c'
    obj = build / 'esp-idf/main/CMakeFiles/__idf_main.dir' / (obj_source.removeprefix('main/') + '.obj')
    src = root / name
    assert obj.is_file() and src.stat().st_mtime_ns <= obj.stat().st_mtime_ns <= elf.stat().st_mtime_ns, f'Stale object: {name}'
    source_hashes[name] = hashlib.sha256(src.read_bytes()).hexdigest()
result = dict(version=version, otaReleaseSequence=7000000+int(version.split('.')[-1]),
              binSha256=hashlib.sha256(binary).hexdigest(), elfSha256=elf_sha,
              bytes=len(binary), gateway=args.gateway, requiredSymbols=required,
              cloudMusic=True, lanControl=True, legacyAutomaticPlayback=False,
              hardwareValidated=False, sourceHashes=source_hashes)
(build / 'music-verification.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
print(json.dumps(result, indent=2))
