"""Verify this repair is present in the actual linked firmware."""
from pathlib import Path
import hashlib
import json
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
build = Path(sys.argv[1]).resolve()
elf = build / 'NuwaxAIPartner.elf'
binary = (build / 'NuwaxAIPartner.bin').read_bytes()
sha = hashlib.sha256(elf.read_bytes()).hexdigest()
assert binary[176:208].hex() == sha, 'BIN/ELF mismatch'
for marker in [b'network recovery revision=20260922-network-recovery-v1',
               b'Wi-Fi STA stop/start completed with event fences',
               b'Wi-Fi local disconnect completed; retry budget unchanged',
               b'Failed to disconnect wifi before reset', b'restart PG2 pad hold failed']:
    assert marker in binary, f'Repair not linked: {marker!r}'
nm = 'C:/Espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin/xtensa-esp32s3-elf-nm.exe'
symbols = subprocess.check_output([nm, '--defined-only', str(elf)], text=True)
assert 'preserve_power_on_restart' in symbols
header = (build / 'config/sdkconfig.h').read_text()
for config in ['CONFIG_NUWAX_PROVISION_RUNTIME_REBOOT_ENABLE 1',
               'CONFIG_NUWAX_POWER_CTRL_GPIO 7', 'CONFIG_BOOTLOADER_NUWAX_EARLY_POWER_HOLD 1']:
    assert '#define ' + config in header, config
sources = ['main/wifi_manager.c', 'main/app_shell.c', 'main/nuwax_ui_v28/nuwax_face.c',
           'main/board/nuwax_power_ctrl.c', 'main/nuwax_provision_new/nuwax_provision_main.c',
           'managed_components/espressif__network_provisioning/src/manager.c']
result = dict(revision='20260922-network-recovery-v1', elfSha256=sha,
              binSha256=hashlib.sha256(binary).hexdigest(), bytes=len(binary),
              sourceHashes={p: hashlib.sha256((root / p).read_bytes()).hexdigest() for p in sources},
              hardwareValidated=False)
(build / 'network-recovery-verification.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
print('PASS: network recovery and restart PG2 hold linked; BIN/ELF match ' + sha)
