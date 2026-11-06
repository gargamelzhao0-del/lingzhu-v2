"""Run real jitter wait and notification transaction through slow-start abort."""
from pathlib import Path
import re
import subprocess

root=Path(__file__).resolve().parents[1]
ns={'__file__':str(root/'tools/test-playback-startup.py')}
s=(root/'tools/test-playback-startup.py').read_text(encoding='utf-8-sig')
exec(s.split("run('production_startup', common + production + tests)")[0],ns)
common=ns['common'].replace('#include "conversation/speaker_jitter_policy.h"','''
#include "conversation/speaker_jitter_policy.h"
#include "conversation/notification_playback.h"
static nuwax_notification_transaction_t tx;
''').replace('assert(++polls<4000)','assert(++polls<20000)')
old=' if(mode==10 && age%9000==0){records++;samples+=320;s_speaker_ctx.stream.pcm_frames++;}'
assert old in common
common=common.replace(old,old+'''
 if(mode==10){
  if(age%9000==0)assert(nuwax_notification_refresh_stream_deadline(&tx,1,2,monotonic_ms(),10000));
  assert(!nuwax_notification_deadline_expired(&tx,monotonic_ms()));
 }
''')
setup=ns['tests'].split('int main(void)')[0]
tests=r'''
int main(void){
 for(unsigned order=0;order<2;order++){
  reset();mode=10;s_speaker_ctx.active_owner=2;
  tx=(nuwax_notification_transaction_t){.active=true,.token=1,.ws_epoch=2,.speaker_generation=9,
      .state=NUWAX_NOTIFICATION_STATE_STREAMING,.deadline_ms=monotonic_ms()+10000,
      .capture_released=true,.wake_detector_released=true,.origin_sleeping=order!=0};
  assert(!wait_start()&&notices==1&&monotonic_ms()-start_ms==10000);
  /* main fault notification and physical-stop callback may arrive in either order */
  if(order==0){assert(speaker_process_pending_protocol_fault());
    assert(nuwax_notification_mark_physical_done(&tx,1,2,9));}
  assert(nuwax_notification_request_cancel(&tx,1,2,monotonic_ms(),3000));
  assert(nuwax_notification_blocks_conversation(&tx));
  assert(!nuwax_notification_complete(&tx,1));
  if(order==1){
    assert(!nuwax_notification_cancel_ready(&tx));
    assert(!nuwax_notification_mark_physical_done(&tx,1,3,9));
    assert(!nuwax_notification_mark_physical_done(&tx,1,2,10));
    assert(speaker_process_pending_protocol_fault());
    assert(nuwax_notification_mark_physical_done(&tx,1,2,9));
  }
  assert(nuwax_notification_cancel_ready(&tx));
  assert(!nuwax_notification_refresh_stream_deadline(&tx,1,2,monotonic_ms(),10000));
  assert(nuwax_notification_mark_cancel_notified(&tx,1,2));
  assert(nuwax_notification_complete(&tx,1));
  assert(!nuwax_notification_blocks_conversation(&tx)&&tx.origin_sleeping==(order!=0));
 }
 puts("PASS: notification slow-start abort=10s; fault/physical-stop ordering; no premature listen; stale epoch/generation rejected; awake/sleep origin preserved");
}
'''
out=root/'build-host-checks/notification-startup';out.mkdir(parents=True,exist_ok=True)
def run(name,production,negative=False):
    c=out/(name+'.c');exe=out/(name+'.exe');c.write_text(common+production+setup+tests,encoding='utf-8')
    p=subprocess.run(['gcc','-std=c11','-O0','-I',str(root/'main'),str(c),
                      str(root/'main/conversation/notification_playback.c'),'-o',str(exe)],capture_output=True,text=True)
    assert p.returncode==0,p.stderr
    p=subprocess.run([str(exe)],capture_output=True,text=True,timeout=15)
    assert (p.returncode!=0)==negative,(name,p.stderr)
    print('PASS: original unbounded notification behavior rejected' if negative else p.stdout.strip())
run('notification_startup',ns['production'])
run('notification_startup_old',ns['production'].replace('notification_startup_expired = generation_current &&',
                                                      'notification_startup_expired = false && generation_current &&'),True)
main=(root/'main/nuwax_ai_partner_main.c').read_text(encoding='utf-8-sig')
branch=main.split('case NUWAX_SPEAKER_OWNER_EVENT_PLAYBACK_FAULT:',1)[1].split('const bool fault_terminal_matches',1)[0]
assert re.search(r's_notification_transaction.active.*?ws_epoch.*?speaker_generation.*?cancel_active_notification\(\s*ws, "notification playback fault"\);\s*break;',branch,re.S)
print('PASS: main routes matching notification fault to notification cancellation before ordinary turn recovery')
