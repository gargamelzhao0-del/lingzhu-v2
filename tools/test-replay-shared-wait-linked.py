"""Bind the tested 146 scheduling changes to the final ELF; isolate data paths."""
from pathlib import Path
import argparse, hashlib, json, re, subprocess
from elftools.elf.elffile import ELFFile

p = argparse.ArgumentParser()
p.add_argument('--elf', type=Path, required=True)
p.add_argument('--baseline', type=Path, required=True)
a = p.parse_args()
r = Path(__file__).resolve().parents[1]
prior = r / 'artifacts/firmware/7.0.145-sd-gateway-test-20261016'
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
read = lambda p: p.read_text(encoding='utf-8-sig')
old = read(a.baseline / 'main/offline_audio_spool.c')
new = read(r / 'main/offline_audio_spool.c')

def block(s, marker):
    start = s.index(marker, s.index('static esp_err_t replay_send_windowed('))
    return s[start:s.index('\n            }\n            if (err != ESP_OK)', start)]

before = block(old, '/* Serve an existing grant')
after = block(new, '/* Decide fairness before waiting')
report = json.loads(read(r / 'build-host-checks/replay-shared-wait/report.json'))
assert report['oldBlockSha256'] == hashlib.sha256(before.encode()).hexdigest()
assert report['newBlockSha256'] == hashlib.sha256(after.encode()).hexdigest()
for name, expected in report['sourceHashes'].items():
    assert sha(r / name) == expected, name
normalized = new.replace(after, before)
normalized = normalized.replace('SD scheduler revision=146', 'SD scheduler revision=145')
normalized = normalized.replace('sender_handoff=event', 'sender_handoff=bounded')
normalized = normalized.replace('cooperate_order=shared_wait help_lead_us=25000', 'cooperate_order=pending_ui_then_fairness')
normalized = normalized.replace('uint32_t fairness_shared_ui = 0U, fairness_ui_pending = 0U;', 'uint32_t fairness_followup_ui = 0U;')
normalized = normalized.replace('shared_ui=%lu', 'followup_ui=%lu').replace(' ui_pending=%lu', '')
normalized = normalized.replace('(unsigned long)fairness_shared_ui', '(unsigned long)fairness_followup_ui')
normalized = normalized.replace(', (unsigned long)fairness_ui_pending', '')
assert normalized == old, 'Unrelated spool/wire/CRC/ACK/capture change'

unchanged = ['main/offline_io_schedule.h', 'main/offline_replay_prefetch.inc',
    'main/offline_replay_pressure.inc', 'main/offline_replay_read.inc', 'main/sd_audio_catalog.c',
    'main/sd_audio_catalog.h', 'main/sd_audio_session_replay.inc', 'main/ws_transport.c',
    'main/ws_mask.h', 'main/nuwax_lvgl_port.c', 'main/nuwax_ui_v28/face/emotion_animation_player.c']
for name in unchanged:
    assert sha(r / name) == sha(a.baseline / name), name

def function(s, name):
    m = re.search(r'^(?:static )?[\w* ]+\b' + name + r'\([^;{}]*\)\s*\{', s, re.M)
    assert m, name
    at = s.index('{', m.start()) + 1; depth = 1
    while depth:
        depth += (s[at] == '{') - (s[at] == '}'); at += 1
    return s[m.start():at]

schedule = read(r / 'main/replay_ui_schedule.c')
schedule_old = read(a.baseline / 'main/replay_ui_schedule.c')
for name in ['nuwax_replay_schedule_init', 'nuwax_replay_schedule_yield', 'nuwax_replay_schedule_snapshot',
             'replay_idle_hook', 'nuwax_replay_dma_loan_begin', 'nuwax_replay_dma_loan_end',
             'nuwax_replay_dma_loan_bytes', 'nuwax_replay_ui_frame_done', 'nuwax_replay_ui_help_snapshot']:
    assert function(schedule, name) == function(schedule_old, name), name
policy = 'main/nuwax_ui_v28/face/emotion_decode_schedule.h'
strip_comments = lambda s: re.sub(r'/\*.*?\*/', '', s, flags=re.S)
assert strip_comments(read(r / policy).replace('LEAD_US 25000U', 'LEAD_US 20000U')) == strip_comments(read(a.baseline / policy))
defs = lambda path: dict(re.findall(r'^#define (CONFIG_\w+) (.*)$', read(path), re.M))
assert defs(a.elf.parent / 'config/sdkconfig.h') == defs(prior / 'config/sdkconfig.h')
binary = a.elf.with_suffix('.bin').read_bytes()
assert binary[48:80].split(b'\0')[0] == b'7.0.146'
assert binary[176:208].hex() == sha(a.elf)
tool = Path('C:/Espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin')
with a.elf.open('rb') as f:
    elf = ELFFile(f)
    symbols = {s.name: (s['st_value'], s['st_size'], s['st_shndx'])
               for s in elf.get_section_by_name('.symtab').iter_symbols() if s['st_size']}
    state_bytes = sum(symbols[n][1] for n in ['s_help_state', 's_help_generation', 's_help_sequence'])
    assert state_bytes == 12
    assert sum(symbols[n][1] for n in ['s_idle_event_storage', 's_idle_event', 's_idle_wait_state']) == 44
    for name in ['s_help_state', 's_help_generation', 's_help_sequence']:
        assert elf.get_section(symbols[name][2]).name == '.dram0.bss', name

def asm_for(prefix):
    names = [n for n in symbols if n == prefix or n.startswith(prefix + '.') or n.startswith(prefix + '$')]
    assert names, prefix
    return '\n'.join(subprocess.check_output([str(tool / 'xtensa-esp32s3-elf-objdump.exe'),
        '-d', '--disassemble=' + n, str(a.elf)], text=True) for n in names)

asm = asm_for('replay_one_ready')
anchors = sorted(set(re.findall(r'\b(?:b\w+|j)\s[^\n]*?\b([0-9a-f]+)\s<replay_one_ready', asm)))
for address in anchors:
    if re.search(r'^' + address + r':', asm, re.M): continue
    asm += subprocess.check_output([str(tool / 'xtensa-esp32s3-elf-objdump.exe'), '-d',
        '--start-address=0x' + address, '--stop-address=' + hex(int(address, 16) + 48), str(a.elf)], text=True)
for target in ['sd_audio_catalog_bind', 'sd_audio_catalog_ack', 'sd_audio_catalog_note_deleted']:
    assert '<' + target + '>' in asm, target
helper = asm_for('nuwax_replay_ui_handoff')
assert '<xEventGroupWaitBits>' in helper and '<vTaskPrioritySet>' not in helper
result = dict(version='7.0.146', elfSha256=sha(a.elf), hardwareValidated=False,
    unchanged=unchanged, effectiveConfigUnchanged=True, helpStateBytes=state_bytes,
    existingStaticEventBytes=44, originalIdleWaitAndBudgetUnchanged=True,
    decoderOnlyLeadUsChanged=[20000,25000], noTaskPriorityChangesInHelper=True,
    spoolChangesOnlyTestedPostSendSchedulingAndMetrics=True)
(r / 'build-host-checks/replay-shared-wait/linked.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
print('PASS final 146 ELF: tested shared wait; unchanged IDLE/fairness, 12ms lease, priorities, '
      'network, recording, reader, transport, CRC/ACK/catalog deletion; no added task/timer/allocation')
