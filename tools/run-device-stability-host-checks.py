"""Run current production lifecycle functions with deterministic hardware doubles.

Does not access a device/cloud or claim physical timing, SD or RTOS validation.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re
import subprocess

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / 'tmp/device-stability-host-checks'
OUT.mkdir(parents=True, exist_ok=True)
SOURCE = (ROOT / 'main/nuwax_ai_partner_main.c').read_text(encoding='utf-8-sig')


def extract(name, source=SOURCE):
    match = re.search(r'^(?:static\s+)?\w+[\s*]+\b' + re.escape(name) + r'\([^;{}]*\)\s*\{', source, re.M)
    if not match:
        raise ValueError(name)
    pos = source.index('{', match.start()) + 1
    depth = 1
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[match.start():pos]


groups = {
    'wake': ['clear_conversation_visual_round','clear_active_turn_guard','begin_active_turn_guard',
        'begin_awake_session','begin_conversation_visual_round','start_prelisten_window',
        'return_to_listening_after_cancel','recover_normal_conversation_after_playback_abort',
        'awake_idle_sleep_admitted','maybe_enter_idle_sleep','ensure_sleep_wake_word_detector',
        'check_conversation_turn_timeout','send_wake_word_greeting','wait_for_mic_start_action',
        'pause_awake_waiting','clear_ws_reconnect_audio_recovery','preserve_awake_session_for_ws_reconnect',
        'arm_ws_reconnect_audio_recovery','recover_awake_conversation_after_ws_reconnect'],
    'offline': ['reset_offline_capture_retry_state','offline_mode_identity','service_offline_duration_limit',
        'service_offline_capture_retry_disconnected','sync_exclusive_audio_mode_ui_state','cancel_offline_mode_lease'],
    'capture': ['recording_space_blocked','healthy_capture_defers_media_probe','refresh_capture_capacity_locked','offline_capture_cancelled','nuwax_offline_audio_request_stop',
        'nuwax_offline_audio_start_capture_continuation'],
}
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--groups', nargs='+', choices=tuple(groups), default=list(groups))
selected_groups = parser.parse_args().groups
evidence = {'hardware_validated': False, 'groups': {}, 'source_sha256': hashlib.sha256(
    (ROOT / 'main/nuwax_ai_partner_main.c').read_bytes()).hexdigest()}
for group, names in groups.items():
    if group not in selected_groups:
        continue
    group_source = SOURCE if group != 'capture' else (ROOT/'main/offline_audio_spool.c').read_text(encoding='utf-8-sig')
    parts = {name: extract(name,group_source) for name in names}
    support = (ROOT / f'tools/host-stubs/device-stability-{group}-support.h').read_text(encoding='utf-8')
    tests = (ROOT / f'tools/host-stubs/device-stability-{group}-tests.inc').read_text(encoding='utf-8')
    if group == 'offline':
        support = '\n'.join(re.findall(r'^#define NUWAX_OFFLINE_CAPTURE_RETRY_(?:INITIAL|MAX)_MS .*', SOURCE, re.M)) + '\n' + support
        support += '\ntypedef int nuwax_offline_audio_mode_t;\n#define s_offline_mode_resume_pending lease\n'
        support = support.replace('static bool lease, capture, expired, meeting_resume,monitoring_resume;',
            'static atomic_bool lease; static bool capture, expired, meeting_resume,monitoring_resume;')
    if group == 'capture':
        config = re.search(r'typedef struct \{\s*nuwax_offline_audio_mode_t mode;\s*uint64_t limit_used_us;.*?\} offline_capture_config_t;',group_source,re.S)
        assert config
        support += config.group(0) + '\n'
    generated = OUT / f'{group}.c'
    generated.write_text(support + '\n\n'.join(parts.values()) + tests, encoding='utf-8')
    executable = OUT / f'{group}.exe'
    command = ['gcc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-parameter',
        '-Wno-unused-variable','-Wno-unused-function','-I',str(ROOT/'main'),str(generated)]
    if group == 'wake':
        command += [str(ROOT/'main/conversation'/f) for f in ['awake_session_state.c','turn_guard.c',
            'playback_terminal_guard.c','prelisten_state.c','listen_resume_state.c']]
    command += ['-o',str(executable)]
    build = subprocess.run(command,capture_output=True,text=True)
    (OUT/f'{group}-build.log').write_text(build.stdout+build.stderr,encoding='utf-8')
    if build.returncode:
        print(build.stdout+build.stderr)
        raise SystemExit(build.returncode)
    run = subprocess.run([str(executable)],capture_output=True,text=True)
    (OUT/f'{group}.log').write_text(run.stdout+run.stderr,encoding='utf-8')
    print(run.stdout+run.stderr,end='')
    if run.returncode:
        raise SystemExit(run.returncode)
    evidence['groups'][group] = {name:hashlib.sha256(code.encode()).hexdigest() for name,code in parts.items()}
(OUT/'evidence.json').write_text(json.dumps(evidence,indent=2),encoding='utf-8')
