"""Replay the observed RX collision using production barrier, guard and cleanup branch."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
main=(root/'main/nuwax_ai_partner_main.c').read_text(encoding='utf-8-sig')
start=main.index('            if (command->rx_playback_rejected) {')
end=main.index('            s_natural_playback_touch_protection_deadline_ms',start)
branch=main[start:end]
rx=main[main.index('normal playback BEGIN overlaps an unresolved audio stream'):main.index('    if (queue_p3_softref_ab_fast',main.index('normal playback BEGIN overlaps an unresolved audio stream'))]
assert 'command.rx_playback_rejected = true;' in rx and 'return;' not in rx
assert 'rx_playback_rejected' not in (root/'main/session_protocol.c').read_text(encoding='utf-8-sig')
common=r"""
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "conversation/turn_guard.h"
#include "conversation/notification_rx_barrier.h"
#define ESP_LOGW(...) ((void)0)
#define NUWAX_APP_STATE_IDLE 0
static nuwax_turn_guard_t s_turn_guard;
static unsigned s_awake_session=1,cleared,resumed,pinned;
static unsigned active_ws_epoch_snapshot(void){return 2;}
static unsigned nuwax_awake_session_state_generation(const unsigned *s){return *s;}
static void clear_last_listen_resume(void){}
static void wake_greeting_metrics_cancel(const char *s){(void)s;}
static void clear_active_turn_guard(const char *s){(void)s;cleared++;pinned=0;nuwax_turn_guard_clear(&s_turn_guard);}
static bool schedule_auto_followup_after_playback(void){resumed++;return true;}
static void set_state(int s){(void)s;assert(0);}
typedef struct {bool rx_playback_rejected;char request_id[48],turn_id[48],playback_id[48];} Command;
static void handle(const Command *command,unsigned event_ws_epoch){switch(0){case 0:
"""
tests=r"""
}}
static void begin(void){
 nuwax_turn_guard_init(&s_turn_guard);
 nuwax_turn_guard_begin(&s_turn_guard,1,"shake_4",NULL,475025,30000);
 cleared=resumed=0;pinned=1;
}
int main(void){
 nuwax_notification_rx_barrier_t b={0};
 assert(nuwax_notification_rx_barrier_arm_normal(&b,2,475015,5000,"old","old_pb"));
 assert(nuwax_notification_rx_barrier_reject_normal(&b,2,475025,5000,"old","old_pb"));
 assert(!nuwax_notification_rx_barrier_arm_normal(&b,2,475085,5000,"new","new_pb"));
 Command c={true,"shake_4","new","new_pb"};
 begin();handle(&c,2);assert(cleared==1&&resumed==1&&!pinned&&!s_turn_guard.active);
 assert(b.state==NUWAX_NOTIFICATION_RX_REJECTED); /* cleanup NEVER unmutes ambiguous audio */
 handle(&c,2);assert(cleared==1&&resumed==1); /* duplicate terminal */
 begin();handle(&c,1);assert(!cleared&&pinned); /* old connection */
 begin();s_awake_session=2;handle(&c,2);assert(!cleared&&pinned);s_awake_session=1;
 begin();strcpy(c.request_id,"shake_3");handle(&c,2);assert(!cleared&&pinned);strcpy(c.request_id,"shake_4");
 begin();s_turn_guard.phase=NUWAX_TURN_PHASE_PLAYING;handle(&c,2);assert(!cleared&&pinned);
 begin();s_turn_guard.speaker_generation=9;handle(&c,2);assert(!cleared&&pinned);
 begin();c.request_id[0]=0;handle(&c,2);assert(!cleared&&pinned);strcpy(c.request_id,"shake_4");
 begin();c.rx_playback_rejected=false;handle(&c,2);assert(!cleared&&pinned);c.rx_playback_rejected=true;
 begin();strcpy(s_turn_guard.turn_id,"different");handle(&c,2);assert(!cleared&&pinned);
 begin();strcpy(s_turn_guard.playback_id,"different");handle(&c,2);assert(!cleared&&pinned);
 begin();s_turn_guard.phase=NUWAX_TURN_PHASE_WAIT_TTS;handle(&c,2);assert(cleared==1);
 begin();s_turn_guard.phase=NUWAX_TURN_PHASE_WAIT_TTS_RETRY;handle(&c,2);assert(cleared==1);
 begin();strcpy(s_turn_guard.turn_id,"new");c.request_id[0]=0;handle(&c,2);assert(cleared==1);
 /* A real notification barrier must stay intact as well. */
 nuwax_notification_rx_barrier_reset(&b);
 assert(nuwax_notification_rx_barrier_arm(&b,2,0,5000,"notify","notify_pb"));
 assert(!nuwax_notification_rx_barrier_arm_normal(&b,2,1,5000,"new","new_pb"));
 assert(b.owner==NUWAX_NOTIFICATION_RX_OWNER_NOTIFICATION);
 puts("PASS: observed collision closes only matching unstarted owner; stale epochs/IDs, duplicate, normal and playing guards; quarantine retained");
}
"""
code=common+branch+tests
policy=(root/'main/conversation/turn_guard.c').read_text(encoding='utf-8-sig')
with tempfile.TemporaryDirectory(prefix='rejected-playback-') as directory:
 path=Path(directory)/'test.c';guard=Path(directory)/'guard.c';exe=path.with_suffix('.exe')
 variants=[('production',code,policy),
 ('lost_rejection_cleanup',code.replace('if (command->rx_playback_rejected)', 'if (false)'),policy),
 ('cross_epoch_cleanup',code.replace('event_ws_epoch == active_ws_epoch_snapshot() &&','true &&'),policy),
 ('cancel_playing',code,policy.replace('guard->phase != NUWAX_TURN_PHASE_WAIT_TEXT &&','guard->phase != NUWAX_TURN_PHASE_PLAYING && guard->phase != NUWAX_TURN_PHASE_WAIT_TEXT &&')),
 ('cancel_live_speaker',code,policy.replace('guard->speaker_generation != 0U ||','false ||'))]
 for name,body,guard_source in variants:
  path.write_text(body);guard.write_text(guard_source)
  subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Wno-unused-parameter','-Wno-unused-function','-I'+str(root/'main'),'-I'+str(root/'main/conversation'),str(path),str(guard),str(root/'main/conversation/notification_rx_barrier.c'),'-o',str(exe)],check=True)
  result=subprocess.run([str(exe)],capture_output=True,text=True)
  assert (result.returncode==0)==(name=='production'),name+result.stderr
  print(result.stdout.strip() if name=='production' else 'PASS: rejected mutant '+name)
