"""Check the built SD speed profile and unchanged product/UI sources against r185."""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('build',type=Path)
p.add_argument('--baseline-build',type=Path,required=True)
a=p.parse_args()
root=Path(__file__).resolve().parents[1]
build=a.build.resolve();baseline=a.baseline_build.resolve()
digest=lambda path:hashlib.sha256(path.read_bytes()).hexdigest()
binary=build/'NuwaxAIPartner.bin';elf=build/'NuwaxAIPartner.elf';data=binary.read_bytes()
assert data[48:80].split(b'\0')[0]==b'7.0.186'
assert data[176:208].hex()==digest(elf)
assert len(data)<=4194304
before=json.loads((baseline/'config/sdkconfig.json').read_text())
after=json.loads((build/'config/sdkconfig.json').read_text())
changes={k:[before.get(k),after.get(k)] for k in before.keys()|after.keys() if before.get(k)!=after.get(k)}
assert changes=={'MBEDTLS_SSL_OUT_CONTENT_LEN':[4096,8192]},changes
for marker in [b'SD formal pipeline revision=186',b'SD formal writes bytes=',
               b'TLS negotiated version=',b'core=%u priority=%u']:
    assert marker in data,marker
units={
    'ws_transport.c':['main/ws_transport.c','main/ws_send_metrics.h'],
    'offline_audio_spool.c':['main/offline_audio_spool.c','main/offline_replay_shared.inc',
        'main/offline_replay_policy.h','main/offline_io_schedule.h','main/offline_replay_prefetch.inc']}
hashes={}
for unit,sources in units.items():
    obj=build/'esp-idf/main/CMakeFiles/__idf_main.dir'/(unit+'.obj')
    for name in sources:
        src=root/name
        assert src.stat().st_mtime_ns<=obj.stat().st_mtime_ns<=elf.stat().st_mtime_ns,name
        hashes[name]=digest(src)
unchanged=set()
for report in ['music-verification.json','ui-asset-verification.json']:
    for name,sha in json.loads((baseline/report).read_text())['sourceHashes'].items():
        assert digest(root/name)==sha,'Product/audio/UI source changed: '+name
        unchanged.add(name)
tool=Path('C:/Espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin')
names=subprocess.check_output([str(tool/'xtensa-esp32s3-elf-nm.exe'),'--defined-only',str(elf)],text=True)
encoders=[line.split()[-1] for line in names.splitlines() if line.split() and
          line.split()[-1].startswith('send_frame_unlocked_buffered')]
assert len(encoders)==1,encoders
asm=subprocess.check_output([str(tool/'xtensa-esp32s3-elf-objdump.exe'),'-d',
    '--disassemble='+encoders[0],str(elf)],text=True)
# The legacy encoder retains its ~4 KiB local array; TLS scratch lives in PSRAM.
import re
frame=re.search(r'\bentry\s+a1,\s*(0x[0-9a-f]+|\d+)',asm)
assert frame and int(frame[1],0)<=4608,asm[:400]
result=dict(version='7.0.186',binSha256=digest(binary),elfSha256=digest(elf),bytes=len(data),
    otaHeadroomBytes=4194304-len(data),configurationChanges=changes,
    sourceHashes=hashes,unchangedProductSources=sorted(unchanged),
    encoderStackFrameBytes=int(frame[1],0),formalSenderCore=1,formalSenderPriority=5,
    captureReserveMinimumBytes=256*1024+16384+32768,
    protocolChanged=False,hardwareValidated=False,measuredThroughputKiBs=None)
(build/'sd-formal-speed-verification.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
print(json.dumps({k:v for k,v in result.items() if k not in ('sourceHashes','unchangedProductSources')},indent=2))
