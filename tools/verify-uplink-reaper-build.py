from pathlib import Path
import hashlib
import json
import sys

root = Path(__file__).resolve().parents[1]
build = Path(sys.argv[1]).resolve()
elf = build / 'NuwaxAIPartner.elf'
binary = (build / 'NuwaxAIPartner.bin').read_bytes()
sha = hashlib.sha256(elf.read_bytes()).hexdigest()
assert binary[176:208].hex() == sha
assert b'recovery=bounded-cancel-v1' in binary
assert b'transport-integrity close attempts elapsed_ms=' in binary
sources = ['main/conversation/audio_uplink.c', 'tools/test-uplink-reaper-budget.py',
           'tools/verify-uplink-reaper-build.py']
obj = build / 'esp-idf/main/CMakeFiles/__idf_main.dir/conversation/audio_uplink.c.obj'
assert (root / sources[0]).stat().st_mtime <= obj.stat().st_mtime <= elf.stat().st_mtime
result = dict(revision='20260927-au05-v1', elfSha256=sha,
              binSha256=hashlib.sha256(binary).hexdigest(), bytes=len(binary),
              hardwareValidated=False,
              sourceHashes={p: hashlib.sha256((root / p).read_bytes()).hexdigest() for p in sources})
(build / 'uplink-reaper-verification.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
print('PASS: AU05 bounded cancellation linked, fresh object, BIN/ELF ' + sha)
