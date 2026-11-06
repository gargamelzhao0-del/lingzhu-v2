"""Pinned ESP-SR 2.4.7 / ESP32-S3 RNNT growth fix. Never edit the vendor file.

Only two register operands change. The archive layout, symbols, relocations,
normal append path, allocation policy and other decoder routines stay intact.
Review rationale: docs/2026-09-16-MultiNet非法释放重启分析.md.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import subprocess

from elftools.elf.elffile import ELFFile

SOURCE_SHA256 = "8f7b29c8143c1ba1dedba70242877ec72150ba3a89e2cca3dcd86ca3b417fee9"
MEMBER = "rnnt_decoder.c.obj"
SECTION = ".text.rnnt_path_append"
# l32i.n a8,a2,16 -> a8,a10,16 (new return value);
# s32i.n a10,a7,16 -> a10,a2,16 (a2 has just become the new path).
BEFORE = bytes.fromhex("88 42 98 0a 2d 0a 1b a8 a9 47")
AFTER = bytes.fromhex("88 4a 98 0a 2d 0a 1b a8 a9 42")


def archive_member(data, wanted):
    if data[:8] != b"!<arch>\n":
        raise ValueError("Not a GNU archive")
    pos, names, found = 8, b"", []
    while pos < len(data):
        hdr = data[pos:pos + 60]
        if len(hdr) != 60 or hdr[58:] != b"`\n":
            raise ValueError("Malformed archive header")
        size = int(hdr[48:58]); start = pos + 60
        if start + size > len(data):
            raise ValueError("Truncated archive member")
        name = hdr[:16].decode("ascii").strip()
        if name == "//":
            names = data[start:start + size]
        elif name.startswith("/") and name[1:].isdigit():
            off = int(name[1:]); end = names.find(b"/\n", off)
            if end < 0:
                raise ValueError("Bad long member name")
            name = names[off:end].decode("ascii")
        else:
            name = name.rstrip("/")
        if name == wanted:
            found.append((start, size))
        pos = start + size + size % 2
    if len(found) != 1:
        raise ValueError(f"Expected one {wanted}, got {len(found)}")
    return found[0]


def patch_bytes(data):
    digest = hashlib.sha256(data).hexdigest()
    if digest != SOURCE_SHA256:
        raise ValueError(f"Unreviewed ESP-SR archive: {digest}; refusing patch")
    start, size = archive_member(data, MEMBER)
    elf = ELFFile(io.BytesIO(data[start:start + size]))
    if elf.elfclass != 32 or not elf.little_endian or elf['e_machine'] != 'EM_XTENSA':
        raise ValueError("Expected Xtensa ELF32 little endian")
    section = elf.get_section_by_name(SECTION)
    if section is None or section.data()[0x46:0x50] != BEFORE:
        raise ValueError("RNNT growth instruction signature changed")
    at = start + section['sh_offset'] + 0x46
    patched = bytearray(data)
    patched[at:at + len(BEFORE)] = AFTER
    changes = [i for i, (a, b) in enumerate(zip(data, patched)) if a != b]
    if changes != [at + 1, at + 9]:
        raise ValueError("Unexpected patch extent")
    return bytes(patched), {'sourceSha256': digest,
        'patchedSha256': hashlib.sha256(patched).hexdigest(),
        'member': MEMBER, 'section': SECTION, 'archiveByteOffsets': changes,
        'changedBytes': 2, 'revision': 'rnnt-growth-new-object-v1'}


def verify_elf(path, objdump):
    data = Path(path).read_bytes()
    elf = ELFFile(io.BytesIO(data))
    symbols = elf.get_section_by_name('.symtab')
    def symbol(name):
        found = symbols.get_symbol_by_name(name) or []
        if len(found) != 1 or not found[0]['st_size']:
            raise ValueError(f"Missing/ambiguous symbol {name}")
        return found[0]
    def disassemble(start, end):
        return subprocess.check_output([str(objdump), '-d',
            f'--start-address={start:#x}', f'--stop-address={end:#x}', str(path)], text=True)
    append = symbol('rnnt_path_append')
    address = append['st_value']
    # Start at the actual growth branch; linear decoding can consume its
    # alignment padding as an instruction on Xtensa.
    growth = disassemble(address + 0x3c, address + append['st_size'])
    load = re.search(r'l32i(?:\.n)?\s+a8,\s*a10,\s*16', growth)
    move = re.search(r'mov(?:\.n)?\s+a2,\s*a10', growth)
    store = re.search(r's32i(?:\.n)?\s+a10,\s*a2,\s*16', growth)
    if not (load and move and store and load.start() < move.start() < store.start()):
        raise ValueError("Linked RNNT growth does not use the new object")
    if re.search(r'l32i(?:\.n)?\s+a8,\s*a2,\s*16|s32i(?:\.n)?\s+a10,\s*a7,\s*16', growth):
        raise ValueError("Old-object growth access survives in ELF")
    search = symbol('rnnt_beam_search_with_fst')
    callers = disassemble(search['st_value'], search['st_value'] + search['st_size'])
    count = len(re.findall(r'call8\s+' + f'{address:x}' + r'\s+<rnnt_path_append>', callers))
    if count != 2:
        raise ValueError(f"Expected both RNNT append call sites, got {count}")
    return {'revision': 'rnnt-growth-new-object-v1', 'elfSha256': hashlib.sha256(data).hexdigest(),
            'appendAddress': hex(address), 'verifiedInternalCallSites': count,
            'newObjectAccessVerified': True, 'disassembly': growth}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=['patch', 'verify'])
    parser.add_argument('--input', required=True)
    parser.add_argument('--output')
    parser.add_argument('--objdump')
    parser.add_argument('--report', required=True)
    args = parser.parse_args()
    if args.mode == 'patch':
        if not args.output or Path(args.input).resolve() == Path(args.output).resolve():
            raise ValueError("A distinct build output path is required")
        data, report = patch_bytes(Path(args.input).read_bytes())
        output = Path(args.output); output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(data)
    else:
        if not args.objdump:
            raise ValueError("objdump required for linked verification")
        report = verify_elf(args.input, args.objdump)
    Path(args.report).write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(f"RNNT growth {args.mode}: PASS ({report['revision']})")


if __name__ == '__main__':
    main()
