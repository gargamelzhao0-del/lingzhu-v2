"""Real adaptation policy and negative tests for short-rebuffer classification."""
from pathlib import Path
import subprocess
import re

root = Path(__file__).resolve().parents[1]
out = root/'build-host-checks/jitter-stability'
policy = (root/'main/conversation/speaker_jitter_policy.h').read_text(encoding='utf-8-sig')
test = (root/'main/speaker_jitter_policy_host_check.c').read_text(encoding='utf-8-sig')
source = (root/'main/conversation/speaker_playback.c').read_text(encoding='utf-8-sig')
assert re.search(r'nuwax_speaker_jitter_adapt_startup_ms\(\s*s_speaker_ctx.jitter_startup_target_ms,\s*s_speaker_ctx.jitter_rebuffer_events > 0U,',source)
assert 'if (midstream) {\n                s_speaker_ctx.jitter_rebuffer_events++;' in source
guard = '''    if (playback_rebuffered) {
        *stable_playbacks = 0U;
        return current_ms;
    }
'''
assert guard in policy
def run(name, content, expected):
    folder = out/name
    (folder/'conversation').mkdir(parents=True,exist_ok=True)
    (folder/'conversation/speaker_jitter_policy.h').write_text(content,encoding='utf-8')
    (folder/'check.c').write_text(test,encoding='utf-8')
    exe = folder/'check.exe'
    subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-I',str(folder),
                    str(folder/'check.c'),'-o',str(exe)],check=True)
    p = subprocess.run([str(exe)],capture_output=True,text=True)
    assert (p.returncode == 0) == expected, (name,p.stderr)
    print('PASS:',name,'passed' if expected else 'defect rejected')
run('production',policy,True)
run('old_short_outage_is_stable',policy.replace(guard,''),False)
run('streak_survives_outage',policy.replace(guard,guard.replace('*stable_playbacks = 0U;', '(void)stable_playbacks;')),False)
run('all_outages_raise_latency',policy.replace('rebuffer_max_wait_ms >= NUWAX_SPEAKER_JITTER_ADAPT_REBUFFER_MIN_MS','rebuffer_max_wait_ms <= UINT32_MAX'),False)
print('PASS: production jitter adaptation; 4800 short-wait/tier combinations; three negative mutants; production wiring')
