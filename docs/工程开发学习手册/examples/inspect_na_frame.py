"""Read ONE NA-v1 PCM16 frame. No archive/WAV parsing or source modification.

Offsets follow main/audio_frame.c. The self-test uses synthetic silence only.
This teaching parser is intentionally stricter about length than a stream peek.
"""
import argparse
import json
from pathlib import Path
import struct

HEADER = struct.Struct("<2sBBBBHHIIHH")
MAX_BYTES = 22 + 65535


def inspect(data):
    if len(data) < HEADER.size or len(data) > MAX_BYTES:
        raise ValueError("invalid frame size")
    magic, version, codec, flags, channels, rate, duration, seq, timestamp, payload, reserved = HEADER.unpack_from(data)
    if magic != b"NA" or version != 1 or codec != 1:
        raise ValueError("expected NA version 1 PCM16")
    if channels == 0 or rate == 0 or duration == 0 or reserved != 0:
        raise ValueError("invalid geometry/reserved field")
    if len(data) != HEADER.size + payload or payload % (2 * channels):
        raise ValueError("payload length/channel alignment mismatch")
    if payload == 0 and not (flags & 2):
        raise ValueError("empty payload without END")
    return {
        "sequence": seq, "timestamp_ms": timestamp, "flags": flags,
        "sample_rate": rate, "channels": channels, "frame_duration_ms": duration,
        "payload_bytes": payload, "samples_per_channel": payload // (2 * channels),
        "payload_seconds": payload / (2 * channels * rate),
    }


def self_test():
    assert HEADER.size == 22
    frame = HEADER.pack(b"NA", 1, 1, 1, 1, 16000, 20, 7, 140, 640, 0) + bytes(640)
    parsed = inspect(frame)
    assert parsed["sequence"] == 7 and parsed["samples_per_channel"] == 320
    assert parsed["payload_seconds"] == 0.020
    terminal = HEADER.pack(b"NA", 1, 1, 2, 1, 16000, 20, 8, 160, 0, 0)
    assert inspect(terminal)["payload_bytes"] == 0
    malformed = [b"", frame[:-1], frame + b"x", b"XX" + frame[2:],
                 HEADER.pack(b"NA", 1, 1, 1, 0, 16000, 20, 7, 140, 640, 0) + bytes(640)]
    for data in malformed:
        try:
            inspect(data)
        except ValueError:
            pass
        else:
            raise AssertionError("malformed frame accepted")
    return "inspect_na_frame: PASS"


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("file", type=Path, nargs="?")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        print(self_test())
    elif args.file:
        # Bound the read too: the file could grow after a stat check.
        with args.file.open("rb") as stream:
            data = stream.read(MAX_BYTES + 1)
        print(json.dumps(inspect(data), ensure_ascii=False, indent=2))
    else:
        parser.error("provide a single NA frame file or --self-test")
