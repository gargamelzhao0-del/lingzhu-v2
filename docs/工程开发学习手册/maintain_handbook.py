"""Build the Chinese handbook and check local links using only Python stdlib.

Run from any directory. Writes only inside this handbook folder.
--refresh-evidence explicitly updates source/config fingerprints; it does NOT
review prose automatically. No firmware builds, device access, network calls,
gateway listeners, or credential values are involved.
"""
import argparse
from datetime import date, datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
from urllib.parse import unquote, urlsplit

BOOK = Path(__file__).resolve().parent
ROOT = BOOK.parent.parent
EVIDENCE = BOOK / "evidence"
CHAPTERS = sorted(BOOK.glob("[0-9][0-9]-*.md"))
LINK = re.compile(r"(?<!!)\[[^\]\n]+\]\(([^\n]+?)\)")


def write_json(path, data):
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def prose(text):
    return re.sub(r"^```[^\n]*\n.*?^```\s*$", "", text, flags=re.M | re.S)


def local_links(path):
    for raw in LINK.findall(prose(path.read_text(encoding="utf-8"))):
        target = raw.strip().strip("<>")
        parsed = urlsplit(target)
        if parsed.scheme or target.startswith("#"):
            continue
        yield (target, (path.parent / unquote(parsed.path)).resolve())


def inventory():
    # Repository inventory, not a claim of line-by-line review.
    out = []
    for top in ("main", "bootloader_components", "tools"):
        for path in sorted((ROOT / top).rglob("*")):
            if not path.is_file() or path.suffix not in {".c", ".h", ".inc", ".py", ".ps1", ".js", ".cmake", ".yml"}:
                continue
            parts = set(path.relative_to(ROOT).parts)
            if parts & {"node_modules", "__pycache__", ".git", "build", "build-host-checks"}:
                continue
            rel = path.relative_to(ROOT).as_posix()
            out.append({"path": rel, "bytes": path.stat().st_size,
                        "kind": "test_or_stub" if any(x in rel for x in ("host_check", "host_stubs", "/tests/", "/test-")) else "source_or_tool"})
    return out


def refresh_evidence(review_date):
    paths = set()
    for page in [BOOK / "README.md", *CHAPTERS]:
        for _, target in local_links(page):
            if target.is_file() and ROOT in target.parents and BOOK not in target.parents:
                paths.add(target)
    for rel in ["CMakeLists.txt", "main/CMakeLists.txt", "main/Kconfig.projbuild", "main/idf_component.yml",
                "dependencies.lock", "sdkconfig.defaults", "sdkconfig.product-ota.defaults",
                "sdkconfig.sd-replay-formal.defaults", "partitions.customwake.csv"]:
        paths.add(ROOT / rel)
    for rel in ["lib/lvgl/src/core/lv_obj.c", "lib/lvgl/src/core/lv_refr.c", "lib/lvgl/src/display/lv_display.c"]:
        paths.add(ROOT / rel)
    try:
        head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        head = None
    version_match = re.search(r'set\(PROJECT_VER "([^"]+)"\)', (ROOT / "CMakeLists.txt").read_text(encoding="utf-8"))
    version = version_match[1] if version_match else "unresolved"
    write_json(EVIDENCE / "source-manifest.json", {
        "schema": "nuwax.handbook.source-evidence.v1", "recorded_utc": datetime.now(timezone.utc).isoformat(),
        "declared_baseline": f"{review_date.isoformat()} / default {version} / current working tree",
        "git_head_context_only": head,
        "warning": "Working-tree fingerprints, not a clean-commit or reproducible-build claim. No firmware tested by this script.",
        "files": [{"path": p.relative_to(ROOT).as_posix(), "bytes": p.stat().st_size, "sha256": digest(p)}
                  for p in sorted(paths) if p.is_file()]})
    items = inventory()
    write_json(EVIDENCE / "source-inventory.json", {"scope": ["main", "bootloader_components", "tools"],
        "note": "Navigation only. Generated build folders excluded; not a production-linked-file list.",
        "count": len(items), "files": items})
    write_json(EVIDENCE / "config-snapshot.json", config_snapshot())


def config_snapshot():
    # Exact whitelist only: never serialize URLs, SSIDs, passwords, tokens, PoP,
    # device IDs or raw full sdkconfig contents.
    allowed = {
        "IDF_TARGET", "ESPTOOLPY_FLASHMODE", "ESPTOOLPY_FLASHSIZE", "ESPTOOLPY_FLASHFREQ",
        "SPIRAM", "SPIRAM_MODE_OCT", "SPIRAM_SPEED", "SPIRAM_MALLOC_ALWAYSINTERNAL",
        "SPIRAM_MALLOC_RESERVE_INTERNAL", "FREERTOS_HZ", "LWIP_TCP_SND_BUF_DEFAULT",
        "NUWAX_TEST_PANEL_ENABLE", "NUWAX_OFFLINE_AUDIO_ENABLE", "NUWAX_OFFLINE_AUDIO_MAX_SEGMENT_BYTES",
        "NUWAX_OFFLINE_AUDIO_MAX_BYTES", "NUWAX_MEETING_OFFLINE_MAX_HOURS", "NUWAX_MONITORING_OFFLINE_MAX_HOURS",
        "NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE", "NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE",
        "NUWAX_LIVE_AEC_ENABLE", "NUWAX_P3_SOFTWARE_REF_ENABLE", "NUWAX_P3_SOFTWARE_REF_CAPACITY_MS",
        "NUWAX_WAKE_WORD_ENABLE", "NUWAX_WAKE_WORD_BACKEND_MULTINET", "NUWAX_WAKE_WORD_BACKEND_WAKENET",
        "NUWAX_CUSTOM_WAKE_WORD_THRESHOLD_PERCENT", "NUWAX_WAKE_WORD_INTERRUPT_ENABLE",
        "NUWAX_WAKE_WORD_TASK_STACK_BYTES", "NUWAX_WAKE_WORD_TASK_PRIORITY",
        "NUWAX_WAKE_CAPTURE_TASK_STACK_BYTES", "NUWAX_WAKE_CAPTURE_TASK_PRIORITY",
        "NUWAX_PROVISION_SEC2_DEV_MODE", "NUWAX_PROVISION_RUNTIME_REBOOT_ENABLE", "NUWAX_OTA_ENABLE",
        "NUWAX_OTA_TOTAL_TIMEOUT_MS", "NUWAX_OTA_BOOT_VALIDATION_TIMEOUT_MS", "BOOTLOADER_APP_ROLLBACK_ENABLE",
        "NUWAX_MUSIC_PLAYBACK_ENABLE", "NUWAX_MUSIC_URL_TEST_ENABLE", "ESP_COREDUMP_ENABLE_TO_NONE",
        "NUWAX_SD_CARD_HIGHSPEED_ENABLE", "NUWAX_VOICE_SAMPLE_RATE", "NUWAX_VOICE_CHANNELS",
        "NUWAX_VOICE_FRAME_DURATION_MS", "NUWAX_VOICE_UPLINK_CODEC_PCM16", "NUWAX_VOICE_UPLINK_QUEUE_DEPTH",
        "NUWAX_VOICE_UPLINK_REALTIME_MAX_AGE_MS", "NUWAX_RT_CAPTURE_QUEUE_DEPTH", "NUWAX_RT_CAPTURE_MAX_DECISION_AGE_MS",
        "NUWAX_SPEAKER_BUFFER_BYTES", "NUWAX_SPEAKER_BACKPRESSURE_POLL_MS", "NUWAX_MONITORING_SILENCE_STOP_MS",
        "NUWAX_MONITORING_PREROLL_MS", "NUWAX_MONITORING_SPEECH_CONFIRM_FRAMES", "NUWAX_AUDIO_CLEANER_ENABLE",
        "CJSON_NESTING_LIMIT", "NUWAX_PRODUCTION_SECURITY_REQUIRED", "SECURE_BOOT", "FLASH_ENCRYPTION_ENABLED",
        "NUWAX_OTA_MIN_BATTERY_PERCENT", "NUWAX_HEAD_TOUCH_ENABLE",
        "NUWAX_HEAD_TOUCH_LONG_PRESS_MS", "NUWAX_HEAD_TOUCH_MAX_VALID_DURATION_MS",
        "NUWAX_HEAD_TOUCH_GAP_TOLERANCE_MS", "NUWAX_TOUCH_WAKE_ACTION_ENABLE",
        "NUWAX_TOUCH_INTERRUPT_ACTION_ENABLE",
    }
    config = ROOT / "tmp/sdkconfig.sd-replay-formal"
    values = {}
    if config.exists():
        for line in config.read_text(encoding="utf-8-sig").splitlines():
            match = re.fullmatch(r"CONFIG_(\w+)=(.*)", line)
            disabled = re.fullmatch(r"# CONFIG_(\w+) is not set", line)
            if match and match[1] in allowed:
                values["CONFIG_" + match[1]] = match[2]
            elif disabled and disabled[1] in allowed:
                values["CONFIG_" + disabled[1]] = "n"
    return {
        "source": "tmp/sdkconfig.sd-replay-formal", "available": config.exists(),
        "scope": "Exact allowlist, credential values excluded. Inspect built sdkconfig.h for an actual artifact.",
        "values": dict(sorted(values.items()))}


def config_drift(saved, current):
    problems = []
    if not current.get("available"):
        problems.append("Local formal config unavailable; saved values were not reverified")
    if saved.get("available") != current.get("available"):
        problems.append("Formal config availability changed")
    old, new = saved.get("values", {}), current.get("values", {})
    for key in sorted(set(old) | set(new)):
        if old.get(key) != new.get(key):
            problems.append("Formal config changed; review prose: " + key)
    return problems


def combine():
    content = ["# Nuwax AI Partner 工程开发学习手册：连续阅读版\n",
               "由各章节自动合成，基线与导航见 [README](README.md)。修改分章正文后运行维护工具重新生成。\n"]
    for chapter in CHAPTERS:
        content.extend(["\n---\n", chapter.read_text(encoding="utf-8").rstrip() + "\n"])
    (BOOK / "全册.md").write_text("\n".join(content), encoding="utf-8")


def validate():
    errors, warnings = [], []
    pages = sorted(BOOK.glob("*.md"))
    refs = 0
    for page in pages:
        data = page.read_text(encoding="utf-8")
        if not data.startswith("# "):
            errors.append(f"Missing title: {page.name}")
        if len(re.findall(r"^```", data, flags=re.M)) % 2:
            errors.append(f"Unclosed code fence: {page.name}")
        if "\ufffd" in data:
            errors.append(f"Replacement character: {page.name}")
        if re.search(r"TODO|TBD|待补充正文|此处省略", prose(data)):
            errors.append(f"Unfinished placeholder: {page.name}")
        for raw, target in local_links(page):
            refs += 1
            if not target.exists():
                errors.append(f"Missing target in {page.name}: {raw}")
        for block in re.findall(r"^```json\s*\n(.*?)^```", data, flags=re.M | re.S):
            try:
                json.loads(block)
            except ValueError as exc:
                errors.append(f"Invalid JSON example in {page.name}: {exc}")
    expected = [f"{n:02d}" for n in range(len(CHAPTERS))]
    if [p.name[:2] for p in CHAPTERS] != expected:
        errors.append("Chapter numbering must be unique, contiguous and begin at 00")
    listed = {p.name for _, p in local_links(BOOK / "README.md") if p in CHAPTERS}
    if not CHAPTERS or listed != {p.name for p in CHAPTERS}:
        errors.append("README must link every numbered chapter")
    manifest_path = EVIDENCE / "source-manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8")) if manifest_path.exists() else {}
    if not manifest.get("files"):
        errors.append("Source evidence missing or empty")
    for item in manifest.get("files", []):
        p = ROOT / item["path"]
        if not p.exists() or digest(p) != item["sha256"]:
            warnings.append("Source changed; manually review related prose: " + item["path"])
    snapshot_path = EVIDENCE / "config-snapshot.json"
    if not snapshot_path.exists():
        errors.append("Config evidence missing")
    else:
        warnings.extend(config_drift(json.loads(snapshot_path.read_text(encoding="utf-8")), config_snapshot()))
    examples = []
    for script, options in [("audio_budget.py", ["--self-test"]),
                            ("inspect_na_frame.py", ["--self-test"]), ("owner_epoch.py", [])]:
        result = subprocess.run([sys.executable, str(BOOK / "examples" / script), *options],
                                text=True, capture_output=True)
        examples.append({"script": script, "exit_code": result.returncode,
                         "stdout": result.stdout.strip(), "stderr": result.stderr.strip()})
        if result.returncode:
            errors.append("Teaching example failed: " + script)
    unique = [BOOK / "README.md", *CHAPTERS]
    report = {"schema": "nuwax.handbook.validation.v1", "checked_utc": datetime.now(timezone.utc).isoformat(),
        "passed": not errors and not warnings, "chapters": len(CHAPTERS),
        "unique_markdown_pages": len(unique), "generated_reading_copy": "全册.md",
        "unique_prose_characters_including_markdown": sum(len(p.read_text(encoding="utf-8")) for p in unique),
        "unique_han_characters": sum(len(re.findall(r"[\u4e00-\u9fff]", p.read_text(encoding="utf-8"))) for p in unique),
        "local_link_occurrences_checked_including_compilation": refs,
        "source_files_fingerprinted": len(manifest.get("files", [])),
        "config_allowlist_compared": snapshot_path.exists(),
        "examples": examples, "errors": errors, "warnings": warnings,
        "limits": ["Local link targets checked, anchor semantics and remote link health not automatically validated.",
                   "Markdown fences/JSON/encoding checked; no rendered PDF or visual layout certification.",
                   "Examples are teaching checks, not firmware regression tests or real hardware measurements.",
                   "Technical claims require manual source review; hashes alone do not prove correctness."]}
    write_json(EVIDENCE / "document-validation.json", report)
    return report


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--refresh-evidence", action="store_true")
    parser.add_argument("--review-date", type=date.fromisoformat,
                        help="Human review date YYYY-MM-DD in the user's timezone; required when refreshing")
    args = parser.parse_args()
    EVIDENCE.mkdir(exist_ok=True)
    if args.refresh_evidence:
        if args.review_date is None:
            parser.error("--refresh-evidence requires --review-date YYYY-MM-DD after manual review")
        refresh_evidence(args.review_date)
    # Create the report target before link validation on first use.
    report_path = EVIDENCE / "document-validation.json"
    if not report_path.exists():
        write_json(report_path, {"status": "validation in progress"})
    combine()
    report = validate()
    print(json.dumps({k: report[k] for k in ("passed", "chapters", "unique_han_characters",
          "source_files_fingerprinted", "errors", "warnings")}, ensure_ascii=False, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
