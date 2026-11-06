"""Check 148 diagnostic scope against the captured 147 source and linked image."""
from pathlib import Path
import argparse
import hashlib
import json
import re
from elftools.elf.elffile import ELFFile

p = argparse.ArgumentParser()
p.add_argument('--elf', type=Path, required=True)
p.add_argument('--baseline', type=Path, required=True)
a = p.parse_args()
r = Path(__file__).resolve().parents[1]
prior = r / 'artifacts/firmware/7.0.147-sd-gateway-test-20261016'
read = lambda path: path.read_text(encoding='utf-8-sig')
sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()

# Compare the complete transport, not just a copy of its decision branches.
transport = read(r / 'main/ws_transport.c')
additions = [
    '    bool previous_wait_timed_out = false;\n'
    '    int64_t previous_wait_finished_us = 0;\n',
    '        if (archive_progress && previous_wait_timed_out) {\n'
    '            nuwax_ws_metrics_after_timeout(trace->metrics, ret,\n'
    '                io_retryable(client, ret, operation_errno),\n'
    '                (uint64_t)(write_started - previous_wait_finished_us));\n'
    '            previous_wait_timed_out = false;\n'
    '        }\n',
    '                previous_wait_timed_out = archive_progress && ready_err == ESP_ERR_TIMEOUT;\n'
    '                previous_wait_finished_us = wait_finished;\n',
]
for addition in additions:
    assert transport.count(addition) == 1
    transport = transport.replace(addition, '')
assert transport == read(a.baseline / 'main/ws_transport.c'), 'Transport policy changed'

spool = read(r / 'main/offline_audio_spool.c')
start = spool.index('    ESP_LOGI(TAG, "offline writable trace revision=148 ')
end = spool.index('    ESP_LOGI(TAG, "offline replay transport progress ', start)
assert spool[:start] + spool[end:] == read(a.baseline / 'main/offline_audio_spool.c')

def function(source, name):
    match = re.search(r'^(?:static )?[\w* ]+\b' + name + r'\([^;{}]*\)\s*\{', source, re.M)
    assert match, name
    at = source.index('{', match.start()) + 1
    depth = 1
    while depth:
        depth += (source[at] == '{') - (source[at] == '}')
        at += 1
    return source[match.start():at]

network = read(r / 'main/network_tx_probe.c')
old_network = read(a.baseline / 'main/network_tx_probe.c')
unchanged_functions = ['replay_retry_now_ms', 'replay_sample_queue', 'replay_sample_read',
    'replay_retry_schedule', 'replay_retry_timer', 'nuwax_network_tx_probe_init',
    '__wrap_esp_netif_transmit_wrap', '__wrap_tcp_output', 'replay_output_retry',
    'nuwax_network_tx_retry_poll']
for name in unchanged_functions:
    assert function(network, name) == function(old_network, name), name
receive = function(network, '__wrap_tcp_input')
assert receive.count('__real_tcp_input(p, inp);') == 1
after_sdk = receive.split('__real_tcp_input(p, inp);')[1]
assert not re.search(r'\bp\s*->', after_sdk), 'Access to consumed pbuf'
assert not re.search(r'\b(?:malloc|calloc|free|vTaskDelay|ESP_LOG\w|esp_timer_start_once)\s*\(', receive)
assert not re.search(r'\b(?:pcb|match)\s*->\s*\w+\s*=(?!=)', receive)
assert not re.search(r'\besp_wifi_set_\w+\s*\(', network)

previous = json.loads(read(prior / 'build-result.json'))
allowed = {'main/network_tx_probe.c', 'main/offline_audio_spool.c', 'main/ws_send_metrics.h',
    'main/ws_transport.c', 'main/ws_frame_host_check.c', 'tools/build-sd-session-test.ps1',
    'tools/cmake/sd-tcp-writable.cmake', 'tools/test-network-tx-probe.py',
    'tools/test-replay-ui-linked.py'}
unchanged = []
for name, expected in previous['sourceHashes'].items():
    assert sha(a.baseline / name) == expected, 'Unverified baseline: ' + name
    if name not in allowed:
        assert sha(r / name) == expected, 'Unexpected source change: ' + name
        unchanged.append(name)

defines = lambda path: dict(re.findall(r'^#define (CONFIG_\w+) (.*)$', read(path), re.M))
assert defines(a.elf.parent / 'config/sdkconfig.h') == defines(prior / 'config/sdkconfig.h')
image = a.elf.with_suffix('.bin').read_bytes()
assert image[48:80].split(b'\0')[0] == b'7.0.148'
assert image[176:208].hex() == sha(a.elf)

for name in ['network-tx-probe/report.json', 'replay-ui-schedule/linked.json']:
    report = json.loads(read(r / 'build-host-checks' / name))
    assert report['version'] == '7.0.148' and report['elfSha256'] == sha(a.elf)

def symbols(path):
    with path.open('rb') as stream:
        elf = ELFFile(stream)
        return {s.name: (s['st_size'], elf.get_section(s['st_shndx']).name)
            for s in elf.get_section_by_name('.symtab').iter_symbols()
            if s['st_size'] and isinstance(s['st_shndx'], int)}

before = symbols(prior / 'NuwaxAIPartner.elf')
after = symbols(a.elf)
state = ['s_sample', 's_tcp_previous', 's_tcp_inputs', 's_tcp_input_max_us', 's_tcp_input_ambiguous']
delta = sum(after[n][0] - before.get(n, (0, ''))[0] for n in state)
assert delta == 68, delta
assert all(after[n][1] == '.dram0.bss' for n in state)

warning_lines = lambda path: sorted(line.split('warning:', 1)[1] for line in read(path).splitlines()
    if 'warning:' in line)
warnings = warning_lines(r / 'tmp/build-sd-148.log')
assert warnings == warning_lines(r / 'tmp/build-sd-147.log'), 'New compiler warnings'

result = dict(version='7.0.148', elfSha256=sha(a.elf), hardwareValidated=False,
    transportPolicyUnchanged=True, spoolOnlyAddsPostPayloadLog=True,
    recoveryFunctionsUnchanged=unchanged_functions, unchangedSourceFiles=unchanged,
    effectiveConfigUnchanged=True, addedDiagnosticStaticBytes=delta,
    compilerWarnings=len(warnings), newCompilerWarnings=0,
    receiveCallsOriginalOnce=True, noPacketAccessAfterOriginal=True,
    receiveNoAllocationWaitOrLog=True, radioQueriesReadOnly=True,
    scope='diagnostics only; no measured throughput improvement')
out = r / 'build-host-checks/network-trace'
out.mkdir(parents=True, exist_ok=True)
(out / 'linked.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
print('PASS final 148 diagnostic scope: exact transport policy and spool preservation; '
      '147 UI/capture/reader/CRC/ACK/config unchanged; receiver lifetime bounded; '
      '68 additional static bytes; linked observers and version verified; no new compiler warnings')
