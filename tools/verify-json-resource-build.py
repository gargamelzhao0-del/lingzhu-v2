from pathlib import Path
import hashlib
import json
import re
import sys

root=Path(__file__).resolve().parents[1]
build=Path(sys.argv[1]).resolve()
elf=build/'NuwaxAIPartner.elf'
binary=(build/'NuwaxAIPartner.bin').read_bytes()
sha=hashlib.sha256(elf.read_bytes()).hexdigest()
assert binary[176:208].hex()==sha
assert b'revision=json-resources-v1' in binary
header=root/'main/json_resource_budget.h'
sources=['main/json_resource_budget.h','tools/test-json-resource-budget.py',
         'tools/verify-json-resource-build.py']
guarded=[]
for p in (root/'main').rglob('*.c'):
    if 'host' in str(p) or '_archive' in str(p):continue
    text=p.read_text(encoding='utf-8-sig')
    assert not re.search(r'\bcJSON_Parse\w*\s*\(',text),p
    if '#include "json_resource_budget.h"' not in text:continue
    rel=p.relative_to(root/'main')
    obj=build/'esp-idf/main/CMakeFiles/__idf_main.dir'/str(rel.with_suffix('.c.obj'))
    assert obj.exists(),obj
    assert max(p.stat().st_mtime,header.stat().st_mtime)<=obj.stat().st_mtime<=elf.stat().st_mtime,obj
    sources.append(p.relative_to(root).as_posix())
    guarded.append(rel.as_posix())
assert len(guarded)==12,guarded
result=dict(revision='20260927-json-resources-v1',elfSha256=sha,
 binSha256=hashlib.sha256(binary).hexdigest(),bytes=len(binary),hardwareValidated=False,
 guardedTranslationUnits=guarded,
 sourceHashes={p:hashlib.sha256((root/p).read_bytes()).hexdigest() for p in sources})
(build/'json-resource-verification.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
print('PASS: JSON ingress marker, 12 fresh guarded translation units, BIN/ELF '+sha)
