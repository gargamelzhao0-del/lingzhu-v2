#!/usr/bin/env python3
"""Convert a verified NWEMO1 pack into the sparse NWEMO2 representation.

NWEMO2 keeps the production catalog, palettes and segment topology unchanged.
Only segment payloads change: every frame contains absolute palette-index runs
for its first frame and sparse absolute-index runs for subsequent frames.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import zlib
from pathlib import Path

WIDTH = 360
HEIGHT = 360
PIXELS = WIDTH * HEIGHT
PARTITION_SIZE = 0x400000
V1_MAGIC = b"NWEMO1\0\0"
V2_MAGIC = b"NWEMO2\0\0"
V1_VERSION = 1
V2_VERSION = 2
FRAME_FLAG_ABSOLUTE = 1
MAX_FRAME_RUNS = 2048

HEADER = struct.Struct("<8s6H7I16s")
ENTRY = struct.Struct("<8HI12s")
SEGMENT = struct.Struct("<4I2H")
FRAME_HEADER = struct.Struct("<HHI")
RUN_HEADER = struct.Struct("<IH")


def align4(value: int) -> int:
    return (value + 3) & ~3


def encode_value_range(offset: int, values: bytes) -> list[tuple[int, bytes]]:
    runs: list[tuple[int, bytes]] = []
    cursor = 0
    limit = len(values)
    while cursor < limit:
        length = min(0xFFFF, limit - cursor)
        runs.append((offset + cursor, values[cursor : cursor + length]))
        cursor += length
    return runs


def encode_absolute(frame: bytes) -> bytes:
    runs = encode_value_range(0, frame)
    if len(runs) > MAX_FRAME_RUNS:
        raise ValueError("absolute frame exceeds firmware run limit")
    record = bytearray(FRAME_HEADER.pack(len(runs), FRAME_FLAG_ABSOLUTE, PIXELS))
    for offset, payload in runs:
        record.extend(RUN_HEADER.pack(offset, len(payload)))
        record.extend(payload)
    return bytes(record)


def encode_delta(previous: bytes, current: bytes) -> bytes:
    changed = bytearray(PIXELS)
    changed_count = 0
    for index, (before, after) in enumerate(zip(previous, current)):
        if before != after:
            changed[index] = 1
            changed_count += 1

    runs: list[tuple[int, bytes]] = []
    row_start = 0
    for _ in range(HEIGHT):
        row_end = row_start + WIDTH
        cursor = row_start
        while cursor < row_end:
            while cursor < row_end and not changed[cursor]:
                cursor += 1
            if cursor >= row_end:
                break
            start = cursor
            while cursor < row_end and changed[cursor]:
                cursor += 1
            runs.extend(encode_value_range(start, current[start:cursor]))
        row_start = row_end

    if len(runs) > MAX_FRAME_RUNS:
        raise ValueError("delta frame exceeds firmware run limit")
    record = bytearray(FRAME_HEADER.pack(len(runs), 0, changed_count))
    for offset, payload in runs:
        record.extend(RUN_HEADER.pack(offset, len(payload)))
        record.extend(payload)
    return bytes(record)


def decode_v1_segment(compressed: bytes, raw_size: int, frame_count: int, raw_crc: int) -> list[bytes]:
    raw = zlib.decompress(compressed)
    if len(raw) != raw_size or zlib.crc32(raw) & 0xFFFFFFFF != raw_crc:
        raise ValueError("v1 segment integrity mismatch")
    if raw_size != frame_count * PIXELS:
        raise ValueError("v1 segment frame size mismatch")

    frames: list[bytes] = []
    current = bytearray(PIXELS)
    for frame_index in range(frame_count):
        encoded = raw[frame_index * PIXELS : (frame_index + 1) * PIXELS]
        if frame_index == 0:
            current[:] = encoded
        else:
            for pixel, delta in enumerate(encoded):
                current[pixel] ^= delta
        frames.append(bytes(current))
    return frames


def convert(blob: bytes, partition_size: int) -> tuple[bytes, dict[str, object]]:
    if len(blob) < HEADER.size:
        raise ValueError("input pack is truncated")
    header = list(HEADER.unpack_from(blob, 0))
    if header[0] != V1_MAGIC or header[1] != V1_VERSION or header[2] != HEADER.size:
        raise ValueError("input is not a supported NWEMO1 pack")
    if header[3] != WIDTH or header[4] != HEIGHT or header[11] != len(blob):
        raise ValueError("input geometry or size mismatch")
    if zlib.crc32(blob[HEADER.size:]) & 0xFFFFFFFF != header[12]:
        raise ValueError("input payload CRC mismatch")

    entry_count = header[5]
    segment_count = header[6]
    entry_offset = header[7]
    segment_offset = header[8]
    palette_offset = header[9]
    old_data_offset = header[10]
    metadata_end = palette_offset + entry_count * 64 * 2
    new_data_offset = align4(metadata_end)
    if old_data_offset != new_data_offset:
        raise ValueError("input metadata layout mismatch")

    entries = blob[entry_offset:segment_offset]
    palettes = blob[palette_offset:metadata_end]
    segment_rows = [
        SEGMENT.unpack_from(blob, segment_offset + index * SEGMENT.size)
        for index in range(segment_count)
    ]

    data = bytearray()
    new_segments: list[tuple[int, int, int, int, int, int]] = []
    frame_total = 0
    changed_total = 0
    run_total = 0
    max_frame_bytes = 0
    for offset, compressed_size, raw_size, raw_crc, frame_count, kind in segment_rows:
        frames = decode_v1_segment(
            blob[offset : offset + compressed_size], raw_size, frame_count, raw_crc
        )
        sparse = bytearray()
        previous: bytes | None = None
        for frame in frames:
            record = encode_absolute(frame) if previous is None else encode_delta(previous, frame)
            span_count, _, changed = FRAME_HEADER.unpack_from(record, 0)
            run_total += span_count
            changed_total += changed
            max_frame_bytes = max(max_frame_bytes, len(record))
            sparse.extend(record)
            previous = frame
            frame_total += 1
        compressed_sparse = zlib.compress(bytes(sparse), 9)
        new_offset = new_data_offset + len(data)
        new_segments.append(
            (
                new_offset,
                len(compressed_sparse),
                len(sparse),
                zlib.crc32(sparse) & 0xFFFFFFFF,
                frame_count,
                kind,
            )
        )
        data.extend(compressed_sparse)
        while len(data) & 3:
            data.append(0)

    segments = b"".join(SEGMENT.pack(*row) for row in new_segments)
    metadata = entries + segments + palettes
    if HEADER.size + len(metadata) > new_data_offset:
        raise ValueError("v2 metadata overflow")
    body = metadata + bytes(new_data_offset - HEADER.size - len(metadata)) + bytes(data)
    total_size = HEADER.size + len(body)
    if total_size > partition_size:
        raise ValueError(
            f"NWEMO2 pack is {total_size} bytes and exceeds partition by {total_size - partition_size}"
        )

    catalog_crc = header[13]
    build_id = f"ui2-{catalog_crc:08x}".encode("ascii")
    new_header = HEADER.pack(
        V2_MAGIC,
        V2_VERSION,
        HEADER.size,
        WIDTH,
        HEIGHT,
        entry_count,
        segment_count,
        entry_offset,
        segment_offset,
        palette_offset,
        new_data_offset,
        total_size,
        zlib.crc32(body) & 0xFFFFFFFF,
        catalog_crc,
        build_id + b"\0" * (16 - len(build_id)),
    )
    result = new_header + body
    manifest = {
        "format": "nuwax-device-emotion-pack-v2",
        "width": WIDTH,
        "height": HEIGHT,
        "targetFps": 40,
        "entries": entry_count,
        "segments": segment_count,
        "frames": frame_total,
        "bytes": len(result),
        "storageBytes": partition_size,
        "remainingBytes": partition_size - len(result),
        "partition": "emotion_0",
        "partitionOffset": "0xB00000",
        "payloadCrc32": f"{zlib.crc32(body) & 0xFFFFFFFF:08x}",
        "catalogCrc32": f"{catalog_crc:08x}",
        "sha256": hashlib.sha256(result).hexdigest(),
        "averageChangedPixels": changed_total // max(1, frame_total),
        "averageRuns": run_total // max(1, frame_total),
        "maximumFrameBytes": max_frame_bytes,
        "sourceSha256": hashlib.sha256(blob).hexdigest(),
    }
    return result, manifest


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument(
        "--output",
        type=Path,
        default=root / "assets" / "emotion" / "emotion-assets.bin",
    )
    parser.add_argument("--partition-size", type=lambda value: int(value, 0), default=PARTITION_SIZE)
    args = parser.parse_args()
    result, manifest = convert(args.input.read_bytes(), args.partition_size)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(result)
    manifest_path = args.output.with_suffix(".json")
    manifest_path.write_text(
        json.dumps(manifest, ensure_ascii=True, indent=2) + "\n", encoding="utf-8"
    )
    print(
        f"wrote {args.output} bytes={len(result)} remaining={manifest['remainingBytes']} "
        f"sha256={manifest['sha256']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
