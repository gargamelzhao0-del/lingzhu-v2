"""Run production mirror block with unavailable media and negative mutations."""
from pathlib import Path
import re
import subprocess
import tempfile

root=Path(__file__).resolve().parents[1]
s=(root/'main/offline_audio_spool.c').read_text(encoding='utf-8-sig')
a=s.index('/* The real-time producer retains PCM')
b=s.index('static TickType_t recovery_remaining_ticks(',a)
defines='\n'.join(re.findall(r'^#define (?:OFFLINE_MIRROR_ATTACH_RETRY_MS|MIRROR_STAGING_FRAMES|MIRROR_CLOSE_TIMEOUT_MS) .*$',s,re.M))
space=s[s.index('static bool recording_space_blocked(void)'):s.index('static bool enabled(void);')]
block=defines+'\n'+space+'\n'+s[a:b]
variants=[('production',block,True),
 ('allocate_without_media',block.replace('!atomic_load_explicit(&s_media_ready, memory_order_acquire) ||\n            recording_space_blocked()','false'),False),
 ('false_complete_after_insert',block.replace('m->rejected_frames == 0U && !m->media_deferred','m->rejected_frames == 0U'),False),
 ('forget_skipped_frames',block.replace('++m->rejected_frames;','(void)m;'),False),
 ('discard_existing_tail',block.replace('    m->complete = m->complete && complete;',
  '    if (!atomic_load(&s_media_ready)) { mirror_finish(m); return ESP_OK; }\n    m->complete = m->complete && complete;'),False)]
with tempfile.TemporaryDirectory(prefix='sd-media-admission-') as d:
 p=Path(d);exe=p/'check.exe'
 for name,code,expected in variants:
  (p/'offline_mirror_host.inc').write_text(code,encoding='utf-8')
  subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function',
   '-I'+str(root/'main/audio_modes/meeting/host_stubs'),'-I'+str(root/'main'),'-I'+str(p),
   str(root/'main/offline_audio_mirror_host_check.c'),'-o',str(exe)],check=True)
  result=subprocess.run([str(exe)],capture_output=True,text=True)
  assert (result.returncode==0)==expected, name+result.stdout+result.stderr
  print('PASS: '+name+(' unavailable-media admission, insertion, existing tail, delayed attach and backpressure' if expected else ' mutant rejected'))
