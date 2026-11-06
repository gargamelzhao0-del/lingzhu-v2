"""Teaching calculator, not a device benchmark. Python standard library only."""
import argparse
import json
import math


def budget(hours=8.0, speed_kib=500.0):
    if not math.isfinite(hours) or not math.isfinite(speed_kib) or hours <= 0 or speed_kib <= 0:
        raise ValueError("hours and speed must be finite positive values")
    frames = math.floor(hours * 3600 * 50)
    pcm_bytes = frames * 640
    record_bytes = frames * 666
    return {
        "geometry": "PCM16 / 16000 Hz / mono / 20 ms",
        "frames": frames,
        "represented_seconds": frames * 0.02,
        "pcm_bytes": pcm_bytes,
        "record_bytes_excluding_segment_header_footer": record_bytes,
        "pcm_MiB": round(pcm_bytes / 1048576, 6),
        "record_MiB": round(record_bytes / 1048576, 6),
        "assumed_effective_KiB_per_second": speed_kib,
        "theoretical_record_transfer_minutes": round(record_bytes / (speed_kib * 1024 * 60), 6),
        "note": "Arithmetic only; no measured card/network speed or durability claim.",
    }


def self_test():
    result = budget()
    assert result["frames"] == 1440000
    assert result["pcm_bytes"] == 921600000
    assert result["record_bytes_excluding_segment_header_footer"] == 959040000
    assert result["theoretical_record_transfer_minutes"] == 31.21875
    for hours, speed in [(0, 500), (8, 0), (float("nan"), 1), (1, float("inf"))]:
        try:
            budget(hours, speed)
        except ValueError:
            pass
        else:
            raise AssertionError("invalid input accepted")
    return "audio_budget: PASS"


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hours", type=float, default=8)
    parser.add_argument("--speed-kib", type=float, default=500)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        print(self_test())
    else:
        print(json.dumps(budget(args.hours, args.speed_kib), ensure_ascii=False, indent=2))
