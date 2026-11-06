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
assert b'ownership=intrusive-v1' in binary
assert b'uplink detached cleanup handoff failed; caller retains session' not in binary
sources = ['main/conversation/audio_uplink.c', 'main/conversation/audio_uplink.h',
           'tools/test-uplink-cleanup-handoff.py', 'tools/fixtures/uplink-cleanup-before-au04.c']
obj = build / 'esp-idf/main/CMakeFiles/__idf_main.dir/conversation/audio_uplink.c.obj'
assert max((root / p).stat().st_mtime for p in sources[:2]) <= obj.stat().st_mtime <= elf.stat().st_mtime
result = dict(revision='20260924-uplink-handoff-v1', elfSha256=sha,
              binSha256=hashlib.sha256(binary).hexdigest(), bytes=len(binary),
              hardwareValidated=False,
              sourceHashes={p: hashlib.sha256((root / p).read_bytes()).hexdigest() for p in sources})
(build / 'uplink-handoff-verification.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
print('PASS: AU04 allocation-free handoff linked, fresh object, BIN/ELF ' + sha)
