from pathlib import Path
import hashlib,json,sys
root=Path(__file__).resolve().parents[1];build=Path(sys.argv[1]).resolve()
elf=build/'NuwaxAIPartner.elf';binary=(build/'NuwaxAIPartner.bin').read_bytes();sha=hashlib.sha256(elf.read_bytes()).hexdigest()
assert binary[176:208].hex()==sha
assert b'owned playback RX rejected; ending unstarted round' in binary
assert b'normal playback BEGIN overlaps an unresolved audio stream' in binary
sources=['main/nuwax_ai_partner_main.c','main/session_protocol.h','main/conversation/turn_guard.c','main/conversation/turn_guard.h','tools/test-rejected-playback-owner.py']
for relative,headers in [('nuwax_ai_partner_main.c',['main/session_protocol.h','main/conversation/turn_guard.h']),('conversation/turn_guard.c',['main/conversation/turn_guard.h'])]:
 obj=build/'esp-idf/main/CMakeFiles/__idf_main.dir'/(relative+'.obj')
 assert max((root/p).stat().st_mtime for p in ['main/'+relative]+headers)<=obj.stat().st_mtime<=elf.stat().st_mtime
result=dict(revision='20260924-rejected-playback-v1',elfSha256=sha,binSha256=hashlib.sha256(binary).hexdigest(),bytes=len(binary),hardwareValidated=False,sourceHashes={p:hashlib.sha256((root/p).read_bytes()).hexdigest() for p in sources})
(build/'rejected-playback-verification.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
print('PASS: rejected playback terminal cleanup linked and fresh; BIN/ELF '+sha)
