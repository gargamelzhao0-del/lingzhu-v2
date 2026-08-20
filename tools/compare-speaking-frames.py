#!/usr/bin/env python3
"""Compare simulator and LVGL speaking-indicator frames in RGB565 space."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from PIL import Image, ImageChops


DEFAULT_ROI = (176, 28, 281, 124)


def rgb565_image(image: Image.Image) -> Image.Image:
    rgb = image.convert("RGB")
    source = rgb.load()
    result = Image.new("RGB", rgb.size)
    target = result.load()
    for y in range(rgb.height):
        for x in range(rgb.width):
            red, green, blue = source[x, y]
            red5 = red >> 3
            green6 = green >> 2
            blue5 = blue >> 3
            target[x, y] = (
                (red5 << 3) | (red5 >> 2),
                (green6 << 2) | (green6 >> 4),
                (blue5 << 3) | (blue5 >> 2),
            )
    return result


def frame_map(directory: Path) -> dict[str, Path]:
    frames = {}
    for pattern in ("*.png", "*.ppm"):
        for path in directory.glob(pattern):
            if path.stem.isdigit():
                frames[path.stem] = path
    return frames


def compare_frame(reference: Image.Image, candidate: Image.Image,
                  roi: tuple[int, int, int, int], threshold: int) -> dict:
    if reference.size != (360, 360) or candidate.size != (360, 360):
        raise ValueError("all input frames must be exactly 360x360")
    reference = rgb565_image(reference).crop(roi)
    candidate = rgb565_image(candidate).crop(roi)
    diff = ImageChops.difference(reference, candidate)
    pixels = []
    roi_x, roi_y = roi[0], roi[1]
    for y in range(diff.height):
        for x in range(diff.width):
            screen_x = roi_x + x
            screen_y = roi_y + y
            if ((screen_x - 180) ** 2 + (screen_y - 180) ** 2 <=
                    180 ** 2):
                pixels.append(diff.getpixel((x, y)))
    changed = sum(1 for pixel in pixels if max(pixel) > threshold)
    channel_total = sum(sum(pixel) for pixel in pixels)
    return {
        "changed_pixels": changed,
        "changed_percent": changed * 100.0 / len(pixels),
        "mean_abs_channel_error": channel_total / (len(pixels) * 3.0),
        "max_channel_error": max(max(pixel) for pixel in pixels),
        "bbox": diff.getbbox(),
        "diff": diff,
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Compare matched 360x360 simulator/LVGL speaking frames after "
            "RGB565 quantization, restricted to the speaking bubble ROI."
        )
    )
    parser.add_argument("--reference-dir", type=Path, required=True)
    parser.add_argument("--candidate-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--threshold", type=int, default=8)
    parser.add_argument("--max-changed-percent", type=float, default=8.0)
    parser.add_argument("--max-mean-error", type=float, default=3.0)
    parser.add_argument("--min-elapsed-ms", type=int, default=0,
                        help="ignore numeric frames before this timestamp")
    parser.add_argument("--max-elapsed-ms", type=int,
                        help="ignore numeric frames after this timestamp")
    parser.add_argument("--roi", nargs=4, type=int, default=DEFAULT_ROI,
                        metavar=("X1", "Y1", "X2", "Y2"))
    args = parser.parse_args()

    reference_frames = frame_map(args.reference_dir)
    candidate_frames = frame_map(args.candidate_dir)
    common = sorted(
        name for name in reference_frames.keys() & candidate_frames.keys()
        if int(name) >= args.min_elapsed_ms and
        (args.max_elapsed_ms is None or int(name) <= args.max_elapsed_ms)
    )
    missing_reference = sorted(candidate_frames.keys() - reference_frames.keys())
    missing_candidate = sorted(reference_frames.keys() - candidate_frames.keys())
    if not common:
        raise SystemExit("no matching PNG/PPM frame names found")

    args.output_dir.mkdir(parents=True, exist_ok=True)
    results = []
    failed = False
    for name in common:
        with Image.open(reference_frames[name]) as reference_image, \
                Image.open(candidate_frames[name]) as candidate_image:
            metrics = compare_frame(reference_image, candidate_image,
                                    tuple(args.roi), args.threshold)
        diff = metrics.pop("diff")
        if (metrics["changed_percent"] > args.max_changed_percent or
                metrics["mean_abs_channel_error"] > args.max_mean_error):
            metrics["passed"] = False
            failed = True
            heatmap = diff.point(lambda value: min(255, value * 6))
            heatmap.save(args.output_dir / f"{name}.diff.png")
        else:
            metrics["passed"] = True
        metrics["frame"] = name
        results.append(metrics)

    report = {
        "format": "nuwax-speaking-rgb565-frame-comparison-v1",
        "roi": args.roi,
        "threshold": args.threshold,
        "limits": {
            "max_changed_percent": args.max_changed_percent,
            "max_mean_error": args.max_mean_error,
            "min_elapsed_ms": args.min_elapsed_ms,
            "max_elapsed_ms": args.max_elapsed_ms,
        },
        "frame_count": len(results),
        "missing_reference": missing_reference,
        "missing_candidate": missing_candidate,
        "worst_changed_percent": max(
            result["changed_percent"] for result in results),
        "worst_mean_abs_channel_error": max(
            result["mean_abs_channel_error"] for result in results),
        "passed": not failed and not missing_reference and not missing_candidate,
        "frames": results,
    }
    report_path = args.output_dir / "speaking-frame-comparison.json"
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2),
                           encoding="utf-8")
    print(json.dumps({key: value for key, value in report.items()
                      if key != "frames"}, ensure_ascii=False, indent=2))
    print(f"report={report_path}")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
