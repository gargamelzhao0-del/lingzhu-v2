"""Verify the control-ID fix and unchanged upload/display profile in the OTA."""
from pathlib import Path
import argparse
import hashlib
import json

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('build', type=Path)
p.add_argument('--baseline-build', type=Path, required=True)
p.add_argument('--version', default='7.0.187')
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
build = a.build.resolve()
baseline = a.baseline_build.resolve()
digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
binary = build / 'NuwaxAIPartner.bin'
elf = build / 'NuwaxAIPartner.elf'
data = binary.read_bytes()
assert data[48:80].split(b'\0')[0].decode() == a.version
assert data[176:208].hex() == digest(elf)
assert len(data) <= 4194304
before = json.loads((baseline / 'config/sdkconfig.json').read_text())
after = json.loads((build / 'config/sdkconfig.json').read_text())
changes = {k: [before.get(k), after.get(k)] for k in before.keys() | after.keys()
           if before.get(k) != after.get(k)}
assert not changes, changes
for mode in ['meeting', 'monitoring']:
    assert ('SD ' + mode + ' control sync reply request=%s epoch=%lu accepted=%d '
            'session=%s cloud_session=%s control_session=%s; capture retained').encode() in data
sources = {}
for mode in ['meeting', 'monitoring']:
    unit = f'audio_modes/{mode}/{mode}_mode.c'
    obj = build / 'esp-idf/main/CMakeFiles/__idf_main.dir' / (unit + '.obj')
    for name in ['main/' + unit, 'main/sd_audio_control_sync.h', 'main/sd_audio_control_compat.h']:
        src = root / name
        assert src.stat().st_mtime_ns <= obj.stat().st_mtime_ns <= elf.stat().st_mtime_ns, name
        sources[name] = digest(src)
unchanged = set()
for report in ['sd-formal-speed-verification.json', 'music-verification.json', 'ui-asset-verification.json']:
    evidence = json.loads((baseline / report).read_text())
    for name, sha in evidence['sourceHashes'].items():
        assert digest(root / name) == sha, 'Speed/audio/display source changed: ' + name
        unchanged.add(name)
assert b'SD formal pipeline revision=186' in data
for name in ['main/audio_modes/meeting/meeting_protocol.c', 'main/audio_modes/monitoring/monitoring_protocol.c',
             'main/sd_audio_protocol.h', 'main/sd_audio_scene_protocol.h']:
    assert (root / name).stat().st_mtime_ns < (baseline / 'NuwaxAIPartner.elf').stat().st_mtime_ns, name
    sources[name] = digest(root / name)
result = dict(version=a.version, binSha256=digest(binary), elfSha256=digest(elf), bytes=len(data),
              otaHeadroomBytes=4194304-len(data), configurationChanges=changes,
              sourceHashes=sources, unchangedProductSources=sorted(unchanged),
              protocolChanged=False, hardwareValidated=False, measuredThroughputKiBs=None)
(build / 'sd-control-exit-verification.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
print(json.dumps({k: v for k, v in result.items() if k not in ('sourceHashes', 'unchangedProductSources')}, indent=2))
