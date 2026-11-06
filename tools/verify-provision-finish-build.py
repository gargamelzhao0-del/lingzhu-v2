"""Fail closed unless the repaired transport and success hooks are linked."""
from pathlib import Path
import hashlib
import importlib.util
import json
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
build = Path(sys.argv[1]).resolve()
elf = build / 'NuwaxAIPartner.elf'
binary = (build / 'NuwaxAIPartner.bin').read_bytes()
sha = hashlib.sha256(elf.read_bytes()).hexdigest()
assert binary[176:208].hex() == sha, 'BIN/ELF mismatch'
for marker in [b'provision transport revision=20260923-provision-finish-v1',
               b'provision success response read; BLE drain grace complete',
               b'provision success query window complete',
               b'WebSocket cleanup yielding to BLE handoff close_err=%s; wake/reconnect skipped',
               b'ble_reprovision_park']:
    assert marker in binary, f'Repair missing in linked firmware: {marker!r}'
commands = json.loads((build / 'compile_commands.json').read_text())
transports = [c for c in commands if Path(c['file']).name == 'protocomm_nimble.c']
patched = build / 'nuwax_patches/protocomm_nimble.c'
assert len(transports) == 1 and Path(transports[0]['file']).resolve() == patched
spec = importlib.util.spec_from_file_location('patcher', root / 'tools/patch-protocomm-lifecycle.py')
patcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patcher)
report = json.loads(patched.with_suffix('.json').read_text())
original = Path(report['input'])
assert hashlib.sha256(original.read_bytes()).hexdigest() == report['input_sha256'], 'SDK source changed during build'
assert patched.read_text() == patcher.patch(original.read_text()), 'Generated transport is stale'
assert hashlib.sha256(patched.read_bytes()).hexdigest() == report['output_sha256']
tool_dir = Path('C:/Espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin')
for symbol, caller in [
    ('__wrap_network_prov_mgr_done', 'wifi_get_status_handler'),
    ('nuwax_provision_success_query_revision', 'gatt_svr_chr_access'),
    ('nuwax_provision_success_response_read', 'gatt_svr_chr_access')]:
    disassembly = subprocess.check_output([
        str(tool_dir / 'xtensa-esp32s3-elf-objdump.exe'), '-d',
        '--disassemble=' + caller, str(elf)], text=True)
    # Xtensa relaxation uses either call8 or l32r of the resolved address
    # followed by callx8 through the SAME register. A mere symbol is insufficient.
    direct = re.search(r'call\w*\s+[^\n]*<' + symbol + '>', disassembly)
    indirect = re.search(r'l32r\s+(a\d+),[^\n]*<' + symbol +
                         r'>[^\n]*\n[^\n]*callx\w*\s+\1\b', disassembly)
    assert direct or indirect, 'No linked call from ' + caller + ' to ' + symbol
sources = ['main/nuwax_ai_partner_main.c', 'main/nuwax_provision_new/nuwax_provision_main.c',
           'main/nuwax_provision_new/nuwax_provision_policy.h', 'main/CMakeLists.txt',
           'CMakeLists.txt', 'tools/cmake/protocomm-lifecycle-fix.cmake',
           'tools/patch-protocomm-lifecycle.py']
result = dict(revision='20260923-provision-finish-v1', elfSha256=sha,
              binSha256=hashlib.sha256(binary).hexdigest(), bytes=len(binary),
              transport=report,
              sourceHashes={p: hashlib.sha256((root / p).read_bytes()).hexdigest() for p in sources},
              hardwareValidated=False)
(build / 'provision-finish-verification.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
print('PASS: build-local BLE transport and real linked response hooks; BIN/ELF match ' + sha)
