"""Confirm final wake evidence revision, compile freshness and stack budget."""
from pathlib import Path
import hashlib
import json
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
build = Path(sys.argv[1]).resolve()
elf = build/'NuwaxAIPartner.elf'
binary = (build/'NuwaxAIPartner.bin').read_bytes()
sha = hashlib.sha256(elf.read_bytes()).hexdigest()
assert binary[176:208].hex() == sha
assert b'revision=20260924-wake-evidence-v1' in binary
assert b'score_revision=20261028-score009-v1' in binary
assert b'strong_guarded' in binary and b'evidence_voice_ms=' in binary
assert b'wake feedback deferred revision=wake-pcm-v3' in binary
assert b'wake candidate diagnostics revision=wake-response-v1' in binary
assert b'custom wake word weak detection rejected command_id=' not in binary
commands = json.loads((build/'compile_commands.json').read_text())
source = root/'main/wake_word_detector.c'
assert len([c for c in commands if Path(c['file']).resolve() == source.resolve()]) == 1
obj = build/'esp-idf/main/CMakeFiles/__idf_main.dir/wake_word_detector.c.obj'
header = root/'main/conversation/wake_word_detection_policy.h'
noise_header = root/'main/conversation/wake_noise_policy.h'
assert max(source.stat().st_mtime, header.stat().st_mtime, noise_header.stat().st_mtime) <= obj.stat().st_mtime <= elf.stat().st_mtime
bridge_header = root/'main/nuwax_ui_v28/face/emotion_component_bridge_policy.h'
bridge_obj = build/'esp-idf/main/CMakeFiles/__idf_main.dir/nuwax_ui_v28/face/emotion_animation_player.c.obj'
assert bridge_header.stat().st_mtime <= bridge_obj.stat().st_mtime <= elf.stat().st_mtime
main_obj = build/'esp-idf/main/CMakeFiles/__idf_main.dir/nuwax_ai_partner_main.c.obj'
assert (root/'main/nuwax_ai_partner_main.c').stat().st_mtime <= main_obj.stat().st_mtime <= elf.stat().st_mtime
state_bridge = root/'main/nuwax_ui_v28/nuwax_state_bridge.c'
state_bridge_obj = build/'esp-idf/main/CMakeFiles/__idf_main.dir/nuwax_ui_v28/nuwax_state_bridge.c.obj'
assert state_bridge.stat().st_mtime <= state_bridge_obj.stat().st_mtime <= elf.stat().st_mtime
tool = Path('C:/Espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin/xtensa-esp32s3-elf-objdump.exe')
asm = subprocess.check_output([str(tool), '-d', '--disassemble=wake_word_task', str(elf)], text=True)
body = asm.split('<wake_word_task>:', 1)[1]
frame = int(re.search(r'\bentry\s+a1,\s*(0x[0-9a-f]+|\d+)', body)[1], 0)
assert frame <= 2304, f'Wake task static stack frame exceeded budget: {frame}'
config = (build/'config/sdkconfig.h').read_text()
assert '#define CONFIG_NUWAX_CUSTOM_WAKE_WORD_THRESHOLD_PERCENT 9' in config
assert '#define NUWAX_WAKE_WEAK_THRESHOLD_Q1000 90U' in header.read_text()
assert '#define CONFIG_NUWAX_WAKE_WORD_TASK_STACK_BYTES 8192' in config
sources = ['main/wake_word_detector.c','main/conversation/wake_word_detection_policy.h',
           'main/Kconfig.projbuild', 'sdkconfig.product-ota.defaults',
           'main/conversation/wake_noise_policy.h', 'main/wake_response_host_check.c',
           'tools/test-wake-response.py', 'main/nuwax_ai_partner_main.c', 'sdkconfig.defaults',
           'main/nuwax_ui_v28/face/emotion_component_bridge_policy.h',
           'main/emotion_component_bridge_policy_host_check.c',
           'main/nuwax_ui_v28/nuwax_state_bridge.c',
           'main/wake_word_detection_policy_host_check.c','tools/test-wake-evidence.py',
           'tools/verify-wake-evidence-build.py']
result = dict(revision='20260924-wake-evidence-v1', elfSha256=sha,
              binSha256=hashlib.sha256(binary).hexdigest(), bytes=len(binary),
              workerStackFrame=frame, workerStackBytes=8192, modelThreshold=0.09,
              finalScoreThreshold=0.09, sensitivityRevision='20261028-score009-v1',
              acousticEvidenceRequired=True, hardwareValidated=False,
              responseRevision='wake-pcm-v3', chatterShadowOnly=True,
              sourceHashes={p:hashlib.sha256((root/p).read_bytes()).hexdigest() for p in sources})
(build/'wake-evidence-verification.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
print(f'PASS: wake evidence revision linked, fresh objects; static frame={frame}/8192; BIN/ELF {sha}')
