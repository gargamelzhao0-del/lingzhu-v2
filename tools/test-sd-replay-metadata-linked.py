"""Check final caller bindings and preserve the measured 141 payload pipeline."""
from pathlib import Path
import argparse, hashlib, json, re, subprocess
p=argparse.ArgumentParser(); p.add_argument('--elf',type=Path,required=True)
p.add_argument('--baseline',type=Path,required=True); args=p.parse_args()
root=Path(__file__).resolve().parents[1]
tool=Path('C:/Espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin')
names=subprocess.check_output([str(tool/'xtensa-esp32s3-elf-nm.exe'),'-S',str(args.elf)],text=True)
callers=[row.split()[-1] for row in names.splitlines() if row.split() and row.split()[-1].startswith('replay_one_ready')]
assert callers
assembly='\n'.join(subprocess.check_output([str(tool/'xtensa-esp32s3-elf-objdump.exe'),'-d','--disassemble='+name,str(args.elf)],text=True) for name in callers)
for target in ['sd_audio_catalog_bind','sd_audio_catalog_ack','sd_audio_catalog_note_deleted']:
    assert '<'+target+'>' in assembly,target
binary=args.elf.with_suffix('.bin').read_bytes()
assert binary[48:80].split(b'\0')[0]==b'7.0.142'
assert b'catalog_reused=%u' in binary and b'confirm_ms=%lu persist_ms=%lu' in binary
source=(root/'main/offline_audio_spool.c').read_text(encoding='utf-8')
baseline=(args.baseline/'main/offline_audio_spool.c').read_text(encoding='utf-8')
def payload(text):
    return text[text.index('static esp_err_t replay_send_windowed('):text.index('#include "offline_replay_metadata.h"')]
assert payload(source)==payload(baseline), 'Measured sender pipeline changed'
prior=json.loads((root/'artifacts/firmware/7.0.141-sd-gateway-test-20261015/build-result.json').read_text(encoding='utf-8'))
unchanged=['main/offline_replay_prefetch.inc','main/offline_io_schedule.h','main/replay_ui_schedule.c',
    'main/replay_ui_schedule.h','main/nuwax_lvgl_port.c','main/ws_transport.c','main/ws_mask.h']
for name in unchanged:
    assert hashlib.sha256((root/name).read_bytes()).hexdigest()==prior['sourceHashes'][name],name
snapshot_bytes=sum(int(row.split()[1],16) for row in names.splitlines() if row.split() and
    row.split()[-1] in ['selected_session','selected_segment','selected_valid','acked_id','acked_index'])
assert 300<snapshot_bytes<=512
defs=lambda path:dict(re.findall(r'^#define (CONFIG_\w+) (.*)$',path.read_text(encoding='utf-8'),re.M))
assert defs(args.elf.parent/'config/sdkconfig.h')==defs(root/'artifacts/firmware/7.0.141-sd-gateway-test-20261015/config/sdkconfig.h')
report={'elfSha256':hashlib.sha256(args.elf.read_bytes()).hexdigest(),'version':'7.0.142',
    'hardwareValidated':False,'snapshotBytes':snapshot_bytes,'unchanged':unchanged,
    'payloadBodyUnchanged':True,'effectiveConfigUnchanged':True,'callers':callers}
dest=root/'build-host-checks/sd-catalog-replay/linked.json'
dest.write_text(json.dumps(report,indent=2),encoding='utf-8')
print('PASS: final 142 ELF reaches bind/ACK/delete hint; one-shot snapshot '+str(snapshot_bytes)+' bytes; 141 payload, display/scheduler, transport and effective config unchanged')
