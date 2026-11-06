"""Verify the boot identity fix is linked into the exact flash/OTA artifact."""
from pathlib import Path
import hashlib
import json
import sys

root=Path(__file__).resolve().parents[1]
build=Path(sys.argv[1]).resolve()
elf=build/'NuwaxAIPartner.elf'
binary=(build/'NuwaxAIPartner.bin').read_bytes()
sha=hashlib.sha256(elf.read_bytes()).hexdigest()
assert binary[176:208].hex()==sha
for marker in (b'revision=ota-identity-v1', b'revision=boot-retry-v1', b'OTA running image confirmed valid',
               b'OTA boot verification finished', b'update_targets_running_partition'):
    assert marker in binary, marker
sources=['main/nuwax_ota_new/nuwax_ota_main.c', 'main/nuwax_ota_new/nuwax_ota_device.c',
         'main/nuwax_ota_new/nuwax_ota_transport.c', 'main/session_protocol.c',
         'main/nuwax_ota_new/nuwax_firmware_report.c']
header=root/'main/nuwax_ota_new/nuwax_ota_main.h'
for name in sources:
    src=root/name
    obj=build/'esp-idf/main/CMakeFiles/__idf_main.dir'/str(Path(name).relative_to('main').with_suffix('.c.obj'))
    assert max(src.stat().st_mtime,header.stat().st_mtime)<=obj.stat().st_mtime<=elf.stat().st_mtime,obj
sources += ['main/nuwax_ota_new/nuwax_ota_main.h','tools/test-ota-identity-cache.py']
result=dict(revision='20260928-ota-boot-retry-v1',version=binary[48:80].split(b'\0')[0].decode(),
 elfSha256=sha,binSha256=hashlib.sha256(binary).hexdigest(),bytes=len(binary),hardwareValidated=False,
 sourceHashes={p:hashlib.sha256((root/p).read_bytes()).hexdigest() for p in sources})
(build/'ota-identity-verification.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
print('PASS: OTA boot hash cache, confirmation markers, fresh objects, BIN/ELF '+sha)
