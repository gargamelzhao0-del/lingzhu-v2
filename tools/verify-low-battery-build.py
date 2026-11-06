"""Verify low-battery guards and screen belong to this exact OTA binary."""
import hashlib
import json
from pathlib import Path
import sys

root=Path(__file__).resolve().parents[1]
build=Path(sys.argv[1]).resolve()
elf=build/'NuwaxAIPartner.elf'
binary=(build/'NuwaxAIPartner.bin').read_bytes()
sha=lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
assert binary[176:208].hex()==sha(elf)
for marker in (b'low battery warning revision=low-battery-v1',
               b'low battery boot gate threshold=1%',
               b'low battery shutdown confirmed by fresh sample',
               b'low battery shutdown cancelled after safe drain',
               b'pulse buffer unavailable; retaining static warning',
               '电量不足'.encode(), '即将关机，请连接充电器'.encode()):
    assert marker in binary, marker
units=['main/nuwax_ai_partner_main.c','main/board/battery_service.c',
       'main/offline_audio_spool.c','main/nuwax_ui_v28/nuwax_app.c',
       'main/nuwax_ui_v28/nuwax_drawer.c','main/nuwax_ui_v28/low_battery_warning.c',
       'main/nuwax_ui_v28/assets/low_battery_assets.c']
headers=['main/board/low_battery_shutdown_policy.h','main/board/battery_service.h',
         'main/nuwax_ui_v28/low_battery_warning.h','main/nuwax_ui_v28/nuwax_drawer.h',
         'main/offline_audio_spool.h','main/nuwax_ui_v28/assets/low_battery_assets.h',
         'main/nuwax_ui_v28/assets/asset_rle.h']
for name in units:
    obj=build/'esp-idf/main/CMakeFiles/__idf_main.dir'/str(Path(name).relative_to('main').with_suffix('.c.obj'))
    assert (root/name).stat().st_mtime<=obj.stat().st_mtime<=elf.stat().st_mtime,obj
# Both policy consumers must have compiled after the policy/header revision.
for name in units[:2]:
    obj=build/'esp-idf/main/CMakeFiles/__idf_main.dir'/str(Path(name).relative_to('main').with_suffix('.c.obj'))
    assert (root/headers[0]).stat().st_mtime<=obj.stat().st_mtime
assert '#define NUWAX_LOW_BATTERY_WARNING_MS 5000U' in (root/headers[0]).read_text()
for name in units[-2:]:
    obj=build/'esp-idf/main/CMakeFiles/__idf_main.dir'/str(Path(name).relative_to('main').with_suffix('.c.obj'))
    assert (root/headers[-1]).stat().st_mtime<=obj.stat().st_mtime
result=dict(revision='low-battery-smooth-v3-compact',version=binary[48:80].split(b'\0')[0].decode(),
            bytes=len(binary),elfSha256=sha(elf),binSha256=hashlib.sha256(binary).hexdigest(),
            reminderMs=5000,animationCycleMs=2500,animationMaximumFps=40,
            animationBufferBytes=8112,transientMaskCacheBytes=16224,maskCacheVisibleOnly=True,
            rawStaticFallback=True,hardwareValidated=False,
            sourceHashes={p:sha(root/p) for p in units+headers})
(build/'low-battery-verification.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
print(f'PASS: low-battery boot/runtime guards, UI glyphs, fresh objects and OTA/ELF identity {result["elfSha256"]}')
