"""Verify the exact compact UI firmware, unchanged configuration and size budget."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('build',type=Path)
p.add_argument('--baseline-build',type=Path,required=True)
p.add_argument('--max-bytes',type=int,default=3932160) # first-stage target: 3.75 MiB
a=p.parse_args()
root=Path(__file__).resolve().parents[1]
build=a.build.resolve();old=a.baseline_build.resolve()
binary=(build/'NuwaxAIPartner.bin').read_bytes()
elf=build/'NuwaxAIPartner.elf'
digest=lambda path:hashlib.sha256(path.read_bytes()).hexdigest()
assert binary[176:208].hex()==digest(elf),'BIN/ELF mismatch'
assert len(binary)<=a.max_bytes,'Compaction target not reached'
assert len(binary)<=4*1024*1024,'Firmware exceeds current OTA manifest limit'
before=json.loads((old/'config/sdkconfig.json').read_text())
after=json.loads((build/'config/sdkconfig.json').read_text())
changes={k:[before.get(k),after.get(k)] for k in before.keys()|after.keys() if before.get(k)!=after.get(k)}
assert not changes,changes
nm='C:/Espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin/xtensa-esp32s3-elf-nm.exe'
output=subprocess.check_output([nm,'-S','--defined-only',str(elf)],text=True)
symbols={r.split()[-1]:int(r.split()[1],16) for r in output.splitlines() if len(r.split())==4}
for name in ['nuwax_low_battery_frames','nuwax_low_battery_frames_data',
             'nuwax_low_battery_static_mask','conversation_bars_data','conversation_ripple_data']:
    assert name in symbols,name
assert 'nuwax_low_battery_masks' not in symbols,'Raw animation still linked'
assert symbols['nuwax_low_battery_static_mask']==8112
sources=['main/nuwax_ui_v28/assets/fonts/nuwax_font_lingzhu.c',
         'main/nuwax_ui_v28/assets/low_battery_assets.c',
         'main/nuwax_ui_v28/assets/low_battery_assets.h',
         'main/nuwax_ui_v28/assets/asset_rle.h',
         'main/nuwax_ui_v28/low_battery_warning.c',
         'main/nuwax_ui_v28/face/conversation_visual.c',
         'main/nuwax_ui_v28/face/conversation_visual_assets.h']
for name in sources:
    unit=name
    if name.endswith('/asset_rle.h'):unit='main/nuwax_ui_v28/low_battery_warning.c'
    elif name.endswith('/conversation_visual_assets.h'):unit='main/nuwax_ui_v28/face/conversation_visual.c'
    elif name.endswith('.h'):unit=name[:-2]+'.c'
    obj=build/'esp-idf/main/CMakeFiles/__idf_main.dir'/(unit.removeprefix('main/')+'.obj')
    assert (root/name).stat().st_mtime_ns<=obj.stat().st_mtime_ns<=elf.stat().st_mtime_ns,name
baseline_music=json.loads((old/'music-verification.json').read_text())
for name,sha in baseline_music['sourceHashes'].items():
    assert digest(root/name)==sha,f'Unrelated audio/UI owner source changed: {name}'
result=dict(version=binary[48:80].split(b'\0')[0].decode(),binSha256=hashlib.sha256(binary).hexdigest(),
    bytes=len(binary),previousBytes=(old/'NuwaxAIPartner.bin').stat().st_size,
    savedBytes=(old/'NuwaxAIPartner.bin').stat().st_size-len(binary),
    otaLimitBytes=4194304,otaHeadroomBytes=4194304-len(binary),
    partitionBytes=4325376,partitionHeadroomBytes=4325376-len(binary),
    configurationChanges=changes,musicAndOwnerSourcesUnchanged=True,hardwareValidated=False,
    steadyStateAdditionalPixelBytes=2112,lowBatteryVisibleAdditionalCacheBytes=16224,
    sourceHashes={name:digest(root/name) for name in sources})
(build/'ui-asset-verification.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
print(json.dumps(result,indent=2))
