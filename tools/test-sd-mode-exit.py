"""Run production sleep reconciliation, UI publication and face selection."""
from pathlib import Path
import re
import subprocess

root = Path(__file__).resolve().parents[1]
out = root / 'build-host-checks/sd-mode-exit'
out.mkdir(parents=True, exist_ok=True)
source = (root / 'main/nuwax_ai_partner_main.c').read_text(encoding='utf-8-sig')


def extract(name):
    match = re.search(r'^static [\w *]+\b' + name + r'\([^;{}]*\)\s*\{', source, re.M)
    assert match, name
    pos = source.index('{', match.start()) + 1
    depth = 1
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[match.start():pos]


(out / 'audio_mode_exit_host.inc').write_text('\n'.join(extract(name) for name in [
    'meeting_or_monitoring_mode_blocks_sleep', 'exclusive_audio_controllers_released',
    'service_exclusive_audio_sleep_reconcile']), encoding='utf-8')
(out / 'audio_mode_ui_host.inc').write_text(extract('sync_exclusive_audio_mode_ui_state'), encoding='utf-8')
for name in ['audio_mode_exit_host_check', 'audio_mode_ui_lifecycle_host_check', 'emotion_face_policy_host_check']:
    cmd = ['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I' + str(out), '-I' + str(root / 'main')]
    if name == 'emotion_face_policy_host_check':
        cmd += ['-I' + str(root / 'main/ui_host_stubs'), '-I' + str(root / 'main/nuwax_ui_v28'),
                str(root / 'main/nuwax_ui_v28/nuwax_face.c')]
    else:
        cmd += ['-I' + str(root / 'main/audio_modes/meeting/host_stubs')]
    exe = out / (name + '.exe')
    subprocess.run(cmd + [str(root / 'main' / (name + '.c')), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
