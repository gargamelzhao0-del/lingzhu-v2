from pathlib import Path
import json
import re
import subprocess

root = next(p for p in Path(__file__).resolve().parents if (p / 'main/conversation/speaker_playback.c').exists())
out = root / 'tmp/review-20260924'
out.mkdir(exist_ok=True)

def run(name, source, extra):
    c = out / (name + '.c')
    exe = out / (name + '.exe')
    c.write_text(source, encoding='utf-8')
    result = subprocess.run(['gcc', '-std=c11', '-O0', '-I', str(root / 'main'),
                            str(c), *extra, '-o', str(exe)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=15)
    assert result.returncode == 0, result.stderr
    print(result.stdout.strip())
    return result.stdout.strip()

cjson = root / 'managed_components/espressif__cjson/cJSON'
json_result = run('nested_json', r'''
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "cJSON.h"
int main(void) {
    char p[2048]="{\"type\":\"pong\",\"extra\":";
    size_t n=strlen(p);
    for(int i=0;i<256;i++)p[n++]='[';
    p[n++]='0';
    for(int i=0;i<256;i++)p[n++]=']';
    p[n++]='}';p[n]=0;
    cJSON *r=cJSON_ParseWithLength(p,n);
    assert(r&&cJSON_IsObject(r));
    printf("nested arrays=256 payload_bytes=%zu parser_accepted=1\n",n);
    cJSON_Delete(r);
}
''', ['-I', str(cjson), '-DCJSON_NESTING_LIMIT=1000', str(cjson/'cJSON.c')])

# Reuse production function extraction and stubs without re-running the
# existing test suite or changing its files.
ns = {'__file__': str(root / 'tools/test-playback-startup.py')}
text = (root / 'tools/test-playback-startup.py').read_text(encoding='utf-8')
exec(text.split("run('production_startup', common + production + tests)")[0], ns)
common = ns['common'].replace('#include "conversation/speaker_jitter_policy.h"', '''
#include "conversation/speaker_jitter_policy.h"
#include "conversation/notification_playback.h"
static nuwax_notification_transaction_t tx;
''').replace('assert(++polls<4000)', 'assert(++polls<20000)')
needle = ' if(mode==9 && age>=6000)s_speaker_ctx.stream.end_accepted=true;'
assert needle in common
common = common.replace(needle, needle + r'''
 if(mode==10){
  if(age%9000==0){
   records++;samples+=320;s_speaker_ctx.stream.pcm_frames++;
   assert(nuwax_notification_refresh_stream_deadline(&tx,1,2,monotonic_ms(),10000));
  }
  assert(!nuwax_notification_deadline_expired(&tx,monotonic_ms()));
 }
''')
setup = ns['tests'].split('int main(void)')[0]
notification_result = run('notification_trickle', common + ns['production'] + setup + r'''
int main(void){
 reset();mode=10;s_speaker_ctx.active_owner=2;
 tx=(nuwax_notification_transaction_t){.active=true,.token=1,.ws_epoch=2,
       .state=NUWAX_NOTIFICATION_STATE_STREAMING,.deadline_ms=monotonic_ms()+10000};
 assert(wait_start());assert(notices==0);
 assert(monotonic_ms()-start_ms==198000);
 printf("notification startup_wait_ms=%u faults=%u transaction_timeout=0\n",monotonic_ms()-start_ms,notices);
}
''', [str(root/'main/conversation/notification_playback.c')])

tool = 'C:/Espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin/xtensa-esp32s3-elf-objdump.exe'
asm = subprocess.check_output([tool, '-d', '--disassemble=parse_value',
                              'C:/tmp/nuwax-playback-startup-v1-20260923/NuwaxAIPartner.elf'], text=True)
frame = int(re.search(r'entry\s+a1,\s*(\d+)', asm)[1])
recursive = len(re.findall(r'call\w*\s+[^\n]*<parse_value>', asm))
assert recursive > 0
(out/'parse_value.asm').write_text(asm, encoding='utf-8')
result = dict(jsonParser=json_result, compiledParseValueFrame=frame,
              compiledRecursiveCallSites=recursive, nestedFramesLowerBound=256*frame,
              wsRxConfiguredStack=16384, notification=notification_result,
              hardwareCrashReproduced=False, productionFilesModified=False)
(out/'report.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
print(json.dumps(result, indent=2))
