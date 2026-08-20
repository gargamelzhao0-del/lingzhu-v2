#!/usr/bin/env python3
"""Convert a verified NWEMO3 pack into independently prefetched NWEMO4 frames.

NWEMO4 keeps the catalog, palettes, segment topology and decoded pixels intact.
Each frame is an independent record. Absolute key and transition handoff frames
use a bounded PackBits-style RLE record so the device can prepare a switch
without a long zlib replay. Delta frames select zlib or the same bounded RLE
codec and pack only pixels inside exact, non-overlapping component rectangles.
Firmware therefore applies and invalidates only localized expression components
during normal playback while avoiding zlib on records that RLE can decode with
bounded storage overhead.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import struct
import zlib
from pathlib import Path

WIDTH = 360
HEIGHT = 360
PIXELS = WIDTH * HEIGHT
PARTITION_SIZE = 0x400000
MAX_REGIONS = 5
MAGIC = b"NWEMO4\0\0"
SEGMENT_MAGIC = b"SGV4"
SEGMENT_FLAG_INDEPENDENT_ZLIB = 1
SEGMENT_FLAG_REGIONAL_PAYLOAD = 2
SEGMENT_FLAG_RLE_RECORDS = 4
SEGMENT_FLAG_HANDOFF_RECORD = 8
FRAME_FLAG_ABSOLUTE = 1
FRAME_FLAG_RLE = 2
MAX_ENCODED_FRAME_BYTES = 12 * 1024
DELTA_RLE_SIZE_SLACK = 256
HANDOFF_OFFSET_MASK = 0x00FFFFFF
HANDOFF_FRAME_SHIFT = 24

HEADER = struct.Struct("<8s6H7I16s")
ENTRY = struct.Struct("<8HI12s")
SEGMENT = struct.Struct("<4I2H")
SEGMENT_HEADER = struct.Struct("<4sHHHH5I")
FRAME_HEADER = struct.Struct("<HBBII")
REGION = struct.Struct("<4H")


def align4(value: int) -> int:
    return (value + 3) & ~3


def load_verifier(root: Path):
    path = root / "tools" / "verify-emotion-pack.py"
    spec = importlib.util.spec_from_file_location("nuwax_verify_emotion_pack", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load verifier: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def row_regions(delta: bytes) -> list[list[int]]:
    regions: list[list[int]] = []
    active: list[list[int]] = []
    for y in range(HEIGHT):
        row = delta[y * WIDTH : (y + 1) * WIDTH]
        spans: list[tuple[int, int]] = []
        x = 0
        while x < WIDTH:
            while x < WIDTH and row[x] == 0:
                x += 1
            if x == WIDTH:
                break
            start = x
            while x < WIDTH and row[x] != 0:
                x += 1
            spans.append((start, x - 1))

        next_active: list[list[int]] = []
        consumed: set[int] = set()
        for x0, x1 in spans:
            candidates = [
                (index, region)
                for index, region in enumerate(active)
                if index not in consumed and x0 <= region[2] + 8 and region[0] <= x1 + 8
            ]
            if candidates:
                index, region = min(
                    candidates,
                    key=lambda item: max(item[1][2], x1) - min(item[1][0], x0),
                )
                consumed.add(index)
                region[0] = min(region[0], x0)
                region[2] = max(region[2], x1)
                region[3] = y
                next_active.append(region)
            else:
                next_active.append([x0, y, x1, y])
        for index, region in enumerate(active):
            if index not in consumed:
                regions.append(region)
        active = next_active
    regions.extend(active)
    return regions


def rectangle_area(region: list[int]) -> int:
    return (region[2] - region[0] + 1) * (region[3] - region[1] + 1)


def merge_regions(regions: list[list[int]], limit: int = MAX_REGIONS) -> list[list[int]]:
    regions = sorted(regions, key=lambda item: (item[1], item[0]))
    while len(regions) > limit:
        best_pair: tuple[int, int] | None = None
        best_penalty: int | None = None
        best_merged: list[int] | None = None
        for left in range(len(regions)):
            for right in range(left + 1, len(regions)):
                first = regions[left]
                second = regions[right]
                merged = [
                    min(first[0], second[0]),
                    min(first[1], second[1]),
                    max(first[2], second[2]),
                    max(first[3], second[3]),
                ]
                penalty = rectangle_area(merged) - rectangle_area(first) - rectangle_area(second)
                if best_penalty is None or penalty < best_penalty:
                    best_pair = (left, right)
                    best_penalty = penalty
                    best_merged = merged
        if best_pair is None or best_merged is None:
            raise RuntimeError("dirty region compaction made no progress")
        left, right = best_pair
        regions[left] = best_merged
        del regions[right]
        regions.sort(key=lambda item: (item[1], item[0]))

    # XOR deltas may only be applied once.  Compacting two components can make
    # their bounding boxes overlap a third component, so close that union here
    # instead of requiring a runtime visited-pixel bitmap.
    merged_overlap = True
    while merged_overlap:
        merged_overlap = False
        for left in range(len(regions)):
            for right in range(left + 1, len(regions)):
                a = regions[left]
                b = regions[right]
                overlaps = not (
                    a[2] < b[0] or b[2] < a[0] or
                    a[3] < b[1] or b[3] < a[1]
                )
                if not overlaps:
                    continue
                regions[left] = [
                    min(a[0], b[0]), min(a[1], b[1]),
                    max(a[2], b[2]), max(a[3], b[3]),
                ]
                del regions[right]
                regions.sort(key=lambda item: (item[1], item[0]))
                merged_overlap = True
                break
            if merged_overlap:
                break
    return regions


def dirty_regions(delta: bytes, absolute: bool) -> list[list[int]]:
    if absolute:
        return [[0, 0, WIDTH - 1, HEIGHT - 1]]
    regions = merge_regions(row_regions(delta))
    for left, first in enumerate(regions):
        for second in regions[left + 1 :]:
            if not (
                first[2] < second[0] or second[2] < first[0] or
                first[3] < second[1] or second[3] < first[1]
            ):
                raise RuntimeError("dirty regions overlap")
    for pixel, value in enumerate(delta):
        if value == 0:
            continue
        x = pixel % WIDTH
        y = pixel // WIDTH
        if not any(r[0] <= x <= r[2] and r[1] <= y <= r[3] for r in regions):
            raise RuntimeError(f"dirty pixel is not covered: {pixel}")
    return regions


def encode_rle(payload: bytes) -> bytes:
    """Encode 1..128-byte literal and repeated runs with a one-byte control."""
    encoded = bytearray()
    cursor = 0
    while cursor < len(payload):
        run = 1
        while (
            cursor + run < len(payload)
            and payload[cursor + run] == payload[cursor]
            and run < 128
        ):
            run += 1
        if run >= 3:
            encoded.extend((0x80 | (run - 1), payload[cursor]))
            cursor += run
            continue

        literal_start = cursor
        cursor += run
        while cursor < len(payload) and cursor - literal_start < 128:
            next_run = 1
            while (
                cursor + next_run < len(payload)
                and payload[cursor + next_run] == payload[cursor]
                and next_run < 128
            ):
                next_run += 1
            if next_run >= 3:
                break
            cursor += min(next_run, 128 - (cursor - literal_start))
        literal_size = cursor - literal_start
        encoded.append(literal_size - 1)
        encoded.extend(payload[literal_start:cursor])
    return bytes(encoded)


def encode_segment(
    frames: list[bytes], fast_absolute_frames: set[int]
) -> tuple[bytes, dict[str, int]]:
    records: list[bytes] = []
    previous = bytes(PIXELS)
    changed_total = 0
    invalidated_total = 0
    max_record_size = 0
    max_inflated_size = 0
    maximum_rle_size = 0
    rle_frames = 0
    rle_delta_frames = 0
    zlib_frames = 0
    for frame_index, frame in enumerate(frames):
        absolute = frame_index == 0
        delta = frame if absolute else bytes(a ^ b for a, b in zip(previous, frame))
        changed = PIXELS if absolute else sum(value != 0 for value in delta)
        regions = dirty_regions(delta, absolute)
        regional = bytearray()
        for x0, y0, x1, y1 in regions:
            width = x1 - x0 + 1
            for y in range(y0, y1 + 1):
                row_start = y * WIDTH + x0
                regional.extend(delta[row_start : row_start + width])
        payload = bytes(regional)
        rle_payload = encode_rle(payload)
        zlib_payload = zlib.compress(payload, 9)
        rle = absolute or (
            len(rle_payload) <= MAX_ENCODED_FRAME_BYTES
            and len(rle_payload) <= len(zlib_payload) + DELTA_RLE_SIZE_SLACK
        )
        compressed = rle_payload if rle else zlib_payload
        if len(compressed) > MAX_ENCODED_FRAME_BYTES:
            raise ValueError(
                f"encoded frame exceeds runtime limit: frame={frame_index} bytes={len(compressed)}"
            )
        if len(compressed) > 0xFFFF:
            raise ValueError("compressed frame exceeds uint16 size")
        if rle:
            maximum_rle_size = max(maximum_rle_size, len(compressed))
            rle_frames += 1
            if not absolute:
                rle_delta_frames += 1
        else:
            zlib_frames += 1
        header = bytearray(
            FRAME_HEADER.pack(
                len(compressed),
                (FRAME_FLAG_ABSOLUTE if absolute else 0)
                | (FRAME_FLAG_RLE if rle else 0),
                len(regions),
                changed,
                zlib.crc32(payload) & 0xFFFFFFFF,
            )
        )
        for index in range(MAX_REGIONS):
            if index < len(regions):
                x0, y0, x1, y1 = regions[index]
                header.extend(REGION.pack(x0, y0, x1 - x0 + 1, y1 - y0 + 1))
                invalidated_total += (x1 - x0 + 1) * (y1 - y0 + 1)
            else:
                header.extend(REGION.pack(0, 0, 0, 0))
        record = bytes(header) + compressed
        records.append(record)
        max_record_size = max(max_record_size, len(record))
        max_inflated_size = max(max_inflated_size, len(payload))
        changed_total += changed
        previous = frame

    handoff_record = b""
    handoff_frame_index = next(iter(fast_absolute_frames), None)
    if len(fast_absolute_frames) > 1:
        raise ValueError("a segment may contain only one fast handoff record")
    if handoff_frame_index is not None:
        frame = frames[handoff_frame_index]
        regions = [[0, 0, WIDTH - 1, HEIGHT - 1]]
        compressed = encode_rle(frame)
        if len(compressed) > MAX_ENCODED_FRAME_BYTES:
            raise ValueError(
                f"handoff frame exceeds runtime limit: frame={handoff_frame_index} bytes={len(compressed)}"
            )
        header = bytearray(
            FRAME_HEADER.pack(
                len(compressed), FRAME_FLAG_ABSOLUTE | FRAME_FLAG_RLE,
                1, PIXELS, zlib.crc32(frame) & 0xFFFFFFFF,
            )
        )
        header.extend(REGION.pack(0, 0, WIDTH, HEIGHT))
        for _ in range(MAX_REGIONS - 1):
            header.extend(REGION.pack(0, 0, 0, 0))
        handoff_record = bytes(header) + compressed
        max_record_size = max(max_record_size, len(handoff_record))
        max_inflated_size = max(max_inflated_size, len(frame))
        maximum_rle_size = max(maximum_rle_size, len(compressed))
        rle_frames += 1

    offsets = [0]
    for record in records:
        offsets.append(offsets[-1] + len(record))
    offset_table = struct.pack(f"<{len(offsets)}I", *offsets)
    records_offset = SEGMENT_HEADER.size + len(offset_table)
    records_blob = b"".join(records)
    handoff_offset = records_offset + len(records_blob)
    if handoff_offset > HANDOFF_OFFSET_MASK:
        raise ValueError("handoff record offset exceeds segment header field")
    reserved = 0
    segment_flags = (
        SEGMENT_FLAG_INDEPENDENT_ZLIB
        | SEGMENT_FLAG_REGIONAL_PAYLOAD
        | SEGMENT_FLAG_RLE_RECORDS
    )
    if handoff_frame_index is not None:
        if handoff_frame_index <= 0 or handoff_frame_index > 0xFF:
            raise ValueError("handoff frame index exceeds segment header field")
        segment_flags |= SEGMENT_FLAG_HANDOFF_RECORD
        reserved = (handoff_frame_index << HANDOFF_FRAME_SHIFT) | handoff_offset
    encoded_records = records_blob + handoff_record
    segment_header = SEGMENT_HEADER.pack(
        SEGMENT_MAGIC,
        SEGMENT_HEADER.size,
        segment_flags,
        len(frames),
        MAX_REGIONS,
        SEGMENT_HEADER.size,
        records_offset,
        max_record_size,
        zlib.crc32(offset_table + encoded_records) & 0xFFFFFFFF,
        reserved,
    )
    return segment_header + offset_table + encoded_records, {
        "changed": changed_total,
        "invalidated": invalidated_total,
        "max_record": max_record_size,
        "max_inflated": max_inflated_size,
        "max_rle": maximum_rle_size,
        "rle_frames": rle_frames,
        "rle_delta_frames": rle_delta_frames,
        "zlib_frames": zlib_frames,
    }


def convert(source: Path, output: Path, manifest_path: Path, partition_size: int) -> dict[str, object]:
    root = Path(__file__).resolve().parents[1]
    verifier = load_verifier(root)
    verified = verifier.validate(source, None)
    if verified["abi"] != 3:
        raise ValueError("NWEMO4 conversion requires a verified NWEMO3 source")

    blob = source.read_bytes()
    values = HEADER.unpack_from(blob)
    entry_count = values[5]
    segment_count = values[6]
    entry_offset = values[7]
    segment_offset = values[8]
    palette_offset = values[9]
    data_offset = values[10]
    catalog_crc = values[13]
    entries_blob = blob[entry_offset:segment_offset]
    palettes_blob = blob[palette_offset:data_offset]
    source_segments = [
        SEGMENT.unpack_from(blob, segment_offset + index * SEGMENT.size)
        for index in range(segment_count)
    ]
    fast_absolute_by_segment: dict[int, set[int]] = {}
    for entry_index in range(entry_count):
        entry = ENTRY.unpack_from(blob, entry_offset + entry_index * ENTRY.size)
        asset_id = entry[0]
        if asset_id == 4:
            fast_absolute_by_segment.setdefault(entry[5], set()).add(12)
        elif asset_id == 11:
            fast_absolute_by_segment.setdefault(entry[6], set()).add(5)

    containers: list[bytes] = []
    segment_rows: list[tuple[int, int, int, int, int, int]] = []
    cursor = data_offset
    changed_total = 0
    invalidated_total = 0
    frame_total = 0
    max_record_size = 0
    max_inflated_size = 0
    max_rle_size = 0
    rle_frame_total = 0
    rle_delta_frame_total = 0
    zlib_frame_total = 0
    for index, frames in enumerate(verified["_frames"]):
        fast_frames = fast_absolute_by_segment.get(index, set())
        if any(frame_index >= len(frames) for frame_index in fast_frames):
            raise ValueError(f"handoff frame is outside segment {index}")
        container, stats = encode_segment(frames, fast_frames)
        source_row = source_segments[index]
        raw_size = len(frames) * PIXELS
        raw_crc = 0
        previous = bytes(PIXELS)
        for frame_index, frame in enumerate(frames):
            delta = frame if frame_index == 0 else bytes(a ^ b for a, b in zip(previous, frame))
            raw_crc = zlib.crc32(delta, raw_crc)
            previous = frame
        segment_rows.append((cursor, len(container), raw_size, raw_crc & 0xFFFFFFFF, len(frames), source_row[5]))
        containers.append(container)
        cursor = align4(cursor + len(container))
        changed_total += stats["changed"]
        invalidated_total += stats["invalidated"]
        max_record_size = max(max_record_size, stats["max_record"])
        max_inflated_size = max(max_inflated_size, stats["max_inflated"])
        max_rle_size = max(max_rle_size, stats["max_rle"])
        rle_frame_total += stats["rle_frames"]
        rle_delta_frame_total += stats["rle_delta_frames"]
        zlib_frame_total += stats["zlib_frames"]
        frame_total += len(frames)

    total_size = cursor
    if total_size > partition_size:
        raise ValueError(f"NWEMO4 pack exceeds partition by {total_size - partition_size} bytes")

    segments_blob = b"".join(SEGMENT.pack(*row) for row in segment_rows)
    body = bytearray(entries_blob + segments_blob + palettes_blob)
    while HEADER.size + len(body) < data_offset:
        body.append(0)
    for container in containers:
        body.extend(container)
        while (HEADER.size + len(body)) & 3:
            body.append(0)
    if HEADER.size + len(body) != total_size:
        raise RuntimeError("NWEMO4 layout size mismatch")

    build_id = b"emotion-v4-40fps"[:15].ljust(16, b"\0")
    header = HEADER.pack(
        MAGIC, 4, HEADER.size, WIDTH, HEIGHT, entry_count, segment_count,
        entry_offset, segment_offset, palette_offset, data_offset, total_size,
        zlib.crc32(body) & 0xFFFFFFFF, catalog_crc, build_id,
    )
    result = header + bytes(body)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(result)
    digest = hashlib.sha256(result).hexdigest()
    manifest = {
        "format": "nuwax-device-emotion-pack-v4",
        "partition": "emotion_0",
        "partitionOffset": "0xB00000",
        "width": WIDTH,
        "height": HEIGHT,
        "targetFps": 40,
        "entries": entry_count,
        "segments": segment_count,
        "frames": frame_total,
        "bytes": len(result),
        "storageBytes": partition_size,
        "remainingBytes": partition_size - len(result),
        "averageChangedPixels": changed_total // max(1, frame_total),
        "averageInvalidatedPixels": invalidated_total // max(1, frame_total),
        "maximumFrameRecordBytes": max_record_size,
        "maximumInflatedFrameBytes": max_inflated_size,
        "maximumRleFrameBytes": max_rle_size,
        "rleFrames": rle_frame_total,
        "rleAbsoluteFrames": rle_frame_total - rle_delta_frame_total,
        "rleDeltaFrames": rle_delta_frame_total,
        "zlibFrames": zlib_frame_total,
        "payloadCrc32": f"{zlib.crc32(body) & 0xFFFFFFFF:08x}",
        "catalogCrc32": f"{catalog_crc:08x}",
        "sha256": digest,
        "sourceSha256": hashlib.sha256(blob).hexdigest(),
    }
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, default=root / "assets" / "emotion" / "emotion-assets.bin")
    parser.add_argument("--output", type=Path, default=root / "artifacts" / "emotion-assets-v4-candidate.bin")
    parser.add_argument("--manifest", type=Path, default=root / "artifacts" / "emotion-assets-v4-candidate.json")
    parser.add_argument("--partition-size", type=lambda value: int(value, 0), default=PARTITION_SIZE)
    args = parser.parse_args()
    manifest = convert(args.input, args.output, args.manifest, args.partition_size)
    print(json.dumps(manifest, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
