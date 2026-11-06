"""Conservative offline log gate. PASS covers stated telemetry criteria only."""
import argparse
import hashlib
import json
import re
from pathlib import Path

ENTRY = re.compile(r'\b([IEWD]) \((\d+)\) ([\w.-]+): (.*)')
FIELD = re.compile(r'(?<![\w])([a-z][a-z0-9_]*)=(-?\d+)(?=$|[\s,;/%])')
PANIC = re.compile(r'Guru Meditation Error|assert failed:|abort\(\) was called|'
                   r'Brownout detector was triggered|Task watchdog got triggered|'
                   r'Interrupt wdt timeout|(?:^|\s)Rebooting\.\.\.', re.I)
HARD = {'queue_full', 'dropped', 'dropped_frames', 'queue_drops', 'sequence_gaps',
        'speaker_underruns', 'write_failures', 'input_gaps', 'invariant_faults',
        'partial_reads_dropped', 'partial_samples_dropped', 'discarded_partial_samples'}
SOFT = {'partial_reads', 'timeouts'}
FAMILIES = ('wake input health', 'wake input final', 'wake capture health',
            'meeting uplink stats', 'meeting capture done', 'monitoring stream closed',
            'monitoring capture phase', 'monitoring capture done',
            'offline SD session closed', 'offline SD writer progress',
            'offline capture reader stopped', 'audio uplink task resources',
            'conversation runtime metrics', 'I2S microphone stream done',
            'acceptance playback capture summary', 'local power capture terminal')


def analyze(lines, minimum_duration=259200, maximum_gap=120,
            minimum_internal=16384, minimum_dma=8192):
    issues, counters, epochs = [], {}, []
    epoch = {'first_ms': None, 'last_ms': None, 'max_gap_ms': 0, 'start_line': 1}
    wake_generation, wake_samples = 0, 0
    wake_last = None
    resources = {name: {'samples': 0, 'minimum': None, 'last_ms': None}
                 for name in ('internal_min', 'dma_largest')}
    counts = {'FAIL': 0, 'INCOMPLETE': 0}

    def issue(kind, code, line, time, **details):
        counts[kind] += 1
        if len(issues) < 500:
            issues.append(dict(kind=kind, code=code, line=line, uptime_ms=time,
                               epoch=len(epochs) if line is not None else None, **details))

    def boundary(line, reason):
        nonlocal epoch, wake_generation, wake_last
        if epoch['first_ms'] is not None:
            issue('FAIL', 'restart_or_clock_reset', line, None, reason=reason)
            epochs.append(epoch)
            epoch = {'first_ms': None, 'last_ms': None, 'max_gap_ms': 0, 'start_line': line}
        wake_generation, wake_last = 0, None
        for resource in resources.values():
            resource['last_ms'] = None

    for number, raw in enumerate(lines, 1):
        if '\ufffd' in raw:
            issue('INCOMPLETE', 'damaged_encoding', number, None)
        match = ENTRY.search(raw)
        # Cloud text previews are not diagnostic evidence.
        preview = bool(match and match[3] in ('nuwax_session', 'session_protocol')
                       and not match[4].startswith(('I2S ', 'heap checkpoint=')))
        if not preview and PANIC.search(raw):
            issue('FAIL', 'panic_or_watchdog', number, None)
        if re.search(r'(?:^|\s)ESP-ROM:|(?:^|\s)rst:0x', raw) and not preview:
            boundary(number, 'boot banner')
        if not match:
            # Legacy snippets can prove loss but cannot prove a healthy run.
            if re.fullmatch(r'[\s\w=,;-]+', raw.strip()):
                for key, value in FIELD.findall(raw):
                    if key in HARD and int(value) > 0:
                        issue('FAIL', 'unscoped_loss_counter', number, None,
                              field=key, value=int(value))
            continue
        level, uptime, tag, message = match[1], int(match[2]), match[3], match[4]
        previous_time = epoch['last_ms']
        if previous_time is not None and uptime < previous_time - 10000:
            boundary(number, 'uptime moved backwards by more than 10 seconds')
            previous_time = None
        if epoch['first_ms'] is None:
            epoch['first_ms'] = uptime
        if previous_time is not None:
            gap = max(0, uptime - previous_time)
            epoch['max_gap_ms'] = max(epoch['max_gap_ms'], gap)
            if gap > maximum_gap * 1000:
                issue('INCOMPLETE', 'log_gap', number, uptime, gap_ms=gap)
        epoch['last_ms'] = max(uptime, previous_time or uptime)
        if preview:
            continue
        if level == 'E' and not re.search(r'\bno_speech\b', message):
            issue('INCOMPLETE', 'error_log_needs_context', number, uptime, module=tag)
        if tag == 'nuwax_wake' and message.startswith('wake detector task started'):
            wake_generation += 1
            wake_last = uptime
        pairs = FIELD.findall(message)
        fields = {key: int(value) for key, value in pairs}
        if message.startswith('heap checkpoint='):
            for name, resource in resources.items():
                if name in fields:
                    value = fields[name]
                    resource['samples'] += 1
                    resource['minimum'] = min(value, resource['minimum']) if resource['minimum'] is not None else value
                    last = resource['last_ms'] if resource['last_ms'] is not None else epoch['first_ms']
                    if uptime - last > maximum_gap*1000:
                        issue('INCOMPLETE', 'memory_coverage_gap', number, uptime, field=name)
                    resource['last_ms'] = uptime
                    threshold = minimum_internal if name == 'internal_min' else minimum_dma
                    if value < threshold:
                        issue('FAIL', 'memory_below_budget', number, uptime,
                              field=name, value=value, threshold=threshold)
        family = next((f for f in FAMILIES if message.startswith(f)), None)
        if family is None:
            continue
        for name in HARD | SOFT:
            occurrences = re.findall(r'(?<![\w])'+name+r'=([^\s,;]+)', message)
            if len(occurrences) > 1 or (occurrences and name not in fields):
                issue('INCOMPLETE', 'malformed_counter', number, uptime, field=name)
        if family == 'wake capture health' and not {'queue_drops', 'invariant_faults', 'partial_reads', 'timeouts'} <= fields.keys():
            issue('INCOMPLETE', 'truncated_capture_health', number, uptime)
        identity = tuple(re.findall(r'\b(?:session|audio_session_id|request|round)=(\S+)', message))
        final = family == 'wake input final'
        if family in ('wake input health', 'wake input final'):
            family = 'wake input'
        if family == 'wake input':
            if {'partial_reads', 'timeouts', 'input_gaps'} <= fields.keys():
                wake_samples += 1
            else:
                issue('INCOMPLETE', 'truncated_wake_health', number, uptime)
            if wake_last is not None and uptime - wake_last > maximum_gap*1000:
                issue('INCOMPLETE', 'wake_coverage_gap', number, uptime)
            wake_last = None if final else uptime
        for name in sorted(HARD | SOFT):
            if name not in fields:
                continue
            value = fields[name]
            key = (len(epochs), tag, family, identity,
                   wake_generation if tag == 'nuwax_wake' else 0, name)
            previous = counters.get(key)
            delta = value if previous is None else value - previous
            if value < 0:
                issue('INCOMPLETE', 'invalid_counter', number, uptime, field=name)
                continue
            if delta < 0:
                issue('INCOMPLETE', 'counter_reset', number, uptime, field=name,
                      module=tag, previous=previous, value=value)
                delta = value
            counters[key] = value
            if delta > 0:
                kind = 'FAIL' if name in HARD else 'INCOMPLETE'
                code = 'loss_counter' if name in HARD else 'read_anomaly_needs_context'
                # A timeout/partial read is not proof of lost audio. It needs
                # lifecycle context; never waive it merely due to an earlier zero.
                issue(kind, code, number, uptime, module=tag, field=name,
                      value=value, delta=delta, initial=previous is None)
    if epoch['first_ms'] is not None:
        epochs.append(epoch)
        if wake_last is not None and epoch['last_ms'] - wake_last > maximum_gap*1000:
            issue('INCOMPLETE', 'wake_coverage_gap', None, epoch['last_ms'])
    longest = max(((e['last_ms']-e['first_ms'])/1000 for e in epochs), default=0)
    if longest < minimum_duration:
        issue('INCOMPLETE', 'duration_short', None, None,
              observed_seconds=longest, required_seconds=minimum_duration)
    if wake_samples < 2:
        issue('INCOMPLETE', 'missing_wake_coverage', None, None, samples=wake_samples)
    for name, resource in resources.items():
        if resource['samples'] < 2:
            issue('INCOMPLETE', 'missing_memory_coverage', None, None, field=name)
        elif resource['last_ms'] is not None and epoch['last_ms'] - resource['last_ms'] > maximum_gap*1000:
            issue('INCOMPLETE', 'memory_coverage_gap', None, None, field=name)
    status = 'FAIL' if counts['FAIL'] else 'INCOMPLETE' if counts['INCOMPLETE'] else 'PASS'
    return dict(schema_version=1, status=status,
                criteria=dict(minimum_duration_seconds=minimum_duration,
                              maximum_gap_seconds=maximum_gap,
                              minimum_internal_bytes=minimum_internal,
                              minimum_dma_largest_bytes=minimum_dma),
                longest_epoch_seconds=longest, epochs=epochs, wake_samples=wake_samples,
                memory=resources, failure_count=counts['FAIL'], incomplete_count=counts['INCOMPLETE'],
                issues=issues, issues_truncated=sum(counts.values())>len(issues),
                scope='Logged telemetry only; not proof of all product scenarios or physical stability.')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('log', type=Path)
    for name, default in [('minimum-duration', 259200), ('maximum-gap', 120),
                          ('minimum-internal', 16384), ('minimum-dma', 8192)]:
        p.add_argument('--'+name, type=int, default=default)
    p.add_argument('--report', type=Path)
    a = p.parse_args()
    values = [a.minimum_duration, a.maximum_gap, a.minimum_internal, a.minimum_dma]
    if min(values) <= 0:
        p.error('duration, gap and budgets must be positive')
    if a.report and a.report.resolve() == a.log.resolve():
        p.error('report must not overwrite the input log')
    try:
        with a.log.open('rb') as f:
            bom = f.read(4)
        encoding = 'utf-16' if bom.startswith((b'\xff\xfe', b'\xfe\xff')) else 'utf-8-sig'
        with a.log.open(encoding=encoding, errors='replace') as f:
            result = analyze(f, *values)
        with a.log.open('rb') as f:
            result['log_sha256'] = hashlib.file_digest(f, 'sha256').hexdigest()
        if a.report:
            a.report.parent.mkdir(parents=True, exist_ok=True)
            a.report.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    except (OSError, UnicodeError) as error:
        # Do not leave an older PASS report after a missing/unreadable input.
        if a.report:
            try:
                a.report.parent.mkdir(parents=True, exist_ok=True)
                a.report.write_text(json.dumps(dict(schema_version=1,status='INCOMPLETE',
                    error=str(error)),indent=2),encoding='utf-8')
            except OSError:
                pass
        print(f'LONG-RUN GATE: INCOMPLETE ({error})')
        return 3
    print(f"LONG-RUN GATE: {result['status']} duration={result['longest_epoch_seconds']:.1f}s "
          f"required={a.minimum_duration}s failures={result['failure_count']} "
          f"incomplete={result['incomplete_count']}")
    for item in result['issues'][:20]:
        print(f"{item['kind']} line={item['line']} t={item['uptime_ms']} "
              f"{item['code']} field={item.get('field', '-')} delta={item.get('delta', '-')}")
    if a.report:
        print(f'Report: {a.report.resolve()}')
    return {'PASS': 0, 'FAIL': 2, 'INCOMPLETE': 3}[result['status']]


if __name__ == '__main__':
    raise SystemExit(main())
