"""Verify production route selection independently from storage readiness."""
from pathlib import Path
import hashlib, json, re, subprocess
root=Path(__file__).resolve().parents[1]
out=root/'build-host-checks/sd-route';out.mkdir(parents=True,exist_ok=True)
def extract(src,name):
    m=re.search(r'^(?:static )?[\w* ]+\b'+name+r'\([^;{}]*\)\s*\{',src,re.M);assert m,name
    at=src.index('{',m.start())+1;depth=1
    while depth:
        depth+=(src[at]=='{')-(src[at]=='}');at+=1
    return src[m.start():at]
files=['main/sd_audio_session_storage.inc','main/board/sd_card_driver.c']
src=[(root/p).read_text() for p in files]
code=r'''
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <assert.h>
static atomic_bool s_media_detected,s_media_ready,s_space_blocked,s_shutdown_requested;
static struct {atomic_bool media_recovery_requested;} s;
static bool initialized;
static bool runtime_initialized(void){return initialized;}
'''
code+=extract(src[1],'nuwax_sd_card_media_detected')
for name in ['nuwax_sd_session_available','nuwax_sd_session_prefers_local']:code+=extract(src[0],name)
code+=r'''
int main(void){
 assert(!nuwax_sd_session_prefers_local());
 initialized=true;assert(!nuwax_sd_session_available()&&!nuwax_sd_session_prefers_local());
 atomic_store(&s_media_detected,true);atomic_store(&s_media_ready,true);
 assert(nuwax_sd_session_available()&&nuwax_sd_session_prefers_local());
 atomic_store(&s.media_recovery_requested,true);atomic_store(&s_media_ready,false);
 assert(!nuwax_sd_session_available()&&nuwax_sd_session_prefers_local());
 atomic_store(&s.media_recovery_requested,false);atomic_store(&s_media_ready,true);
 atomic_store(&s_space_blocked,true);
 /* Capacity is checked by mode admission: a full monitor card must remain
  * available long enough for its retention owner to reclaim eligible data. */
 assert(nuwax_sd_session_available()&&nuwax_sd_session_prefers_local());
 atomic_store(&s_space_blocked,false);atomic_store(&s_shutdown_requested,true);
 assert(!nuwax_sd_session_available()&&nuwax_sd_session_prefers_local());
 atomic_store(&s_shutdown_requested,false);atomic_store(&s_media_ready,false);
 atomic_store(&s_media_detected,false);assert(!nuwax_sd_session_prefers_local());
 puts("PASS production local-route predicate: known card stays local through probe/recovery/full/shutdown; readiness still gates capture; absent media uses existing route");
}
'''
(out/'route.c').write_text(code,encoding='utf-8')
subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror',str(out/'route.c'),'-o',str(out/'route.exe')],check=True)
subprocess.run([str(out/'route.exe')],check=True)
(out/'report.json').write_text(json.dumps({'hardwareValidated':False,'sourceHashes':{p:hashlib.sha256((root/p).read_bytes()).hexdigest() for p in files}},indent=2),encoding='utf-8')
