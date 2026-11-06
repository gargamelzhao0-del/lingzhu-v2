"""Decode local SD integrity evidence. Read-only; never opens a device or uploads audio."""
import argparse
import json
from pathlib import Path
import struct
import zlib


def decode(path):
    data = Path(path).read_bytes()
    if len(data) < 512 or data[:8] != b'NWXDIA2\0':
        raise ValueError('invalid evidence signature or short header')
    u32 = lambda at: struct.unpack_from('<I', data, at)[0]
    u64 = lambda at: struct.unpack_from('<Q', data, at)[0]
    if u32(8) != 1 or u32(12) != 512 or zlib.crc32(data[:508]) != u32(508):
        raise ValueError('unsupported format or damaged header CRC')
    n, m = u32(24), u32(28)
    if n > 66696 or m > 512 or len(data) != 512 + n + m:
        raise ValueError('invalid evidence lengths')
    shadow, observed = data[512:512+n], data[512+n:]
    if zlib.crc32(shadow) != u32(388) or zlib.crc32(observed) != u32(392):
        raise ValueError('damaged evidence payload CRC')
    text = lambda at, n: data[at:at+n].split(b'\0')[0].decode('utf-8', errors='replace')
    fields = dict(session=text(128, 48), file=text(176, 128), physical_index=u32(76),
        stage={1:'queue',2:'during_write',3:'record_readback',4:'seal_readback',5:'seal_size'}.get(u32(64), 'unknown'),
        tick_ms=u32(80), file_offset=u64(16), shadow_bytes=n, observed_block_bytes=m,
        observed_block_file_offset=u64(16)+u32(104) if m else None,
        full_batch_shadow=bool(u32(68)&1), queued_crc_available=bool(u32(68)&2),
        differing_bytes=u32(60), first_difference=None if u32(56)==0xffffffff else u64(16)+u32(56),
        first_sequence=u32(348), last_sequence=u32(352), record_count=u32(356),
        expected_file_bytes=u64(88)+32 if u32(64) in (4,5) else None,
        observed_file_bytes=u64(96) if u32(64) in (4,5) else None,
        queue_crc=f'{u32(44):08x}', before_write_crc=f'{u32(48):08x}', after_write_crc=f'{u32(52):08x}',
        expected_crc=f'{u32(32):08x}', observed_crc=f'{u32(36):08x}',
        incremental_shadow_crc=f'{u32(40):08x}', snapshot_crc=f'{u32(84):08x}',
        writes=u32(364), reads=u32(368), sdk_errors=u32(344), foreign_writes=u32(340),
        covered_sectors=u32(328), unknown_sectors=u32(332), evictions=u32(336),
        matched_sectors=u32(372), mismatched_sectors=u32(376), source_mutations=u32(360),
        bad_lba=u64(304), sector_write_crc=f'{u32(312):08x}', sector_read_crc=f'{u32(316):08x}',
        raw_attempts=u32(380), raw_valid=bool(u32(384)), raw_crc=f'{u32(320):08x}', raw_error=u32(324))
    # These observations locate a boundary; none independently proves card failure.
    hints = []
    if fields['queued_crc_available'] and u32(44) != u32(48):
        hints.append('queue source changed before fwrite')
    if u32(48) != u32(52): hints.append('source changed during fwrite')
    if u32(40) != u32(84): hints.append('retained shadow changed after its incremental CRC')
    if u32(388) != u32(84): hints.append('retained shadow changed between freeze and evidence export')
    if fields['mismatched_sectors'] and fields['raw_valid']:
        hints.append('fresh internal-DMA reread matches latest write witness' if u32(312)==u32(320)
                     else 'fresh internal-DMA reread also differs from latest write witness')
    if fields['unknown_sectors'] or fields['evictions']:
        hints.append('sector witness coverage is partial; missing witnesses cannot establish correctness')
    fields['observations'] = hints
    return fields


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('file', type=Path)
    args=parser.parse_args()
    try: print(json.dumps(decode(args.file),ensure_ascii=False,indent=2))
    except (OSError, ValueError) as error: parser.exit(2, str(error)+'\n')
