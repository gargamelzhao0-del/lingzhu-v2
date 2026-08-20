#!/usr/bin/env python3
"""Validate NWEMO1/NWEMO2/NWEMO3 packs and optionally compare pixels."""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
import zlib
from pathlib import Path

WIDTH = 360
HEIGHT = 360
FRAME_PIXELS = WIDTH * HEIGHT
PARTITION_SIZE = 0x400000
INVALID_SEGMENT = 0xFFFF
FRAME_FLAG_ABSOLUTE = 1
FRAME_FLAG_RLE = 2
MAX_FRAME_RUNS = 2048
V3_SEGMENT_MAGIC = b"SGV3"
V3_SEGMENT_FLAG_KEY_RLE = 1 << 0
V3_SEGMENT_FLAG_TAIL_ZLIB = 1 << 1
V3_SEGMENT_FLAG_TRANSITION = 1 << 2
V3_SEGMENT_FLAG_WRAP = 1 << 3
V3_SEGMENT_SUPPORTED_FLAGS = (
    V3_SEGMENT_FLAG_KEY_RLE
    | V3_SEGMENT_FLAG_TAIL_ZLIB
    | V3_SEGMENT_FLAG_TRANSITION
    | V3_SEGMENT_FLAG_WRAP
)
V3_MAX_BOOTSTRAP_BYTES = 32768
V4_SEGMENT_MAGIC = b"SGV4"
V4_SEGMENT_FLAG_INDEPENDENT_ZLIB = 1
V4_SEGMENT_FLAG_REGIONAL_PAYLOAD = 2
V4_SEGMENT_FLAG_RLE_RECORDS = 4
V4_SEGMENT_FLAG_HANDOFF_RECORD = 8
V4_SEGMENT_SUPPORTED_FLAGS = (
    V4_SEGMENT_FLAG_INDEPENDENT_ZLIB
    | V4_SEGMENT_FLAG_REGIONAL_PAYLOAD
    | V4_SEGMENT_FLAG_RLE_RECORDS
    | V4_SEGMENT_FLAG_HANDOFF_RECORD
)
V4_MAX_REGIONS = 5
V4_MAX_RECORD_BYTES = 12352
V4_HANDOFF_OFFSET_MASK = 0x00FFFFFF
V4_HANDOFF_FRAME_SHIFT = 24

HEADER = struct.Struct("<8s6H7I16s")
ENTRY = struct.Struct("<8HI12s")
SEGMENT = struct.Struct("<4I2H")
FRAME_HEADER = struct.Struct("<HHI")
RUN_HEADER = struct.Struct("<IH")
KEY_RUN = struct.Struct("<HB")
V3_SEGMENT_HEADER = struct.Struct("<4sHH14I")
V4_SEGMENT_HEADER = struct.Struct("<4sHHHH5I")
V4_FRAME_HEADER = struct.Struct("<HBBII")
V4_REGION = struct.Struct("<4H")

ASSETS = (
    (1, "dizziness", 1, 40),
    (2, "happy", 1, 40),
    (3, "excited", 1, 40),
    (4, "sleeping", 1, 40),
    (5, "provisioning", 1, 40),
    (6, "disconnected", 1, 40),
    (7, "thinking", 1, 40),
    (8, "thought_complete", 4, 40),
    (9, "meeting", 1, 40),
    (10, "monitoring", 1, 40),
    (11, "touch", 2, 40),
    (12, "playful", 1, 40),
    (13, "like", 1, 40),
    (14, "angry", 1, 40),
    (15, "listening", 1, 40),
    (16, "question", 1, 40),
)


def fail(message: str) -> None:
    raise ValueError(message)


def packed_name(value: str) -> str:
    return value.encode("ascii")[:11].decode("ascii")


def decode_v1_frames(raw: bytes, frame_count: int) -> list[bytes]:
    if len(raw) != frame_count * FRAME_PIXELS:
        fail("v1 frame stream size mismatch")
    current = bytearray(FRAME_PIXELS)
    frames: list[bytes] = []
    for frame_index in range(frame_count):
        encoded = raw[frame_index * FRAME_PIXELS : (frame_index + 1) * FRAME_PIXELS]
        if frame_index == 0:
            current[:] = encoded
        else:
            for pixel, delta in enumerate(encoded):
                current[pixel] ^= delta
        frames.append(bytes(current))
    return frames


def decode_v2_frames(raw: bytes, frame_count: int) -> tuple[list[bytes], int, int]:
    cursor = 0
    current = bytearray(FRAME_PIXELS)
    frames: list[bytes] = []
    changed_total = 0
    run_total = 0
    for frame_index in range(frame_count):
        if cursor + FRAME_HEADER.size > len(raw):
            fail(f"v2 frame {frame_index} header truncated")
        run_count, flags, changed_count = FRAME_HEADER.unpack_from(raw, cursor)
        cursor += FRAME_HEADER.size
        if flags & ~FRAME_FLAG_ABSOLUTE:
            fail(f"v2 frame {frame_index} has unknown flags")
        absolute = bool(flags & FRAME_FLAG_ABSOLUTE)
        if absolute != (frame_index == 0):
            fail(f"v2 frame {frame_index} absolute flag mismatch")
        if run_count > MAX_FRAME_RUNS or (absolute and run_count == 0):
            fail(f"v2 frame {frame_index} run count is invalid")
        if run_count == 0 and changed_count != 0:
            fail(f"v2 frame {frame_index} empty delta count mismatch")
        previous_end = 0
        encoded_pixels = 0
        before = bytes(current)
        for run_index in range(run_count):
            if cursor + RUN_HEADER.size > len(raw):
                fail(f"v2 frame {frame_index} run header truncated")
            offset, length = RUN_HEADER.unpack_from(raw, cursor)
            cursor += RUN_HEADER.size
            if length == 0 or offset < previous_end or offset + length > FRAME_PIXELS:
                fail(f"v2 frame {frame_index} run {run_index} range mismatch")
            if not absolute and offset // WIDTH != (offset + length - 1) // WIDTH:
                fail(f"v2 frame {frame_index} delta run crosses a row")
            if cursor + length > len(raw):
                fail(f"v2 frame {frame_index} run payload truncated")
            current[offset : offset + length] = raw[cursor : cursor + length]
            cursor += length
            previous_end = offset + length
            encoded_pixels += length
        if encoded_pixels != changed_count:
            fail(f"v2 frame {frame_index} changed count mismatch")
        if absolute and (changed_count != FRAME_PIXELS or previous_end != FRAME_PIXELS):
            fail(f"v2 frame {frame_index} absolute coverage mismatch")
        if not absolute:
            actual_changed = sum(left != right for left, right in zip(before, current))
            if actual_changed != changed_count:
                fail(f"v2 frame {frame_index} contains unchanged delta pixels")
        frames.append(bytes(current))
        changed_total += changed_count
        run_total += run_count
    if cursor != len(raw):
        fail(f"v2 segment has {len(raw) - cursor} trailing raw bytes")
    return frames, changed_total, run_total


def apply_delta_record(current: bytearray, record: bytes) -> tuple[int, int]:
    if len(record) < FRAME_HEADER.size:
        fail("v3 delta record header truncated")
    run_count, flags, changed_count = FRAME_HEADER.unpack_from(record)
    if flags != 0 or run_count > MAX_FRAME_RUNS or changed_count > FRAME_PIXELS:
        fail("v3 delta record header mismatch")
    cursor = FRAME_HEADER.size
    previous_end = 0
    encoded_pixels = 0
    before = bytes(current)
    for run_index in range(run_count):
        if cursor + RUN_HEADER.size > len(record):
            fail("v3 delta run header truncated")
        offset, length = RUN_HEADER.unpack_from(record, cursor)
        cursor += RUN_HEADER.size
        if (
            length == 0
            or offset < previous_end
            or offset + length > FRAME_PIXELS
            or offset // WIDTH != (offset + length - 1) // WIDTH
            or cursor + length > len(record)
        ):
            fail(f"v3 delta run {run_index} mismatch")
        current[offset : offset + length] = record[cursor : cursor + length]
        cursor += length
        previous_end = offset + length
        encoded_pixels += length
    if cursor != len(record) or encoded_pixels != changed_count:
        fail("v3 delta record size mismatch")
    actual_changed = sum(left != right for left, right in zip(before, current))
    if actual_changed != changed_count:
        fail("v3 delta contains unchanged pixels")
    return changed_count, run_count


def split_delta_records(raw: bytes, frame_count: int) -> list[bytes]:
    cursor = 0
    records: list[bytes] = []
    for frame_index in range(frame_count):
        if cursor + FRAME_HEADER.size > len(raw):
            fail(f"v3 tail frame {frame_index} header truncated")
        run_count, flags, changed_count = FRAME_HEADER.unpack_from(raw, cursor)
        if flags != 0 or run_count > MAX_FRAME_RUNS or changed_count > FRAME_PIXELS:
            fail(f"v3 tail frame {frame_index} header mismatch")
        end = cursor + FRAME_HEADER.size
        for run_index in range(run_count):
            if end + RUN_HEADER.size > len(raw):
                fail(f"v3 tail frame {frame_index} run header truncated")
            offset, length = RUN_HEADER.unpack_from(raw, end)
            end += RUN_HEADER.size
            if (
                length == 0
                or offset + length > FRAME_PIXELS
                or offset // WIDTH != (offset + length - 1) // WIDTH
                or end + length > len(raw)
            ):
                fail(f"v3 tail frame {frame_index} run {run_index} mismatch")
            end += length
        records.append(raw[cursor:end])
        cursor = end
    if cursor != len(raw):
        fail("v3 tail has trailing raw bytes")
    return records


def decode_v3_key(encoded: bytes) -> bytes:
    current = bytearray()
    cursor = 0
    while cursor < len(encoded):
        if cursor + KEY_RUN.size > len(encoded):
            fail("v3 key RLE truncated")
        length, value = KEY_RUN.unpack_from(encoded, cursor)
        cursor += KEY_RUN.size
        if length == 0 or len(current) + length > FRAME_PIXELS:
            fail("v3 key RLE range mismatch")
        current.extend(bytes((value,)) * length)
    if len(current) != FRAME_PIXELS:
        fail("v3 key RLE coverage mismatch")
    return bytes(current)


def decode_v3_segment(container: bytes, frame_count: int, kind: int) -> dict[str, object]:
    if len(container) < V3_SEGMENT_HEADER.size:
        fail("v3 segment header truncated")
    values = V3_SEGMENT_HEADER.unpack_from(container)
    (
        magic, header_size, flags,
        key_offset, key_size, key_crc,
        tail_offset, tail_size, tail_raw_size, tail_raw_crc,
        transition_offset, transition_size, transition_crc,
        wrap_offset, wrap_size, wrap_crc, payload_crc,
    ) = values
    if (
        magic != V3_SEGMENT_MAGIC
        or header_size != V3_SEGMENT_HEADER.size
        or flags & ~V3_SEGMENT_SUPPORTED_FLAGS
        or not (flags & V3_SEGMENT_FLAG_KEY_RLE)
        or key_offset != header_size
        or key_size == 0
        or key_size > V3_MAX_BOOTSTRAP_BYTES
        or key_offset + key_size > len(container)
        or zlib.crc32(container[header_size:]) & 0xFFFFFFFF != payload_crc
    ):
        fail("v3 segment header mismatch")

    expected_offset = key_offset + key_size
    key = container[key_offset:expected_offset]
    if zlib.crc32(key) & 0xFFFFFFFF != key_crc:
        fail("v3 key CRC mismatch")
    first = decode_v3_key(key)

    has_tail = bool(flags & V3_SEGMENT_FLAG_TAIL_ZLIB)
    if has_tail != (frame_count > 1):
        fail("v3 tail presence mismatch")
    if has_tail:
        if tail_offset != expected_offset or tail_size == 0 or tail_raw_size == 0:
            fail("v3 tail layout mismatch")
        tail_end = tail_offset + tail_size
        if tail_end > len(container):
            fail("v3 tail range mismatch")
        tail_raw = zlib.decompress(container[tail_offset:tail_end])
        if len(tail_raw) != tail_raw_size or zlib.crc32(tail_raw) & 0xFFFFFFFF != tail_raw_crc:
            fail("v3 tail integrity mismatch")
        expected_offset = tail_end
        records = split_delta_records(tail_raw, frame_count - 1)
    else:
        if any((tail_offset, tail_size, tail_raw_size, tail_raw_crc)):
            fail("v3 empty tail metadata mismatch")
        records = []

    is_loop = kind == 1
    has_transition = bool(flags & V3_SEGMENT_FLAG_TRANSITION)
    has_wrap = bool(flags & V3_SEGMENT_FLAG_WRAP)
    if has_transition != is_loop or has_wrap != is_loop:
        fail("v3 loop patch presence mismatch")

    transition = b""
    if has_transition:
        if (
            transition_offset != expected_offset
            or transition_size < FRAME_HEADER.size
            or transition_size > V3_MAX_BOOTSTRAP_BYTES
        ):
            fail("v3 transition layout mismatch")
        transition_end = transition_offset + transition_size
        if transition_end > len(container):
            fail("v3 transition range mismatch")
        transition = container[transition_offset:transition_end]
        if zlib.crc32(transition) & 0xFFFFFFFF != transition_crc:
            fail("v3 transition CRC mismatch")
        expected_offset = transition_end
    elif any((transition_offset, transition_size, transition_crc)):
        fail("v3 empty transition metadata mismatch")

    wrap = b""
    if has_wrap:
        if (
            wrap_offset != expected_offset
            or wrap_size < FRAME_HEADER.size
            or wrap_size > V3_MAX_BOOTSTRAP_BYTES
        ):
            fail("v3 wrap layout mismatch")
        wrap_end = wrap_offset + wrap_size
        if wrap_end > len(container):
            fail("v3 wrap range mismatch")
        wrap = container[wrap_offset:wrap_end]
        if zlib.crc32(wrap) & 0xFFFFFFFF != wrap_crc:
            fail("v3 wrap CRC mismatch")
        expected_offset = wrap_end
    elif any((wrap_offset, wrap_size, wrap_crc)):
        fail("v3 empty wrap metadata mismatch")
    if expected_offset != len(container):
        fail("v3 segment has trailing bytes")

    current = bytearray(first)
    frames = [bytes(current)]
    changed_total = FRAME_PIXELS
    run_total = 1
    for record in records:
        changed, runs = apply_delta_record(current, record)
        frames.append(bytes(current))
        changed_total += changed
        run_total += runs
    canonical = bytearray()
    canonical.extend(FRAME_HEADER.pack(2, FRAME_FLAG_ABSOLUTE, FRAME_PIXELS))
    canonical.extend(RUN_HEADER.pack(0, 0xFFFF))
    canonical.extend(first[:0xFFFF])
    canonical.extend(RUN_HEADER.pack(0xFFFF, FRAME_PIXELS - 0xFFFF))
    canonical.extend(first[0xFFFF:])
    canonical.extend(b"".join(records))
    return {
        "frames": frames,
        "changed": changed_total,
        "runs": run_total,
        "transition": transition,
        "wrap": wrap,
        "canonical": bytes(canonical),
    }


def decode_rle_record(encoded: bytes, expected_size: int) -> bytes:
    output = bytearray()
    cursor = 0
    while cursor < len(encoded):
        control = encoded[cursor]
        cursor += 1
        count = (control & 0x7F) + 1
        if control & 0x80:
            if cursor >= len(encoded):
                fail("v4 rle repeated run is truncated")
            output.extend((encoded[cursor],) * count)
            cursor += 1
        else:
            if count > len(encoded) - cursor:
                fail("v4 rle literal run is truncated")
            output.extend(encoded[cursor : cursor + count])
            cursor += count
        if len(output) > expected_size:
            fail("v4 rle record exceeds decoded size")
    if len(output) != expected_size:
        fail("v4 rle decoded size mismatch")
    return bytes(output)


def decode_v4_segment(container: bytes, frame_count: int) -> dict[str, object]:
    if len(container) < V4_SEGMENT_HEADER.size:
        fail("v4 segment header truncated")
    (
        magic, header_size, flags, declared_frames, max_regions,
        table_offset, records_offset, max_record_size, payload_crc, reserved,
    ) = V4_SEGMENT_HEADER.unpack_from(container)
    table_size = (frame_count + 1) * 4
    if (
        magic != V4_SEGMENT_MAGIC
        or header_size != V4_SEGMENT_HEADER.size
        or not flags & V4_SEGMENT_FLAG_INDEPENDENT_ZLIB
        or flags & ~V4_SEGMENT_SUPPORTED_FLAGS
        or declared_frames != frame_count
        or max_regions != V4_MAX_REGIONS
        or table_offset != header_size
        or records_offset != table_offset + table_size
        or max_record_size < V4_FRAME_HEADER.size + V4_MAX_REGIONS * V4_REGION.size
        or max_record_size > V4_MAX_RECORD_BYTES
        or records_offset > len(container)
        or zlib.crc32(container[table_offset:]) & 0xFFFFFFFF != payload_crc
    ):
        fail("v4 segment header mismatch")
    offsets = struct.unpack_from(f"<{frame_count + 1}I", container, table_offset)
    if offsets[0] != 0 or any(offsets[i] > offsets[i + 1] for i in range(frame_count)):
        fail("v4 frame offset table mismatch")
    has_handoff = bool(flags & V4_SEGMENT_FLAG_HANDOFF_RECORD)
    handoff_frame_index = reserved >> V4_HANDOFF_FRAME_SHIFT
    handoff_offset = reserved & V4_HANDOFF_OFFSET_MASK
    regular_records_end = records_offset + offsets[-1]
    if (
        (not has_handoff and (reserved != 0 or regular_records_end != len(container)))
        or (has_handoff and (
            not flags & V4_SEGMENT_FLAG_RLE_RECORDS
            or handoff_frame_index == 0
            or handoff_frame_index >= frame_count
            or handoff_offset != regular_records_end
            or handoff_offset >= len(container)
        ))
    ):
        fail("v4 records size mismatch")

    current = bytearray(FRAME_PIXELS)
    frames: list[bytes] = []
    changed_total = 0
    region_total = 0
    canonical_crc = 0
    for frame_index in range(frame_count):
        start = records_offset + offsets[frame_index]
        end = records_offset + offsets[frame_index + 1]
        record = container[start:end]
        fixed_size = V4_FRAME_HEADER.size + V4_MAX_REGIONS * V4_REGION.size
        if len(record) < fixed_size or len(record) > max_record_size:
            fail(f"v4 frame {frame_index} record size mismatch")
        compressed_size, frame_flags, region_count, changed, raw_crc = V4_FRAME_HEADER.unpack_from(record)
        absolute = bool(frame_flags & FRAME_FLAG_ABSOLUTE)
        rle = bool(frame_flags & FRAME_FLAG_RLE)
        if (
            frame_flags & ~(FRAME_FLAG_ABSOLUTE | FRAME_FLAG_RLE)
            or (frame_index == 0 and not absolute)
            or (frame_index != 0 and absolute)
            or (rle and not flags & V4_SEGMENT_FLAG_RLE_RECORDS)
            or region_count > V4_MAX_REGIONS
            or (changed == 0) != (region_count == 0)
            or changed > FRAME_PIXELS
            or fixed_size + compressed_size != len(record)
        ):
            fail(f"v4 frame {frame_index} contract mismatch")
        regions = []
        for index in range(V4_MAX_REGIONS):
            region = V4_REGION.unpack_from(record, V4_FRAME_HEADER.size + index * V4_REGION.size)
            if index < region_count:
                x, y, width, height = region
                if width == 0 or height == 0 or x + width > WIDTH or y + height > HEIGHT:
                    fail(f"v4 frame {frame_index} region bounds mismatch")
                regions.append(region)
            elif any(region):
                fail(f"v4 frame {frame_index} unused region is not empty")
        for left, first in enumerate(regions):
            for second in regions[left + 1:]:
                if not (
                    first[0] + first[2] <= second[0]
                    or second[0] + second[2] <= first[0]
                    or first[1] + first[3] <= second[1]
                    or second[1] + second[3] <= first[1]
                ):
                    fail(f"v4 frame {frame_index} regions overlap")
        encoded = record[fixed_size:]
        regional_payload = bool(flags & V4_SEGMENT_FLAG_REGIONAL_PAYLOAD)
        expected_payload_size = (
            sum(width * height for _, _, width, height in regions)
            if regional_payload else FRAME_PIXELS
        )
        payload = (
            decode_rle_record(encoded, expected_payload_size)
            if rle else zlib.decompress(encoded)
        )
        if len(payload) != expected_payload_size or zlib.crc32(payload) & 0xFFFFFFFF != raw_crc:
            fail(f"v4 frame {frame_index} delta integrity mismatch")
        if regional_payload:
            delta_buffer = bytearray(FRAME_PIXELS)
            payload_cursor = 0
            for x, y, width, height in regions:
                for row in range(y, y + height):
                    row_start = row * WIDTH + x
                    delta_buffer[row_start : row_start + width] = payload[
                        payload_cursor : payload_cursor + width
                    ]
                    payload_cursor += width
            if payload_cursor != len(payload):
                fail(f"v4 frame {frame_index} regional payload mismatch")
            delta = bytes(delta_buffer)
        else:
            delta = payload
        if absolute and (
            regions != [(0, 0, WIDTH, HEIGHT)] or expected_payload_size != FRAME_PIXELS
        ):
            fail(f"v4 frame {frame_index} absolute region mismatch")
        actual_changed = FRAME_PIXELS if absolute else sum(value != 0 for value in delta)
        if changed != actual_changed:
            fail(f"v4 frame {frame_index} changed count mismatch")
        previous = bytes(current)
        for pixel, value in enumerate(delta):
            if value == 0 and not absolute:
                continue
            x = pixel % WIDTH
            y = pixel // WIDTH
            if not any(rx <= x < rx + rw and ry <= y < ry + rh for rx, ry, rw, rh in regions):
                fail(f"v4 frame {frame_index} dirty coverage mismatch")
            current[pixel] = value if absolute else current[pixel] ^ value
        frame = bytes(current)
        frames.append(frame)
        canonical_delta = (
            frame if frame_index == 0
            else bytes(before ^ after for before, after in zip(previous, frame))
        )
        canonical_crc = zlib.crc32(canonical_delta, canonical_crc)
        changed_total += changed
        region_total += region_count
    if has_handoff:
        record = container[handoff_offset:]
        fixed_size = V4_FRAME_HEADER.size + V4_MAX_REGIONS * V4_REGION.size
        if len(record) < fixed_size or len(record) > max_record_size:
            fail("v4 handoff record size mismatch")
        compressed_size, frame_flags, region_count, changed, raw_crc = (
            V4_FRAME_HEADER.unpack_from(record)
        )
        regions = [
            V4_REGION.unpack_from(record, V4_FRAME_HEADER.size + index * V4_REGION.size)
            for index in range(V4_MAX_REGIONS)
        ]
        if (
            frame_flags != FRAME_FLAG_ABSOLUTE | FRAME_FLAG_RLE
            or region_count != 1
            or changed != FRAME_PIXELS
            or regions[0] != (0, 0, WIDTH, HEIGHT)
            or any(any(region) for region in regions[1:])
            or fixed_size + compressed_size != len(record)
        ):
            fail("v4 handoff record contract mismatch")
        handoff_frame = decode_rle_record(record[fixed_size:], FRAME_PIXELS)
        if (
            zlib.crc32(handoff_frame) & 0xFFFFFFFF != raw_crc
            or handoff_frame != frames[handoff_frame_index]
        ):
            fail("v4 handoff frame differs from animation frame")
    return {
        "frames": frames,
        "changed": changed_total,
        "runs": region_total,
        "canonical_crc": canonical_crc & 0xFFFFFFFF,
    }


def selected_segments(flags: int, intro: int, loop: int, completion: int) -> tuple[tuple[int, int], ...]:
    if flags == 1:
        if completion != INVALID_SEGMENT:
            fail("persistent topology mismatch")
        return ((intro, 0), (loop, 1))
    if flags == 2:
        if intro != INVALID_SEGMENT or loop != INVALID_SEGMENT:
            fail("transient topology mismatch")
        return ((completion, 2),)
    if flags == 4:
        if intro != INVALID_SEGMENT or loop != INVALID_SEGMENT:
            fail("completion topology mismatch")
        return ((completion, 3),)
    fail("unknown asset flags")


def validate(pack_path: Path, manifest_path: Path | None) -> dict[str, object]:
    blob = pack_path.read_bytes()
    digest = hashlib.sha256(blob).hexdigest()
    if len(blob) < HEADER.size or len(blob) > PARTITION_SIZE:
        fail(f"pack size is invalid: {len(blob)}")
    values = HEADER.unpack_from(blob, 0)
    (
        magic, version, header_size, width, height, entry_count, segment_count,
        entry_offset, segment_offset, palette_offset, data_offset, total_size,
        payload_crc, catalog_crc, build_id,
    ) = values
    if (magic, version) not in (
        (b"NWEMO1\0\0", 1),
        (b"NWEMO2\0\0", 2),
        (b"NWEMO3\0\0", 3),
        (b"NWEMO4\0\0", 4),
    ):
        fail("unsupported emotion pack ABI")
    if (
        header_size != HEADER.size or width != WIDTH or height != HEIGHT
        or entry_count != len(ASSETS) or segment_count != 30
        or entry_offset != HEADER.size
        or segment_offset != entry_offset + entry_count * ENTRY.size
        or palette_offset != segment_offset + segment_count * SEGMENT.size
        or data_offset != (palette_offset + entry_count * 128 + 3) & ~3
        or total_size != len(blob)
    ):
        fail("header contract mismatch")
    if zlib.crc32(blob[header_size:]) & 0xFFFFFFFF != payload_crc:
        fail("payload CRC mismatch")

    segment_rows = []
    previous_end = data_offset
    for index in range(segment_count):
        row = SEGMENT.unpack_from(blob, segment_offset + index * SEGMENT.size)
        offset, compressed_size, raw_size, raw_crc, frame_count, kind = row
        if (
            compressed_size == 0 or raw_size == 0 or frame_count == 0 or kind > 3
            or offset & 3 or offset < previous_end or offset + compressed_size > total_size
        ):
            fail(f"segment {index} layout mismatch")
        encoded = blob[offset : offset + compressed_size]
        if version == 1:
            raw = zlib.decompress(encoded)
            if len(raw) != raw_size or zlib.crc32(raw) & 0xFFFFFFFF != raw_crc:
                fail(f"segment {index} integrity mismatch")
            frames = decode_v1_frames(raw, frame_count)
            changed = 0
            runs = 0
            transition = b""
            wrap = b""
        elif version == 2:
            raw = zlib.decompress(encoded)
            if len(raw) != raw_size or zlib.crc32(raw) & 0xFFFFFFFF != raw_crc:
                fail(f"segment {index} integrity mismatch")
            frames, changed, runs = decode_v2_frames(raw, frame_count)
            transition = b""
            wrap = b""
        elif version == 3:
            decoded = decode_v3_segment(encoded, frame_count, kind)
            frames = decoded["frames"]
            changed = decoded["changed"]
            runs = decoded["runs"]
            transition = decoded["transition"]
            wrap = decoded["wrap"]
            canonical = decoded["canonical"]
            if len(canonical) != raw_size or zlib.crc32(canonical) & 0xFFFFFFFF != raw_crc:
                fail(f"segment {index} canonical stream mismatch")
        else:
            decoded = decode_v4_segment(encoded, frame_count)
            frames = decoded["frames"]
            changed = decoded["changed"]
            runs = decoded["runs"]
            transition = b""
            wrap = b""
            if raw_size != frame_count * FRAME_PIXELS or decoded["canonical_crc"] != raw_crc:
                fail(f"segment {index} v4 canonical stream mismatch")
        previous_end = (offset + compressed_size + 3) & ~3
        segment_rows.append((kind, frames, changed, runs, transition, wrap))

    references = [0] * segment_count
    frames_by_segment: list[list[bytes] | None] = [None] * segment_count
    names = []
    decoded_frames = 0
    changed_total = 0
    run_total = 0
    for index, contract in enumerate(ASSETS):
        row = ENTRY.unpack_from(blob, entry_offset + index * ENTRY.size)
        (
            asset_id, flags, fps, palette_count, intro, loop, completion,
            reserved, asset_palette_offset, raw_name,
        ) = row
        expected_id, expected_name, expected_flags, expected_fps = contract
        name = raw_name.split(b"\0", 1)[0].decode("ascii")
        names.append(name)
        if (
            asset_id != expected_id or name != packed_name(expected_name)
            or flags != expected_flags or fps != expected_fps or palette_count != 64
            or reserved != 0 or asset_palette_offset != palette_offset + index * 128
        ):
            fail(f"asset {expected_id} contract mismatch")
        palette = struct.unpack_from("<64H", blob, asset_palette_offset)
        for segment_index, expected_kind in selected_segments(flags, intro, loop, completion):
            if segment_index >= segment_count:
                fail(f"asset {asset_id} segment index mismatch")
            references[segment_index] += 1
            kind, frames, changed, runs, _, _ = segment_rows[segment_index]
            if kind != expected_kind:
                fail(f"asset {asset_id} segment kind mismatch")
            if any(max(frame) >= palette_count for frame in frames):
                fail(f"asset {asset_id} palette index overflow")
            frames_by_segment[segment_index] = frames
            decoded_frames += len(frames)
            changed_total += changed
            run_total += runs
            _ = palette
    if references != [1] * segment_count or any(item is None for item in frames_by_segment):
        fail("segment ownership mismatch")

    if version == 3:
        for index, contract in enumerate(ASSETS):
            row = ENTRY.unpack_from(blob, entry_offset + index * ENTRY.size)
            flags, intro, loop = row[1], row[4], row[5]
            if flags != 1:
                continue
            intro_frames = frames_by_segment[intro]
            loop_frames = frames_by_segment[loop]
            if intro_frames is None or loop_frames is None:
                fail("v3 persistent topology is incomplete")
            transition = segment_rows[loop][4]
            transition_frame = bytearray(intro_frames[-1])
            apply_delta_record(transition_frame, transition)
            if bytes(transition_frame) != loop_frames[0]:
                fail(f"v3 asset {row[0]} intro-to-loop patch mismatch")
            wrap = segment_rows[loop][5]
            wrap_frame = bytearray(loop_frames[-1])
            apply_delta_record(wrap_frame, wrap)
            if bytes(wrap_frame) != loop_frames[0]:
                fail(f"v3 asset {row[0]} loop-wrap patch mismatch")

    manifest = None
    if manifest_path is not None:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest_entries = manifest.get("entries")
        entry_value = len(manifest_entries) if isinstance(manifest_entries, list) else manifest_entries
        if (
            manifest.get("bytes") != len(blob)
            or manifest.get("targetFps") != 40
            or manifest.get("segments") != segment_count
            or entry_value != entry_count
            or manifest.get("partition") != "emotion_0"
            or manifest.get("partitionOffset") != "0xB00000"
            or manifest.get("storageBytes") != PARTITION_SIZE
            or manifest.get("remainingBytes") != PARTITION_SIZE - len(blob)
            or manifest.get("payloadCrc32") != f"{payload_crc:08x}"
            or manifest.get("catalogCrc32") != f"{catalog_crc:08x}"
            or manifest.get("sha256") != digest
        ):
            fail("JSON manifest mismatch")

    return {
        "abi": version,
        "build_id": build_id.split(b"\0", 1)[0].decode("ascii"),
        "sha256": digest,
        "bytes": len(blob),
        "remaining_bytes": PARTITION_SIZE - len(blob),
        "entries": entry_count,
        "segments": segment_count,
        "decoded_frames": decoded_frames,
        "average_changed_pixels": changed_total // max(1, decoded_frames) if version >= 2 else None,
        "average_runs": run_total // max(1, decoded_frames) if version >= 2 else None,
        "names": names,
        "_frames": frames_by_segment,
    }


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser()
    parser.add_argument("--pack", type=Path, default=root / "assets" / "emotion" / "emotion-assets.bin")
    parser.add_argument("--manifest", type=Path, default=root / "assets" / "emotion" / "emotion-assets.json")
    parser.add_argument("--reference", type=Path)
    args = parser.parse_args()
    try:
        result = validate(args.pack, args.manifest if args.manifest.exists() else None)
        if args.reference:
            reference = validate(args.reference, None)
            if result["_frames"] != reference["_frames"]:
                fail("decoded pixels differ from reference pack")
            result["pixel_exact_reference"] = str(args.reference)
        result.pop("_frames", None)
    except (OSError, ValueError, json.JSONDecodeError, zlib.error) as error:
        print(f"emotion pack verification failed: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, ensure_ascii=True, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
