"""Generate and check a synthetic SD audio protocol fixture; Python standard library only.

Run: python sample.py
The files in sample/ contain 960 ms of silence, never a real device recording.
*.bin chunk files are WebSocket Binary message payloads, without WebSocket framing.
"""

import hashlib
import json
import struct
import wave
import zlib
from pathlib import Path


SESSION = "meeting_0123456789abcdef"
FRAME_COUNT = 48
FRAME_BYTES = 662
CHUNK_BYTES = 8064


def make_archive(mode=0, session=SESSION):
    header = bytearray(96)
    header[:8] = b"NWXOFF1\0"
    struct.pack_into("<HHBBHIHHIQ", header, 8, 1, 96, mode, 1, 1, 16000, 20, 2, 0, 1000)
    header[36:36 + len(session)] = session.encode("ascii")
    records, crc = [], 0
    for seq in range(FRAME_COUNT):
        frame = bytearray(FRAME_BYTES)
        struct.pack_into("<2sBBBBHHIIHH", frame, 0,
                         b"NA", 1, 1, 0, 1, 16000, 20, seq, 1000 + seq * 20, 640, 0)
        crc = zlib.crc32(frame, crc)
        records.append(struct.pack("<I", len(frame)) + frame)
    footer = bytearray(32)
    footer[:8] = b"NWXEND1\0"
    struct.pack_into("<HHIQIB", footer, 8, 1, 32, FRAME_COUNT,
                     FRAME_COUNT * FRAME_BYTES, crc, 1)
    return bytes(header) + b"".join(records) + bytes(footer)


def parse_archive(data):
    """Validate the current typed SD PCM format and extract PCM, without trusting lengths."""
    if len(data) < 128 or data[:8] != b"NWXOFF1\0":
        raise ValueError("Invalid archive header")
    if struct.unpack_from("<HH", data, 8) != (1, 96):
        raise ValueError("Unsupported archive version")
    if data[12] not in (0, 1) or data[13] != 1 or struct.unpack_from("<H", data, 14)[0] != 1:
        raise ValueError("Unsupported mode / channels / codec")
    if struct.unpack_from("<IH", data, 16) != (16000, 20):
        raise ValueError("Unsupported sample rate / frame duration")
    footer_pos = len(data) - 32
    if data[footer_pos:footer_pos + 8] != b"NWXEND1\0":
        raise ValueError("Invalid archive footer")
    version, size, count, payload_bytes, expected_crc, complete = struct.unpack_from(
        "<HHIQIB", data, footer_pos + 8)
    if (version, size) != (1, 32) or complete not in (0, 1):
        raise ValueError("Unsupported footer")
    offset, crc, total = 96, 0, 0
    pcm, sequences, timestamps = [], [], []
    while offset < footer_pos:
        if offset + 4 > footer_pos:
            raise ValueError("Truncated record length")
        length = struct.unpack_from("<I", data, offset)[0]
        offset += 4
        if length != 662 or offset + length > footer_pos:
            raise ValueError("Invalid or truncated PCM record")
        frame = data[offset:offset + length]
        magic, ver, codec, flags, channels, rate, duration, seq, ts, nbytes, reserved = struct.unpack_from(
            "<2sBBBBHHIIHH", frame)
        if (magic, ver, codec, channels, rate, duration, nbytes) != (b"NA", 1, 1, 1, 16000, 20, 640):
            raise ValueError("Unsupported NA audio geometry")
        crc = zlib.crc32(frame, crc)
        total += length
        pcm.append(frame[22:])
        sequences.append(seq)
        timestamps.append(ts)
        offset += length
    if offset != footer_pos or len(pcm) != count or total != payload_bytes or crc != expected_crc:
        raise ValueError("Archive count / length / CRC mismatch")
    if len(data) != 96 + count * 4 + payload_bytes + 32:
        raise ValueError("Archive total size mismatch")
    result = {
        "audio_session_id": data[36:84].split(b"\0", 1)[0].decode("ascii"),
        "segment_index": struct.unpack_from("<I", data, 24)[0],
        "mode": "meeting" if data[12] == 0 else "monitoring",
        "start_uptime_ms": struct.unpack_from("<Q", data, 28)[0],
        "frame_count": count, "na_payload_bytes": payload_bytes, "crc32": crc,
        "complete": bool(complete), "audio_duration_ms": count * 20,
        "first_sequence": sequences[0] if sequences else None,
        "last_sequence": sequences[-1] if sequences else None,
        "sequence_gaps": sum(b != ((a + 1) & 0xffffffff) for a, b in zip(sequences, sequences[1:])),
        "timestamp_gaps": sum(((b - a) & 0xffffffff) != 20 for a, b in zip(timestamps, timestamps[1:])),
    }
    # Gaps are diagnostic results: monitoring may intentionally omit silence.
    return result, b"".join(pcm)


def make_chunk(archive, offset, size, request_id, version=1, final=False):
    request = request_id.encode("ascii")
    if not 0 < len(request) <= 39 or version not in (1, 2):
        raise ValueError("Invalid chunk identity / version")
    if not 0 < size <= CHUNK_BYTES or offset < 0 or offset + size > len(archive):
        raise ValueError("Invalid chunk range")
    header = bytearray(64)
    header[:6] = b"NWXCH1"
    struct.pack_into("<III", header, 8, version, offset, size)
    header[20:20 + len(request)] = request
    struct.pack_into("<I", header, 60, int(final) if version == 2 else 0)
    return bytes(header) + archive[offset:offset + size]


def write_examples(directory):
    directory.mkdir(parents=True, exist_ok=True)
    archive = make_archive()
    summary, pcm = parse_archive(archive)
    assert zlib.crc32(b"123456789") == 0xCBF43926
    assert summary["sequence_gaps"] == summary["timestamp_gaps"] == 0
    assert len(archive) == 32096 and len(pcm) == 30720
    (directory / "archive.bin").write_bytes(archive)
    monitoring = make_archive(1, "monitor_0123456789abcdef")
    monitoring_summary, monitoring_pcm = parse_archive(monitoring)
    assert monitoring_summary["crc32"] == summary["crc32"] and monitoring_pcm == pcm
    (directory / "monitoring-archive.bin").write_bytes(monitoring)
    with wave.open(str(directory / "audio.wav"), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(16000)
        wav.writeframes(pcm)
    # All current modes use one file commit, without intermediate data ACKs.
    profiles = [
        ("meeting-file", "meeting_audio"),
        ("monitoring-realtime", "monitoring_audio_realtime"),
        ("monitoring-history", "monitoring_audio_history"),
    ]
    scenarios = []
    for name, route in profiles:
        is_monitoring = route != "meeting_audio"
        data = monitoring if is_monitoring else archive
        session = "monitor_0123456789abcdef" if is_monitoring else SESSION
        begin = {
            "type": route + "_begin", "request_id": "offline_begin_00000001",
            "audio_session_id": session, "segment_index": 0,
            "total_bytes": len(data), "crc32": summary["crc32"],
        }
        commit = {"type": route + "_commit", "request_id": "offline_commit_00000001", "audio_session_id": session, "segment_index": 0}
        end = {"type": ("monitoring" if is_monitoring else "meeting") + "_audio_session_end",
               "request_id": "sd_end_00000001", "audio_session_id": session,
               "segment_count": 1, "recording_complete": True}
        end_ack = {"type": end["type"] + "_ack", "request_id": end["request_id"],
                   "audio_session_id": session, "ok": True}
        events = []
        binary_files = []

        def add(direction, payload, binary=False):
            events.append({"direction": direction, "binary_file" if binary else "json": payload})

        begin_ack = {"type": route + "_begin_ack", "request_id": begin["request_id"],
                     "audio_session_id": session, "segment_index": 0, "ok": True, "next_offset": 0}
        add("device_to_cloud", begin)
        add("cloud_to_device", begin_ack)
        for index, offset in enumerate(range(0, len(data), CHUNK_BYTES)):
            size = min(CHUNK_BYTES, len(data) - offset)
            final = offset + size == len(data)
            request = begin["request_id"]
            packet = make_chunk(data, offset, size, request, 2, final)
            filename = f"{name}-chunk-{index:02}.bin"
            (directory / filename).write_bytes(packet)
            binary_files.append(filename)
            add("device_to_cloud", filename, binary=True)
        add("device_to_cloud", commit)
        add("cloud_to_device", {"type": route + "_commit_ack", "request_id": commit["request_id"],
                                "audio_session_id": session, "segment_index": 0, "ok": True})
        add("device_to_cloud", end)
        add("cloud_to_device", end_ack)
        transcript = f"transcript-{name}.json"
        (directory / transcript).write_text(json.dumps(events, indent=2) + "\n", encoding="utf-8")
        scenarios.append({"name": name, "transcript": transcript,
                          "archive": "monitoring-archive.bin" if is_monitoring else "archive.bin",
                          "ack_scope": "file_commit",
                          "binary_files": binary_files})
    manifest = {
        "description": "Synthetic 960 ms silence; no user audio. Each scenario starts with independent storage; session_end is sent after recording stops.",
        "device_id_for_test": "NX-CLOUD-EXAMPLE",
        "total_bytes": len(archive), **summary,
        "archive_sha256": hashlib.sha256(archive).hexdigest(),
        "pcm_bytes": len(pcm), "sample_rate": 16000, "channels": 1, "sample_bits": 16,
        "chunk_payload_lengths": [8064, 8064, 8064, 7904],
        "commit_confirmed_bytes": 32096,
        "scenarios": scenarios,
        "monitoring_example": {
            "file": "monitoring-archive.bin",
            "audio_session_id": "monitor_0123456789abcdef",
            "segment_index": 0,
            "start_uptime_ms": 1000, "audio_duration_ms": 960,
            "crc32": monitoring_summary["crc32"],
            "archive_sha256": hashlib.sha256(monitoring).hexdigest(),
        },
    }
    (directory / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


if __name__ == "__main__":
    print(json.dumps(write_examples(Path(__file__).resolve().parent / "sample"), indent=2))
