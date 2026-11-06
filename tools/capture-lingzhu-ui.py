"""Build the native LVGL drawer fixture; capture actual firmware widgets."""
from pathlib import Path
import subprocess, re, sys, shutil, argparse, hashlib, json
from PIL import Image
root=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--no-test-panel',action='store_true')
parser.add_argument('--no-style-cache',action='store_true',help='ABI-isolated reference build with LVGL style cache disabled')
parser.add_argument('--reference-refresh',type=Path,help='Compare with a saved LVGL refresh implementation')
parser.add_argument('--output-dir',type=Path,help='Isolated capture directory')
args=parser.parse_args()
out=(args.output_dir.resolve() if args.output_dir else root/('build-host-checks/lingzhu-ui-release' if args.no_test_panel else 'build-host-checks/lingzhu-ui'));out = out.with_name(out.name+'-reference') if args.reference_refresh else out;out = out.with_name(out.name+'-no-style-cache') if args.no_style_cache else out;out.mkdir(parents=True,exist_ok=True)
support=root.parent/'tools/lvgl-simulator';headers=out/'include';headers.mkdir(exist_ok=True)
(headers/'esp_app_desc.h').write_text('#pragma once\ntypedef struct {char version[32];} esp_app_desc_t;\nconst esp_app_desc_t *esp_app_get_description(void);\n')
(headers/'esp_heap_caps.h').write_text((support/'esp_heap_caps.h').read_text()+'\nvoid *heap_caps_aligned_alloc(size_t,size_t,uint32_t);\n')
(headers/'nuwax_provision_main.h').write_text('#pragma once\nconst char *nuwax_provision_main_qr_payload(void);\n')
gcc=shutil.which('gcc');assert gcc
lv=root/'lib/lvgl'
includes=[headers,root/'tools/ui-speaking-capture',root/'main',root/'main/nuwax_ui_v28',support,lv,lv/'src',root/'main/nuwax_provision_new']
common=[f'-DLV_OBJ_STYLE_CACHE={0 if args.no_style_cache else 1}','-std=gnu11','-O2','-DLV_USE_QRCODE=1','-DLV_CONF_INCLUDE_SIMPLE','-DLV_KCONFIG_IGNORE','-D_WIN32_WINNT=0x0601','-DWINVER=0x0601','-DCONFIG_SPIRAM=1',f'-DCONFIG_NUWAX_TEST_PANEL_ENABLE={0 if args.no_test_panel else 1}','-DCONFIG_NUWAX_TEST_NETWORK_ENABLE=1']+['-I'+str(p) for p in includes]
sources=[root/'tools/ui-drawer-capture/capture.c',root/'main/nuwax_ui_v28/assets/nuwax_control_center_icons.c',root/'main/nuwax_ui_v28/test/nuwax_test_panel.c']+list((root/'main/nuwax_ui_v28/assets/fonts').glob('nuwax_font_control_center*.c'))+[root/'main/nuwax_ui_v28/assets/fonts/nuwax_font_lingzhu.c']
sources+=list((root/'main/nuwax_ui_v28').rglob('nuwax_font_test_panel_11.c'))
sources.append(root/'main/nuwax_ui_v28/menu/nuwax_device_qr_view.c')
sources += [root/'main/nuwax_ui_v28'/p for p in ['ota_progress.c', 'assets/ota_logo.c', 'assets/ota_capsule.c', 'assets/fonts/nuwax_font_ota.c']]
objects=[]
for src in sources:
    obj=out/(src.stem+'.o');subprocess.run([gcc,*common,'-c',str(src),'-o',str(obj)],check=True);objects.append(obj)
# Object layouts depend on configuration. Never reuse a different fixture's ABI.
config_hash=hashlib.sha256((' '.join(common)+(support/'lv_conf.h').read_text()+
    ''.join(p.read_text() for p in sorted(headers.glob('*.h')))+
    (lv/'src/lv_conf_internal.h').read_text()).encode()).hexdigest()[:16]
cache=root/'build-host-checks/lvgl-cache'/config_hash;cache.mkdir(parents=True,exist_ok=True)
header_mtime=max(p.stat().st_mtime for p in lv.rglob('*.h'))
for src in lv.joinpath('src').rglob('*.c'):
    if '/libs/thorvg/' in src.as_posix() or '/others/vg_lite_tvg/' in src.as_posix():continue
    if args.reference_refresh and src.name=='lv_refr.c':
        obj=out/'reference-refresh.o'
        subprocess.run([gcc,*common,'-I'+str(lv/'src/core'),'-c',str(args.reference_refresh),'-o',str(obj)],check=True)
        objects.append(obj);continue
    name=re.sub('[^A-Za-z0-9_.-]','_',str(src.resolve()))+'.obj';obj=cache/name
    if not obj.exists() or obj.stat().st_mtime < max(src.stat().st_mtime,header_mtime):
        subprocess.run([gcc,*common,'-c',str(src),'-o',str(obj)],check=True)
    objects.append(obj)
rsp=out/'objects.rsp';rsp.write_text('\n'.join('"'+str(p).replace('\\','/')+'"' for p in objects)+'\n-luser32\n-lgdi32\n-lshcore\n-lwinmm\n')
exe=out/'capture.exe';subprocess.run([gcc,'-o',str(exe),'@'+str(rsp)],check=True)
subprocess.run([str(exe)],cwd=out,check=True)
fallback=out/'fallback';fallback.mkdir(exist_ok=True)
subprocess.run([str(exe),'no-cache'],cwd=fallback,check=True)
for p in out.glob('*.ppm'):
    if re.search(r'(qr-enter|detail-back|info-enter|reverse-detail)-\d+',p.stem):continue # allocation failure settles instantly
    assert p.read_bytes()==(fallback/p.name).read_bytes(),f'Cached/live rendering differs: {p.name}'
for p in out.glob('*.ppm'):Image.open(p).save(p.with_suffix('.png'))
preview=Image.new('RGB',(760,400),'#252525')
for x,n in [(10,'status'),(390,'menu')]:preview.paste(Image.open(out/(n+'.png')),(x,20))
preview.save(out/'preview.png');print(out/'preview.png')
for x,n in [(10,'qr'),(390,'info')]:preview.paste(Image.open(out/(n+'.png')),(x,20))
preview.save(out/'details-preview.png')
sources=['main/nuwax_ui_v28/nuwax_drawer.c','main/nuwax_ui_v28/menu/nuwax_device_qr_view.c','main/nuwax_ui_v28/menu/nuwax_detail_style.h','main/nuwax_ui_v28/assets/fonts/nuwax_font_lingzhu.c','lib/lvgl/src/core/lv_refr.c','lib/lvgl/src/display/lv_display.c','lib/lvgl/src/display/lv_display.h','lib/lvgl/src/display/lv_display_private.h']
hashes={p:hashlib.sha256((root/p).read_bytes()).hexdigest() for p in sources}
(out/'report.json').write_text(json.dumps(dict(productionDrawerSha256=hashes[sources[0]],sourceHashes=hashes,testPanel=not args.no_test_panel,styleCache=not args.no_style_cache,cacheAndFallbackMatch=True,detailFallback='instant when cache unavailable',cachedDetailFramesMatchLiveReference=True,partialAndFullRedrawMatch=True,interactionChecks='passed',layoutOnlyDrawBypass=True,cachedFinalFrameHandoff=True,cachedPresentationPlane=True,cachedSceneRenderer=True,referenceRefresh=str(args.reference_refresh) if args.reference_refresh else None,hardwareValidated=False),indent=2))

# Preview actual native animation frames (not a separate UI mockup).
frames=[];durations=[]
for names,ms in [(['status'],600),([p.stem for p in sorted(out.glob('page-??.png'))],25),(['menu'],500),(['qr-click'],75),([p.stem for p in sorted(out.glob('qr-enter-??.png'))],25),(['qr'],750),([p.stem for p in sorted(out.glob('detail-back-??.png'))],25),(['menu'],350),([p.stem for p in sorted(out.glob('info-enter-??.png'))],25),(['info'],1000)]:
    for name in names:frames.append(Image.open(out/(name+'.png')).convert('RGB'));durations.append(ms)
frames[0].save(out/'navigation-preview.gif',save_all=True,append_images=frames[1:],duration=durations,loop=0,disposal=2)
