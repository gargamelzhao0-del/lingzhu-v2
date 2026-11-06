"""Execute current dialogue/UI paths with deterministic RTOS/hardware doubles.

No serial port or network is accessed. Logical deadlines are simulated; this
does not constitute physical microphone, RTOS or long-duration validation.
"""
from pathlib import Path
import ctypes
import hashlib
import json
import re
import subprocess
from c_source_extract import function

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'tmp/dialogue-r207'
OUT.mkdir(parents=True, exist_ok=True)
if hasattr(ctypes, 'windll'):
    ctypes.windll.kernel32.SetErrorMode(0x8003)
MAIN = (ROOT / 'main/nuwax_ai_partner_main.c').read_text(encoding='utf-8')
checks = []


def run(name, source, libraries=(), expect_failure=False):
    generated = OUT / (name + '.c')
    generated.write_text(source, encoding='utf-8')
    exe = OUT / (name + '.exe')
    command = ['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
               '-Wno-unused-function', '-Wno-unused-variable',
               '-I' + str(ROOT / 'main'), str(generated)]
    command += [str(ROOT / path) for path in libraries] + ['-o', str(exe)]
    build = subprocess.run(command, text=True, capture_output=True)
    (OUT / (name + '-build.log')).write_text(build.stdout + build.stderr, encoding='utf-8')
    if build.returncode:
        raise RuntimeError(build.stdout + build.stderr)
    result = subprocess.run([str(exe)], text=True, capture_output=True, timeout=20)
    (OUT / (name + '.log')).write_text(result.stdout + result.stderr, encoding='utf-8')
    if expect_failure:
        if not result.returncode:
            raise RuntimeError(f'{name}: regression detector missed the injected defect')
        print('PASS negative control:', name, '(injected defect rejected)')
    else:
        print(result.stdout + result.stderr, end='')
    if result.returncode and not expect_failure:
        raise RuntimeError(f'{name} failed: {result.returncode}')
    checks.append({'check': name, 'expected_failure': expect_failure,
                   'sha256': hashlib.sha256(source.encode()).hexdigest()})


for kind in ['ui', 'entry']:
    template = (ROOT / f'tools/host-stubs/dialogue-r207-{kind}.c.in').read_text(encoding='utf-8')
    sources = json.loads((ROOT / f'tools/host-stubs/dialogue-r207-{kind}-sources.json').read_text())
    for name, path in sources.items():
        body = function((ROOT / path).read_text(encoding='utf-8'), name)
        if name == 'nuwax_state_bridge_update_owned':
            body = body.replace('bool nuwax_state_bridge_update_owned(',
                                'static bool nuwax_state_bridge_update_owned_real(', 1)
            if kind == 'ui':
                body = body.replace('    nuwax_device_state_t s = state_snapshot();',
                    '    if(inject_after_accept){inject_after_accept=false;set_state(NUWAX_APP_STATE_SLEEPING);}\n'
                    '    nuwax_device_state_t s = state_snapshot();', 1)
        template = template.replace('@FUNCTION:' + name + '@', body)
    if kind == 'entry':
        start = MAIN.index('    const bool audio_input_event =', MAIN.index(
            'static void handle_session_event(const nuwax_session_command_t *command,\n'))
        end = MAIN.index('    if (audio_input_event &&\n        nuwax_head_touch_agent_intent_audio_matches', start)
        gate = MAIN[start:end]
        start = MAIN.index('        case NUWAX_SESSION_EVENT_ASR_RESULT:', end)
        end = MAIN.index('        case NUWAX_SESSION_EVENT_TEXT_RESPONSE:', start)
        template = template.replace('@ASR_ENTRY@',
            'static void admitted_asr_event(const nuwax_session_command_t *command){\n' +
            gate + '\nswitch(command->event){\n' + MAIN[start:end] +
            'default:assert(!"unexpected event");}}')
    assert not re.search(r'@[A-Z_]+:', template)
    libraries = ['main/conversation/conversation_state.c',
        'main/conversation/awake_session_state.c', 'main/conversation/prelisten_state.c',
        'main/conversation/listen_resume_state.c', 'main/conversation/sleep_transaction.c',
        'main/nuwax_ui_v28/emotion_display_policy.c']
    run(kind, template, libraries)
    if kind == 'ui':
        needle = 'if (s_lifecycle_publication_seq != 0U &&'
        assert template.count(needle) == 1
        mutant = template.replace(needle, 'if (false && s_lifecycle_publication_seq != 0U &&')
    else:
        needle = '    switch (action->action) {'
        assert template.count(needle) == 1
        mutant = template.replace(needle,
            '    callbacks->clear_auto_followup(callbacks->ctx, "injected_old_clear");\n' + needle)
    run(kind + '-old-bug', mutant, libraries, expect_failure=True)

template = (ROOT / 'tools/host-stubs/dialogue-r207-liveness.c.in').read_text(encoding='utf-8')
for name in ['note_valid_conversation_activity', 'service_normal_conversation_inactivity',
             'awake_idle_sleep_admitted', 'maybe_enter_idle_sleep']:
    template = template.replace('@FUNCTION:' + name + '@', function(MAIN, name))
run('liveness', template, ['main/conversation/awake_session_state.c',
    'main/conversation/prelisten_state.c', 'main/conversation/listen_resume_state.c',
    'main/conversation/turn_guard.c'])

run('deadlines', r'''
#include <assert.h>
#include <stdio.h>
#include "conversation/normal_conversation_policy.h"
#include "conversation/listen_resume_state.h"
#include "conversation/ui_sync_pending.h"
int main(void){
    nuwax_normal_conversation_policy_t p={0};
    nuwax_normal_conversation_begin(&p,1,1000,60000);
    assert(!nuwax_normal_conversation_expired(&p,1,60999));
    assert(nuwax_normal_conversation_expired(&p,1,61000));
    nuwax_normal_conversation_note_activity(&p,2,61000);
    assert(nuwax_normal_conversation_expired(&p,1,61000));
    nuwax_normal_conversation_note_activity(&p,1,61000);
    assert(!nuwax_normal_conversation_expired(&p,1,120999));
    assert(nuwax_normal_conversation_expired(&p,1,121000));
    nuwax_normal_conversation_begin(&p,3,UINT32_MAX-1000,60000);
    assert(!nuwax_normal_conversation_expired(&p,3,58998));
    assert(nuwax_normal_conversation_expired(&p,3,58999));
    uint32_t now=0,attempts=0,delay;
    while((delay=nuwax_normal_conversation_retry_delay(&p,now))!=0){
        assert(delay<=1000);now+=delay;assert(++attempts<12);
        /* Transient task creation without 500 ms stable input is insufficient. */
        for(unsigned i=0;i<24;i++)nuwax_normal_conversation_note_capture_frame(&p);
    }
    assert(now==5000);
    for(unsigned i=0;i<25;i++)nuwax_normal_conversation_note_capture_frame(&p);
    assert(!p.failure_active);
    assert(nuwax_normal_conversation_retry_delay(&p,10000)==100);
    puts("PASS 60s exact inactivity, real activity reset, generation/wrap fencing, bounded resource failures");
    nuwax_listen_resume_state_t r={0};
    nuwax_listen_resume_state_bind_round(&r,1,"audio1");
    assert(nuwax_listen_resume_state_arm_post_end_asr_bounded(&r,2,1000,15000,60000));
    assert(nuwax_listen_resume_state_note_progress(&r,20,"",0));
    assert(!nuwax_listen_resume_state_note_progress(&r,20,"",0));
    assert(!nuwax_listen_resume_state_note_progress(&r,19,"",0));
    assert(nuwax_listen_resume_state_note_progress(&r,0,"hello",1));
    assert(!nuwax_listen_resume_state_note_progress(&r,0,"hello",1));
    assert(nuwax_listen_resume_state_note_progress(&r,0,"hello",2));
    assert(!nuwax_listen_resume_state_note_progress(&r,0,"hello",1));
    for(uint32_t t=10000;t<=60000;t+=10000){
        assert(nuwax_listen_resume_state_note_progress(&r,(int)t,"",0));
        nuwax_listen_resume_state_refresh_post_end_asr(&r,t+15000);
    }
    assert(!nuwax_listen_resume_state_post_end_asr_expired(&r,60999));
    assert(nuwax_listen_resume_state_post_end_asr_expired(&r,61000));
    assert(nuwax_listen_resume_state_arm_post_end_asr_bounded(&r,2,60000,15000,60000));
    assert(nuwax_listen_resume_state_post_end_asr_expired(&r,61000));
    assert(!nuwax_listen_resume_state_arm_post_end_asr_bounded(&r,3,60000,15000,60000));
    nuwax_listen_resume_state_bind_round(&r,1,"audio2");
    assert(!r.post_end_total_bounded && r.progress_frames==0);
    assert(nuwax_listen_resume_state_arm_post_end_asr_bounded(&r,3,UINT32_MAX-1000,15000,60000));
    nuwax_listen_resume_state_refresh_post_end_asr(&r,65000);
    assert(nuwax_listen_resume_state_post_end_asr_expired(&r,58999));
    nuwax_listen_resume_state_disarm_post_end_asr(&r);
    assert(!nuwax_listen_resume_state_post_end_asr_expired(&r,70000));
    nuwax_listen_resume_state_bind_round(&r,2,"wrap-zero");
    assert(nuwax_listen_resume_state_arm_post_end_asr_bounded(&r,4,UINT32_MAX-14999,15000,60000));
    assert(nuwax_listen_resume_state_is_waiting_post_end_asr(&r));
    assert(!nuwax_listen_resume_state_post_end_asr_expired(&r,0));
    assert(nuwax_listen_resume_state_post_end_asr_expired(&r,1));
    puts("PASS post-END 15s quiet/60s total deadline; duplicate, phase, epoch and round fences");
    nuwax_ui_sync_pending_t ui={0};
    nuwax_ui_sync_submit(&ui,NUWAX_APP_STATE_LISTENING,1);uint32_t old=ui.publication_seq;
    nuwax_ui_sync_submit(&ui,NUWAX_APP_STATE_LISTENING,1);
    assert(!nuwax_ui_sync_complete_publication(&ui,NUWAX_APP_STATE_LISTENING,1,old));
    assert(ui.pending);
    assert(nuwax_ui_sync_complete_publication(&ui,NUWAX_APP_STATE_LISTENING,1,ui.publication_seq));
    ui.publication_seq=UINT32_MAX;nuwax_ui_sync_submit(&ui,NUWAX_APP_STATE_SLEEPING,2);
    assert(ui.publication_seq==1);
    puts("PASS same-enum and wrap-safe UI publication completion");
    return 0;
}
''', ['main/conversation/listen_resume_state.c'])

(OUT / 'evidence.json').write_text(json.dumps(
    {'hardware_validated': False, 'checks': checks}, indent=2), encoding='utf-8')
