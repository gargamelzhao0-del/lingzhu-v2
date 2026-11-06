"""Verify the actual Xtensa optimizer with IDF's no-builtin-memcpy flag.

No target execution or throughput claim. The negative control reconstructs
ordinary memcpy calls so this check must first reproduce the old overhead.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--toolchain', type=Path, default=Path(
    'C:/Espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin'))
parser.add_argument('--output', type=Path, default=Path('build-host-checks/ws-mask-codegen'))
parser.add_argument('--elf', type=Path, help='Also verify mask instructions in the linked sender using DWARF line attribution')
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent
header = (root/'main/ws_mask.h').read_text(encoding='utf-8')
args.output.mkdir(parents=True, exist_ok=True)
probe = '''#include "ws_mask.h"
void mask_probe(uint8_t *d,const uint8_t *s,size_t n,const uint8_t *k,size_t phase) {
    nuwax_ws_mask_copy(d,s,n,k,phase);
}
void parts_probe(uint8_t *d,const uint8_t *h,size_t hn,const uint8_t *s,size_t off,size_t n,const uint8_t *k) {
    nuwax_ws_mask_copy_parts(d,h,hn,s,off,n,k);
}
'''
report = {'headerSha256': hashlib.sha256((root/'main/ws_mask.h').read_bytes()).hexdigest(),
          'target': 'esp32s3', 'hardwareBenchmark': False}

def reachable_mask_instructions(sym, label):
    """Xtensa has 2/3-byte instructions and padding after unconditional jumps.

    Linear objdump can decode two padding bytes plus the next block's first
    byte as an instruction, then invent calls at wrong instruction boundaries.
    Follow branch destinations and decode a missing boundary from its exact PC.
    Never whitelist a call or drop a reachable instruction to pass this check.
    """
    start, end = sym['st_value'], sym['st_value'] + sym['st_size']
    decoded, visited, pending, output, masks = {}, set(), [start], [], []
    repair_starts = []

    def decode_from(pc):
        asm = subprocess.check_output([str(args.toolchain/'xtensa-esp32s3-elf-objdump.exe'), '-dl',
            f'--start-address={pc}', f'--stop-address={end}', str(args.elf)], text=True)
        output.append(asm)
        in_mask = False
        for line in asm.splitlines():
            if re.search(r'\.[ch]:\d+', line):
                in_mask = 'ws_mask.h:' in line
            m = re.match(r'\s*([0-9a-f]+):\s+([0-9a-f]+)\s+(\S+)(.*)', line)
            if m:
                address = int(m[1], 16)
                instruction = (len(m[2]) // 2, m[3], m[4], in_mask)
                if address in visited:
                    assert decoded[address][:3] == instruction[:3], 'Conflicting reachable instruction decoding'
                decoded[address] = instruction

    decode_from(start)
    while pending:
        pc = pending.pop()
        while pc not in visited:
            assert start <= pc < end, f'Unexpected fallthrough outside {sym.name}'
            if pc not in decoded:
                repair_starts.append(hex(pc))
                decode_from(pc)
            assert pc in decoded, f'Missing instruction at {pc:x}'
            size, op, operands, in_mask = decoded[pc]
            assert size in (2, 3) and op not in ('ill', '.byte'), f'Invalid reachable instruction {pc:x}: {op}'
            visited.add(pc)
            if in_mask:
                masks.append(op)
            if op.startswith('ret'):
                break
            assert op != 'jx', f'Unresolved indirect jump in {sym.name}'
            if op == 'j' or op.startswith('b') or op.startswith('loop'):
                target = re.search(r'\b([0-9a-f]+)\s+<', operands)
                assert target, f'Unresolved branch {pc:x}: {op} {operands}'
                target = int(target[1], 16)
                assert start <= target < end, f'Branch leaves checked sender {sym.name}'
                pending.append(target)
                if op == 'j':
                    break
            pc += size
    (args.output/f'linked-{label}-{sym.name}.txt').write_text('\n'.join(output), encoding='utf-8')
    return masks, {'reachableInstructions': len(visited), 'redecodedBranchTargets': repair_starts}

for name, content in [('ordinary', header.replace('__builtin_memcpy(', 'memcpy(')), ('builtin', header)]:
    out = args.output/name
    out.mkdir(exist_ok=True)
    (out/'ws_mask.h').write_text(content, encoding='utf-8')
    (out/'probe.c').write_text(probe, encoding='utf-8')
    subprocess.run([str(args.toolchain/'xtensa-esp32s3-elf-gcc.exe'), '-O2', '-mlongcalls',
                    '-fstrict-aliasing', '-fno-builtin-memcpy', '-Wall', '-Wextra', '-Werror',
                    '-c', str(out/'probe.c'), '-o', str(out/'probe.obj')], check=True)
    asm = subprocess.check_output([str(args.toolchain/'xtensa-esp32s3-elf-objdump.exe'),
                                   '-dr', str(out/'probe.obj')], text=True)
    (out/'disassembly.txt').write_text(asm, encoding='utf-8')
    refs = len(re.findall(r'R_XTENSA_\w+\s+memcpy\b', asm))
    if name == 'ordinary':
        assert refs > 0, 'Negative control did not reproduce ROM memcpy dependencies'
    else:
        assert refs == 0 and not re.search(r'\bmemcpy\b', asm), 'ROM memcpy remains in mask probes'
        assert re.search(r'\bl32i(?:\.n)?\s', asm) and re.search(r'\bs32i(?:\.n)?\s', asm)
        assert re.search(r'\bxor\s', asm), 'Expected word XOR code missing'
    report[name] = {'memcpyRelocations': refs, 'objectBytes': (out/'probe.obj').stat().st_size}
if args.elf:
    from elftools.elf.elffile import ELFFile
    with args.elf.open('rb') as f:
        elf = ELFFile(f)
        symbols = [s for s in elf.get_section_by_name('.symtab').iter_symbols() if s['st_size']]
    paths = {}
    for label, prefix in [('single', 'send_frame_unlocked_buffered'), ('batch', 'send_archive_batch_unlocked')]:
        funcs = [s for s in symbols if s.name.startswith(prefix)]
        if label == 'batch' and not funcs:
            # The compiler may inline the batch encoder into its only caller.
            funcs = [s for s in symbols if s.name.startswith('send_frame_with_timeout_buffered')]
        assert funcs, f'Linked {label} sender symbol missing'
        instructions = []
        control_flow = []
        for sym in funcs:
            mask_ops, cfg = reachable_mask_instructions(sym, label)
            instructions.extend(mask_ops)
            control_flow.append(cfg)
        assert 'xor' in instructions, f'No linked {label} mask XOR found'
        assert any(x.startswith('l32i') for x in instructions) and any(x.startswith('s32i') for x in instructions)
        calls = [x for x in instructions if x.startswith('call')]
        assert not calls, f'Function calls remain in linked {label} mask: {calls}'
        paths[label] = {'symbols': [s.name for s in funcs], 'maskInstructions': len(instructions),
                        'maskCalls': len(calls), 'controlFlow': control_flow}
    report['linked'] = {'elfSha256': hashlib.sha256(args.elf.read_bytes()).hexdigest(),
                        'maskInstructions': sum(p['maskInstructions'] for p in paths.values()),
                        'maskCalls': sum(p['maskCalls'] for p in paths.values()), 'paths': paths}
    print('PASS: final ELF single and batch mask ranges contain word loads/XOR/stores and zero function calls')
(args.output/'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
print('PASS: Xtensa -O2 -fstrict-aliasing -fno-builtin-memcpy; ordinary-copy negative control reproduced; word XOR uses loads/stores without memcpy calls')
