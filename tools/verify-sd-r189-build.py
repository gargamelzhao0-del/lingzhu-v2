"""Verify r189 stop recovery, TLS/TCP optimization and retained product contracts."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('build',type=Path)
p.add_argument('--baseline-build',type=Path,required=True)
p.add_argument('--baseline-sources',type=Path,required=True)
p.add_argument('--version',default='7.0.189')
a=p.parse_args()
root=Path(__file__).resolve().parents[1];build=a.build.resolve()
digest=lambda path:hashlib.sha256(path.read_bytes()).hexdigest()
binary=build/'NuwaxAIPartner.bin';elf=build/'NuwaxAIPartner.elf';data=binary.read_bytes()
assert data[48:80].split(b'\0')[0].decode()==a.version
assert data[176:208].hex()==digest(elf)
assert len(data)<=4194304
# Keep the conservative image budget and the actually deployed partition map.
assert digest(build/'partition_table/partition-table.bin')==digest(a.baseline_build/'partition_table/partition-table.bin')
before=json.loads((a.baseline_build/'config/sdkconfig.json').read_text())
after=json.loads((build/'config/sdkconfig.json').read_text())
changes={k:[before.get(k),after.get(k)] for k in before.keys()|after.keys() if before.get(k)!=after.get(k)}
assert set(changes)=={'NUWAX_SD_REPLAY_EARLY_TCP_WRITABLE'},changes
assert after['NUWAX_SD_REPLAY_EARLY_TCP_WRITABLE']
assert after['MBEDTLS_SSL_OUT_CONTENT_LEN']==8192
assert after['NUWAX_OFFLINE_AUDIO_REPLAY_FORMAL_ENABLE']
assert not after['NUWAX_OFFLINE_AUDIO_REPLAY_TEST_ENABLE']
assert not after.get('NUWAX_TEST_NETWORK_ENABLE',False)
assert not after.get('MBEDTLS_HARDWARE_AES',False)
for marker in [b'SD formal pipeline revision=189',b'SD formal fairness idle_wakes=',
               b'SD formal TLS aead_ms=',b'TLS negotiated version=',
               b'SD session finalization incomplete session=', b'SD meeting stop retained across disconnect',
               b'SD monitoring stop retained across disconnect']:
    assert marker in data,marker
baseline=json.loads(a.baseline_sources.read_text())
protected=['main/nuwax_ai_partner_main.c','main/nuwax_lvgl_port.c','main/offline_replay_prefetch.inc',
    'main/sd_audio_session_replay.inc','main/sd_audio_scene_replay.inc','main/sd_audio_protocol.h',
    'main/sd_audio_scene_protocol.h','main/audio_modes/meeting/meeting_protocol.c',
    'main/audio_modes/monitoring/monitoring_protocol.c','sdkconfig.product-ota.defaults',
    'main/wake_word_detector.c']
for name in protected: assert digest(root/name)==baseline[name],name
allowed_main={'main/Kconfig.projbuild','main/tls_cipher_policy.h','main/ws_transport.c',
    'main/offline_replay_shared.inc','main/audio_modes/meeting/meeting_mode.c',
    'main/audio_modes/monitoring/monitoring_mode.c','main/sd_audio_mode_host_check.inc'}
changed_main={n for n,h in baseline.items() if n.startswith('main/') and digest(root/n)!=h}
assert changed_main==allowed_main,changed_main ^ allowed_main
# Check the actual SDK and application compilation flags, not just Kconfig.
commands=json.loads((build/'compile_commands.json').read_text())
compiled_tcp=[]
for entry in commands:
    name=Path(entry['file']).name
    if name in ('api_msg.c','sockets.c','ws_transport.c'):
        assert '-DTCP_SNDLOWAT=2881' in entry['command'],name
        compiled_tcp.append(name)
assert set(compiled_tcp)=={'api_msg.c','sockets.c','ws_transport.c'},compiled_tcp

units={
    'audio_modes/meeting/meeting_mode.c':['main/audio_modes/meeting/meeting_mode.c'],
    'audio_modes/monitoring/monitoring_mode.c':['main/audio_modes/monitoring/monitoring_mode.c'],
    'tls_record_metrics.c':['main/tls_record_metrics.c','main/tls_record_metrics.h','main/tls_cipher_policy.h'],
    'ws_transport.c':['main/ws_transport.c','main/ws_send_metrics.h'],
    'replay_ui_schedule.c':['main/replay_ui_schedule.c','main/replay_ui_schedule.h'],
    'offline_audio_spool.c':['main/offline_audio_spool.c','main/offline_replay_shared.inc',
        'main/offline_replay_policy.h','main/offline_io_schedule.h','main/sd_audio_session_storage.inc']}
for unit,sources in units.items():
    obj=build/'esp-idf/main/CMakeFiles/__idf_main.dir'/(unit+'.obj')
    for name in sources: assert (root/name).stat().st_mtime_ns <= obj.stat().st_mtime_ns <= elf.stat().st_mtime_ns,name
tool=Path('C:/Espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin')
symbols=subprocess.check_output([str(tool/'xtensa-esp32s3-elf-nm.exe'),'-S','--defined-only',str(elf)],text=True)
asm=subprocess.check_output([str(tool/'xtensa-esp32s3-elf-objdump.exe'),'-d',str(elf)],text=True)
sizes={m[3]:(int(m[1],16),int(m[2],16)) for m in re.finditer(
    r'^(\w+)\s+(\w+)\s+\w\s+(\S+)$',symbols,re.M)}
cache={}
def bodies(prefix):
    if prefix in cache: return cache[prefix]
    found=[]
    for match in re.finditer(r'^\w+ <([^>]+)>:\n([\s\S]*?)(?=^\w+ <|\Z)',asm,re.M):
        if match[1]==prefix or match[1].startswith(prefix+'.') or match[1].startswith(prefix+'$'):
            body=match[0]
            # Xtensa's linear disassembler can lose alignment on literal/padding
            # bytes between basic blocks. Decode real branch targets separately.
            start,size=sizes[match[1]]
            targets={int(v,16) for v in re.findall(
                r'\b([0-9a-f]+) <'+re.escape(match[1])+r'\+0x[0-9a-f]+>',body)}
            for target in sorted(targets):
                if start <= target < start+size:
                    body+='\n'+subprocess.check_output([str(tool/'xtensa-esp32s3-elf-objdump.exe'),'-d',
                        '--start-address='+hex(target),'--stop-address='+hex(min(target+64,start+size)),str(elf)],text=True)
            found.append(body)
    assert found,prefix
    cache[prefix]='\n'.join(found)
    return cache[prefix]
links={
    'send_all_until':['nuwax_tls_metrics_begin','nuwax_tls_metrics_end'],
    '__wrap_psa_aead_encrypt':['psa_aead_encrypt'],
    '__wrap_mbedtls_net_send':['mbedtls_net_send'],
    'nuwax_replay_schedule_yield':['xEventGroupWaitBits','vTaskDelay'],
    'nuwax_replay_schedule_init':['esp_register_freertos_idle_hook_for_cpu'],
    'mbedtls_ssl_encrypt_buf':['__wrap_psa_aead_encrypt'],
    'esp_create_mbedtls_handle':['__wrap_mbedtls_net_send'],
    'nuwax_meeting_mode_notify_ws_disconnected':['preserve_sd_stop_on_disconnect'],
    'nuwax_monitoring_mode_notify_ws_disconnected':['preserve_sd_stop_on_disconnect'],
}
for caller,callees in links.items():
    body=bodies(caller)
    for callee in callees: assert '<'+callee+'>' in body,(caller,callee)
# The two SDK functions above must use the wrappers: merely linking the
# diagnostic functions would not prove that TLS records are measured.
assert 'nuwax_tls_software_suites' in symbols
observer=re.search(r'^\w+\s+(\w+)\s+\w\s+s_idle_observers$',symbols,re.M)
assert observer and int(observer[1],16)<=256
frames={}
for name in ['__wrap_psa_aead_encrypt','__wrap_mbedtls_net_send','replay_one_ready','preserve_sd_stop_on_disconnect']:
    body=bodies(name)
    matches=re.findall(r'entry\s+a1,\s*(0x[0-9a-f]+|\d+)',body)
    assert matches,name
    frames[name]=max(int(v,0) for v in matches)
    assert frames[name]<=2048,(name,frames[name])
result=dict(version=a.version,bytes=len(data),binSha256=digest(binary),elfSha256=digest(elf),
    otaImageBudgetBytes=4194304,otaBudgetHeadroomBytes=4194304-len(data),otaPartitionBytes=0x420000,configurationChanges=changes,unchangedProductSources=protected,
    sourceHashes={n:digest(root/n) for names in units.values() for n in names},linkedCalls=links,
    observerBytes=int(observer[1],16),staticStackFrames=frames,protocolChanged=False,
    changedMainSources=sorted(changed_main),tcpLowat=2881,tcpCompiledUnits=compiled_tcp,
    hardwareValidated=False,measuredThroughputKiBs=None,targetThroughputKiBs=500)
(build/'sd-tls-speed-verification.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
print(json.dumps({k:v for k,v in result.items() if k not in ('sourceHashes','unchangedProductSources','linkedCalls')},indent=2))
