from pathlib import Path
import hashlib
import json
import sys

root=Path(__file__).resolve().parents[1]
build=Path(sys.argv[1]).resolve()
elf=build/'NuwaxAIPartner.elf';binary=(build/'NuwaxAIPartner.bin').read_bytes()
sha=hashlib.sha256(elf.read_bytes()).hexdigest()
assert binary[176:208].hex()==sha
assert b'revision=media-admission-v1' in binary
assert b'SD mirror closed without staging' in binary
sources=['main/offline_audio_spool.c','main/offline_audio_spool.h',
         'main/offline_audio_mirror_host_check.c','tools/test-sd-media-admission.py']
obj=build/'esp-idf/main/CMakeFiles/__idf_main.dir/offline_audio_spool.c.obj'
assert max((root/p).stat().st_mtime for p in sources[:2]) <= obj.stat().st_mtime <= elf.stat().st_mtime
result=dict(revision='20260924-media-admission-v1',elfSha256=sha,
 binSha256=hashlib.sha256(binary).hexdigest(),bytes=len(binary),hardwareValidated=False,
 sourceHashes={p:hashlib.sha256((root/p).read_bytes()).hexdigest() for p in sources})
(build/'media-admission-verification.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
print('PASS: SD media admission linked, fresh object, BIN/ELF '+sha)
