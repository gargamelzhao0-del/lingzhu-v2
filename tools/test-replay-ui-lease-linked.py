"""Verify 145's linked callers, tested source and unchanged data/lease bounds."""
from pathlib import Path
import argparse, hashlib, json, re, subprocess
from elftools.elf.elffile import ELFFile
p = argparse.ArgumentParser()
p.add_argument('--elf', type=Path, required=True)
p.add_argument('--baseline', type=Path, required=True)
a = p.parse_args()
r = Path(__file__).resolve().parents[1]
prior = r / 'artifacts/firmware/7.0.144-sd-gateway-test-20261016'
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
read = lambda p: p.read_text(encoding='utf-8-sig')
old = read(a.baseline / 'main/offline_audio_spool.c')
new = read(r / 'main/offline_audio_spool.c')
def block(s, marker):
    start = s.index(marker, s.index('static esp_err_t replay_send_windowed('))
    return s[start:s.index('\n            }\n            if (err != ESP_OK)', start)]
before = block(old, '/* Fairness first:')
after = block(new, '/* Serve an existing grant')
report = json.loads(read(r / 'build-host-checks/replay-ui-lease/report.json'))
assert report['sourceSha256'] == sha(r / 'main/offline_audio_spool.c')
assert report['oldBlockSha256'] == hashlib.sha256(before.encode()).hexdigest()
assert report['newBlockSha256'] == hashlib.sha256(after.encode()).hexdigest()
for name, expected in report['sourceHashes'].items():
    assert sha(r / name) == expected, name
normalized = new.replace(after, before).replace('SD scheduler revision=145', 'SD scheduler revision=144')
normalized = normalized.replace('cooperate_order=pending_ui_then_fairness', 'cooperate_order=fairness_then_ui')
normalized = normalized.replace('    uint32_t fairness_ui_ready = 0U, fairness_ui_expired = 0U, fairness_ui_cancelled = 0U;\n', '')
normalized = normalized.replace(' ui_ready=%lu ui_expired=%lu ui_cancelled=%lu', '')
normalized = normalized.replace(',\n             (unsigned long)fairness_ui_ready, (unsigned long)fairness_ui_expired,\n             (unsigned long)fairness_ui_cancelled);', ');')
assert normalized == old, 'Unrelated spool/wire/CRC/ACK/capture change'

owner_name = 'main/nuwax_ui_v28/face/emotion_animation_player.c'
owner = read(r / owner_name)
owner = owner.replace('decode_help_finish(int64_t now_us, emotion_decode_help_action_t action)', 'decode_help_finish(int64_t now_us)')
owner = owner.replace('''nuwax_replay_ui_set_help_state(action == EMOTION_DECODE_HELP_READY
        ? NUWAX_REPLAY_UI_HELP_READY : action == EMOTION_DECODE_HELP_EXPIRED
        ? NUWAX_REPLAY_UI_HELP_EXPIRED : NUWAX_REPLAY_UI_HELP_CANCELLED);''', 'nuwax_replay_ui_set_help(false);')
owner = owner.replace('decode_help_finish(esp_timer_get_time(), EMOTION_DECODE_HELP_CANCEL)', 'decode_help_finish(esp_timer_get_time())')
owner = owner.replace('nuwax_replay_ui_set_help_state(NUWAX_REPLAY_UI_HELP_CANCELLED)', 'nuwax_replay_ui_set_help(false)')
owner = owner.replace('nuwax_replay_ui_set_help_state(NUWAX_REPLAY_UI_HELP_REQUESTED)', 'nuwax_replay_ui_set_help(true)')
owner = owner.replace('decode_help_finish(now_us, action)', 'decode_help_finish(now_us)')
assert owner == read(a.baseline / owner_name), 'Decoder changed beyond publishing grant outcomes'

unchanged = ['main/offline_io_schedule.h', 'main/offline_replay_prefetch.inc',
    'main/offline_replay_pressure.inc', 'main/offline_replay_read.inc', 'main/sd_audio_catalog.c',
    'main/sd_audio_catalog.h', 'main/sd_audio_session_replay.inc', 'main/ws_transport.c',
    'main/ws_mask.h', 'main/nuwax_lvgl_port.c', 'main/nuwax_ui_v28/face/emotion_decode_schedule.h']
for name in unchanged:
    assert sha(r / name) == sha(a.baseline / name), name
# Fairness and DMA observation functions remain byte-for-byte unchanged.
def function(s, name):
    m = re.search(r'^(?:static )?[\w* ]+\b' + name + r'\([^;{}]*\)\s*\{', s, re.M)
    assert m, name
    at = s.index('{', m.start()) + 1
    depth = 1
    while depth:
        depth += (s[at] == '{') - (s[at] == '}')
        at += 1
    return s[m.start():at]
schedule = read(r / 'main/replay_ui_schedule.c')
schedule_old = read(a.baseline / 'main/replay_ui_schedule.c')
for name in ['nuwax_replay_schedule_init', 'nuwax_replay_schedule_yield', 'nuwax_replay_schedule_snapshot',
             'replay_idle_hook', 'nuwax_replay_dma_loan_begin', 'nuwax_replay_dma_loan_end',
             'nuwax_replay_dma_loan_bytes', 'nuwax_replay_ui_frame_done']:
    assert function(schedule, name) == function(schedule_old, name), name
defs = lambda path: dict(re.findall(r'^#define (CONFIG_\w+) (.*)$', read(path), re.M))
assert defs(a.elf.parent / 'config/sdkconfig.h') == defs(prior / 'config/sdkconfig.h')
binary = a.elf.with_suffix('.bin').read_bytes()
assert binary[48:80].split(b'\0')[0] == b'7.0.145'
assert binary[176:208].hex() == sha(a.elf)
for marker in [b'catalog_reused=%u', b'confirm_ms=%lu persist_ms=%lu',
               b'cooperate_order=pending_ui_then_fairness', b'ui_expired=%lu ui_cancelled=%lu']:
    assert marker in binary, marker
tool = Path('C:/Espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin')
with a.elf.open('rb') as f:
    elf = ELFFile(f)
    symbols = {s.name: (s['st_value'], s['st_size'], s['st_shndx'])
               for s in elf.get_section_by_name('.symtab').iter_symbols() if s['st_size']}
    state_bytes = sum(symbols[n][1] for n in ['s_help_state', 's_help_generation', 's_help_sequence'])
    assert state_bytes == 12
    assert 'nuwax_replay_ui_set_help' not in symbols
    assert sum(symbols[n][1] for n in ['s_idle_event_storage', 's_idle_event', 's_idle_wait_state']) == 44
    for name in ['s_help_state', 's_help_generation', 's_help_sequence']:
        assert elf.get_section(symbols[name][2]).name == '.dram0.bss', name
def asm_for(prefix):
    names = [n for n in symbols if n == prefix or n.startswith(prefix + '.') or n.startswith(prefix + '$')]
    assert names, prefix
    return '\n'.join(subprocess.check_output([str(tool / 'xtensa-esp32s3-elf-objdump.exe'),
        '-d', '--disassemble=' + n, str(a.elf)], text=True) for n in names)
asm = asm_for('replay_one_ready')
# Xtensa linear disassembly can consume padding with the next instruction and
# miss a reachable block's first byte. Re-anchor only at observed branch targets
# inside this function; this verifies actual calls, not source-text proximity.
anchors = sorted(set(re.findall(r'\b(?:b\w+|j)\s[^\n]*?\b([0-9a-f]+)\s<replay_one_ready', asm)))
for address in anchors:
    if re.search(r'^' + address + r':', asm, re.M):
        continue
    asm += subprocess.check_output([str(tool / 'xtensa-esp32s3-elf-objdump.exe'), '-d',
        '--start-address=0x' + address, '--stop-address=' + hex(int(address, 16) + 48), str(a.elf)], text=True)
for target in ['sd_audio_catalog_bind', 'sd_audio_catalog_ack', 'sd_audio_catalog_note_deleted']:
    assert '<' + target + '>' in asm, target
owner_asm = asm_for('decode_help_finish') + asm_for('nuwax_emotion_animation_player_service_decode')
assert '<nuwax_replay_ui_set_help_state>' in owner_asm
result = dict(version='7.0.145', elfSha256=sha(a.elf), hardwareValidated=False,
    unchanged=unchanged, effectiveConfigUnchanged=True, helpStateBytes=state_bytes,
    existingStaticEventBytes=44, decoderChangesOnlyPublishOutcomes=True,
    spoolChangesOnlyTestedPostSendSchedulingAndMetrics=True)
(r / 'build-host-checks/replay-ui-lease/linked.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
print('PASS final 145 ELF: tested grant order and outcome publication; 12ms lease, priorities, '
      'fairness/IDLE guards, network, recording, CRC/ACK/catalog deletion unchanged')
