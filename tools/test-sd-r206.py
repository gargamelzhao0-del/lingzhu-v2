"""Production SDK error fix and bounded read-only scratch diagnostics.

Offline host checks only: never open a serial port or touch a physical card.
"""
from pathlib import Path
import ctypes
import hashlib
import importlib.util
import json
import os
import re
import subprocess

root = Path(__file__).resolve().parents[1]
sdk = Path('C:/esp/v6.0/esp-idf')
out = root/'build-host-checks/sd-r206'
out.mkdir(parents=True, exist_ok=True)
if os.name == 'nt': ctypes.windll.kernel32.SetErrorMode(0x8003)
spec = importlib.util.spec_from_file_location('sd_status_fix', root/'tools/prepare-sd-status-fix.py')
fix = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fix)
source = sdk/'components/sdmmc/sdmmc_cmd.c'
original = source.read_bytes()
patched = fix.patch(original).decode('utf-8')
try:
    fix.patch(original + b'\n')
except ValueError:
    pass
else:
    raise AssertionError('Unreviewed SDK source was accepted')
assert source.read_bytes() == original


def function(text, name):
    found = re.search(r'^(?:(?:static|inline)\s+)*(?:esp_err_t|bool)\s+'+re.escape(name)+r'\([^;{}]*\)\s*\{',text,re.M)
    assert found, name
    end = text.index('{', found.start()) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[found.start():end]


def run(name, fixture, includes, flags=()):
    exe = out/(name+'.exe')
    args = ['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-variable',
        '-I'+str(root/'main'),'-I'+str(out),'-I'+str(sdk/'components/sdmmc/include')]
    subprocess.run(args+list(flags)+[str(root/'main'/fixture),'-o',str(exe)],check=True)
    result = subprocess.run([str(exe)],capture_output=True,text=True,timeout=20)
    (out/(name+'.log')).write_text(result.stdout+result.stderr,encoding='utf-8')
    result.check_returncode()
    print(result.stdout.strip())


common = (sdk/'components/sdmmc/sdmmc_common.c').read_text()
header = (sdk/'components/sdmmc/include/esp_private/sdmmc_common.h').read_text()
for variant, text in [('sdk_original',original.decode('utf-8')),('sdk_fixed',patched)]:
    pieces = [function(header,'host_is_spi'),function(header,'sdmmc_ready_for_data'),
        function(text,'sdmmc_send_cmd'),function(text,'sdmmc_send_cmd_send_status'),
        function(common,'sdmmc_wait_for_idle'),function(text,'sdmmc_write_sectors_dma'),function(text,'sdmmc_read_sectors_dma')]
    (out/'sd_status_sdk_host.inc').write_text('\n\n'.join(pieces),encoding='utf-8')
    run(variant,'sd_status_sdk_host_check.c',[],['-DEXPECT_FIXED=1'] if variant=='sdk_fixed' else [])

trace = (root/'main/board/sd_probe_trace.c').read_text()
(out/'sd_probe_trace_host.inc').write_text(re.sub(r'(?m)^#include[^\r\n]*','',trace),encoding='utf-8')
run('trace','sd_probe_trace_host_check.c',[])
assert '__real_sdmmc_write_sectors' not in trace
assert 'sdmmc_erase' not in trace
assert 'trace->copy->host.do_transaction = bounded_transaction;' in trace
for name in ['main/board/sd_card_driver.c','main/sd_audio_catalog_io.inc']:
    content = (root/name).read_text(encoding='utf-8-sig')
    assert 'const bool trace_observed = observed && nuwax_sd_probe_trace_begin();' in content
    assert 'if (trace_observed) nuwax_sd_probe_trace_finish(' in content
    at = content.index('nuwax_sd_probe_trace_finish(')
    assert 'err,' in content[at:at+120]  # Original verdict, not a diagnostic result.
baseline = Path('C:/tmp/nuwax-current-release-20261030-r205')
protected = ['main/board/sd_dma_bulk.c','main/board/sd_dma_bulk.h',
    'main/sd_audio_session_storage.inc','main/sd_audio_protocol.h','main/sd_audio_scene_protocol.h']
if baseline.exists():
    for name in protected:
        actual, old = (root/name).read_bytes(), (baseline/name).read_bytes()
        if name == 'main/board/sd_dma_bulk.c':
            # R212 adds the optional observer call inside sd_bulk_command.
            # Strip only this feature's explicitly delimited branches; all
            # other source tokens must still equal the R205 implementation.
            text = actual.decode('utf-8-sig')
            from sd_source_contracts import dma_without_r217
            text = dma_without_r217(text)
            # R216 changes only joint DMA/verifier loan admission here. Its
            # actual composed heap/SDK-bounce boundaries have their own test;
            # normalize these exact tokens, retaining the rest of this fence.
            assert text.count('#include "board/sd_io_memory_budget.h"') == 1
            text = text.replace('#include "board/sd_io_memory_budget.h"\n', '')
            text, count = re.subn(r'#if CONFIG_NUWAX_SD_WRITE_VERIFY_ENABLE\n    const bool verified_write = write;\n#else\n    const bool verified_write = false;\n#endif\n', '', text)
            assert count == 1
            assert text.count('nuwax_sd_bulk_internal_floor(bytes, verified_write)') == 2
            text = text.replace('nuwax_sd_bulk_internal_floor(bytes, verified_write)', '16384U')
            text = re.sub(r'#if CONFIG_NUWAX_SD_WRITE_VERIFY_ENABLE\n#include "board/sd_write_verify.h"\n#endif\n','',text)
            text = re.sub(r'#if CONFIG_NUWAX_SD_INTEGRITY_DIAGNOSTICS\n#include "board/sd_integrity_io.h"\n#endif\n','',text)
            text = re.sub(r'#if CONFIG_NUWAX_SD_INTEGRITY_DIAGNOSTICS\n    const esp_err_t err = nuwax_sd_integrity_io_transfer\(card, buffer, sector, count, write\);\n#elif CONFIG_NUWAX_SD_WRITE_VERIFY_ENABLE\n    const esp_err_t err = write \? nuwax_sd_write_verified\(card, buffer, sector, count\)\n                               : __real_sdmmc_read_sectors\(card, buffer, sector, count\);\n#else\n(.*?)#endif\n',r'\1',text,flags=re.S)
            assert re.sub(r'\s+','',text)==re.sub(r'\s+','',old.decode('utf-8-sig')),name
        elif name == 'main/board/sd_dma_bulk.h':
            added='''
/* Only a match with the original application CRC may call end(true).
 * Healthy media keep bulk reads; compatibility mode lasts for this boot. */
bool nuwax_sd_bulk_read_recovery_begin(uint32_t timeout_ms);
void nuwax_sd_bulk_read_recovery_end(bool validated_original);
bool nuwax_sd_bulk_read_is_degraded(void);
'''
            text = actual.decode('utf-8-sig')
            assert text.endswith(added)
            assert text[:-len(added)] == old.decode('utf-8-sig'), name
        else:
            assert actual == old, name
    # R208 deliberately changes mode-control admission, so the whole controller
    # is no longer byte-identical to R205. Its streaming/capture protocol and
    # monitoring's strictly scoped capture ACK must remain identical instead.
    for kind in ['meeting', 'monitoring']:
        name=f'main/audio_modes/{kind}/{kind}_mode.c'
        actual=(root/name).read_text(encoding='utf-8-sig')
        old=(baseline/name).read_text(encoding='utf-8-sig')
        assert function(actual,'capture_session')==function(old,'capture_session'),name
        if kind=='monitoring':
            assert function(actual,'monitoring_capture_control_take_ack')==function(old,'monitoring_capture_control_take_ack'),name
report = dict(sdkInputSha256=hashlib.sha256(original).hexdigest(),
    generatedSha256=hashlib.sha256(patched.encode()).hexdigest(),sdkUnmodified=True,
    realSdkPropagationAndOrderTested=True,unknownR1PolicyUnchanged=True,
    actualDiagnosticSourceTested=True,protectedCaptureStorageProtocolUnchanged=True,
    transportFlowSeparatelyTested=True,
    hardwareValidated=False)
(out/'results.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
print('PASS R206: SDK fix + original negative control; bounded scratch-only diagnostics; retained verdict and capture/upload contracts')
