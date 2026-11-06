"""Bind 144's post-send reorder to the ELF and isolate wire/capture changes."""
from pathlib import Path
import argparse, hashlib, json, re, subprocess
from elftools.elf.elffile import ELFFile

p = argparse.ArgumentParser()
p.add_argument('--elf', type=Path, required=True)
p.add_argument('--baseline', type=Path, required=True)
a = p.parse_args()
r = Path(__file__).resolve().parents[1]
prior = r / 'artifacts/firmware/7.0.143-sd-gateway-test-20261015'
sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
read = lambda path: path.read_text(encoding='utf-8-sig')
old = read(a.baseline / 'main/offline_audio_spool.c')
new = read(r / 'main/offline_audio_spool.c')

def block(source, marker):
    start = source.index(marker, source.index('static esp_err_t replay_send_windowed('))
    end = source.index('\n            }\n            if (err != ESP_OK)', start)
    return source[start:end]

before = block(old, '/* No transport lock is held here.')
after = block(new, '/* Fairness first:')
report = json.loads((r / 'build-host-checks/replay-cooperate-order/report.json').read_text())
assert report['sourceSha256'] == sha(r / 'main/offline_audio_spool.c')
assert report['oldBlockSha256'] == hashlib.sha256(before.encode()).hexdigest()
assert report['newBlockSha256'] == hashlib.sha256(after.encode()).hexdigest()
assert new.count(after) == 1
normalized = new.replace(after, before)
normalized = normalized.replace('SD scheduler revision=144', 'SD scheduler revision=143')
normalized = normalized.replace(' cooperate_order=fairness_then_ui', '')
normalized = normalized.replace('    uint32_t fairness_followup_ui = 0U;\n', '')
normalized = normalized.replace(' byte_bound=65536 followup_ui=%lu', ' byte_bound=65536')
normalized = normalized.replace(', (unsigned long)fairness_followup_ui);', ');')
assert normalized == old, 'Unrelated spool/CRC/ACK/capture change'

unchanged = ['main/replay_ui_schedule.c', 'main/replay_ui_schedule.h',
    'main/offline_io_schedule.h', 'main/offline_replay_prefetch.inc',
    'main/offline_replay_pressure.inc', 'main/offline_replay_read.inc',
    'main/sd_audio_catalog.c', 'main/sd_audio_catalog.h', 'main/sd_audio_session_replay.inc',
    'main/ws_transport.c', 'main/ws_mask.h', 'main/nuwax_lvgl_port.c',
    'main/nuwax_ui_v28/face/emotion_animation_player.c',
    'main/nuwax_ui_v28/face/emotion_decode_schedule.h']
for name in unchanged:
    assert sha(r / name) == sha(a.baseline / name), name
defs = lambda path: dict(re.findall(r'^#define (CONFIG_\w+) (.*)$', read(path), re.M))
assert defs(a.elf.parent / 'config/sdkconfig.h') == defs(prior / 'config/sdkconfig.h')
binary = a.elf.with_suffix('.bin').read_bytes()
assert binary[48:80].split(b'\0')[0] == b'7.0.144'
assert binary[176:208].hex() == sha(a.elf)
for marker in [b'catalog_reused=%u', b'confirm_ms=%lu persist_ms=%lu',
               b'cooperate_order=fairness_then_ui', b'followup_ui=%lu']:
    assert marker in binary, marker

tool = Path('C:/Espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin')
with a.elf.open('rb') as f:
    elf = ELFFile(f)
    symbols = {s.name: (s['st_value'], s['st_size'], s['st_shndx'])
               for s in elf.get_section_by_name('.symtab').iter_symbols() if s['st_size']}
    names = ['s_idle_event_storage', 's_idle_event', 's_idle_wait_state']
    static_bytes = sum(symbols[n][1] for n in names)
    assert static_bytes == 44
    for name in names:
        assert elf.get_section(symbols[name][2]).name == '.dram0.bss', name
callers = [n for n in symbols if n.startswith('replay_one_ready')]
asm = '\n'.join(subprocess.check_output([str(tool / 'xtensa-esp32s3-elf-objdump.exe'),
    '-d', '--disassemble=' + name, str(a.elf)], text=True) for name in callers)
for target in ['sd_audio_catalog_bind', 'sd_audio_catalog_ack', 'sd_audio_catalog_note_deleted']:
    assert '<' + target + '>' in asm, target

result = {'version': '7.0.144', 'elfSha256': sha(a.elf), 'hardwareValidated': False,
    'unchanged': unchanged, 'effectiveConfigUnchanged': True,
    'spoolChangesLimitedToTestedPostSendReorderAndMetrics': True,
    'existingStaticEventBytes': static_bytes, 'durableMetadataCallers': callers}
out = r / 'build-host-checks/replay-cooperate-order'
(out / 'linked.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
print('PASS final 144 ELF: tested post-send reorder only; 143 protocol/CRC/ACK/capture/'
      'decoder/reader/transport/config unchanged; durable catalog deletion remains linked')
