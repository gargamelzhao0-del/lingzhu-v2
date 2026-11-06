"""Positive/negative fixtures for the actual log gate and PowerShell exit codes."""
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('gate', root/'tools/check-device-longrun-gate.py')
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)
out = root/'build-host-checks/w01/longrun'
out.mkdir(parents=True, exist_ok=True)


def line(time, message, tag='nuwax_wake'):
    return f'I ({time}) {tag}: {message}'


def healthy(end=60000):
    result = ['ESP-ROM:esp32s3', 'rst:0x1 (POWERON),boot:0x8',
              line(0, 'wake detector task started')]
    for time in range(0, end+1, 10000):
        result += [line(time, 'heap checkpoint=periodic internal_min=20000 dma_largest=10000', 'nuwax_diag'),
                   line(time, 'wake input health frames=100 partial_reads=0 timeouts=0 input_gaps=0'),
                   line(time, 'wake capture health queue_drops=0 invariant_faults=0 partial_reads=0 timeouts=0')]
    return result


cases = {}
def case(name, lines, expected, code=None, **kwargs):
    result = gate.analyze(lines, minimum_duration=60, **kwargs)
    assert result['status'] == expected, (name, result)
    if code:
        assert any(i['code'] == code for i in result['issues']), (name, result)
    (out/(name+'.log')).write_text('\n'.join(lines)+'\n', encoding='utf-8')
    (out/(name+'.json')).write_text(json.dumps(result, indent=2), encoding='utf-8')
    cases[name] = dict(status=expected, expected_code=code)
    return result


good = healthy()
case('healthy', good, 'PASS')
case('empty', [], 'INCOMPLETE', 'duration_short')
case('short', healthy(20000), 'INCOMPLETE', 'duration_short')
case('old-false-pass', ['internal_min=20000 dma_largest=10000',
     'partial_reads=0 timeouts=0 input_gaps=0', 'dropped_frames=99',
     'queue_drops=12 partial_reads=5 timeouts=5 input_gaps=3'], 'FAIL', 'unscoped_loss_counter')
for field in sorted(gate.HARD):
    case('loss-'+field, good+[line(60010, 'wake input health partial_reads=0 timeouts=0 input_gaps=0 '+field+'=3')],
         'FAIL' if field != 'input_gaps' else 'FAIL', 'loss_counter')
for field in sorted(gate.SOFT):
    case('late-'+field, good+[line(60010, 'wake input health frames=200 partial_reads=2 timeouts=2 input_gaps=0')],
         'INCOMPLETE', 'read_anomaly_needs_context')
repeated = case('counter-delta', good+[
    line(60010, 'wake capture health queue_drops=3 invariant_faults=0 partial_reads=0 timeouts=0'),
    line(60020, 'wake capture health queue_drops=3 invariant_faults=0 partial_reads=0 timeouts=0'),
    line(60030, 'wake capture health queue_drops=5 invariant_faults=0 partial_reads=0 timeouts=0')], 'FAIL')
assert [i['delta'] for i in repeated['issues'] if i['code']=='loss_counter'] == [3, 2]
case('counter-reset', good+[line(60010, 'wake input health partial_reads=2 timeouts=0 input_gaps=0'),
     line(60020, 'wake input health partial_reads=0 timeouts=0 input_gaps=0')], 'INCOMPLETE', 'counter_reset')
case('mode-stop-start', good+[line(60001, 'wake input final partial_reads=0 timeouts=0 input_gaps=0'),
     line(60002, 'wake detector task started'),
     line(60003, 'wake input health partial_reads=0 timeouts=0 input_gaps=0')], 'PASS')
case('stop-timeout-needs-review', good+[line(60001, 'wake input final partial_reads=1 timeouts=1 input_gaps=0')],
     'INCOMPLETE', 'read_anomaly_needs_context')
case('module-isolation', good+[line(60001, 'meeting uplink stats round=1 dropped=0'),
     line(60002, 'monitoring stream closed session=x dropped=3')], 'FAIL')
case('log-gap', good+[line(300000, 'heartbeat', 'main')], 'INCOMPLETE', 'log_gap')
case('memory-missing', [s for s in good if 'heap checkpoint=' not in s], 'INCOMPLETE', 'missing_memory_coverage')
case('wake-missing', [s for s in good if 'wake input health' not in s], 'INCOMPLETE', 'missing_wake_coverage')
case('memory-budget', good+[line(60001, 'heap checkpoint=end internal_min=1200 dma_largest=10000','nuwax_diag')],
     'FAIL', 'memory_below_budget')
case('reboot-banner', good+['ESP-ROM:esp32s3', 'rst:0xc (SW_CPU_RESET),boot:0x8', line(20,'startup')],
     'FAIL', 'restart_or_clock_reset')
case('reboot-uptime', good+[line(10,'startup')], 'FAIL', 'restart_or_clock_reset')
case('interleaved-time', good+[line(59980,'heartbeat','main')], 'PASS')
for name, text in [('panic','Guru Meditation Error: Core 0 panic\'ed'),
                   ('wdt','E (60001) task_wdt: Task watchdog got triggered.'),
                   ('brownout','Brownout detector was triggered'),
                   ('assert','assert failed: test.c:123'), ('abort','abort() was called at PC 0x123')]:
    case(name, good+[text], 'FAIL', 'panic_or_watchdog')
case('no-speech', good+[line(60001,'RX type=error reason=no_speech','nuwax_session')], 'PASS')
case('transcript-is-not-diagnostics', good+[line(60001,'ASR text: Guru Meditation Error queue_drops=20','nuwax_session')], 'PASS')
case('unknown-error', good+['E (60001) nuwax_ws: send failed'], 'INCOMPLETE', 'error_log_needs_context')
case('malformed-counter', good+[line(60001,'wake capture health queue_drops=bad')], 'INCOMPLETE', 'malformed_counter')
case('duplicate-counter', good+[line(60001,'wake capture health queue_drops=3 queue_drops=0')], 'INCOMPLETE', 'malformed_counter')
case('truncated-health', good+[line(60001,'wake input health frames=100')], 'INCOMPLETE', 'truncated_wake_health')
case('damaged-encoding', good+['\ufffdtruncated UART bytes'], 'INCOMPLETE', 'damaged_encoding')
assert gate.analyze(good)['status']=='INCOMPLETE', 'Default must require 72 hours'
full_run=[]
for time in range(0,259200001,60000):
    full_run.extend([line(time,'heap checkpoint=periodic internal_min=20000 dma_largest=10000','nuwax_diag'),
                     line(time,'wake input health partial_reads=0 timeouts=0 input_gaps=0')])
assert gate.analyze(full_run)['status']=='PASS', 'A fully covered synthetic 72-hour run must pass'
# Test sparse metrics despite otherwise continuous logging.
sparse = healthy(300000)
sparse = [s for s in sparse if 'heap checkpoint=' not in s or s.startswith('I (0)') or s.startswith('I (10000)')]
case('sparse-memory', sparse, 'INCOMPLETE', 'memory_coverage_gap')
# Exercise actual CLI + PS wrapper with BOM/UTF-16 inputs and JSON reports.
shell = shutil.which('pwsh') or shutil.which('powershell')
assert shell
for name, expected in [('healthy',0), ('old-false-pass',2), ('short',3)]:
    log = out/(name+'.log'); report = out/(name+'-cli.json')
    if name == 'healthy':
        log = out/'healthy-utf16.log';log.write_text('\n'.join(good),encoding='utf-16')
    result = subprocess.run([shell,'-NoProfile','-ExecutionPolicy','Bypass','-File',
        str(root/'tools/check-device-longrun-gate.ps1'),'-LogPath',str(log),
        '-MinimumDurationSeconds','60','-ReportPath',str(report),'-Python',sys.executable],
        capture_output=True,text=True)
    assert result.returncode==expected,(name,result.stdout,result.stderr)
    assert json.loads(report.read_text())['status']=={0:'PASS',2:'FAIL',3:'INCOMPLETE'}[expected]
missing_report=out/'missing-input.json';missing_report.write_text('{"status":"PASS"}',encoding='utf-8')
result=subprocess.run([sys.executable,str(root/'tools/check-device-longrun-gate.py'),
                       str(out/'does-not-exist.log'),'--report',str(missing_report)],capture_output=True)
assert result.returncode==3 and json.loads(missing_report.read_text())['status']=='INCOMPLETE'
log=out/'healthy.log';before=log.read_bytes()
result=subprocess.run([sys.executable,str(root/'tools/check-device-longrun-gate.py'),str(log),'--report',str(log)],capture_output=True)
assert result.returncode!=0 and log.read_bytes()==before
(out/'results.json').write_text(json.dumps(cases,indent=2),encoding='utf-8')
print(f'PASS: {len(cases)} log fixtures; counter deltas, lifecycle resets, coverage, no_speech and CLI exit codes 0/2/3')
