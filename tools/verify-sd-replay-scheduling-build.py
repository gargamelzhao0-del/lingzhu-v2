"""Verify the bounded replay selector/cache are in this exact firmware image."""
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
elf_sha = hashlib.sha256(elf.read_bytes()).hexdigest()
assert binary[176:208].hex() == elf_sha, 'BIN/ELF mismatch'
sources = ['main/sd_audio_catalog.c', 'main/sd_audio_scene_replay.inc',
           'main/sd_audio_session_replay.inc', 'main/offline_audio_spool.c']
commands = json.loads((build / 'compile_commands.json').read_text())
for name in [sources[0], sources[3]]:
    path = root / name
    assert len([c for c in commands if Path(c['file']).resolve() == path.resolve()]) == 1
    obj = build / 'esp-idf/main/CMakeFiles/__idf_main.dir' / (path.name + '.obj')
    dependencies = [path] if name == sources[0] else [path, root / sources[1], root / sources[2]]
    assert all(p.stat().st_mtime <= obj.stat().st_mtime <= elf.stat().st_mtime for p in dependencies)
tool_dir = Path('C:/Espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin')
symbols = subprocess.check_output([str(tool_dir / 'xtensa-esp32s3-elf-nm.exe'),
                                   '-S', '--defined-only', str(elf)], text=True)
sizes = {}
for symbol in ['replay_scans', 'replay_scan_generation', 's_scene_clock']:
    match = re.search(r'^\w+\s+(\w+)\s+\w\s+' + symbol + r'$', symbols, re.M)
    assert match, f'missing scheduling state: {symbol}'
    sizes[symbol] = int(match[1], 16)
assert sizes['replay_scans'] <= 1024 and sizes['s_scene_clock'] == 8
asm = subprocess.check_output([str(tool_dir / 'xtensa-esp32s3-elf-objdump.exe'), '-d', str(elf)], text=True)
frames = {}
prefixes = ('sd_audio_catalog_next', 'catalog_next_impl', 'find_replay_session',
            'card_scene_prepare', 'card_replay_next', 'formal_replay_task')
for match in re.finditer(r'^\w+ <([^>]+)>:\s*\n([^\n]+)', asm, re.M):
    name, first_instruction = match.groups()
    if not name.startswith(prefixes):
        continue
    frame = re.search(r'\bentry\s+a1,\s*(0x[0-9a-f]+|\d+)', first_instruction)
    if frame:
        frames[name] = int(frame[1], 0)
        assert frames[name] <= 2048, (name, frames[name])
assert 'sd_audio_catalog_next' in frames and any(n.startswith('card_scene_prepare') for n in frames)
result = dict(elfSha256=elf_sha, binSha256=hashlib.sha256(binary).hexdigest(),
              freshCompiledObjects=True, boundedSchedulingStateLinked=sizes,
              stackFrames=frames, protocolChanged=False, hardwareValidated=False,
              sourceHashes={p: hashlib.sha256((root / p).read_bytes()).hexdigest() for p in sources})
(build / 'sd-replay-scheduling-verification.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
print(f'PASS: bounded replay scheduling/cache linked; state={sizes}; static frames={frames}; BIN/ELF match')
