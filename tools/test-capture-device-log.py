"""Host regression checks. Never opens a real serial port."""

from datetime import datetime, timedelta, timezone
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location(
    "capture_device_log", Path(__file__).with_name("capture-device-log.py"))
capture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)

BOOT = (b"ESP-ROM:esp32s3-20210327\r\n"
        b"rst:0x1 (POWERON),boot:0x8 (SPI_FAST_FLASH_BOOT)\r\n"
        b"I (24) boot: ESP-IDF v6.0 2nd stage bootloader\r\n"
        b"I (811) cpu_start: Multicore app\r\n"
        b"I (1106) app_init: Project name: NuwaxAIPartner\r\n"
        b"I (1107) app_init: App version: 7.0.188\r\n"
        b"I (1129) main_task: Calling app_main()\r\n")
BOOTLOADER = b"I (24) boot: ESP-IDF v6.0 2nd stage bootloader\r\n"


class FakeSerial:
    """Replay bytes / I/O failures, recording every control-line operation."""

    def __init__(self, reads=(), open_errors=0):
        self.port = "FAKE-COM6"
        self.is_open = False
        self.dtr = True
        self.reads = list(reads)
        self.open_errors = open_errors
        self.operations = []

    def setRTS(self, value):
        self.operations.append(("rts", value, self.is_open))

    def setDTR(self, value):
        self.operations.append(("dtr", value, self.is_open))
        self.dtr = value

    def open(self):
        self.operations.append(("open",))
        if self.open_errors:
            self.open_errors -= 1
            raise OSError("port unavailable")
        self.is_open = True

    def close(self):
        self.operations.append(("close",))
        self.is_open = False

    @property
    def in_waiting(self):
        return 65536

    def read(self, count):
        item = self.reads.pop(0)
        if isinstance(item, BaseException):
            raise item
        return item

    def write(self, data):
        raise AssertionError("Must never transmit to the device")

    def reset_input_buffer(self):
        raise AssertionError("Must never discard received data")

    def send_break(self, *args):
        raise AssertionError("Must never reset the device")


class CaptureTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="nuwax-log-test-")
        self.directory = Path(self.tmp.name)
        self.recorders = []

    def tearDown(self):
        for recorder in self.recorders:
            recorder.close()
        self.tmp.cleanup()

    def recorder(self):
        recorder = capture.RunRecorder(self.directory, "COM6", 115200, announce=lambda _: None)
        self.recorders.append(recorder)
        return recorder

    def contents(self, recorder):
        recorder.close()
        return [p.read_bytes() for p in recorder.files]

    def events(self, recorder):
        recorder.sync() if not recorder.closed else None
        return [json.loads(line) for line in recorder.journal_path.read_text(encoding="utf-8").splitlines()]

    def test_boot_stages_are_one_file(self):
        recorder = self.recorder()
        recorder.feed(BOOT)
        self.assertEqual(self.contents(recorder), [BOOT])

    def test_one_byte_reads_keep_utf8_ansi_and_markers(self):
        data = b"\x1b[0;32m" + BOOT + "I (2000) text: 中文日志\r\n".encode() + b"\xff\xfe\n"
        recorder = self.recorder()
        for byte in data:
            recorder.feed(bytes([byte]))
        self.assertEqual(self.contents(recorder), [data])

    def test_rapid_boot_loop_splits_each_boot(self):
        banner = b"ESP-ROM:esp32s3-20210327\n"
        recorder = self.recorder()
        recorder.feed(banner * 4)
        self.assertEqual(self.contents(recorder), [banner] * 4)

    def test_bootloader_only_restart(self):
        run = BOOTLOADER + b"I (4000) nuwax_wake: ready\n"
        recorder = self.recorder()
        recorder.feed(run + run)
        self.assertEqual(self.contents(recorder), [run, run])

    def test_app_only_restart(self):
        run = b"I (1106) app_init: Project name: NuwaxAIPartner\nI (1200) NuwaxPartner6: running\n"
        recorder = self.recorder()
        recorder.feed(run + run)
        self.assertEqual(self.contents(recorder), [run, run])

    def test_slow_boot_stages_stay_together(self):
        data = BOOT.replace(b"(811)", b"(30000)").replace(b"(1106)", b"(30100)")
        recorder = self.recorder()
        recorder.feed(data)
        self.assertEqual(self.contents(recorder), [data])

    def test_running_after_rom_then_lost_rom_on_restart(self):
        first = b"ESP-ROM:esp32s3\nI (5000) nuwax_wake: ready\n"
        recorder = self.recorder()
        recorder.feed(first + BOOTLOADER)
        self.assertEqual(self.contents(recorder), [first, BOOTLOADER])

    def test_midrun_attach_is_labelled_partial(self):
        first = b"I (999000) offline_audio: progress\n"
        recorder = self.recorder()
        recorder.feed(first + BOOT)
        self.assertEqual(self.contents(recorder), [first, BOOT])
        starts = [e for e in self.events(recorder) if e["event"] == "run_start"]
        self.assertFalse(starts[0]["startup_observed"])
        self.assertIn("attached", starts[0]["file"])
        self.assertTrue(starts[1]["startup_observed"])

    def test_silence_does_not_split(self):
        start = datetime(2026, 10, 27, tzinfo=timezone.utc)
        recorder = self.recorder()
        recorder.feed(b"I (100000) nuwax_wake: ready\n", start)
        recorder.feed(b"I (100001) nuwax_wake: ready\n", start + timedelta(days=2))
        self.assertEqual(len(self.contents(recorder)), 1)

    def test_multicore_ordering_and_one_large_regression_do_not_split(self):
        data = (b"I (100000) task: a\nI (99990) task: b\n"
                b"I (2000) task: late\nI (100001) task: c\n")
        recorder = self.recorder()
        recorder.feed(data)
        self.assertEqual(self.contents(recorder), [data])

    def test_three_early_uptimes_infer_restart(self):
        first = b"I (900000) nuwax_wake: ready\n"
        second = b"I (2000) nuwax_wake: start\nI (2500) task: b\nI (3000) task: c\n"
        recorder = self.recorder()
        recorder.feed(first + second)
        self.assertEqual(self.contents(recorder), [first, second])
        self.assertIn("probable_restart_uptime", [e.get("reason") for e in self.events(recorder)])

    def test_candidate_followed_by_boot_marker_has_no_lost_lines(self):
        first = b"I (900000) nuwax_wake: ready\n"
        second = b"I (500) esp_image: load\nI (1106) app_init: Project name: NuwaxAIPartner\n"
        recorder = self.recorder()
        recorder.feed(first + second)
        self.assertEqual(self.contents(recorder), [first, second])

    def test_uptime_wrap_is_not_restart(self):
        data = f"I ({capture.UPTIME_WRAP - 2}) task: a\nI (0) task: b\nI (20) task: c\nI (3000) task: d\n".encode()
        recorder = self.recorder()
        recorder.feed(data)
        self.assertEqual(self.contents(recorder), [data])
        self.assertIn("uptime_counter_wrap", [e["event"] for e in self.events(recorder)])

    def test_panic_and_core_dump_stay_in_crashed_run(self):
        crash = BOOT + b"Guru Meditation Error!\nBacktrace: 0x123\n==== CORE DUMP START ====\nAAA=\n==== CORE DUMP END ====\nRestarting now.\n"
        recorder = self.recorder()
        recorder.feed(crash + BOOT)
        self.assertEqual(self.contents(recorder), [crash, BOOT])

    def test_partial_line_saved_on_stop(self):
        data = b"I (123) log: incomplete \xe4\xb8"
        recorder = self.recorder()
        recorder.feed(data)
        self.assertEqual(self.contents(recorder), [data])

    def test_long_unterminated_line_is_bounded_and_preserved(self):
        data = b"x" * (capture.MAX_RECORD * 4 + 7)
        recorder = self.recorder()
        for pos in range(0, len(data), 501):
            recorder.feed(data[pos:pos + 501])
            self.assertLess(len(recorder.buffer), capture.MAX_RECORD)
        self.assertEqual(self.contents(recorder), [data])

    def test_no_false_boot_marker_in_middle_of_oversized_line(self):
        data = b"x" * capture.MAX_RECORD + BOOTLOADER
        recorder = self.recorder()
        recorder.feed(data)
        self.assertEqual(self.contents(recorder), [data])
        starts = [e for e in self.events(recorder) if e["event"] == "run_start"]
        self.assertEqual(starts[0]["reason"], "attached_mid_run")

    def test_rollback_buffer_has_size_and_time_bounds(self):
        recorder = self.recorder()
        data = b"I (900000) task: a\nI (20) task: b\n" + b"x\n" * capture.MAX_CANDIDATE
        recorder.feed(data)
        self.assertLess(recorder.candidate_bytes, capture.MAX_CANDIDATE)
        self.assertEqual(self.contents(recorder), [data])
        recorder = self.recorder()
        recorder.feed(b"I (900000) task: a\nI (20) task: b\n", capture.now() - timedelta(seconds=10))
        recorder.sync()
        self.assertFalse(recorder.candidate)

    def test_same_timestamp_cannot_overwrite_files(self):
        frozen = capture.now()
        with patch.object(capture, "now", return_value=frozen):
            first, second = self.recorder(), self.recorder()
            first.feed(BOOT)
            second.feed(BOOT)
        self.assertNotEqual(first.journal_path, second.journal_path)
        self.assertNotEqual(first.files[0], second.files[0])
        self.assertEqual(self.contents(first), [BOOT])
        self.assertEqual(self.contents(second), [BOOT])

    def test_empty_capture_has_no_empty_run_file(self):
        recorder = self.recorder()
        self.assertEqual(self.contents(recorder), [])

    def test_index_ranges_cover_all_saved_bytes(self):
        recorder = self.recorder()
        recorder.feed(BOOT)
        recorder.sync()
        recorder.feed(b"I (600000) task: finish\n")
        saved = self.contents(recorder)[0]
        ranges = [e for e in self.events(recorder) if e["event"] == "received"]
        self.assertEqual(ranges[0]["offset_start"], 0)
        self.assertEqual(ranges[-1]["offset_end"], len(saved))
        self.assertEqual(ranges[0]["offset_end"], ranges[1]["offset_start"])
        self.assertIn("+", ranges[0]["first_received_at"])

    def test_line_split_between_reads_keeps_both_receive_times(self):
        recorder = self.recorder()
        first = capture.now()
        last = first + timedelta(seconds=1)
        recorder.feed(b"I (30) task: first", first)
        recorder.feed(b" and last\n", last)
        self.contents(recorder)
        receipt = next(e for e in self.events(recorder) if e["event"] == "received")
        self.assertEqual(receipt["first_received_at"], capture.iso(first))
        self.assertEqual(receipt["last_received_at"], capture.iso(last))

    def test_no_reset_sequence_matches_installed_idf_monitor(self):
        from esp_idf_monitor.base.serial_reader import SerialReader
        from esp_idf_monitor.base.reset import Reset
        expected, actual = FakeSerial(), FakeSerial()
        strategy = Reset.__new__(Reset)  # Avoid enumerating ANY real port.
        strategy.serial_instance = expected
        reader = types.SimpleNamespace(serial=expected, reset_strategy=strategy)
        SerialReader.open_serial(reader, reset=False)
        capture.open_no_reset(actual)
        self.assertEqual(actual.operations, expected.operations)
        self.assertFalse(actual.dtr)

    def test_reconnect_keeps_same_run_when_device_did_not_reboot(self):
        first = b"I (500000) nuwax_wake: a\npartial "
        second = b"line\nI (500010) nuwax_wake: b\n"
        port = FakeSerial([first, OSError("USB disconnected"), second])
        recorder = self.recorder()
        with patch.object(capture, "notice"):
            capture.capture(port, recorder, quiet=True, stop=lambda: not port.reads,
                            sleep=lambda _: None)
        self.assertEqual(self.contents(recorder), [first + second])
        self.assertFalse(port.is_open)
        self.assertEqual(sum(e["event"] == "connected" for e in self.events(recorder)), 2)
        self.assertEqual(sum(e["event"] == "disconnected" for e in self.events(recorder)), 1)

    def test_reconnect_with_boot_creates_new_run(self):
        port = FakeSerial([BOOT, OSError("USB disconnected"), BOOT], open_errors=1)
        recorder = self.recorder()
        with patch.object(capture, "notice"):
            capture.capture(port, recorder, quiet=True, stop=lambda: not port.reads,
                            sleep=lambda _: None)
        self.assertEqual(self.contents(recorder), [BOOT, BOOT])
        self.assertEqual(sum(op[0] == "open" for op in port.operations), 3)

    def test_disk_error_is_fatal_not_misreported_as_disconnection(self):
        port = FakeSerial([BOOT])
        recorder = self.recorder()
        with patch.object(recorder, "feed", side_effect=OSError("disk full")):
            with patch.object(capture, "notice"):
                with self.assertRaisesRegex(OSError, "disk full"):
                    capture.capture(port, recorder, quiet=True, stop=lambda: not port.reads)
        self.assertFalse(port.is_open)
        self.assertNotIn("disconnected", [e["event"] for e in self.events(recorder)])

    @unittest.skipUnless(os.name == "nt", "PowerShell launcher is for Windows")
    def test_powershell_launcher_with_fake_serial_and_unicode_output_path(self):
        # The child interpreter imports this fake module before site-packages.
        # It is impossible for this fixture to open a physical COM port.
        fake_modules = self.directory / "fake-modules"
        fake_modules.mkdir()
        first = BOOT + b"I (900000) nuwax_wake: ready\n"
        source = f'''\
import signal
class Serial:
    def __init__(self, port, baudrate, timeout, **kwargs):
        assert port is None and baudrate == 115200 and timeout == 0.25
        assert kwargs == dict(xonxoff=False, rtscts=False, dsrdtr=False)
        self.port = None
        self.is_open = False
        self.dtr = True
        self.in_waiting = 1024
        self.reads = [{first!r}, {BOOT!r}]
    def setRTS(self, value):
        self.rts = value
    def setDTR(self, value):
        self.dtr = value
    def open(self):
        assert self.port == 'COM6' and self.rts and self.dtr
        self.is_open = True
    def close(self):
        self.is_open = False
    def read(self, count):
        assert self.is_open and not self.rts and not self.dtr
        if not self.reads:
            signal.raise_signal(signal.SIGINT)
            return b''
        return self.reads.pop(0)
'''
        (fake_modules / "serial.py").write_text(source, encoding="utf-8")
        output = self.directory / "中文 日志"
        launcher = Path(__file__).with_name("capture-device-log.ps1")
        env = dict(os.environ, PYTHONPATH=str(fake_modules), PYTHONNOUSERSITE="1")
        result = subprocess.run(
            ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(launcher),
             "-PythonExe", sys.executable, "-Port", "COM6", "-OutputDir", str(output), "-Quiet"],
            capture_output=True, env=env, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
        logs = sorted(output.glob("*.log"))
        self.assertEqual([p.read_bytes() for p in logs], [first, BOOT])
        self.assertEqual(len(list(output.glob("*.events.jsonl"))), 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
