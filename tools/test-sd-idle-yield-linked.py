"""Bind 143's bounded fairness wait to the ELF and isolate it from 142's wire/data paths."""
from pathlib import Path
import argparse, hashlib, json, re, subprocess
from elftools.elf.elffile import ELFFile

p=argparse.ArgumentParser()
p.add_argument('--elf', type=Path, required=True)
p.add_argument('--baseline', type=Path, required=True)
a=p.parse_args()
r=Path(__file__).resolve().parents[1]
prior=r/'artifacts/firmware/7.0.142-sd-gateway-test-20261015'
sha=lambda path:hashlib.sha256(path.read_bytes()).hexdigest()
read=lambda path:path.read_text(encoding='utf-8-sig')
old=read(a.baseline/'main/offline_audio_spool.c')
new=read(r/'main/offline_audio_spool.c')

# Normalize exactly the intended changes; any unrelated sender, CRC, ACK,
# release, cancellation or catalog change fails the comparison.
new=new.replace('SD scheduler revision=143', 'SD scheduler revision=141')
new=new.replace(' byte_bound=65536 fairness_wait=idle_event', ' byte_bound=65536')
new=new.replace('    uint32_t fairness_idle_wakes = 0U, fairness_timer_wakes = 0U;\n'
                '    uint64_t fairness_wait_us = 0U;\n', '')
wait='''                    const int64_t fairness_started = esp_timer_get_time();
#ifdef ESP_PLATFORM
                    if (nuwax_replay_schedule_yield()) fairness_idle_wakes++;
                    else fairness_timer_wakes++;
#else
                    vTaskDelay(1U);
                    fairness_timer_wakes++;
#endif
                    fairness_wait_us += esp_timer_get_time() - fairness_started;'''
assert new.count(wait)==1, 'Expected exactly one modified sender fairness wait'
new=new.replace(wait, '                    vTaskDelay(1U);')
log='''    ESP_LOGI(TAG, "offline replay fairness idle_wakes=%lu timer_wakes=%lu wait_ms=%llu byte_bound=65536",
             (unsigned long)fairness_idle_wakes, (unsigned long)fairness_timer_wakes,
             (unsigned long long)(fairness_wait_us / 1000U));
'''
assert new.count(log)==1
new=new.replace(log, '')
assert new==old, 'An unrelated spool or sender change was introduced'

unchanged=['main/offline_io_schedule.h','main/offline_replay_prefetch.inc',
    'main/offline_replay_pressure.inc','main/sd_audio_catalog.c','main/sd_audio_catalog.h',
    'main/sd_audio_session_replay.inc','main/ws_transport.c','main/ws_mask.h',
    'main/nuwax_lvgl_port.c','main/nuwax_ui_v28/face/emotion_animation_player.c',
    'main/nuwax_ui_v28/face/emotion_decode_schedule.h']
for name in unchanged:
    assert sha(r/name)==sha(a.baseline/name), name
defs=lambda path:dict(re.findall(r'^#define (CONFIG_\w+) (.*)$',read(path),re.M))
assert defs(a.elf.parent/'config/sdkconfig.h')==defs(prior/'config/sdkconfig.h')
binary=a.elf.with_suffix('.bin').read_bytes()
assert binary[48:80].split(b'\0')[0]==b'7.0.143'
assert binary[176:208].hex()==sha(a.elf)
assert b'catalog_reused=%u' in binary and b'confirm_ms=%lu persist_ms=%lu' in binary

tool=Path('C:/Espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin')
with a.elf.open('rb') as f:
    elf=ELFFile(f)
    symbols={s.name:(s['st_value'],s['st_size'],s['st_shndx'])
        for s in elf.get_section_by_name('.symtab').iter_symbols() if s['st_size']}
    names=['s_idle_event_storage','s_idle_event','s_idle_wait_state']
    static_bytes=sum(symbols[n][1] for n in names)
    assert 0<static_bytes<=192, static_bytes
    for name in names:
        assert elf.get_section(symbols[name][2]).name=='.dram0.bss', name

callers=[n for n in symbols if n.startswith('replay_one_ready')]
asm='\n'.join(subprocess.check_output([str(tool/'xtensa-esp32s3-elf-objdump.exe'),
    '-d','--disassemble='+name,str(a.elf)], text=True) for name in callers)
for target in ['sd_audio_catalog_bind','sd_audio_catalog_ack','sd_audio_catalog_note_deleted']:
    assert '<'+target+'>' in asm, target

report={'version':'7.0.143','elfSha256':sha(a.elf),'hardwareValidated':False,
    'staticEventBytes':static_bytes,'unchanged':unchanged,'effectiveConfigUnchanged':True,
    'spoolChangesLimitedToBoundedFairnessWaitAndMetrics':True,
    'durableMetadataCallers':callers}
out=r/'build-host-checks/sd-idle-yield';out.mkdir(parents=True,exist_ok=True)
(out/'linked.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
print(f'PASS final 143 ELF: {static_bytes} static internal bytes; bounded sender wait only; '
      '142 protocol/CRC/ACK/capture/decoder/reader/config unchanged; catalog reuse and durable deletion remain linked')
