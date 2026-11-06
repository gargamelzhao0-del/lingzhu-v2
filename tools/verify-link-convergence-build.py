"""Verify the no-card/link-convergence fixes are present in a fresh build."""
from pathlib import Path
import hashlib
import json
import sys

root = Path(__file__).resolve().parents[1]
build = Path(sys.argv[1]).resolve()
elf = build / 'NuwaxAIPartner.elf'
binary = (build / 'NuwaxAIPartner.bin').read_bytes()
sha = hashlib.sha256(elf.read_bytes()).hexdigest()
assert binary[176:208].hex() == sha, 'BIN/ELF mismatch'
assert b'link convergence revision=20260923-link-convergence-v1' in binary
assert b'diagnostic NVS full; retaining RAM reports without online compaction' in binary
assert b'diagnostic NVS namespace compacted after no-space; retrying snapshot' not in binary
sources = ['main/nuwax_ai_partner_main.c', 'main/app_shell.c',
           'main/offline_audio_spool.c', 'main/device_error_report.c']
commands = json.loads((build / 'compile_commands.json').read_text())
for source in sources:
    path = root / source
    assert len([c for c in commands if Path(c['file']).resolve() == path.resolve()]) == 1
    obj = build / 'esp-idf/main/CMakeFiles/__idf_main.dir' / (path.name + '.obj')
    assert path.stat().st_mtime <= obj.stat().st_mtime <= elf.stat().st_mtime, 'Stale object: ' + source
sources += ['tools/test-link-convergence.py', 'main/wifi_background_recovery_host_check.c']
result = dict(revision='20260923-link-convergence-v1', elfSha256=sha,
              binSha256=hashlib.sha256(binary).hexdigest(), bytes=len(binary),
              sourceHashes={p: hashlib.sha256((root / p).read_bytes()).hexdigest() for p in sources},
              hardwareValidated=False)
(build / 'link-convergence-verification.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
print('PASS: link convergence and no-compaction markers, four fresh compile inputs, BIN/ELF ' + sha)
