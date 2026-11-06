"""Verify merged firmware identity, retained SD/transport and dialogue stack frames."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('build', type=Path)
p.add_argument('--baseline-build', type=Path, required=True)
p.add_argument('--baseline-sources', type=Path, required=True)
p.add_argument('--version', default='7.0.207')
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
build = a.build.resolve()
binary, elf = build / 'NuwaxAIPartner.bin', build / 'NuwaxAIPartner.elf'
image = binary.read_bytes()
assert image[48:80].split(b'\0')[0].decode() == a.version
assert image[176:208].hex() == sha(elf)
assert len(image) < 0x420000
assert sha(build / 'partition_table/partition-table.bin') == sha(
    a.baseline_build / 'partition_table/partition-table.bin')
config = json.loads((build / 'config/sdkconfig.json').read_text())
before = json.loads((a.baseline_build / 'config/sdkconfig.json').read_text())
assert config == before, {k: [before.get(k), config.get(k)] for k in before.keys() | config.keys()
                          if before.get(k) != config.get(k)}
assert config['NUWAX_CONVERSATION_AWAKE_IDLE_TIMEOUT_MS'] == 60000
assert config['NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE']
assert not config['NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE']

# Preserve the complete SD 206 repair and existing network/audio paths.
protected = set()
for pattern in ['sd_*.c', 'sd_*.h', 'sd_*.inc', 'offline*.c', 'offline*.h', 'offline*.inc']:
    protected.update(path.relative_to(root).as_posix() for path in (root / 'main').glob(pattern)
                     if not path.name.endswith('host_check.c'))
protected.update(['main/ws_transport.c', 'main/session_protocol.c', 'main/mic_input.c',
    'main/conversation/speaker_playback.c', 'main/conversation/audio_uplink.c',
    'main/conversation/sleep_transaction.c', 'main/wake_word_detector.c',
    'main/replay_ui_schedule.c', 'main/nuwax_lvgl_port.c',
    'main/board/sd_card_driver.c', 'main/board/sd_probe_trace.c',
    'main/board/sd_probe_trace.h', 'tools/prepare-sd-status-fix.py',
    'tools/cmake/sd-status-fix.cmake',
    'main/audio_modes/meeting/meeting_mode.c',
    'main/audio_modes/monitoring/monitoring_mode.c'])
for name in protected:
    assert sha(root / name) == sha(a.baseline_sources / name), name

main = (root / 'main/nuwax_ai_partner_main.c').read_text(encoding='utf-8')
bridge = (root / 'main/nuwax_ui_v28/nuwax_state_bridge.c').read_text(encoding='utf-8')
assert 'nuwax_state_bridge_update(' not in main
assert main.count('nuwax_state_bridge_update_owned(') == 3
assert main.count('nuwax_listen_resume_state_arm_post_end_asr_bounded(') == 2
assert bridge.index('publication_seq - s_lifecycle_publication_seq') < bridge.index(
    'const bool lifecycle_changed = app_state != s_last_app_state;')
for marker in [b'conversation inactive timeout_ms=', b'conversation_inactive_60s',
               b'local_audio_recovery_exhausted', b'SD probe comparison revision=206']:
    assert marker in image, marker

toolchain = Path('C:/Espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin')
nm = toolchain / 'xtensa-esp32s3-elf-nm.exe'
objdump = toolchain / 'xtensa-esp32s3-elf-objdump.exe'


def frames(image_elf, prefix):
    symbols = subprocess.check_output([str(nm), '-S', '--defined-only', str(image_elf)], text=True)
    names = [m[1] for m in re.finditer(r'^\w+\s+\w+\s+[tT]\s+(\S+)$', symbols, re.M)
             if m[1] == prefix or m[1].startswith(prefix + '.') or m[1].startswith(prefix + '$')]
    assert names, prefix
    sizes = []
    for name in names:
        asm = subprocess.check_output([str(objdump), '-d', '--disassemble=' + name,
                                       str(image_elf)], text=True)
        entries = re.findall(r'\bentry\s+a1,\s*(0x[0-9a-f]+|\d+)', asm)
        assert entries, name
        sizes.extend(int(value, 0) for value in entries)
    return max(sizes)


stack = {}
for current, previous in [('nuwax_state_bridge_update_owned', 'nuwax_state_bridge_update'),
                          ('set_state', 'set_state'), ('shell_task', 'shell_task')]:
    stack[current] = {'bytes': frames(elf, current),
                      'baseline': frames(a.baseline_build / 'NuwaxAIPartner.elf', previous)}
    assert stack[current]['bytes'] <= stack[current]['baseline'] + 64, stack[current]
stack['service_normal_conversation_inactivity'] = {'bytes': frames(elf, 'service_normal_conversation_inactivity')}
assert stack['service_normal_conversation_inactivity']['bytes'] <= 1024
for source, obj in [('main/nuwax_ai_partner_main.c', 'nuwax_ai_partner_main.c.obj'),
                    ('main/nuwax_ui_v28/nuwax_state_bridge.c', 'nuwax_ui_v28/nuwax_state_bridge.c.obj'),
                    ('main/conversation/listen_resume_state.c', 'conversation/listen_resume_state.c.obj'),
                    ('main/conversation/shell_action_dispatch.c', 'conversation/shell_action_dispatch.c.obj')]:
    assert (root / source).stat().st_mtime <= (
        build / 'esp-idf/main/CMakeFiles/__idf_main.dir' / obj).stat().st_mtime <= elf.stat().st_mtime, source

result = dict(version=a.version, otaSha256=sha(binary), elfSha256=sha(elf), bytes=len(image),
    retainedSdVersion='7.0.206', idleTimeoutMs=60000, protocolUnchanged=True,
    sdkconfigUnchanged=True, partitionsUnchanged=True, protectedFiles=sorted(protected),
    staticStackFrames=stack, hardwareValidated=False, verificationToolSha256=sha(Path(__file__)))
(build / 'dialogue-r207-verification.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
print(json.dumps(result, ensure_ascii=False, indent=2))
