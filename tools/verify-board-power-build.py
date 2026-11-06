"""Check that the board-power fixes are linked into the actual firmware."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("build", type=Path)
p.add_argument("--nm", default="C:/Espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin/xtensa-esp32s3-elf-nm.exe")
a = p.parse_args()
b = a.build.resolve()
elf = b / "NuwaxAIPartner.elf"
binary = (b / "NuwaxAIPartner.bin").read_bytes()
elf_sha = hashlib.sha256(elf.read_bytes()).hexdigest()
assert binary[176:208].hex() == elf_sha, "BIN/ELF identity mismatch"
symbols = subprocess.check_output([a.nm, "--defined-only", str(elf)], text=True)
names = {line.split()[-1] for line in symbols.splitlines() if line.split()}
required = ["bq27220_get_status_partial", "bq27220_revalidate_profile",
            "nuwax_power_ctrl_prepare_shutdown", "nuwax_power_ctrl_shutdown_preparing",
            "nuwax_offline_audio_prepare_shutdown", "nuwax_i2c_bus_get_stats",
            "nuwax_led_status_set_charging", "nuwax_led_start_breath"]
for name in required:
    assert name in names, f"Board fix not linked: {name}"
for marker in [b"profile revalidation attempt=", b"contact=continuous", b"I2C totals transactions=",
               b"revision=charge-config-v1", b"charge diag reason=%s",
               b"revision=charge-history-v1", b"charge history row=",
               b"charging indication revision=measured-current-v1",
               b"revision=static-level-v1 off=high on=low",
               b"output check state=%d gpio=%d level=%d expected_static=%d",
               b"waiting for physical power recovery:",
               b"OTA power admission revision=percent-v1 min_percent=%d profile_required=0 hard_voltage_guard=1"]:
    assert marker in binary, f"Missing integration marker: {marker!r}"
assert b"product control ready qualification_ms=%u gap_tolerance_ms=" not in binary
config = json.loads((b / "config/sdkconfig.json").read_text(encoding="utf-8"))
assert config["NUWAX_OTA_MIN_BATTERY_PERCENT"] == 5, "OTA minimum percentage must be 5 inclusive"
result = dict(revision="board-power-boundaries-v1", elfSha256=elf_sha,
              binSha256=hashlib.sha256(binary).hexdigest(), requiredSymbols=required,
              integrationMarkersPresent=True, hardwareValidated=False,
              criticalVoltageAutoShutdownEnabled=False,
              otaMinimumPercent=5, otaProfileRequired=False,
              otaHardVoltageGuard=True)
(b / "board-power-verification.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
print("PASS: board power fixes linked, BIN/ELF match; ELF SHA256=" + elf_sha)
