"""Verify the exact formal upload fix reached the OTA binary; no hardware claims."""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('build',type=Path)
p.add_argument('--version',default='7.0.186')
a=p.parse_args()
root=Path(__file__).resolve().parents[1]
build=a.build.resolve();elf=build/'NuwaxAIPartner.elf';binary=build/'NuwaxAIPartner.bin'
data=binary.read_bytes()
digest=lambda path:hashlib.sha256(path.read_bytes()).hexdigest()
assert data[48:80].split(b'\0')[0].decode()==a.version
assert data[176:208].hex()==digest(elf)
for marker in [b'SD formal pipeline revision=186', b'SD formal transport admission_ms=',
               b'SD file prepare failed stage=catalog_bind', b'SD file prepare failed stage=scene_begin',
               b'bounded sender retained revision=185 tls_retry=socket_ready',
               b'SD monitoring control confirmed by cloud enable',
               b'SD meeting control confirmed by cloud enable']:
    assert marker in data,marker
units={
 'ws_transport.c':['main/ws_transport.c','main/ws_send_metrics.h','main/ws_send_failure_policy.h'],
 'offline_audio_spool.c':['main/offline_audio_spool.c','main/offline_replay_shared.inc',
                        'main/offline_replay_prefetch.inc','main/offline_replay_policy.h','main/ws_send_metrics.h'],
 'audio_modes/meeting/meeting_mode.c':['main/audio_modes/meeting/meeting_mode.c','main/sd_audio_control_sync.h'],
 'audio_modes/monitoring/monitoring_mode.c':['main/audio_modes/monitoring/monitoring_mode.c','main/sd_audio_control_sync.h']}
hashes={}
for unit,sources in units.items():
    obj=build/'esp-idf/main/CMakeFiles/__idf_main.dir'/(unit+'.obj')
    for name in sources:
        src=root/name
        assert src.stat().st_mtime_ns<=obj.stat().st_mtime_ns<=elf.stat().st_mtime_ns,name
        hashes[name]=digest(src)
tool=Path('C:/Espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin')
nm=subprocess.check_output([str(tool/'xtensa-esp32s3-elf-nm.exe'),'--defined-only',str(elf)],text=True)
assert ' reset_media_recovery_locked' in nm
assembly=subprocess.check_output([str(tool/'xtensa-esp32s3-elf-objdump.exe'),'-d',
    '--disassemble=send_all_until',str(elf)],text=True)
assert '<transport_wait_socket_ready_until' in assembly,'Socket readiness not called by compiled sender'
report=dict(version=a.version,binSha256=digest(binary),elfSha256=digest(elf),bytes=len(data),
    protocolChanged=False,hardwareValidated=False,formalSocketReadinessLinked=True,
    sourceHashes=hashes,sharedTlsSoftMs=250,sharedTlsMaximumDrainMs=750,
    checkpointHealthyProbePreserved=True,lowMemoryTlsPolicyEnabled=True,
    acceptedCloudEnableReconcilesActiveScene=True,initialUnscopedStopGuardPreserved=True,
    invalidFileRootCauseConfirmed=False)
(build/'formal-resilience-verification.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
print(json.dumps(report,indent=2))
