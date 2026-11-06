"""Host tests of the actual patched instruction bytes, not a full RNNT model.

The bounded interpreter executes the post-extend append tail. Its memory
checker rejects all accesses to the retired object, even reads. Real Xtensa
disassembly and internal call targets are separately checked after linking.
"""
import importlib.util
import io
from pathlib import Path
import unittest

from elftools.elf.elffile import ELFFile

spec = importlib.util.spec_from_file_location('fix', Path(__file__).with_name('patch-rnnt-growth.py'))
fix = importlib.util.module_from_spec(spec); spec.loader.exec_module(fix)
ROOT = Path(__file__).resolve().parent.parent


def append_code(archive):
    start, size = fix.archive_member(archive, fix.MEMBER)
    return ELFFile(io.BytesIO(archive[start:start + size])).get_section_by_name(fix.SECTION).data()


def run_tail(code, length, token, reuse_old=False):
    old, new, tokens = 0x1000, 0x2000, 0x3000
    regs = [0] * 16
    regs[2], regs[7], regs[10], regs[3], regs[4] = old, old, new, token, 0x3f000000
    memory = {new + i * 4: 100 + i for i in range(10)}
    memory.update({new: tokens, new + 16: length, new + 20: length + 10})
    memory.update({tokens + i * 4: i + 1 for i in range(length)})
    memory.update({old + i * 4: 0xa5a5a5a5 for i in range(10)})
    old_image = {a: v for a, v in memory.items() if old <= a < old + 40}
    def check(address):
        if old <= address < old + 40 and not reuse_old:
            raise ValueError('access to freed path')
    pc = 0x46
    while pc < len(code):
        b0, b1 = code[pc:pc + 2]
        op = b0 & 15
        if (b0, b1) == (0x1d, 0xf0):  # retw.n
            break
        if op in (8, 9):  # l32i.n / s32i.n
            target, base, offset = b0 >> 4, b1 & 15, (b1 >> 4) * 4
            address = regs[base] + offset; check(address)
            if op == 8:
                regs[target] = memory[address]
            else:
                memory[address] = regs[target]
        elif op == 13:  # mov.n
            regs[b0 >> 4] = regs[b1 & 15]
        elif op == 11:  # addi.n (only +1 in this bounded tail)
            assert b0 >> 4 == 1
            regs[b1 >> 4] = regs[b1 & 15] + 1
        elif code[pc:pc + 3] == bytes.fromhex('90 88 a0'):  # addx4 a8,a8,a9
            regs[8] = regs[8] * 4 + regs[9]; pc += 3; continue
        elif (b0, b1) in ((0x0c, 0x18), (0x0c, 0x08)):  # movi.n a8,1/0
            regs[8] = b1 >> 4
        else:
            raise AssertionError(f'unreviewed tail instruction at {pc:x}')
        pc += 2
    assert regs[2] == new
    assert memory[new + 16] == length + 1
    assert memory[new + 20] == length + 10
    assert memory[tokens + length * 4] == token
    assert all(memory[tokens + i * 4] == i + 1 for i in range(length))
    assert memory[new + 12] == 0x3f000000 and memory[new + 24] == 1 and memory[new + 28] == 0
    assert {a: memory[a] for a in old_image} == old_image


class GrowthFixTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = (ROOT / 'managed_components/espressif__esp-sr/lib/esp32s3/libmultinet.a').read_bytes()
        cls.patched, cls.report = fix.patch_bytes(cls.source)

    def test_only_two_operand_bytes_change(self):
        before, after = append_code(self.source), append_code(self.patched)
        self.assertEqual([i for i, (a, b) in enumerate(zip(before, after)) if a != b], [0x47, 0x4f])
        self.assertEqual(len(self.source), len(self.patched))
        self.assertEqual(sum(a != b for a, b in zip(self.source, self.patched)), 2)
        # Normal append/blank branches and all other routines are unchanged.
        self.assertEqual(before[:0x46], after[:0x46])

    def test_original_detects_freed_object_access(self):
        with self.assertRaisesRegex(ValueError, 'freed path'):
            run_tail(append_code(self.source), 10, 3)

    def test_growth_boundaries_and_reused_memory(self):
        for length in (1, 10, 20, 30, 100, 1000):
            for token in (1, 3, 255, 65535):
                for reused in (False, True):
                    with self.subTest(length=length, token=token, reused=reused):
                        run_tail(append_code(self.patched), length, token, reused)

    def test_unknown_or_already_patched_library_rejected(self):
        bad = bytearray(self.source); bad[-1] ^= 1
        for data in (bytes(bad), self.patched, b''):
            with self.assertRaisesRegex(ValueError, 'Unreviewed'):
                fix.patch_bytes(data)


if __name__ == '__main__':
    unittest.main(verbosity=2)
