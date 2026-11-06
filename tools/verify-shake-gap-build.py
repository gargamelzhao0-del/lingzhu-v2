"""Verify shake fix identity, fresh compilation and bounded worker stack frame."""
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
assert b'revision=20260928-shake-rearm-v3' in binary
assert b'gyro_only=0' in binary and b'qualified shake pattern span_ms=' in binary
assert b'shake health attempts=' in binary
assert b'busy_skips=%lu quiet_valid_ms=%lu' in binary
assert b'sampling_revision=20260928-qmi-bounded-v2' in binary
path = root / 'main/board/shake_detector.c'
commands = json.loads((build / 'compile_commands.json').read_text())
assert len([c for c in commands if Path(c['file']).resolve() == path.resolve()]) == 1
obj = build / 'esp-idf/main/CMakeFiles/__idf_main.dir/board/shake_detector.c.obj'
policy = root / 'main/board/shake_motion_policy.h'
assert max(path.stat().st_mtime, policy.stat().st_mtime) <= obj.stat().st_mtime <= elf.stat().st_mtime
tool = Path('C:/Espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin/xtensa-esp32s3-elf-objdump.exe')
asm = subprocess.check_output([str(tool), '-d', '--disassemble=shake_detector_task', str(elf)], text=True)
body = asm.split('<shake_detector_task>:', 1)[1]
frame = int(re.search(r'\bentry\s+a1,\s*(0x[0-9a-f]+|\d+)', body)[1], 0)
assert frame <= 512, f'Shake worker frame unexpectedly large: {frame}'
qmi = root/'main/board/qmi8658a_driver.c'
qmi_obj = build/'esp-idf/main/CMakeFiles/__idf_main.dir/board/qmi8658a_driver.c.obj'
assert qmi.stat().st_mtime <= qmi_obj.stat().st_mtime <= elf.stat().st_mtime
sources = ['main/board/shake_detector.c', 'main/board/shake_motion_policy.h',
           'main/shake_motion_policy_host_check.c', 'main/board/qmi8658a_driver.c',
           'main/board/board_drivers.h', 'main/board/shake_detector.h',
           'tools/test-shake-gap.py', 'tools/test-qmi-poll.py', 'tools/test-shake-startup.py']
result = dict(revision='20260928-shake-rearm-v3', samplingRevision='20260928-qmi-bounded-v2', elfSha256=sha,
              binSha256=hashlib.sha256(binary).hexdigest(), bytes=len(binary),
              workerStackFrame=frame, hardwareValidated=False,
              sourceHashes={p: hashlib.sha256((root / p).read_bytes()).hexdigest() for p in sources})
(build / 'shake-gap-verification.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
print(f'PASS: shake revision, fresh compile, worker frame={frame}, BIN/ELF {sha}')
