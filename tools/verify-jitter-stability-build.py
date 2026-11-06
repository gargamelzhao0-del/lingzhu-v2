"""Verify compiled playback policy revision and immutable firmware identity."""
from pathlib import Path
import hashlib
import json
import sys

root = Path(__file__).resolve().parents[1]
build = Path(sys.argv[1]).resolve()
elf = build/'NuwaxAIPartner.elf'
binary = (build/'NuwaxAIPartner.bin').read_bytes()
sha = hashlib.sha256(elf.read_bytes()).hexdigest()
assert binary[176:208].hex() == sha
assert b'revision=20260924-jitter-stability-v1 short_rebuffer_breaks_streak=1' in binary
source = root/'main/conversation/speaker_playback.c'
header = root/'main/conversation/speaker_jitter_policy.h'
obj = build/'esp-idf/main/CMakeFiles/__idf_main.dir/conversation/speaker_playback.c.obj'
commands = json.loads((build/'compile_commands.json').read_text())
assert len([c for c in commands if Path(c['file']).resolve() == source.resolve()]) == 1
assert max(source.stat().st_mtime,header.stat().st_mtime) <= obj.stat().st_mtime <= elf.stat().st_mtime
startup = json.loads((build/'playback-startup-verification.json').read_text())
assert startup['elfSha256'] == sha
sources = ['main/conversation/speaker_playback.c','main/conversation/speaker_jitter_policy.h',
           'main/speaker_jitter_policy_host_check.c','tools/test-jitter-stability.py',
           'tools/verify-jitter-stability-build.py']
result = dict(revision='20260924-jitter-stability-v1',elfSha256=sha,
              binSha256=hashlib.sha256(binary).hexdigest(),bytes=len(binary),
              speakerStackFrames=startup['stackFrames'],hardwareValidated=False,
              sourceHashes={p:hashlib.sha256((root/p).read_bytes()).hexdigest() for p in sources})
(build/'jitter-stability-verification.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
print(f'PASS: jitter stability revision linked, fresh playback policy, BIN/ELF {sha}')
