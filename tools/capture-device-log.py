"""Read-only ESP32-S3 serial capture, split by observed device boots.

The serial open sequence matches the installed ESP-IDF monitor's --no-reset
path. This tool never transmits console input, reset commands or serial breaks.
Raw .log files contain only received bytes; the JSONL journal holds PC times.
"""

import argparse
import codecs
from datetime import datetime
import json
import os
from pathlib import Path
import re
import signal
import sys
import time


ANSI = re.compile(rb"\x1b\[[0-?]*[ -/]*[@-~]")
UPTIME = re.compile(rb"^[IWEVD] \((\d+)\) ")
BOOT_MARKERS = (
    (re.compile(rb"^ESP-ROM:esp32s3\b", re.I), "rom", 1),
    (re.compile(rb"^rst:0x[0-9a-f]+.*\bboot:", re.I), "reset_reason", 2),
    (re.compile(rb"^[IWE] \(\d+\) boot: ESP-IDF .*2nd stage bootloader"), "bootloader", 3),
    (re.compile(rb"^[IWE] \(\d+\) cpu_start: (?:Multicore|Unicore) app"), "app_cpu_start", 4),
    (re.compile(rb"^[IWE] \(\d+\) app_init: Project name:"), "app_information", 5),
)
MAX_RECORD = 16384
MAX_CANDIDATE = 65536
UPTIME_WRAP = 1 << 32


def now():
    return datetime.now().astimezone()


def iso(value):
    return value.isoformat(timespec="milliseconds")


def create_unique(directory, stem, suffix):
    for attempt in range(10000):
        extra = f"_{attempt}" if attempt else ""
        path = directory / f"{stem}{extra}{suffix}"
        try:
            return path, path.open("xb")
        except FileExistsError:
            continue
    raise OSError(f"Cannot allocate a unique log filename in {directory}")


def notice(message):
    print(f"[capture] {message}", file=sys.stderr, flush=True)


class RunRecorder:
    """Bounded line parser; reboot evidence is kept separate from raw bytes."""

    def __init__(self, directory, port, baud, announce=notice):
        self.directory = Path(directory).resolve()
        self.directory.mkdir(parents=True, exist_ok=True)
        self.port = re.sub(r"[^a-zA-Z0-9_-]", "_", port)
        self.announce = announce
        stamp = now().strftime("%Y%m%d_%H%M%S_%f")
        self.journal_path, self.journal = create_unique(
            self.directory, f"capture_{stamp}_{self.port}", ".events.jsonl")
        self.files = []
        self.file = None
        self.path = None
        self.buffer = bytearray()
        self.buffer_time = None
        self.buffer_last_time = None
        self.continuation = False
        self.candidate = []
        self.candidate_hits = 0
        self.candidate_bytes = 0
        self.stage = 0
        self.runtime_started = False
        self.uptime_max = None
        self.offset = 0
        self.range_start = 0
        self.range_first = None
        self.last_received = None
        self.closed = False
        self.event("capture_start", port=port, baud=baud, reset=False,
                   raw_format="unmodified serial bytes", pid=os.getpid())
        self.sync()
        self.announce(f"Journal: {self.journal_path}")

    def event(self, kind, at=None, **fields):
        row = {"time": iso(at or now()), "event": kind, **fields}
        self.journal.write((json.dumps(row, ensure_ascii=False) + "\n").encode("utf-8"))

    def _start(self, reason, at):
        stamp = at.strftime("%Y%m%d_%H%M%S_%f")
        label = "attached" if reason == "attached_mid_run" else "boot"
        stem = f"{stamp}_{self.port}_run{len(self.files) + 1:03d}_{label}"
        self.path, self.file = create_unique(self.directory, stem, ".log")
        self.files.append(self.path)
        self.offset = self.range_start = 0
        self.range_first = self.last_received = None
        self.stage = 0
        self.runtime_started = False
        self.uptime_max = None
        self.event("run_start", at, file=self.path.name, reason=reason,
                   startup_observed=reason not in ("attached_mid_run", "probable_restart_uptime"))
        self.announce(f"Log: {self.path} ({reason})")

    def _write(self, data, at, uptime=None, last_at=None):
        if self.file is None:
            self._start("attached_mid_run", at)
        self.file.write(data)
        self.offset += len(data)
        if self.range_first is None:
            self.range_first = at
        self.last_received = last_at or at
        if uptime is not None:
            self.uptime_max = max(self.uptime_max or 0, uptime)

    def _checkpoint(self):
        if self.file is not None:
            self.file.flush()
            os.fsync(self.file.fileno())
            if self.offset != self.range_start:
                self.event("received", file=self.path.name,
                           offset_start=self.range_start, offset_end=self.offset,
                           first_received_at=iso(self.range_first),
                           last_received_at=iso(self.last_received))
                self.range_start = self.offset
                self.range_first = None

    def _finish(self, reason):
        if self.file is not None:
            self._checkpoint()
            self.event("run_closed", file=self.path.name, reason=reason,
                       bytes=self.offset, last_received_at=iso(self.last_received),
                       max_observed_uptime_ms=self.uptime_max)
            self.file.close()
            self.file = None

    def _rotate(self, reason, at):
        self._finish("next_boot_observed" if reason != "probable_restart_uptime"
                     else "probable_restart_uptime")
        self._start(reason, at)

    def _flush_candidate(self):
        pending, self.candidate = self.candidate, []
        self.candidate_hits = self.candidate_bytes = 0
        for data, at, uptime, last_at in pending:
            self._write(data, at, uptime, last_at)

    def _is_rollback(self, uptime):
        # Do not split for normal multicore log reordering. If boot banners were
        # lost during USB enumeration, require THREE early-uptime log records.
        return (uptime is not None and self.uptime_max is not None
                and uptime <= 60000 and self.uptime_max - uptime >= 10000)

    def _record(self, data, at, continuation=False, last_at=None):
        clean = ANSI.sub(b"", data).lstrip(b"\r\n") if not continuation else b""
        match = UPTIME.match(clean)
        uptime = int(match[1]) if match else None
        marker = next(((name, rank) for regex, name, rank in BOOT_MARKERS
                       if regex.match(clean)), None)
        if marker:
            reason, rank = marker
            if self.candidate:
                self._rotate(reason, self.candidate[0][1])
                self._flush_candidate()
            elif self.file is None:
                self._start(reason, at)
            elif self.stage == 0 or rank <= self.stage or self.runtime_started:
                self._rotate(reason, at)
            self.stage = rank
            self._write(data, at, uptime, last_at)
            return
        if (uptime is not None and self.uptime_max is not None
                and UPTIME_WRAP - 120000 <= self.uptime_max < UPTIME_WRAP
                and uptime < 120000):
            self._flush_candidate()
            self.event("uptime_counter_wrap", at, file=self.path.name)
            self.uptime_max = uptime
        rollback = self._is_rollback(uptime)
        if rollback or (self.candidate and uptime is None):
            self.candidate.append((data, at, uptime, last_at))
            self.candidate_bytes += len(data)
            self.candidate_hits += int(rollback)
            if self.candidate_hits >= 3:
                self._rotate("probable_restart_uptime", self.candidate[0][1])
                # A later bootloader/app banner belongs to this inferred boot.
                self.stage = 2
                self._flush_candidate()
            elif self.candidate_bytes >= MAX_CANDIDATE:
                self._flush_candidate()
            return
        self._flush_candidate()
        self._write(data, at, uptime, last_at)
        if (b"main_task: Calling app_main()" in clean or b"NuwaxPartner6:" in clean
                or (match and (b" nuwax_" in clean or b" offline_audio:" in clean))):
            self.runtime_started = True

    def feed(self, data, at=None):
        at = at or now()
        if not self.buffer:
            self.buffer_time = at
        self.buffer_last_time = at
        self.buffer.extend(data)
        while self.buffer:
            end = self.buffer.find(b"\n", 0, MAX_RECORD)
            if end < 0 and len(self.buffer) < MAX_RECORD:
                break
            count = end + 1 if end >= 0 else MAX_RECORD
            record = bytes(self.buffer[:count])
            del self.buffer[:count]
            self._record(record, self.buffer_time, self.continuation, at)
            self.continuation = end < 0
            self.buffer_time = at if self.buffer else None

    def flush_input(self):
        if self.buffer:
            self._record(bytes(self.buffer), self.buffer_time, self.continuation,
                         self.buffer_last_time)
            self.buffer.clear()
        self.buffer_time = None
        self.buffer_last_time = None
        self.continuation = False
        self._flush_candidate()

    def sync(self):
        # A lone reordered record must not remain in RAM forever if the device
        # then goes quiet. Slow/sparse evidence stays in the current run.
        if self.candidate and (now() - self.candidate[0][1]).total_seconds() >= 5:
            self._flush_candidate()
        self._checkpoint()
        self.journal.flush()
        os.fsync(self.journal.fileno())

    def close(self, reason="capture_stopped"):
        if self.closed:
            return
        try:
            self.flush_input()
            self._finish(reason)
            self.event("capture_stopped", reason=reason)
            self.sync()
        finally:
            self.closed = True
            if self.file is not None:
                self.file.close()
            self.journal.close()


def open_no_reset(port):
    """Mirror ESP-IDF SerialReader.open_serial(reset=False), including usbser.

    In esp_idf_monitor.base.constants LOW=True, HIGH=False (active-low lines).
    Preserve the working monitor's order, not pySerial's default auto-open.
    No hard(), reset_input_buffer(), break or serial.write() calls are made.
    """
    def set_rts(level):
        port.setRTS(level)
        port.setDTR(port.dtr)  # Same Windows usbser.sys workaround as IDF.

    set_rts(True)
    port.setDTR(True)
    port.open()
    set_rts(False)
    port.setDTR(False)


def capture(port, recorder, retry_seconds=2.0, quiet=False, stop=lambda: False,
            sleep=time.sleep, monotonic=time.monotonic):
    decoder = codecs.getincrementaldecoder("utf-8")("replace")
    next_sync = monotonic() + 1.0
    next_warning = 0.0
    connection_count = 0
    try:
        while not stop():
            if not port.is_open:
                try:
                    open_no_reset(port)
                except OSError as exc:
                    port.close()
                    if monotonic() >= next_warning:
                        notice(f"Waiting for {port.port}: {exc}. Close other serial monitors if busy.")
                        recorder.event("connect_failed", detail=str(exc))
                        recorder.sync()
                        next_warning = monotonic() + 15.0
                    sleep(retry_seconds)
                    continue
                connection_count += 1
                recorder.event("connected", connection=connection_count, port=port.port)
                recorder.sync()
                notice(f"Connected to {port.port}; read-only, no reset. Ctrl+C stops capture.")
            try:
                data = port.read(min(port.in_waiting or 1, 65536))
            except OSError as exc:
                recorder.flush_input()
                recorder.event("disconnected", detail=str(exc))
                recorder.sync()
                port.close()
                decoder.reset()
                notice(f"Serial disconnected; waiting for {port.port} to return.")
                sleep(retry_seconds)
                continue
            if data:
                recorder.feed(data)
                if not quiet:
                    try:
                        sys.stdout.write(decoder.decode(data))
                        sys.stdout.flush()
                    except (OSError, UnicodeError):
                        quiet = True
                        notice("Console output unavailable; file capture continues.")
            if monotonic() >= next_sync:
                recorder.sync()
                next_sync = monotonic() + 1.0
    finally:
        port.close()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM6")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--output-dir", type=Path,
                        default=Path(__file__).resolve().parents[1] / "runtime-logs" / "device-serial")
    parser.add_argument("--retry-seconds", type=float, default=2.0)
    parser.add_argument("--quiet", action="store_true", help="Save logs without echoing device output")
    args = parser.parse_args(argv)
    if args.baud <= 0 or not 0.1 <= args.retry_seconds <= 60:
        parser.error("baud must be positive; retry-seconds must be between 0.1 and 60")
    try:
        import serial
    except ImportError:
        parser.error("pySerial is missing. Run with the ESP-IDF Python interpreter.")
    recorder = None
    result = 0
    reason = "capture_stopped"
    stop_requested = [False]
    previous_handlers = {}

    def request_stop(signum, frame):
        # Finish processing the bytes already read instead of interrupting a
        # file write halfway through. No keys are forwarded to the device.
        stop_requested[0] = True

    try:
        for sig in (signal.SIGINT, getattr(signal, "SIGBREAK", signal.SIGINT)):
            if sig not in previous_handlers:
                previous_handlers[sig] = signal.signal(sig, request_stop)
        recorder = RunRecorder(args.output_dir, args.port, args.baud)
        # Construct CLOSED so pySerial cannot apply its default control lines
        # before open_no_reset has prepared the monitor-compatible state.
        port = serial.Serial(port=None, baudrate=args.baud, timeout=0.25,
                             xonxoff=False, rtscts=False, dsrdtr=False)
        port.port = args.port
        capture(port, recorder, args.retry_seconds, args.quiet,
                stop=lambda: stop_requested[0])
    except KeyboardInterrupt:
        notice("Capture stopped; saving the final bytes.")
    except OSError as exc:
        result, reason = 1, "capture_error"
        notice(f"Capture failed: {exc}")
    finally:
        if recorder is not None:
            try:
                recorder.close(reason)
            except OSError as exc:
                result = 1
                notice(f"Cannot finish saving logs: {exc}")
        for sig, handler in previous_handlers.items():
            signal.signal(sig, handler)
    return result


if __name__ == "__main__":
    sys.exit(main())
