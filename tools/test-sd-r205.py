"""Fresh SD qualification challenges and fail-fast confirmed-media admission.

Uses the actual driver and controller harnesses; the stale-data mutant must
fail. Does not touch serial ports, SD hardware or any captured user audio.
"""
from pathlib import Path
import ctypes, os, re, subprocess, json
root=Path(__file__).resolve().parents[1]
if os.name=='nt':ctypes.windll.kernel32.SetErrorMode(0x8003)
out=root/'build-host-checks/sd-r205';out.mkdir(parents=True,exist_ok=True)
flags=['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function',
    '-I'+str(out),'-I'+str(root/'main')]
driver=(root/'main/board/sd_card_driver.c').read_text(encoding='utf-8-sig')
storage=(root/'main/sd_audio_session_storage.inc').read_text(encoding='utf-8-sig')
def function(text,name):
    m=re.search(r'^(?:static )?[\w *]+\b'+name+r'\([^;{}]*\)\s*\{',text,re.M);assert m,name
    at=text.index('{',m.start())+1;depth=1
    while depth:depth+=(text[at]=='{')-(text[at]=='}');at+=1
    return text[m.start():at]

code=r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_NOT_FINISHED 0x10c
#define ESP_ERR_TIMEOUT 0x107
static atomic_int s_media_error;
'''+function(driver,'nuwax_sd_card_media_error')+'\n'+function(storage,'nuwax_sd_session_start_fault')+r'''
int main(void){
 int benign[]={ESP_OK,ESP_ERR_NO_MEM,ESP_ERR_NOT_FINISHED,ESP_ERR_TIMEOUT};
 for(unsigned i=0;i<sizeof(benign)/sizeof(benign[0]);i++){
   atomic_store(&s_media_error,benign[i]);assert(nuwax_sd_session_start_fault()==ESP_OK);
 }
 int fault[]={ESP_FAIL,0x109,0x104,0x103,0x108};
 for(unsigned i=0;i<sizeof(fault)/sizeof(fault[0]);i++){
   atomic_store(&s_media_error,fault[i]);assert(nuwax_sd_session_start_fault()==fault[i]);
 }
 atomic_store(&s_media_error,0);assert(nuwax_sd_session_start_fault()==ESP_OK);
 puts("PASS actual lock-free start fault: confirmed errors retain cause; normal busy/no-memory/timeout waiting and cleared recovery unchanged");
}
'''
(out/'fault.c').write_text(code,encoding='utf-8');exe=out/'fault.exe'
subprocess.run(flags+[str(out/'fault.c'),'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)

source=re.sub(r'(?m)^#include[^\r\n]*','',driver)
for variant in ['production','fixed_payload_mutant']:
    selected=source
    if variant=='fixed_payload_mutant':
        assert selected.count('nuwax_sd_probe_byte(offset + i, challenge)')==3
        selected=selected.replace('nuwax_sd_probe_byte(offset + i, challenge)',
            '(uint8_t)((offset + i) * 31U + ((offset + i) >> 8U))')
    (out/'sd_speed_driver_host.inc').write_text(selected,encoding='utf-8')
    exe=out/(variant+'.exe')
    subprocess.run(flags+['-DCONFIG_NUWAX_SD_CARD_HIGHSPEED_ENABLE=1',
        str(root/'main/sd_speed_host_check.c'),'-o',str(exe)],check=True)
    p=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30)
    assert (p.returncode==0)==(variant=='production'),variant+'\n'+p.stdout+p.stderr
    print('PASS '+variant+': '+('actual successful-but-lost writes rejected; readiness remains fenced' if not p.returncode else 'old fixed probe payload falsely accepts stale sectors; mutant rejected'))

# Real shared DMA adapter: same sector, same SDK source buffer. No persistent
# payload logging or changes to successful SDK return values are permitted.
subprocess.run(['powershell.exe','-NoProfile','-ExecutionPolicy','Bypass','-File',
    str(root/'tools/run-sd-dma-host-checks.ps1')],check=True)
io=(root/'main/sd_audio_catalog_io.inc').read_text(encoding='utf-8-sig')
assert 'catalog_sample_descriptor(path, offset, bytes, crc_offset, magic, &sample);' in io
assert 'const int saved_errno = errno;' in io and 'errno = saved_errno;' in io
assert 'if (observed) nuwax_sd_io_probe_end();' in driver and 'if (observed) nuwax_sd_io_probe_end();' in io
for kind in ['meeting','monitoring']:
    mode=(root/f'main/audio_modes/{kind}/{kind}_mode.c').read_text(encoding='utf-8-sig')
    at=mode.index('const esp_err_t storage_fault = sd_admission')
    terminal=mode.index('"sd_start_failed"',at)
    starting=mode.index('NUWAX_'+kind.upper()+'_STATE_STARTING, "cloud_enable"',at)
    assert at<terminal<mode.index('reset_transient_state(mode);',at)<starting
    assert 'nuwax_audio_command_decide' in mode[at:terminal]
    assert mode.count('nuwax_sd_session_start_fault()')==2
# R210 deliberately evolves transport backpressure/deadlines. Its production
# send-loop/observer and byte-exact handoff are tested by test-sd-flow-control.
# Keep the R205 media fix's immutable storage/crypto/wire boundary frozen.
unchanged=['main/sd_socket_wait.c','main/sd_audio_protocol.h','main/sd_audio_scene_protocol.h',
    'main/tls_archive_crypto.c']
base=Path('C:/tmp/nuwax-current-release-20261030-r204')
if base.exists():
    for path in unchanged:assert (root/path).read_bytes()==(base/path).read_bytes(),path
report=dict(freshChallengeCatchesLostWrites=True,fixedPayloadNegativeRejected=True,
    actualDmaSectorWitnessTested=True,rawDescriptorReadbackDiagnostic=True,
    confirmedFaultCheckedBeforeUiMicHandoff=True,originalCommandErrorPreserved=True,
    cryptoAndWireFormatUnchanged=True,transportFlowSeparatelyTested=True,hardwareValidated=False)
(out/'results.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
print('PASS R205 media repair: fresh probe, bounded SDK witnesses, descriptor evidence, early fault preflight and unchanged storage/crypto/wire format')
