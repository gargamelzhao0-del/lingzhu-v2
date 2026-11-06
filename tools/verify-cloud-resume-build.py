"""Check cloud-resume build identity, compile input freshness and linked claims."""
from pathlib import Path
import hashlib
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
assert b'cloud resume revision=20260923-cloud-resume-v1 local_wake=epoch_independent settle_delay_ms=0' in binary
main = root / 'main/nuwax_ai_partner_main.c'
commands = json.loads((build / 'compile_commands.json').read_text())
inputs = [c for c in commands if Path(c['file']).resolve() == main.resolve()]
assert len(inputs) == 1, 'Main compile input is ambiguous'
obj = build / 'esp-idf/main/CMakeFiles/__idf_main.dir/nuwax_ai_partner_main.c.obj'
assert main.stat().st_mtime <= obj.stat().st_mtime <= elf.stat().st_mtime, 'Stale main object or ELF'
objdump = Path('C:/Espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin/xtensa-esp32s3-elf-objdump.exe')
def disassemble(symbol):
    return subprocess.check_output([str(objdump), '-d', '--disassemble=' + symbol, str(elf)], text=True)

def require_call(assembly, symbol):
    direct = re.search(r'call\w*\s+[^\n]*<' + symbol + '>', assembly)
    indirect = re.search(r'l32r\s+(a\d+),[^\n]*<' + symbol + r'>[^\n]*\n[^\n]*callx\w*\s+\1\b', assembly)
    assert direct or indirect, 'Missing linked call: ' + symbol

helper = 'meeting_or_monitoring_mode_blocks_sleep'
require_call(disassemble('wake_word_supervisor_task'), helper)
assembly = disassemble(helper)
# Xtensa alignment padding after a jump can desynchronize linear objdump.
# Also decode the actual local branch destinations, bounded by this function.
end = max(int(a, 16) for a in re.findall(r'^([0-9a-f]+):', assembly, re.M)) + 3
targets = re.findall(r'\b([0-9a-f]+) <' + helper + r'\+0x[0-9a-f]+>', assembly)
for target in set(targets):
    assembly += subprocess.check_output([str(objdump), '-d', '--start-address=0x' + target,
                                        '--stop-address=' + str(end), str(elf)], text=True)
for symbol in ['nuwax_meeting_mode_has_audio_claim', 'nuwax_monitoring_mode_has_audio_claim']:
    require_call(assembly, symbol)
sources = ['main/nuwax_ai_partner_main.c', 'main/audio_modes/meeting/meeting_mode.c',
           'main/audio_modes/monitoring/monitoring_mode.c', 'tools/test-cloud-resume.py',
           'main/wake_supervisor_host_check.c', 'main/cloud_ready_sleep_host_check.c']
result = dict(revision='20260923-cloud-resume-v1', elfSha256=sha,
              binSha256=hashlib.sha256(binary).hexdigest(), bytes=len(binary),
              sourceHashes={p: hashlib.sha256((root / p).read_bytes()).hexdigest() for p in sources},
              hardwareValidated=False)
(build / 'cloud-resume-verification.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
print('PASS: cloud resume marker, fresh main object, linked audio claims and BIN/ELF identity ' + sha)
