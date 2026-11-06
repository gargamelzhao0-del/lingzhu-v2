"""Verify startup fixes in the linked ELF and the actual flash image."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('build', type=Path)
p.add_argument('--nm', default='C:/Espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin/xtensa-esp32s3-elf-nm.exe')
a = p.parse_args()
b = a.build.resolve()
elf = b / 'NuwaxAIPartner.elf'
image = (b / 'NuwaxAIPartner.bin').read_bytes()
elf_sha = hashlib.sha256(elf.read_bytes()).hexdigest()
assert image[176:208].hex() == elf_sha, 'BIN/ELF identity mismatch'
symbols = subprocess.check_output([a.nm, '--defined-only', str(elf)], text=True)
names = {line.split()[-1] for line in symbols.splitlines() if line.split()}
for name in ['nuwax_wifi_prepare_boot_config', 'nuwax_i2c_bus_get_stats']:
    assert name in names, f'Startup fix not linked: {name}'
for marker in [b'boot optimization revision=20260922-startup-v1',
               b'OTA rejected before audio reservation: version_not_newer',
               b'boot STA configuration prepared before wake',
               b'I2C contention long_holds=']:
    assert marker in image, f'Missing image marker: {marker!r}'
result = dict(revision='20260922-startup-v1', elfSha256=elf_sha,
              binSha256=hashlib.sha256(image).hexdigest(),
              linkedMarkersVerified=True, hardwareValidated=False)
(b / 'startup-verification.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
print('PASS: startup fixes linked, BIN/ELF identity: ' + elf_sha)
