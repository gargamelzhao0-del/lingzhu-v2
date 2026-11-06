"""Real wake policy, bounded evidence timeline and regression-killing mutants."""
from pathlib import Path
import subprocess
import re

root = Path(__file__).resolve().parents[1]
out = root / 'build-host-checks/wake-evidence'
out.mkdir(parents=True, exist_ok=True)
policy = (root/'main/conversation/wake_word_detection_policy.h').read_text(encoding='utf-8-sig')
test = (root/'main/wake_word_detection_policy_host_check.c').read_text(encoding='utf-8-sig')
source = (root/'main/wake_word_detector.c').read_text(encoding='utf-8-sig')

def block_after(text, marker):
    start = text.index('{', text.index(marker))
    depth = 1
    end = start + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

gap = block_after(source, 'if (capture_gap_since_feed)')
timeout = block_after(source, 'else if (mn_state == ESP_MN_STATE_TIMEOUT)')
candidate = block_after(source, 'if (mn_state == ESP_MN_STATE_DETECTED)')
assert 'nuwax_wake_evidence_reset(&wake_evidence)' in gap
assert 'nuwax_wake_evidence_reset' not in timeout
assert 'nuwax_wake_evidence_reset(&wake_evidence)' in candidate
assert source.index('nuwax_wake_evidence_observe(&wake_evidence') < source.index('esp_mn_state_t mn_state =')
assert re.search(r'action_eligible = nuwax_wake_detection_accept\(\s*probability_q1000,\s*noise_floor_avg_abs,\s*evidence.signal_max,\s*evidence.voiced_ms,\s*evidence.clipped_samples,\s*evidence.total_samples\)', candidate)
assert 'nuwax_wake_evidence_t wake_evidence = {0}' in source

def run(name, content, expected):
    folder = out/name
    (folder/'conversation').mkdir(parents=True, exist_ok=True)
    (folder/'conversation/wake_word_detection_policy.h').write_text(content, encoding='utf-8')
    (folder/'check.c').write_text(test, encoding='utf-8')
    exe = folder/'check.exe'
    subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror', '-I',str(folder),
                    '-I',str(root/'main'),str(folder/'check.c'),'-o',str(exe)], check=True)
    result = subprocess.run([str(exe)], capture_output=True, text=True)
    assert (result.returncode == 0) == expected, (name, result.stderr)
    print('PASS:', name, 'accepted' if expected else 'old defect rejected')

run('production', policy, True)
run('old_score_floor', policy.replace('WEAK_THRESHOLD_Q1000 90U',
                                    'WEAK_THRESHOLD_Q1000 100U'), False)
run('too_low_score_floor', policy.replace('WEAK_THRESHOLD_Q1000 90U',
                                        'WEAK_THRESHOLD_Q1000 80U'), False)
needle = '    /* A model score is not independent acoustic evidence.'
assert needle in policy
run('unconditional_strong', policy.replace(needle,
    '    if (probability_q1000 >= 100U) return true;\n'+needle), False)
run('stale_voice', policy.replace('youngest_voice > NUWAX_WAKE_VOICE_TAIL_HOLD_MS',
                                 'youngest_voice == UINT32_MAX'), False)
run('silence_counts_as_voice', policy.replace('if (voice_like) {',
                                            'if (voice_like || samples) {'), False)
run('gap_keeps_evidence', policy.replace('e->valid = 0U;', '(void)e;'), False)
print('PASS: real production wake policy, rollover/expiry/gap/score/clipping boundaries; six negative mutants; live wiring contracts')
