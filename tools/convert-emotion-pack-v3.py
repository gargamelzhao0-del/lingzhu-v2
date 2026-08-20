#!/usr/bin/env python3
"""Convert a verified NWEMO2 pack into the restart-efficient NWEMO3 format.

NWEMO3 preserves the production catalog, palettes, 30-segment topology and
every decoded pixel. Each segment stores a compact uncompressed key frame, a
separate zlib delta tail and, for loop segments, explicit intro/loop and loop
wrap patches. This lets firmware restart a segment without inflating its
absolute first frame from a long zlib stream.
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
INVALID_SEGMENT = 0xFFFF
V2_MAGIC = b"NWEMO2\0\0"
V3_MAGIC = b"NWEMO3\0\0"
V2_VERSION = 2
V3_VERSION = 3
FRAME_FLAG_ABSOLUTE = 1
MAX_FRAME_RUNS = 2048

V3_SEGMENT_MAGIC = b"SGV3"
V3_SEGMENT_FLAG_KEY_RLE = 1 << 0
V3_SEGMENT_FLAG_TAIL_ZLIB = 1 << 1
V3_SEGMENT_FLAG_TRANSITION = 1 << 2
V3_SEGMENT_FLAG_WRAP = 1 << 3

HEADER = struct.Struct("<8s6H7I16s")
ENTRY = struct.Struct("<8HI12s")
SEGMENT = struct.Struct("<4I2H")
FRAME_HEADER = struct.Struct("<HHI")
RUN_HEADER = struct.Struct("<IH")
KEY_RUN = struct.Struct("<HB")
V3_SEGMENT_HEADER = struct.Struct("<4sHH14I")


def align4(value: int) -> int:
    return (value + 3) & ~3


def encode_value_range(offset: int, values: bytes) -> list[tuple[int, bytes]]:
    runs: list[tuple[int, bytes]] = []
    cursor = 0
    while cursor < len(values):
        length = min(0xFFFF, len(values) - cursor)
        runs.append((offset + cursor, values[cursor : cursor + length]))
        cursor += length
    return runs


def encode_absolute(frame: bytes) -> bytes:
    runs = encode_value_range(0, frame)
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


def parse_v2_frames(raw: bytes, frame_count: int) -> list[bytes]:
    cursor = 0
    current = bytearray(PIXELS)
    frames: list[bytes] = []
    for frame_index in range(frame_count):
        if cursor + FRAME_HEADER.size > len(raw):
            raise ValueError(f"v2 frame {frame_index} header truncated")
        run_count, flags, changed_count = FRAME_HEADER.unpack_from(raw, cursor)
        cursor += FRAME_HEADER.size
        absolute = bool(flags & FRAME_FLAG_ABSOLUTE)
        if flags & ~FRAME_FLAG_ABSOLUTE or absolute != (frame_index == 0):
            raise ValueError(f"v2 frame {frame_index} flags mismatch")
        if run_count > MAX_FRAME_RUNS or changed_count > PIXELS:
            raise ValueError(f"v2 frame {frame_index} bounds mismatch")
        previous_end = 0
        encoded_pixels = 0
        for run_index in range(run_count):
            if cursor + RUN_HEADER.size > len(raw):
                raise ValueError(f"v2 frame {frame_index} run header truncated")
            offset, length = RUN_HEADER.unpack_from(raw, cursor)
            cursor += RUN_HEADER.size
            if (
                length == 0
                or offset < previous_end
                or offset + length > PIXELS
                or (not absolute and offset // WIDTH != (offset + length - 1) // WIDTH)
                or cursor + length > len(raw)
            ):
                raise ValueError(f"v2 frame {frame_index} run {run_index} invalid")
            current[offset : offset + length] = raw[cursor : cursor + length]
            cursor += length
            previous_end = offset + length
            encoded_pixels += length
        if encoded_pixels != changed_count:
            raise ValueError(f"v2 frame {frame_index} changed count mismatch")
        if absolute and (changed_count != PIXELS or previous_end != PIXELS):
            raise ValueError(f"v2 frame {frame_index} absolute coverage mismatch")
        frames.append(bytes(current))
    if cursor != len(raw):
        raise ValueError("v2 segment has trailing bytes")
    return frames


def encode_key_rle(frame: bytes) -> bytes:
    encoded = bytearray()
    cursor = 0
    while cursor < len(frame):
        end = cursor + 1
        while end < len(frame) and frame[end] == frame[cursor] and end - cursor < 0xFFFF:
            end += 1
        encoded.extend(KEY_RUN.pack(end - cursor, frame[cursor]))
        cursor = end
    return bytes(encoded)


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def convert(blob: bytes, partition_size: int) -> tuple[bytes, dict[str, object]]:
    if len(blob) < HEADER.size:
        raise ValueError("input pack is truncated")
    header = list(HEADER.unpack_from(blob, 0))
    if header[0] != V2_MAGIC or header[1] != V2_VERSION or header[2] != HEADER.size:
        raise ValueError("input is not a supported NWEMO2 pack")
    if header[3] != WIDTH or header[4] != HEIGHT or header[11] != len(blob):
        raise ValueError("input geometry or size mismatch")
    if crc32(blob[HEADER.size:]) != header[12]:
        raise ValueError("input payload CRC mismatch")

    entry_count = header[5]
    segment_count = header[6]
    entry_offset = header[7]
    segment_offset = header[8]
    palette_offset = header[9]
    metadata_end = palette_offset + entry_count * 64 * 2
    data_offset = align4(metadata_end)
    if header[10] != data_offset:
        raise ValueError("input metadata layout mismatch")

    entries_blob = blob[entry_offset:segment_offset]
    palettes = blob[palette_offset:metadata_end]
    entries = [
        ENTRY.unpack_from(blob, entry_offset + index * ENTRY.size)
        for index in range(entry_count)
    ]
    source_segments = [
        SEGMENT.unpack_from(blob, segment_offset + index * SEGMENT.size)
        for index in range(segment_count)
    ]

    frames_by_segment: list[list[bytes]] = []
    raw_by_segment: list[bytes] = []
    for index, (offset, compressed_size, raw_size, raw_crc, frame_count, _) in enumerate(source_segments):
        raw = zlib.decompress(blob[offset : offset + compressed_size])
        if len(raw) != raw_size or crc32(raw) != raw_crc:
            raise ValueError(f"source segment {index} integrity mismatch")
        frames_by_segment.append(parse_v2_frames(raw, frame_count))
        raw_by_segment.append(raw)

    intro_for_loop: dict[int, int] = {}
    for entry in entries:
        flags, intro, loop = entry[1], entry[4], entry[5]
        if flags == 1 and loop != INVALID_SEGMENT:
            if intro == INVALID_SEGMENT or loop in intro_for_loop:
                raise ValueError("persistent segment topology is not unique")
            intro_for_loop[loop] = intro

    data = bytearray()
    output_segments: list[tuple[int, int, int, int, int, int]] = []
    frame_total = 0
    changed_total = 0
    run_total = 0
    maximum_frame_bytes = 0
    bootstrap_bytes = 0
    for segment_index, source in enumerate(source_segments):
        _, _, raw_size, raw_crc, frame_count, kind = source
        frames = frames_by_segment[segment_index]
        records = [encode_absolute(frames[0])]
        records.extend(encode_delta(frames[i - 1], frames[i]) for i in range(1, len(frames)))
        reconstructed = b"".join(records)
        if reconstructed != raw_by_segment[segment_index]:
            raise ValueError(f"source segment {segment_index} is not canonical NWEMO2")

        key = encode_key_rle(frames[0])
        tail_raw = b"".join(records[1:])
        tail = zlib.compress(tail_raw, 9) if tail_raw else b""
        transition = b""
        wrap = b""
        flags = V3_SEGMENT_FLAG_KEY_RLE
        if tail:
            flags |= V3_SEGMENT_FLAG_TAIL_ZLIB
        if kind == 1:
            intro_index = intro_for_loop.get(segment_index)
            if intro_index is None:
                raise ValueError(f"loop segment {segment_index} has no owning intro")
            transition = encode_delta(frames_by_segment[intro_index][-1], frames[0])
            wrap = encode_delta(frames[-1], frames[0])
            flags |= V3_SEGMENT_FLAG_TRANSITION | V3_SEGMENT_FLAG_WRAP

        key_offset = V3_SEGMENT_HEADER.size
        tail_offset = key_offset + len(key) if tail else 0
        transition_offset = key_offset + len(key) + len(tail) if transition else 0
        wrap_offset = key_offset + len(key) + len(tail) + len(transition) if wrap else 0
        payload = key + tail + transition + wrap
        container_header = V3_SEGMENT_HEADER.pack(
            V3_SEGMENT_MAGIC,
            V3_SEGMENT_HEADER.size,
            flags,
            key_offset,
            len(key),
            crc32(key),
            tail_offset,
            len(tail),
            len(tail_raw),
            crc32(tail_raw) if tail_raw else 0,
            transition_offset,
            len(transition),
            crc32(transition) if transition else 0,
            wrap_offset,
            len(wrap),
            crc32(wrap) if wrap else 0,
            crc32(payload),
        )
        container = container_header + payload
        output_offset = data_offset + len(data)
        output_segments.append((output_offset, len(container), raw_size, raw_crc, frame_count, kind))
        data.extend(container)
        while len(data) & 3:
            data.append(0)

        for record in records:
            run_count, _, changed_count = FRAME_HEADER.unpack_from(record)
            frame_total += 1
            changed_total += changed_count
            run_total += run_count
            maximum_frame_bytes = max(maximum_frame_bytes, len(record))
        bootstrap_bytes += len(key) + len(transition) + len(wrap)

    segments_blob = b"".join(SEGMENT.pack(*row) for row in output_segments)
    metadata = entries_blob + segments_blob + palettes
    if HEADER.size + len(metadata) > data_offset:
        raise ValueError("v3 metadata overflow")
    body = metadata + bytes(data_offset - HEADER.size - len(metadata)) + bytes(data)
    total_size = HEADER.size + len(body)
    if total_size > partition_size:
        raise ValueError(
            f"NWEMO3 pack is {total_size} bytes and exceeds partition by {total_size - partition_size}"
        )

    catalog_crc = header[13]
    build_id = f"ui3-{catalog_crc:08x}".encode("ascii")
    output_header = HEADER.pack(
        V3_MAGIC,
        V3_VERSION,
        HEADER.size,
        WIDTH,
        HEIGHT,
        entry_count,
        segment_count,
        entry_offset,
        segment_offset,
        palette_offset,
        data_offset,
        total_size,
        crc32(body),
        catalog_crc,
        build_id + b"\0" * (16 - len(build_id)),
    )
    result = output_header + body
    manifest = {
        "format": "nuwax-device-emotion-pack-v3",
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
        "payloadCrc32": f"{crc32(body):08x}",
        "catalogCrc32": f"{catalog_crc:08x}",
        "sha256": hashlib.sha256(result).hexdigest(),
        "averageChangedPixels": changed_total // max(1, frame_total),
        "averageRuns": run_total // max(1, frame_total),
        "maximumFrameBytes": maximum_frame_bytes,
        "bootstrapBytes": bootstrap_bytes,
        "sourceSha256": hashlib.sha256(blob).hexdigest(),
    }
    return result, manifest


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--input",
        type=Path,
        default=root / "assets" / "emotion" / "emotion-assets.bin",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=root / "artifacts" / "emotion-assets-v3-candidate.bin",
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
